// d3d9_min.h - minimal hand-rolled D3D9 declarations for the RE6 VR proxy.
//
// We deliberately do NOT include <d3d9.h>: the Windows 10 SDK has no d3d9.h
// (it only ships d3d9helper.h), and pulling in the legacy DirectX SDK is not an
// option here. The ABI we need is stable and well documented:
//
//   IDirect3D9        : 17 vtable slots (0..16)
//   IDirect3DDevice9  : 119 vtable slots (0..118), Present() == slot 17
//   IDirect3D9Ex      : 21 slots (the 17 above plus CreateDeviceEx, slot 17)
//   IDirect3DDevice9Ex: 119 slots + 8 Ex methods appended
//
// Everything we do not call is declared with its exact parameter count so the
// layout (and therefore the offsets we patch) is exact.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// ---------------------------------------------------------------- constants
#define D3D_SDK_VERSION 32

typedef DWORD D3DFORMAT;
#define D3DFMT_UNKNOWN  ((D3DFORMAT)0)
#define D3DFMT_R5G6B5   ((D3DFORMAT)23)
#define D3DFMT_X1R5G5B5 ((D3DFORMAT)24)
#define D3DFMT_A8R8G8B8 ((D3DFORMAT)21)
#define D3DFMT_X8R8G8B8 ((D3DFORMAT)22)
#define D3DFMT_A16B16G16R16F ((D3DFORMAT)113)
#define D3DFMT_A32B32G32R32F ((D3DFORMAT)116)

// D3DSWAPEFFECT
#define D3DSWAPEFFECT_DISCARD 1
#define D3DSWAPEFFECT_FLIP    2
#define D3DSWAPEFFECT_COPY    3
#define D3DSWAPEFFECT_OVERLAY 4
#define D3DSWAPEFFECT_FLIPEX  5

// D3DDEVTYPE / D3DDEVICE_CREATION_PARAMETERS
#define D3DDEVTYPE_HAL  1
#define D3DDEVTYPE_NULLREF 4

// Present flags
#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 0x00000001
#define D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL 0x00000002
#define D3DPRESENTFLAG_DEVICECLIP 0x00000004
#define D3DPRESENTFLAG_VIDEO 0x00000010

// Behaviour flags we want to keep (so our Ex device behaves like the D3D9 one)
#define D3DCREATE_FPU_PRESERVE 0x00000002
#define D3DCREATE_MULTITHREADED 0x00000004
#define D3DCREATE_PUREDEVICE 0x00000010
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040
#define D3DCREATE_MIXED_VERTEXPROCESSING 0x00000080
#define D3DCREATE_DISABLE_DRIVER_MANAGEMENT 0x00000100

// IDirect3D9::CreateDevice behaviour flags
#define D3DCREATE_ADAPTERGROUP_DEVICE 0x00000200

struct D3DDISPLAYMODE {
    UINT      Width;
    UINT      Height;
    UINT      RefreshRate;
    D3DFORMAT Format;
};

struct D3DPRESENT_PARAMETERS {
    UINT                BackBufferWidth;
    UINT                BackBufferHeight;
    D3DFORMAT           BackBufferFormat;
    UINT                BackBufferCount;
    DWORD               MultiSampleType;
    DWORD               MultiSampleQuality;
    DWORD               SwapEffect;
    HWND                hDeviceWindow;
    BOOL                Windowed;
    BOOL                EnableAutoDepthStencil;
    D3DFORMAT           AutoDepthStencilFormat;
    DWORD               Flags;
    UINT                FullScreen_RefreshRateInHz;
    UINT                PresentationInterval;
};

struct D3DDEVICE_CREATION_PARAMETERS {
    UINT    AdapterOrdinal;
    DWORD   DeviceType;
    HWND    hFocusWindow;
    DWORD   BehaviorFlags;
};

// ------------------------------------------------------------------ vtables
struct IDirect3D9;
struct IDirect3D9Ex;
struct IDirect3DDevice9;
struct IDirect3DDevice9Ex;

struct IDirect3D9Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3D9 *, const IID &, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDirect3D9 *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDirect3D9 *);
    HRESULT (STDMETHODCALLTYPE *RegisterSoftwareDevice)(IDirect3D9 *, void *);
    UINT    (STDMETHODCALLTYPE *GetAdapterCount)(IDirect3D9 *);
    HRESULT (STDMETHODCALLTYPE *GetAdapterIdentifier)(IDirect3D9 *, UINT, DWORD, void *);
    UINT    (STDMETHODCALLTYPE *GetAdapterModeCount)(IDirect3D9 *, UINT, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *EnumAdapterModes)(IDirect3D9 *, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE *);
    HRESULT (STDMETHODCALLTYPE *GetAdapterDisplayMode)(IDirect3D9 *, UINT, D3DDISPLAYMODE *);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceType)(IDirect3D9 *, UINT, DWORD, D3DFORMAT, D3DFORMAT, BOOL);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceFormat)(IDirect3D9 *, UINT, DWORD, D3DFORMAT, DWORD, DWORD, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceMultiSampleType)(IDirect3D9 *, UINT, DWORD, D3DFORMAT, BOOL, DWORD, DWORD *);
    HRESULT (STDMETHODCALLTYPE *CheckDepthStencilMatch)(IDirect3D9 *, UINT, DWORD, D3DFORMAT, D3DFORMAT, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceFormatConversion)(IDirect3D9 *, UINT, DWORD, D3DFORMAT, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *GetDeviceCaps)(IDirect3D9 *, UINT, DWORD, void *);
    HMONITOR(STDMETHODCALLTYPE *GetAdapterMonitor)(IDirect3D9 *, UINT);
    HRESULT (STDMETHODCALLTYPE *CreateDevice)(IDirect3D9 *, UINT, DWORD, HWND, DWORD,
                                              D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
};

struct IDirect3D9 {
    IDirect3D9Vtbl *lpVtbl;
};

// 119 slots, in order. Unused ones are declared with the right arity so the
// offsets stay exact; Present() must land on slot 17.
struct IDirect3DDevice9Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3DDevice9 *, const IID &, void **);   // 0
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDirect3DDevice9 *);                                  // 1
    ULONG   (STDMETHODCALLTYPE *Release)(IDirect3DDevice9 *);                                 // 2
    HRESULT (STDMETHODCALLTYPE *TestCooperativeLevel)(IDirect3DDevice9 *);                    // 3
    UINT    (STDMETHODCALLTYPE *GetAvailableTextureMem)(IDirect3DDevice9 *);                  // 4
    HRESULT (STDMETHODCALLTYPE *EvictManagedResources)(IDirect3DDevice9 *);                   // 5
    HRESULT (STDMETHODCALLTYPE *GetDirect3D)(IDirect3DDevice9 *, IDirect3D9 **);              // 6
    HRESULT (STDMETHODCALLTYPE *GetDeviceCaps)(IDirect3DDevice9 *, void *);                   // 7
    HRESULT (STDMETHODCALLTYPE *GetDisplayMode)(IDirect3DDevice9 *, UINT, D3DDISPLAYMODE *);  // 8
    HRESULT (STDMETHODCALLTYPE *GetCreationParameters)(IDirect3DDevice9 *, D3DDEVICE_CREATION_PARAMETERS *); // 9
    HRESULT (STDMETHODCALLTYPE *SetCursorProperties)(IDirect3DDevice9 *, UINT, UINT, void *); // 10
    void    (STDMETHODCALLTYPE *SetCursorPosition)(IDirect3DDevice9 *, int, int, DWORD);      // 11
    BOOL    (STDMETHODCALLTYPE *ShowCursor)(IDirect3DDevice9 *, BOOL);                        // 12
    HRESULT (STDMETHODCALLTYPE *CreateAdditionalSwapChain)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, void **); // 13
    HRESULT (STDMETHODCALLTYPE *GetSwapChain)(IDirect3DDevice9 *, UINT, void **);             // 14
    UINT    (STDMETHODCALLTYPE *GetNumberOfSwapChains)(IDirect3DDevice9 *);                   // 15
    HRESULT (STDMETHODCALLTYPE *Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);          // 16
    HRESULT (STDMETHODCALLTYPE *Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *); // 17
    HRESULT (STDMETHODCALLTYPE *GetBackBuffer)(IDirect3DDevice9 *, UINT, UINT, DWORD, void **); // 18
    HRESULT (STDMETHODCALLTYPE *GetRasterStatus)(IDirect3DDevice9 *, UINT, void *);           // 19
    HRESULT (STDMETHODCALLTYPE *SetDialogBoxMode)(IDirect3DDevice9 *, BOOL);                  // 20
    void    (STDMETHODCALLTYPE *SetGammaRamp)(IDirect3DDevice9 *, UINT, DWORD, const void *);  // 21
    void    (STDMETHODCALLTYPE *GetGammaRamp)(IDirect3DDevice9 *, UINT, void *);              // 22
    HRESULT (STDMETHODCALLTYPE *CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, DWORD, void **, void **); // 23
    HRESULT (STDMETHODCALLTYPE *CreateVolumeTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, DWORD, void **, void **); // 24
    HRESULT (STDMETHODCALLTYPE *CreateCubeTexture)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, DWORD, void **, void **); // 25
    HRESULT (STDMETHODCALLTYPE *CreateVertexBuffer)(IDirect3DDevice9 *, UINT, DWORD, DWORD, DWORD, void **, void **); // 26
    HRESULT (STDMETHODCALLTYPE *CreateIndexBuffer)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DFORMAT, void **, void **); // 27
    HRESULT (STDMETHODCALLTYPE *CreateRenderTarget)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, DWORD, DWORD, BOOL, void **, void **); // 28
    HRESULT (STDMETHODCALLTYPE *CreateDepthStencilSurface)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, DWORD, DWORD, BOOL, void **, void **); // 29
    HRESULT (STDMETHODCALLTYPE *UpdateSurface)(IDirect3DDevice9 *, void *, const RECT *, void *, const POINT *); // 30
    HRESULT (STDMETHODCALLTYPE *UpdateTexture)(IDirect3DDevice9 *, void *, void *);           // 31
    HRESULT (STDMETHODCALLTYPE *GetRenderTargetData)(IDirect3DDevice9 *, void *, void *);     // 32
    HRESULT (STDMETHODCALLTYPE *GetFrontBufferData)(IDirect3DDevice9 *, UINT, void *);        // 33
    HRESULT (STDMETHODCALLTYPE *StretchRect)(IDirect3DDevice9 *, void *, const RECT *, void *, const RECT *, DWORD); // 34
    HRESULT (STDMETHODCALLTYPE *ColorFill)(IDirect3DDevice9 *, void *, const RECT *, DWORD);  // 35
    HRESULT (STDMETHODCALLTYPE *CreateOffscreenPlainSurface)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, DWORD, void **, void **); // 36
    HRESULT (STDMETHODCALLTYPE *SetRenderTarget)(IDirect3DDevice9 *, DWORD, void *);          // 37
    HRESULT (STDMETHODCALLTYPE *GetRenderTarget)(IDirect3DDevice9 *, DWORD, void **);         // 38
    HRESULT (STDMETHODCALLTYPE *SetDepthStencilSurface)(IDirect3DDevice9 *, void *);          // 39
    HRESULT (STDMETHODCALLTYPE *GetDepthStencilSurface)(IDirect3DDevice9 *, void **);         // 40
    HRESULT (STDMETHODCALLTYPE *BeginScene)(IDirect3DDevice9 *);                              // 41
    HRESULT (STDMETHODCALLTYPE *EndScene)(IDirect3DDevice9 *);                                // 42
    HRESULT (STDMETHODCALLTYPE *Clear)(IDirect3DDevice9 *, DWORD, const void *, DWORD, DWORD, float, DWORD); // 43
    HRESULT (STDMETHODCALLTYPE *SetTransform)(IDirect3DDevice9 *, DWORD, const void *);       // 44
    HRESULT (STDMETHODCALLTYPE *GetTransform)(IDirect3DDevice9 *, DWORD, void *);             // 45
    HRESULT (STDMETHODCALLTYPE *MultiplyTransform)(IDirect3DDevice9 *, DWORD, const void *);  // 46
    HRESULT (STDMETHODCALLTYPE *SetViewport)(IDirect3DDevice9 *, const void *);               // 47
    HRESULT (STDMETHODCALLTYPE *GetViewport)(IDirect3DDevice9 *, void *);                     // 48
    HRESULT (STDMETHODCALLTYPE *SetMaterial)(IDirect3DDevice9 *, const void *);               // 49
    HRESULT (STDMETHODCALLTYPE *GetMaterial)(IDirect3DDevice9 *, void *);                     // 50
    HRESULT (STDMETHODCALLTYPE *SetLight)(IDirect3DDevice9 *, DWORD, const void *);           // 51
    HRESULT (STDMETHODCALLTYPE *GetLight)(IDirect3DDevice9 *, DWORD, void *);                 // 52
    HRESULT (STDMETHODCALLTYPE *LightEnable)(IDirect3DDevice9 *, DWORD, BOOL);                // 53
    HRESULT (STDMETHODCALLTYPE *GetLightEnable)(IDirect3DDevice9 *, DWORD, BOOL *);           // 54
    HRESULT (STDMETHODCALLTYPE *SetClipPlane)(IDirect3DDevice9 *, DWORD, const float *);      // 55
    HRESULT (STDMETHODCALLTYPE *GetClipPlane)(IDirect3DDevice9 *, DWORD, float *);            // 56
    HRESULT (STDMETHODCALLTYPE *SetRenderState)(IDirect3DDevice9 *, DWORD, DWORD);            // 57
    HRESULT (STDMETHODCALLTYPE *GetRenderState)(IDirect3DDevice9 *, DWORD, DWORD *);          // 58
    HRESULT (STDMETHODCALLTYPE *CreateStateBlock)(IDirect3DDevice9 *, DWORD, void **);        // 59
    HRESULT (STDMETHODCALLTYPE *BeginStateBlock)(IDirect3DDevice9 *);                         // 60
    HRESULT (STDMETHODCALLTYPE *EndStateBlock)(IDirect3DDevice9 *, void **);                  // 61
    HRESULT (STDMETHODCALLTYPE *SetClipStatus)(IDirect3DDevice9 *, const void *);             // 62
    HRESULT (STDMETHODCALLTYPE *GetClipStatus)(IDirect3DDevice9 *, void *);                   // 63
    HRESULT (STDMETHODCALLTYPE *GetTexture)(IDirect3DDevice9 *, DWORD, void **);              // 64
    HRESULT (STDMETHODCALLTYPE *SetTexture)(IDirect3DDevice9 *, DWORD, void *);               // 65
    HRESULT (STDMETHODCALLTYPE *GetTextureStageState)(IDirect3DDevice9 *, DWORD, DWORD, DWORD *); // 66
    HRESULT (STDMETHODCALLTYPE *SetTextureStageState)(IDirect3DDevice9 *, DWORD, DWORD, DWORD); // 67
    HRESULT (STDMETHODCALLTYPE *GetSamplerState)(IDirect3DDevice9 *, DWORD, DWORD, DWORD *);  // 68
    HRESULT (STDMETHODCALLTYPE *SetSamplerState)(IDirect3DDevice9 *, DWORD, DWORD, DWORD);    // 69
    HRESULT (STDMETHODCALLTYPE *ValidateDevice)(IDirect3DDevice9 *, DWORD *);                 // 70
    HRESULT (STDMETHODCALLTYPE *SetPaletteEntries)(IDirect3DDevice9 *, UINT, const void *);   // 71
    HRESULT (STDMETHODCALLTYPE *GetPaletteEntries)(IDirect3DDevice9 *, UINT, void *);         // 72
    HRESULT (STDMETHODCALLTYPE *SetCurrentTexturePalette)(IDirect3DDevice9 *, UINT);          // 73
    HRESULT (STDMETHODCALLTYPE *GetCurrentTexturePalette)(IDirect3DDevice9 *, UINT *);        // 74
    HRESULT (STDMETHODCALLTYPE *SetScissorRect)(IDirect3DDevice9 *, const RECT *);            // 75
    HRESULT (STDMETHODCALLTYPE *GetScissorRect)(IDirect3DDevice9 *, RECT *);                  // 76
    HRESULT (STDMETHODCALLTYPE *SetSoftwareVertexProcessing)(IDirect3DDevice9 *, BOOL);       // 77
    BOOL    (STDMETHODCALLTYPE *GetSoftwareVertexProcessing)(IDirect3DDevice9 *);             // 78
    HRESULT (STDMETHODCALLTYPE *SetNPatchMode)(IDirect3DDevice9 *, float);                    // 79
    float   (STDMETHODCALLTYPE *GetNPatchMode)(IDirect3DDevice9 *);                           // 80
    HRESULT (STDMETHODCALLTYPE *DrawPrimitive)(IDirect3DDevice9 *, DWORD, UINT, UINT);        // 81
    HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitive)(IDirect3DDevice9 *, DWORD, INT, UINT, UINT, UINT, UINT); // 82
    HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP)(IDirect3DDevice9 *, DWORD, UINT, const void *, UINT); // 83
    HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitiveUP)(IDirect3DDevice9 *, DWORD, UINT, UINT, UINT, const void *, DWORD, const void *, UINT); // 84
    HRESULT (STDMETHODCALLTYPE *ProcessVertices)(IDirect3DDevice9 *, UINT, UINT, UINT, void *, void *, void *, DWORD); // 85
    HRESULT (STDMETHODCALLTYPE *CreateVertexDeclaration)(IDirect3DDevice9 *, const void *, void **); // 86
    HRESULT (STDMETHODCALLTYPE *SetVertexDeclaration)(IDirect3DDevice9 *, void *);            // 87
    HRESULT (STDMETHODCALLTYPE *GetVertexDeclaration)(IDirect3DDevice9 *, void **);           // 88
    HRESULT (STDMETHODCALLTYPE *SetFVF)(IDirect3DDevice9 *, DWORD);                           // 89
    HRESULT (STDMETHODCALLTYPE *GetFVF)(IDirect3DDevice9 *, DWORD *);                         // 90
    HRESULT (STDMETHODCALLTYPE *CreateVertexShader)(IDirect3DDevice9 *, const DWORD *, void **); // 91
    HRESULT (STDMETHODCALLTYPE *SetVertexShader)(IDirect3DDevice9 *, void *);                 // 92
    HRESULT (STDMETHODCALLTYPE *GetVertexShader)(IDirect3DDevice9 *, void **);                // 93
    HRESULT (STDMETHODCALLTYPE *SetVertexShaderConstantF)(IDirect3DDevice9 *, UINT, const float *, UINT); // 94
    HRESULT (STDMETHODCALLTYPE *GetVertexShaderConstantF)(IDirect3DDevice9 *, UINT, float *, UINT); // 95
    HRESULT (STDMETHODCALLTYPE *SetVertexShaderConstantI)(IDirect3DDevice9 *, UINT, const INT *, UINT); // 96
    HRESULT (STDMETHODCALLTYPE *GetVertexShaderConstantI)(IDirect3DDevice9 *, UINT, INT *, UINT); // 97
    HRESULT (STDMETHODCALLTYPE *SetVertexShaderConstantB)(IDirect3DDevice9 *, UINT, const BOOL *, UINT); // 98
    HRESULT (STDMETHODCALLTYPE *GetVertexShaderConstantB)(IDirect3DDevice9 *, UINT, BOOL *, UINT); // 99
    HRESULT (STDMETHODCALLTYPE *SetStreamSource)(IDirect3DDevice9 *, UINT, void *, UINT, UINT); // 100
    HRESULT (STDMETHODCALLTYPE *GetStreamSource)(IDirect3DDevice9 *, UINT, void **, UINT *, UINT *); // 101
    HRESULT (STDMETHODCALLTYPE *SetStreamSourceFreq)(IDirect3DDevice9 *, UINT, UINT);         // 102
    HRESULT (STDMETHODCALLTYPE *GetStreamSourceFreq)(IDirect3DDevice9 *, UINT, UINT *);       // 103
    HRESULT (STDMETHODCALLTYPE *SetIndices)(IDirect3DDevice9 *, void *);                      // 104
    HRESULT (STDMETHODCALLTYPE *GetIndices)(IDirect3DDevice9 *, void **);                     // 105
    HRESULT (STDMETHODCALLTYPE *CreatePixelShader)(IDirect3DDevice9 *, const DWORD *, void **); // 106
    HRESULT (STDMETHODCALLTYPE *SetPixelShader)(IDirect3DDevice9 *, void *);                  // 107
    HRESULT (STDMETHODCALLTYPE *GetPixelShader)(IDirect3DDevice9 *, void **);                 // 108
    HRESULT (STDMETHODCALLTYPE *SetPixelShaderConstantF)(IDirect3DDevice9 *, UINT, const float *, UINT); // 109
    HRESULT (STDMETHODCALLTYPE *GetPixelShaderConstantF)(IDirect3DDevice9 *, UINT, float *, UINT); // 110
    HRESULT (STDMETHODCALLTYPE *SetPixelShaderConstantI)(IDirect3DDevice9 *, UINT, const INT *, UINT); // 111
    HRESULT (STDMETHODCALLTYPE *GetPixelShaderConstantI)(IDirect3DDevice9 *, UINT, INT *, UINT); // 112
    HRESULT (STDMETHODCALLTYPE *SetPixelShaderConstantB)(IDirect3DDevice9 *, UINT, const BOOL *, UINT); // 113
    HRESULT (STDMETHODCALLTYPE *GetPixelShaderConstantB)(IDirect3DDevice9 *, UINT, BOOL *, UINT); // 114
    HRESULT (STDMETHODCALLTYPE *DrawRectPatch)(IDirect3DDevice9 *, UINT, const float *, const void *); // 115
    HRESULT (STDMETHODCALLTYPE *DrawTriPatch)(IDirect3DDevice9 *, UINT, const float *, const void *); // 116
    HRESULT (STDMETHODCALLTYPE *DeletePatch)(IDirect3DDevice9 *, UINT);                       // 117
    HRESULT (STDMETHODCALLTYPE *CreateQuery)(IDirect3DDevice9 *, DWORD, void **);             // 118
};

struct IDirect3DDevice9 {
    IDirect3DDevice9Vtbl *lpVtbl;
};

// IDirect3D9Ex is a separate 21-slot interface: slots 0..16 match IDirect3D9 and
// slot 17 is CreateDeviceEx (there is no plain CreateDevice on the Ex vtable).
// It is spelled out in full rather than derived from IDirect3D9Vtbl so that the
// self parameter types stay exact and no casts are needed.
struct IDirect3DDevice9;
struct IDirect3DDevice9Ex;

// --------------------------------------------------------------- entry points
typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT sdk_version);
// HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex**) - resolved dynamically.
typedef HRESULT(WINAPI *PFN_Direct3DCreate9Ex)(UINT sdk_version, IDirect3D9Ex **out);

struct IDirect3D9ExVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3D9Ex *, const IID &, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDirect3D9Ex *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDirect3D9Ex *);
    HRESULT (STDMETHODCALLTYPE *RegisterSoftwareDevice)(IDirect3D9Ex *, void *);
    UINT    (STDMETHODCALLTYPE *GetAdapterCount)(IDirect3D9Ex *);
    HRESULT (STDMETHODCALLTYPE *GetAdapterIdentifier)(IDirect3D9Ex *, UINT, DWORD, void *);
    UINT    (STDMETHODCALLTYPE *GetAdapterModeCount)(IDirect3D9Ex *, UINT, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *EnumAdapterModes)(IDirect3D9Ex *, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE *);
    HRESULT (STDMETHODCALLTYPE *GetAdapterDisplayMode)(IDirect3D9Ex *, UINT, D3DDISPLAYMODE *);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceType)(IDirect3D9Ex *, UINT, DWORD, D3DFORMAT, D3DFORMAT, BOOL);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceFormat)(IDirect3D9Ex *, UINT, DWORD, D3DFORMAT, DWORD, DWORD, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceMultiSampleType)(IDirect3D9Ex *, UINT, DWORD, D3DFORMAT, BOOL, DWORD, DWORD *);
    HRESULT (STDMETHODCALLTYPE *CheckDepthStencilMatch)(IDirect3D9Ex *, UINT, DWORD, D3DFORMAT, D3DFORMAT, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceFormatConversion)(IDirect3D9Ex *, UINT, DWORD, D3DFORMAT, D3DFORMAT);
    HRESULT (STDMETHODCALLTYPE *GetDeviceCaps)(IDirect3D9Ex *, UINT, DWORD, void *);
    HMONITOR(STDMETHODCALLTYPE *GetAdapterMonitor)(IDirect3D9Ex *, UINT);
    HRESULT (STDMETHODCALLTYPE *CreateDeviceEx)(IDirect3D9Ex *, UINT, DWORD, HWND, DWORD,
                                                D3DPRESENT_PARAMETERS *, void *, IDirect3DDevice9 **);
};

struct IDirect3D9Ex {
    IDirect3D9ExVtbl *lpVtbl;

    // The plain IDirect3D9 methods are the first 17 slots of the Ex vtable, so
    // an Ex factory can still be used to create a non-Ex device.
    IDirect3D9 *as_d3d9() { return reinterpret_cast<IDirect3D9 *>(this); }
};

// IDirect3DDevice9Ex is a 128-slot interface: the 119 IDirect3DDevice9 slots
// followed by the 9 Ex-only methods (CheckDeviceState .. WaitForVBlank).
// Only the shared prefix matters to us, so the Ex methods are not spelled out.
struct IDirect3DDevice9Ex {
    IDirect3DDevice9Vtbl *lpVtbl;
};

// ------------------------------------------------------------------- GUIDs
// Namespaced in a struct so the identifiers stay `IDirect3DDevice9` while the
// symbols remain valid C++ (the DirectX headers do this via macros).
struct DXGI_IID {
    static const IID IDirect3D9;
    static const IID IDirect3D9Ex;
    static const IID IDirect3DDevice9;
    static const IID IDirect3DDevice9Ex;
    static const IID IDirect3DTexture9;
    static const IID IDirect3DSurface9;
};

#define D3D9_IID(name) (::DXGI_IID::name)

// ----------------------------------------------------------------- textures
#define D3DPOOL_DEFAULT 0
#define D3DPOOL_SYSTEMMEM 2
#define D3DUSAGE_RENDERTARGET 0x00000001
#define D3DUSAGE_DYNAMIC      0x00000200

#define D3DBACKBUFFER_TYPE_MONO 0
#define D3DLOCK_READONLY 0x00000010
#define D3DLOCK_NOSYSLOCK 0x00000800

// D3DCLEAR flags. Getting these wrong is silent: passing the ZBUFFER bit to a
// device created without a depth buffer returns D3DERR_INVALIDCALL and leaves the
// colour buffer untouched, which looks exactly like a broken readback.
#define D3DCLEAR_TARGET  0x00000001
#define D3DCLEAR_ZBUFFER 0x00000002
#define D3DCLEAR_STENCIL 0x00000004

struct D3DLOCKED_RECT {
    INT  Pitch;
    void *pBits;
};
#define D3DTEXF_NONE 0
#define D3DTEXF_LINEAR 2
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000

struct D3DSURFACE_DESC {
    D3DFORMAT Format;
    DWORD     Type;
    DWORD     Usage;
    DWORD     Pool;
    DWORD     MultiSampleType;
    DWORD     MultiSampleQuality;
    UINT      Width;
    UINT      Height;
};

struct IDirect3DSurface9;
struct IDirect3DTexture9;

struct IDirect3DTexture9Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3DTexture9 *, const IID &, void **);   // 0
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDirect3DTexture9 *);                                 // 1
    ULONG   (STDMETHODCALLTYPE *Release)(IDirect3DTexture9 *);                                // 2
    HRESULT (STDMETHODCALLTYPE *GetDevice)(IDirect3DTexture9 *, IDirect3DDevice9 **);         // 3
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(IDirect3DTexture9 *, const IID &, const void *, DWORD, DWORD); // 4
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(IDirect3DTexture9 *, const IID &, void *, DWORD *); // 5
    HRESULT (STDMETHODCALLTYPE *FreePrivateData)(IDirect3DTexture9 *, const IID &);           // 6
    DWORD   (STDMETHODCALLTYPE *SetPriority)(IDirect3DTexture9 *, DWORD);                     // 7
    DWORD   (STDMETHODCALLTYPE *GetPriority)(IDirect3DTexture9 *);                            // 8
    void    (STDMETHODCALLTYPE *PreLoad)(IDirect3DTexture9 *);                                // 9
    DWORD   (STDMETHODCALLTYPE *GetType)(IDirect3DTexture9 *);                                // 10
    DWORD   (STDMETHODCALLTYPE *SetLOD)(IDirect3DTexture9 *, DWORD);                          // 11
    DWORD   (STDMETHODCALLTYPE *GetLOD)(IDirect3DTexture9 *);                                 // 12
    DWORD   (STDMETHODCALLTYPE *GetLevelCount)(IDirect3DTexture9 *);                          // 13
    HRESULT (STDMETHODCALLTYPE *SetAutoGenFilterType)(IDirect3DTexture9 *, DWORD);            // 14
    DWORD   (STDMETHODCALLTYPE *GetAutoGenFilterType)(IDirect3DTexture9 *);                   // 15
    void    (STDMETHODCALLTYPE *GenerateMipSubLevels)(IDirect3DTexture9 *);                   // 16
    HRESULT (STDMETHODCALLTYPE *GetLevelDesc)(IDirect3DTexture9 *, UINT, D3DSURFACE_DESC *);  // 17
    HRESULT (STDMETHODCALLTYPE *GetSurfaceLevel)(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **); // 18
    HRESULT (STDMETHODCALLTYPE *LockRect)(IDirect3DTexture9 *, UINT, void *, const RECT *, DWORD); // 19
    HRESULT (STDMETHODCALLTYPE *UnlockRect)(IDirect3DTexture9 *, UINT);                       // 20
    HRESULT (STDMETHODCALLTYPE *AddDirtyRect)(IDirect3DTexture9 *, const RECT *);             // 21
};

struct IDirect3DTexture9 {
    IDirect3DTexture9Vtbl *lpVtbl;

    ULONG Release() { return lpVtbl->Release(this); }
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9 **out) {
        return lpVtbl->GetSurfaceLevel(this, level, out);
    }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) {
        return lpVtbl->GetLevelDesc(this, level, desc);
    }
};

struct IDirect3DSurface9Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3DSurface9 *, const IID &, void **);   // 0
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDirect3DSurface9 *);                                 // 1
    ULONG   (STDMETHODCALLTYPE *Release)(IDirect3DSurface9 *);                                // 2
    HRESULT (STDMETHODCALLTYPE *GetDevice)(IDirect3DSurface9 *, IDirect3DDevice9 **);         // 3
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(IDirect3DSurface9 *, const IID &, const void *, DWORD, DWORD); // 4
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(IDirect3DSurface9 *, const IID &, void *, DWORD *); // 5
    HRESULT (STDMETHODCALLTYPE *FreePrivateData)(IDirect3DSurface9 *, const IID &);           // 6
    DWORD   (STDMETHODCALLTYPE *SetPriority)(IDirect3DSurface9 *, DWORD);                     // 7
    DWORD   (STDMETHODCALLTYPE *GetPriority)(IDirect3DSurface9 *);                            // 8
    void    (STDMETHODCALLTYPE *PreLoad)(IDirect3DSurface9 *);                                // 9
    DWORD   (STDMETHODCALLTYPE *GetType)(IDirect3DSurface9 *);                                // 10
    // Slot 11 exists in the runtime's surface objects but is not part of the
    // published interface. Measured rather than assumed: with GetDesc at 11 (as
    // d3d9.h documents) every "LockRect" call was really GetDesc, which writes 32
    // bytes into the caller's D3DLOCKED_RECT - that is what produced the NULL
    // pBits, the "structured exception" frame copies and the jumps to 0x16.
    // Verified with build\_copytest: slot 12 fills in Format/Type/Pool/Width/
    // Height and slot 15 returns an HDC. See PIT 10 in README.md.
    void    (STDMETHODCALLTYPE *Reserved11)(IDirect3DSurface9 *);                             // 11
    HRESULT (STDMETHODCALLTYPE *GetDesc)(IDirect3DSurface9 *, D3DSURFACE_DESC *);             // 12
    HRESULT (STDMETHODCALLTYPE *LockRect)(IDirect3DSurface9 *, void *, const RECT *, DWORD);  // 13
    HRESULT (STDMETHODCALLTYPE *UnlockRect)(IDirect3DSurface9 *);                             // 14
    HRESULT (STDMETHODCALLTYPE *GetDC)(IDirect3DSurface9 *, HDC *);                           // 15
    HRESULT (STDMETHODCALLTYPE *ReleaseDC)(IDirect3DSurface9 *, HDC);                         // 16
};

struct IDirect3DSurface9 {
    IDirect3DSurface9Vtbl *lpVtbl;

    ULONG Release() { return lpVtbl->Release(this); }
    HRESULT GetDesc(D3DSURFACE_DESC *desc) { return lpVtbl->GetDesc(this, desc); }
};

#define D3D9_VTABLE_INDEX_PRESENT 17
