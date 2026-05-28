// dinput8.dll HiJack Project - Pinned Single Export Proxy
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <detours.h>
#include <string>

// ─── 1. Real DirectInput Function Pointers ────────────────────────
static HMODULE g_hRealDInput = nullptr;

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t o_DirectInput8Create = nullptr;

// ─── 2. Core Initialization (Binding to the real System32 file) ───
void LoadRealDInput() {
    if (g_hRealDInput) return;

    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string realPath = std::string(sysDir) + "\\dinput8.dll";

    g_hRealDInput = LoadLibraryA(realPath.c_str());
    if (g_hRealDInput) {
        o_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(g_hRealDInput, "DirectInput8Create");
    }
}

// ─── 3. Native Exports (Safely passing data to the game) ──────────
extern "C" {
    HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
        if (!g_hRealDInput) LoadRealDInput();

        if (o_DirectInput8Create) {
            return o_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
        }
        return E_FAIL;
    }
}

// ─── 4. Detours & Memory Pinning Logic ────────────────────────────
static HMODULE g_OldModule = nullptr;

typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryExW_t oLoadLibraryExW = nullptr;

HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName) {
        if (g_OldModule && _wcsicmp(lpLibFileName, L"dinput8.dll") == 0) {
            return g_OldModule; // Force Steam to use our pinned module
        }
    }
    return oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

static void InstallHook() {
    oLoadLibraryExW = reinterpret_cast<LoadLibraryExW_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryExW"));
    if (!oLoadLibraryExW) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&oLoadLibraryExW), reinterpret_cast<PVOID>(hkLoadLibraryExW));
    DetourTransactionCommit();
}

static void UninstallHook() {
    if (!oLoadLibraryExW) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&oLoadLibraryExW), reinterpret_cast<PVOID>(hkLoadLibraryExW));
    DetourTransactionCommit();
    oLoadLibraryExW = nullptr;
}

// ─── 5. OpenSteamTool Injection ───────────────────────────────────
BOOL OpenSteamToolLoad() {
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        const char* exeName = strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        if (_stricmp(exeName, "steam.exe") != 0) return TRUE;
    }

    // Modern tool path resolution via Windows Registry
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
    {
        DisableThreadLibraryCalls(hModule);
        g_OldModule = hModule;

        // PIN THE DLL: This prevents Steam from dynamically unloading us via FreeLibrary
        HMODULE pinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&hkLoadLibraryExW),
            &pinned);

        LoadRealDInput();
        InstallHook();

        if (!OpenSteamToolLoad()) return FALSE;
        break;
    }
    case DLL_PROCESS_DETACH:
        UninstallHook();
        if (g_hRealDInput) {
            FreeLibrary(g_hRealDInput);
        }
        break;
    }
    return TRUE;
}