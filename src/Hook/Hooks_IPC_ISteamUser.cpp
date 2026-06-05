#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "PendingAPICalls.h"
#include "Utils/AppTicket.h"
#include "Utils/Log.h"
#include "Hooks_Misc.h"

#include <filesystem>
#include <string>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {
    using namespace IPCMessages::IClientUser;

    // ── DYNAMIC DENUVO AUTO-SPOOFER ────────────────────────────────
    // HIT-ONLY cache: we only store successful resolutions. A failed
    // scan (0) is NEVER cached, so GetSteamID keeps retrying every call
    // exactly like the original code — important because GetSteamID
    // fires early and may run before userdata/appId is fully ready.
    std::mutex                          g_OwnerCacheMutex;
    std::unordered_map<AppId_t, uint64> g_OwnerSteamIdCache;

    // ── Persisted owner cache (registry) ──────────────────────────
    // Once we resolve a strong owner SteamID for an AppID we write it
    // here, so future calls AND future sessions skip the userdata scan
    // entirely. Per-AppID QWORD under:
    //   HKCU\Software\OpenSteamTool\SpoofOwners\<appId>
    constexpr const char* kSpoofOwnersKey = "Software\\OpenSteamTool\\SpoofOwners";

    static uint64_t GetPersistedOwnerSteamID(AppId_t activeAppId) {
        uint64_t steamId64 = 0;
        DWORD size = sizeof(steamId64);
        if (RegGetValueA(HKEY_CURRENT_USER, kSpoofOwnersKey,
            std::to_string(activeAppId).c_str(),
            RRF_RT_REG_QWORD, nullptr, &steamId64, &size) != ERROR_SUCCESS) {
            return 0;
        }
        return steamId64;
    }

    static void SetPersistedOwnerSteamID(AppId_t activeAppId, uint64_t steamId64) {
        LSTATUS rc = RegSetKeyValueA(HKEY_CURRENT_USER, kSpoofOwnersKey,
            std::to_string(activeAppId).c_str(),
            REG_QWORD, &steamId64, sizeof(steamId64));
        if (rc != ERROR_SUCCESS) {
            LOG_IPC_WARN("DENUVO AUTO-SPOOF: failed to persist owner for App {} (rc={})",
                activeAppId, rc);
        }
    }

    // Read the currently-logged-in 32-bit AccountID. This account is
    // RUNNING the spoof — by definition it does NOT own the game — so
    // a userdata/<appId>/ folder under it is just a failed-activation
    // leftover, not proof of ownership. Exclude it from the owner hunt.
    static uint32_t GetActiveAccountId() {
        DWORD activeUser = 0;
        DWORD size = sizeof(activeUser);
        if (RegGetValueA(HKEY_CURRENT_USER,
            "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser",
            RRF_RT_REG_DWORD, nullptr, &activeUser, &size) != ERROR_SUCCESS) {
            return 0;
        }
        return static_cast<uint32_t>(activeUser);
    }

    static uint64_t GetDynamicOwnerSteamID(AppId_t activeAppId) {
        if (activeAppId == 0) return 0;

        // Session cache (HIT-only).
        {
            std::lock_guard<std::mutex> lock(g_OwnerCacheMutex);
            auto it = g_OwnerSteamIdCache.find(activeAppId);
            if (it != g_OwnerSteamIdCache.end()) {
                return it->second;
            }
        }

        // Persistent registry cache — survives restarts.
        if (uint64_t persisted = GetPersistedOwnerSteamID(activeAppId)) {
            LOG_IPC_DEBUG("DENUVO AUTO-SPOOF: App {} owner from registry -> 0x{:X}",
                activeAppId, persisted);
            std::lock_guard<std::mutex> lock(g_OwnerCacheMutex);
            g_OwnerSteamIdCache[activeAppId] = persisted;
            return persisted;
        }

        // Steam install path.
        char steamPath[MAX_PATH];
        DWORD pathSize = MAX_PATH;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
            RRF_RT_REG_SZ, nullptr, steamPath, &pathSize) != ERROR_SUCCESS) {
            return 0;
        }
        std::string pathStr(steamPath);
        for (char& c : pathStr) { if (c == '/') c = '\\'; }

        fs::path userdataDir = fs::path(pathStr) / "userdata";
        std::error_code ec;
        if (!fs::exists(userdataDir, ec)) return 0;

        const uint32_t activeAccount = GetActiveAccountId();
        uint64_t fallbackActiveOwner = 0;   // weak fallback if no other acct hits

        for (const auto& entry : fs::directory_iterator(userdataDir, ec)) {
            if (!entry.is_directory()) continue;

            std::string accountIdStr = entry.path().filename().string();
            uint32_t accountId = 0;
            try { accountId = std::stoul(accountIdStr); }
            catch (...) { continue; }

            fs::path gameConfigDir = entry.path() / std::to_string(activeAppId);
            if (!fs::exists(gameConfigDir, ec)) continue;

            uint64_t steamId64 = 0x0110000100000000ULL + accountId;

            if (activeAccount != 0 && accountId == activeAccount) {
                fallbackActiveOwner = steamId64;
                LOG_IPC_DEBUG("DENUVO AUTO-SPOOF: skipping active account {} for App {} "
                    "(failed-activation leftover, not proof of ownership)",
                    accountId, activeAppId);
                continue;
            }

            LOG_IPC_INFO("DENUVO AUTO-SPOOF: Found Owner for App {} -> SteamID {}",
                         activeAppId, steamId64);

            {
                std::lock_guard<std::mutex> lock(g_OwnerCacheMutex);
                g_OwnerSteamIdCache[activeAppId] = steamId64;
            }
            SetPersistedOwnerSteamID(activeAppId, steamId64);
            return steamId64;
        }

        if (fallbackActiveOwner) {
            LOG_IPC_WARN("DENUVO AUTO-SPOOF: only the active account has App {} on disk; "
                "falling back to SteamID {} (uncached, will retry)",
                activeAppId, fallbackActiveOwner);
            return fallbackActiveOwner;
        }

        return 0;
    }

    // [Post-Handler]: IClientUser::GetSteamID
    void HandlerPost_IClientUser_GetSteamID(CPipeClient* pipe,CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        AppId_t appId = Hooks_Misc::ResolveAppId();

        // 1. Manual Lua config first (original behavior).
        uint64 spoofed = AppTicket::GetSpoofSteamID(appId);

        // 2. Fallback: hunt the disk for the true owner (Denuvo auto-spoofer).
        if (!spoofed) {
            spoofed = GetDynamicOwnerSteamID(appId);
        }

        if (!spoofed) {
            LOG_IPC_WARN("IClientUser::GetSteamID: AppId={} no valid steamid - cannot spoof", appId);
            return;
        }

        GetSteamIDResp resp{pWrite};
        if (!resp.ok()) return;
        LOG_IPC_DEBUG("IClientUser::GetSteamID: AppId={} Original: {} -> Spoofed: 0x{:X}({})", 
                        appId,resp.DebugString(),spoofed, spoofed);
        resp.set_returnValue(spoofed);
    }

    // [Post-Handler]: IClientUser::GetAppOwnershipTicketExtendedData
    void HandlerPost_IClientUser_GetAppOwnershipTicketExtendedData(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        GetAppOwnershipTicketExtendedDataReq req{pRead};
        if (!req.ok()) return;

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData:{}", req.DebugString());
        if (req.cbMaxTicket() < 0) return;

        AppTicket::AppOwnershipTicket ticket{};
        AppId_t appId = req.unAppID() == kOnlineFixAppId ? Hooks_Misc::ResolveAppId() : req.unAppID();
        
        if (!AppTicket::GetAppOwnershipTicket(appId, ticket)) return;
        if (ticket.data.size() > static_cast<size_t>(req.cbMaxTicket())) {
            LOG_IPC_WARN("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} ticket too large ({} bytes) for buffer ({} bytes)",
                         appId, ticket.data.size(), req.cbMaxTicket());
            return;
        }

        GetAppOwnershipTicketExtendedDataResp resp{pWrite, static_cast<size_t>(req.cbMaxTicket())};
        if (!resp.ok()) return;

        resp.set_returnValue(ticket.totalSize);
        if (!resp.set_pTicket(ticket.data)) return;
        resp.set_piAppId(ticket.appIdOffset);
        resp.set_piSteamId(ticket.steamIdOffset);
        resp.set_piSignature(ticket.signatureOffset);
        resp.set_pcbSignature(ticket.signatureSize);

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} {}", 
                        appId,resp.DebugString());
    }

    // [Post-Handler]: IClientUser::RequestEncryptedAppTicket
    // Reads the hAsyncCall steamclient already wrote into the response,
    // so we know which AppId to mint an eticket for in GetAPICallResult.
    void HandlerPost_IClientUser_RequestEncryptedAppTicket(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        RequestEncryptedAppTicketResp resp{pWrite};
        if (!resp.ok()) return;

        AppId_t appId = Hooks_Misc::ResolveAppId();
        std::vector<uint8_t> ticket = AppTicket::GetEncryptedTicketFromRegistry(appId);
        if (ticket.empty()) {
            LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }

        const SteamAPICall_t hAsyncCall = resp.returnValue();
        PendingAPICalls::RecordEncryptedTicket(hAsyncCall, appId);
        LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} hAsyncCall=0x{:X} - recorded",
                      appId, hAsyncCall);
    }

    // [Post-Handler]: IClientUser::GetEncryptedAppTicket
    void HandlerPost_IClientUser_GetEncryptedAppTicket(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        AppId_t appId = Hooks_Misc::ResolveAppId();
        std::vector<uint8_t> ticket = AppTicket::GetEncryptedTicketFromRegistry(appId);
        if (ticket.empty()) {
            LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }

        uint32 ticketSize = static_cast<uint32>(ticket.size());
        uint32 newCapacity = pWrite->Capacity() + ticketSize;
        if (!Hooks_Misc::EnsureBufferCapacity(pWrite, newCapacity,true)) {
            LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - failed to ensure buffer size", appId);
            return;
        }

        GetEncryptedAppTicketResp resp{pWrite};
        if (!resp.ok()) return;

        resp.set_returnValue(true);
        resp.set_pcbTicket(ticketSize);
        if (!resp.set_pTicket(ticket)) return;

        LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} {}", appId, resp.DebugString());
    }

} // namespace

namespace Hooks_IPC_ISteamUser {
    void Register() {
        IPCHandlerEntry UserEntries[] = {
            ADD_IPC_POST_HANDLER(IClientUser, GetSteamID),
            ADD_IPC_POST_HANDLER(IClientUser, GetAppOwnershipTicketExtendedData),
            ADD_IPC_POST_HANDLER(IClientUser, RequestEncryptedAppTicket),
            ADD_IPC_POST_HANDLER(IClientUser, GetEncryptedAppTicket),
        };
        Hooks_IPC::RegisterHandlers(UserEntries);
    }
}
