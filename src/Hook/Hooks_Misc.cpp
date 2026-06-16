#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/HookSupport/VehCommon.h"
#include "dllmain.h"
#include "Utils/SteamMetadata/PatternLoader.h"

#include <fstream>
#include <thread>
#include <shlobj.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace {
    // ── Resolve-only functions ─────────────────────────────────────
    RESOLVE_FUNC(CUtlBufferEnsureCapacity, void*, CUtlBuffer* pCUtlBuffer, uint32 newCapacity);

    // ── VEH-captured functions (one-shot int3) ───────────────────────────────
    // On int3 hit, ctx->Rcx is stored to the named output variable.
    CAPTURE_THIS_FUNC(GetAppIDForCurrentPipe, AppId_t,      g_steamEngine,    void*);
    CAPTURE_THIS_FUNC(GetAppDataFromAppInfo,  int64,        g_pCAppInfoCache, void*, AppId_t, const char*, uint8*, int32);

    // Assumes one game at a time.  Set by SpawnProcess VEH when -onlinefix
    // is detected; cleared when a non-onlinefix game launches.
    AppId_t   g_OnlineFixRealAppId;
    std::unordered_map<AppId_t, std::string> g_GameNameCache;
    bool      g_IsOffline = false;


    // ── SpawnProcess interception ────────────────────────────────────────────
    // CUser_SpawnProcess(pCUser, pExePath, pCommandLine, pWorkingDir,
    //                    pGameID, ...)
    // arg1=pCUser, arg2=pExePath, arg3=pCommandLine, arg4=pWorkingDir
    // arg5=pGameID (CGameID*; low 24 bits = AppId)
    // ── Safe Offline Checker ────────────────────────────────────────
    static bool IsSteamInOfflineMode() {
        if (wcsstr(GetCommandLineW(), L"-offline")) return true;
        char steamPath[MAX_PATH] = {};
        DWORD pathSize = sizeof(steamPath);
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(steamPath), &pathSize);
            RegCloseKey(hKey);
            if (steamPath[0] != '\0') {
                std::string vdfPath = std::string(steamPath) + "/config/loginusers.vdf";
                std::ifstream file(vdfPath);
                std::string line;
                while (std::getline(file, line)) {
                    if (line.find("\"WantsOfflineMode\"") != std::string::npos
                     && line.find("\"1\"") != std::string::npos) return true;
                }
            }
        }
        return false;
    }

    // ── Cloud sync via companion (ONENNABE / KingStore) ─────────────
    // Reads the companion's dynamic Flask port from
    //   %LOCALAPPDATA%\OnennabeCloudSaves\onennabe.port   (ONENNABE)
    //   %LOCALAPPDATA%\KingstoreCloudSaves\kingstore.port  (KingStore)
    // and POSTs sync triggers on game launch / exit.
    namespace CloudSync {

        static uint16_t ReadAdvertisedPort() {
            wchar_t localAppDataBuf[MAX_PATH] = {};
            const wchar_t* localAppData = nullptr;
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppDataBuf, MAX_PATH) > 0) {
                localAppData = localAppDataBuf;
            } else {
                wchar_t* shellPath = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &shellPath))) {
                    wcsncpy_s(localAppDataBuf, shellPath, MAX_PATH - 1);
                    CoTaskMemFree(shellPath);
                    localAppData = localAppDataBuf;
                }
            }
            if (!localAppData || !localAppData[0]) return 0;

            // Try ONENNABE port file first, then KingStore
            const wchar_t* kPaths[] = {
                L"\\OnennabeCloudSaves\\onennabe.port",
                L"\\KingstoreCloudSaves\\kingstore.port",
            };
            for (const auto* rel : kPaths) {
                std::wstring path = std::wstring(localAppData) + rel;
                HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) continue;
                char buf[256] = {};
                DWORD bytesRead = 0;
                ReadFile(h, buf, sizeof(buf) - 1, &bytesRead, nullptr);
                CloseHandle(h);
                if (bytesRead == 0) continue;
                const char* p = strstr(buf, "\"port\"");
                if (!p) continue;
                p = strchr(p, ':');
                if (!p) continue;
                ++p;
                while (*p && !isdigit(static_cast<unsigned char>(*p))) ++p;
                if (!isdigit(static_cast<unsigned char>(*p))) continue;
                uint16_t port = static_cast<uint16_t>(atoi(p));
                LOG_MISC_DEBUG("CloudSync: port={} from {}", port,
                    std::string(path.begin(), path.end()));
                return port;
            }
            return 0;
        }

        static bool DoPost(uint16_t port, const wchar_t* urlPath,
            const std::string& body, DWORD timeoutMs)
        {
            HINTERNET hSession = WinHttpOpen(L"OSTCloudSync/1.0",
                WINHTTP_ACCESS_TYPE_NO_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSession) return false;
            WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
            HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", port, 0);
            if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
            HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", urlPath,
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (!hReq) {
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return false;
            }
            BOOL ok = WinHttpSendRequest(hReq,
                L"Content-Type: application/json\r\n", -1L,
                const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()), 0);
            if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
            DWORD status = 0, sz = sizeof(status);
            if (ok) {
                WinHttpQueryHeaders(hReq,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    nullptr, &status, &sz, nullptr);
                // Must drain body — blocks until Python finishes pulling saves
                DWORD avail = 0;
                do {
                    if (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                        std::vector<char> tmp(avail);
                        DWORD read = 0;
                        WinHttpReadData(hReq, tmp.data(), avail, &read);
                    }
                } while (avail > 0);
            }
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return ok && status >= 200 && status < 300;
        }

        // Forward declarations — implementations after GetAppDataFromAppInfo hook
        static std::string              ResolveRootToken(const std::string& token);
        static std::string              ResolveGameInstallDir(AppId_t appId);
        static std::string              ReadAppInfoString(AppId_t appId, const char* key);
        static std::vector<std::string> DiscoverManifestPaths(AppId_t appId);
        static std::string              BuildBody(AppId_t appId, const std::vector<std::string>& paths);

        static bool TriggerPull(AppId_t appId) {
            uint16_t port = ReadAdvertisedPort();
            if (port == 0) {
                LOG_MISC_DEBUG("CloudSync: companion not running, skipping pull for {}", appId);
                return false;
            }
            auto paths = DiscoverManifestPaths(appId);
            auto body  = BuildBody(appId, paths);
            LOG_MISC_INFO("CloudSync: /pull AppId={} port={}", appId, port);
            bool ok = DoPost(port, L"/cloudsync/pull", body, 10000);
            if (!ok) LOG_MISC_WARN("CloudSync: /pull failed (companion down or timeout)");
            return ok;
        }

        static void TriggerPush(AppId_t appId) {
            uint16_t port = ReadAdvertisedPort();
            if (port == 0) return;
            auto paths = DiscoverManifestPaths(appId);
            auto body  = BuildBody(appId, paths);
            LOG_MISC_INFO("CloudSync: /push AppId={} port={}", appId, port);
            DoPost(port, L"/cloudsync/push", body, 5000);
        }

        static void WatchExitAndPush(AppId_t watchAppId, AppId_t cloudAppId) {
            std::wstring keyPath = L"Software\\Valve\\Steam\\Apps\\"
                + std::to_wstring(watchAppId);
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0,
                KEY_READ | KEY_NOTIFY, &hKey) != ERROR_SUCCESS) return;

            DWORD running = 0, sz = sizeof(running);
            for (int i = 0; i < 30 && running != 1; ++i) {
                sz = sizeof(running);
                RegQueryValueExW(hKey, L"Running", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(&running), &sz);
                if (running != 1) Sleep(200);
            }
            if (running != 1) { RegCloseKey(hKey); return; }

            HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            for (int loops = 0; loops < 8640; ++loops) {
                RegNotifyChangeKeyValue(hKey, FALSE,
                    REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE);
                WaitForSingleObject(hEvent, 10000);
                sz = sizeof(running);
                if (RegQueryValueExW(hKey, L"Running", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(&running), &sz) != ERROR_SUCCESS) break;
                if (running == 0) {
                    LOG_MISC_INFO("CloudSync: AppId {} exited, triggering push", cloudAppId);
                    TriggerPush(cloudAppId);
                    break;
                }
            }
            CloseHandle(hEvent);
            RegCloseKey(hKey);
        }

    } // namespace CloudSync

    static void OnSpawnProcessHit(OSTPlatform::Trap::Context& ctx, const VehCommon::Int3Site& /*site*/) {
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);
        AppId_t watchAppId = appId;

        if (LuaConfig::HasDepot(appId) && cmdLine && strstr(cmdLine, "-onlinefix"))
        {
            g_OnlineFixRealAppId = appId;
            pGameID->SetAppID(kOnlineFixAppId);
            watchAppId = kOnlineFixAppId;
            LOG_MISC_INFO("SpawnProcess: appid {} -> {}, cmd=\"{}\"", appId, kOnlineFixAppId, cmdLine);
        } else {
            g_OnlineFixRealAppId = 0;
        }

        // Pull saves synchronously before game reads them; push on exit.
        if (LuaConfig::HasDepot(appId)) {
            CloudSync::TriggerPull(appId);
            std::thread(CloudSync::WatchExitAndPush, watchAppId, appId).detach();
        }
    }

    // ── SteamController_OptedInMask ──────────────────────────────────────────
    // Called by CUser_BuildSpawnEnvBlock with pGameID's appid to
    // compute EnableConfiguratorSupport and the SDL_* env vars.
    // With 480 the spawned game inherits Spacewar's Steam Input
    // opt-in and gameoverlayrenderer hijacks the XInput stream.
    HOOK_FUNC(OptedInMask, int64,void* pThis, AppId_t appId)
    {
        if (appId == kOnlineFixAppId && g_OnlineFixRealAppId) {
            LOG_MISC_INFO("OptedInMask: appid {} -> {}",appId, g_OnlineFixRealAppId);
            appId = g_OnlineFixRealAppId;
        }
        return oOptedInMask(pThis, appId);
    }

    // ── CUser_BuildSpawnEnvBlock ─────────────────────────────────────────────
    // pOverlayCGameID drives SteamOverlayGameId, which the in-game
    // overlay reads for screenshot tags, community URLs, and asset
    // selection.  pCGameID drives SteamGameId / SteamAppId; leave it
    // at 480 so the in-game ownership bypass holds.
    HOOK_FUNC(BuildSpawnEnvBlock, int64,
              void* pThis, CGameID* pCGameID, void* a3, void* env,
              CGameID* pOverlayCGameID, void* a6, int a7,
              void* a8, void* a9, unsigned int a10, char a11)
    {
        if (g_OnlineFixRealAppId && pOverlayCGameID
            && pOverlayCGameID->AppID(true) == kOnlineFixAppId) 
        {
            LOG_MISC_INFO("BuildSpawnEnvBlock: SetAppID in OverlayCGameID {} -> {}",
                          pOverlayCGameID->AppID(true), g_OnlineFixRealAppId);
            pOverlayCGameID->SetAppID(g_OnlineFixRealAppId);
        }
        return oBuildSpawnEnvBlock(pThis, pCGameID, a3, env,
                                    pOverlayCGameID, a6, a7,
                                    a8, a9, a10, a11);
    }

    // CAppInfoCache::GetOrAddAppData
    // The injected package keeps Lua-provided ids in PackageInfo::AppIdVec.
    // Some of those ids can actually be depot ids, but we cannot trust the
    // Lua config to classify app ids and depot ids for us. In offline mode,
    // depot ids usually have only placeholder appinfo data. That blocks
    // CClientAppManager_ProcessPendingLicenseUpdates, because it waits for
    // every AppIdVec entry to have resolved appinfo unless the entry has been
    // marked as a known-unknown id by the PICS path. For injected ids that
    // still have placeholder appinfo, set skip_flag so Steam treats them like
    // PICS unknown_appids instead of keeping the license update pending.
    HOOK_FUNC(GetOrAddAppData,CAppData*,void* pCache, AppId_t appId,bool bCreate)
    {
        CAppData* pData = oGetOrAddAppData(pCache, appId, bCreate);
        // Only active in offline mode — online, PICS resolves app info normally.
        if (!g_IsOffline) return pData;
        if (LuaConfig::HasDepot(appId, false) && pData && !bCreate && pData->IsUnresolvedAppInfo()) {
            LOG_MISC_DEBUG("GetOrAddAppData: offline — marking {} skip_flag=true", appId);
            pData->bSkipFlag = true;
        }
        return pData;
    }

    // ── CloudSync manifest discovery implementations ──────────────────────
    // Placed here — after CAPTURE_THIS_FUNC(GetAppDataFromAppInfo) ensures
    // oGetAppDataFromAppInfo is declared before these functions call it.
    namespace CloudSync {

        std::string ReadAppInfoString(AppId_t appId, const char* key) {
            if (!CAPTURE_READY(GetAppDataFromAppInfo)) return "";
            char buf[1024] = {};
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, key,
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            return (len > 1) ? std::string(buf, static_cast<size_t>(len - 1)) : "";
        }

        std::string ResolveRootToken(const std::string& token) {
            static const struct { const char* name; const KNOWNFOLDERID* fid; } kTokens[] = {
                { "WinMyDocuments",     &FOLDERID_Documents       },
                { "WinAppDataLocal",    &FOLDERID_LocalAppData    },
                { "WinAppDataLocalLow", &FOLDERID_LocalAppDataLow },
                { "WinAppDataRoaming",  &FOLDERID_RoamingAppData  },
                { "WinSavedGames",      &FOLDERID_SavedGames      },
                { "LinuxXdgDataHome",   &FOLDERID_RoamingAppData  },
                { "MacAppSupport",      &FOLDERID_RoamingAppData  },
            };
            for (const auto& t : kTokens) {
                if (_stricmp(token.c_str(), t.name) == 0) {
                    PWSTR pwsz = nullptr;
                    if (SHGetKnownFolderPath(*t.fid, 0, nullptr, &pwsz) == S_OK && pwsz) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0, nullptr, nullptr);
                        std::string out(len > 0 ? len - 1 : 0, '\0');
                        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, out.data(), len, nullptr, nullptr);
                        CoTaskMemFree(pwsz);
                        return out;
                    }
                }
            }
            if (_stricmp(token.c_str(), "gameinstall") == 0 ||
                _stricmp(token.c_str(), "App Install Directory") == 0)
                return "{GAME_INSTALL_DIR}";
            return "";
        }

        std::string ResolveGameInstallDir(AppId_t appId) {
            char steamPath[MAX_PATH] = {};
            HKEY hKey;
            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0,
                KEY_READ, &hKey) != ERROR_SUCCESS) return "";
            DWORD sz = sizeof(steamPath);
            RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(steamPath), &sz);
            RegCloseKey(hKey);
            if (!steamPath[0]) return "";

            std::vector<std::string> libs;
            libs.emplace_back(steamPath);
            std::string libVdf = std::string(steamPath) + "/steamapps/libraryfolders.vdf";
            std::ifstream f(libVdf);
            std::string line;
            while (std::getline(f, line)) {
                auto p = line.find("\"path\"");
                if (p == std::string::npos) continue;
                auto q1 = line.find('"', p+6), q2 = line.find('"', q1+1);
                auto q3 = line.find('"', q2+1), q4 = line.find('"', q3+1);
                if (q4 != std::string::npos) libs.emplace_back(line.substr(q3+1, q4-q3-1));
            }
            for (auto& lib : libs) {
                std::string acf = lib + "/steamapps/appmanifest_" + std::to_string(appId) + ".acf";
                std::ifstream af(acf);
                if (!af) continue;
                std::string l;
                while (std::getline(af, l)) {
                    auto p = l.find("\"installdir\"");
                    if (p == std::string::npos) continue;
                    auto q1 = l.find('"', p+12), q2 = l.find('"', q1+1);
                    auto q3 = l.find('"', q2+1), q4 = l.find('"', q3+1);
                    if (q4 != std::string::npos)
                        return lib + "/steamapps/common/" + l.substr(q3+1, q4-q3-1);
                }
            }
            return "";
        }

        std::vector<std::string> DiscoverManifestPaths(AppId_t appId) {
            std::vector<std::string> out;
            for (int i = 0; i < 64; ++i) {
                std::string base = "ufs/savefiles/" + std::to_string(i);
                std::string root = ReadAppInfoString(appId, (base + "/root").c_str());
                if (root.empty()) break;
                std::string sub   = ReadAppInfoString(appId, (base + "/path").c_str());
                std::string platf = ReadAppInfoString(appId, (base + "/platforms/0").c_str());
                if (!platf.empty()) {
                    bool ok = false;
                    for (int p = 0; p < 4 && !ok; ++p) {
                        std::string pf = ReadAppInfoString(appId,
                            (base + "/platforms/" + std::to_string(p)).c_str());
                        if (pf.empty()) break;
                        ok = _stricmp(pf.c_str(), "windows") == 0 || _stricmp(pf.c_str(), "all") == 0;
                    }
                    if (!ok) continue;
                }
                std::string base_dir = ResolveRootToken(root);
                if (base_dir == "{GAME_INSTALL_DIR}") base_dir = ResolveGameInstallDir(appId);
                if (base_dir.empty()) continue;
                std::string full = base_dir;
                if (!sub.empty()) {
                    if (full.back() != '\\' && full.back() != '/') full += '\\';
                    for (char& c : sub) if (c == '/') c = '\\';
                    full += sub;
                }
                while (!full.empty() && (full.back() == '\\' || full.back() == '/')) full.pop_back();
                out.push_back(full);
                LOG_MISC_INFO("CloudSync: manifest[{}] AppId={} root={} -> {}", i, appId, root, full);
            }
            return out;
        }

        std::string BuildBody(AppId_t appId, const std::vector<std::string>& paths) {
            std::string body = "{\"appid\":" + std::to_string(appId);
            if (!paths.empty()) {
                body += ",\"paths\":[";
                for (size_t i = 0; i < paths.size(); ++i) {
                    if (i) body += ',';
                    body += '"';
                    for (char c : paths[i]) { if (c == '\\' || c == '"') body += '\\'; body += c; }
                    body += '"';
                }
                body += ']';
            }
            body += '}';
            return body;
        }

    } // namespace CloudSync

}

namespace Hooks_Misc {
    void Install() {
        RESOLVE_C(CUtlBufferEnsureCapacity);

        ARM_CAPTURE_C(GetAppIDForCurrentPipe);
        ARM_CAPTURE_C(GetAppDataFromAppInfo);

        ARM_INT3_C(SpawnProcess, true, &OnSpawnProcessHit, nullptr);

        g_IsOffline = !PatternLoader::WasRemoteReachable() || IsSteamInOfflineMode();
        LOG_MISC_INFO("Hooks_Misc: offline mode = {}", g_IsOffline);

        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildSpawnEnvBlock);
        INSTALL_HOOK_C(OptedInMask);
        INSTALL_HOOK_C(GetOrAddAppData);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildSpawnEnvBlock);
        UNINSTALL_HOOK(OptedInMask);
        UNINSTALL_HOOK(GetOrAddAppData);
        UNHOOK_END();
    }

    AppId_t GetAppIDForCurrentPipeWrap() {
        if (!CAPTURE_READY(GetAppIDForCurrentPipe)) {
            LOG_MISC_WARN("GetAppIDForCurrentPipeWrap called before capture — returning 0");
            return 0;
        }
        auto appid = oGetAppIDForCurrentPipe(g_steamEngine);
        if (!appid) {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId=0(Not GamePipe)");
        } else {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId={}", appid);
        }
        return appid;
    }

    
    AppId_t ResolveAppId() {
        if (g_OnlineFixRealAppId) return g_OnlineFixRealAppId;
        return GetAppIDForCurrentPipeWrap();
    }
    
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity,bool updatePut)
    {
        if (oCUtlBufferEnsureCapacity) {
            LOG_MISC_DEBUG("Before ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            oCUtlBufferEnsureCapacity(pWrite, newCapacity);
            LOG_MISC_DEBUG("After ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            if(updatePut) pWrite->m_Put = newCapacity;
            return true;
        }
        LOG_MISC_WARN("EnsureBufferCapacity: oCUtlBufferEnsureCapacity not resolved");
        return false;
    }

    // ── Game name ────────────────────────────────────────────────
    std::string GetGameNameByAppID(AppId_t appId)
    {
        auto it = g_GameNameCache.find(appId);
        if (it != g_GameNameCache.end()) return it->second;

        std::string name;

        if (CAPTURE_READY(GetAppDataFromAppInfo)) {
            char buf[256] = {};
            // "common/name" triggers auto-localization: the function detects
            // prefix "common" (keyType=2) + key "name", then tries
            // "name_localized/<current_lang>" before falling back to "name".
            // Returns strlen+1 on success, -1 on failure.
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, "common/name",
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1)
                name.assign(buf, static_cast<size_t>(len - 1));
        }

        LOG_MISC_DEBUG("GetGameNameByAppID({}): {}", appId, name);
        g_GameNameCache[appId] = name;
        return name;
    }

    bool IsOfflineMode() { return g_IsOffline; }

}
