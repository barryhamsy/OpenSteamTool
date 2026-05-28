// dwrite.dll HiJack Project - Dynamic Wrapper
#include <windows.h>
#include <cstring>
#include <string>

// ─── 1. Real DWrite Function Pointers ───────────────────────────
typedef HRESULT(WINAPI* DWriteCreateFactory_t)(DWORD, REFIID, IUnknown**);

static HMODULE g_hRealDWrite = nullptr;
static DWriteCreateFactory_t o_DWriteCreateFactory = nullptr;

// ─── 2. Core Initialization ─────────────────────────────────────
void LoadRealDWrite() {
    if (g_hRealDWrite) return;

    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string realPath = std::string(sysDir) + "\\dwrite.dll";

    g_hRealDWrite = LoadLibraryA(realPath.c_str());
    if (g_hRealDWrite) {
        o_DWriteCreateFactory = (DWriteCreateFactory_t)GetProcAddress(g_hRealDWrite, "DWriteCreateFactory");
    }
}

// ─── 3. Native Exports ──────────────────────────────────────────
extern "C" {
    HRESULT WINAPI DWriteCreateFactory(DWORD factoryType, REFIID iid, IUnknown** factory) {
        if (!g_hRealDWrite) LoadRealDWrite();
        return o_DWriteCreateFactory ? o_DWriteCreateFactory(factoryType, iid, factory) : E_FAIL;
    }
}

// ─── 4. OpenSteamTool Injection ─────────────────────────────────
BOOL OpenSteamToolLoad() {
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        const char* exeName = strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        if (_stricmp(exeName, "steam.exe") != 0) return TRUE;
    }

    char steamPath[MAX_PATH] = { 0 };
    DWORD bufferSize = sizeof(steamPath);
    LSTATUS status = RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steamPath, &bufferSize);

    if (status == ERROR_SUCCESS) {
        std::string toolPath = std::string(steamPath) + "\\OpenSteamTool.dll";
        for (char& c : toolPath) { if (c == '/') c = '\\'; }
        return LoadLibraryA(toolPath.c_str()) != NULL;
    }

    return LoadLibraryA("OpenSteamTool.dll") != NULL;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved) {
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LoadRealDWrite();
        if (!OpenSteamToolLoad()) return FALSE;
        break;
    }
    return TRUE;
}