// loader.cpp - minimal Win32 host that exercises the d3d9 proxy on its own.
//
// Purpose: verify the proxy (DllMain, MinHook init, Direct3DCreate9 wrapper,
// IDirect3DDevice9::Present detour, OpenXR init) without involving RE6 at all.
// It creates a small window, brings up a D3D9 device and presents in a loop, so
// the proxy log shows exactly how far it got. Useful for bisecting a problem:
// if this works and the game does not, the problem is game-side.
//
// Build:  scripts\build_harness.bat   (~build\harness.exe)
// Run:    copy next to the game's d3d9.dll, or pass the proxy's full path.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "d3d9_min.h"

namespace {

PFN_Direct3DCreate9 g_create9 = nullptr;
bool g_running = true;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const int seconds = (argc > 2) ? atoi(argv[2]) : 15;
    const int width = (argc > 3) ? atoi(argv[3]) : 1280;
    const int height = (argc > 4) ? atoi(argv[4]) : 720;
    // "reset" makes the harness perform a device Reset mid-run: that is what a
    // fullscreen game does at start-up, and the proxy has to survive it.
    const bool do_reset = (argc > 5) && lstrcmpA(argv[5], "reset") == 0;
    const char *proxy_path = (argc > 1) ? argv[1] : "d3d9.dll";

    printf("[harness] starting (proxy=%s, %d seconds, %dx%d%s)\n", proxy_path, seconds, width, height,
           do_reset ? ", with a mid-run Reset" : "");

    HMODULE proxy = LoadLibraryA(proxy_path);
    if (!proxy) {
        printf("[harness] LoadLibraryA(%s) failed: %lu\n", proxy_path, GetLastError());
        return 1;
    }
    printf("[harness] proxy loaded at %p\n", (void *)proxy);

    g_create9 = (PFN_Direct3DCreate9)GetProcAddress(proxy, "Direct3DCreate9");
    if (!g_create9) {
        printf("[harness] proxy has no Direct3DCreate9\n");
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"re6vr_harness";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"re6vr harness", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        printf("[harness] CreateWindowExW failed: %lu\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    IDirect3D9 *d3d9 = g_create9(D3D_SDK_VERSION);
    printf("[harness] Direct3DCreate9 -> %p\n", (void *)d3d9);
    if (!d3d9) return 1;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = 75 /*D3DFMT_D24S8*/;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d3d9->lpVtbl->CreateDevice(d3d9, 0, D3DDEVTYPE_HAL, hwnd,
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                            &pp, &dev);
    printf("[harness] CreateDevice -> 0x%08lX, device %p\n", (unsigned long)hr, (void *)dev);
    if (FAILED(hr) || !dev) return 1;

    // "vsmatrix" uploads one known view matrix and one known projection matrix,
    // which calibrates the RE6VR_CAPTURE_VS classifier: the log must report reg 0
    // as VIEW and reg 8 as PROJ. Without this the probe could only be trusted after
    // a game launch.
    const bool do_vs_matrix = (argc > 5) && lstrcmpA(argv[5], "vsmatrix") == 0;
    // "vsdecoy" uploads the pair that fooled the pick logic twice: a projection matrix
    // whose "translation" is one over the screen size (1/3840 = 0.000260417, an
    // ordinary small number that passes any world-scale test) followed by the real
    // camera. A correct classifier must label the first PROJ and the second VIEW, so
    // that pick index 0 lands on the camera.
    const bool do_vs_decoy = (argc > 5) && lstrcmpA(argv[5], "vsdecoy") == 0;
    // "vsmoving" uploads one camera whose rotation changes every frame plus a static
    // rigid matrix, so the `auto` camera search has something to actually search for.
    const bool do_vs_moving = (argc > 5) && lstrcmpA(argv[5], "vsmoving") == 0;

    const DWORD start = GetTickCount();
    int frames = 0;
    bool reset_done = false;
    MSG msg;
    while (g_running && (GetTickCount() - start) < (DWORD)seconds * 1000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (do_reset && !reset_done && (GetTickCount() - start) > 2000) {
            reset_done = true;
            HRESULT rhr = dev->lpVtbl->Reset(dev, &pp);
            printf("[harness] Reset -> 0x%08lX (the proxy must rebuild its surfaces)\n",
                   (unsigned long)rhr);
        }
        // "vsmoving" is the shape the `auto` camera search has to cope with: several
        // rigid matrices are uploaded, only ONE of them turns from frame to frame, and
        // nothing about the matrices themselves says which. The one that moves is the
        // camera; the static ones stand in for the HUD and sprite placements that made
        // "rotate every view matrix" stretch the picture on the real game.
        if (do_vs_moving) {
            // Offset so the angle is never a multiple of pi: at those values the yaw is
            // the identity and the classifier calls the window "UI/world-axis", which
            // would silently remove the moving camera from the test.
            const float a = 0.7853982f + 0.0628f * (float)(frames % 97);
            const float cs = cosf(a), sn = sinf(a);
            // The camera WALKS too, and walking is the signal `trace_check.py` keys on: a
            // HUD placement keeps a fixed translation, so translation travel is what tells
            // the world camera apart from everything else that is rigid.
            const float wx = 1200.0f + 6.0f * (float)(frames % 211);
            const float wz = -800.0f - 4.0f * (float)(frames % 173);
            const float pair[32] = {
                // reg 0..3 : the MOVING camera - yaw and position both change every frame
                cs,   0.0f, -sn,  0.0f,
                0.0f, 1.0f,  0.0f, 0.0f,
                sn,   0.0f,  cs,  0.0f,
                wx,   300.0f, wz,  1.0f,
                // reg 8..11 : a static rigid matrix, as a HUD or prop placement would be
                1.0f, 0.0f,  0.0f, 0.0f,
                0.0f, 1.0f,  0.0f, 0.0f,
                0.0f, 0.0f,  1.0f, 0.0f,
                0.0f, 0.0f,  1.0f, 1.0f,
            };
            // The "title screen" stretch: the moving camera is NOT uploaded yet, so the only
            // candidates are unit-scale UI matrices. Letting the camera search start here is
            // exactly the mistake that got a UI matrix rotated on the real game, and it
            // showed up as a stretched picture from the title screen onwards.
            if (frames >= 60) dev->lpVtbl->SetVertexShaderConstantF(dev, 0, pair, 8);
            else dev->lpVtbl->SetVertexShaderConstantF(dev, 8, pair + 16, 4);
        }
        if (do_vs_decoy) {
            // Both matrices in ONE call, which is the shape the game actually uses: the
            // title-screen batch carries a projection matrix at reg 1..4 and the camera at
            // reg 5..8, plus a transposed copy of the camera after it. The decoy stands in
            // for the projection: rigid 3x3 (identity) with the screen reciprocal as its
            // "translation" (1/3840, 1/2160), which is exactly what was picked instead of
            // the camera twice.
            //
            // What the labels actually come out as, measured rather than assumed: the decoy
            // is "VIEW^T" (its 5.2e-04 translation is under the 0.01 gate, so it never
            // reaches the rigid branch) and the camera is "UI/world-axis" (its rotation is
            // the identity - a camera that has not turned yet). The property this mode
            // exists to protect still holds either way: the decoy is not a selectable view
            // matrix, so pick index 0 lands on the camera.
            const float combined[32] = {
                // reg 0..3 : the decoy (projection, screen-space translation)
                1.0f,         0.0f,        0.0f, 0.0f,
                0.0f,         1.0f,        0.0f, 0.0f,
                0.0f,         0.0f,        1.0f, 0.0f,
                0.000260417f, 0.000462963f, 0.0f, 1.0f,
                // reg 4..7 : the real camera, far from the origin like the game's.
                // Stored as the transpose of the "30 degrees about Y" it represents, which
                // is the reading the engine uploads for its own camera.
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                1920.0f, -30.0f, 4430.0f, 1.0f,
            };
            dev->lpVtbl->SetVertexShaderConstantF(dev, 0, combined, 8);
        }
        // "vsviews" uploads THREE view matrices in one frame, in two batches, the last of
        // them at a register well away from the others. This is the layout that made "pick
        // the last view matrix" silently do nothing: the last one is only the third *view
        // candidate* although it sits tens of registers later, and the selection code was
        // comparing a candidate number against a register offset. With the game's real
        // batches (VIEW at reg 1/4/6/27) the same mismatch left every candidate unchosen -
        // the hook fired 600k times and rotated nothing. One view matrix cannot catch that,
        // so the regression needs three, and it needs them in separate batches.
        const bool do_vs_views = (argc > 5) && lstrcmpA(argv[5], "vsviews") == 0;
        if (do_vs_views) {
            // All translations are deliberately world-sized and all three rotations
            // non-trivial, so each one classifies as a selectable view matrix. The yaw
            // matrices are stored as the transpose of the angle named here, which is the
            // same reading the game uploads; the classifier accepts either.
            const float views[48] = {
                // reg 0..3 : camera A, 30 degrees about Y, at (3, 1.25, -7)
                0.8660254f, 0.0f, -0.5f, 0.0f,
                0.0f,       1.0f,  0.0f, 0.0f,
                0.5f,       0.0f,  0.8660254f, 0.0f,
                3.0f,       1.25f, -7.0f, 1.0f,
                // reg 4..7 : camera B, 60 degrees about Y, at (100, 20, -300)
                0.5f,       0.0f, -0.8660254f, 0.0f,
                0.0f,       1.0f,  0.0f, 0.0f,
                0.8660254f, 0.0f,  0.5f, 0.0f,
                100.0f,     20.0f, -300.0f, 1.0f,
                // reg 24..27 : camera C, -45 degrees about Y, at (-40, 5, 250). Uploaded in
                // its own batch, far from the others, so the log can tell which one moved.
                0.7071068f,  0.0f, 0.7071068f, 0.0f,
                0.0f,        1.0f, 0.0f, 0.0f,
                -0.7071068f, 0.0f, 0.7071068f, 0.0f,
                -40.0f,      5.0f, 250.0f, 1.0f,
            };
            // The second call must name camC's own offset and its own length. Passing
            // views + 24 with 9 vec4 read 12 floats past the end of the array into stack
            // memory, which put the wrong registers in play and could have invented a
            // candidate out of adjacent stack garbage.
            dev->lpVtbl->SetVertexShaderConstantF(dev, 0, views, 8);
            dev->lpVtbl->SetVertexShaderConstantF(dev, 24, views + 32, 4);
        }
        // "camconvention" uploads an identity-rotation camera at reg 0 and the SAME
        // camera pose rotated at reg 16. It answers which order the head rotation is
        // applied in, by making the expected result computable by hand: for a non-identity
        // anchor, V*R and R*V differ, and only the log says which one the code produced.
        // The identity camera is included as the degenerate sanity case (its rotation must
        // come out as exactly R).
        const bool do_cam_convention = (argc > 5) && lstrcmpA(argv[5], "camconvention") == 0;
        if (do_cam_convention) {
            // 30 degrees of yaw, then 20 degrees of pitch: R = Ry(-30) * Rx(20), which is
            // NOT symmetric, so a transposed reading is visible in the log.
            const float cam[32] = {
                // reg 0..3 : identity camera - view basis = world basis, moved to (10, 2, -5)
                1.0f,  0.0f,  0.0f,  0.0f,
                0.0f,  1.0f,  0.0f,  0.0f,
                0.0f,  0.0f,  1.0f,  0.0f,
                10.0f, 2.0f, -5.0f,  1.0f,
                // reg 16..19 : the same construction with R applied to the basis.
                // Row 1 col 2 must be NEGATIVE: with +0.2961981 here the three rows are unit
                // length but not mutually perpendicular (row0.row1 = -0.2962), so the window
                // is not a rotation at all and classify() rejects it outright.
                0.8660254f,  0.0f,       -0.5f,      0.0f,
                -0.1710101f, 0.9396926f, -0.2961981f, 0.0f,
                0.4698463f,  0.3420201f,  0.8137977f, 0.0f,
                -300.0f,     40.0f,      700.0f,      1.0f,
            };
            // The second argument is the FIRST REGISTER, but the source pointer has to be
            // offset as well. Passing `cam` with register 16 re-uploads camera A at reg 16
            // instead of placing camera B there, and the two matrices silently become one.
            dev->lpVtbl->SetVertexShaderConstantF(dev, 0, cam, 8);
            dev->lpVtbl->SetVertexShaderConstantF(dev, 16, cam + 16, 4);
        }
        if (do_vs_matrix) {
            // A rigid transform: a yaw of minus 30 degrees about Y (the sin-term signs put
            // it on that side - the exact sign does not matter to the classifier, which
            // accepts either reading), then translated. Rows are unit length and mutually
            // perpendicular, and the translation sits in the fourth row - the shape the
            // probe should recognise as a view matrix.
            const float c = 0.8660254f, sn = 0.5f;
            const float view[16] = {
                c,    0.0f, -sn,  0.0f,
                0.0f, 1.0f,  0.0f, 0.0f,
                sn,   0.0f,  c,   0.0f,
                3.0f, 1.25f, -7.0f, 1.0f,
            };
            // Standard left-handed D3D projection: fourth column zero, fourth row
            // (0, 0, 1, 0).
            const float proj[16] = {
                1.29904f, 0.0f,     0.0f,  0.0f,
                0.0f,     1.73205f, 0.0f,  0.0f,
                0.0f,     0.0f,     1.0020f, 1.0f,
                0.0f,     0.0f,    -0.2002f, 0.0f,
            };
            dev->lpVtbl->SetVertexShaderConstantF(dev, 0, view, 4);
            dev->lpVtbl->SetVertexShaderConstantF(dev, 8, proj, 4);
        }
        dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0xFF203040, 1.0f, 0);
        dev->lpVtbl->BeginScene(dev);
        dev->lpVtbl->EndScene(dev);
        hr = dev->lpVtbl->Present(dev, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            printf("[harness] Present failed 0x%08lX after %d frames\n", (unsigned long)hr, frames);
            break;
        }
        ++frames;
        Sleep(10);
    }
    printf("[harness] presented %d frames\n", frames);

    dev->lpVtbl->Release(dev);
    d3d9->lpVtbl->Release(d3d9);
    printf("[harness] done\n");
    return 0;
}
