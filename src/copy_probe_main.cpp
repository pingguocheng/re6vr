// copy_probe_main.cpp - records which D3D9 calls the compositor's per-frame copy
// may use on this machine, with no game and no headset involved.
//
// Why it exists: the frame copy is the one part of the proxy the harness cannot
// check when no headset is connected, because OpenXR refuses to hand out a
// session (XR_ERROR_FORM_FACTOR_UNAVAILABLE) and composition then never runs.
// The build that failed passed NULL as StretchRect's destination - which D3D9
// rejects with D3DERR_INVALIDCALL on every frame - so this host reproduces the
// exact sequence against the system d3d9.dll and prints a table of HRESULTs plus
// a luma reading of what actually landed in the SYSTEMMEM surface.
//
// Build: scripts\build_copyprobe.bat  -> build\_copytest\copyprobe.exe
// Run:   from build\_copytest, so that any d3d9.dll in the directory is not the
//        one under test (the probe uses LOAD_LIBRARY_SEARCH_SYSTEM32 anyway).
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <tlhelp32.h>

#include "d3d9_min.h"
#include "seh_guard.h"

namespace {


// A fault inside d3d9.dll takes the whole process with it, and a plain print
// never reaches the log because stdout is block-buffered once it is piped. This
// appends straight to the log file instead, so the last thing recorded survives.
void append_line(const char *text) {
    HANDLE f = CreateFileA("copy_probe.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, (DWORD)lstrlenA(text), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

LONG WINAPI veh(EXCEPTION_POINTERS *info) {
    char buf[512];
    const EXCEPTION_RECORD *er = info->ExceptionRecord;
    HMODULE mod = nullptr;
    char modname[MAX_PATH] = "(unknown)";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &mod)) {
        GetModuleFileNameA(mod, modname, MAX_PATH);
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "VEH: code=0x%08lX addr=%p module=%s",
                (unsigned long)er->ExceptionCode, er->ExceptionAddress, modname);
    append_line(buf);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "VEH: %s address=%p",
                    er->ExceptionInformation[0] == 1 ? "write to" : "read from",
                    (void *)er->ExceptionInformation[1]);
        append_line(buf);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // let the process die as it normally would
}

void say(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    append_line(buf);   // one writer, unbuffered, so nothing is ever reordered or lost
}

// One D3D9 call plus its result. Kept in a plain struct so every call can run
// inside seh::guard, which needs its body in a function of its own.
struct Job {
    IDirect3DDevice9 *dev = nullptr;
    void *src = nullptr;
    void *dst = nullptr;
    DWORD filter = D3DTEXF_NONE;
    UINT back_buffer = 0;
    const char *tag = "";
    void *out = nullptr;   // surface obtained, or the bits a LockRect returned
    long pitch = 0;
    D3DSURFACE_DESC desc = {};
    HRESULT hr = E_FAIL;
};

void run_get_rt(void *ctx) {
    Job *j = static_cast<Job *>(ctx);
    j->hr = j->dev->lpVtbl->GetRenderTarget(j->dev, 0, &j->out);
}

void run_get_bb(void *ctx) {
    Job *j = static_cast<Job *>(ctx);
    j->hr = j->dev->lpVtbl->GetBackBuffer(j->dev, 0, j->back_buffer, D3DBACKBUFFER_TYPE_MONO, &j->out);
}

void run_stretch(void *ctx) {
    Job *j = static_cast<Job *>(ctx);
    j->hr = j->dev->lpVtbl->StretchRect(j->dev, j->src, nullptr, j->dst, nullptr, j->filter);
}

void run_readback(void *ctx) {
    Job *j = static_cast<Job *>(ctx);
    j->hr = j->dev->lpVtbl->GetRenderTargetData(j->dev, j->src, j->dst);
}

void run_getdesc(void *ctx) {
    Job *j = static_cast<Job *>(ctx);
    IDirect3DSurface9 *surf = static_cast<IDirect3DSurface9 *>(j->src);
    j->hr = surf->lpVtbl->GetDesc(surf, &j->desc);
}

// The lock result lives at a fixed address on purpose: if a call ever returns
// with the stack pointer off by a word, a local's address is wrong and the values
// read back would look like garbage. A global cannot be misread that way, so what
// it holds is what d3d9 actually wrote.
D3DLOCKED_RECT g_locked = {};

void run_lock(void *ctx) {
    Job *j = static_cast<Job *>(ctx);
    IDirect3DSurface9 *surf = static_cast<IDirect3DSurface9 *>(j->src);
    g_locked.Pitch = 0;
    g_locked.pBits = nullptr;
    char msg[256];
    // Written straight to the file: if LockRect takes the process down, these lines
    // are what says whether it happened on the way in or on the way out.
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[%s] calling LockRect(surf=%p flags=0x%lX) result@%p",
                j->tag, (void *)surf, (unsigned long)j->filter, (void *)&g_locked);
    append_line(msg);

    j->hr = surf->lpVtbl->LockRect(surf, &g_locked, nullptr, j->filter);

    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[%s] LockRect returned hr=0x%08lX bits=%p pitch=%ld",
                j->tag, (unsigned long)j->hr, g_locked.pBits, g_locked.Pitch);
    append_line(msg);
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[%s] raw %08lX %08lX (%u bytes)",
                j->tag, ((const unsigned long *)&g_locked)[0], ((const unsigned long *)&g_locked)[1],
                (unsigned)sizeof(g_locked));
    append_line(msg);
    if (g_locked.pBits && (uintptr_t)g_locked.pBits > 0x10000) {
        const unsigned long *peek = (const unsigned long *)g_locked.pBits;
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[%s] first pixels: %08lX %08lX %08lX %08lX",
                    j->tag, peek[0], peek[1], peek[2], peek[3]);
        append_line(msg);
    }
    j->out = g_locked.pBits;
    j->pitch = g_locked.Pitch;
}

// Calls one vtable slot of a surface with (this, buffer32) and dumps what it
// wrote. Kept for re-measuring the slot map if a future runtime shifts it again;
// only useful on a surface whose methods are already known to be callable.
struct ScanJob {
    IDirect3DSurface9 *surf = nullptr;
    int slot = 0;
    unsigned long out[8] = {};
    HRESULT hr = E_FAIL;
};

bool run(const char *label, seh::VoidFn fn, Job *j) {
    const bool completed = seh::guard(fn, j);
    if (!completed) {
        say("%-52s RAISED a structured exception", label);
        return false;
    }
    say("%-52s hr=0x%08lX%s", label, (unsigned long)j->hr, SUCCEEDED(j->hr) ? "  OK" : "");
    return SUCCEEDED(j->hr);
}

// Samples what actually arrived in a locked surface. A frame of pure black reads
// as luma 0, which is exactly the "headset shows nothing" case.
void scan(const char *label, const void *bits, long pitch, UINT w, UINT h) {
    const uint8_t *base = static_cast<const uint8_t *>(bits);
    uint64_t luma = 0;
    uint32_t lit = 0, n = 0;
    for (UINT y = 0; y < h; y += 16) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(base + (size_t)y * pitch);
        for (UINT x = 0; x < w; x += 16) {
            const uint32_t px = row[x];
            luma += (px & 0xFFu) + ((px >> 8) & 0xFFu) + ((px >> 16) & 0xFFu);
            if ((px & 0x00FFFFFFu) != 0) ++lit;
            ++n;
        }
    }
    say("%-52s %u/%u sampled pixels non-black, mean luma %llu",
        label, lit, n, (unsigned long long)(n ? luma / ((uint64_t)n * 3) : 0));
}

// Prints the object pointer and the first vtable slots. A jump to an address
// like 0x16 means a slot held garbage, so seeing the table beats guessing.
void dump_vtbl(const char *label, void *obj) {
    say("%-24s obj=%p", label, obj);
    if (!obj) return;
    void **vt = *reinterpret_cast<void ***>(obj);
    say("%-24s vtbl=%p", label, (void *)vt);
    if (!vt) return;
    for (int i = 0; i < 20; i += 5) {
        char line[320] = "";
        for (int k = 0; k < 5; ++k) {
            char one[64];
            _snprintf_s(one, sizeof(one), _TRUNCATE, "[%02d]=%p ", i + k, vt[i + k]);
            strcat_s(line, one);
        }
        say("    %s", line);
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int main(int argc, char **argv) {
    const UINT w = (argc > 1) ? (UINT)atoi(argv[1]) : 3840;
    const UINT h = (argc > 2) ? (UINT)atoi(argv[2]) : 2160;

    DeleteFileA("copy_probe.txt");
    setvbuf(stdout, nullptr, _IONBF, 0);   // never lose the last line to a crash
    AddVectoredExceptionHandler(1, &veh);
    say("copy probe: back buffer %ux%u fmt=22 (X8R8G8B8)", w, h);

    HMODULE real = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!real) {
        say("cannot load the system d3d9.dll (err %lu)", GetLastError());
        return 1;
    }
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(real, path, MAX_PATH);
    say("system d3d9.dll: %ls", path);

    auto create9 = (PFN_Direct3DCreate9)GetProcAddress(real, "Direct3DCreate9");
    if (!create9) {
        say("system d3d9.dll has no Direct3DCreate9");
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"re6vr_copy_probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"re6vr copy probe", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        say("CreateWindowExW failed: %lu", GetLastError());
        return 1;
    }

    IDirect3D9 *d3d9 = create9(D3D_SDK_VERSION);
    if (!d3d9) {
        say("Direct3DCreate9 returned NULL");
        return 1;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d3d9->lpVtbl->CreateDevice(d3d9, 0, D3DDEVTYPE_HAL, hwnd,
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                            &pp, &dev);
    say("CreateDevice(%ux%u)                                   hr=0x%08lX", w, h, (unsigned long)hr);
    if (FAILED(hr) || !dev) return 1;

    // The state the proxy copies from: a finished frame, scene closed.
    dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0xFF203040, 1.0f, 0);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->EndScene(dev);

    Job rt;
    rt.dev = dev;
    if (!run("T0  GetRenderTarget(0)", &run_get_rt, &rt)) return 1;
    IDirect3DSurface9 *color_target = static_cast<IDirect3DSurface9 *>(rt.out);

    Job bb;
    bb.dev = dev;
    run("T0b GetBackBuffer(0,0,MONO)", &run_get_bb, &bb);
    IDirect3DSurface9 *back_buffer = static_cast<IDirect3DSurface9 *>(bb.out);

    // The staging pair the compositor uses.
    IDirect3DTexture9 *staging = nullptr;
    IDirect3DSurface9 *staging_surface = nullptr;
    hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                    D3DPOOL_DEFAULT, reinterpret_cast<void **>(&staging), nullptr);
    if (SUCCEEDED(hr) && staging) staging->lpVtbl->GetSurfaceLevel(staging, 0, &staging_surface);
    IDirect3DSurface9 *sysmem = nullptr;
    HRESULT hr_sysmem = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8,
                                                                 D3DPOOL_SYSTEMMEM,
                                                                 reinterpret_cast<void **>(&sysmem), nullptr);
    say("staging texture + %ux%u SYSTEMMEM surface               hr=0x%08lX / 0x%08lX",
        w, h, (unsigned long)hr, (unsigned long)hr_sysmem);
    if (!staging_surface || !sysmem) {
        say("staging surfaces unavailable (rt=%p sysmem=%p)", (void *)staging_surface, (void *)sysmem);
        return 1;
    }

    dump_vtbl("color target", color_target);
    dump_vtbl("staging surface", staging_surface);
    dump_vtbl("sysmem surface", sysmem);

    // The surface slot map was settled here with a slot scan: GetDesc answered at
    // slot 12 (Format=22, Type=1, Pool=2, 1280x720) and slot 15 handed back an
    // HDC, which is what moved GetDesc/LockRect one slot up in d3d9_min.h. The
    // scan itself is gone - it called unknown slots with the wrong arity and took
    // the process down - and everything below is the corrected map.

    // T0e: the same lock on a tiny surface, to separate "this surface is not what
    // it claims to be" from "locking is broken at this size".
    IDirect3DSurface9 *tiny = nullptr;
    hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                  reinterpret_cast<void **>(&tiny), nullptr);
    say("T0e CreateOffscreenPlainSurface(16x16) hr=0x%08lX obj=%p", (unsigned long)hr, (void *)tiny);
    if (tiny) {
        Job lt;
        lt.dev = dev;
        lt.src = tiny;
        lt.tag = "T0e";
        lt.filter = D3DLOCK_READONLY;
        if (run("T0e LockRect(16x16 sysmem)", &run_lock, &lt)) {
            say("T0e bits=%p pitch=%ld (a 16x16 X8R8G8B8 surface should read pitch=64)",
                lt.out, lt.pitch);
            if (lt.out) tiny->lpVtbl->UnlockRect(tiny);
        }
        tiny->lpVtbl->Release(tiny);
    }

    // T0f: ask the surface what it is. GetDesc is the other method that writes
    // into a caller-supplied struct, and the proxy's notes record it faulting
    // inside d3d9.dll, so this is also a check for a hooking or wrapper layer.
    Job d0;
    d0.dev = dev;
    d0.src = sysmem;
    d0.tag = "T0f";
    if (run("T0f GetDesc(sysmem)", &run_getdesc, &d0)) {
        unsigned long *raw = reinterpret_cast<unsigned long *>(&d0.desc);
        say("T0f desc: fmt=%lu type=%lu usage=0x%lX pool=%lu ms=%lu/%lu %ux%u",
            (unsigned long)d0.desc.Format, (unsigned long)d0.desc.Type, (unsigned long)d0.desc.Usage,
            (unsigned long)d0.desc.Pool, (unsigned long)d0.desc.MultiSampleType,
            (unsigned long)d0.desc.MultiSampleQuality, d0.desc.Width, d0.desc.Height);
        say("T0f raw: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", raw[0], raw[1], raw[2],
            raw[3], raw[4], raw[5], raw[6], raw[7]);
    }

    // Which image of d3d9 is doing the work, and who else is in this process.
    HMODULE d3d9_mod = GetModuleHandleA("d3d9.dll");
    HMODULE on12 = GetModuleHandleA("d3d9on12.dll");
    say("modules: d3d9=%p d3d9on12=%p", (void *)d3d9_mod, (void *)on12);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                say("   module %ls", me.szModule);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    // T0c: lock the SYSTEMMEM surface while it is still empty. Deliberately after
    // the checks above, because this call is the one that takes the process down.
    Job l0;
    l0.dev = dev;
    l0.src = sysmem;
    l0.tag = "T0c";
    l0.filter = D3DLOCK_READONLY;
    if (run("T0c LockRect(sysmem, READONLY) before any copy", &run_lock, &l0)) {
        if (l0.out) {
            say("T0c bits=%p pitch=%ld (surface is %u MB)", l0.out, l0.pitch,
                (unsigned)((uint64_t)w * h * 4 / (1024 * 1024)));
            sysmem->lpVtbl->UnlockRect(sysmem);
        } else {
            say("T0c LockRect returned S_OK but pBits=NULL on an untouched surface");
        }
    }

    // T0d: the same lock with no flags at all, in case READONLY is the problem.
    Job l0b;
    l0b.dev = dev;
    l0b.src = sysmem;
    l0b.tag = "T0d";
    l0b.filter = 0;
    if (run("T0d LockRect(sysmem, flags=0)", &run_lock, &l0b)) {
        say("T0d bits=%p pitch=%ld", l0b.out, l0b.pitch);
        if (l0b.out) sysmem->lpVtbl->UnlockRect(sysmem);
    }

    // T1-T3: the path the fixed proxy takes.
    //
    // The frame is produced the way the game produces one: clear, one scene, then
    // a Present. The Present matters - with D3DSWAPEFFECT_DISCARD the runtime is
    // free to hand out an unbacked back buffer until the first one, and a readback
    // taken before that comes back as zeros no matter how correct the copy is.
    HRESULT clear_hr = dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0xFF203040, 1.0f, 0);
    say("T1a Clear(0xFF203040)                              hr=0x%08lX", (unsigned long)clear_hr);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->EndScene(dev);
    HRESULT present_hr = dev->lpVtbl->Present(dev, nullptr, nullptr, nullptr, nullptr);
    say("T1a Present (back buffer primed)                  hr=0x%08lX", (unsigned long)present_hr);
    clear_hr = dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0xFF203040, 1.0f, 0);
    say("T1a Clear(0xFF203040) again                        hr=0x%08lX", (unsigned long)clear_hr);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->EndScene(dev);

    Job s1;
    s1.dev = dev;
    s1.src = color_target;
    s1.dst = staging_surface;
    s1.filter = D3DTEXF_NONE;
    if (run("T1  StretchRect(render target -> staging, NONE)", &run_stretch, &s1)) {
        Job r1;
        r1.dev = dev;
        r1.src = staging_surface;
        r1.dst = sysmem;
        if (run("T2  GetRenderTargetData(staging -> sysmem)", &run_readback, &r1)) {
            Job l1;
            l1.dev = dev;
            l1.src = sysmem;
            if (run("T3  LockRect(sysmem)", &run_lock, &l1) && l1.out) {
                scan("T3b contents of the readback (clear was 0x203040)", l1.out, l1.pitch, w, h);
                sysmem->lpVtbl->UnlockRect(sysmem);
            } else {
                say("T3b no bits: pBits=%p (expected non-NULL)", l1.out);
            }
        }
    }

    // T3c: the same readback, but from a surface filled on the GPU directly. This
    // separates "the readback carries no data" from "the back buffer was empty".
    HRESULT fill_hr = dev->lpVtbl->ColorFill(dev, staging_surface, nullptr, 0xFF00FF00);
    say("T3c ColorFill(staging, 0xFF00FF00)                 hr=0x%08lX", (unsigned long)fill_hr);
    if (SUCCEEDED(fill_hr)) {
        Job r1c;
        r1c.dev = dev;
        r1c.src = staging_surface;
        r1c.dst = sysmem;
        if (run("T3c GetRenderTargetData(staging -> sysmem)", &run_readback, &r1c)) {
            Job l1c;
            l1c.dev = dev;
            l1c.src = sysmem;
            if (run("T3c LockRect(sysmem)", &run_lock, &l1c) && l1c.out) {
                scan("T3d contents after ColorFill (expect luma 85)", l1c.out, l1c.pitch, w, h);
                sysmem->lpVtbl->UnlockRect(sysmem);
            }
        }
    }

    // T3e: the whole chain, with the render target re-fetched after the Present -
    // which is what the proxy does every frame. The surfaces captured before the
    // first Present belong to a swap chain buffer that has since been rotated
    // away, so reading those is the one thing that legitimately comes back black.
    dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0xFF203040, 1.0f, 0);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->EndScene(dev);
    Job rt2;
    rt2.dev = dev;
    if (run("T3e GetRenderTarget(0) after Present", &run_get_rt, &rt2) && rt2.out) {
        Job s6;
        s6.dev = dev;
        s6.src = rt2.out;
        s6.dst = staging_surface;
        s6.filter = D3DTEXF_NONE;
        if (run("T3e StretchRect(fresh RT -> staging)", &run_stretch, &s6)) {
            Job r6;
            r6.dev = dev;
            r6.src = staging_surface;
            r6.dst = sysmem;
            if (run("T3e GetRenderTargetData(staging -> sysmem)", &run_readback, &r6)) {
                Job l6;
                l6.dev = dev;
                l6.src = sysmem;
                if (run("T3e LockRect(sysmem)", &run_lock, &l6) && l6.out) {
                    scan("T3f contents of the whole chain (expect luma 48)", l6.out, l6.pitch, w, h);
                    sysmem->lpVtbl->UnlockRect(sysmem);
                }
            }
        }
        // T3g: does the blit copy anything at all? The staging target is filled
        // with green first: if it comes back black, the blit ran and the render
        // target itself is black; if it stays green, the blit did nothing.
        HRESULT fill2 = dev->lpVtbl->ColorFill(dev, staging_surface, nullptr, 0xFF00FF00);
        say("T3g ColorFill(staging, green) before the blit        hr=0x%08lX", (unsigned long)fill2);
        Job s7;
        s7.dev = dev;
        s7.src = rt2.out;
        s7.dst = staging_surface;
        s7.filter = D3DTEXF_NONE;
        if (run("T3g StretchRect(fresh RT -> staging)", &run_stretch, &s7)) {
            Job r7;
            r7.dev = dev;
            r7.src = staging_surface;
            r7.dst = sysmem;
            if (run("T3g GetRenderTargetData(staging -> sysmem)", &run_readback, &r7)) {
                Job l7;
                l7.dev = dev;
                l7.src = sysmem;
                if (run("T3g LockRect(sysmem)", &run_lock, &l7) && l7.out) {
                    scan("T3h green=blit did nothing, black=RT is black", l7.out, l7.pitch, w, h);
                    sysmem->lpVtbl->UnlockRect(sysmem);
                }
            }
        }

        static_cast<IDirect3DSurface9 *>(rt2.out)->lpVtbl->Release(
            static_cast<IDirect3DSurface9 *>(rt2.out));
    }

    // T4: same blit from the explicit back buffer (what submit() would use).
    Job s2;
    s2.dev = dev;
    s2.src = back_buffer ? back_buffer : color_target;
    s2.dst = staging_surface;
    run("T4  StretchRect(back buffer -> staging, NONE)", &run_stretch, &s2);

    // T5: the scaling blit the earlier build relied on. (The identifier is not
    // called "small": <windows.h> still defines that as a macro for char.)
    IDirect3DTexture9 *scaled = nullptr;
    IDirect3DSurface9 *scaled_surface = nullptr;
    hr = dev->lpVtbl->CreateTexture(dev, 1280, 720, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                    D3DPOOL_DEFAULT, reinterpret_cast<void **>(&scaled), nullptr);
    if (SUCCEEDED(hr) && scaled) scaled->lpVtbl->GetSurfaceLevel(scaled, 0, &scaled_surface);
    if (scaled_surface) {
        Job s3;
        s3.dev = dev;
        s3.src = color_target;
        s3.dst = scaled_surface;
        run("T5  StretchRect(4K -> 720p, NONE)  [scaling]", &run_stretch, &s3);
    }

    // T6: the exact call the broken build made - NULL destination.
    Job s4;
    s4.dev = dev;
    s4.src = color_target;
    s4.dst = nullptr;
    run("T6  StretchRect(src -> NULL, NONE)  [the old bug]", &run_stretch, &s4);

    // T7: the fallback that build used instead - reading the back buffer itself.
    Job r2;
    r2.dev = dev;
    r2.src = back_buffer ? back_buffer : color_target;
    r2.dst = sysmem;
    if (run("T7  GetRenderTargetData(back buffer -> sysmem)", &run_readback, &r2)) {
        Job l2;
        l2.dev = dev;
        l2.src = sysmem;
        if (run("T7b LockRect after reading the back buffer", &run_lock, &l2)) {
            if (l2.out) {
                scan("T7c contents after the back-buffer read", l2.out, l2.pitch, w, h);
                sysmem->lpVtbl->UnlockRect(sysmem);
            } else {
                say("T7c LockRect returned S_OK but pBits=NULL - the old dead end");
            }
        }
    }

    // T8: format conversion through StretchRect (X8R8G8B8 -> A8R8G8B8).
    IDirect3DTexture9 *a8 = nullptr;
    IDirect3DSurface9 *a8_surface = nullptr;
    hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, reinterpret_cast<void **>(&a8), nullptr);
    if (SUCCEEDED(hr) && a8) a8->lpVtbl->GetSurfaceLevel(a8, 0, &a8_surface);
    if (a8_surface) {
        Job s5;
        s5.dev = dev;
        s5.src = color_target;
        s5.dst = a8_surface;
        run("T8  StretchRect(X8R8G8B8 -> A8R8G8B8)  [convert]", &run_stretch, &s5);
        a8_surface->lpVtbl->Release(a8_surface);
    }
    if (a8) a8->Release();
    if (scaled_surface) scaled_surface->lpVtbl->Release(scaled_surface);
    if (scaled) scaled->Release();
    if (sysmem) sysmem->lpVtbl->Release(sysmem);
    if (staging_surface) staging_surface->lpVtbl->Release(staging_surface);
    if (staging) staging->Release();
    if (color_target) color_target->lpVtbl->Release(color_target);
    if (back_buffer) back_buffer->lpVtbl->Release(back_buffer);
    dev->lpVtbl->Release(dev);
    d3d9->lpVtbl->Release(d3d9);

    say("done");
    return 0;
}
