# Head-tracked camera rotation injection in three flatscreen-PC VR mods

Scope: camera rotation/pose mechanism only. Stereo rendering mentioned only where it explains the camera path.

Evidence labels: **VERIFIED** = I read the primary source and can give a verbatim quote. **INFERENCE** = reasoning from what I read. **UNVERIFIED** = could not confirm.

## Access notes (affects what could be verified)

- `raw.githubusercontent.com` and `github.com/.../raw/...` are blocked in this environment. Source files were read through the jsDelivr GitHub mirror (`https://cdn.jsdelivr.net/gh/OWNER/REPO@BRANCH/PATH`). Repo trees/lists came from `api.github.com` until it returned HTTP 403 rate-limit, then from `https://data.jsdelivr.com/v1/packages/gh/...` and GitHub Atom feeds. (VERIFIED, operational fact.)
- `api.github.com` rate-limited mid-research; `github.com` HTML pages return mostly navigation chrome to this fetcher, so issue/wiki/release HTML pages were unusable. Release notes were obtained via `releases.atom`. (VERIFIED.)
- Large files are truncated (~100 KB) by the fetch tool. REFramework `src/mods/VR.cpp` is 160,098 bytes, so only roughly the first 2,727 lines were readable. Anything below is marked accordingly.

---

## (A) Minecraft — Vivecraft (`Vivecraft/VivecraftMod`)

Repo default branch is **`Multiloader-26.2`** (VERIFIED: `api.github.com/repos/Vivecraft/VivecraftMod` returns `"default_branch":"Multiloader-26.2"`; a `master` tree request returns 404). All paths below are under that branch. This is a modern (Minecraft 1.21-era, official Mojang mappings) codebase: the legacy names in the brief (`EntityRenderer`, `orientCamera`, `rotationYaw`, `setVRRotation`, `hmdRot`) do **not** appear in the files I read; the equivalents are `Camera`, `GameRenderer`, `Entity.setYRot/setXRot`, `Camera.setRotation`.

### A.1 The core: the camera mixin replaces the engine's own camera orientation

File: `common/src/main/java/org/vivecraft/mixin/client_vr/CameraVRMixin.java` — `@Mixin(Camera.class)`.

The engine method that vanilla uses to place/orient the camera from the player entity is cancelled outright, and Vivecraft writes the VR pose instead (VERIFIED):

```java
@Inject(method = "alignWithEntity", at = @At("HEAD"), cancellable = true)
private void vivecraft$setOrientation(float partialTicks, CallbackInfo ci) {
    if (!RenderPassType.isVanilla()) {
        this.vivecraft$setupVRCamera();
        ci.cancel();
    }
}
```

and the pose is written into the engine camera through the vanilla setters, with an explicit comment on why only Euler yaw/pitch is used (VERIFIED, same file):

```java
VRData.VRDevicePose eye = dataholder.vrPlayer.getVRDataWorld().getEye(renderpass);
this.setPosition(eye.getPosition());
// we cannot set the rotation to the full matrix, because particles would rotate with the head
// instead of being world up oriented
this.setRotation(eye.getYaw(), -eye.getPitch());
```

**Answer to "fields or matrix": both, deliberately split.**
- Euler yaw/pitch (derived from the HMD direction) go into the engine `Camera` fields.
- The full HMD rotation matrix replaces the engine's view-rotation matrix (VERIFIED, same file):

```java
@Inject(method = "getViewRotationMatrix", at = @At("HEAD"), cancellable = true)
private void vivecraft$vrModelView(Matrix4f dest, CallbackInfoReturnable<Matrix4f> cir) {
    if (!RenderPassType.isVanilla()) {
        cir.setReturnValue(dest.set(RenderHelper.getVRModelView(ClientDataHolderVR.getInstance().currentPass)));
    }
}
```

File: `common/src/main/java/org/vivecraft/client_vr/render/helpers/RenderHelper.java` (VERIFIED):

```java
public static Matrix4f getVRModelView(RenderPass renderPass) {
    return DATA_HOLDER.vrPlayer.getVRDataWorld().getEye(renderPass).getMatrix().transpose();
}
```

`VRDevicePose.getMatrix()` itself is `new Matrix4f().rotationY(VRData.this.rotation_radians).mul(this.matrix)` (VERIFIED, `client_vr/VRData.java`), i.e. the raw HMD quaternion as a 4x4 with the room-rotation applied.

### A.2 Where the Euler angles come from

File: `common/src/main/java/org/vivecraft/client_vr/VRData.java`, inner class `VRDevicePose` (VERIFIED):

```java
public float getYawRad()   { Vector3f dir = this.getDirection(); return (float) Math.atan2(-dir.x, dir.z); }
public float getPitchRad() { Vector3f dir = this.getDirection(); return (float) Math.asin(dir.y / dir.length()); }
public float getRollRad()  { return (float) -Math.atan2(this.matrix.m01(), this.matrix.m11()); }
```

So the head quaternion is reduced to a yaw/pitch pair for the engine entity, and roll is discarded on that path (roll survives only in the matrix path of A.1). The eye poses themselves come from `MCVR` (`client_vr/provider/MCVR.java`: `hmdRotation`, `getEyeRotation(RenderPass)`, `getEyePosition(RenderPass)`) — VERIFIED by field usage in the `VRData` constructor; `MCVR.java` itself (82 KB) was not read in full.

### A.3 Per-render-pass "render view entity" (RVE) swap

Vivecraft runs the world once per eye/pass and, for each pass, temporarily overwrites the Minecraft camera entity's transform fields with the pose for that pass, then restores them.

Driver, `client_vr/render/helpers/VRPassHelper.java` (VERIFIED):

```java
((GameRendererExtension) MC.gameRenderer).vivecraft$cacheRVEPos(MC.getCameraEntity());
((GameRendererExtension) MC.gameRenderer).vivecraft$setupRVE();
MC.gameRenderer.update(deltaTracker);
...
// restore player
((GameRendererExtension) MC.gameRenderer).vivecraft$restoreRVEPos(MC.getCameraEntity());
```

Implementation, `mixin/client_vr/renderer/GameRendererVRMixin.java` (VERIFIED) — this is the raw engine-field write the brief asks about:

```java
public void vivecraft$setupRVEAtDevice(VRData.VRDevicePose eyePose) {
    if (this.vivecraft$cached) {
        Vec3 eye = eyePose.getPosition();
        Entity entity = this.minecraft.getCameraEntity();
        entity.setPosRaw(eye.x, eye.y, eye.z);
        ...
        entity.setXRot(-eyePose.getPitch());
        entity.xRotO = entity.getXRot();
        entity.setYRot(eyePose.getYaw());
        entity.yRotO = entity.getYRot();
        if (entity instanceof LivingEntity livingEntity) {
            livingEntity.yHeadRot = entity.getYRot();
            livingEntity.yHeadRotO = entity.getYRot();
        }
        entity.eyeHeight = 0.0001F;
    }
}
```

`vivecraft$cacheRVEPos` / `vivecraft$restoreRVEPos` save and restore `pos`, `xOld/yOld/zOld`, `xo/yo/zo`, `xRot/xRotO`, `yRot/yRotO`, `yHeadRot/yHeadRotO` and `eyeHeight` (VERIFIED, same file). `eyeHeight = 0.0001F` is set "non 0 to fix some division by 0 issues" (source comment).

So: **Vivecraft writes engine entity fields directly and restores them around each VR pass** — it does not leave HMD rotation in the entity all frame.

### A.4 The logic tick also gets the HMD rotation (persistent write)

File: `common/src/main/java/org/vivecraft/client_vr/gameplay/VRPlayer.java`, `doPermanentLookOverride(LocalPlayer, VRData)` (VERIFIED):

```java
} else {
    // use HMD only if no crosshair hit.
    player.setYRot(data.hmd.getYaw());
    player.setYHeadRot(player.getYRot());
    player.setXRot(-data.hmd.getPitch());
}
```
with the preceding comment `// This is used for all sorts of things both client and server side.`

Called from `VRPlayer.postTick()` (VERIFIED):

```java
// Vivecraft - setup the player entity with the correct view for the logic tick.
this.doPermanentLookOverride(this.mc.player, this.vrdata_world_post);
```

Priority order inside that method (VERIFIED): passenger → steering direction; blocking → shield-hand controller yaw/pitch; sprinting/jumping/fall-flying/swimming → configurable CONTROLLER/WAIST/HMD device; crosshair hit present → look at the crosshair point; else → HMD yaw/pitch.

### A.5 Third person / mirror / "smooth camera"

- **First person is forced.** `mixin/client_vr/MinecraftVRMixin.java`, in `vivecraft$switchVRState(boolean)` (VERIFIED):
  ```java
  // force first person camera in VR
  this.vivecraft$lastCameraType = this.options.getCameraType();
  this.options.setCameraType(CameraType.FIRST_PERSON);
  ```
  The saved camera type is restored when VR is switched off.
- **The vanilla perspective key is repurposed** — pressing it changes the desktop mirror mode instead of camera type (VERIFIED, same file):
  ```java
  @WrapOperation(method = "handleKeybinds", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/Options;setCameraType(Lnet/minecraft/client/CameraType;)V"))
  private void vivecraft$changeVrMirror(Options instance, CameraType pointOfView, Operation<Void> original) {
      if (VRState.VR_RUNNING) {
          ClientDataHolderVR.getInstance().vrSettings.setOptionValue(VRSettings.VrOptions.MIRROR_DISPLAY);
  ```
- **"Third person" as a render pass = the desktop/mixed-reality camera, not the player camera.** `RenderPass.THIRD` maps to `VRData`'s `c2` pose, which is either controller/tracker 2 (`mcVR.getAimRotation(2)`) when the MR camera is being moved, or a fixed stored pose (`vrSettings.vrFixedCampos` / `vrFixedCamrotQuat`) (VERIFIED, `VRData` constructor). `VRPassHelper` selects `WorldRenderPass.MIXED_REALITY` for `RenderPass.THIRD`. `CameraVRMixin.vivecraft$setupVRCamera()` is what writes that pose into the engine camera for the pass, so third person reuses the same injection path with a different source pose.
- **"Smooth camera"**: no handling of vanilla `Options.smoothCamera` was found in the files read (`CameraVRMixin`, `GameRendererVRMixin`, `MinecraftVRMixin`, `OptionsVRMixin`, `MouseHandlerVRMixin`) — **no result found**. What *is* there is that mouse turning is disabled and mouse deltas are synthesized from HMD aim deltas, so vanilla mouse smoothing has nothing to act on (VERIFIED for the cancellation; INFERENCE for the consequence):
  ```java
  @WrapWithCondition(method = "turnPlayer", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/player/LocalPlayer;turn(DD)V"))
  private boolean vivecraft$noTurning(LocalPlayer instance, double x, double y) {
      return !VRState.VR_RUNNING;
  }
  ```
  (`MouseHandlerVRMixin.java`; the same file computes `this.accumulatedDX = -yaw * Mth.RAD_TO_DEG * 5F;` from the aim-vector delta for GUI/menu mouse use.)
- The camera is explicitly reset at frame start so a stale pass pose cannot leak (VERIFIED, `MinecraftVRMixin.vivecraft$toggleVRState`): `// reset camera position, if there is one, since it only gets set at the start of rendering, and the last renderpass can be anywhere`, followed by `this.gameRenderer.mainCamera().update(this.deltaTracker);`.

### A.6 Summary for (A)

Vivecraft injects HMD orientation at three levels: (1) engine camera Euler fields via `Camera.setRotation(yaw, -pitch)` from a mixin that cancels `Camera.alignWithEntity`; (2) the engine camera's view-rotation matrix wholesale via `Camera.getViewRotationMatrix`; (3) the camera entity's `yRot/xRot/yHeadRot` fields, both persistently each logic tick (`VRPlayer.doPermanentLookOverride`) and temporarily per render pass (`GameRendererVRMixin.vivecraft$setupRVEAtDevice`, bracketed by cache/restore). It never post-multiplies onto the engine's own view matrix — it replaces it.

---

## (B) praydog — REFramework VR (RE2 / RE3 / RE7 / RE8)

Repo `praydog/REFramework`, branch `master`. Note: the per-game files the brief lists (`src/mods/vr/RE2.cpp`, `RE3.cpp`, `RE7.cpp`, `RE8.cpp`) **do not exist on master** — `src/mods/vr/games/` contains only `RE8VR.cpp` / `RE8VR.hpp` (VERIFIED via `api.github.com/repos/praydog/REFramework/contents/src/mods/vr/games`). A commits query for `src/mods/vr/RE2.cpp` returns `[]` (VERIFIED, empty result). RE2/RE3 first-person + VR camera work lives in `src/mods/FirstPerson.cpp`; RE7/RE8 in `src/mods/vr/games/RE8VR.cpp` plus the Lua script `scripts/re8_vr.lua`.

### B.1 What is hooked (native function hooks, not a constant buffer)

File: `src/mods/Hooks.cpp` (VERIFIED):

```cpp
std::optional<std::string> Hooks::hook_camera_get_view_matrix() {
    auto func = sdk::find_native_method("via.Camera", "get_ViewMatrix");
    ...
    auto native_func = utility::calculate_absolute(ref->addr + 4);
    // Hook the native function
    m_camera_get_view_matrix_hook = std::make_unique<FunctionHook>(native_func, camera_get_view_matrix_hook);
```

The dispatcher calls every mod's `on_camera_get_view_matrix(camera, result)` (VERIFIED, same file, `camera_get_view_matrix_hook_internal`), which lands in `VR::on_camera_get_view_matrix`, `src/mods/VR.cpp` (VERIFIED):

```cpp
if (camera != sdk::get_primary_camera()) {
    return;
}
auto& mtx = *result;
//get the flipped eye to get the correct transform. something something right->left handedness i think
const auto current_eye_transform = get_current_eye_transform(true);
// Apply the complete eye transform. This fixes the need for parallel projections on all canted headsets like Pimax
mtx = current_eye_transform * mtx;
```

That is a **pre-multiply of the full per-eye HMD transform onto the engine's returned view matrix** — i.e. the final camera matrix is produced in the engine's own `via.Camera.get_ViewMatrix` call, before any rendering API sees it.

Other hooks in `Hooks.cpp` (all VERIFIED):
- `hook_camera_get_projection_matrix()` — same pattern on `via.Camera.get_ProjectionMatrix`; `VR::on_camera_get_projection_matrix` overwrites `*result = get_current_projection_matrix(false);`.
- `hook_view_get_size()` — `via.SceneView.get_Size` (render-target size spoof).
- `hook_update_camera_controller()` — **RE2/RE3 only**: `sdk::find_native_method(game_namespace("camera.PlayerCameraController"), "updateCameraPosition")` wrapped in a `FunctionHook`; comment `// Can be found by breakpointing camera controller's worldPosition`.
- `hook_update_camera_controller2()` — **RE2/RE3 only**: `camera.TwirlerCameraControllerRoot.update`; comment `// Can be found by breakpointing camera controller's worldRotation`.
- `hook_update_transform()` — pattern-scanned `UpdateTransform` job function (`m_update_transform_hook`), feeding `Mod::on_pre_update_transform` / `on_update_transform`.
- `hook_application_entry()` / `hook_all_application_entries()` — replaces `via.Application` entry function pointers so mods get `on_pre_application_entry` / `on_application_entry` per engine phase.

**No constant-buffer/shader-level camera rotation hook was found.** The D3D11/D3D12 components exist (`src/mods/vr/D3D11Component.cpp`, `D3D12Component.cpp`) and handle eye submission/present, but the camera pose is injected at the engine API level. (VERIFIED that projection and view matrices are overridden in `via.Camera` hooks; the D3D components' internals were not read — no result found for a constant-buffer rotation path.)

### B.2 Rotation composed in world space: `VR::apply_hmd_transform`

File: `src/mods/VR.cpp` (VERIFIED):

```cpp
void VR::apply_hmd_transform(::REJoint* camera_joint) {
    auto rotation = m_original_camera_rotation;
    auto position = m_original_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);
    }
}
```

```cpp
void VR::apply_hmd_transform(glm::quat& rotation, Vector4f& position) {
    const auto rotation_offset = get_rotation_offset();
    const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{get_rotation(0)});
    ...
    if (!m_decoupled_pitch->value()) {
        camera_rotation = rotation;
        new_rotation = glm::normalize(rotation * current_hmd_rotation);
    } else if (m_decoupled_pitch->value()) {
        // facing forward matrix
        const auto camera_rotation_matrix = utility::math::remove_y_component(Matrix4x4f{rotation});
        camera_rotation = glm::quat{camera_rotation_matrix};
        new_rotation = glm::normalize(camera_rotation * current_hmd_rotation);
    }
    auto current_relative_pos = rotation_offset * (get_position(0) - m_standing_origin);
    ...
    rotation = new_rotation;
    position = position + current_head_pos;
}
```

This is a **post-multiply of the HMD quaternion onto the engine camera rotation**, then a direct write into an engine `REJoint` (rotation always, position if positional tracking is on). The joint-based path is entered from `VR::update_camera_origin()`, which is called by `VR::update_camera()` (VERIFIED):

```cpp
    if (!inside_on_end) {
        m_original_camera_position = sdk::get_joint_position(camera_joint);
        m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
        ...
    }
    apply_hmd_transform(camera_joint);
```

`VR::restore_camera()` exists and is guarded by `m_needs_camera_restore`, with an `inside_on_end` re-entrancy flag (VERIFIED for the functions' existence and guards; the call sites of `update_camera()` / `restore_camera()` lie beyond the readable part of the 160 KB `VR.cpp` — UNVERIFIED).

### B.3 Historical vs current: the camera-controller struct write was retired

`src/mods/VR.cpp` (VERIFIED — the body is now empty and the old code is commented out):

```cpp
void VR::on_update_camera_controller(RopewayPlayerCameraController* controller) {
    // get headset rotation
    /*const auto& headset_pose = m_game_poses[0];
    ...
    *(glm::quat*)&controller->worldRotation = glm::quat{ headset_rotation  };
    controller->worldPosition += get_current_offset();*/
}
```

However the controller-struct write is very much alive in the RE2/RE3 first-person path — `src/mods/FirstPerson.cpp` (VERIFIED):

```cpp
        CAMSYS(m_camera_system, cameraController)->worldPosition = *(Vector4f*)&camera_pos;
        CAMSYS(m_camera_system, cameraController)->worldRotation = *(Vector4f*)&final_quat;

        transform->get_position() = *(Vector4f*)&camera_pos;
        transform->get_angles() = *(Vector4f*)&final_quat;

        // Apply the new matrix
        *(Matrix3x4f*)&mtx = final_mat;
```
and
```cpp
            CAMCTRL(m_player_camera_controller, pitch) = m_last_controller_angles.x;
            CAMCTRL(m_player_camera_controller, yaw) = m_last_controller_angles.y;
```
with the source comment above them: `// These are what control the real rotation, so only set it in a cutscene or something`.

The HMD is composed in that function as (VERIFIED, `FirstPerson::update_camera_transform`):
```cpp
    auto& mtx = transform->get_world_transform();
    ...
    const auto real_headset_rotation = vr->get_rotation(0);
    ...
        if (is_first_person_allowed()) {
            final_mat *= headset_rotation;
            vr->recenter_view(); // only affects third person/cutscenes
        } else {
            final_mat = vr->get_last_render_matrix();
        }
```
and the joint is finally written (VERIFIED, same function):
```cpp
        if (joint != nullptr) {
            sdk::set_joint_rotation(joint, m_last_camera_matrix);
            sdk::set_joint_position(joint, m_last_camera_matrix[3]);
        }
```

So for RE2/RE3 the answer to "engine camera fields or matrix" is: **both** — the engine camera controller's `worldRotation`/`worldPosition` (and its `pitch`/`yaw` members when leaving cutscenes), the camera `RETransform`'s position/angles/worldTransform matrix, and the transform's joint 0.

### B.4 RE7/RE8: hooked Lua camera update → C++ pose write into `app.PlayerCamera`

`scripts/re8_vr.lua` (VERIFIED):

```lua
-- Normal Ethan camera
sdk.hook(
    sdk.find_type_definition("app.PlayerCamera"):get_method("lateUpdate"), 
    on_pre_player_camera_update, 
    on_post_player_camera_update
)
```
with
```lua
local function fix_player_camera(player_camera)
    if true then
        re8vr:fix_player_camera(player_camera)
        return
    end
```
and, for RE7, the same pair is installed on `app.CH8PlayerCamera:lateUpdate()` and `app.CH9PlayerCamera:lateUpdate()` (VERIFIED). Camera shake is separately zeroed by hooking `app.PlayerCamera:updateCameraShakeValue` and writing `player_camera:set_field("MovementShakePosition", zero_vec)` (VERIFIED).

`re8vr:fix_player_camera` is the C++ `RE8VR::fix_player_camera(::REManagedObject* player_camera)` in `src/mods/vr/games/RE8VR.cpp`. It reads the camera's own matrix, composes the HMD pose, and writes it back into **named managed fields of the engine camera object** (VERIFIED):

```cpp
    auto original_camera_matrix = sdk::call_object_func_easy<Matrix4x4f>(camera, "get_WorldMatrix");
    auto original_camera_rotation = glm::quat{original_camera_matrix};
    auto updated_camera_pos = original_camera_matrix[3];

    vr->apply_hmd_transform(original_camera_rotation, updated_camera_pos);
```
```cpp
    auto camera_rot_no_shake_field = sdk::get_object_field<glm::quat>(player_camera, "<CameraRotation>k__BackingField");
    ...
    vr->apply_hmd_transform(camera_rot_no_shake, zero_v4);
    vr->apply_hmd_transform(camera_rot, camera_pos);
```
```cpp
    // Transform is used for things like Ethan's light
    // and determining where the player is looking
    sdk::set_transform_position(camera_transform, camera_pos);
    sdk::set_transform_rotation(camera_transform, camera_rot);
    
    // Joint is used for the actual final rendering of the game world
    if (m_is_in_cutscene) {
        sdk::set_joint_position(camera_joint, camera_pos_pre_hmd);
        sdk::set_joint_rotation(camera_joint, camera_rot_pre_hmd);
    } else {
        const auto rot_delta = glm::inverse(camera_rot_pre_hmd) * camera_rot;
        auto forward = rot_delta * Vector3f{0.0f, 0.0f, 1.0f};
        forward = glm::normalize(Vector3f{forward.x, 0.0, forward.z});
        sdk::set_joint_position(camera_joint, camera_pos_pre_hmd);
        sdk::set_joint_rotation(camera_joint, camera_rot_pre_hmd * utility::math::to_quat(forward));
    }
```
and finally the backing fields (VERIFIED):
```cpp
    *camera_rotation_field = fixed_rot;      // <CameraRotation>k__BackingField
    *camera_position_field = camera_pos;     // <CameraPosition>k__BackingField
```
plus, for RE8, `*fixed_aim_rotation_field = fixed_rot;` and the `CameraRotationWithMovementShake` / `CameraRotationWithCameraShake` / `PrevCameraRotation` fields (VERIFIED). Cutscene handling nudges joints back to the pre-HMD pose and slerps the GUI.

Note the split: **the transform gets the full HMD pose; the joint (which drives world rendering) gets only the yaw component of the HMD delta** in the non-cutscene case — the pitch used for rendering comes through the compiled view matrix path of B.1 (INFERENCE from the code structure and from `on_camera_get_view_matrix`'s comment about the "complete eye transform").

### B.5 praydog's own description of the approach

- `README.md` (VERIFIED, verbatim): `* Generic 6DOF VR support for all games` and `* Motion controls for RE2/RE3/RE7/RE8`, under "Included Mods → VR"; install instructions `### VR * Install SteamVR ... * Extract the whole zip file into your corresponding game folder.`; credits `[cursey](https://github.com/cursey/) for helping develop the VR component and the scripting system.`
- A wiki page exists: `https://github.com/praydog/REFramework/wiki/VR-Troubleshooting` (referenced from the README; the page body was not retrievable through this fetcher — navigation chrome only). No design/architecture write-up by praydog describing the camera-injection mechanism was found: **no result found** for a Patreon/blog/release-note explanation of the hook strategy.
- Requested identifiers: `run_hmd_update`, `get_camera_data`, `set_rotation`, `set_position` — **no result found** in the readable portion of `src/mods/VR.cpp` (~first 2,727 lines; the file is 160,098 bytes and was truncated by the fetch tool). What exists there instead: `apply_hmd_transform`, `update_camera`, `update_camera_origin`, `restore_camera`, `get_current_eye_transform`, `set_rotation_offset`, `recenter_view`. The name `CameraData`/`m_camera_data` **does** exist as a member struct in `RE8VR` (VERIFIED from `m_camera_data.last_hmd_active_state`, `m_camera_data.last_gui_quat`, etc. in `src/mods/vr/games/RE8VR.cpp`); `RE8VR.hpp` could not be fetched (jsDelivr returns `unsupported content type "application/octet-stream"` for `.hpp`).
- The VR Lua API surface is registered in `VR::on_lua_state_created` (VERIFIED) and includes `get_rotation`, `get_transform`, `get_current_eye_transform`, `get_current_projection_matrix`, `set_rotation_offset`, `recenter_view`, `get_standing_origin`/`set_standing_origin` — this is what the RE7/RE8 Lua script drives.

### B.6 Summary for (B)

praydog hooks the **engine's camera API natively** (`FunctionHook` on `via.Camera.get_ViewMatrix` and `get_ProjectionMatrix` natives, resolved via `sdk::find_native_method` + pattern scan) and, separately, **writes pose into engine camera state**: `REJoint` rotation/position via `sdk::set_joint_rotation`/`set_joint_position` (`VR::apply_hmd_transform`), the `RETransform` position/angles/world matrix and the `RopewayPlayerCameraController`'s `worldRotation`/`worldPosition`/`pitch`/`yaw` for RE2/RE3 (`FirstPerson::update_camera_transform`), and named managed backing fields `<CameraRotation>` / `<CameraPosition>` of `app.PlayerCamera` for RE7/RE8 (`RE8VR::fix_player_camera`, driven from a Lua `sdk.hook` on `app.PlayerCamera:lateUpdate`). The composition is a quaternion post-multiply in world space (`new_rotation = glm::normalize(rotation * current_hmd_rotation)`) for the joint path and a matrix pre-multiply (`mtx = current_eye_transform * mtx`) for the view-matrix path. No D3D constant-buffer rotation injection was found.

---

## (C) Alien: Isolation — "MotherVR" by Nibre

### C.1 What actually exists

- Repository `Nibre/MotherVR` contains **only a README** — VERIFIED by jsDelivr's file listing: `{"version":"master","files":[{"type":"file","name":"README.md","hash":"...","size":510}]}` (`https://data.jsdelivr.com/v1/packages/gh/Nibre/MotherVR@master`). The commit feed shows exactly four commits, all `Update README.md` / `Initial commit` (VERIFIED, `https://github.com/Nibre/MotherVR/commits/master.atom`).
- Therefore: **no source code exists to read, and the camera-rotation mechanism cannot be verified from source. No write-up by Nibre describing the head-tracking implementation was found** (`no result found`). The repo README's technical content in full is (VERIFIED, `https://cdn.jsdelivr.net/gh/Nibre/MotherVR@master/README.md`): installation = "Drag the dxgi.dll into your Alien: Isolation game folder", settings in `Options->MotherVR`, recalibration with "LB + RB ... or by pressing Ctrl + Alt".
- A 2017 ComputerBase news item claims the mod "implementiert" the never-released Sega demo VR mode and that "der Quellcode ... über GitHub zur Verfügung gestellt" (source code is provided on GitHub) — the second claim is **false** per C.1 above; the first is a journalist's framing, not a Nibre statement (VERIFIED quote, INFERENCE on attribution).
- `eurogamer.net` (403) and `arstechnica.com` (405, human verification) articles were unfetchable; reddit and `old.reddit.com` were unreachable from this environment. `github.com/Nibre/MotherVR/releases` renders as navigation chrome to this fetcher.

### C.2 What Nibre's own release notes do establish (VERIFIED, from `https://github.com/Nibre/MotherVR/releases.atom`)

All quotes are Nibre's release notes text.

- Delivery is a single DXGI/D3D11 proxy: "Drag the `dxgi.dll` (and `openvr_api.dll` if you want SteamVR) into your Alien Isolation game folder." (0.3.0). Disable with `-novr`.
- The game itself has a VR initialization path being driven by the mod: "This has to do with how the game initializes VR, it waits until those videos are over before submitting any frames." (0.1.0, about startup movies).
- Frames are dropped rather than glued to the head when the game forces eye-locked content: "This is because the game glues them to your eyes, and I'd rather drop those frames (for now) than make people sick." (0.3.0, intro/loading screens).
- **Rotation for snap turning is fed through the game's own player-rotation pipeline, not applied as an after-the-fact offset**: "When the game rotates the player, it does this acceleration-smoothing (like mouse-acceleration) between the old rotation and the new one, making it possibly *more* sickening." (0.2.0, explaining why snap turning slipped).
- The mod manipulates the game's own camera-collision "blinders" and head bob: "Because I can now control the blackout blinders directly..." (0.2.0); "**Some Blinding During Cutscenes Disabled**"; "-disableblinders ... if you'd like to disable all geometry intersection blinding"; "**(Practically) All Head Bobbing Removed**" (0.6.0).
- Menus/HUD are re-anchored relative to the camera: "the fixed menus will automatically rotate to align with the ground, and move to a nicer position in front of you" (0.6.0); "Crosshair ... is being rendered onto the flat HUD" (0.4.0).
- Comfort/rotation features are settings, not hard-coded: "Snap Rotation", "Snap Rotation Segments", "Toggle Aim/Motion Sensor" (0.8.0 settings tree); "Recalibration ... now correctly accounts for your rotation. When you interact with objects ... 'forward' will now be the direction you calibrated towards" (0.6.0); "Recenter combo on Xbox is Left Bumper + Right Bumper" (0.3.0).

### C.3 Mechanism: what can and cannot be claimed

- **VERIFIED**: the mod is a `dxgi.dll` proxy (therefore intercepts DXGI/D3D11 present/device creation — it must, to submit stereo frames to the Oculus/OpenVR compositor); the game has its own VR initialization that the mod engages; the mod writes rotation into the game's player-rotation state (per the acceleration-smoothing note) and reaches the game's camera-level effects (blinders, head bob, menu anchoring, crosshair).
- **INFERENCE**: head-tracked rotation is injected into the game's own player/camera transform state, not into a shader constant buffer. The two independent supports for this are (i) the game's own "acceleration-smoothing (like mouse-acceleration)" applies to rotations the mod requests, which only happens if the value enters the game's rotation state, and (ii) the geometry-intersection blinders — driven by the engine's camera collision — fire when the player's head goes into geometry, which requires the HMD head position/orientation to be present in the engine camera transform.
- **UNVERIFIED**: whether any part of the head-tracking path is implemented through Alien: Isolation's Lua scripting (the Cathode engine's Lua VM, which the mod tools/community script mods do expose). No primary or secondary source found stating MotherVR uses Lua — `no result found`. Also UNVERIFIED: the exact engine camera object/fields touched, since there is no source, no symbols, and no Nibre write-up.

---

## URL list — everything actually fetched and read

Vivecraft (repo `Vivecraft/VivecraftMod`, branch `Multiloader-26.2`)
- https://api.github.com/repos/Vivecraft/VivecraftMod
- https://api.github.com/repos/Vivecraft/VivecraftMod/git/trees/master?recursive=1 (404 — wrong branch)
- https://api.github.com/repos/Vivecraft/VivecraftMod/git/trees/Multiloader-26.2?recursive=1
- https://api.github.com/repos/Vivecraft/VivecraftMod/contents/common/src/main/java/org/vivecraft
- https://api.github.com/repos/Vivecraft/VivecraftMod/contents/common/src/main/java/org/vivecraft/client_vr
- https://api.github.com/repos/Vivecraft/VivecraftMod/git/trees/Multiloader-26.2:common/src/main/java/org/vivecraft/mixin?recursive=1
- https://api.github.com/repos/Vivecraft/VivecraftMod/git/trees/Multiloader-26.2:common/src/main/java/org/vivecraft/mixin/client_vr/renderer?recursive=1
- https://api.github.com/repos/Vivecraft/VivecraftMod/git/trees/Multiloader-26.2:common/src/main/java/org/vivecraft/client_vr/gameplay?recursive=1
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/mixin/client_vr/CameraVRMixin.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/mixin/client_vr/renderer/GameRendererVRMixin.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/mixin/client_vr/MinecraftVRMixin.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/mixin/client_vr/MouseHandlerVRMixin.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/mixin/client_vr/OptionsVRMixin.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/client_vr/VRData.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/client_vr/gameplay/VRPlayer.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/client_vr/gameplay/interact_modules/ThirdPersonCameraModule.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/client_vr/render/helpers/RenderHelper.java
- https://cdn.jsdelivr.net/gh/Vivecraft/VivecraftMod@Multiloader-26.2/common/src/main/java/org/vivecraft/client_vr/render/helpers/VRPassHelper.java

REFramework (repo `praydog/REFramework`, branch `master`)
- https://api.github.com/repos/praydog/REFramework/git/trees/master?recursive=1
- https://api.github.com/repos/praydog/REFramework/git/trees/master:src/mods?recursive=1
- https://api.github.com/repos/praydog/REFramework/contents/src/mods/vr
- https://api.github.com/repos/praydog/REFramework/contents/src/mods/vr/games
- https://api.github.com/repos/praydog/REFramework/commits?path=src/mods/vr/RE2.cpp&per_page=5 (empty)
- https://api.github.com/repos/praydog/REFramework/tags?per_page=100 (403 rate limit)
- https://data.jsdelivr.com/v1/packages/gh/praydog/REFramework
- https://data.jsdelivr.com/v1/packages/gh/praydog/REFramework@1.5.9.1?structure=flat (403, package size limit)
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/README.md
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/VR.cpp (truncated at ~2,727 lines by the fetch tool)
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/Hooks.cpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/FirstPerson.cpp (truncated)
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/vr/games/RE8VR.cpp
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/scripts/re8_vr.lua (truncated)
- https://cdn.jsdelivr.net/gh/praydog/REFramework@master/src/mods/VR.hpp (failed: unsupported content type application/octet-stream)
- https://cdn.statically.io/gh/praydog/REFramework/master/src/mods/VR.hpp (failed: fetch failed)
- https://raw.githack.com/praydog/REFramework/master/src/mods/VR.hpp (failed: fetch failed)
- https://github.com/praydog/REFramework/wiki/VR-Troubleshooting (navigation chrome only)

MotherVR (repo `Nibre/MotherVR`)
- https://cdn.jsdelivr.net/gh/Nibre/MotherVR@master/README.md
- https://cdn.jsdelivr.net/gh/Nibre/MotherVR@d9fe9501451b12b7a51f87d8f79dad5be351618e/README.md
- https://data.jsdelivr.com/v1/packages/gh/Nibre/MotherVR@master
- https://github.com/Nibre/MotherVR (chrome)
- https://github.com/Nibre/MotherVR/releases (chrome)
- https://github.com/Nibre/MotherVR/releases.atom (full release notes)
- https://github.com/Nibre/MotherVR/commits/master.atom
- https://github.com/Nibre/MotherVR/issues?q=is%3Aissue (chrome)
- https://www.computerbase.de/news/gaming/alien-isolation-vr.60439/
- https://www.eurogamer.net/alien-isolation-modder-adds-vr-support (403)
- https://arstechnica.com/gaming/2017/07/fan-made-oculus-vr-patch-for-alien-isolation-now-available-for-download/ (405, human verification)
- https://old.reddit.com/r/oculus/search?q=MotherVR&restrict_sr=on&sort=relevance&t=all (fetch failed)

Local artifacts read (fetch spill files, full untruncated text of the fetches listed above)
- `C:\Users\<user>\AppData\Local\Temp\<tmp>\session-<id>\6bcb82b7e9cd-web_fetch.txt` (VR.cpp)
- `C:\Users\<user>\AppData\Local\Temp\<tmp>\session-<id>\c85bc61ecb9f-web_fetch.txt` (RE8VR.cpp)
- `C:\Users\<user>\AppData\Local\Temp\<tmp>\session-<id>\1f21ec2d6f99-web_fetch.txt` (FirstPerson.cpp)
- `C:\Users\<user>\AppData\Local\Temp\<tmp>\session-<id>\ea95ae7e201e-web_fetch.txt` (re8_vr.lua)
- `C:\Users\<user>\AppData\Local\Temp\<tmp>\session-<id>\7164e3bc19da-web_fetch.txt`, `4915549098f0-web_fetch.txt`, `b8b9a9a0aa0d-web_fetch.txt` (repo trees)
