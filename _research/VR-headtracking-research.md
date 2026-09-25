# VR 头部追踪如何旋转游戏相机 —— 源码级调研报告

> 目标：为一个 **OpenXR + 32 位 DirectX 9 + Capcom MT Framework 2.x（Resident Evil 6）** 的 VR 层，找出「正确地把 HMD 姿态注入游戏相机」的机制。
> 结论先说：**现有所有成熟 VR mod 都不用「hook 着色器常量并旋转矩阵」这条路**。REFramework、FreeCam、Vireio、dmc4_hook 全部走「写引擎自己的相机对象 / hook 相机更新函数」。
> 你观察到的「画面拉伸而不是旋转」在数学上是必然结果 —— 见第 4 节。

**验证等级标注约定**（全文遵守）：
- **[已验证]** = 我直接读了源码/文档原文，可引用原句。
- **[推断]** = 我基于已验证事实做的推理，未在源码中直接看到。
- **[未找到]** = 明确检索无结果，不要当成否定证据。

---

## 1. praydog/REFramework 是怎么做头部追踪的（RE Engine）

### 1.1 结论

REFramework 的头部追踪 **完全不碰着色器常量**。我做了一次全文 grep：对 `src/mods/FirstPerson.cpp`（84 KB，实际承载头部追踪的文件）搜索
`SetVertexShaderConstantF|ConstantBuffer|SetVSConstant|SetPixelShaderConstant|SetShaderConstant` → **0 命中** [已验证]。

它做的是：**hook 引擎自己的 transform 更新函数，然后在引擎更新完之后直接覆写相机对象（`via.Transform`）的 position / angles(四元数) / worldTransform 3×4 矩阵，并同步写 camera controller 的 worldPosition / worldRotation。**

### 1.2 hook 了哪些函数（`src/mods/Hooks.cpp`）[已验证]

| 目标 | 定位方式 | 回调 |
|---|---|---|
| `via::Transform::updateTransform`（原生） | AOB 扫描，6 条候选 pattern | `update_transform_hook(RETransform* t, uint8_t a2, uint32_t a3)` |
| `camera.PlayerCameraController::updateCameraPosition` | `sdk::find_native_method(...)`（仅 RE2/RE3） | `update_camera_controller_hook` |
| `camera.TwirlerCameraControllerRoot::update` | 同上（仅 RE2/RE3） | `update_camera_controller2_hook` |
| `via.SceneView::get_Size` | 在 getter 内做二次 pattern 扫描 | `view_get_size_hook` → 把渲染目标尺寸伪造成 HMD 分辨率 |
| `via.Camera::get_ProjectionMatrix` | 同上 | `camera_get_projection_matrix_hook` → 用 HMD 的逐眼投影矩阵覆盖 |
| `via.Camera::get_ViewMatrix` | 同上 | `camera_get_view_matrix_hook` |
| `via.render.EntityRenderer::updateBeforeLockScene`（TDB<74） | native method | — |
| Scene / PostEffect / Overlay 渲染层的 draw+update | 伪造实例取 vtable | VR 提交用 |
| `via.Application` 的全部 entry（`UpdateBehavior` `LateUpdateBehavior` `UpdateMotion` `LockScene` `BeginRendering` …） | 直接改写 `entry->func` 指针 | 帧序控制 |
| `via.gui` 的 GUI draw | AOB | — |

`UpdateTransform` 的定位注释（原文）：

```cpp
// Version 2 Dec 17th, 2019 (works on old version too) game.exe+0x1DD3FF0
// If this ever changes, get the singleton for via.SceneManager, find its
// constructor function, and look for the job function added near the end of the constructor
// UpdateTransform gets called near the end of the job, looks like this:
/*
  if ( *(_BYTE *)(v2 + 0x114) )
    UpdateTransform(v14, 0, v10);
  else
    sub_141DD4140(v14, 0i64, v10);
*/
...
spdlog::info("UpdateTransform: {:x}", update_transform);
// Can be found by breakpointing RETransform's worldTransform
m_update_transform_hook = std::make_unique<FunctionHook>(update_transform, &update_transform_hook);
```

注意 **"Can be found by breakpointing RETransform's worldTransform"** —— 作者定位相机/transform 的通用手法就是「在 Cheat Engine 里对 worldTransform 下断点，找谁在写它」。这条方法论对你找 RE6 的相机同样适用。

hook 包装本身（`on_pre_*` 在原始函数之前，`on_*` 在之后）：

```cpp
void* Hooks::update_transform_hook_internal(RETransform* t, uint8_t a2, uint32_t a3) {
    if (!g_framework->is_ready()) return original(t, a2, a3);
    auto& mods = g_framework->get_mods()->get_mods();
    for (auto& mod : mods) mod->on_pre_update_transform(t);
    auto ret = m_update_transform_hook->get_original<...>()(t, a2, a3);
    for (auto& mod : mods) mod->on_update_transform(t);
    return ret;
}
```

### 1.3 HMD 姿态怎么和游戏相机旋转合成（`FirstPerson::update_camera_transform`）[已验证]

这是核心函数，由 `FirstPerson::on_update_transform()` 在**引擎更新完相机 transform 之后**调用（`if (transform == CAMSYS(m_camera_system, mainCamera)->get_game_object()->get_transform())`）。

合成 HMD 旋转的那一行：

```cpp
auto& mtx = transform->get_world_transform();
...
auto cam_rot_mat = glm::extractMatrixRotation(Matrix4x4f{ m_last_controller_rotation });   // 游戏自己的相机旋转
auto head_rot_mat = glm::extractMatrixRotation(m_last_bone_matrix) * Matrix4x4f{ -1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,0,0,1 };
...
const auto real_headset_rotation = vr->get_rotation(0);
...
if (vr->is_hmd_active()) {
    const auto last_headset_rotation = !is_player_in_control ? m_last_headset_rotation_pre_cutscene : glm::identity<Matrix4x4f>();
    const auto inverse_last_headset_rotation = !is_player_in_control ? glm::inverse(m_last_headset_rotation_pre_cutscene) : glm::identity<Matrix4x4f>();
    const auto headset_rotation = inverse_last_headset_rotation * real_headset_rotation;

    if (is_first_person_allowed()) {
        final_mat *= headset_rotation;   // <<< 后乘 HMD 旋转到相机旋转矩阵
        vr->recenter_view();             // only affects third person/cutscenes
    } else {
        final_mat = vr->get_last_render_matrix();
    }
}
```

然后是**回写引擎对象**：

```cpp
if (is_first_person_allowed()) {
    camera_pos = Vector4f{ final_pos, 1.0f };
    CAMSYS(m_camera_system, cameraController)->worldPosition = *(Vector4f*)&camera_pos;
    CAMSYS(m_camera_system, cameraController)->worldRotation = *(Vector4f*)&final_quat;

    transform->get_position() = *(Vector4f*)&camera_pos;
    transform->get_angles()   = *(Vector4f*)&final_quat;   // 四元数

    *(Matrix3x4f*)&mtx = final_mat;                        // mtx = get_world_transform()
}
```

还有两处关键细节：

1. **用 `updateCamera` 布尔量阻止引擎覆盖你的值** [已验证]：
```cpp
// Lets camera modification work in cutscenes/action camera etc
if (!is_player_camera && !is_switching_camera && is_first_person_allowed()) {
    CAMSYS(m_camera_system, mainCameraController)->updateCamera = false;
} else {
    CAMSYS(m_camera_system, mainCameraController)->updateCamera = true;
}
```
以及注释：
```cpp
// These are what control the real rotation, so only set it in a cutscene or something
// If we did it all the time, the view would drift constantly
//CAMSYS(m_camera_system, cameraController)->pitch = m_last_controller_angles.x;
//CAMSYS(m_camera_system, cameraController)->yaw = m_last_controller_angles.y;
```
—— 这就是你要面对的「引擎每帧重算覆盖」问题，作者的正解是**同时把引擎的"请重算相机"标志关掉**。

2. **VR 模式下把相机 pitch 分量清零，交给头显控制俯仰** [已验证]：
```cpp
auto& cam_forward3 = *(Vector3f*)&cam_rot_mat[2];
// Zero out the Y component of the forward vector
// When using VR so only the user can control the up/down rotation with the headset
if (vr->is_hmd_active() && vr->is_using_controllers() && !is_paused) {
    cam_forward3[1] = 0.0f;
    cam_forward3 = glm::normalize(cam_forward3);
}
```

3. 还有 `sdk::set_joint_rotation(joint, m_last_camera_matrix)` / `set_joint_position`，把相机矩阵也写进相机的 joint 0 [已验证]。

### 1.4 引擎的相机对象长什么样（`shared/sdk/CameraSystemDispatch.hpp`）[已验证]

这是「camera object 是什么」的直接答案。RE Engine（`app::ropeway::camera::*`）：

```cpp
// RopewayPlayerCameraController
Vector4f pivotPosition;    // 0x60
Vector4f pivotRotation;    // 0x70
Vector4f worldPosition;    // 0x80
Vector4f worldRotation;    // 0x90  (四元数)
RECamera* activeCamera;    // 0xA8 (RE2) / 0xB8
REJoint*  joint;
RECameraParam* cameraParam;
TwirlerCameraSettings* cameraLimitSettings;
float pitch;               // 0x108
float yaw;                 // 0x10C

// RopewayMainCameraController
bool     updateCamera;                  // 0x51  <<< 引擎是否重算相机
Vector4f cameraObjectPosition;          // 0x60
Vector4f cameraObjectRotation;          // 0x70
Vector4f cameraPosition;                // 0x80
Vector4f cameraRotation;                // 0xA0  (RE3 多了 cameraDampingCameraPosition @0x90)
float    fov;                           // 0xB4
float    switchInterpolationTime;       // 0xC0
RECamera* mainCamera;                   // 0xE0

// RopewayCameraSystem
REGameObject* cameraGameObject;          // 0x90
RopewayPlayerCameraController* cameraController;      // 0xA0
RopewayMainCameraController*   mainCameraController;  // 0xF0
RECamera* mainCamera;                                 // 0xC8
```

宏 `CAMSYS(ptr, field)` / `CAMCTRL(ptr, field)` / `MAINCAM(ptr, field)` 只是处理 RE2/RE3/RE8 之间的偏移差异 —— 说明**同一套字段名跨游戏稳定，但偏移会变**。

### 1.5 FreeCam 也一样写 transform（`src/mods/FreeCam.cpp`）[已验证]

```cpp
transform->get_world_transform() = m_last_camera_matrix;
transform->get_position() = m_last_camera_matrix[3];

// IDK!!!
if (gi.tdb_ver() < 81) {
    if (joint != nullptr) {
        joint->posOffset = Vector4f{};
        *(Vector4f*)&joint->anglesOffset = Vector4f{0.0f, 0.00f, 0.0f, 1.0f};
    }
} else { ... sdk::set_joint_local_rotation(joint, ...); sdk::set_joint_local_position(joint, Vector4f{}); }
```
相机对象用 `sdk::get_primary_camera()` 获取；旋转用欧拉角数组 `m_custom_angles[0]/[1]` + `m_twist`，再转四元数。

### 1.6 立体渲染是另一条路，且和头部旋转无关 [已验证]

`src/mods/vr/D3D11Component.cpp` 的 `on_frame` 是**交替眼**方案：把 backbuffer 拷到左右眼纹理，`if (vr->m_render_frame_count % 2 == vr->m_left_eye_interval)` 决定这一帧算哪只眼，然后 `xrEndFrame` / `VRCompositor()->Submit`。**没有任何常量缓冲矩阵修补**。逐眼差异由 `via.Camera::get_ProjectionMatrix` hook 提供。

> 也就是说在 REFramework 里：**旋转（头部追踪）走"写相机对象"，投影（立体）走"hook 投影矩阵 getter"，两者都不走着色器常量缓冲的暴力矩阵替换。**

### 1.7 `VR.cpp` 里的 view/projection hook 具体做什么 [已验证]

`VR::on_camera_get_view_matrix()`（由 `via.Camera::get_ViewMatrix` 的 native hook 分发）：

```cpp
void VR::on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (result == nullptr || !g_framework->is_ready()) return;
    if (!is_hmd_active() || m_disable_view_matrix_override) return;
    if (camera != sdk::get_primary_camera()) return;
    auto& mtx = *result;
    //get the flipped eye to get the correct transform. something something right->left handedness i think
    const auto current_eye_transform = get_current_eye_transform(true);
    // Apply the complete eye transform. This fixes the need for parallel projections on all canted headsets like Pimax
    mtx = current_eye_transform * mtx;
}
```
==> **左乘一个"逐眼矩阵"到引擎返回的 view 矩阵上**。注意这是**逐眼（IPD/斜眼）偏移**，不是头部旋转；头部旋转已经在 1.3 的相机 transform 上做完了。这里也没有用 `m_eye_offsets`/`get_eye_transform`/`get_position(eye)` —— 逐眼矩阵是预算好的（OpenVR 下来自 `GetEyeToHeadTransform` / `GetProjectionMatrix(eye,nearz,farz)`，即**由 runtime 提供**）。

```cpp
void VR::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (result == nullptr || !g_framework->is_ready() || !is_hmd_active() || m_disable_projection_matrix_override) return;
    *result = get_current_projection_matrix(false);   // 注意：这里 camera != get_primary_camera() 的判断被注释掉了
}
```
逐眼选择：`get_current_eye_transform(bool flip)` / `get_current_projection_matrix(bool flip)` 按 `m_frame_count % 2 == (flip ? m_right_eye_interval : m_left_eye_interval)` 取 `runtime->eyes[Left|Right]` / `projections[Left|Right]`。

**着色器常量：确认没有。** 对 VR.cpp 已抓取到的 100 KB 区域 grep：`SetVertexShaderConstantF` / `VSSetConstantBuffers` / `Map(` / `D3D11_MAPPED_SUBRESOURCE` → 0 命中；`stereo`/`m_native_stereo`/`m_fake_stereo_hook`/`FFakeStereoRenderingHook` → 0 命中（后者是 UEVR 的术语，不在 REFramework 里）。
（限制说明：VR.cpp 全文 160 KB，抓取工具在 ~100 KB 处截断，最后 ~60 KB 未能读取，因此"完全不存在"严格说只对已读区域成立。但相机相关函数都在已读区域。）

`get_rotation(0)` = **原始 HMD 朝向**（索引 0 = HMD，不是眼睛）；recenter 是**独立的** `m_rotation_offset`，在使用点才应用：
```cpp
const auto rotation_offset = get_rotation_offset();
const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{get_rotation(0)});
...
void VR::recenter_view() {
    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));  // flatten = 只保留 yaw
    set_rotation_offset(new_rotation_offset);
}
```

帧序（`VR::update_hmd_state()`）：`if (synchronize_stage == EARLY) synchronize_frame(); update_poses(); update_matrices(nearz, farz);`，注释原文 *"Forcefully update the camera transform after submitting the frame ... if this is not done, the left eye will jitter a lot"*；OpenVR `synchronize_frame()` = `SetTrackingSpace(TrackingUniverseStanding)` + 阻塞 `WaitGetPoses(...)`；`VERY_LATE` 用在右眼 present 的末尾。

---

## 2. MT Framework 专属调研

### 2.1 有没有 MT Framework 的 VR / 头追踪 mod？ [未找到]

多轮检索结论：**没有公开的 MT Framework VR mod**，也**没有 MT Framework 的头部追踪实现**。找到的最接近的东西：

- **`muhopensores/dmc4_hook`** —— Devil May Cry 4（**DX9** 版）的训练器，README 原文：「A trainer for the original DX9 version of Devil May Cry 4 **based on [reframework](https://github.com/praydog/REFramework/)**, using [minhook], [glm], [Dear ImGui]」。**这是目前唯一一个「DX9 + MT Framework + REFramework 架构」的开源代码库**，也是最值得你逐文件读的参照物。
  注意：DMC4 是 **MT Framework 1.x**，RE5/RE6/LP2/MHW 是 2.x，**偏移不可直接搬**。
- `jaryn-kubik/ddda-dinput8`（Dragon's Dogma: Dark Arisen，DX9，MinHook + dinput8 代理）——同类架构的另一个参考。
- Dragon's Dogma 有 Nexus 上的 "Free Camera" mod（mods/1110），但 Nexus 对本次抓取返回 403，内容未读到 [未找到]。

### 2.2 相机表示：**它是 look-at 参数化相机，不是矩阵相机** [已验证]

DMC4（MTF 1.x）`src/sdk/uActor.hpp` 原文：

```cpp
struct uCamera {                 // 基础引擎相机类：pos/up/target + 视锥，没有矩阵
    struct cUnit base;
    float mFarPlane; float mNearPlane; float mAspect; float mFov;
    long padding28[2];
    MtVector3 mCameraPos;
    MtVector3 mCameraUp;
    MtVector3 mTargetPos;
    MtVector4 mFrustum[6];
};

struct uFreeCamera {             // 引擎自带的 debug/free camera 类
    struct uCamera uCameraBase;
    struct uCoord* mpParent; long mParentNo;
    struct uCoord* mpTarget; long mTargetNo;
    long mControlPad;
    char paddingd4[12];
    MtVector3 mControlSpeed;
};
```

MTF 2.0（Monster Hunter World，与 RE6 同代）的 C# 绑定（`Fexty12573/SharpPluginLoader`，**这是 MTF 2.x 相机最好的公开文档**）[已验证]：

```
Camera : Unit
  FarClip       0x138
  NearClip      0x13C
  AspectRatio   0x140
  FieldOfView   0x144   // 弧度
  Position      0x150
  Up            0x160
  Target        0x170
  注释原文："Represents an instance of a uCamera class."
```
并且 —— 关键 —— **view / projection 矩阵是 uCamera 的虚函数，不是存储字段** [已验证]：

```csharp
GetTargetWorld()       = GetVirtualFunction(33)
GetViewMatrix()        = GetVirtualFunction(34)   // NativeAction<nint,nint>，往出参写 Matrix4x4
GetProjectionMatrix()  = GetVirtualFunction(35)
```

> **即：引擎每帧从 pos/up/target/FOV 现算 view 矩阵。相机对象上没有 view 矩阵可供你"改写"。**

### 2.3 view / projection 矩阵真正的存放处：**viewport 对象** [已验证]

DMC4 `src/sdk/Cam.hpp` 原文：

```cpp
struct sCamera_ViewPort {
    void* vtable;
    void* mpCamera;
    void* mpTestCamera;
    uint32_t mAttr;
    uint8_t mActive; uint8_t mNo; uint8_t mSceneNo; uint8_t mMode;
    MtRect mRegion;
    char padding0[12];
    MtVector4 mFrustum[6];
    MtColor mClearColor; float mClearZ; uint32_t mClearStencil;
    void* mpRenderTarget; void* mpDepthStencil; void* mpSubPixelMask;
    char padding1[8];
    MtMatrix mViewMat;          // <<< 每 viewport 的 view 矩阵
    MtMatrix mProjMat;          // <<< 每 viewport 的 proj 矩阵
    MtMatrix mPrevViewMat;
    MtMatrix mPrevProjMat;
    float mFogStart; float mFogEnd; ... mFogPlaneDir; mFogColor;
    MtVector4 mTransFogFactor[2]; MtVector4 mTransFogColor[2];
    MtMatrix mTransViewMat[2];  MtMatrix mTransProjMat[2];  MtMatrix mTransViewProjMat[2];
    MtMatrix mTransPrevViewMat[2]; MtMatrix mTransPrevProjMat[2]; MtMatrix mTransPrevViewProjMat[2];
    MtVector4 mTransEyePos[2];
};
static_assert(sizeof(sCamera_ViewPort) == 0x590);

struct sCamera : CSystem {
    float mSubPixelOfsX, mSubPixelOfsY, mViewSubFrame, mWorldSubFrame;
    sCamera_ViewPort viewports[8];
    MtRect ScreenRect; float SceneSize[2];
    uint32_t mLayoutMode; uint32_t mPause;
};
```
作者自己写的辅助函数（`DebugCam.cpp`）[已验证语法/调用路径]：
```cpp
static sCamera* get_sCamera() {
    uintptr_t sMain = 0x00E5574C;
    sCamera* ptr   = *(sCamera**)(*(uintptr_t*)sMain + 0x10358);   // 二级指针链到 sCamera 单例
    return ptr;
}
static sCamera_ViewPort* get_viewport(uint32_t index) {
    return &get_sCamera()->viewports[index];
}
```
结构体成员偏移（**由结构体定义 + `static_assert` 推导，非原作者注释**）[推断，但自洽]：
`sCamera_ViewPort::mViewMat = +0xB0`、`mProjMat = +0xF0`、`mPrevViewMat = +0x130`、`mPrevProjMat = +0x170`；
`sCamera::viewports` 起始于 `sCamera + 0x30`，步长 `0x590`。

MHW（MTF 2.0）完全同构 [已验证]：
```
CameraSystem → SingletonManager.GetSingleton("sMhCamera")
GetViewport(i) => GetInlineObject<Viewport>(0x50 + i * 0x1A0), i ∈ [0,8)
Viewport { Camera* @0x8; bool Visible @0x20; Rectangle Region @0x28;
           Matrix4x4 ViewMatrix @0xA0; ProjectionMatrix @0xE0;
           PrevViewMatrix @0x120; PrevProjectionMatrix @0x160; }
WorldToScreen = Transform(Transform(worldPos, ViewMatrix), ProjectionMatrix)
```

### 2.4 MT Framework 的相机**控制器**：yaw/pitch 是标量字段 [已验证]

`dmc4_hook/src/mods/CameraSettings.cpp` 是决定性证据。它 hook **读取相机字段的那条指令**，用 naked asm detour 加偏移：

| 功能 | hook 的 RVA | 读/写的字段（原始 asm） |
|---|---|---|
| 相机高度 | `0x0191C5` | `movss xmm0,[edi+0xD0]` |
| 相机距离 | `0x01946C` | `movss xmm0,[edi+0xE0]` |
| 距离(锁定) | `0x01A140` | `movss xmm0,[ebx+0xDC]` |
| 相机角度 | `0x01914C` | `movss xmm2,[edi+0xD4]` |
| FOV | `0x018193` | `movss xmm0,[esi+0xE4]` |
| FOV(战斗) | `0x0180EA` | `movss xmm0,[esi+0xE8]` |
| 相机 yaw（灵敏度/顺逆时针/刹车） | `0x0225A4` / `0x0225BB` / `0x022575` | `movss [esi+0x268], xmm4`（**写入相机 yaw**） |
| 相机回正（键盘/手柄） | `0x02261A` / `0x022681` | `movss [esi+0x260], xmm0`（**yaw 目标** = 玩家朝向 `[edx+0x1210]` ± 1.57 rad） |

原句：
```cpp
naked void camera_sens_clockwise_proc(void) {
    _asm {
        cmp byte ptr [camera_sens_enabled], 0
        je code
        mulss xmm4, [double_camera_sens]
    code:
        movss [esi+0x00000268], xmm4      // <<< 相机 yaw
        jmp dword ptr [CameraSettings::camera_sens_clockwise_continue]
    }
}
```
```cpp
naked void camera_reset_keyboard_proc(void) {
    _asm {
        ...
        //camleft:
            movss xmm0, [edx+0x00001210]   // 玩家朝向
            subss xmm0, [degrees]          // 90 度左
            jmp retcode
        camright:
            movss xmm0, [edx+0x00001210]
            addss xmm0, [degrees]          // 90 度右
        retcode:
            jmp dword ptr [CameraSettings::camera_reset_keyboard_continue]
    }
}
```

> **这是 MT Framework 里"转动相机"的真正杠杆点：摄像头朝向是一个被引擎每帧重算的标量 yaw（+ pitch/angle），而不是一个矩阵。**
> 对头部追踪的含义：**找 `[camCtrl+0x268]` 这类 yaw 字段以及"读取它的那条指令"，在那里加上 HMD 的 yaw/pitch 增量** —— 这正是 `camera_sens_clockwise_proc` 的写法。

### 2.5 修改 MT Framework 的实际手法 [已验证]

1. **没有"MT Framework AOB"统一约定。** `dmc4_hook` 有完整的 pattern-scan 基础设施（`utility::Scan.cpp`、`utility::Pattern.cpp`、支持 `?` 通配），但**所有相机 mod 都用硬编码模块偏移** `install_hook_offset(0x519E60, ...)`（内部就是 `GetModuleHandle(0) + offset`），因为它基于固定基址的 32 位 Steam exe（先轮询 `*(int*)0x00B84120 != 0xF6A005C7` 等 SteamStub 解包完成）。AOB 主要出现在 **Cheat Engine 表**里。
2. **hook 的粒度是"某条读指令"而不是函数入口**，且 `install_hook_offset(rva, hook, &cont, 8)` 会拷贝 8 字节原始指令，detour 里手工执行再跳回。
3. 多级指针链从静态基址出发（`GetWorldPtr<T>({0x8C8, 0x70, i*4})`、`GetWorldPtr<T>({0x99C, 0x2710, 0x70, ...})`）。
4. D3D9 设备获取（`D3D9Hook.cpp`）：**不 hook vtable**，而是 inline hook 游戏自己的 present/reset 调用点（`present_fn=0x008F33F5`、`reset_fn=0x008F135A`），注释：「this is unfortunate i wanted to do it right this time but pc gaming had another opinion」。设备指针 `*(SRender**)0x00E552D8` → `IDirect3DDevice9* @ SRender+0x34`。

### 2.6 MT Framework 有内建反射系统（DTI）——找相机的最可靠办法 [已验证]

MTF 2.0 的 exe 内嵌 **DTI（Data Type Information）**：类名、大小、继承、**字段名 + 偏移 + 类型**、CRC32、虚表地址。类名前缀约定：

```
u = Unit Resource (继承 cUnit)
c = Class
n = Namespace
r = Resource (继承 cResource)
s = Singleton
a = Area (继承 cArea)
```
基类是 `MtObject`。每个 MTF 对象的首 5 个虚函数固定为：`~Class()` / `CreateGUI` / `IsInitialized` / `GetDTI` / `PopulatePropertyList`，因此**类特有虚函数从 vtable 偏移 0x28 起**。DTI 实例布局：`+0x00 VFT, +0x08 ClassName, +0x10 Next, +0x18 Child, +0x20 Parent, +0x28 Link, +0x30 Flags&Size/4, +0x38 CRC32`。

公开工具：`Andoryuuta/MHW-ClassPropDump`（dumper，`src/DTIDumper.cpp` / `src/Mt.hpp`）、dump 仓库 `Andoryuuta/MHW-DTI-Dumps`。
`src/Mt.hpp` 给出了 `MtDTI`(size 0x38，**64 位**) / `MtProperty`(0x58) / `MtPropertyList`(0x20) / `MtDTIHashTable`(256 桶，0x800) 的完整定义，以及 `PropType` 枚举：
```
matrix44 = 0x13, quaternion = 0x16, vector4 = 0x15, float3x3 = 0x25,
float4x3 = 0x26, float4x4 = 0x27, vector3 = 0x14, float3x4 = 0x3B, ...
```
[未找到] 我没能验证 **RE6 的 exe 是否含 DTI**（这是 64 位 MHW dumper，32 位需要改结构体宽度）。若 RE6 有 DTI，你可以直接 dump 出相机类名与字段偏移，**彻底不用猜**。见第 5 节验证步骤。

### 2.7 DX9 着色器常量：有实测数据，但对你的方案不利 [已验证 + 推断]

`dmc4_hook/src/mods/ShaderEditor.cpp` 里嵌了一段作者手写、并用 `D3DXAssembleShader` + `CreatePixelShader` **成功注入 DMC4** 的 DX9 pixel shader（`ps_3_0`）。它的常量用法是唯一一处 MTF DX9 常量约定的硬证据：

```
def c12, 0, 1, -1, -0
def c13, 0.25, 0.5, 0, 0
nrm_pp r0.xyz, v1
dp3_pp r0.x, r0, -c7          // c7  = 相机/view 前向方向
add r0.xyz, c0, -v0           // c0  = 相机世界坐标
... c3.x / c3.z / c3.w        // 雾 起止/密度
dp4 r0.y, c4, r1              // c4  = 雾平面方程
mov r0.xyz, c6 ; cmp_pp r0.xyz, -c2.x, r0, r1   // c6 = 雾颜色, c2.x = 雾开关
mad_sat_pp r0.w, v0.w, c10.x, c10.y             // c9/c10/c11 = 淡出阈值
```
==> **c0 = 相机位置，c7 = 相机前向，c2.x = 雾开关，c3/c4/c5/c6 = 雾参数，c9–c11 = 淡出。** 这些是 **pixel shader** 常量。

反向结论（同样重要）：
- 我**找不到任何 MT Framework 代码调用 `SetVertexShaderConstantF`**，也**没有任何 c0–c3=world / c4–c7=view / c8–c11=projection 的固定约定** [未找到]。
- MTF 是**逐材质 shader 程序**，常量布局随材质变化；repo 作者自己的注释说 MTF 的 draw command「use POD structs so no MTFramework DTIs」，并称整个 shader 替换思路「basically useless」[已验证原文意思]。
- **所以：不存在一个"引擎级统一 view 矩阵槽位"等着你去旋转。** 你按"形状像 view 矩阵"筛出来的寄存器，命中的极可能是别的东西（见第 4 节）。

### 2.8 相机参数还能从数据文件改 [已验证]

RE5/RE6 家族的相机距离等参数也存在于 `.CCL` / `.CHN*` 数据文件里，社区用十六进制编辑改（residentevilmodding 论坛 Shigu 的说明）。说明**相机参数是"数据"，不是渲染期矩阵**——与 2.2/2.4 一致。

### 2.9 RE: Revelations（MTF）的相机 AOB —— 与你的信息一致 [已验证]

原始帖（`residentevilmodding.boards.net/thread/12865/fov-fix`）中 picoleet 的可用脚本，逐字：
```
aobscan(cameraDistanceReadAOB, F3 0F 10 77 18 F3 0F 58 70 78)   // "rerev.exe"+8CC6C : movss xmm6,[edi+18]
aobscan(cameraModeReadAOB,     8B BE D8 00 00 00 8B CB)         // "rerev.exe"+8BAE3 : mov edi,[esi+000000D8]
...
chkladderclimbing:
  mov ecx,[pCameraMode]
  cmp byte ptr [ecx+dc],1
fCustomCameraDistance: dd (float)-170.0
```
→ 相机距离标量 `[cameraObj+0x18]`、相机控制器指针 `[esi+0xD8]`、相机模式字节 `[controller+0xDC]`。
另外他们发现游戏本身用 `c7 46 3c 00 00 70 42`（`mov [esi+0x3C], 0x42700000` = 60.0f）**把硬编码 FOV 常量写进相机对象的 +0x3C**，于是用 code cave 跳转就地改那个立即数。

**模式很清楚：MTF 的相机是一堆标量字段，mod 通过 hook 读写它们的指令来改。**

### 2.10 变换节点是 `uCoord`，**写完必须手动调用 `updateLmat`/`updateWmat`** [已验证]

MT Framework 的变换节点**不叫 `Transform`，叫 `uCoord` / `UCoord`**（`cUnit` 的子类）。DMC4（32 位）ReClass dump 与 `uActor.hpp` 里 `uActorMain::uCoord` 字节完全一致：

```cpp
class CUnit : public MtObject {
    union { uint32_t bitfield; struct { uint16_t pad0; uint8_t mTransMode; uint8_t mTransView; }; };  // 0x04
    CUnit *mp_next_unit;   // 0x08
    CUnit *mp_prev_unit;   // 0x0C
    float m_delta_time;    // 0x10
    char reserved_state_flags[4];
}; static_assert(sizeof(CUnit) == 0x18);

class UCoord : public CUnit {
    UCoord *mp_parent;   // 0x18
    uint32_t mParentNo;  // 0x1C  "attached entity's joint index"
    uint32_t mOrder;     // 0x20  "axis order"
    Vector3f m_pos;      // 0x30
    Vector4  m_quat;     // 0x40   <<< 四元数旋转
    Vector3f m_scale;    // 0x50
    Matrix4x4 m_lmat;    // 0x60   <<< 局部矩阵
    Matrix4x4 m_wmat;    // 0xA0   <<< 世界矩阵
}; static_assert(sizeof(UCoord) == 0xE0);
```
虚函数（`uActor` vtable dump）：`get_dti() +0x10`、`render(void* mtrans) +0x24`、`die() +0x30`、**`updateLmat() +0x34`**（由 pos/quat/scale 构建局部矩阵）、**`updateWmat() +0x38`**（world = parent->mWmat * mLmat）、`getJointMatrix(int) +0x3C`。

> **关键陷阱**：`m_pos` / `m_quat` 只是"参数"，`m_wmat` 才是渲染用的矩阵。你改完 pos/quat 之后**必须调用 `updateLmat()` 和 `updateWmat()`**，否则改动不会传播到世界矩阵 —— 这会表现成"我明明改了字段但画面没反应"。

基础类型有个坑：**`MtVector3 { float x,y,z; uint32_t padding; }` = 0x10（16 字节步长！）**，`MtVector4` 也是 0x10，`MtMatrix { MtVector4 vectors[4]; }` = 0x40。按 12 字节步长遍历 MTF 的向量数组一定会读错。

### 2.11 不靠偏移表也能找到相机：用 `MtObject::get_dti()` 按**名字**找 [已验证]

这是最可移植的杠杆点（跨 MTF 世代不用改偏移）。32 位布局：

```cpp
class MtObject {                 // size 0x04
  virtual void vec_del_dtor(uint32_t);  // vtable +0x00
  virtual void create_ui(void*);        // +0x04
  virtual bool is_enable_instance();    // +0x08
  virtual void create_property(void*);  // +0x0C
  virtual MtDTI* get_dti();             // +0x10
};
class MtDTI {                    // size 0x20
  /* vtable @0 */ char* m_name;  // +0x04
  MtDTI *mp_next;   // +0x08
  MtDTI *mp_child;  // +0x0C
  MtDTI *mp_parent; // +0x10
  MtDTI *mp_link;   // +0x14
  size_t m_size;    // +0x18
  uint32_t m_id;    // +0x1C
};
```
**解析算法**（来自 `MtFrameworkNodeInfoReader.cs` 源码，已在 dmc4 dump 上验证 —— 那里 `get_dti()` 就是 `return (MtDTI*)0x00E5C5A8;`，即 `mov eax,<abs32>`）：
```
vtable = *(uint32_t*)(obj + 0)
code   = *(uint32_t*)(vtable + 0x10)        // get_dti 的代码地址
dti    = *(uint32_t*)(code + 1)             // mov eax,<abs32> 的 4 字节操作数
name   = *(char**)(dti + 0x04)              // 读 ≤64 字节 UTF-8
```
==> **你可以遍历 MTF 的 DTI hash table（256 桶）或从单例管理器出发，按类名（如 `sXxxCamera`、`uCamera`）找到相机对象，再用 DTI 的字段表拿偏移。** MHW 的相机单例叫 `sMhCamera` —— 说明**单例名是逐游戏不同的，RE6 的必须自己发现**。

DTI 的注意点：只包含**开发者手工注册过**的信息。名字/大小/vtable/CRC32/基类通常有；**字段偏移经常缺失**，会写成 `Offset:0x7FFFFFFFFFFFFFFF, PSEUDO-PROP` 并附 Getter/Setter 地址，需要你自己反汇编（例如 `lea rax,[rcx+0xc]` → 字段在 +0xC）。

### 2.12 相机是调度器里的一个 `cUnit`，必须 hook 在相机自己的 update 里 [已验证 + 一处未验证]

```cpp
sUnit : CSystem { MoveLine mMoveLine[32]; }   // size 0x320
MoveLine { void* vtable; char* mName;
           uint32_t mParallel:1, mPause:1, mTrans:1, mLineType:6, reserved:23;
           cUnit* mTop; cUnit* mBottom; float mDeltaTime; }   // size 0x18
```
`SharpPluginLoader` 的 `Unit.cs` 文档原文：*"A unit is basically an object that can act on its own (i.e. has an update method)… **Schedulers, Cameras, Visual Filters are also units**"*，且 `public class Camera : Unit`；`Unit` 字段：`Rno 0x8, UnitParam 0xC, LineNo 0x10, DrawMode 0x48, DeltaSec 0x68, DeltaTime 0x6C, ComponentManager 0x70`；Move/Draw/Fix 是 `0x14` 的 bit 0/1/3。
`dmc4_hook` 把相机/灯/滤镜生成到 **MoveLine 17**（`sUnit_spawn(obj, 17)`，`viewport->mAttr = 0x17`）。
**[未验证]** 游戏相机具体跑在哪条 MoveLine、以及"相机 update 是否保证在逻辑 update 之后"没有任何可达文档。实用结论：**在相机自己的 update 内部 hook 并写状态，不要从 `Present` 里写。**

### 2.13 DMC4 的相机对象完整布局（供你对照 RE6 的探测结果）[已验证]

```cpp
class cCameraPlayer {                 // 玩法相机（标量参数那个）
    char pad_0[0x10];
    Vector3f pos;          // 0x10
    Vector3f lookat;       // 0x20
    float nearClipPlane;   // 0x40
    float angle;           // 0xD4
    float distance;        // 0xD8
    float distanceLockon;  // 0xDC
    float FOV;             // 0xE4
    float FOVBattle;       // 0xE8
    Matrix4x4 possibleMat1;// 0x200
}; static_assert(sizeof(cCameraPlayer) == 0x240);

class uCameraCtrl {                   // 作者注："sMediator + D0 camera, had a more useable glm mat but less settable values"
    float nearClipPlane;   // 0x1C
    float FOV;             // 0x24
    Vector3f pos;          // 0x30
    Vector3f up;           // 0x40
    Vector3  lookat;       // 0x50
    Matrix4x4 possibleMat5;// 0x1B0
    cCameraPlayer* cCameraPlayer1; // 0x490
}; static_assert(sizeof(uCameraCtrl) == 0x494);

class SMediator { /* ... */ uCameraCtrl* camera1; /* 0xD0 */ };   // size 0x878
```
[推断] `possibleMat5 @0x1B0` 很可能是该相机的 view 矩阵，但**未确认** —— 作者故意选择用 `glm::lookAt(pos, lookat, up)` + `glm::perspective(FOV, ...)` 重建，而不是直接读它（见 `World2Screen.cpp` 的 `g_vp = projMatrix * viewMatrix;`）。

---

## 3. 通用模式横向对比

### 3.1 四种模式

| 模式 | 说明 | 代表 |
|---|---|---|
| **(a) hook 着色器常量/常量缓冲上传，改矩阵** | 拦 `SetVertexShaderConstantF` / `VSSetConstantBuffers`，替换矩阵 | Vireio Perception 的 DX9 shader 路径（且它改的是**投影矩阵**，见 3.2） |
| **(b) 写引擎相机对象字段** | 直接改 position / rotation / target / up / FOV / yaw 标量 | REFramework `FirstPerson.cpp`/`FreeCam.cpp`、`dmc4_hook` `CameraSettings.cpp`/`DebugCam.cpp`、UEVR、LukeRoss、MotherVR、Vivecraft、Quake VR |
| **(c) hook 相机更新 / view-matrix 构建函数，在其前后合成 HMD 姿态** | 引擎算完之后覆盖，或 hook getter 改返回值 | REFramework（`updateTransform` hook + `via.Camera::get_ViewMatrix` hook）、Source engine（`OverrideView`/`OverrideStereoView`）、UEVR（`CalculateStereoViewOffset`）、Vivecraft（mixin 掉 `Camera.alignWithEntity`）、`dmc4_hook`（RVA naked detour） |
| **(d) 引擎自身脚本/反射 API** | 用引擎托管层设 `Camera.transform` | REFramework 的 Lua/C# API |

**结论：(b) + (c) 是绝对主流，(a) 作为"旋转机制"极其罕见。**

### 3.2 逐个项目的机制（均标注验证等级）

**① REFramework（RE Engine）** [已验证] —— 见第 1 节。头部旋转 = `final_mat *= headset_rotation`，写回 `via.Transform` 与 camera controller；逐眼偏移 = `via.Camera::get_ViewMatrix` 里 `mtx = current_eye_transform * mtx`；逐眼投影 = `via.Camera::get_ProjectionMatrix` 直接返回 HMD 投影。**无任何常量缓冲矩阵修补。**

**② UEVR（praydog，Unreal）** [已验证，来自 `include/uevr/API.h`]
注入点是引擎的**立体视点偏移函数**，参数是**位置 + 欧拉角**，不是矩阵：
```c
typedef void (*UEVR_Stereo_CalculateStereoViewOffsetCb)(
    UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters,
    UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double);
// UEVR_Rotatorf = { float pitch, yaw, roll; }
```
回调 `on_early_/on_pre_/on_post_calculate_stereo_view_offset`；docs.uevr.io 原文："called before VR transformations are applied to the view, before the stereo device calculates the view offset."
==> **HMD 朝向是以"角度"注入的，不是矩阵。** UEVR 还提供自己的假 `IStereoRendering` 设备（`FFakeStereoRenderingHook`），引擎侧相关文件为 `src/mods/vr/FFakeStereoRenderingHook.cpp`、`IXRTrackingSystemHook.cpp`。
**整个公开插件 API 里没有任何常量缓冲接口**；唯一的 D3D 回调是 swapchain / render-target 级（`UEVR_OnPostRenderVRFrameworkDX11Cb(immediate_context, ID3D11Texture2D*, ID3D11RenderTargetView*)`、`on_present`、`on_device_reset`）。`UEVR_VRData` 只暴露 `get_ue_projection_matrix`、`get_eye_offset`、`set_standing_origin`、`set_rotation_offset`、`recenter_view`、`recenter_horizon` —— **没有 view-matrix 或常量缓冲 setter**。
（未验证：`CalculateStereoViewOffset` 具体是 Unreal 的哪个符号是**推断**；`APlayerCameraManager::UpdateCamera`/`ULocalPlayer::CalcSceneView`/`FSceneView`/`LateUpdateCamera` 这些字符串**在能读到的 UEVR 文件里没找到**；324 KB 的 `FFakeStereoRenderingHook.cpp` 未读，所以"它内部也没有 CB 修补"是**推断**。）

**③ LukeRoss REAL VR（GTA V / RDR2 / Cyberpunk）** [已验证：其 README 原文；无源码]
- README 原句："tied the **yaw** … of the in-game camera to the headset's relative yaw"；"tied the **pitch** … to the headset's absolute pitch"；"tied the **roll** … of the rendering camera to the headset's roll"；"overrode the game's stringent limits on camera pitch"；"**decoupled the camera angles from the position**"。
- 逐眼：**交替眼渲染（AER）** —— "stereoized the world rendering by **shifting the in-game camera into the eye positions on alternate frames**"；帧数 100L/99R；作者原话 "ASW is incompatible with my mods because it doesn't work with alternate eye rendering"。
- 还有一句非常关键：**"applied a supplementary view fix during rendering to perfectly correct the camera angles and position even when the in-game camera resists being changed"**，热键 "O Toggle view matrix fix enable - on at start"。
==> 主流是 (b) 写引擎相机 yaw/pitch/roll，外加一个**渲染期的"view matrix fix"**（具体代数未公开）。**任何可读文本里都找不到** `SetVertexShaderConstantF`/`SetVSConstant`/CB 槽位号/相机偏移 [未找到]。仓库 `LukeRoss00/gta5-real-mod` 只有一个 README；`LukeRoss00/RVR` 只有 issue 区。**无源码。**

**④ Source engine VR（Valve `source-sdk-2013` `client_virtualreality.cpp`）** [已验证]
HMD 姿态以矩阵读入（`VMatrix matMideyeZeroFromMideyeCurrent = g_pSourceVR->GetMideyePose();`），在世界空间合成（`m_WorldFromMidEye = worldFromTorso * matMideyeZeroFromMideyeCurrent;`），然后**转换回引擎的 origin + QAngle 字段**：
```cpp
// Finally convert back to origin+angles that the game understands.
pViewMiddle->origin = m_WorldFromMidEye.GetTranslation();
VectorAngles( m_WorldFromMidEye.GetForward(), m_WorldFromMidEye.GetUp(), pViewMiddle->angles );
*pViewModelAngles = pViewMiddle->angles;
```
逐眼：`OverrideStereoView()` 里 `VMatrix worldFromLeftEye = m_WorldFromMidEye * matOffsetLeft;` 然后 `MatrixAngles( worldFromLeftEye.As3x4(), pViewLeft->angles, pViewLeft->origin );`，并直接设 `pViewLeft->m_bViewToProjectionOverride = true; g_pSourceVR->GetEyeProjectionMatrix(&pViewLeft->m_ViewToProjection, ...)`。
==> **写的是 `CViewSetup::angles` + `CViewSetup::origin`（不是 `m_angViewAngles`），外加逐眼 `m_ViewToProjection`。**
细节：`vr_translation_limit`（"How far the in-game head will translate before being clamped."）、独立的 torso yaw 概念（`m_PlayerTorsoAngle[ROLL]=0; m_PlayerTorsoAngle[PITCH]=0;`，注释 "torso should not have roll or pitch or you'll make people ill"）、`vr_moveaim_mode` 枚举含 `HMM_SHOOTMOVELOOKMOUSE`（"HMD is ignored completely, mouse does everything"）。
（HL2VR 自己的 fork **无公开源码** —— FAQ 原文 "there is currently no definitive timeline"；把 Valve 的这段当作 Source 引擎 VR 的答案，把"HL2VR 也这么做"视为**推断**。）

**⑤ Vivecraft（Minecraft）** [已验证，源码]
`CameraVRMixin` 取消 `Camera.alignWithEntity` 并执行 `this.setRotation(eye.getYaw(), -eye.getPitch())`，注释原文：
> "we cannot set the rotation to the full matrix, **because particles would rotate with the head**"

它还替换了完整视图旋转矩阵（`Camera.getViewRotationMatrix` → `RenderHelper.getVRModelView`），每个 render pass 写实体字段 `entity.setXRot(-eyePose.getPitch()); entity.setYRot(eyePose.getYaw()); livingEntity.yHeadRot = entity.getYRot();`（配 cache/restore），每个逻辑 tick 走 `VRPlayer.doPermanentLookOverride`：`player.setYRot(data.hmd.getYaw()); player.setYHeadRot(player.getYRot()); player.setXRot(-data.hmd.getPitch());`
==> **(b)+(c) 混合**。那句注释值得记住：**只改部分矩阵/只改矩阵会导致粒子等附属物跟着头转** —— 这是"该改哪个表示"的实战经验。

**⑥ Quake VR（`vittorioromeo/quakevr`）** [已验证]
- `Quake/vr.cpp` `VR_UpdateScreenContent()`：`cl.viewangles[YAW] = orientation[YAW]; cl.viewangles[ROLL] = orientation[ROLL];`（pitch 同理），HMD 四元数经 `QuatToYawPitchRoll()` 转欧拉。
- `Quake/view.cpp` `V_CalcRefdef()`：`r_refdef.viewangles = cl.viewangles;` 且 `ent->angles[YAW] = VR_GetBodyYawAngle();`（头/身 yaw 分离）。
- `Quake/gl_rmain.cpp` `R_SetupGL()`：`if(VR_EnabledAndNotFake()) VR_SetMatrices();`
==> 同为 **(b)**：HMD → 引擎角度字段 → 引擎自己构建 view 矩阵。

**⑦ Alien: Isolation MotherVR（Nibre）** [已验证：作者发布说明；无源码]
`Nibre/MotherVR` 只有 README（510 B），发的是 `dxgi.dll`。头部旋转走**游戏自己的玩家旋转状态** —— 作者原文："When the game rotates the player, it does this acceleration-smoothing (like mouse-acceleration) between the old rotation and the new one"，并且作者要去驱动引擎的相机效果（"blackout blinders"、head bob、"rotate to align with the ground"）。
==> **(b)**。具体字段**未验证**。

**⑧ Vireio Perception（开源 DX9 VR 驱动）—— (a) 模式的完整实证** [已验证]

这是最接近你技术栈的公开实现（`d3d9.dll` 代理 + 逐游戏 profile）。

**(1) 它只改投影矩阵，而且必须先"反投影"**（`DxProxy/DxProxy/D3DProxyDeviceAdv.cpp`）：

```cpp
void D3DProxyDeviceAdv::adjustEyeOffsetAndViewFrustum(D3DXMATRIX &outMatrix, D3DXMATRIX &inMatrix) {
    D3DXMATRIX transform;
    D3DXMatrixTranslation(&transform, separation*eyeShutter*10.0f+offset*10.0f, 0, 0);
    float adjustedFrustumOffset = convergence*eyeShutter*0.1f;
    D3DXMATRIX reProject;
    D3DXMatrixPerspectiveOffCenterLH(&reProject, l+adjustedFrustumOffset, r+adjustedFrustumOffset, b, t, n, f);
    if(trackerInitialized && tracker->isAvailable()) {
        D3DXMATRIX rollMatrix;
        D3DXMatrixRotationZ(&rollMatrix, tracker->currentRoll);
        D3DXMatrixMultiply(&transform, &transform, &rollMatrix);   // 头部追踪只用了 roll
    }
    outMatrix = inMatrix * invertProjection * transform * reProject;   // <<< 注意 invertProjection
}
```

**(2) 它靠 D3DX 常量表的"名字"识别寄存器，而不是靠矩阵形状**：

```cpp
void D3DProxyDeviceAdv::parse4by4Matrices(D3DXCONSTANT_DESC &desc) {
    if(desc.Name == NULL) return;
    if(!strstr(desc.Name, "proj") && !strstr(desc.Name, "Proj")) return;   // 按名字！而且是 proj 不是 view
    if(desc.RegisterCount != 4) return;
    TargetMatrix tm;
    tm.startRegister = desc.RegisterIndex;
    tm.transformationFunc = (desc.Class == 2) ? &transformRowMajor4by4 : &transform4by4;  // 行/列主序分别处理
    targetMatrices.push_back(tm);
}
```
```cpp
void transformRowMajor4by4(IDirect3DDevice9 *pD3Ddev, int startRegister, D3DProxyDeviceAdv* proxyDev) {
    float pConstantData[16];
    pD3Ddev->GetVertexShaderConstantF(startRegister, pConstantData, 4);
    D3DXMATRIX original = D3DXMATRIX(pConstantData);
    D3DXMATRIX transposed;
    D3DXMatrixTranspose(&transposed, &original);        // 先转置
    proxyDev->adjustEyeOffsetAndViewFrustum(original, transposed);
    D3DXMatrixTranspose(&transposed, &original);        // 再转回来
    pD3Ddev->SetVertexShaderConstantF(startRegister, transposed, 4);
    memset(&dirtyRegisters[startRegister], 0, sizeof(boolean)*4);
}
```
常量被延迟到 **Draw 调用前**才改（`transformDirtyShaderParams` 在 `DrawIndexedPrimitive`/`DrawPrimitive`/`DrawRectPatch`/`DrawTriPatch` 里调用），用 `dirtyRegisters[300]` 追踪哪些寄存器被写过。

**(3) 按游戏硬编码寄存器号**：
```cpp
bool D3DProxyDeviceEgo::validRegister(UINT reg) {
    switch(game_type) {
    case EGO: case EGO_DIRT:  if(reg != 12) return true; else return false;
    default:                  if(reg == 0) return true; else return false;
    }
}
```
**(4) 它假设那个常量是 World\*View\*Projection 合体矩阵**：
```cpp
D3DXMatrixMultiply(&worldViewMatrix, &sourceMatrix, &matProjectionInv);   // 反投影 → 得到 World*View
D3DXMatrixPerspectiveOffCenterLH(&matProjection, convergence*eyeShutter-spacing, ..., 1.0f, 100.0f);
D3DXMatrixMultiply(&sourceMatrix, &worldViewMatrix, &matProjection);
currentMatrix[0] += separation*eyeShutter+offset;      // 直接粗暴改一个 float
```
**(5) 启发式失败的实证痕迹**：`if(inMatrix._41 == 0.0f) return; // Mirror's Edge compat. hack (otherwise you get weird squares)`；`void D3DProxyDeviceAdv::findWeirdMirrorsEdgeShader(UINT pSizeOfData) { if(pSizeOfData != 172) return; ... }`（按 shader 字节码长度特判）。
**(6) 固定功能管线路径**（`D3DProxyDeviceFixed.cpp`）—— 只有这条路上它才直接改 view 矩阵（`D3DTS_VIEW` 时后乘 roll 矩阵 + 平移；`D3DTS_PROJECTION` 时 `* matProjectionInv` → 改 `transMatrix[8]` → `* matProjection`）。

**重要观察** [已验证]：即便是这个 DX9 VR 驱动，头部追踪也只把 **roll** 乘进矩阵。yaw/pitch 只作为 `MotionTracker::yaw/pitch/deltaYaw/deltaPitch` + `multiplierYaw/multiplierPitch` 存在（`MotionTracker.h`），**没有被注入到相机矩阵**。这说明当时的 DX9 VR wrapper 并没有真正解决"用矩阵做 yaw/pitch 头部追踪"，而是绕开了。

### 3.3 为什么 (b)+(c) 是主流

所有子代理独立收敛到同一解释，与我的分析一致：

1. **引擎相机是"单一真相源"**：阴影、反射、剔除、天空盒、音频、动画、UI 定位全都读它。写一次相机对象，所有 pass 自动一致。
2. **改常量寄存器只影响那一次 draw**，而且必须**每个 pass 重复**：天空盒/水面/平面反射用的 view 是去掉平移的、阴影贴图用的是光源 view、billboard 用的是相机朝向 —— 它们**合法地使用不同的矩阵**。你只改一个，就会出现"部分 pass 转了、部分没转"，而期望未旋转 view 的 pass 会通过屏幕空间查找被重新投影 → 扭曲。
3. **引擎反射让 (b) 变得廉价**：REFramework 有 TDB/DTI（类型 + 方法名 + 字段偏移），Unreal 有 UObject 反射，MTF 2.0 有 DTI，Source/Quake/Minecraft 都是开源的。有反射就不需要猜矩阵。

### 3.4 3DMigoto / HelixMod：连它们也不做"view 矩阵自动识别" [已验证]

- `[Hunting]` 是**人工循环+打标记**（`hunting=1`、`marking_mode=skip`、`previous/next/mark_pixelshader` 等热键）。wiki 原文流程："Numpad 2 steps up through active Pixel Shaders. Numpad 1 steps down … press Numpad 3 to mark a Pixel Shader" → "Edit the file to fix the shader. This is where you apply your superpower." —— **一个手动的人类回路，没有任何 orthonormal/translation/inverse-transpose 自动启发式**。
- 3DMigoto 的立体本身来自 **NVIDIA 3D Vision 驱动**（NVAPI；`[Profile] StereoProfile`、`StereoConvergence`），它只负责给单个 shader 打补丁修坏掉的特效。
- `d3dx.ini` 里**逐字记录的失败模式**：
  > "These two options change which constant buffers the driver uses to pass the separation and convergence… The default value is 12, and you may need to change it **if the game already uses that constant buffer for any purpose**, which should be apparent as you will see 2D geometry on any shader that uses this."
- **同一个 shader 里还有一堆非相机矩阵需要分别修**：`fix_InvTransform=ScreenToLight,InverseTranslatedViewProjectionMatrix`、`fix_BackProjectionTransform1=ScreenToTranslatedWorldMatrix._m00,_m02,_m01`、`fix_ObjectPosition1=PointPositionAndInverseRadius`、`fix_ObjectPosition2=SpotPositionAndInverseRadius`，以及一段 ShaderRegex 示例，插入的注释直接写着 `// UE4 shadow correction:`，作用于 `TranslatedWorldToShadowMatrix`。
- **"你的写入会被覆盖"有文档证据**：`analyse_options` 的 `dump_on_unmap`（"whenever the game maps them to the CPU with the Map()/Unmap() calls. Typically used to update constant buffers"）与 `dump_on_update`（"whenever the game updates them with the UpdateSubresource() call"）。
- **逆矩阵是独立寄存器**：pixel shader 消费游戏上传的 screen→world 逆矩阵（`ScreenToWorld`、`InverseTranslatedViewProjectionMatrix`、`TranslatedWorldToShadowMatrix`）。只改正向 view 而不管逆矩阵 → 屏幕空间特效滑动/剪切 [推断]。

### 3.5 未覆盖项（明确声明）

- **vorpX**：**没有覆盖**。vorpx.com、其论坛、helixmod.blogspot.com、mtbs3d.com、Wayback Machine 在当前环境全部 403/不可达。**请把 vorpX 当作"未调研"，而不是"部分调研"。**
- HL2VR 自己的 fork：无公开源码。Doom 3 BFG VR：`neo/vr/*.cpp` 无法读取。Jedi Fallen Order VR / Elden Ring VR / Dark Souls VR：无公开源码（后两者是 LukeRoss 的闭源 Patreon 版本；GitHub 上同名仓库是无关的 UE4 资产工程，**不要引用**）。

---


## 4. 为什么「改常量缓冲里的矩阵」会变成拉伸/剪切而不是旋转

### 4.1 数学上的判决性理由 [推断，但为线性代数结论]

设 view 矩阵 V，相机世界矩阵 C = V⁻¹。两者左上 3×3 **都是正交矩阵**（旋转），行/列向量单位长度、相互正交，第 4 行（或列，取决于约定）为 (0,0,0,1)。

- **任何正交矩阵左乘或右乘一个旋转，结果仍然是正交矩阵。** 所以：如果你真的改到了 view 矩阵的 3×3，无论乘法顺序对错，结果**都还是一个合法旋转** —— 最坏情况是转轴/方向不对（例如转反了、绕世界原点转），**绝不会出现拉伸/剪切**。
- 因此：**「画面拉伸」本身就是"你改的那个矩阵不是纯 view 矩阵"的判决性证据。**

反过来说，拉伸意味着你改的 4×4 的左上 3×3 **不是正交的**：

1. **VP / WVP 合体矩阵**（最常见的元凶）。透视投影矩阵 K 的左上 3×3 = `diag(fx, fy, ...)` 含非等比缩放，VP = V·K 的 3×3 就带上了这个缩放；再乘一个旋转 → 得到剪切矩阵 → 透视除法后**画面被拉斜/拉伸**。
   **更精确的表述**：如果那个寄存器里装的是合体 `P·V`，你左乘一个旋转得到 `R·P·V`，而正确的变换应该是 `P·R·V`。`P` 不是旋转，所以 `R·P·V` 不再是一个合法透视矩阵 —— **用于透视除法的齐次 w 不再是 view 深度的纯函数**，于是每个像素的缩放比例不同 = 非均匀拉伸/剪切，而不是整体旋转。
   **实证**：Vireio 必须写成 `inMatrix * invertProjection * transform * reProject` —— 也就是**先把投影除掉回到 view 空间，改完再乘回去**（`D3DProxyDeviceAdv.cpp`）；`D3DProxyDeviceEgo.cpp` 更是直接用 `sourceMatrix * matProjectionInv` 来"反投影得到 World\*View"。**这是"常量里的矩阵往往已经是 VP/WVP"最直接的一手证据。**
2. **骨骼蒙皮调色板（bone palette）**。DX9 用 `SetVertexShaderConstantF` 一次上传整块骨骼矩阵（每根骨头 3 或 4 个寄存器，4×3 或 4×4），它们天然是「正交 3×3 + 世界尺度平移」—— **和你的分类器判据一模一样**。改这些 → 角色/物件被拉变形。**这极可能就是你"整个画面在扭曲"的一部分原因**（MTF 一次 Draw 会连物体矩阵一起上传）。
3. **法线矩阵 / inverse-transpose 矩阵**：含非等比缩放甚至 1/scale，乘旋转必然剪切。
4. **相机世界矩阵 C 而不是 view 矩阵 V**：C 也是「正交 3×3 + 平移」，同样能通过你的判据；但它与 V **互为逆**，处理它会让旋转方向相反。而且如果引擎用 C 做天空盒/billboard、用 V 做几何，你只改一个就会出现**不同物件朝向不一致**（这会看起来像"画面被撕开"）。z1rp 的 RE 博客把这一点讲得很清楚：`view = inverse(camera world)`，两者并存，靠 `dot(Right,Pos)` 与平移分量互相验证。[已验证该文表述]
5. **转置/主序搞错**：如果矩阵在常量里是"列主序/转置"存放，你按行去旋转，等于对真正的列向量做混合 → 直接破坏平移分量，出现位移+拉伸。Vireio 专门分了 `transformRowMajor4by4`（`desc.Class == 2` 时先 `D3DXMatrixTranspose` 再处理）[已验证]。
6. **只改一部分**：如果矩阵是转置存放，只改"前 3 行的 xyz"会打到真正的平移分量上。
7. **同一个寄存器被多个 pass 复用**：MTF 是逐材质 shader，同一 c 槽位在不同材质里语义不同（第 2.7 节）。你改的是一个 per-material 的矩阵，可能同时被用作视锥/雾/贴花/粒子朝向输入。
8. **引擎重建/覆盖**：`SetVertexShaderConstantF` 是**有状态**的：常量一直有效直到被覆盖。如果引擎在帧内某处**重新上传**该寄存器（例如先设一次、后面某 pass 又设一次），你的修改会被丢弃 —— 表现就是"有时有效有时没效果/闪烁"。REFramework 的正解是关掉 `updateCamera` 标志；`dmc4_hook` 的正解是 hook **读指令**而不是写指令。3DMigoto 的文档也证实了这种逐帧重写：
   - `dump_on_unmap`：*"whenever the game maps them to the CPU with the Map() / Unmap() calls. Typically used to update constant buffers"*
   - `dump_on_update`：*"whenever the game updates them with the UpdateSubresource() call"*
9. **【3DMigoto 逐字记录的"缓冲区复用"故障】** `d3dx.ini` `[Profile]` 原文：
   > "These two options change which constant buffers the driver uses to pass the separation and convergence… The default value is 12, and you may need to change it **if the game already uses that constant buffer for any purpose, which should be apparent as you will see 2D geometry on any shader that uses this.**"
   —— 这是"同一个常量缓冲被多用途复用"的一手证据（后果是"看到 2D 几何"，本质上就是非刚体变换）。
10. **【同一 shader 里存在多个"相机类"矩阵，需要分别处理】** 3DMigoto 的 autofix 选项暴露了这个现实：
    `fix_InvTransform=ScreenToLight,InverseTranslatedViewProjectionMatrix`、`fix_BackProjectionTransform1=ScreenToTranslatedWorldMatrix._m00,_m02,_m01`、`fix_ObjectPosition1=PointPositionAndInverseRadius`、`fix_ObjectPosition2=SpotPositionAndInverseRadius`，以及一段插入注释直接写着 `// UE4 shadow correction:`、作用于 `TranslatedWorldToShadowMatrix` 的 ShaderRegex。**阴影/光源/逆矩阵都是独立寄存器，必须分别修。** 你只改一个"view 矩阵"必然造成部分 pass 不一致。
11. **【逆矩阵过期】** pixel shader 会消费游戏上传的 screen→world 逆矩阵（`ScreenToWorld`、`InverseTranslatedViewProjectionMatrix`、`TranslatedWorldToShadowMatrix`）。你只改正向 view 而不同步这些逆矩阵 → 屏幕空间特效滑动/剪切 [推断]。
12. **【只改矩阵表示、不改引擎状态 → 附属物跟着头转】** Vivecraft 把这条写进了注释：
    > "we cannot set the rotation to the full matrix, **because particles would rotate with the head**"
    这是"该改哪个表示（角度字段 vs 完整矩阵）"的实战经验：只替换视图矩阵，会让挂在相机上的粒子/billboard 一起转。
13. **【引擎"抗拒被改"】** LukeRoss 的 README 明确承认这一点，并因此加了一层渲染期修正：*"applied a supplementary view fix during rendering to perfectly correct the camera angles and position **even when the in-game camera resists being changed**"*（热键 "Toggle view matrix fix"）。这从侧面说明：**光写相机字段不够，引擎的自动回正/平滑会和你抢。** MTF 里对应的东西就是 `camera_lockon_corrects`、attack-towards-camera 这类自动修正（`dmc4_hook` 用 nop patch 关掉它们）。

### 4.2 方法论：怎么可靠地识别矩阵（社区共识）

**A. 生产代码里的正交性启发式 —— 一手来源** [已验证]

你想找的"orthonormal 3x3 + 平移"判据，**确实存在于生产代码里**，但它被用来**校验**而不是**搜索**。Valve 的 `source-sdk-2013` `client_virtualreality.cpp` 里就有：

```cpp
bool IsOrthonormal ( VMatrix Mat, float fTolerance )   // 检查 LenFwd/LenUp/LenLeft ≈ 1，以及 DotFwdUp/DotUpLeft/DotLeftFwd ≈ 0
...
Assert ( IsOrthonormal ( worldFromLeftEye, 0.001f ) );  // 容差 0.001
```
同一个文件里还有投影矩阵的**结构**判据，注释逐字：
> "The projection matrices should be of the form: p0 0 z1 p1 / 0 p2 z2 p3 / 0 0 z3 1 / (p0 = X fov, p1 = X offset, p2 = Y fov, p3 = Y offset)"
随后是一串断言：`proj.m[0][1]==0`、`proj.m[0][3]==0`、`proj.m[1][0]==0`、`proj.m[3][0]==0`、`proj.m[3][1]==0`、`proj.m[3][2]==-1`、`proj.m[3][3]==0` —— **零模式 + `m[3][2] == -1` 是透视矩阵的特征**。

> 但请特别注意：**相机世界矩阵同样满足正交性判据**（它是 view 的逆），所以正交性**无法**区分 view 与 camera-world，也无法区分骨骼矩阵。这正是为什么所有成熟实现最后都放弃形状分类、改用**反射 + 行为**（见 B）或**逐游戏硬编码**。

**B. 按行为而非形状识别**

- z1rp 的 RE 系列（"Reversing The ViewProjection Matrix"）原话就是做法总结：
  > "Move the camera around and watch which parts of the matrix change. **Let the behavior tell you what's what.**"
  > "如果数值正交、单位长度、有明显的平移分量（通常 > 1.0f）、并以齐次坐标行/列 [0,0,0,1] 结尾，你可以比较确定它是真的变换矩阵。"
  该文同时给出了 view ↔ camera-world 的互验方法（`dot(Right,Pos)` 对平移分量、求逆后与另一个矩阵比对）。
- **Cheat Engine**：搜 `1.0f` / `-1.0f`，然后"向上看 / 向下看"反复 rescan 缩小到随相机朝向变化的地址；确认后 "find what accesses" 找更新函数（z1rp 的流程就是这个）。
- **REFramework**：对 `RETransform::worldTransform` 下断点找更新函数（`Hooks.cpp` 注释原文 "Can be found by breakpointing RETransform's worldTransform"）；再用引擎反射（TDB）按类型名 + 方法名定位。
- **3DMigoto / HelixMod**：**没有**自动 view 矩阵识别。进 hunting 模式，逐个 shader / index buffer / vertex buffer 循环切换，**打标记让目标物件消失**来定位（`marking_mode`）；识别的是 resource hash，不是数学形状。
- **MT Framework**：硬编码 RVA + AOB + 多级指针链，hook **读取**相机字段的指令。

---

## 5. 对 RE6 OpenXR 层的具体建议

### 5.1 立刻可做的判断（基于上面所有证据）

1. **停止按"形状像 view 矩阵"筛 `SetVertexShaderConstantF`。** MTF 没有引擎级统一 view 矩阵槽位（2.7）；你会持续命中物体矩阵/骨骼调色板/VP 合体矩阵，结果必然是变形而不是旋转（4.1）。
2. **把头部追踪的注入点移到相机对象/相机更新函数**，与 REFramework 一致：
   - 目标字段（MTF 语义）：`mCameraPos` / `mTargetPos` / `mCameraUp`（look-at 相机，2.2）+ 控制器的 yaw/pitch 标量（2.4）+ `mFov`。
   - 注入方式：**hook 读取这些字段的指令**（像 `camera_sens_clockwise_proc` 那样），或 hook 相机更新函数在其后覆写。
   - 数学：yaw/pitch/roll 改 `mTargetPos` 与 `mCameraUp`；若引擎用标量 yaw，就直接加。

### 5.2 定位 RE6 相机的可执行步骤

> **两条前置知识（最容易踩的坑）**
> 1. MTF 的变换节点是 `uCoord`（pos 0x30 / **quat 0x40** / lmat 0x60 / wmat 0xA0），**改完 pos/quat 必须调用虚函数 `updateLmat()`（vtable+0x34）与 `updateWmat()`（+0x38）**，否则世界矩阵不更新 → "字段改了但画面没反应"。见 2.10。
> 2. `MtVector3` 是 **16 字节步长**（含 padding），不是 12；按 12 步长遍历 MTF 向量数组一定读错。

1. **先确认 RE6 是否有 MTF DTI，并优先尝试「按名字找相机」（最可移植，跨世代不用改偏移）。**
   用 2.11 的算法把任意 MTF 对象的类名读出来：
   ```
   vtable = *(u32*)(obj+0);  code = *(u32*)(vtable+0x10);
   dti = *(u32*)(code+1);    name = *(char**)(dti+0x04);
   ```
   然后遍历 DTI hash table（256 桶）或从单例管理器出发，按 `s*Camera` / `uCamera` / `cCamera*` 之类名字筛选。
   - 若有 DTI：把 `MHW-ClassPropDump` 改成 32 位（`MtDTI` 从 0x38 改小、指针 4 字节）直接 dump 相机类名与字段偏移 —— **最省时间的路**。DTI 字段偏移可能缺失（显示 `0x7FFFFFFFFFFFFFFF, PSEUDO-PROP` + Getter/Setter 地址），那就反汇编 getter（形如 `lea rax,[rcx+0xc]` → 字段在 +0xC）。
   - 若没有 DTI：走 2/3。
2. **找相机 singleton 与 viewport。** 参考 DMC4 的 `*(sCamera**)(*(uintptr_t*)0xE5574C + 0x10358)` 形态：找一个静态基址 + 固定偏移的二级指针。用 CE 搜 `1.0f` / `-1.0f`，上下看缩小到随视角变化的地址。
3. **找相机控制器与 yaw/pitch。** 对 `mCameraPos` / `mTargetPos` 下"find what accesses"，拿到更新函数 RVA；在更新函数里找形如 `movss [reg+0x??], xmm` 的 yaw 写入与 `movss xmm, [reg+0x??]` 的读取。
4. **试验性注入**：先在 update 之后直接改 `mTargetPos`（yaw/pitch）与 `mCameraUp`（roll），验证能不能转；再改成在"读指令"处加增量（更稳，不会被后续覆盖）。
5. **记得抑制引擎的自动回正/锁定校正**（MTF 有 `camera_lockon_corrects`、attack-towards-camera 之类的自动修正，`dmc4_hook` 用 nop patch 关掉它们）。否则你的头部旋转会被引擎每帧"拉回去"。

### 5.3 立体与视口（顺带的架构建议）

MTF 的 `sCamera::viewports[8]` 是**多视口架构**，而引擎自带 `uFreeCamera` 可以先构造再挂到另一个 viewport 上 [已验证 DMC4 做法]：
```cpp
static void* (__stdcall*freecam_cons)() = (void*(__stdcall*)())0x9197A0;   // uFreeCamera ctor
uFreeCamera* cam = (uFreeCamera*)freecam_cons();
devil4_sdk::spawn_or_something((void*)0x00E552CC, (MtObject*)cam, 0x17);   // 放进 MoveLine 17
set_viewport(1, REGION_FULLSCREEN, (uintptr_t)cam);   // viewport->mpCamera = cam; mMode = FULLSCREEN; mAttr = 0x17
cam->uCameraBase.mCameraPos  = ((uCamera*)viewport0->mpCamera)->mCameraPos;
cam->uCameraBase.mTargetPos  = ((uCamera*)viewport0->mpCamera)->mTargetPos;
cam->uCameraBase.mCameraUp   = ((uCamera*)viewport0->mpCamera)->mCameraUp;
get_viewport(0)->mActive = enable;   // 关掉 gameplay cam，让引擎渲染 viewport[1]
```
对 VR 的意义很大：**你得到一个真正走引擎渲染管线的第二相机**（正确剔除、雾、HUD），而不用去骗常量缓冲。

### 5.4 关于常量路径的"如果一定要用"

若你仍然想要一条常量兜底：**仿 Vireio —— 只改投影矩阵，并先反投影**（`out = in * inverse(K) * T * K'`）。它的好处是：
- 不会破坏几何 pass 的世界/view 变换一致性（几何仍由引擎自己的 view 决定）；
- 头部**旋转**不应该走这条路（转 projection 只能做 shear/off-axis，做不出旋转），所以旋转仍必须走相机对象。

---

## 6. 验证等级总结（哪些是我读源码确认的）

| 结论 | 等级 |
|---|---|
| REFramework 头部追踪不碰着色器常量（grep 0 命中） | 已验证 |
| REFramework hook `UpdateTransform` / `PlayerCameraController::updateCameraPosition` / `TwirlerCameraControllerRoot::update` / `via.Camera::get_ViewMatrix` / `get_ProjectionMatrix` / `get_Size` | 已验证（Hooks.cpp 原文） |
| REFramework 的 HMD 旋转是 `final_mat *= headset_rotation`，并回写 transform position/angles/worldTransform 与 controller worldPosition/worldRotation | 已验证（FirstPerson.cpp 原文） |
| REFramework 用 `mainCameraController->updateCamera = false` 阻止引擎覆盖 | 已验证 |
| REFramework D3D11 立体是交替眼 backbuffer 拷贝，无常量缓冲矩阵修补 | 已验证（D3D11Component.cpp 原文） |
| RE Engine 相机结构字段名与偏移（CameraSystemDispatch.hpp） | 已验证 |
| FreeCam 覆写 `transform->get_world_transform()` / `get_position()` | 已验证 |
| MT Framework 相机是 look-at 参数化（pos/up/target/FOV），无存储矩阵；view 矩阵是 uCamera 虚函数 | 已验证（DMC4 uActor.hpp + MHW SharpPluginLoader 绑定） |
| MTF 的 view/proj 矩阵存在 `sCamera::viewports[i].mViewMat/mProjMat`；MHW 同构 | 已验证（结构体原文）；**偏移为我推导** |
| DMC4 相机 yaw @`[esi+0x268]`、yaw target @`[esi+0x260]`、玩家朝向 @`[edx+0x1210]`；相机参数 @0xD0/0xD4/0xE0/0xE4/0xE8 | 已验证（CameraSettings.cpp naked asm 原文） |
| DMC4 相机 mod 用硬编码 RVA 而非 AOB；无 "MT Framework AOB" 约定 | 已验证 |
| MTF 有 DTI 反射系统 + 公开 dumper | 已验证（wiki + Mt.hpp 原文） |
| MTF 变换节点是 `uCoord`：pos 0x30 / quat 0x40 / scale 0x50 / lmat 0x60 / wmat 0xA0（size 0xE0），虚函数 `updateLmat` vtable+0x34、`updateWmat` +0x38 | 已验证（DMC4 ReClass dump + uActor.hpp 字节一致；vtable dump） |
| 32 位 `MtObject`/`MtDTI` 布局与 `get_dti()`→类名的解析算法 | 已验证（`MtFrameworkNodeInfoReader.cs` 源码 + dmc4 dump 中 `get_dti()` 就是 `mov eax,<abs32>`） |
| `MtVector3`/`MtVector4` 均为 0x10 字节步长（含 padding），`MtMatrix` = 0x40 | 已验证 |
| 相机是 `cUnit`，跑在 `sUnit::mMoveLine[32]` 调度器里；DMC4 把 free camera 生成到 MoveLine 17 | 已验证（结构体 + `Unit.cs` 文档 + `sUnit_spawn(obj,17)`） |
| 游戏相机具体在哪条 MoveLine、以及相机 update 的时序保证 | **未验证 / 无可达文档** |
| DMC4 `cCameraPlayer` / `uCameraCtrl` / `SMediator` 完整布局 | 已验证（ReClass dump + static_assert）；`possibleMat5 @0x1B0` 是否为 view 矩阵是**推断** |
| 不存在 `MT-Framework-SDK` / `via-engine` / `MTFramework-Reversed` / `RE6Hook` / `MTFUnpacker` 仓库；praydog / alphazolam 没有 MTF 相机工作 | 已验证（GitHub 搜索 `total_count:0` / 逐仓库枚举） |
| `via::` 命名空间、`mtx_setup`/`mtx_multiply` 之类的 MTF 数学库 | **未找到**（`dmc4_hook` 里连 `via` 这个词都不出现；项目数学全用 glm） |
| RE6 的相机偏移/AOB；RE6 free camera | **未找到**（只有行为类 mod；CE 表是编译后的 .CT） |
| Dragon's Dogma "Free Camera" mod（mods/1110）确实存在，但机制未读到 | 页面 403/不可达 |
| RE6 的 exe 是否含 DTI | **未验证（推断可能有）** |
| MTF DX9 常量：c0=相机位置、c7=相机前向、c2/c3/c4/c6=雾、c9-c11=淡出（**pixel** shader） | 已验证（ShaderEditor.cpp 内嵌 shader 原文） |
| 存在 c0-c3=world / c4-c7=view / c8-c11=proj 的 MTF 约定 | **未找到**（很可能不存在） |
| Vireio 按 shader 常量表名字 `"proj"` 与硬编码寄存器号识别；只改投影并先反投影 | 已验证（源码原文） |
| Vireio 头部追踪只注入 roll，yaw/pitch 未进矩阵 | 已验证 |
| REFramework `VR::on_camera_get_view_matrix` 是 `mtx = current_eye_transform * mtx`（逐眼偏移，非头部旋转） | 已验证（VR.cpp 原文） |
| REFramework VR.cpp 已读区域内无任何 `SetVertexShaderConstantF`/常量缓冲写入 | 已验证（grep 0 命中；末尾 ~60 KB 未读到） |
| UEVR 的注入点是 `CalculateStereoViewOffset` 回调，参数为 position + `UEVR_Rotatorf{pitch,yaw,roll}`；公开 API 无常量缓冲接口 | 已验证（`include/uevr/API.h`） |
| LukeRoss 把游戏相机 yaw/pitch/roll 绑定到头显；交替眼 = 把相机移到眼位；另有未公开的渲染期 "view matrix fix" | 已验证（README 原文）；**无源码** |
| Source engine VR 写的是 `CViewSetup::origin` + `CViewSetup::angles`，逐眼设 `m_ViewToProjection` + `m_bViewToProjectionOverride` | 已验证（Valve `client_virtualreality.cpp`） |
| Valve 源码里存在 `bool IsOrthonormal(VMatrix, float fTolerance)`（单位长 + 互相正交，容差 0.001），但用于 **Assert 校验**而非搜索 | 已验证（原文） |
| Vivecraft 取消 `Camera.alignWithEntity`、`setRotation(eye.getYaw(), -eye.getPitch())`，并替换视图旋转矩阵；注释说明"不能整体替换矩阵，否则粒子会跟着头转" | 已验证（源码） |
| Quake VR 写 `cl.viewangles[YAW/ROLL]` | 已验证（源码） |
| 3DMigoto **没有**自动 view 矩阵检测；`[Hunting]` 是人工打标记循环；其立体来自 NVIDIA 3D Vision 驱动 | 已验证（`d3dx.ini` + 项目 wiki） |
| 3DMigoto 逐字记录了"常量缓冲被游戏复用于其他用途"的故障模式 | 已验证（`d3dx.ini` `[Profile]` 注释原文） |
| MotherVR 走游戏自己的玩家旋转状态 | 已验证（作者发布说明）；**无源码**，具体字段未验证 |
| "(b) 写引擎相机字段 + (c) hook 相机更新" 是 VR mod 的主流 | 在各项目上分别已验证；"主流"是综合判断 |
| "拉伸 ⇒ 不是纯 view 矩阵" | 推断（线性代数结论 + Vireio 的 invertProjection 实证 + 3DMigoto 缓冲复用实证） |
| 有没有 MT Framework 的 VR/头追踪 mod | **未找到**（多轮检索无结果） |
| vorpX 的机制 | **完全未覆盖**（所有相关站点 403/不可达） |
| HL2VR 自身 fork / Doom 3 BFG VR / Jedi FO VR / Elden Ring VR / Dark Souls VR 的相机代码 | **无公开源码**（不要引用 GitHub 上同名的 UE4 资产仓库） |

---

## 7. 参考 URL（我实际抓取并阅读过的）

**REFramework（praydog）**
- https://github.com/praydog/REFramework
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/FirstPerson.cpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/Hooks.cpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/FreeCam.cpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/Camera.cpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/shared/sdk/RETransform.cpp
- https://ghproxy.net/https://raw.githubusercontent.com/praydog/REFramework/master/shared/sdk/CameraSystemDispatch.hpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/vr/D3D11Component.cpp

**MT Framework / DMC4 / DDDA**
- https://github.com/muhopensores/dmc4_hook （README: `.../master/README.md`）
- https://ghproxy.net/https://raw.githubusercontent.com/muhopensores/dmc4_hook/master/src/CMakeLists.txt
- https://ghproxy.net/https://raw.githubusercontent.com/muhopensores/dmc4_hook/master/src/mods/DebugCam.cpp
- https://ghproxy.net/https://raw.githubusercontent.com/muhopensores/dmc4_hook/master/src/mods/CameraSettings.cpp
- https://ghproxy.net/https://raw.githubusercontent.com/muhopensores/dmc4_hook/master/src/sdk/World2Screen.cpp
- https://github.com/jaryn-kubik/ddda-dinput8 （`Cheats.cpp`）
- https://github.com/chrispurnell/ddda-dinput8
- https://github.com/Ezekial711/MonsterHunterWorldModding/wiki/The-DTI-and-MtFramework-2.0 （raw: `https://ghproxy.net/https://raw.githubusercontent.com/wiki/Ezekial711/MonsterHunterWorldModding/The-DTI-and-MtFramework-2.0.md`）
- https://github.com/Andoryuuta/MHW-ClassPropDump （`src/Mt.hpp`, `README.md`）
- https://github.com/Andoryuuta/MHW-DTI-Dumps
- https://github.com/Fexty12573/SharpPluginLoader （MTF 2.0 `Camera` / `Viewport` / `CameraSystem` C# 绑定，经子代理读取）

**Vireio Perception（DX9 VR 驱动）**
- https://github.com/mscoder610/Perception
- `DxProxy/DxProxy/D3DProxyDeviceAdv.cpp`、`D3DProxyDeviceEgo.cpp`、`D3DProxyDeviceUnreal.cpp`、`D3DProxyDeviceFixed.cpp`、`D3DProxyDevice.cpp`、`Direct3DDevice9.cpp`、`StereoView.cpp`、`MotionTracker.h`

**矩阵识别方法论**
- https://zero-irp.github.io/ViewProj-Blog/part-2.1-view-matrix/
- https://zero-irp.github.io/ViewProj-Blog/part-2.3-View-Projection-matrix/
- https://zero-irp.github.io/ViewProj-Blog/part-3-finding-and-reversing-matrices/
- https://leotorrez.github.io/modding/guides/hunting
- https://deepwiki.com/bo3b/3Dmigoto/3.1-hunting-system （LLM 生成，仅作线索）

**RE 社区相机数据**
- https://residentevilmodding.boards.net/thread/12865/fov-fix （RE:R 相机 AOB，逐字核对）
- https://residentevilmodding.boards.net/thread/4333/camera-distance-change （RE5 相机参数在 .CCL/.CHN 数据文件）
- https://www.nexusmods.com/residentevil6/mods/317 （RE6 "Less Intrusive Cameras"，**未能抓取，403**）

**其他 VR mod（子代理读取；标注了验证方式）**
- https://cdn.jsdelivr.net/gh/praydog/UEVR@master/include/uevr/API.h ＋ https://docs.uevr.io/ ＋ https://data.jsdelivr.com/v1/packages/gh/praydog/UEVR@master （文件清单）
- https://github.com/LukeRoss00/gta5-real-mod （只有 README；README 原文已引）
- https://github.com/ValveSoftware/source-sdk-2013 → `sp/src/game/client/client_virtualreality.cpp`（经镜像 `https://git.dgby.dev/JohnPeel/source-sdk-2013/raw/<commit>/sp/src/game/client/client_virtualreality.cpp` 读取）
- https://cdn.jsdelivr.net/gh/bo3b/3Dmigoto@master/Dependencies/d3dx.ini ＋ https://github-wiki-see.page/m/bo3b/3Dmigoto/wiki/Using-3Dmigoto-to-find-and-fix-shaders
- https://github.com/vittorioromeo/quakevr （`Quake/vr.cpp`、`Quake/view.cpp`、`Quake/gl_rmain.cpp`）
- Vivecraft（`Multiloader-26.2` 分支：`CameraVRMixin`、`RenderHelper`、`VRPlayer`）
- https://github.com/Nibre/MotherVR （只有 README）

**工具 / 环境**
- `https://ghproxy.net/` + `https://raw.githubusercontent.com/...` —— 本环境唯一能可靠读取**任意** raw 文件（含 `.hpp` 与 wiki `.md`）的方式。实测可用的等价镜像：`https://gh-proxy.com/raw.githubusercontent.com/...`（wiki 路径形如 `.../raw.githubusercontent.com/wiki/OWNER/REPO/Page.md`）。
- 其他可用：`https://cdn.jsdelivr.net/gh/OWNER/REPO@BRANCH/PATH`（仅 `.cpp`/`.md`，`.hpp` 会返回 `application/octet-stream`）、`https://data.jsdelivr.com/v1/packages/gh/OWNER/REPO@BRANCH?structure=flat`（列文件）、`https://api.github.com/repos/.../contents/...`（有速率限制）。
- **不可用**：直连 `raw.githubusercontent.com`、`cdn.statically.io`、`raw.githack.com`、`r.jina.ai`、`web.archive.org`、`helixmod.blogspot.com`、`mtbs3d.com`、`vorpx.com`、`nexusmods.com`、`stackoverflow.com`、`patreon.com`；本环境 shell（pwsh）**无网络**。
