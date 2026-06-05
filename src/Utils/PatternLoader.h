#pragma once
#include <windows.h>
#include <string>

namespace PatternLoader {

    // Load metadata before installing hooks for a module.
    bool Load(HMODULE module, const std::string& dllPath, const std::string& component);

    // Resolve by RVA first, then fall back to signature scanning.
    void* FindPattern(HMODULE module, const char* funcName);

    // Report unresolved functions after all hooks have been installed.
    void ReportMissingFunctions();

    // True if AT LEAST ONE Load() call this session fetched its TOML from
    // the remote source (GitHub/jsDelivr/custom mirror) rather than the
    // local cache. False means every Load() fell back to cache — i.e. the
    // machine had no usable network at DLL init.
    //
    // Other hooks (e.g. Hooks_Misc::GetOrAddAppData) use this as the offline
    // signal: "online enough to fetch our patterns" ≈ "online enough for
    // PICS to respond." Snapshotted at init, not re-evaluated mid-session.
    bool WasRemoteReachable();

} // namespace PatternLoader
