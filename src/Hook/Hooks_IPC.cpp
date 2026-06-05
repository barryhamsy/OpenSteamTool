#include "dllmain.h"
#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Utils/Hash.h"
#include "Utils/IPCLoader.h"
#include <algorithm>

namespace {

    RESOLVE_FUNC(GetPipeClient, CPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    struct ResolvedHandler {
        EIPCInterface         interfaceID;
        uint32                funcHash;
        std::string           name;
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
            post(entry.post) {
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
        CPipeClient* pipe = nullptr;
        ResolvedHandler* handler = nullptr;
        bool enabled() const { return pipe && handler; }
    };

    static IPCDispatch ResolveDispatch(void* pServer, HSteamPipe hSteamPipe, CUtlBuffer* pRead)
    {
        IPCDispatch dispatch{};
        dispatch.pipe = GetPipe(pServer, hSteamPipe);
        if (!dispatch.pipe) return dispatch;

        IPCMessages::IPCRequest request{ pRead };
        if (!request.ok() || request.command() != EIPCCommand::InterfaceCall) return dispatch;

        IPCMessages::IPCInterfaceCall call{ request.body() };
        if (!call.ok()) return dispatch;

        dispatch.handler = FindHandler(call.interfaceID(), call.funcHash());
        return dispatch;
    }

    HOOK_FUNC(IPCProcessMessage, bool, void* pServer, HSteamPipe hSteamPipe,
        CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        bool handled = false;
        bool result = false;

        // Determine who is talking to Steam: The Game (> 0) or The UI (0)
        AppId_t pipeAppId = Hooks_Misc::GetAppIDForCurrentPipeWrap();

        IPCMessages::IPCRequest request{ pRead };
        if (request.ok() && request.command() == EIPCCommand::InterfaceCall) {
            IPCMessages::IPCInterfaceCall call{ request.body() };

            // ════════════════════════════════════════════════════════════════
            // 1. THE GAME PIPE: Capcom Save Fix
            //
            // Two response shapes need different sanitization:
            //   - 2-byte response (prefix 0x0B + 1 bool):  IsCloudEnabledForApp,
            //     IsCloudEnabledThisApp, etc.  Write 0 so the game sees cloud
            //     as DISABLED and falls back to local saves cleanly.  If we
            //     leave this untouched the game sees Steam's account-level
            //     value (usually true), then asks cloud for save data, finds
            //     nothing for the spoofed AppID, and decides to start fresh —
            //     classic Capcom "save wiped every launch" symptom.
            //   - 5-byte response (prefix 0x0B + 1 EResult): SyncFiles,
            //     BeginFileWriteBatch, etc.  Write 1 (k_EResultOK) so the
            //     game thinks the sync succeeded and proceeds without panic.
            //
            // File ops (>18 bytes carry filename + payload) pass through
            // untouched so the game's own FileWrite / FileRead calls still
            // hit Steam's local UFS layer for actual save persistence.
            // ════════════════════════════════════════════════════════════════
            if (pipeAppId > 0 && call.ok() && call.interfaceID() == EIPCInterface::IClientRemoteStorage) {
                if (pRead->TellPut() >= 14) {
                    AppId_t targetAppId = *reinterpret_cast<const AppId_t*>(pRead->Base() + 10);

                    if (LuaConfig::HasDepot(targetAppId)) {
                        const int32_t reqSize = pRead->TellPut();

                        // Let file ops (>18 bytes) pass so Capcom games can write saves!
                        if (reqSize <= 18) {
                            result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                            handled = true;

                            if (result && pWrite->TellPut() >= 2) {
                                uint8* resBase = pWrite->Base();
                                if (resBase[0] == 0x0B) {
                                    if (pWrite->TellPut() == 2) {
                                        resBase[1] = 0;                                    // bool → false (cloud disabled)
                                    }
                                    else if (pWrite->TellPut() == 5) {
                                        *reinterpret_cast<uint32_t*>(resBase + 1) = 1;     // EResult → k_EResultOK
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ════════════════════════════════════════════════════════════════
            // 2. THE UI PIPE: Safe Visual Scrubber (Zero Jumping Errors!)
            // ════════════════════════════════════════════════════════════════
            else if (pipeAppId == 0 && call.ok() && call.interfaceID() == EIPCInterface::IClientAppManager) {
                result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                handled = true;

                if (result && pWrite && pWrite->TellPut() >= 9) {
                    uint8* resBase = pWrite->Base();
                    if (resBase[0] == 0x0B) { // Valid IPC Response
                        uint8* data = resBase + 1;
                        int dataLen = pWrite->TellPut() - 1;

                        for (int i = 0; i <= dataLen - 8; i++) {
                            AppId_t id = *reinterpret_cast<AppId_t*>(data + i);

                            // STRICT TARGET LOCK: Stop checking HasDepot() against thousands of IDs!
                            // We ONLY scrub if the UI is drawing the exact Spacewar ID (480) or the currently spoofed game.
                            // This mathematically guarantees other games are untouched!
                            AppId_t activeRealId = Hooks_Misc::ResolveAppId();
                            if (id == kOnlineFixAppId || (activeRealId > 0 && id == activeRealId)) {

                                uint32_t* state = reinterpret_cast<uint32_t*>(data + i + 4);
                                const uint32_t k_CloudMask = 0x001C0000;

                                // Extreme Validation: Only wipe if it is unmistakably an AppState bitmask
                                if ((*state & k_CloudMask) != 0 && *state < 0x02000000) {
                                    *state &= ~k_CloudMask; // Safely erase the UI error
                                }
                            }
                        }
                    }
                }
            }
        }

        const IPCDispatch dispatch = ResolveDispatch(pServer, hSteamPipe, pRead);

        if (!handled) {
            if (dispatch.enabled() && dispatch.handler->pre) dispatch.handler->pre(dispatch.pipe, pRead, pWrite);
            result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
        }

        if (!handled && dispatch.enabled() && dispatch.handler->post) {
            dispatch.handler->post(dispatch.pipe, pRead, pWrite);
        }

        return result;
    }

} // namespace

namespace Hooks_IPC {
    void Install() {
        RESOLVE_C(GetPipeClient);
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

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
            if (!m) continue;
            g_Handlers.emplace_back(e, *m);
        }
    }
}