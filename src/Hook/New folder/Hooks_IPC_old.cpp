#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Hash.h"
#include "Hooks_Misc.h"

namespace {

    RESOLVE_FUNC(GetPipeClient, CSteamPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CSteamPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    // ════════════════════════════════════════════════════════════════
    //  Handler registry
    // ════════════════════════════════════════════════════════════════
    using namespace Hooks_IPC;

    std::vector<IpcHandlerEntry> g_Handlers;

    static const IpcHandlerEntry* FindHandler(EIPCInterface iface, uint32 funcHash) {
        for (auto& e : g_Handlers) {
            if (e.interfaceID == iface && e.funcHash == funcHash) return &e;
        }
        return nullptr;
    }

    // ════════════════════════════════════════════════════════════════
    //  Main hook
    // ════════════════════════════════════════════════════════════════
    HOOK_FUNC(IPCProcessMessage, bool,
        void* pServer, HSteamPipe hSteamPipe,
        CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        CSteamPipeClient* pipe = GetPipe(pServer, hSteamPipe);

        // ── Parse header, find handler ──────────────────────────
        const IpcHandlerEntry* entry = nullptr;

        if (pRead->TellPut() >= IPC_HEADER_SIZE) {
            const uint8* data = pRead->Base();
            const auto cmd = static_cast<EIPCCommand>(data[OFFSET_CMD]);

            if (cmd == EIPCCommand::Handshake) {
                LOG_IPC_INFO("[Handshake]: {}", pipe->DebugString());
            }
            else if (cmd == EIPCCommand::InterfaceCall) {

                const auto iface = static_cast<EIPCInterface>(data[OFFSET_INTERFACE_ID]);
                const uint32 funcHash = *reinterpret_cast<const uint32*>(data + OFFSET_FUNC_HASH);
                const char* ifaceName = EIPCInterfaceName(iface);

                // ════════════════════════════════════════════════════════════════
                //  THE PROVEN CLOUDBYPASS LOGIC
                //  NOTE: must run BEFORE the steam-pipe exclusion below, otherwise
                //  cloud status queries arriving on a low-numbered steam pipe get
                //  excluded before we can suppress them.
                // ════════════════════════════════════════════════════════════════
                if (ifaceName && std::string_view(ifaceName).find("RemoteStorage") != std::string_view::npos) {
                    if (pRead->TellPut() >= OFFSET_ARGS + 4) {
                        AppId_t targetAppId = *reinterpret_cast<const AppId_t*>(data + OFFSET_ARGS);

                        if (LuaConfig::HasDepot(targetAppId)) {
                            // ── Never intercept write/read operations ───────────
                            // File operations carry filename + data and are much larger.
                            // Status queries are exactly 14 bytes (AppId in args).
                            int32_t reqSize = pRead->TellPut();
                            if (reqSize > 18) {
                                LOG_IPC_TRACE("CloudBypass: skipping file op AppId={} reqSize={}", targetAppId, reqSize);
                            }
                            else {
                                // Status/enable query only — safe to suppress
                                bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                                if (result && pWrite->TellPut() >= 2) {
                                    uint8* resBase = pWrite->Base();
                                    if (resBase[0] == RESPONSE_PREFIX) {
                                        if (pWrite->TellPut() == 2) {
                                            resBase[1] = 0;
                                        }
                                        else if (pWrite->TellPut() == 5) {
                                            *reinterpret_cast<uint32_t*>(resBase + 1) = 1; // 1 = k_EResultOK
                                        }
                                        LOG_IPC_INFO("CloudBypass: suppressed AppId={} reqSize={} respSize={}", targetAppId, reqSize, pWrite->TellPut());
                                    }
                                }
                                return result; // Return immediately to feed the sanitized response to the UI
                            }
                        }
                    }
                }
                // ════════════════════════════════════════════════════════════════

                // exclude InterfaceCall from steam
                if ((pipe->m_hSteamPipe & 0xFFFF) <= 2) {
                    LOG_IPC_TRACE("[InterfaceCall] from steam, pipe=0x{:08X} skip handler", pipe->m_hSteamPipe);
                    return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                }

                entry = FindHandler(iface, funcHash);
                if (entry) {
                    LOG_IPC_DEBUG("[InterfaceCall] {} {} realAppId={},AppId={}",
                        entry->name, pipe->DebugString(),
                        Hooks_Misc::ResolveAppId(),
                        Hooks_Misc::GetAppIDForCurrentPipeWrap()
                    );
                }
                else {
                    LOG_IPC_TRACE("[InterfaceCall(unhandled)]{}::0x{:08X} {} realAppId={},AppId={}",
                        ifaceName ? ifaceName : "Unknown", funcHash,
                        pipe->DebugString(),
                        Hooks_Misc::ResolveAppId(),
                        Hooks_Misc::GetAppIDForCurrentPipeWrap()
                    );
                }
            }
            else {
                LOG_IPC_TRACE("[{}] {}", EIPCCommandName(cmd), pipe->DebugString());
            }
        }

        // ── Run original ────────────────────────────────────────
        const bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
        if (!result || !entry) return result;

        // Only run handlers for apps with configured depots.
        AppId_t appId = Hooks_Misc::ResolveAppId();
        if (!LuaConfig::HasDepot(appId)) {
            LOG_IPC_TRACE("{}: appId={} has no configured depot, skip handler {}",
                entry->name, appId, pipe->DebugString());
            return result;
        }

        entry->handler(pipe, pRead, pWrite);
        return result;
    }

} // namespace


namespace Hooks_IPC {

    void RegisterHandlers(const IpcHandlerEntry* entries, size_t count) {
        g_Handlers.insert(g_Handlers.end(), entries, entries + count);
    }

    void Install() {
        RESOLVE_C(GetPipeClient);

        // Interface modules register their handlers here.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        HOOK_BEGIN();
        INSTALL_HOOK_C(IPCProcessMessage);
        HOOK_END();

        LOG_INFO("IPC: ProcessMessage hooked (CloudBypass Active)");
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(IPCProcessMessage);
        UNHOOK_END();
    }

}