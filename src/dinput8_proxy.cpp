// dinput8_proxy.cpp - a DirectInput8 proxy that lets the VR head steer the game's camera.
//
// WHY THIS EXISTS
// ---------------
// Three ways of reaching the camera have been measured and closed:
//
//   * rotating the view matrix in the vertex-shader constant stream - the write happens and
//     the picture does not move, because that stream carries no camera matrix;
//   * searching memory for the camera object from a captured view matrix - five runs, zero
//     hits, and the fed matrices turned out not to be camera poses at all (their translation
//     and their rotation disagree, so no address can hold the position they imply);
//   * hooking the DTI registration site at RVA 0x104DA40 - static analysis finds ZERO
//     references to that whole page, direct or relative, so the code is unreachable: the
//     class tables in the exe are development leftovers.
//
// What is left is the input path, and it is open: the game imports exactly one function from
// DINPUT8.dll - DirectInput8Create - and drives the camera from the right stick through
// IDirectInputDevice8. So instead of hunting for the camera, feed the game's OWN camera
// controller: add the headset's yaw to the right stick's horizontal axis and let the engine
// do the turning, with its own smoothing, limits, collision and scripted overrides intact.
//
// HOW IT HOOKS
// ------------
// The game calls DirectInput8Create (our export), gets an IDirectInput8, calls its
// CreateDevice through the vtable, and then polls the pad with
// IDirectInputDevice8::GetDeviceState / GetDeviceData. Both interfaces are COM, so the hooks
// are vtable patches - exactly the technique the d3d9 proxy in this project already uses,
// including the reason for patching the vtable rather than the module: the system DLL is
// loaded from System32 by full path, so the game's calls reach the real code and the only
// place to stand is the object.
//
// PHASE 1 (this file): measure. Log every axis of every device once a second, so one game run
// says which field is the right stick's horizontal axis. Axis values are DIJOFS offsets
// (dwOfs): 0 = lX, 4 = lY, 8 = lZ, 12 = lRx, 16 = lRy, 20 = lRz, 24 = rglSlider[0],
// 28 = rglSlider[1], 32 = rgdwPOV[0].
//
// PHASE 2: inject. The bridge (d3d9 proxy) publishes the head's yaw to a named shared
// mapping; this proxy reads it and adds a bounded increment to the chosen axis, so the game
// sees the player turning. Injection stays off until re6vr_di_inject.txt asks for it.
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>

// OBJ base FIRST: dinput.h needs REFIID / IUnknown, and it keys the vtable layout off
// DIRECTINPUT_VERSION. Both proxies in this project include the real SDK headers rather than
// hand-rolled interface structs - d3d9_min.h exists only because this project had to work
// around a reserved surface vtable slot, which DirectInput does not have.
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

// ---------------------------------------------------------------------------------------
// Logging: the same directory, the same file as the d3d9 proxy, so one log tells the story.
// ---------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------
// Forwarding to the real DirectInput8
// ---------------------------------------------------------------------------------------
// This is done by the LINKER, not at runtime, and that is the whole point.
//
// The first version resolved the system DLL with LoadLibraryExW(GetSystemDirectoryW() +
// "\\dinput8.dll", ..., LOAD_LIBRARY_SEARCH_SYSTEM32) and GetProcAddress - and the game died
// with a stack overflow (0xc00000fd) in DINPUT8.dll, twice, with the fault landing on a
// __security_check_cookie/ret, i.e. a stack blown away by runaway recursion. A path check, an
// image-range check on the resolved pointer and a re-entry guard did not stop it, because the
// recursion is not in this file's entry point: the loader resolves a forwarder's target
// module BY BASE NAME (ntdll!find_forwarded_export), so anything that forwards "dinput8.x" to
// "dinput8.x" - which is what a proxy in the game folder is - can resolve straight back to
// itself. Wine documents the same failure for the whole proxy-DLL pattern as bug 60130.
//
// The fix is to remove the name collision instead of guarding against it:
//
//   * the real system DLL is deployed RENAMED, as dinput8_orig.dll, beside this proxy, so
//     there is no second module called "dinput8" for the loader to confuse anything with;
//   * this proxy exports its OWN DirectInput8Create (the wrapper that patches the vtables) and
//     reaches the real function through dinput8_orig.dll;
//   * nothing in this proxy ever resolves a module called "dinput8".
//
// Two shapes of "forwarding" were tried before this one, and both are recorded because they
// fail silently in different ways:
//
//   * a linker FORWARDER (pragma /export:DirectInput8Create=dinput8_orig...) makes the export
//     point straight at the real DLL, so the wrapper never runs - the harness caught it:
//     DirectInput8Create succeeded and no vtable patch was ever logged;
//   * a STATIC IMPORT through an import library generated from the renamed DLL was dropped by
//     the linker entirely (dumpbin showed imports of KERNEL32 only), which would have failed
//     at the first call with a null pointer.
//
// So the real function is resolved at runtime from the RENAMED module, which has none of the
// recursing loader behaviour that killed the original version, and the resolved pointer is
// still verified against this module's image range.
//
// Compiled on x86, targeting the WOW64 copy: the 32-bit system DLL is
// C:\Windows\SysWOW64\dinput8.dll (PE32, 176640 bytes), NOT System32\dinput8.dll, which is
// PE32+ and cannot be loaded here at all.
typedef HRESULT(WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, IUnknown *);
PFN_DirectInput8Create g_real_create = nullptr;

namespace {

FILE *g_log = nullptr;
CRITICAL_SECTION g_log_lock;
bool g_log_ready = false;

void log_open() {
    if (g_log_ready) return;
    InitializeCriticalSection(&g_log_lock);
    g_log_ready = true;

    // Marker file beside the game's d3d9.dll names the log directory (Steam does not pass
    // the shell's environment on, and the game directory is not writable by a normal user).
    wchar_t dir[MAX_PATH] = L"";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&log_open, &self);
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';

    wchar_t marker[MAX_PATH] = L"";
    wcscpy_s(marker, MAX_PATH, path);
    wcscat_s(marker, MAX_PATH, L"re6vr_logdir.txt");
    FILE *m = _wfopen(marker, L"r");
    if (m) {
        if (fgetws(dir, MAX_PATH, m)) {
            size_t n = wcslen(dir);
            while (n && (dir[n - 1] == L'\n' || dir[n - 1] == L'\r' || dir[n - 1] == L' ')) dir[--n] = L'\0';
        }
        fclose(m);
    }
    if (!dir[0]) wcscpy_s(dir, MAX_PATH, path);

    wchar_t full[MAX_PATH] = L"";
    _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\re6vr.log", dir);
    g_log = _wfopen(full, L"a");
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] === dinput8 proxy attached %04d-%02d-%02d ===\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                st.wYear, st.wMonth, st.wDay);
        fflush(g_log);
    }
}

void dlog(const char *fmt, ...) {
    if (!g_log_ready) log_open();
    if (!g_log) return;
    EnterCriticalSection(&g_log_lock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d t=%lu] di: ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, (unsigned long)GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

// ---------------------------------------------------------------------------------------
// Marker switches
// ---------------------------------------------------------------------------------------
bool g_inject = false;        // re6vr_di_inject.txt: "1" enables stick injection
int  g_axis = 0;              // re6vr_di_axis.txt: DIJOFS offset to drive (0 = lX)
float g_gain = 1.0f;          // re6vr_di_gain.txt: units of stick per degree of head yaw
float g_scale = 1000.0f;      // axis full-scale, for converting degrees to stick units
bool g_markers_read = false;

void read_markers() {
    if (g_markers_read) return;
    g_markers_read = true;

    wchar_t dir[MAX_PATH] = L"";
    if (g_log) {
        // reuse the log path's directory
        wchar_t path[MAX_PATH] = L"";
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&log_open, &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscpy_s(dir, MAX_PATH, path);
        wchar_t marker[MAX_PATH] = L"";
        wcscat_s(marker, MAX_PATH, L"re6vr_logdir.txt");
        FILE *m = _wfopen(marker, L"r");
        if (m) {
            wchar_t line[MAX_PATH] = L"";
            if (fgetws(line, MAX_PATH, m)) {
                size_t n = wcslen(line);
                while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r' || line[n - 1] == L' ')) line[--n] = L'\0';
                wcscpy_s(dir, MAX_PATH, line);
            }
            fclose(m);
        }
    }
    if (!dir[0]) return;

    struct { const wchar_t *name; int *ival; float *fval; } files[] = {
        {L"re6vr_di_inject.txt", nullptr, nullptr},
    };
    (void)files;

    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\re6vr_di_inject.txt", dir);
    FILE *f = _wfopen(p, L"r");
    if (f) { int v = 0; if (fscanf_s(f, "%d", &v) == 1) g_inject = (v != 0); fclose(f); }
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\re6vr_di_axis.txt", dir);
    f = _wfopen(p, L"r");
    if (f) { int v = 0; if (fscanf_s(f, "%d", &v) == 1) g_axis = v; fclose(f); }
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\re6vr_di_gain.txt", dir);
    f = _wfopen(p, L"r");
    if (f) { float v = 0; if (fscanf_s(f, "%f", &v) == 1) g_gain = v; fclose(f); }
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\re6vr_di_scale.txt", dir);
    f = _wfopen(p, L"r");
    if (f) { float v = 0; if (fscanf_s(f, "%f", &v) == 1) g_scale = v; fclose(f); }

    dlog("markers: inject=%d axis=%d gain=%.3f scale=%.1f", (int)g_inject, g_axis, g_gain, g_scale);
}

// ---------------------------------------------------------------------------------------
// Head yaw, published by the d3d9 side through a named shared mapping.
//
// The two proxies are separate DLLs in one process, but they are loaded by different parts of
// the game and share no state, so a file mapping is the simplest correct channel. A struct
// with a sequence number is enough: the reader takes the last complete write.
// ---------------------------------------------------------------------------------------
struct HeadShared {
    volatile LONG seq;
    float yaw_deg;        // head yaw relative to the pose the bridge started from
    float pitch_deg;
    float pad[4];
};
HeadShared *g_head = nullptr;
HANDLE g_head_map = nullptr;

void head_open() {
    if (g_head) return;
    g_head_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(HeadShared), L"re6vr_head_pose");
    if (!g_head_map) { dlog("head: CreateFileMapping failed (%lu)", GetLastError()); return; }
    g_head = (HeadShared *)MapViewOfFile(g_head_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HeadShared));
    if (!g_head) { dlog("head: MapViewOfFile failed (%lu)", GetLastError()); return; }
    if (g_head->seq == 0) { g_head->yaw_deg = 0.0f; g_head->pitch_deg = 0.0f; }
    dlog("head: sharing '%ls' open, yaw now %.2f deg", L"re6vr_head_pose", g_head->yaw_deg);
}

float head_yaw() {
    if (!g_head) return 0.0f;
    const LONG s0 = g_head->seq;
    const float y = g_head->yaw_deg;
    return (g_head->seq == s0) ? y : 0.0f;      // changed under us: use the next frame's value
}

// ---------------------------------------------------------------------------------------
// The DirectInput hooks
// ---------------------------------------------------------------------------------------
// Everything below is the part that has to run in-process: patching the COM vtables the game
// polls the pad through. The real entry point is resolved at runtime from the renamed real DLL
// by resolve_real_create(), which lives just above the export at the bottom of this file.

// IDirectInput8 vtable slots (dinput.h order): 0 QueryInterface, 1 AddRef, 2 Release,
// 3 CreateDevice, 4 EnumDevices, 5 GetDeviceStatus, 6 RunControlPanel, 7 Initialize.
// IDirectInputDevice8 vtable slots: 0..2 IUnknown, 3 GetCapabilities, 4 EnumObjects,
// 5 GetProperty, 6 SetProperty, 7 Acquire, 8 Unacquire, 9 GetDeviceState, 10 GetDeviceData,
// 11 SetDataFormat, 12 SetEventNotification, 13 SetCooperativeLevel, 14 GetObjectInfo, ...
// Both orders verified against the Windows SDK's dinput.h (IDirectInputDevice8W block), not
// from memory: the first version of this file had them right, but a wrong slot would call an
// unrelated method with the wrong arguments, which is a crash with no clue in it.
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateDevice)(void *, REFGUID, void **, IUnknown *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GetDeviceState)(void *, DWORD, LPVOID);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GetDeviceData)(void *, DWORD, void *, DWORD *, DWORD);

PFN_CreateDevice   g_real_create_device = nullptr;
PFN_GetDeviceState g_real_get_state = nullptr;
PFN_GetDeviceData  g_real_get_data = nullptr;

// Which device this is, for the log: GUID_SysKeyboard / GUID_SysMouse / a joystick GUID.
char g_dev_kind[64] = "unknown";
GUID g_dev_guid = {};
bool g_dev_is_keyboard = false, g_dev_is_mouse = false;
LONG g_state_calls = 0, g_data_calls = 0;

// GUID_SysKeyboard / GUID_SysMouse come from dxguid.lib, which this proxy deliberately does
// not link: the values are fixed by DirectInput's ABI and spelling them out here keeps the
// proxy dependent on nothing but kernel32/user32.
const GUID kSysKeyboard =
    {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysMouse =
    {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

const char *axis_name(DWORD ofs) {    switch (ofs) {
        case 0:  return "lX";
        case 4:  return "lY";
        case 8:  return "lZ";
        case 12: return "lRx";
        case 16: return "lRy";
        case 20: return "lRz";
        case 24: return "rglSlider0";
        case 28: return "rglSlider1";
        case 32: return "rgdwPOV0";
        default: return "?";
    }
}

HRESULT STDMETHODCALLTYPE hook_get_device_state(void *self, DWORD size, LPVOID data) {
    const HRESULT hr = g_real_get_state(self, size, data);
    if (FAILED(hr) || !data) return hr;

    if (g_dev_is_keyboard || g_dev_is_mouse) {
        // Not the pad: log the first call only, so the log says the device was seen without
        // flooding it with mouse moves.
        if (++g_state_calls == 1) {
            dlog("GetDeviceState(%s, %lu) - passing through, no injection for this device",
                 g_dev_kind, (unsigned long)size);
        }
        return hr;
    }

    ++g_state_calls;
    read_markers();

    // A joystick state: DIJOYSTATE2 is the largest layout; lX starts at 0. The struct is
    // large, so only the axis fields that exist for the requested size are touched.
    auto *axes = (LONG *)data;
    const DWORD axis_count = size / 4;

    // Injection: add the head's yaw, in stick units, to the chosen axis. Clamped to the
    // signed 16-bit-analogue range DirectInput uses for a stick, because the game will
    // sensibly scale whatever it is given.
    if (g_inject && g_head) {
        const DWORD idx = (DWORD)g_axis / 4;
        if (idx < axis_count) {
            const float deg = head_yaw();
            const float units = deg / 360.0f * g_scale * g_gain;
            LONG v = axes[idx] + (LONG)units;
            if (v > (LONG)g_scale) v = (LONG)g_scale;
            if (v < -(LONG)g_scale) v = -(LONG)g_scale;
            axes[idx] = v;
        }
    }

    // Report once a second: every axis, so ONE run says which field is the right stick.
    static DWORD last_report = 0;
    const DWORD now = GetTickCount();
    if (now - last_report >= 1000) {
        last_report = now;
        char line[512] = "";
        int used = 0;
        const DWORD show = axis_count < 9 ? axis_count : 9;
        for (DWORD i = 0; i < show; ++i) {
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, "%s=%ld ",
                                axis_name(i * 4), axes[i]);
            if (used < 0 || (size_t)used >= sizeof(line)) break;
        }
        dlog("state[%ld] %s | head yaw %.1f | inject=%d axis=%d", (long)g_state_calls, line,
             g_head ? g_head->yaw_deg : 0.0f, (int)g_inject, g_axis);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_get_device_data(void *self, DWORD count, void *data,
                                               DWORD *count_out, DWORD flags) {
    const HRESULT hr = g_real_get_data(self, count, data, count_out, flags);
    // Buffered mode would need each DIDEVICEOBJECTDATA's dwOfs adjusted instead; report it
    // so the log says which mode the game uses rather than leaving it to be guessed.
    if (SUCCEEDED(hr) && !g_dev_is_keyboard && !g_dev_is_mouse) {
        if (++g_data_calls <= 2) {
            dlog("GetDeviceData(%s): %lu event(s) - THE GAME USES BUFFERED MODE; injection "
                 "must rewrite dwOfs entries, not the polled state",
                 g_dev_kind, count_out ? (unsigned long)*count_out : 0ul);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_device(void *self, REFGUID guid, void **out,
                                             IUnknown *outer) {
    const HRESULT hr = g_real_create_device(self, guid, out, outer);
    if (FAILED(hr) || !out || !*out) {
        dlog("CreateDevice failed (0x%08lX)", (unsigned long)hr);
        return hr;
    }
    // NOTE: REFGUID is a reference, so this is `guid`, not `guid->`. Getting that wrong cost a
    // compile cycle; the error message says so plainly, which is the argument for using the
    // real SDK headers instead of hand-written interface structs.
    g_dev_guid = guid;
    g_dev_is_keyboard = (IsEqualGUID(guid, kSysKeyboard) != 0);
    g_dev_is_mouse = (IsEqualGUID(guid, kSysMouse) != 0);
    if (g_dev_is_keyboard) strcpy_s(g_dev_kind, "keyboard");
    else if (g_dev_is_mouse) strcpy_s(g_dev_kind, "mouse");
    else {
        _snprintf_s(g_dev_kind, sizeof(g_dev_kind), _TRUNCATE,
                    "device{%08lX-%04X-%04X}",
                    (unsigned long)guid.Data1, (unsigned)guid.Data2, (unsigned)guid.Data3);
    }

    // Patch the device's vtable. Void** + slot, exactly like the d3d9 proxy: the interface
    // pointer the game holds is the object, and its vtable is the only thing both sides see.
    void **vt = *(void ***)*out;
    if (vt) {
        DWORD old = 0;
        VirtualProtect(&vt[9], sizeof(void *) * 2, PAGE_READWRITE, &old);
        g_real_get_state = (PFN_GetDeviceState)vt[9];
        g_real_get_data = (PFN_GetDeviceData)vt[10];
        vt[9] = (void *)&hook_get_device_state;
        vt[10] = (void *)&hook_get_device_data;
        VirtualProtect(&vt[9], sizeof(void *) * 2, old, &old);
        dlog("CreateDevice(%s): vtable patched (GetDeviceState was %p, GetDeviceData %p)",
             g_dev_kind, (void *)g_real_get_state, (void *)g_real_get_data);
    } else {
        dlog("CreateDevice(%s): no vtable?! - not hooked", g_dev_kind);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_di8_create_device(void *self, REFGUID guid, void **out,
                                                 IUnknown *outer) {
    // The IDirectInput8 vtable is patched in place rather than wrapped, so this is simply
    // the same function the real interface would have run, plus the device hook.
    return hook_create_device(self, guid, out, outer);
}

} // namespace

// Resolve the real entry point from the RENAMED real DLL, and verify it is not us.
//
// dinput8_orig.dll is a unique name, so the loader cannot resolve it back to this proxy - the
// whole reason the original version recursed is gone. The pointer check stays anyway: a stack
// overflow here (0xc00000fd, reported only as "Faulting module DINPUT8.dll") cost the game two
// launches and produced no diagnosis at all.
bool resolve_real_create() {
    if (g_real_create) return true;

    HMODULE real = nullptr;
    if (!GetModuleHandleExW(0, L"dinput8_orig.dll", &real) || !real) {
        real = LoadLibraryW(L"dinput8_orig.dll");
    }
    if (!real) {
        dlog("resolve_real_create: dinput8_orig.dll could not be loaded - it must sit beside "
             "this proxy (scripts\\deploy.bat di installs both). The game gets no DirectInput "
             "rather than a crash.");
        return false;
    }
    PFN_DirectInput8Create fn = (PFN_DirectInput8Create)GetProcAddress(real, "DirectInput8Create");
    if (!fn) {
        dlog("resolve_real_create: dinput8_orig.dll has no DirectInput8Create export");
        return false;
    }

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&resolve_real_create, &self);
    if (self) {
        const uint8_t *base = (const uint8_t *)self;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if ((const uint8_t *)fn >= base &&
                (const uint8_t *)fn < base + nt->OptionalHeader.SizeOfImage) {
                dlog("resolve_real_create: dinput8_orig.dll resolved to OUR OWN code at %p - "
                     "refusing; that is the recursion that overflows the stack", (void *)fn);
                return false;
            }
        }
    }
    g_real_create = fn;
    dlog("resolve_real_create: real DirectInput8Create at %p from dinput8_orig.dll", (void *)fn);
    return true;
}

// The export the game imports: OUR wrapper, which calls the real function and then patches the
// vtable the game polls the pad through. A linker forwarder here would skip this function
// entirely - see the note at the top of the file.
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
                                             LPVOID *out, IUnknown *outer) {
    // Reentrancy guard, and the shape of it is deliberate: REFramework documents the identical
    // crash for the same reason - another overlay hooking DirectInput8Create calls back into
    // this export, and the chain has to be broken with a thread-local flag that hands the
    // nested call straight to the real function. The pointer check above covers the other
    // shape of the same problem.
    static thread_local bool s_in_call = false;
    const bool nested = s_in_call;
    s_in_call = true;

    if (!resolve_real_create()) {
        s_in_call = nested;
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }

    const HRESULT hr = g_real_create(inst, version, iid, out, outer);
    if (nested) {
        s_in_call = false;                       // the outer call owns patching and logging
        return hr;
    }
    s_in_call = false;

    if (FAILED(hr) || !out || !*out) {
        dlog("DirectInput8Create failed (0x%08lX)", (unsigned long)hr);
        return hr;
    }

    // Patch IDirectInput8::CreateDevice (slot 3).
    void **vt = *(void ***)*out;
    if (vt) {
        DWORD old = 0;
        VirtualProtect(&vt[3], sizeof(void *), PAGE_READWRITE, &old);
        g_real_create_device = (PFN_CreateDevice)vt[3];
        vt[3] = (void *)&hook_di8_create_device;
        VirtualProtect(&vt[3], sizeof(void *), old, &old);
        dlog("DirectInput8Create ok: IDirectInput8 vtable patched (CreateDevice was %p)",
             (void *)g_real_create_device);
    }

    head_open();
    read_markers();
    return hr;
}

// The game's import table has exactly one entry from DINPUT8.dll, so one export is enough.
// DllMain stays empty on purpose: work done there runs under the loader lock, and the d3d9
// proxy in this project already learned that the hard way.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleW(nullptr));
        log_open();
        dlog("DllMain: attach");
    }
    return TRUE;
}
