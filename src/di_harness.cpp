// di_harness.cpp - exercise the dinput8 proxy outside the game.
//
// The d3d9 proxy in this project is proven by harness.exe, which drives it without RE6. The
// dinput8 proxy stops the game from starting, and "the game does not launch" says nothing
// about WHERE it fails - so this does the same job for input: load the proxy, call
// DirectInput8Create through it, create a keyboard device and a joystick device through the
// patched vtable, and poll them. The first step that fails is printed, and the proxy's own log
// fills in the rest.
//
// Build: scripts\build_di_harness.bat      Run: build\di_harness.exe build\dinput8.dll
#include <windows.h>
#include <cstdio>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

typedef HRESULT(WINAPI *PFN_Create)(HINSTANCE, DWORD, REFIID, LPVOID *, IUnknown *);

// The data formats are spelled out here instead of linking dinput8.lib for c_dfDIKeyboard /
// c_dfDIJoystick2. Two reasons: the SDK's .lib is not needed for anything else, and keeping
// the harness's only DLL dependency explicit makes it obvious that DirectInput8Create comes
// from the proxy under test and not from a static import of the system library.
static DIOBJECTDATAFORMAT g_kbd_obj = {nullptr, offsetof(DIDATAFORMAT, dwDataSize), 0x00000004, 0};
static DIDATAFORMAT g_kbd_fmt = {sizeof(DIDATAFORMAT), sizeof(DIDATAFORMAT), 0x00000001, 256,
                                 sizeof(g_kbd_obj) / sizeof(DIOBJECTDATAFORMAT), &g_kbd_obj};

static DIOBJECTDATAFORMAT g_js_obj[6] = {
    {nullptr, 0,  0x00000004 | 0x00000400, 0},   // lX, axis
    {nullptr, 4,  0x00000004 | 0x00000400, 0},   // lY
    {nullptr, 8,  0x00000004 | 0x00000400, 0},   // lZ
    {nullptr, 12, 0x00000004 | 0x00000400, 0},   // lRx
    {nullptr, 16, 0x00000004 | 0x00000400, 0},   // lRy
    {nullptr, 20, 0x00000004 | 0x00000400, 0},   // lRz
};
static DIDATAFORMAT g_js_fmt = {sizeof(DIDATAFORMAT), sizeof(DIJOYSTATE2), 0x00000001, 6,
                                sizeof(g_js_obj) / sizeof(DIOBJECTDATAFORMAT), g_js_obj};

int main(int argc, char **argv) {
    const char *dll = (argc > 1) ? argv[1] : "dinput8.dll";

    // A real window FIRST. DirectInput refuses CreateDevice with DIERR_NOTINITIALIZED
    // (0x80040154) when the process has no window - which is exactly what happened on the
    // first run of this harness: every device creation failed, so the proxy's CreateDevice
    // hook was never reached and the harness proved less than it looked like it did. The game
    // has a window, so the harness needs one too.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"re6vr_di_harness";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"re6vr di harness", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr,
                                wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    printf("[di-harness] window %p\n", (void *)hwnd);

    printf("[di-harness] loading %s\n", dll);
    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        printf("[di-harness] LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    printf("[di-harness] loaded at %p\n", (void *)h);

    PFN_Create create = (PFN_Create)GetProcAddress(h, "DirectInput8Create");
    if (!create) {
        printf("[di-harness] no DirectInput8Create export\n");
        return 1;
    }
    printf("[di-harness] DirectInput8Create at %p\n", (void *)create);

    IDirectInput8W *di = nullptr;
    HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                        (LPVOID *)&di, nullptr);
    printf("[di-harness] step 1: DirectInput8Create -> 0x%08lX, iface %p\n",
           (unsigned long)hr, (void *)di);
    if (FAILED(hr) || !di) return 2;

    printf("[di-harness] step 2: CreateDevice(keyboard) - this goes through the proxy hook\n");
    IDirectInputDevice8W *kbd = nullptr;
    hr = di->CreateDevice(GUID_SysKeyboard, &kbd, nullptr);
    printf("[di-harness] step 2 -> 0x%08lX, device %p\n", (unsigned long)hr, (void *)kbd);
    if (FAILED(hr) || !kbd) return 3;

    printf("[di-harness] step 3: SetDataFormat + Acquire + GetDeviceState(keyboard)\n");
    hr = kbd->SetDataFormat(&g_kbd_fmt);
    printf("[di-harness] SetDataFormat -> 0x%08lX\n", (unsigned long)hr);
    hr = kbd->Acquire();
    printf("[di-harness] Acquire -> 0x%08lX\n", (unsigned long)hr);
    char keys[256] = {0};
    hr = kbd->GetDeviceState(sizeof(keys), keys);
    printf("[di-harness] GetDeviceState -> 0x%08lX\n", (unsigned long)hr);

    printf("[di-harness] step 4: CreateDevice(joystick) - enumerated, not assumed\n");
    IDirectInputDevice8W *pad = nullptr;
    hr = di->CreateDevice(GUID_Joystick, &pad, nullptr);
    printf("[di-harness] CreateDevice(joystick) -> 0x%08lX, device %p\n",
           (unsigned long)hr, (void *)pad);
    if (SUCCEEDED(hr) && pad) {
        hr = pad->SetDataFormat(&g_js_fmt);
        printf("[di-harness] SetDataFormat(joystick) -> 0x%08lX\n", (unsigned long)hr);
        hr = pad->Acquire();
        printf("[di-harness] Acquire(joystick) -> 0x%08lX\n", (unsigned long)hr);
        DIJOYSTATE2 js = {};
        for (int i = 0; i < 3; ++i) {
            hr = pad->GetDeviceState(sizeof(js), &js);
            printf("[di-harness] joystick GetDeviceState -> 0x%08lX  lX=%ld lY=%ld lZ=%ld "
                   "lRx=%ld lRy=%ld lRz=%ld\n",
                   (unsigned long)hr, js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz);
            Sleep(300);
        }
        pad->Unacquire();
        pad->Release();
    }

    kbd->Unacquire();
    kbd->Release();
    di->Release();
    printf("[di-harness] done\n");
    return 0;
}
