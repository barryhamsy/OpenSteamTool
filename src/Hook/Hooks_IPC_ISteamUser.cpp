#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Utils/AppTicket.h"
#include "Utils/Log.h"
#include "Hooks_Misc.h"
#include <filesystem>
#include <string>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {
    // ── eticket: hAsyncCall → appId mapping ────────────────────────
    std::unordered_map<uint64, AppId_t> g_EticketAsyncCalls;

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

    // Returns the persisted owner SteamID for this AppID, or 0 if none.
    uint64_t GetPersistedOwnerSteamID(AppId_t activeAppId) {
        uint64_t steamId64 = 0;
        DWORD size = sizeof(steamId64);
        if (RegGetValueA(HKEY_CURRENT_USER, kSpoofOwnersKey,
            std::to_string(activeAppId).c_str(),
            RRF_RT_REG_QWORD, nullptr, &steamId64, &size) != ERROR_SUCCESS) {
            return 0;
        }
        return steamId64;
    }

    // Persist a resolved owner SteamID. Only call for STRONG hits.
    void SetPersistedOwnerSteamID(AppId_t activeAppId, uint64_t steamId64) {
        LSTATUS rc = RegSetKeyValueA(HKEY_CURRENT_USER, kSpoofOwnersKey,
            std::to_string(activeAppId).c_str(),
            REG_QWORD, &steamId64, sizeof(steamId64));
        if (rc != ERROR_SUCCESS) {
            LOG_IPC_WARN("DENUVO AUTO-SPOOF: failed to persist owner for App {} (rc={})",
                activeAppId, rc);
        }
    }

    // Read the currently-logged-in 32-bit AccountID from the registry.
    // This is the account RUNNING the game / attempting the spoof. By
    // definition it does NOT own the game (that's why we spoof), so it
    // must be EXCLUDED when hunting for the legitimate owner.
    uint32_t GetActiveAccountId() {
        DWORD activeUser = 0;
        DWORD size = sizeof(activeUser);
        if (RegGetValueA(HKEY_CURRENT_USER,
            "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser",
            RRF_RT_REG_DWORD, nullptr, &activeUser, &size) != ERROR_SUCCESS) {
            return 0;
        }
        return static_cast<uint32_t>(activeUser);
    }

    uint64_t GetDynamicOwnerSteamID(AppId_t activeAppId) {
        if (activeAppId == 0) return 0;

        // Return a cached HIT if we already resolved this AppID this session.
        {
            std::lock_guard<std::mutex> lock(g_OwnerCacheMutex);
            auto it = g_OwnerSteamIdCache.find(activeAppId);
            if (it != g_OwnerSteamIdCache.end()) {
                return it->second;
            }
        }

        // Next, check the persisted registry value (survives restarts).
        // A hit here lets us skip the userdata scan entirely.
        if (uint64_t persisted = GetPersistedOwnerSteamID(activeAppId)) {
            LOG_IPC_DEBUG("DENUVO AUTO-SPOOF: App {} owner from registry -> 0x{:X}",
                activeAppId, persisted);
            std::lock_guard<std::mutex> lock(g_OwnerCacheMutex);
            g_OwnerSteamIdCache[activeAppId] = persisted;
            return persisted;
        }

        // Ask the Registry where Steam is installed
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

        // The account currently logged in is the one trying to spoof; a
        // userdata/<appId>/ folder under it is just a FAILED-activation
        // leftover, NOT proof of ownership. Skip it and look for a
        // DIFFERENT account that has the folder — that's the real owner.
        const uint32_t activeAccount = GetActiveAccountId();

        uint64_t fallbackActiveOwner = 0; // active acct has folder, but no
        // other candidate — weak guess.

// Scan the userdata folder for the legitimate owner
        for (const auto& entry : fs::directory_iterator(userdataDir, ec)) {
            if (!entry.is_directory()) continue;

            std::string accountIdStr = entry.path().filename().string();
            uint32_t accountId = 0;
            try {
                accountId = std::stoul(accountIdStr);
            }
            catch (...) {
                continue; // Ignore non-numeric folders (e.g. "ac", "0")
            }

            fs::path gameConfigDir = entry.path() / std::to_string(activeAppId);
            if (!fs::exists(gameConfigDir, ec)) continue;

            uint64_t steamId64 = 0x0110000100000000ULL + accountId;

            // The active (spoofing) account having the folder proves
            // nothing — remember it only as a last resort.
            if (activeAccount != 0 && accountId == activeAccount) {
                fallbackActiveOwner = steamId64;
                LOG_IPC_DEBUG("DENUVO AUTO-SPOOF: skipping active account {} for App {} "
                    "(failed-activation leftover, not proof of ownership)",
                    accountId, activeAppId);
                continue;
            }

            // A DIFFERENT account owns the folder — strong owner signal.
            LOG_INFO("DENUVO AUTO-SPOOF: Found Owner for App {} -> SteamID {}", activeAppId, steamId64);

            // Cache strong hits in memory AND persist to registry so we
            // never need to scan this AppID again, even after a restart.
            {
                std::lock_guard<std::mutex> lock(g_OwnerCacheMutex);
                g_OwnerSteamIdCache[activeAppId] = steamId64;
            }
            SetPersistedOwnerSteamID(activeAppId, steamId64);
            return steamId64;
        }

        // No other account had the folder. Fall back to the active account
        // if it did (e.g. single-account machine that legitimately owns it).
        // This is a WEAK result — do NOT cache it, so we keep retrying in
        // case the real owner's userdata appears later.
        if (fallbackActiveOwner) {
            LOG_IPC_WARN("DENUVO AUTO-SPOOF: only the active account has App {} on disk; "
                "falling back to SteamID {} (uncached, will retry)",
                activeAppId, fallbackActiveOwner);
            return fallbackActiveOwner;
        }

        return 0; // NOT cached — retry next call.
    }

    // ── Handler: IClientUser::GetSteamID ──────────────────────────
    //  Request:  no args
    //  Response: [uint8 prefix=0x0B][uint64 SteamID]   (9 bytes)
    void Handler_IClientUser_GetSteamID(CSteamPipeClient* pipe,
        CUtlBuffer*, CUtlBuffer* pWrite)
    {
        AppId_t appId = Hooks_Misc::ResolveAppId();

        // 1. Try checking the manual Lua config first (original behavior)
        uint64 spoofed = AppTicket::GetSpoofSteamID(appId);

        // 2. If no config is set, dynamically hunt the disk for the true owner
        if (!spoofed) {
            spoofed = GetDynamicOwnerSteamID(appId);
        }

        if (!spoofed) {
            LOG_IPC_WARN("IClientUser::GetSteamID: AppId={} no valid steamid - cannot spoof", appId);
            return;
        }

        uint8* base = pWrite->Base();
        base[0] = RESPONSE_PREFIX;
        memcpy(base + 1, &spoofed, sizeof(spoofed));
        LOG_IPC_DEBUG("IClientUser::GetSteamID: AppId={} -> Spoofed: 0x{:X}({})", appId, spoofed, spoofed);
    }

    // ── Handler: IClientUser::GetAppOwnershipTicketExtendedData ───
    void Handler_IClientUser_GetAppOwnershipTicketExtendedData(
        CSteamPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        const uint8* reqData = pRead->Base();
        const int32  reqSize = pRead->m_Put;
        if (reqSize < OFFSET_ARGS + 8) return;
        const uint8* args = reqData + OFFSET_ARGS;
        const uint32 reqAppID = *reinterpret_cast<const uint32*>(args);
        const int32  reqBufSize = *reinterpret_cast<const int32*>(args + 4);

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: req AppID={} bufSize={}",
            reqAppID, reqBufSize);

        std::vector<uint8_t> ticket = AppTicket::GetAppOwnershipTicketFromRegistry(reqAppID);
        if (ticket.empty() || ticket.size() < 4) return;

        const uint32 ticketSize = static_cast<uint32>(ticket.size());
        const uint32 sigOffset = *reinterpret_cast<const uint32*>(ticket.data());

        const uint32 totalSize = 1 + 4 + reqBufSize + 16;
        if (static_cast<uint32>(pWrite->m_Put) < totalSize) return;

        uint8* base = pWrite->Base();

        base[0] = RESPONSE_PREFIX;
        memcpy(base + 1, &ticketSize, 4);
        const uint32 copySize = (ticketSize < static_cast<uint32>(reqBufSize))
            ? ticketSize : static_cast<uint32>(reqBufSize);
        memcpy(base + 5, ticket.data(), copySize);
        if (copySize < static_cast<uint32>(reqBufSize))
            memset(base + 5 + copySize, 0, reqBufSize - copySize);

        const uint32 piAppId = 16;
        const uint32 piSteamId = 8;
        const uint32 piSignature = sigOffset;
        const uint32 pcbSignature = 128;
        const uint32 outOff = 5 + reqBufSize;
        memcpy(base + outOff, &piAppId, 4);
        memcpy(base + outOff + 4, &piSteamId, 4);
        memcpy(base + outOff + 8, &piSignature, 4);
        memcpy(base + outOff + 12, &pcbSignature, 4);

        AppId_t appId = Hooks_Misc::ResolveAppId();
        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} -> {} bytes "
            "(sigOffset={})", appId, ticketSize, sigOffset);
    }

    // ── Handler: IClientUser::RequestEncryptedAppTicket ──────────
    void Handler_IClientUser_RequestEncryptedAppTicket(
        CSteamPipeClient* pipe, CUtlBuffer*, CUtlBuffer* pWrite)
    {
        if (pWrite->m_Put < 9) return;

        AppId_t appId = Hooks_Misc::ResolveAppId();
        auto ticket = AppTicket::GetEncryptedTicketFromRegistry(appId);
        if (ticket.empty()) {
            LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }

        uint8* base = pWrite->Base();
        uint64 hAsyncCall;
        memcpy(&hAsyncCall, base + 1, sizeof(hAsyncCall));

        g_EticketAsyncCalls[hAsyncCall] = appId;
        LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} hAsyncCall=0x{:016X} - recorded", appId, hAsyncCall);
    }

    // ── Handler: IClientUser::GetEncryptedAppTicket ───────────────
    void Handler_IClientUser_GetEncryptedAppTicket(
        CSteamPipeClient* pipe, CUtlBuffer*, CUtlBuffer* pWrite)
    {
        AppId_t appId = Hooks_Misc::ResolveAppId();
        auto ticket = AppTicket::GetEncryptedTicketFromRegistry(appId);
        if (ticket.empty()) {
            LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }

        const uint32 ticketSize = static_cast<uint32>(ticket.size());
        const int32 totalSize = 1 + 1 + 4 + ticketSize;
        // New Hooks_Misc::EnsureBufferSize returns bool and no longer sets
        // m_Put for us — check the result and set m_Put explicitly.
        if (!Hooks_Misc::EnsureBufferSize(pWrite, totalSize)) {
            LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - failed to ensure buffer size", appId);
            return;
        }
        pWrite->m_Put = totalSize;

        uint8* base = pWrite->Base();
        base[0] = RESPONSE_PREFIX;
        base[1] = 1;
        memcpy(base + 2, &ticketSize, sizeof(ticketSize));
        memcpy(base + 6, ticket.data(), ticketSize);

        LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} -> {} bytes", appId, ticketSize);
    }

    const Hooks_IPC::IpcHandlerEntry g_Entries[] = {
        ADD_IPC_HANDLER(IClientUser, GetSteamID),
        ADD_IPC_HANDLER(IClientUser, GetAppOwnershipTicketExtendedData),
        ADD_IPC_HANDLER(IClientUser, RequestEncryptedAppTicket),
        ADD_IPC_HANDLER(IClientUser, GetEncryptedAppTicket),
    };

} // namespace

namespace Hooks_IPC_ISteamUser {
    void Register() {
        Hooks_IPC::RegisterHandlers(g_Entries, std::size(g_Entries));
    }

    AppId_t LookupEticketAsyncCall(uint64 hAsyncCall) {
        auto it = g_EticketAsyncCalls.find(hAsyncCall);
        return it != g_EticketAsyncCalls.end() ? it->second : 0;
    }
    void EraseEticketAsyncCall(uint64 hAsyncCall) {
        g_EticketAsyncCalls.erase(hAsyncCall);
    }
}