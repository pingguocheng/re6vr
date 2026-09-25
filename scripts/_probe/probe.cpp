// probe.cpp - build variants used to bisect the proxy DLL load crash.
//
// Each variant appends a line to a trace file at well-defined points, so the
// last line present tells us how far the loader / DllMain got. Build variants
// are selected with /DPROBE_LEVEL=n (see build_probes.bat).
#include <windows.h>

#include <cstdio>

static void trace(const char *text) {
    wchar_t dir[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sprobe_trace.txt", dir);

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, (DWORD)strlen(text), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

// 1: pull in the d3d11 import
#if PROBE_LEVEL >= 1
#include <d3d11.h>
#endif

// 2: pull in MinHook (static lib, linked by the build script)
#if PROBE_LEVEL >= 2
#include "MinHook.h"
#endif

// 3: pull in the HLSL compiler
#if PROBE_LEVEL >= 3
#include <d3dcompiler.h>
#endif

class CppStaticInit {
public:
    CppStaticInit() { trace("cpp static initialiser ran"); }
};
static CppStaticInit g_cpp_static;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        trace("DllMain: process attach");
        DisableThreadLibraryCalls(instance);
#if PROBE_LEVEL >= 1
        {
            ID3D11Device *dev = nullptr;
            D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &dev, &got, nullptr);
            trace(SUCCEEDED(hr) ? "D3D11CreateDevice ok" : "D3D11CreateDevice failed");
            if (dev) dev->Release();
        }
#endif
#if PROBE_LEVEL >= 2
        {
            MH_STATUS st = MH_Initialize();
            trace(st == MH_OK ? "MH_Initialize ok" : "MH_Initialize failed");
        }
#endif
#if PROBE_LEVEL >= 3
        {
            ID3DBlob *blob = nullptr;
            ID3DBlob *err = nullptr;
            const char *src = "float4 main() : SV_TARGET { return 1; }";
            HRESULT hr = D3DCompile(src, strlen(src), "p", nullptr, nullptr, "main", "ps_4_0", 0, 0,
                                    &blob, &err);
            trace(SUCCEEDED(hr) ? "D3DCompile ok" : "D3DCompile failed");
            if (blob) blob->Release();
            if (err) err->Release();
        }
#endif
#if PROBE_LEVEL >= 5
        {
            wchar_t sysdir[MAX_PATH] = L"";
            GetSystemDirectoryW(sysdir, MAX_PATH);
            wchar_t real_path[MAX_PATH];
            _snwprintf_s(real_path, MAX_PATH, _TRUNCATE, L"%s\\d3d9.dll", sysdir);
            HMODULE real = LoadLibraryExW(real_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            trace(real ? "LoadLibraryExW(system d3d9.dll) ok"
                       : "LoadLibraryExW(system d3d9.dll) FAILED");
            if (real) {
                FARPROC p = GetProcAddress(real, "Direct3DCreate9");
                trace(p ? "GetProcAddress(Direct3DCreate9) ok"
                        : "GetProcAddress(Direct3DCreate9) FAILED");
            }
        }
#endif
#if PROBE_LEVEL >= 6
        {
            // Reproduce the logger the proxy uses: fopen + fputs + OutputDebugString.
            FILE *f = nullptr;
            if (_wfopen_s(&f, L"probe_logger.txt", L"wb") == 0 && f) {
                fputs("logger wrote a line\n", f);
                fflush(f);
                fclose(f);
                OutputDebugStringW(L"logger wrote a line\n");
                trace("fopen-based logger ok");
            } else {
                trace("fopen-based logger FAILED");
            }
        }
#endif
        trace("DllMain: done");
    }
    return TRUE;
}
