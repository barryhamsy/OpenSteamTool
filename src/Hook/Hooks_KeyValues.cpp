// Hook KeyValues — entry point for KV-tree manipulation.
// NOTE: This is the fork's 480 spoofer + cloud eradicator. Upstream
// dropped the FindOrCreateKey-based logic (repurposed the file for
// ReadAsBinary); we keep it. Only the resolve macro was updated to the
// new _C convention. Do not "sync to upstream" — this is intentional.

#include "Hooks_KeyValues.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Hooks_Misc.h"
#include <string_view>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace {

    // New macro convention: RESOLVE_FUNC generates FindOrCreateKey_t and
    // oFindOrCreateKey for us (replaces the old hand-written typedef).
    RESOLVE_FUNC(FindOrCreateKey, KeyValues*, KeyValues* parent, const char* name, bool create, KeyValues** out);

    // ── KV field access — validated offset ─────────────────────────
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

    // ── Hierarchical Tree Trackers & Persistent Memory ──
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
        if (g_CachedAppId == 0)
            g_CachedAppId = Hooks_Misc::ResolveAppId();
        return g_CachedAppId;
    }

    void EnsureRealName(AppId_t appId) {
        if (g_RealName.empty() && appId != 0)
            g_RealName = Hooks_Misc::GetGameNameByAppID(appId);
    }

    bool VecContains(const std::vector<KeyValues*>& vec, KeyValues* ptr) {
        return std::find(vec.begin(), vec.end(), ptr) != vec.end();
    }

    // Thread-local parser for boot-time file scanning
    thread_local AppId_t g_ParsedAppId = 0;

    // ── The Dual-Brain Hook ───────────────────────────────────────────────
    KeyValues* WINAPI hkFindOrCreateKey(
        KeyValues* pThis, const char* setName, bool bCreate, KeyValues** ppUnknown)
    {
        KeyValues* pResult = oFindOrCreateKey(pThis, setName, bCreate, ppUnknown);
        if (!pThis || !setName || !pResult) return pResult;

        std::string_view keyQuery(setName);

        // ── 1. BOOT-TIME CLOUD ERADICATOR ──
        // Tracks AppID nodes as Steam reads localconfig.vdf during startup.
        if (isdigit(static_cast<unsigned char>(setName[0]))) {
            AppId_t id = static_cast<AppId_t>(std::strtoul(setName, nullptr, 10));
            if (id > 0) g_ParsedAppId = id;
        }

        // If Steam is parsing a game we manage, sanitize its cloud state immediately in memory.
        if (g_ParsedAppId > 0 && LuaConfig::HasDepot(g_ParsedAppId)) {
            if (keyQuery == "ufs" || keyQuery == "cloudenabled" ||
                keyQuery == "cloud_enabled" || keyQuery == "CloudEnabled") {
                KV_SetString(pResult, "0");
            }
            else if (keyQuery == "last_sync_state") {
                // FIXED: Steam expects exactly "synchronized" to clear the UI error flag
                KV_SetString(pResult, "synchronized");
            }
            else if (keyQuery == "CloudSyncError") {
                KV_SetString(pResult, ""); // Wipes the internal error memory
            }
        }

        // ── 2. ACTIVE GAME SPOOFER (Spacewar 480) ──
        AppId_t activeAppId = GetActiveAppId();

        // If no game is running, return early to save CPU. 
        // Boot-time cloud eradication has already finished above.
        if (activeAppId == 0) return pResult;

        const std::string activeAppStr = std::to_string(activeAppId);

        // ── STRUCTURAL TRACKING ──
        if (keyQuery == "480") g_Node480 = pResult;
        if (keyQuery == activeAppStr) g_NodeActiveApp = pResult;

        if (keyQuery == "common") {
            if (pThis == g_Node480) g_Node480Common = pResult;
            else if (pThis == g_NodeActiveApp) g_NodeActiveAppCommon = pResult;
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

        TrackLocale(g_Node480Common, g_Node480Locales);
        TrackLocale(g_NodeActiveAppCommon, g_NodeActiveAppLocales);

        const bool is480Locale = VecContains(g_Node480Locales, pThis);
        const bool isActiveAppLocale = VecContains(g_NodeActiveAppLocales, pThis);

        // ── DATA CAPTURE ──
        if (pThis == g_NodeActiveAppCommon || isActiveAppLocale) {
            const char* val = KV_GetString(pResult);
            if (val && val[0] != '\0') {
                if (keyQuery == "name" || keyQuery == "SortAs") g_RealName = val;
                else if (keyQuery == "icon")                    g_RealIcon = val;
                else if (keyQuery == "clienticon")              g_RealClientIcon = val;
                else if (keyQuery == "logo")                    g_RealLogo = val;
                else if (keyQuery == "logo_small")              g_RealLogoSmall = val;
            }
        }

        // ── BLANKET SPOOF ──
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

        // ── TREE SPOOF (Spacewar 480 Injection) ──
        if (pThis == g_Node480Common || is480Locale) {
            if (keyQuery == "name" || keyQuery == "SortAs") {
                EnsureRealName(activeAppId);
                if (!g_RealName.empty()) KV_SetString(pResult, g_RealName.c_str());
            }
            else if (keyQuery == "icon") {
                KV_SetString(pResult, !g_RealIcon.empty() ? g_RealIcon.c_str() : "b0dd14e414c81fce7288764b85c18684725bebf7");
            }
            else if (keyQuery == "clienticon" && !g_RealClientIcon.empty()) KV_SetString(pResult, g_RealClientIcon.c_str());
            else if (keyQuery == "logo" && !g_RealLogo.empty()) KV_SetString(pResult, g_RealLogo.c_str());
            else if (keyQuery == "logo_small" && !g_RealLogoSmall.empty()) KV_SetString(pResult, g_RealLogoSmall.c_str());
        }

        return pResult;
    }

} // anonymous namespace

namespace Hooks_KeyValues {

    void Install() {
        RESOLVE_C(FindOrCreateKey);

        if (!oFindOrCreateKey) {
            LOG_KEYVALUE_WARN("KeyValues: Failed to resolve FindOrCreateKey via PatternLoader.");
            return;
        }

        HOOK_BEGIN();
        DetourAttach(reinterpret_cast<PVOID*>(&oFindOrCreateKey), reinterpret_cast<PVOID>(hkFindOrCreateKey));
        HOOK_END();

        LOG_KEYVALUE_INFO("KeyValues: Proven tracking layer successfully initialized.");
    }

    void Uninstall() {
        if (oFindOrCreateKey) {
            UNHOOK_BEGIN();
            DetourDetach(reinterpret_cast<PVOID*>(&oFindOrCreateKey), reinterpret_cast<PVOID>(hkFindOrCreateKey));
            UNHOOK_END();
        }

        g_Node480 = nullptr;
        g_Node480Common = nullptr;
        g_NodeActiveApp = nullptr;
        g_NodeActiveAppCommon = nullptr;
        g_CachedAppId = 0;
        g_Node480Locales.clear();
        g_NodeActiveAppLocales.clear();
        g_RealName.clear();
        g_RealIcon.clear();
        g_RealClientIcon.clear();
        g_RealLogo.clear();
        g_RealLogoSmall.clear();
    }

} // namespace Hooks_KeyValues