#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/PatternLoader.h"
#include "Utils/VehCommon.h"
#include "dllmain.h"

#include <fstream>
#include <string>

namespace {
    RESOLVE_FUNC(CUtlBufferEnsureCapacity, void*, CUtlBuffer* pCUtlBuffer, uint32 newCapacity);

    CAPTURE_THIS_FUNC(GetAppIDForCurrentPipe, AppId_t, g_steamEngine, void*);

    AppId_t   g_OnlineFixRealAppId;
    std::unordered_map<AppId_t, std::string> g_GameNameCache;
    bool g_IsOffline = false;
    void* g_pCAppInfoCache = nullptr;

    // ── Safe Offline Checker ──
    bool IsSteamInOfflineMode() {
        if (wcsstr(GetCommandLineW(), L"-offline")) return true;

        char steamPath[MAX_PATH] = { 0 };
        DWORD pathSize = sizeof(steamPath);
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(steamPath), &pathSize);
            RegCloseKey(hKey);

            if (steamPath[0] != '\0') {
                std::string vdfPath = std::string(steamPath) + "/config/loginusers.vdf";
                std::ifstream file(vdfPath);
                std::string line;
                while (std::getline(file, line)) {
                    if (line.find("\"WantsOfflineMode\"") != std::string::npos && line.find("\"1\"") != std::string::npos) return true;
                }
            }
        }
        return false;
    }

    static void OnSpawnProcessHit(PCONTEXT ctx, const VehCommon::Int3Site& /*site*/) {
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);

        if (LuaConfig::HasDepot(appId) && cmdLine && strstr(cmdLine, "-onlinefix")) {
            g_OnlineFixRealAppId = appId;
            pGameID->SetAppID(kOnlineFixAppId);
        }
        else {
            g_OnlineFixRealAppId = 0;
        }
    }

    HOOK_FUNC(OptedInMask, int64, void* pThis, AppId_t appId) {
        if (appId == kOnlineFixAppId && g_OnlineFixRealAppId) appId = g_OnlineFixRealAppId;
        return oOptedInMask(pThis, appId);
    }

    HOOK_FUNC(BuildSpawnEnvBlock, int64, void* pThis, CGameID* pCGameID, void* a3, void* env, CGameID* pOverlayCGameID, void* a6, int a7, void* a8, void* a9, unsigned int a10, char a11) {
        if (g_OnlineFixRealAppId && pOverlayCGameID && pOverlayCGameID->AppID(true) == kOnlineFixAppId) {
            pOverlayCGameID->SetAppID(g_OnlineFixRealAppId);
        }
        return oBuildSpawnEnvBlock(pThis, pCGameID, a3, env, pOverlayCGameID, a6, a7, a8, a9, a10, a11);
    }

    HOOK_FUNC(GetOrAddAppData, CAppData*, void* pCache, AppId_t appId, bool bCreate) {
        CAppData* pData = oGetOrAddAppData(pCache, appId, bCreate);
        if (!g_IsOffline) return pData;
        if (LuaConfig::HasDepot(appId, false) && pData && !bCreate && pData->IsUnresolvedAppInfo()) {
            pData->bSkipFlag = true;
        }
        return pData;
    }

    // ── Pure Cache Pointer Capture (No overrides!) ──
    HOOK_FUNC(GetAppDataFromAppInfo, int64, void* pThis, AppId_t appId, const char* key, uint8* buf, int32 bufSize) {
        g_pCAppInfoCache = pThis;
        return oGetAppDataFromAppInfo(pThis, appId, key, buf, bufSize);
    }
}

namespace Hooks_Misc {
    void Install() {
        RESOLVE_C(CUtlBufferEnsureCapacity);
        ARM_CAPTURE_C(GetAppIDForCurrentPipe);
        ARM_INT3_C(SpawnProcess, true, &OnSpawnProcessHit, nullptr);

        g_IsOffline = !PatternLoader::WasRemoteReachable() || IsSteamInOfflineMode();

        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildSpawnEnvBlock);
        INSTALL_HOOK_C(OptedInMask);
        INSTALL_HOOK_C(GetOrAddAppData);
        INSTALL_HOOK_C(GetAppDataFromAppInfo);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildSpawnEnvBlock);
        UNINSTALL_HOOK(OptedInMask);
        UNINSTALL_HOOK(GetOrAddAppData);
        UNINSTALL_HOOK(GetAppDataFromAppInfo);
        UNHOOK_END();
    }

    AppId_t GetAppIDForCurrentPipeWrap() {
        if (!CAPTURE_READY(GetAppIDForCurrentPipe)) return 0;
        return oGetAppIDForCurrentPipe(g_steamEngine);
    }

    AppId_t ResolveAppId() {
        if (g_OnlineFixRealAppId) return g_OnlineFixRealAppId;
        return GetAppIDForCurrentPipeWrap();
    }

    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity, bool updatePut) {
        if (oCUtlBufferEnsureCapacity) {
            oCUtlBufferEnsureCapacity(pWrite, newCapacity);
            if (updatePut) pWrite->m_Put = newCapacity;
            return true;
        }
        return false;
    }

    std::string GetGameNameByAppID(AppId_t appId) {
        auto it = g_GameNameCache.find(appId);
        if (it != g_GameNameCache.end()) return it->second;

        std::string name;
        if (oGetAppDataFromAppInfo && g_pCAppInfoCache) {
            char buf[256] = {};
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, "common/name", reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1) name.assign(buf, static_cast<size_t>(len - 1));
        }

        g_GameNameCache[appId] = name;
        return name;
    }
}