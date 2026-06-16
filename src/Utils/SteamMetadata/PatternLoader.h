#pragma once

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>

namespace PatternLoader {

    // Load metadata before installing hooks for a module.
    bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component);

    // Resolve by RVA first, then fall back to signature scanning.
    void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // Report unresolved functions after all hooks have been installed.
    void ReportMissingFunctions();

    // True if AT LEAST ONE Load() call this session fetched its TOML from
    // the remote source rather than the local cache. False means every
    // Load() fell back to cache — i.e. the machine had no usable network.
    //
    // Hooks_Misc uses this as the offline signal: "online enough to fetch
    // our patterns" ≈ "online enough for PICS to respond."
    bool WasRemoteReachable();

} // namespace PatternLoader
