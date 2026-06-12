#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/PatternLoader.h"
#include "Utils/VehCommon.h"
#include "dllmain.h"

#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <shlobj.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

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

    // ── Cloud sync via ONENNABE companion ─────────────────────────────────
    // Reads ONENNABE's dynamic Flask port from
    //   %LOCALAPPDATA%\OpenSteamTool\onennabe.port
    // and POSTs sync triggers on game launch / exit.
    //
    // Pull is synchronous — blocks SpawnProcess until Drive→local completes
    // (or 10s timeout), so the game's first FileRead sees fresh saves.
    // Push runs on a detached thread that watches HKCU\...\Apps\<id>\Running
    // and POSTs /push when it transitions to 0.
    //
    // All operations fail gracefully: if ONENNABE isn't running, the port
    // file is missing, or the network is down, the game launches normally
    // with local-only saves.
    namespace CloudSync {

        static uint16_t ReadAdvertisedPort() {
            // Resolve %LOCALAPPDATA% — try environment variable first,
            // then SHGetKnownFolderPath as fallback (more reliable inside
            // injected DLLs and sandboxed environments like Windows Sandbox).
            wchar_t localAppDataBuf[MAX_PATH] = {};
            const wchar_t* localAppData = nullptr;

            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppDataBuf, MAX_PATH) > 0) {
                localAppData = localAppDataBuf;
            }
            else {
                wchar_t* shellPath = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &shellPath))) {
                    wcsncpy_s(localAppDataBuf, shellPath, MAX_PATH - 1);
                    CoTaskMemFree(shellPath);
                    localAppData = localAppDataBuf;
                }
            }
            if (!localAppData || !localAppData[0]) {
                LOG_MISC_DEBUG("CloudSync: ReadAdvertisedPort — LOCALAPPDATA not resolved");
                return 0;
            }

            std::wstring path = std::wstring(localAppData) + L"\\OnennabeCloudSaves\\onennabe.port";
            LOG_MISC_DEBUG("CloudSync: ReadAdvertisedPort — checking {}",
                std::string(path.begin(), path.end()));

            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                LOG_MISC_DEBUG("CloudSync: ReadAdvertisedPort — port file not found");
                return 0;
            }

            char buf[256] = {};
            DWORD bytesRead = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &bytesRead, nullptr);
            CloseHandle(h);
            if (bytesRead == 0) return 0;

            // Tiny JSON parse: find "port": then the first decimal digit
            const char* p = strstr(buf, "\"port\"");
            if (!p) return 0;
            p = strchr(p, ':');
            if (!p) return 0;
            ++p;
            while (*p && !isdigit(static_cast<unsigned char>(*p))) ++p;
            if (!isdigit(static_cast<unsigned char>(*p))) return 0;
            uint16_t port = static_cast<uint16_t>(atoi(p));
            LOG_MISC_DEBUG("CloudSync: ReadAdvertisedPort — port={}", port);
            return port;
        }

        static bool DoPost(uint16_t port, const wchar_t* urlPath,
            const std::string& body, DWORD timeoutMs)
        {
            HINTERNET hSession = WinHttpOpen(L"OnennabeCloudSaves/1.0",
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
            }

            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return ok && status >= 200 && status < 300;
        }

        // ── AutoCloud manifest discovery ──────────────────────────────────
        // Steam's appinfo cache stores a 'ufs/savefiles/N' manifest for every
        // game that uses external save paths.  Each entry has:
        //   root       — token like "WinMyDocuments" / "WinAppDataLocal"
        //   path       — subpath under that root
        //   recursive  — "1" = walk subdirs, "0" = top level only (unused here)
        //   pattern    — usually "*", we don't filter (Python walks fully)
        //
        // We read these via the existing oGetAppDataFromAppInfo hook and
        // g_pCAppInfoCache capture in Hooks_Misc, then resolve the root
        // tokens to absolute Windows paths.  Resolved paths are JSON-
        // serialised and passed to ONENNABE in the /pull and /push bodies.
        //
        // FORWARD DECLARATIONS only — the implementations live at the
        // bottom of the anonymous namespace (after the HOOK_FUNC that
        // declares oGetAppDataFromAppInfo, which they call).
        static std::string              ResolveRootToken(const std::string& token);
        static std::string              ResolveGameInstallDir(AppId_t appId);
        static std::string              ReadAppInfoString(AppId_t appId, const char* key);
        static std::vector<std::string> DiscoverManifestPaths(AppId_t appId);
        static std::string              BuildBody(AppId_t appId,
            const std::vector<std::string>& paths);
        // Synchronous pull. Blocks up to 10s. Returns false on any failure.
        static bool TriggerPull(AppId_t appId) {
            uint16_t port = ReadAdvertisedPort();
            if (port == 0) {
                LOG_MISC_DEBUG("CloudSync: ONENNABE not running, skipping pull for {}", appId);
                return false;
            }
            std::vector<std::string> paths = DiscoverManifestPaths(appId);
            std::string body = BuildBody(appId, paths);
            LOG_MISC_INFO("CloudSync: /pull AppId={} port={} manifest_paths={}",
                appId, port, paths.size());
            bool ok = DoPost(port, L"/cloudsync/pull", body, 10000);
            if (!ok) LOG_MISC_WARN("CloudSync: /pull failed (ONENNABE down or timeout)");
            return ok;
        }

        // Fire-and-forget push. Returns immediately; ONENNABE queues the work.
        static void TriggerPush(AppId_t appId) {
            uint16_t port = ReadAdvertisedPort();
            if (port == 0) return;
            std::vector<std::string> paths = DiscoverManifestPaths(appId);
            std::string body = BuildBody(appId, paths);
            LOG_MISC_INFO("CloudSync: /push AppId={} port={} manifest_paths={}",
                appId, port, paths.size());
            DoPost(port, L"/cloudsync/push", body, 5000);
        }

        // Background-thread helper. Watches HKCU\...\Apps\<watchAppId>\Running
        // until it transitions to 0, then POSTs /push for cloudAppId.
        //   watchAppId = what Steam tracks locally (480 for -onlinefix, real
        //                AppID otherwise)
        //   cloudAppId = the real AppID — what ONENNABE syncs under
        static void WatchExitAndPush(AppId_t watchAppId, AppId_t cloudAppId) {
            std::wstring keyPath = L"Software\\Valve\\Steam\\Apps\\"
                + std::to_wstring(watchAppId);
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0,
                KEY_READ | KEY_NOTIFY, &hKey) != ERROR_SUCCESS) {
                LOG_MISC_WARN("CloudSync: cannot open Apps\\{} for exit watch", watchAppId);
                return;
            }

            // Wait briefly for Steam to set Running=1.  If we never see it,
            // the game probably failed to launch — bail out without pushing.
            DWORD running = 0, sz = sizeof(running);
            for (int i = 0; i < 30 && running != 1; ++i) {
                sz = sizeof(running);
                RegQueryValueExW(hKey, L"Running", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(&running), &sz);
                if (running != 1) Sleep(200);
            }
            if (running != 1) {
                LOG_MISC_WARN("CloudSync: AppId {} never reached Running=1, aborting", watchAppId);
                RegCloseKey(hKey);
                return;
            }

            // Watch for 1→0. Poll every 10s as a safety net in case
            // RegNotifyChangeKeyValue misses a write while being re-armed.
            HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            for (int loops = 0; loops < 8640; ++loops) {  // 8640*10s = 24h max
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

    static void OnSpawnProcessHit(PCONTEXT ctx, const VehCommon::Int3Site& /*site*/) {
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);

        // Watch key is what Steam tracks LOCALLY — 480 for onlinefix games
        // (because we rewrite pGameID below), real AppID for plain depots.
        AppId_t watchAppId = appId;

        if (LuaConfig::HasDepot(appId) && cmdLine && strstr(cmdLine, "-onlinefix")) {
            g_OnlineFixRealAppId = appId;
            pGameID->SetAppID(kOnlineFixAppId);
            watchAppId = kOnlineFixAppId;
        }
        else {
            g_OnlineFixRealAppId = 0;
        }

        // ── Cloud sync hook ───────────────────────────────────────────────
        // Pull saves from Drive synchronously BEFORE the game gets a chance
        // to read them, then schedule the post-exit push on a detached
        // thread. Both ops no-op cleanly if ONENNABE isn't running.
        if (LuaConfig::HasDepot(appId)) {
            CloudSync::TriggerPull(appId);
            std::thread(CloudSync::WatchExitAndPush, watchAppId, appId).detach();
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

    // ══════════════════════════════════════════════════════════════════
    // CloudSync manifest discovery implementations
    // Placed AFTER HOOK_FUNC(GetAppDataFromAppInfo) so oGetAppDataFromAppInfo
    // is declared by the time these functions try to call it.
    // ══════════════════════════════════════════════════════════════════
    namespace CloudSync {

        std::string ResolveRootToken(const std::string& token) {
            static const struct { const char* name; const KNOWNFOLDERID* fid; } kTokens[] = {
                { "WinMyDocuments",      &FOLDERID_Documents       },
                { "WinAppDataLocal",     &FOLDERID_LocalAppData    },
                { "WinAppDataLocalLow",  &FOLDERID_LocalAppDataLow },
                { "WinAppDataRoaming",   &FOLDERID_RoamingAppData  },
                { "WinSavedGames",       &FOLDERID_SavedGames      },
                { "LinuxXdgDataHome",    &FOLDERID_RoamingAppData  },
                { "MacAppSupport",       &FOLDERID_RoamingAppData  },
            };
            for (const auto& t : kTokens) {
                if (_stricmp(token.c_str(), t.name) == 0) {
                    PWSTR pwsz = nullptr;
                    if (SHGetKnownFolderPath(*t.fid, 0, nullptr, &pwsz) == S_OK && pwsz) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1,
                            nullptr, 0, nullptr, nullptr);
                        std::string out(len > 0 ? len - 1 : 0, '\0');
                        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, pwsz, -1,
                            out.data(), len, nullptr, nullptr);
                        CoTaskMemFree(pwsz);
                        return out;
                    }
                }
            }
            if (_stricmp(token.c_str(), "gameinstall") == 0 ||
                _stricmp(token.c_str(), "App Install Directory") == 0) {
                return "{GAME_INSTALL_DIR}";
            }
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
            if (steamPath[0] == '\0') return "";

            std::vector<std::string> libs;
            libs.emplace_back(steamPath);

            std::string libVdf = std::string(steamPath) + "/steamapps/libraryfolders.vdf";
            std::ifstream f(libVdf);
            std::string line;
            while (std::getline(f, line)) {
                auto p = line.find("\"path\"");
                if (p == std::string::npos) continue;
                auto q1 = line.find('"', p + 6);
                if (q1 == std::string::npos) continue;
                auto q2 = line.find('"', q1 + 1);
                auto q3 = line.find('"', q2 + 1);
                auto q4 = line.find('"', q3 + 1);
                if (q4 == std::string::npos) continue;
                libs.emplace_back(line.substr(q3 + 1, q4 - q3 - 1));
            }

            for (auto& lib : libs) {
                std::string acf = lib + "/steamapps/appmanifest_" + std::to_string(appId) + ".acf";
                std::ifstream af(acf);
                if (!af) continue;
                std::string l;
                while (std::getline(af, l)) {
                    auto p = l.find("\"installdir\"");
                    if (p == std::string::npos) continue;
                    auto q1 = l.find('"', p + 12);
                    auto q2 = l.find('"', q1 + 1);
                    auto q3 = l.find('"', q2 + 1);
                    auto q4 = l.find('"', q3 + 1);
                    if (q4 == std::string::npos) continue;
                    std::string installdir = l.substr(q3 + 1, q4 - q3 - 1);
                    return lib + "/steamapps/common/" + installdir;
                }
            }
            return "";
        }

        std::string ReadAppInfoString(AppId_t appId, const char* key) {
            if (!oGetAppDataFromAppInfo || !g_pCAppInfoCache) return "";
            char buf[1024] = {};
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, key,
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len <= 1) return "";
            return std::string(buf, static_cast<size_t>(len - 1));
        }

        std::vector<std::string> DiscoverManifestPaths(AppId_t appId) {
            std::vector<std::string> out;
            for (int i = 0; i < 64; ++i) {
                std::string base = "ufs/savefiles/" + std::to_string(i);
                std::string root = ReadAppInfoString(appId, (base + "/root").c_str());
                if (root.empty()) break;

                std::string sub = ReadAppInfoString(appId, (base + "/path").c_str());
                std::string platf = ReadAppInfoString(appId, (base + "/platforms/0").c_str());

                if (!platf.empty()) {
                    bool wantThis = false;
                    for (int p = 0; p < 4; ++p) {
                        std::string pf = ReadAppInfoString(appId,
                            (base + "/platforms/" + std::to_string(p)).c_str());
                        if (pf.empty()) break;
                        if (_stricmp(pf.c_str(), "windows") == 0 ||
                            _stricmp(pf.c_str(), "all") == 0) {
                            wantThis = true; break;
                        }
                    }
                    if (!wantThis) continue;
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
                LOG_MISC_INFO("CloudSync: manifest[{}] AppId={} root={} path={} -> {}",
                    i, appId, root, sub, full);
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
                    for (char c : paths[i]) {
                        if (c == '\\' || c == '"') body += '\\';
                        body += c;
                    }
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