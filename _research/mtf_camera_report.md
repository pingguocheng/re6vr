# MT Framework 2.x — Camera & Transform internal representation
## A source-level research report (primary sources only)

**Method / access notes.** `raw.githubusercontent.com` is unreachable from this sandbox, and `grep.app`, `sourcegraph.com`, `web.archive.org`, `nexusmods.com`, `fearlessrevolution.com` and `gamefaqs` all refused or timed out. `api.github.com` works, and plain-text retrieval of any GitHub file (including `.hpp` and wiki `.md`) works through the proxy:

```
https://ghproxy.net/https://raw.githubusercontent.com/<owner>/<repo>/<branch>/<path>
https://ghproxy.net/https://raw.githubusercontent.com/wiki/<owner>/<repo>/<Page>.md
```

Everything below marked **VERIFIED** was read in a primary source (quoted verbatim where useful). Everything marked **INFERENCE** is my own reasoning. Explicit negative results are called out rather than guessed at.

---

## 1. Repository inventory — what actually exists

| Repo | What it is | Game / engine | Status |
|---|---|---|---|
| [`muhopensores/dmc4_hook`](https://github.com/muhopensores/dmc4_hook) | Full REFramework-architecture trainer, C++, DX9, with SDK reversal, free camera, photo mode, camera parameter mods | **DMC4 DX9 = MT Framework 1.x** | Source fully read — **the single most valuable source found** |
| [`muhopensores/ReClass.NET-MtFrameworkPlugin`](https://github.com/muhopensores/ReClass.NET-MtFrameworkPlugin) | ReClass.NET plugin that resolves MT Framework class names from a live process | any x86 MTF | Source read — gives the runtime type-name algorithm |
| [`Fexty12573/SharpPluginLoader`](https://github.com/Fexty12573/SharpPluginLoader) | C# plugin loader for MHW with explicit `Camera` / `Viewport` / `CameraSystem` bindings | **MHW = MT Framework 2.0 / "World Engine"** | Source read — best-documented MTF 2.x camera |
| [`Andoryuuta/MHW-DTI-Dumps`](https://github.com/Andoryuuta/MHW-DTI-Dumps) + [`MHW-ClassPropDump`](https://github.com/Andoryuuta/MHW-ClassPropDump) | Dumped DTI (runtime type database) for MHW as a C++ header | MHW 2.0 | Repo confirmed; API rate-limited before I could pull the dump file itself |
| [`Ezekial711/MonsterHunterWorldModding` wiki](https://github.com/Ezekial711/MonsterHunterWorldModding/wiki/The-DTI-and-MtFramework-2.0) | "The DTI and MtFramework 2.0" — class prefixes, DTI layout, vtable layout | MHW 2.0 | Read in full |
| [`Wildenhaus/MtWest`](https://github.com/Wildenhaus/MtWest) | "A 64-bit reverse engineering project for Dead Rising, which runs on the MtFramework Engine" | Dead Rising (MTF) | README only; no usable code (repo ~15 KB) |
| [`PredatorCZ/RevilLib`](https://github.com/PredatorCZ/RevilLib) | MT Framework + RE Engine **file-format** library | both | Formats only — no runtime camera |
| [`muhopensores/dmc2camhack`](https://github.com/muhopensores/dmc2camhack), [`dmc3-inputs-thing`](https://github.com/muhopensores/dmc3-inputs-thing), [`dmc5perfmod`](https://github.com/muhopensores/dmc5perfmod), [`dx11_mod_base`](https://github.com/muhopensores/dx11_mod_base) | camera/input mods for DMC2/DMC3 PC ports; DMC5 perf mod | **DMC2/DMC3 PC ports are NOT MT Framework; DMC5 is RE Engine** | Not applicable |

**Explicit negatives (searched, no result):**
- No repo named `MT-Framework-SDK`, `via-engine`, `MTFramework-Reversed`, `RE6Hook`, or `MTFUnpacker`. GitHub repo search for `re6 hook OR RE6Hook OR dmc4 OR mtframework camera` returned `"total_count":0`.
- **No MT Framework work by praydog.** REFramework is RE Engine (RE2/RE3/DMC5/MHW-rise era). `dmc4_hook` says it is "based on reframework" — meaning the *SDK/loader architecture* (minhook + glm + Dear ImGui + `utility::FunctionHook`/`Scan`/`Pattern`, `Mod`/`Mods` class model), not MT Framework game support inherited from praydog.
- **No MT Framework camera work by alphazolam** (14 repos enumerated, all RE Engine: `fmt_RE_MESH-Noesis-Plugin`, `EMV-Engine`, `RE_RSZ`, `MMDK`, `Motlist-Tool`, `Skill-Maker`, …).
- **Ekey / tunip3: no result** in any source reachable. Nothing by them on GitHub under those names; their RE5/RE6 trainers don't appear to be open source.
- **No MT Framework VR mod exists.** I found none for RE5, RE6, DMC4, Lost Planet 1/2, Dragon's Dogma, MH3 or MHW.

---

## 2. `via::` namespace — **NO RESULT, treat as unverified**

I could **not** find a single primary source that uses `via::cCamera`, `via::Camera`, `via::cTransform`, `via::Transform`, or `via::mtx`. Specifically:

- `muhopensores/dmc4_hook` contains **no occurrence of `via`** at all. Its SDK uses the game-specific namespace `uActorMain::` (from a ReClass dump), plus engine-generic names `MtObject`, `MtDTI`, `CUnit`/`cUnit`, `CSystem`, `sRender`, `sCamera`, `uActor`, `UCoord`/`uCoord`, `MtMatrix`, `MtVector3/4`.
- The only `via` hit anywhere was a `vectree.io` PDF titled "Platform Abstraction Layer" (not fetchable).

Conclusion: **`via::` is plausible as the internal Capcom namespace but is unsupported by any source I could reach.** Do not build on it. The naming that *is* attested by primary sources is the `Mt*` prefix (engine-wide), the `u`/`c`/`s`/`r`/`a`/`n` class prefixes (game-side), and `sXxx` for singletons.

---

## 3. The camera class

### 3.1 Base engine camera — `uCamera` (VERIFIED, DMC4 / MTF 1.x)

`src/sdk/uActor.hpp` in dmc4_hook:

```cpp
struct uCamera {
    struct cUnit base;
    float mFarPlane;
    float mNearPlane;
    float mAspect;
    float mFov;
    long padding28[2];
    MtVector3 mCameraPos;
    MtVector3 mCameraUp;
    MtVector3 mTargetPos;
    MtVector4 mFrustum[6];
};

struct uFreeCamera {                    // engine's OWN debug/free camera
    struct uCamera uCameraBase;
    struct uCoord* mpParent;
    long mParentNo;
    struct uCoord* mpTarget;
    long mTargetNo;
    long mControlPad;
    char paddingd4[12];
    MtVector3 mControlSpeed;
};
```

**Key structural fact: `uCamera` stores near/far/aspect/FOV + position + up + target. It stores NO matrix.** The projection/view matrix is *computed*.

### 3.2 MT Framework 2.0 confirmation with explicit offsets (VERIFIED, MHW)

`SharpPluginLoader.Core/View/Camera.cs` — comment: *"Represents an instance of a uCamera class."*

```csharp
public class Camera : Unit
{
    public ref Vector3 Position    => ref GetRef<Vector3>(0x150);
    public ref Vector3 Up          => ref GetRef<Vector3>(0x160);
    public ref Vector3 Target      => ref GetRef<Vector3>(0x170);
    public ref float   FarClip     => ref GetRef<float>(0x138);
    public ref float   NearClip    => ref GetRef<float>(0x13C);
    public ref float   AspectRatio => ref GetRef<float>(0x140);
    public ref float   FieldOfView => ref GetRef<float>(0x144);   // radians

    // The matrices are VIRTUAL FUNCTIONS on the camera:
    public unsafe Vector3 GetTargetWorld()      { ... GetVirtualFunction(33) ... }
    public unsafe Matrix4x4 GetViewMatrix()      { ... GetVirtualFunction(34) ... }
    public unsafe Matrix4x4 GetProjectionMatrix(){ ... GetVirtualFunction(35) ... }
}
```

`SharpPluginLoader.Core/CameraSystem.cs`:

```csharp
/// Exposes functionality related to the sMhCamera singleton.
public static class CameraSystem
{
    public static MtObject SingletonInstance => SingletonManager.GetSingleton("sMhCamera")!;
    public static Viewport MainViewport => GetViewport(0);
    public static Viewport GetViewport(int index)
    {
        Ensure.IsTrue(index is >= 0 and < 8);
        return SingletonInstance.GetInlineObject<Viewport>(0x50 + index * 0x1A0);
    }
}
```

`SharpPluginLoader.Core/View/Viewport.cs`:

```csharp
public class Viewport : MtObject
{
    public Camera? Camera                 => GetObject<Camera>(0x8);
    public ref bool Visible               => ref GetRef<bool>(0x20);
    public ref Rectangle Region           => ref GetRef<Rectangle>(0x28);
    public ref Matrix4x4 ViewMatrix       => ref GetRef<Matrix4x4>(0xA0);   // <== THE VIEW MATRIX
    public ref Matrix4x4 ProjectionMatrix => ref GetRef<Matrix4x4>(0xE0);   // <== THE PROJECTION MATRIX
    public ref Matrix4x4 PrevViewMatrix   => ref GetRef<Matrix4x4>(0x120);
    public ref Matrix4x4 PrevProjectionMatrix => ref GetRef<Matrix4x4>(0x160);

    public bool WorldToScreen(Vector3 worldPosition, out Vector2 screenPos)
    {
        Vector4 worldPos = new(worldPosition, 1.0f);
        var viewPos = Vector4.Transform(worldPos, ViewMatrix);
        var clipPos = Vector4.Transform(viewPos, ProjectionMatrix);   // row-vector convention
        if (clipPos.W < (Camera?.NearClip ?? 0.01f)) { screenPos = default; return false; }
        var ndcPos = clipPos / clipPos.W;
        ...
    }
}
```

**=> This is the crux answer, corroborated independently for MTF 1.x and 2.0:**
1. The camera holds *look-at parameters*, and the engine computes view/projection via virtual functions.
2. The **final matrices that reach the GPU live on the VIEWPORT**, a per-screen object, in a `sXxxCamera` singleton.
3. `sMhCamera` (MTF 2.0) viewport stride is `0x1A0`, first viewport at singleton `+0x50`, 8 viewports. DMC4 (MTF 1.x) stride is `0x590`, viewports at `sCamera+0x30`. **Offsets differ between engine generations — do not port.**

### 3.3 DMC4 (MTF 1.x) `sCamera` and the per-viewport matrices (VERIFIED struct, derived offsets)

`src/sdk/Cam.hpp`:

```cpp
struct sCamera_ViewPort {
    void* vtable;
    void* mpCamera;               // 0x04  -> uCamera*
    void* mpTestCamera;           // 0x08
    uint32_t mAttr;               // 0x0C
    uint8_t mActive;              // 0x10
    uint8_t mNo;
    uint8_t mSceneNo;
    uint8_t mMode;                // 0x13  (REGION_MODE: FULLSCREEN etc.)
    MtRect mRegion;               // 0x14
    char padding0[12];
    struct uActorMain::MtVector4 mFrustum[6];   // 0x30
    MtColor mClearColor;
    float mClearZ;
    uint32_t mClearStencil;
    void* mpRenderTarget;
    void* mpDepthStencil;
    void* mpSubPixelMask;
    char padding1[8];
    MtMatrix mViewMat;            // 0xB0
    MtMatrix mProjMat;            // 0xF0
    MtMatrix mPrevViewMat;        // 0x130
    MtMatrix mPrevProjMat;        // 0x170
    float mFogStart; ...          // 0x1B0 ..
    /* fog, UknVecs, mTransFog*, then: */
    MtMatrix mTransViewMat[2];
    MtMatrix mTransProjMat[2];
    MtMatrix mTransViewProjMat[2];
    MtMatrix mTransPrevViewMat[2];
    MtMatrix mTransPrevProjMat[2];
    MtMatrix mTransPrevViewProjMat[2];
    struct uActorMain::MtVector4 mTransEyePos[2];
};
static_assert(sizeof(sCamera_ViewPort) == 0x590);

struct sCamera : CSystem {
    float mSubPixelOfsX, mSubPixelOfsY, mViewSubFrame, mWorldSubFrame;
    sCamera_ViewPort viewports[8];   // 0x30, stride 0x590
    MtRect ScreenRect;
    float SceneSize[2];
    uint32_t mLayoutMode;
    uint32_t mPause;
};
```

Offsets `mViewMat=+0xB0` … are **my own arithmetic** on the declared field order, validated against the author's `static_assert(sizeof(...) == 0x590)` — self-consistent, but the author never printed the offsets.

Accessors, verbatim (both from dmc4_hook):

```cpp
// src/sdk/Cam.hpp, PhotoMode.cpp, DebugCam.cpp
static sCamera* get_sCamera() {
    uintptr_t sMain = 0x00E5574C;                                   // -> SDevil4Main*
    sCamera* ptr    = *(sCamera**)(*(uintptr_t*)sMain + 0x10358);   // SDevil4Main::s_camera
    return ptr;
}
// src/mods/ShaderEditor.cpp — D3D9 device
static sRenderStub** sRenderPtr = (sRenderStub**)0x00E552D8;        // -> sRender*
// sRenderStub { char pad[52]; IDirect3DDevice9* D3D9Device; };     // device @ +0x34
```

### 3.4 DMC4 gameplay camera `cCameraPlayer` — the "scalar parameters" camera (VERIFIED)

`src/sdk/ReClass_Internal.hpp`:

```cpp
class cCameraPlayer {
    char pad_0[0x10];
    Vector3f pos;         // 0x10
    char pad_1c[0x4];
    Vector3f lookat;      // 0x20
    char pad_2c[0x14];
    float nearClipPlane;  // 0x40
    char pad_44[0x90];
    float angle;          // 0xd4
    float distance;       // 0xd8
    float distanceLockon; // 0xdc
    char pad_e0[0x4];
    float FOV;            // 0xe4
    float FOVBattle;      // 0xe8
    char pad_ec[0x114];
    Matrix4x4 possibleMat1; // 0x200  // "buncha possibilities up til 230" (author's note)
}; static_assert(sizeof(cCameraPlayer) == 0x240);
```

And the containing controller:

```cpp
// "sMediator + D0 camera, had a more useable glm mat but less settable values"
class uCameraCtrl {
    char pad_0[0x1c];
    float nearClipPlane;   // 0x1c
    char pad_20[0x4];
    float FOV;             // 0x24
    char pad_28[0x8];
    Vector3f pos;          // 0x30
    char pad_3c[0x4];
    Vector3f up;           // 0x40
    char pad_4c[0x4];
    Vector3 lookat;        // 0x50
    char pad_60[0x154];
    Matrix4x4 possibleMat5;      // 0x1b0
    char pad_1f0[0x2a0];
    cCameraPlayer* cCameraPlayer1; // 0x490
}; static_assert(sizeof(uCameraCtrl) == 0x494);

class SMediator { ... class uCameraCtrl* camera1; // 0xd0 ... };  // size 0x878
```

The author's own comments are worth noting: `uCameraCtrl` "had a more useable glm mat but less settable values"; the `possibleMat*` fields are unidentified matrices. **INFERENCE:** `possibleMat5 @0x1b0` is very likely the game's view matrix for that camera, but nobody has confirmed it — DMC4Hook deliberately does **not** use it and instead rebuilds the matrix with `glm::lookAt` from `pos`/`lookat`/`up`/`FOV`.

---

## 4. The transform class — VERIFIED

MT Framework's transform node is **`uCoord` / `UCoord`** (a `cUnit` subclass), and it is *not* named `Transform` in any source I found.

```cpp
// src/sdk/ReClass_Internal.hpp
class CUnit : public MtObject {
    union { uint32_t bitfield;
            struct { uint16_t pad0; uint8_t mTransMode; uint8_t mTransView; }; };  // 0x04
    class CUnit *mp_next_unit;   // 0x08
    class CUnit *mp_prev_unit;   // 0x0C
    float m_delta_time;          // 0x10
    char reserved_state_flags[4];// 0x14
}; static_assert(sizeof(CUnit) == 0x18);

class UCoord : public CUnit {
    class UCoord *mp_parent;   // 0x18
    uint32_t mParentNo;        // 0x1C  "attached entity's joint index"
    uint32_t mOrder;           // 0x20  "axis order"
    char pad_024_c[0xC];       // 0x24
    Vector3f m_pos;            // 0x30
    char pad_003_c[4];         // 0x3C
    Vector4  m_quat;           // 0x40   <== QUATERNION ROTATION
    Vector3f m_scale;          // 0x50
    char pad_05C_c[4];         // 0x5C
    Matrix4x4 m_lmat;          // 0x60   <== LOCAL MATRIX
    Matrix4x4 m_wmat;          // 0xA0   <== WORLD MATRIX
}; static_assert(sizeof(UCoord) == 0xE0);
```

The same layout appears byte-identically in `src/sdk/uActor.hpp` under `uActorMain::uCoord` (`mPos`, `mQuat`, `mScale`, `mLmat`, `mWmat`, total `0xe0`). **Two independent declarations agree => VERIFIED.**

Primitives:
```cpp
struct MtVector3 { float x, y, z; uint32_t padding; };  static_assert(sizeof == 0x10);  // 16-byte stride!
struct MtVector4 { float x, y, z, w; };                 static_assert(sizeof == 0x10);
struct MtMatrix  { struct MtVector4 vectors[4]; };      static_assert(sizeof == 0x40);
```

**Helper functions — the `updateLmat` / `updateWmat` pair (VERIFIED via the vtable dump):**
```cpp
class uActor {
    virtual void destructor();     // vtable 0x00
    virtual void getTypeInfo();    // 0x04
    virtual void ukn1();           // 0x08
    virtual void ukn2();           // 0x0C
    virtual void getDTI();         // 0x10
    virtual void setup();          // 0x14
    virtual void freeze();         // 0x18
    virtual void ukn4();           // 0x1C
    virtual void ukn5();           // 0x20
    virtual void render(void* mtrans); // 0x24
    virtual void ukn7(); ukn8();   // 0x28, 0x2C
    virtual void die();            // 0x30
    virtual void updateLmat();     // 0x34   <== local matrix from pos/quat/scale
    virtual void updateWmat();     // 0x38   <== world matrix = parent->mWmat * mLmat
    virtual void getJointMatrix(int jntInd); // 0x3C
    ...
};
```
`src/sdk/Devil4.hpp` exposes them as callable SDK functions: `uactor_sdk::updateLmat(void* obj)` and `uactor_sdk::updateWmat(void* obj)`.

There is **no `mtx_setup` / `mtx_multiply` / `via::mtx`** in any source I read. There is no publicly reversed MTF math library — dmc4_hook uses **glm** for all vector/matrix work and only uses `MtMatrix` for raw memory access.

### 4.1 Is there a global camera matrix written each frame?

**Yes — but it is per-viewport, not global, and it is written by the camera/render task, not by the camera object.** VERIFIED for both generations:
- DMC4: `sCamera::viewports[i].mViewMat` / `.mProjMat` (and `.mTransViewMat[2]`, `.mTransViewProjMat[2]` for split-screen).
- MHW: `sMhCamera` singleton → `Viewport @0x50 + i*0x1A0` → `ViewMatrix @0xA0`, `ProjectionMatrix @0xE0`.

`PrevViewMatrix`/`PrevProjMatrix` exist in both → the renderer keeps last frame's matrices (motion blur / TAA / reprojection).

---

## 5. How a mod actually rotates the camera — three techniques, all VERIFIED in dmc4_hook

### Technique A — overwrite the look-at vectors each frame (this is the direct answer)
`src/mods/DebugCam.cpp`. Hook at `module+0x519E60`, where the original instruction is `mov eax,[esi+0x000000C0]` (`esi` = the camera). The mod builds a view matrix in glm, applies rotations/translations, then **decomposes it back into the three vectors the engine actually consumes**:

```cpp
static void __stdcall freecam_mouse_input(uCamera* camera) {
    Vector3f cam_pos    = *(Vector3f*)&camera->mCameraPos;
    Vector3f cam_up     = *(Vector3f*)&camera->mCameraUp;
    Vector3f cam_lookat = *(Vector3f*)&camera->mTargetPos;
    glm::mat4 viewMatrix = glm::lookAt(cam_pos, cam_lookat, cam_up);

    // mouse: yaw about cam_pos, pitch about origin
    glm::mat4 rotateX = glm::rotate(glm::mat4(1.0f), (float)(y_diff * 0.001), glm::vec3(1,0,0));
    glm::mat4 rotateY = glm::rotate(glm::mat4(1.0f), (float)(x_diff * 0.001), glm::vec3(0,1,0));
    viewMatrix = glm::translate(viewMatrix, cam_pos);
    viewMatrix = viewMatrix * rotateY;
    viewMatrix = glm::translate(viewMatrix, -cam_pos);
    viewMatrix = rotateX * viewMatrix;

    glm::mat4 rotateZ = glm::rotate(glm::mat4(1.0f), 0.0f, glm::vec3(0,0,1));  // roll, Q/E => ±0.005
    viewMatrix = rotateZ * viewMatrix;
    // WASD/space/ctrl => translateMat; viewMatrix = translateMat * viewMatrix;

    // decompose back:
    glm::vec4 target = glm::inverse(viewMatrix) * glm::vec4(0.0f, 0.0f, -700.0f, 1.0f); // NOTE -700
    glm::vec4 up     = glm::inverse(viewMatrix) * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    glm::vec4 pos    = glm::inverse(viewMatrix) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    up = glm::normalize(up - pos);

    camera->mCameraPos = pos; camera->mTargetPos = target; camera->mCameraUp = up;
}
```
**=> To rotate an MT Framework camera you move `mTargetPos` (and `mCameraUp` for roll). You never write a matrix into the camera.**

### Technique B — spawn the engine's own `uFreeCamera` into a spare viewport (the cleanest path)
```cpp
static void* (__stdcall*freecam_cons)() = (void*(__stdcall*)())0x9197A0;   // uFreeCamera ctor (DMC4 DX9)
static void set_viewport(uint32_t index, REGION_MODE mode, uintptr_t CameraPtr) {
    sCamera* sCam = get_sCamera();
    sCamera_ViewPort* viewport = &sCam->viewports[index];
    viewport->mpCamera = (void*)CameraPtr;
    viewport->mMode    = mode;        // REGION_FULLSCREEN
    viewport->mAttr    = 0x17;
}
static void ToggleGameplayCam(bool enable) {
    uFreeCamera* cam = (uFreeCamera*)freecam_cons();
    if (DebugCam::freecamGamepadControls) cam->mControlPad = 0;
    devil4_sdk::spawn_or_something((void*)0x00E552CC, (MtObject*)cam, 0x17); // sUnit spawn, MoveLine 0x17
    sCamera_ViewPort* first_vp = get_viewport(0);
    set_viewport(1, REGION_FULLSCREEN, (uintptr_t)cam);
    cam->uCameraBase.mCameraPos  = ((uCamera*)*(uintptr_t*)&first_vp->mpCamera)->mCameraPos;
    cam->uCameraBase.mTargetPos  = ((uCamera*)*(uintptr_t*)&first_vp->mpCamera)->mTargetPos;
    cam->uCameraBase.mCameraUp   = ((uCamera*)*(uintptr_t*)&first_vp->mpCamera)->mCameraUp;
    sCamera_ViewPort* vp = get_viewport(0);
    vp->mActive = enable;   // hide gameplay cam; engine renders viewport[1] fullscreen
}
```
Used by both `DebugCam` and `PhotoMode`. **This gives you a fully engine-integrated second camera — correct culling, fog, HUD, post-filters.** For a stereo/VR or multi-view mod this is the mechanism to reuse: the engine already supports up to 8 viewports and split-screen regions (`REGION_TOPLEFT … REGION_BOTTOMRIGHT`).

### Technique C — hook the instruction that *reads* a scalar parameter
`src/mods/CameraSettings.cpp`. Every hook is `naked` asm that re-executes the original `movss` and adds a delta. Exhaustive table (DMC4 DX9 RVAs and the field each hook reads; VERBATIM from source):

| Feature | Hook RVA | Field |
|---|---|---|
| camera height | `0x0191C5` | `movss xmm0,[edi+0xD0]` |
| camera distance | `0x01946C` | `movss xmm0,[edi+0xE0]` |
| distance (lock-on) | `0x01A140` | `movss xmm0,[ebx+0xDC]` |
| camera angle | `0x01914C` | `movss xmm2,[edi+0xD4]` |
| angle (lock-on) | `0x0198E6` | `movss xmm0,[ebx+0xD4]` |
| FOV (in battle) | `0x0180EA` | `movss xmm0,[esi+0xE8]` |
| FOV | `0x018193` | `movss xmm0,[esi+0xE4]` |
| camera reset yaw | `0x02261A`, `0x022681` | writes `[esi+0x260] = [edx+0x1210] ± 1.57` (player facing ± 90°) |
| rotation sensitivity | `0x0225A4`, `0x0225BB`, `0x022575` | writes `[esi+0x268]` (camera yaw), multiplies by 2.0 |
| pause camera | `0xF960A` | NOPs a call |
| noclip cam | `0xF9318`, `0xF9334`, `0x180C1` | NOPs camera presets |
| disable camera events | `0x93BDE` | cutscene camera path |
| disable lock-on autocorrect | `0x1A402`, `0x1A27B` | |
| disable attack-towards-camera | `0x19514` | `jmp` patch |

Note `[esi+0x268]` (camera yaw, live) and `[esi+0x260]` (camera yaw target) live in a **different** struct than `cCameraPlayer::angle` (`+0xD4`): the sensitivity hook writes the controller's raw yaw, not `cCameraPlayer`. `NoclipCam`'s own help text: *"Remove camera presets and instead use a camera that can move through walls. Enable before entering a stage."*

---

## 6. DX9 and how the matrices reach the GPU

### 6.1 The device and the transform path (VERIFIED)
- `SRender : CSystem { char pad_0020[20]; IDirect3DDevice9* mp_device; // 0x0034 }`, `sizeof(SRender)==0x60`.
- `SDevil4Main { char pad_0000[66376]; SRender* s_render; // 0x10348 }`, `sizeof==0x10388`, reached via global `0x00E5574C`.
- dmc4_hook's own overlay draws with **fixed-function** D3D9: `SetPixelShader(nullptr); SetVertexShader(nullptr);` then `bd->pd3dDevice->SetTransform(D3DTS_PROJECTION, &view);` — i.e. it sets a combined view-projection into the fixed-function projection slot for its debug geometry. That is the mod's own path, not the game's.

### 6.2 `SetVertexShaderConstantF` — **NO RESULT**
I found **no** call to `SetVertexShaderConstantF` anywhere in dmc4_hook, and **no** documented MT Framework convention such as "c0-c3 = world, c4-c7 = view, c8-c11 = projection". MT Framework does per-material shader programs with its own constant tables; dmc4_hook's shader author states plainly: *"draw commands seem to use POD structs so no MTFramework DTIs"* and calls the shader-replacement approach *"basically useless."*

**Do not assume a fixed VS constant layout.** If you need the GPU-side matrices, hooking the camera/viewport (Technique A or B) is the supported route.

### 6.3 The one piece of hard evidence about MTF DX9 constant registers (VERIFIED, `src/mods/ShaderEditor.cpp`)
The repo embeds a **verbatim DMC4 `ps_3_0` pixel shader** (assembled with `D3DXAssembleShader`, injected via `CreatePixelShader`; the mod is `#ifdef DEVELOPER` and disabled). Its constant usage is the only concrete register evidence I found:

```asm
ps_3_0
    def c12, 0, 1, -1, -0
    def c13, 0.25, 0.5, 0, 0
    ...
    nrm_pp r0.xyz, v1
    dp3_pp r0.x, r0, -c7            ; <== c7 = camera/view FORWARD direction (facing term on the world normal)
    ...
    add r0.xyz, c0, -v0             ; <== c0 = camera world POSITION
    dp3 r0.x, r0, r0
    rsq r0.x, r0.x
    rcp r0.x, r0.x                  ;     |c0 - v0| = distance to camera
    add r0.x, r0.x, -c3.x
    mul r0.x, r0.x, c3.z            ;     c3.x = fog start, c3.z = fog density
    mad r1, v0.xyzx, c12.yyyx, c12.xxxy
    dp4 r0.y, c4, r1                ; <== c4 = fog PLANE equation
    mul r0.y, r0.y, c3.w
    max r1.x, r0.x, r0.y
    min r0.x, c5.w, r1.x            ;     c5.w = fog max
    mov r0.xyz, c6
    add r1.yzw, -r0.xxyz, c5.xxyz   ; <== c5.xyz / c6 = fog colours
    mad_pp r1.xyz, r1.x, r1.yzww, c6
    cmp_pp r0.xyz, -c2.x, r0, r1    ; <== c2.x = fog enable flag
    mov r1.y, c13.y
    mad_pp oC0.xyz, r0, r1.y, c1.x
    mov_pp r0.z, c6.w
    mul_pp oC0.w, r0.z, r1.x
    mad_sat_pp r0.w, v0.w, c10.x, c10.y   ; c9/c10/c11 = alpha fade thresholds
    mul_sat r0.x, r0.x, c11.x
```

**VERIFIED reading: `c0` = camera world position, `c7` = camera forward direction; `c2.x` = fog enable; `c3`/`c4`/`c5`/`c6` = fog parameters; `c9`/`c10`/`c11` = alpha/fade; `c12`/`c13` = shader-local.**
**Important caveat (INFERENCE-level confidence):** this is *one material's* pixel shader. The camera position/direction are supplied per-draw by the material system, not by a documented engine-wide register block. **Material-dependent, not engine-wide.**

---

## 7. The camera "task" / job system (your Q5)

MT Framework's scheduler is `sUnit` (singleton) holding **`MoveLine`**s (task/job lists):

```cpp
class MoveLine {
    void* vtable;       // 0x0
    char* mName;        // 0x4
    uint32_t mParallel : 1;   // 0x8
    uint32_t mPause    : 1;
    uint32_t mTrans    : 1;
    uint32_t mLineType : 6;
    uint32_t reserved  : 23;
    cUnit* mTop;        // 0xC
    cUnit* mBottom;     // 0x10
    float mDeltaTime;   // 0x14
}; static_assert(sizeof(MoveLine) == 0x18);

class sUnit : public CSystem { MoveLine mMoveLine[32]; };
static_assert(sizeof(sUnit) == 0x320);
```
`cUnit` carries `mp_next_unit` / `mp_prev_unit` / `m_delta_time` (+ `mTransMode`, `mTransView`), so each line is a doubly-linked list of units updated in order, with `mParallel` allowing parallel execution and `mDeltaTime` per line.

Supporting evidence that **cameras are units** — SharpPluginLoader `Unit.cs` doc comment (VERIFIED):
> *"A unit is basically an object that can act on its own (i.e. has an update method). It doesn't necessarily need to have a physical representation in the game world (e.g. **Schedulers, Cameras, Visual Filters are also units**)."*

…and `public class Camera : Unit` in the same codebase.

In dmc4_hook, cameras/lights/filters are spawned onto **MoveLine 17**: `sUnit_spawn(obj, 17)`, `s_unit->mMoveLine[17]`, iteration via `obj = obj->mp_next_unit`, and `viewport->mAttr = 0x17`.

**UNVERIFIED (explicit gap):** I found no source stating **which** MoveLine index the gameplay camera update runs on, nor an ordering guarantee that the camera task runs after the logic update. Your "camera task runs after logic" hypothesis is *consistent* with the design (`mParallel`, per-line `mDeltaTime`) but is **not documented anywhere I could reach**. Practical consequence: write camera state from a hook **inside the camera's own update** (which is what Technique A does) rather than from `Present`.

---

## 8. Locating the camera at runtime without hardcoded offsets — the MTF type database

This is the most portable lever available, and it is **engine-level, not game-level**.

### 8.1 `MtObject` / `MtDTI` (x86, VERIFIED from dmc4_hook's ReClass dump)
```cpp
class MtObject {                                    // sizeof == 0x04 (vtable only)
    virtual void vec_del_dtor(uint32_t i) {};       // vtable 0x00
    virtual void create_ui(void* prop) {};          // 0x04
    virtual bool is_enable_instance() {return 1;};  // 0x08
    virtual void create_property(void* prop) {};    // 0x0C
    virtual MtDTI* get_dti() { return (MtDTI*)0x00E5C5A8; }; // 0x10
};

class MtDTI {                                       // sizeof == 0x20
    /* +0x00 vtable */
    char *m_name;        // 0x04
    MtDTI *mp_next;      // 0x08
    MtDTI *mp_child;     // 0x0C
    MtDTI *mp_parent;    // 0x10
    MtDTI *mp_link;      // 0x14
    size_t m_size;       // 0x18
    uint32_t m_id;       // 0x1C
    virtual void vec_del_dtor(unsigned int x) {};
    virtual void* new_instance() {return nullptr;};
};
```

### 8.2 The exact runtime name-resolution algorithm (VERIFIED, `MtFrameworkNodeInfoReader.cs`)
```csharp
private static string ReadPtrInfo(IntPtr value, IRemoteMemoryReader process)
{
    var mtVtablePtr = value;
    if (mtVtablePtr.MayBeValid())
    {
        var getDtiPtr = process.ReadRemoteIntPtr(mtVtablePtr + 0x10);   // MtObject::get_dti() code address
        if (getDtiPtr.MayBeValid())
        {
            var dtiOffset = process.ReadRemoteIntPtr(getDtiPtr + 1);    // operand bytes of `mov eax, <abs32>`
            if (dtiOffset.MayBeValid())
            {
                var namePtr = process.ReadRemoteIntPtr(dtiOffset + 0x4);        // MtDTI::m_name
                if (namePtr.MayBeValid())
                {
                    var info = process.ReadRemoteStringUntilFirstNullCharacter(namePtr, Encoding.UTF8, 64);
                    if (info.Length > 0 && info[0].IsPrintable()) return info;
                }
            }
        }
    }
    return null;
}
```
i.e. **`*(char**)(*(uint32_t*)(*(uint32_t*)(obj) + 0x10) + 1) + 0x4)` → class-name string.** This works on *any* `MtObject*`, with no per-game offsets. It is exactly how you can find a `cCamera`/`sCamera`/`uCamera` in RE6 by name instead of guessing.

### 8.3 MTF 2.0 (x64 / MHW) DTI layout (VERIFIED from the wiki)
```asm
+0x00: VFT
+0x08: Class Name
+0x10: Next
+0x18: Child
+0x20: Parent
+0x28: Link
+0x30: Flags and Size/4
+0x38: CRC32 Hash
```
```cpp
+0x00: ~MtDTI<T>();
+0x08: T* NewInstance();
+0x10: T* CtorInstance(T* obj);
+0x18: T** CtorInstanceArray(T** arr, u32 count);
```
Class prefixes (VERIFIED, wiki): `u` = Unit Resource (inherits `cUnit`), `c` = Class, `n` = Namespace, `r` = Resource (inherits `cResource`), `s` = **Singleton**, `a` = Area (inherits `cArea`). The MHW camera singleton is literally named **`sMhCamera`** — the *game* name, not `sCamera`. Expect `sRe6Camera`/`sCamera`-style naming, discoverable only via the DTI.

Universal vtable prefix for every `MtObject` subclass (VERIFIED):
```cpp
+0x00: ~Class();
+0x08: void CreateGUI(MtProperty&);
+0x10: bool IsInitialized();
+0x18: MtDTI& GetDTI();
+0x20: void PopulatePropertyList(MtPropertyList&);
// class-specific virtuals therefore start at +0x28 (entry 6)
```
**Critical caveat on DTI limits (VERIFIED, wiki):** the DTI only contains what the developers registered by hand. Class name, size, vtable address, CRC32 and base classes are generally present; **field offsets often are not** (they show as `Offset:0x7FFFFFFFFFFFFFFF, PSEUDO-PROP` with a getter/setter address you must disassemble, e.g. `lea rax,[rcx+0xc]` → field at `+0xC`).

---

## 9. Known camera / free-cam / VR mods for MT Framework games

| Game | Engine | Camera mod? | Detail |
|---|---|---|---|
| **Devil May Cry 4 (DX9)** | MTF 1.x | **YES — full free camera + photo mode** | [`muhopensores/dmc4_hook`](https://github.com/muhopensores/dmc4_hook). Free cam (`DebugCam`), engine free-cam injection (`PhotoMode`), parameter mods (`CameraSettings`), noclip cam, cutscene camera disable. Hooks **functions/instructions**, not shader constants. See §5. |
| **Resident Evil 6** | MTF 2.x | Behaviour mods only — **no free camera, no published offsets** | [`nexusmods.com/residentevil6/mods/317`](https://www.nexusmods.com/residentevil6/mods/317) "Less Intrusive Cameras and Restrictive Movement" (page unreachable from here, incl. `?tab=docs`; referenced from [GameFAQs](https://gamefaqs.gamespot.com/boards/605604-resident-evil-6/80983769)). Cheat tables exist (e.g. URHEIM's advanced CT, Sept 2026, [residentevilmodding thread 21067](https://residentevilmodding.boards.net/thread/21067/resident-cheat-table-engine-required)) but they ship as compiled table binaries; **no source or offsets published**. |
| **Resident Evil 5 / RE5 Gold** | MTF 2.x | Trainer with camera options (wilsonso's "RE5 GE Ultimate Trainer", [nexusmods RE5GE mods/2](https://www.nexusmods.com/residentevil5goldedition/mods/2)) | Closed source; no offsets published. |
| **Resident Evil: Revelations** | MTF | **YES — FOV/camera-distance Cheat Engine script with AOBs** | See §10. |
| **Dragon's Dogma: Dark Arisen** | MTF 2.x | A "Free Camera" mod **exists** — but I could not read it | [`nexusmods.com/dragonsdogma/mods/1110`](https://www.nexusmods.com/dragonsdogma/mods/1110) titled "Free Camera". Nexus returns HTTP 403 (Cloudflare) to every proxy and direct attempt; archive.org, codetabs, allorigins, corsproxy all failed. **Could not verify its contents, mechanism, or author.** Separately, a DD:DA "advanced developer tools (dinput8.dll hooks)" mod exists but its page also 403s. **Do not assume it hooks the camera transform — unverified.** |
| **Monster Hunter: World** | MTF 2.0 / "World Engine" | **YES — FreeCam Plus** | [`nexusmods.com/monsterhunterworld/mods/2347`](https://www.nexusmods.com/monsterhunterworld/mods/2347) "FreeCam Plus (MHW CC Studio)". Page unreachable from here; existence confirmed via search. |
| **Dragon's Dogma Online** | MTF 2.x | FOV mod exists | [`nexusmods.com/dragonsdogmaonline/mods/26`](https://www.nexusmods.com/dragonsdogmaonline/mods/26) "Aim - Spell Cast - Skill Use FOV Increase". Unreachable; existence only. |
| **DMC5** | **RE Engine, not MTF** | (many camera mods) | Correctly excluded. |
| **DMC2 / DMC3 PC ports** | Not MTF | `dmc2camhack`, `dmc3-inputs-thing` exist | `dmc2camhack`'s own description: *"backup of lost dmc2 camhack source, unfinished and not supported atm"*. Not applicable to MTF. |
| **Any MTF game — VR** | — | **NO RESULT** | No MT Framework VR mod found for RE5, RE6, DMC4, Lost Planet 1/2, Dragon's Dogma, MH3 or MHW. |
| **Dragon's Dogma 2** | **RE Engine** | (REFramework mods, e.g. `alphazolam/Skill-Maker`) | Correctly excluded. |

**False lead, reported so you don't chase it:** [`xeavin.gitbook.io/free-camera`](https://xeavin.gitbook.io/free-camera) is a well-documented free camera with roll, FOV, frame-freeze and frame-advance — but its [Setup page](https://xeavin.gitbook.io/free-camera/getting-started/setup.md) shows it targets **Final Fantasy XII: The Zodiac Age** via Vortex + "External File Loader"/"LUA Loader", and its mod page is [`finalfantasy12/mods/514`](https://www.nexusmods.com/finalfantasy12/mods/514). **Not MT Framework.**

---

## 10. The "MT Framework AOB hooking" question — corrected

**There is no community-wide "MT Framework AOB" convention in open-source code.** What actually happens:

1. **Cheat Engine tables use AOB scans** because the user's exe varies. VERIFIED, your RE: Revelations lead is fully corroborated. From [residentevilmodding.boards.net/thread/12865/fov-fix](https://residentevilmodding.boards.net/thread/12865/fov-fix), picoleet's working script verbatim:
```
aobscan(cameraDistanceReadAOB,F3 0F 10 77 18 F3 0F 58 70 78)   // "rerev.exe"+8CC6C :
                                                                //   F3 0F 10 77 18 = movss xmm6,[edi+18]
                                                                //   F3 0F 58 70 78 = addss xmm6,[eax+78]
aobscan(cameraModeReadAOB,8B BE D8 00 00 00 8B CB)              // "rerev.exe"+8BAE3 : mov edi,[esi+000000D8]
...
chkladderclimbing:
    mov ecx,[pCameraMode]
    cmp byte ptr [ecx+dc],1        // camera-mode byte at [controller+0xDC]
...
fCustomCameraDistance: dd (float)-170.0
fCustomCameraInterval: dd (float)5.0
```
=> camera **distance** scalar at `[cameraObj+0x18]`; the read is hooked and `xmm6` replaced with a custom value. Camera controller pointer at `[esi+0xD8]`; camera-mode byte at `[controller+0xDC]`. **Exactly as you described.**
Bonus finding in the same thread: RE:R's FOV/far-plane is a **hardcoded immediate written into the camera object** — `c7 46 3c 00 00 70 42` = `mov dword [esi+0x3C], 0x42700000` (= 60.0f). picoleet patched that immediate via a code cave (`0x0fcbd50`: `d9 5e 3c` / `c7 46 3c 00 00 70 42` / `8b cb` / `e9 61 b3 4c ff`, then `jmp` at `0x4970bd`).

2. **Native DLL mods use fixed RVAs per exe build.** dmc4_hook has full `Pattern`/`Scan` AOB infrastructure (`utility::scan(module, pattern)` with `?` wildcards, `build_pattern`, `calculate_absolute`) — **but no camera/transform mod uses it.** All camera hooks are `install_hook_offset(ptrdiff_t, ...)` = `GetModuleHandle(0) + offset`, valid because DMC4 DX9 is a fixed-base 32-bit image (with SteamStub, which the loader waits out by polling `*(int*)0x00B84120 != 0xF6A005C7`).

3. **Which code regions are "safe" to hook:** from the evidence, the safe pattern is **the instruction that reads or writes a camera field inside the camera's own update function**, patched with a 5–12 byte detour that re-executes the original instruction and jumps back (`install_hook_offset(rva, hook, detour, &continue, next_instruction_offset)`). dmc4_hook's per-frame free camera hooks at `+0x519E60` (original `mov eax,[esi+0xC0]` — a camera getter called every frame). Hooking `Present` is explicitly *not* used for camera writes; it is only used for the ImGui frame.

---

## 11. Explicit "no result" list

- `via::` namespace, `via::cCamera`, `via::cTransform`, `via::mtx`: **no primary source.**
- `mtx_setup` / `mtx_multiply` / a reversed MTF math library: **no result.** dmc4_hook uses **glm**.
- `SetVertexShaderConstantF` in any MTF mod: **no result.** No documented c0–c3/c4–c7/c8–c11 convention.
- Which `MoveLine` index the camera update runs on, and whether it is guaranteed to run after logic: **no result.**
- RE6-specific camera struct offsets / AOB patterns: **no result** — RE6 mods are either closed-source CT binaries or behavioural mods. Anything claiming RE6 offsets should be treated as unverified until disassembled.
- Dragon's Dogma: Dark Arisen free camera mechanism: **no result** (page unreachable, not merely unread).
- MT Framework VR mod: **does not exist** as far as I can determine.
- `MT-Framework-SDK` / `via-engine` / `MTFramework-Reversed` / `RE6Hook` / `MTFUnpacker` repos: **do not exist.**
- praydog MT Framework camera work: **does not exist** (his MTF-adjacent relevance is only that dmc4_hook reuses REFramework's loader architecture).

---

## 12. Practical synthesis — how to locate/overwrite the MTF camera transform

1. **The camera is a look-at camera.** `uCamera` = `{near, far, aspect, fov, pos, up, target, frustum[6]}`. There is no writable matrix on it. Rotate by setting `target` (yaw/pitch) and `up` (roll).
2. **The matrices that reach the GPU live on the viewport**, in a `sXxxCamera` singleton: `viewports[i].mViewMat` / `.mProjMat` (+ `Prev*`, + `mTrans*` for split-screen). MTF 1.x (DMC4): `+0xB0`/`+0xF0`, stride `0x590`, viewports at singleton `+0x30`. MTF 2.0 (MHW): `+0xA0`/`+0xE0`, stride `0x1A0`, first viewport at singleton `+0x50`. **RE6 will be its own set — measure, don't port.**
3. **Find the camera by name, not by offset**, using `MtObject::get_dti()` (vtable `+0x10`) → `MtDTI::m_name` (`+0x4`), and walk `mp_parent` for base classes. The MHW singleton is `sMhCamera`, proof that the singleton name is game-specific and only discoverable through the DTI.
4. **The engine already has a free camera and up to 8 viewports.** `uFreeCamera` derives from `uCamera`; spawn it and point a spare viewport at it (`viewport->mpCamera`, `mMode = REGION_FULLSCREEN`, `mAttr = 0x17`, and set `viewports[0].mActive`). This is how DMC4Hook's photo mode works and it is the most robust route for a second/stereo view.
5. **The transform node is `uCoord`/`UCoord`**: `m_pos @0x30`, `m_quat @0x40`, `m_scale @0x50`, `m_lmat @0x60`, `m_wmat @0xA0`; after writing pos/quat/scale you must call the virtuals `updateLmat` (vtable `+0x34`) and `updateWmat` (`+0x38`) or the change will not propagate.
6. **Hook inside the camera's own update**, not `Present`. Cameras are `cUnit`s on a `MoveLine` in the `sUnit` scheduler, updated with per-line `mDeltaTime` and an `mParallel` flag.

---

## 13. URLs actually fetched and read

**dmc4_hook (`muhopensores/dmc4_hook`, branch `master`) — all via ghproxy.net plain text**
- `README.md`
- `src/Mod.hpp`
- `src/Mods.cpp`
- `src/ModFramework.cpp`
- `src/D3D9Hook.cpp`
- `src/GuiFunctions.cpp`
- `src/GuiFunctions.hpp`
- `src/utility/Address.hpp`
- `src/utility/Pattern.cpp`
- `src/utility/Scan.cpp`
- `src/utility/Memory.cpp`
- `src/sdk/ReClass.hpp`
- `src/sdk/ReClass_Internal.hpp` (full)
- `src/sdk/Cam.hpp`
- `src/sdk/uActor.hpp`
- `src/sdk/Devil4.hpp`
- `src/sdk/World2Screen.hpp`
- `src/sdk/World2Screen.cpp`
- `src/mods/CameraSettings.cpp`
- `src/mods/CameraSettings.hpp`
- `src/mods/DebugCam.cpp`
- `src/mods/PhotoMode.cpp`
- `src/mods/NoclipCam.cpp`
- `src/mods/DisableCameraEvents.cpp`
- `src/mods/PlayerRotation.cpp`
- `src/mods/ShaderEditor.cpp`
- `src/mods/ShaderEditor.hpp`
- wiki: `raw.githubusercontent.com/wiki/muhopensores/dmc4_hook/Home.md`

**ReClass plugin**
- `muhopensores/ReClass.NET-MtFrameworkPlugin`: `README.md`, `MtFrameworkPluginExt.cs`, `MtFrameworkNodeInfoReader.cs`

**MTF 2.0 / MHW**
- `Fexty12573/SharpPluginLoader`: `SharpPluginLoader.Core/View/Camera.cs`, `SharpPluginLoader.Core/CameraSystem.cs`, `SharpPluginLoader.Core/View/Viewport.cs`, `SharpPluginLoader.Core/Unit.cs`
- `https://fexty12573.github.io/SharpPluginLoader/API/SharpPluginLoader.Core.View.Camera`
- `https://fexty12573.github.io/SharpPluginLoader/API/Generated/SharpPluginLoader.Core.CameraSystem.html`
- `https://fexty12573.github.io/SharpPluginLoader/Development/GameObjects.html`
- `https://ghproxy.net/https://raw.githubusercontent.com/wiki/Ezekial711/MonsterHunterWorldModding/The-DTI-and-MtFramework-2.0.md`
- `https://ghproxy.net/https://raw.githubusercontent.com/Andoryuuta/MHW-DTI-Dumps/master/README.md`

**Community / reverse-engineering threads**
- `https://residentevilmodding.boards.net/thread/12865/fov-fix` (RE: Revelations FOV/distance; full AOB script read)
- `https://residentevilmodding.boards.net/thread/21067/resident-cheat-table-engine-required` (RE6 CT, Sept 2026)
- `https://xeavin.gitbook.io/free-camera`, `/home.md`, `/llms.txt`, `/getting-started/setup.md`, `/support-and-updates/faq.md` (identified as FF12, **not MTF**)

**GitHub API queries used**
- `api.github.com/search/repositories?q=MT+Framework+SDK&sort=stars`
- `api.github.com/search/repositories?q=MTFramework`
- `api.github.com/search/repositories?q=mtframework+in:name,description,readme&sort=stars&per_page=30`
- `api.github.com/search/repositories?q=re6+hook+OR+RE6Hook+OR+dmc4+OR+mtframework+camera` → `total_count: 0`
- `api.github.com/search/repositories?q=user:muhopensores`, `?q=user:alphazolam`
- `api.github.com/repos/muhopensores/dmc4_hook/contents/src`, `/src/sdk`, `/src/utility`, `/src/mods`
- `api.github.com/repos/muhopensores/ReClass.NET-MtFrameworkPlugin/git/trees/main?recursive=1`
- `api.github.com/repos/muhopensores/dmc4_hook/git/trees/master?recursive=1`

**Fetched and found unusable / empty for this question**
- `https://wildenhaus/MtWest` README (Dead Rising MTF project, README only)
- `https://github.com/PredatorCZ/RevilLib` (file formats only)
- `https://github.com/muhopensores/dmc4_hook` HTML (boilerplate-truncated)
- `https://github.com/search?q=%22via%3A%3AcCamera%22&type=code` (boilerplate-truncated)
- Blocked outright: `raw.githubusercontent.com` (direct), `grep.app` (429), `sourcegraph.com` (403), `web.archive.org`, `nexusmods.com` (403), `fearlessrevolution.com` (403), `gamefaqs.gamespot.com`, `steamcommunity.com`, `api.allorigins.win` (522), `api.codetabs.com` (503), `corsproxy.io` (401)
