// version.dll proxy (x64). UnityPlayer.dll imports version.dll, so placing this DLL next to the game exe loads the
// mod at startup. Every export forwards to the system version.dll. On x64 all of these functions take only
// integer/pointer arguments, so a generic 8-argument forwarder is ABI-correct for each of them.
#include <windows.h>
#include <cstdint>

static HMODULE g_real = nullptr;

static FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\version.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

typedef uintptr_t (WINAPI* Fwd8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

#define PROXY(name)                                                                                                 \
    extern "C" uintptr_t WINAPI Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,      \
                                             uintptr_t f, uintptr_t g, uintptr_t h) {                             \
        static Fwd8 p = (Fwd8)Real(#name);                                                                          \
        if (!p) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }                                                  \
        return p(a, b, c, d, e, f, g, h);                                                                           \
    }

PROXY(GetFileVersionInfoA)
PROXY(GetFileVersionInfoByHandle)
PROXY(GetFileVersionInfoExA)
PROXY(GetFileVersionInfoExW)
PROXY(GetFileVersionInfoSizeA)
PROXY(GetFileVersionInfoSizeExA)
PROXY(GetFileVersionInfoSizeExW)
PROXY(GetFileVersionInfoSizeW)
PROXY(GetFileVersionInfoW)
PROXY(VerFindFileA)
PROXY(VerFindFileW)
PROXY(VerInstallFileA)
PROXY(VerInstallFileW)
PROXY(VerLanguageNameA)
PROXY(VerLanguageNameW)
PROXY(VerQueryValueA)
PROXY(VerQueryValueW)
