#include "dllmain.h"
#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Pipe/PipeManager.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/SteamMetadata/IPCLoader.h"

namespace {

    RESOLVE_FUNC(GetPipeClient, CPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    //  Handler dispatch table
    struct ResolvedHandler {
        EIPCInterface         interfaceID;
        uint32                funcHash;
        std::string           name;       // "IClientUser::GetSteamID" — for logs
        uint32                fencepost;
        uint32                argc;
        IPCHandlerFn          pre;
        IPCHandlerFn          post;

        ResolvedHandler(const IPCHandlerEntry& entry, const IPCLoader::Method& method)
            : interfaceID(method.interfaceID),
              funcHash(method.funcHash),
              name(std::string(entry.interfaceName) + "::" + entry.methodName),
              fencepost(method.fencepost),
              argc(method.argc),
              pre(entry.pre),
              post(entry.post) {}

        std::string DebugString() const {
            return std::format("{} -> hash=0x{:08X} fencepost=0x{:08X} argc={}",
                name, funcHash, fencepost, argc);
        }
    };
    std::vector<ResolvedHandler> g_Handlers;

    static ResolvedHandler* FindHandler(EIPCInterface iface, uint32 funcHash) {
        for (auto& e : g_Handlers) {
            if (e.interfaceID == iface && e.funcHash == funcHash) return &e;
        }
        return nullptr;
    }

    struct IPCDispatch {
        CPipeClient*     pipe = nullptr;
        ResolvedHandler* handler = nullptr;

        bool enabled() const {
            return pipe && handler;
        }

        std::string DebugString() const {
            return std::format("{} {}",pipe ? pipe->DebugString() : "null",
                                handler ? handler->DebugString() : "null");
        }
    };

    static IPCDispatch ResolveDispatch(void* pServer,HSteamPipe hSteamPipe,CUtlBuffer* pRead)
    {
        IPCDispatch dispatch{};
        dispatch.pipe = GetPipe(pServer, hSteamPipe);
        if (!dispatch.pipe) return dispatch;

        // We only care about InterfaceCall messages
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok()) return dispatch;
        if (request.command() != EIPCCommand::InterfaceCall) return dispatch;

        // Ignore calls when appId is not resolved or not in Lua config
        if (!LuaConfig::HasDepot(Hooks_Misc::ResolveAppId())) return dispatch;

        // Parse out the interface call header to find the handler
        IPCMessages::IPCInterfaceCall call{request.body()};
        if (!call.ok()) return dispatch;

        // Lookup handler by interface ID + method hash
        dispatch.handler = FindHandler(call.interfaceID(), call.funcHash());
        if (!dispatch.handler) return dispatch;

        LOG_IPC_TRACE("Resolved IPC handler: {}", dispatch.DebugString());
        return dispatch;
    }

    static void HandleHandshake(void* pServer, HSteamPipe hSteamPipe,CUtlBuffer* pRead)
    {
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok()|| request.command() != EIPCCommand::Handshake) return;
        IPCMessages::IPCHandshakeReq handshake{request.body()};
        if(!handshake.ok()) return;

        CPipeClient* pipe = GetPipe(pServer, hSteamPipe);
        if (!pipe) return;
        // set client PID 
        pipe->m_clientPID = handshake.pid();

        LOG_IPC_DEBUG("Received handshake from {},{}", pipe->DebugString(), handshake.DebugString());
        PipeManager::OnHandshake(pipe);
    }

    HOOK_FUNC(IPCProcessMessage, bool,void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        // Handle handshake messages (PipeManager bookkeeping)
        HandleHandshake(pServer, hSteamPipe, pRead);

        bool handled = false;
        bool result  = false;

        // Determine who is talking to Steam: The Game (> 0) or The UI (0)
        AppId_t pipeAppId = Hooks_Misc::GetAppIDForCurrentPipeWrap();

        IPCMessages::IPCRequest request{ pRead };
        if (request.ok() && request.command() == EIPCCommand::InterfaceCall) {
            IPCMessages::IPCInterfaceCall call{ request.body() };

            // ════════════════════════════════════════════════════════════════
            // 1. THE GAME PIPE: Capcom Save Fix
            //
            // Two response shapes need different sanitization:
            //   - 2-byte  (prefix 0x0B + 1 bool):  IsCloudEnabledForApp etc.
            //     Write 0 so the game sees cloud as DISABLED and falls back
            //     to local saves.  Without this, Capcom games start fresh
            //     every launch ("save wiped" symptom).
            //   - 5-byte  (prefix 0x0B + 1 EResult): SyncFiles etc.
            //     Write 1 (k_EResultOK) so the game thinks sync succeeded.
            //
            // File ops (>18 bytes) pass through untouched so FileWrite /
            // FileRead calls still reach Steam's local UFS layer.
            // ════════════════════════════════════════════════════════════════
            if (pipeAppId > 0 && call.ok() && call.interfaceID() == EIPCInterface::IClientRemoteStorage) {
                if (pRead->TellPut() >= 14) {
                    AppId_t targetAppId = *reinterpret_cast<const AppId_t*>(pRead->Base() + 10);
                    if (LuaConfig::HasDepot(targetAppId)) {
                        const int32_t reqSize = pRead->TellPut();
                        if (reqSize <= 18) {
                            result  = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                            handled = true;
                            if (result && pWrite->TellPut() >= 2) {
                                uint8* resBase = pWrite->Base();
                                if (resBase[0] == 0x0B) {
                                    if (pWrite->TellPut() == 2)
                                        resBase[1] = 0;                                   // bool → false
                                    else if (pWrite->TellPut() == 5)
                                        *reinterpret_cast<uint32_t*>(resBase + 1) = 1;    // EResult → OK
                                }
                            }
                        }
                    }
                }
            }

            // ════════════════════════════════════════════════════════════════
            // 2. THE UI PIPE: Safe Visual Scrubber
            //
            // Strip the Cloud flags from AppState fields in IClientAppManager
            // responses so the library view never renders cloud-sync badges
            // for spoofed games (which would jump / spin / show errors).
            // ════════════════════════════════════════════════════════════════
            else if (pipeAppId == 0 && call.ok() && call.interfaceID() == EIPCInterface::IClientAppManager) {
                result  = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                handled = true;

                if (result && pWrite && pWrite->TellPut() >= 9) {
                    uint8* resBase = pWrite->Base();
                    if (resBase[0] == 0x0B) {
                        uint8* data   = resBase + 1;
                        int    dataLen = pWrite->TellPut() - 1;

                        for (int i = 0; i <= dataLen - 8; i++) {
                            AppId_t id = *reinterpret_cast<AppId_t*>(data + i);
                            // Steam AppIDs are 1 000 – ~3 000 000; HasDepot filters to ours
                            if (id >= 480 && id < 0x02000000 && LuaConfig::HasDepot(id)) {
                                const uint32_t k_CloudMask = 0x001C0000;
                                for (int offset = 4; offset <= 24; offset += 4) {
                                    if (i + offset + 4 <= dataLen) {
                                        uint32_t* state = reinterpret_cast<uint32_t*>(data + i + offset);
                                        if ((*state & k_CloudMask) != 0 && *state < 0x02000000)
                                            *state &= ~k_CloudMask;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Early return if one of the custom handlers above processed the message
        if (handled) return result;

        // ── Generic dispatch for registered IPC handlers ──────────────────
        IPCDispatch dispatch = ResolveDispatch(pServer, hSteamPipe, pRead);
        if (!dispatch.enabled()) return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        if (dispatch.handler->pre)
            dispatch.handler->pre(dispatch.pipe, pRead, pWrite);

        result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        if (result && dispatch.handler->post)
            dispatch.handler->post(dispatch.pipe, pRead, pWrite);

        return result;
    }

} // namespace


namespace Hooks_IPC {

    void Install() {
        RESOLVE_C(GetPipeClient);

        // Each module registers a static array. Hash lookup against the
        // IPCLoader metadata happens inside RegisterHandlers.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        LOG_IPC_INFO("Hooks_IPC: {} handlers registered", g_Handlers.size());

        HOOK_BEGIN();
        INSTALL_HOOK_C(IPCProcessMessage);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(IPCProcessMessage);
        UNHOOK_END();
    }

    void RegisterHandlers(std::span<const IPCHandlerEntry> entries) {
        for (const auto& e : entries) {
            const auto* m = IPCLoader::Find(e.interfaceName, e.methodName);
            if (!m) {
                LOG_IPC_WARN("[Handler Disabled] no IPC spec for {}",e.DebugString());
                continue;
            }
            auto& handler = g_Handlers.emplace_back(e,*m);
            LOG_IPC_DEBUG("Hooks_IPC: resolved {}", handler.DebugString());
        }
    }

}
