// Hooks_KeyValues.cpp — KeyValues tree manipulation.
// * ReadAsBinary  — offline license injection (offline only) + parse context reset
// * FindOrCreateKey — cloud state scrubbers + active game name/icon spoofer (480)

#include "Hooks_KeyValues.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Hooks_Misc.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include <windows.h>

#include <string_view>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace {

    RESOLVE_FUNC(FindOrCreateKey, KeyValues*, KeyValues* parent, const char* name, bool create, KeyValues** out);

    KeyValues* KV_FindKey(KeyValues* parent, const char* name) {
        return oFindOrCreateKey ? oFindOrCreateKey(parent, name, false, nullptr) : nullptr;
    }

    // ── KeyValuesSystem — symbol ↔ string (from vstdlib_s64.dll) ──────────
    IKeyValuesSystem* GetKeyValuesSystem() {
        static IKeyValuesSystem* sys = []() -> IKeyValuesSystem* {
            auto vstdlib = OSTPlatform::DynamicLibrary::GetLoaded("vstdlib_s64.dll");
            if (!vstdlib) return nullptr;
            auto pfn = reinterpret_cast<KeyValuesSystemSteam_t>(
                OSTPlatform::DynamicLibrary::GetSymbol(vstdlib, "KeyValuesSystemSteam"));
            return pfn ? pfn() : nullptr;
        }();
        return sys;
    }

    const char* GetKeyName(int symbol) {
        auto* sys = GetKeyValuesSystem();
        if (!sys) return nullptr;
        auto name = sys->GetStringForSymbol(symbol);
        LOG_KEYVALUE_TRACE("GetKeyName: symbol={} -> name={}", symbol, name ? name : "(null)");
        return name ? name : nullptr;
    }

    constexpr ptrdiff_t KV_STRING_OFFSET = 0x10;

    const char* KV_GetString(KeyValues* kv) {
        if (!kv) return nullptr;
        const char* val = *reinterpret_cast<const char**>(
            reinterpret_cast<uint8_t*>(kv) + KV_STRING_OFFSET);
        if (!val || reinterpret_cast<uintptr_t>(val) < 0x10000) return nullptr;
        return val;
    }

    void KV_SetString(KeyValues* kv, const char* value) {
        if (!kv || !value) return;
        *reinterpret_cast<const char**>(
            reinterpret_cast<uint8_t*>(kv) + KV_STRING_OFFSET) = value;
    }

    // g_ParsedAppId tracks the most recently parsed numeric key so cloud scrubbers
    // know which AppID's subtree is currently being walked.  Reset at the start of
    // every ReadAsBinary call so IDs from one VDF file don't contaminate the next.
    static AppId_t g_ParsedAppId = 0;

    // ── Hard Offline Injector ──────────────────────────────────────────────
    HOOK_FUNC(ReadAsBinary, bool, KeyValues* root, void* buf, int depth,
              bool textMode, void* symTable) {
        bool ok = oReadAsBinary(root, buf, depth, textMode, symTable);

        // Reset parse context at the start of every VDF parse so numeric keys
        // from loginusers.vdf / config.vdf don't contaminate cloud scrubbers
        // in hkFindOrCreateKey for the next file parsed.
        g_ParsedAppId = 0;

        // License injection only needed offline — online, Steam has real licenses
        // from Valve.  Injecting unconditionally corrupts config.vdf / loginusers.vdf
        // which then persists across restarts and blocks new game unlocks.
        if (!Hooks_Misc::IsOfflineMode()) return ok;

        if (ok && root && oFindOrCreateKey) {
            int32 symbol = (*reinterpret_cast<int32*>(root)) & 0xFFFFFF;
            const char* name = GetKeyName(symbol);

            if (name && _stricmp(name, "Licenses") == 0) {
                KeyValues* pLicenseNode = oFindOrCreateKey(root, "0", true, nullptr);
                if (pLicenseNode) {
                    KV_SetString(oFindOrCreateKey(pLicenseNode, "State",         true, nullptr), "1");
                    KV_SetString(oFindOrCreateKey(pLicenseNode, "LicenseType",   true, nullptr), "1");
                    KV_SetString(oFindOrCreateKey(pLicenseNode, "PaymentMethod", true, nullptr), "1");
                    KV_SetString(oFindOrCreateKey(pLicenseNode, "TimeCreated",   true, nullptr), "1700000000");
                }
            }
        }
        return ok;
    }

    // ── Active game spoofer state ──────────────────────────────────────────
    static KeyValues* g_Node480 = nullptr;
    static KeyValues* g_Node480Common = nullptr;
    static KeyValues* g_NodeActiveApp = nullptr;
    static KeyValues* g_NodeActiveAppCommon = nullptr;

    static std::vector<KeyValues*> g_Node480Locales;
    static std::vector<KeyValues*> g_NodeActiveAppLocales;

    static std::string g_RealName;
    static std::string g_RealIcon;
    static std::string g_RealClientIcon;
    static std::string g_RealLogo;
    static std::string g_RealLogoSmall;

    static AppId_t g_CachedAppId = 0;

    AppId_t GetActiveAppId() {
        if (g_CachedAppId == 0) g_CachedAppId = Hooks_Misc::ResolveAppId();
        return g_CachedAppId;
    }

    void EnsureRealName(AppId_t appId) {
        if (g_RealName.empty() && appId != 0) g_RealName = Hooks_Misc::GetGameNameByAppID(appId);
    }

    bool VecContains(const std::vector<KeyValues*>& vec, KeyValues* ptr) {
        return std::find(vec.begin(), vec.end(), ptr) != vec.end();
    }

    KeyValues* WINAPI hkFindOrCreateKey(
        KeyValues* pThis, const char* setName, bool bCreate, KeyValues** ppUnknown)
    {
        if (!pThis || !setName) return oFindOrCreateKey(pThis, setName, bCreate, ppUnknown);

        std::string_view keyQuery(setName);

        // Track the most recently parsed numeric key as the current AppID context.
        if (isdigit(static_cast<unsigned char>(setName[0]))) {
            AppId_t id = static_cast<AppId_t>(std::strtoul(setName, nullptr, 10));
            if (id > 0) g_ParsedAppId = id;
        }

        // ════════════════════════════════════════════════════════════════════
        // CLOUD STATE SCRUBBERS — surgical, name-keyed only
        //
        // Fire only during localconfig.vdf parsing for spoofed AppIDs.
        // Wipes stale cloud-error state so library badges stay clean.
        // ════════════════════════════════════════════════════════════════════

        if (keyQuery == "CloudSyncError" || keyQuery == "cloudsyncerror") {
            KeyValues* pResult = oFindOrCreateKey(pThis, setName, bCreate, ppUnknown);
            if (pResult && g_ParsedAppId > 0 && LuaConfig::HasDepot(g_ParsedAppId))
                KV_SetString(pResult, "");
            return pResult;
        }

        if (keyQuery == "last_sync_state" || keyQuery == "LastSyncState") {
            KeyValues* pResult = oFindOrCreateKey(pThis, setName, bCreate, ppUnknown);
            if (pResult && g_ParsedAppId > 0 && LuaConfig::HasDepot(g_ParsedAppId))
                KV_SetString(pResult, "synchronized");
            return pResult;
        }

        // Force cloud sync flag to user-disabled so Steam stops trying to upload.
        if (keyQuery == "CloudEnabled"    || keyQuery == "EnableCloudSync" ||
            keyQuery == "CloudSync"       || keyQuery == "CloudDesktopSync")
        {
            if (g_ParsedAppId > 0 && LuaConfig::HasDepot(g_ParsedAppId)) {
                KeyValues* pResult = oFindOrCreateKey(pThis, setName, bCreate, ppUnknown);
                if (pResult) KV_SetString(pResult, "0");
                return pResult;
            }
        }

        KeyValues* pResult = oFindOrCreateKey(pThis, setName, bCreate, ppUnknown);
        if (!pResult) return pResult;

        // ── Active game spoofer (Spacewar 480) ──────────────────────────────
        AppId_t activeAppId = GetActiveAppId();
        if (activeAppId == 0) return pResult;

        const std::string activeAppStr = std::to_string(activeAppId);

        if (keyQuery == "480")           g_Node480       = pResult;
        if (keyQuery == activeAppStr)    g_NodeActiveApp = pResult;

        if (keyQuery == "common") {
            if (pThis == g_Node480)
                g_Node480Common = pResult;
            else if (pThis == g_NodeActiveApp)
                g_NodeActiveAppCommon = pResult;
            else if (g_NodeActiveAppCommon == nullptr && g_Node480Common != nullptr && pThis != g_Node480)
                g_NodeActiveAppCommon = pResult;
        }

        auto TrackLocale = [&](KeyValues* commonNode, std::vector<KeyValues*>& localeVec) {
            if (pThis != commonNode) return;
            if (keyQuery == "name" || keyQuery == "SortAs" ||
                keyQuery == "icon" || keyQuery == "logo" ||
                keyQuery == "clienticon" || keyQuery == "logo_small") return;
            if (!VecContains(localeVec, pResult)) localeVec.push_back(pResult);
        };

        TrackLocale(g_Node480Common,       g_Node480Locales);
        TrackLocale(g_NodeActiveAppCommon, g_NodeActiveAppLocales);

        const bool is480Locale        = VecContains(g_Node480Locales,        pThis);
        const bool isActiveAppLocale  = VecContains(g_NodeActiveAppLocales,  pThis);

        // Capture real name/icon from the active app's common section
        if (pThis == g_NodeActiveAppCommon || isActiveAppLocale) {
            const char* val = KV_GetString(pResult);
            if (val && val[0] != '\0') {
                if (keyQuery == "name"       || keyQuery == "SortAs")  g_RealName        = val;
                else if (keyQuery == "icon")                            g_RealIcon        = val;
                else if (keyQuery == "clienticon")                      g_RealClientIcon  = val;
                else if (keyQuery == "logo")                            g_RealLogo        = val;
                else if (keyQuery == "logo_small")                      g_RealLogoSmall   = val;
            }
        }

        // Replace "Spacewar" with the real game name wherever it appears
        if (keyQuery == "name" || keyQuery == "SortAs" ||
            keyQuery == "gamedir" || keyQuery == "localized_name")
        {
            const char* val = KV_GetString(pResult);
            if (val) {
                std::string lower(val);
                for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower.find("spacewar") != std::string::npos) {
                    EnsureRealName(activeAppId);
                    if (!g_RealName.empty()) KV_SetString(pResult, g_RealName.c_str());
                }
            }
        }

        // Overlay 480's common section with the real game's assets
        if (pThis == g_Node480Common || is480Locale) {
            if (keyQuery == "name" || keyQuery == "SortAs") {
                EnsureRealName(activeAppId);
                if (!g_RealName.empty()) KV_SetString(pResult, g_RealName.c_str());
            }
            else if (keyQuery == "icon") {
                KV_SetString(pResult, !g_RealIcon.empty()
                    ? g_RealIcon.c_str()
                    : "b0dd14e414c81fce7288764b85c18684725bebf7");
            }
            else if (keyQuery == "clienticon" && !g_RealClientIcon.empty())
                KV_SetString(pResult, g_RealClientIcon.c_str());
            else if (keyQuery == "logo"       && !g_RealLogo.empty())
                KV_SetString(pResult, g_RealLogo.c_str());
            else if (keyQuery == "logo_small" && !g_RealLogoSmall.empty())
                KV_SetString(pResult, g_RealLogoSmall.c_str());
        }

        return pResult;
    }

} // anonymous namespace

namespace Hooks_KeyValues {

    void Install() {
        RESOLVE_C(FindOrCreateKey);

        HOOK_BEGIN();
        INSTALL_HOOK_C(ReadAsBinary);

        // Use the native macro instead of raw DetourAttach
        if (oFindOrCreateKey) {
            INSTALL_HOOK_C(FindOrCreateKey);
        }
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(ReadAsBinary);

        // Use the native macro instead of raw DetourDetach
        if (oFindOrCreateKey) {
            UNINSTALL_HOOK_C(FindOrCreateKey);
        }
        UNHOOK_END();

        // Preserve your exact variable cleanups
        g_Node480 = nullptr; g_Node480Common = nullptr;
        g_NodeActiveApp = nullptr; g_NodeActiveAppCommon = nullptr;
        g_CachedAppId = 0; g_ParsedAppId = 0;
        g_Node480Locales.clear(); g_NodeActiveAppLocales.clear();
        g_RealName.clear(); g_RealIcon.clear(); g_RealClientIcon.clear();
        g_RealLogo.clear(); g_RealLogoSmall.clear();
    }

} // namespace Hooks_KeyValues
