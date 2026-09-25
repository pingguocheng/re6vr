# re6vr — 真立体 VR 渲染（方案 B）交接文档

> 写于 2026-09-25 傍晚，作者：实现头部追踪那一轮的主代理。
> **新会话请先读这一份，再读 README；本文件的目标是让你不必回溯对话就能接手。**

## 0. 一句话现状

游戏相机**已经在我们手上**（朝向每帧可控、无累积漂移、佩戴倾斜已标定），头部追踪在头显里"对味了"。
但画面仍然是**一块固定在虚拟空间里的屏幕**：游戏渲染一次 → 贴到 quad 上 → 两个眼睛去看这块 quad。
所以"画面范围不跟头朝向对应"。方案 B = 改成**每眼渲染一次、直接投影提交**，取消 quad。

## 1. 已经站住的事实（不要重新验证）

| 事实 | 位置/值 |
|---|---|
| 渲染器读的相机姿势 | `uCamera* +0x50` pos / `+0x60` up / `+0x70` target / `+0x4C` fov(度) / `+0x44` near / `+0x40` far |
| 构视图函数 | `BH6.exe+0x1F80B0`（VA `0x005F80B0`），vtable slot 18，家族共享；`ecx`=相机对象，1 个栈参 = `Matrix* out` |
| look-at 构造器 | `0x00E6FD20`（`ecx`=out，栈上 eye/target/up）。**当前头部旋转就写在这里**，prologue `55 8B EC 83 E4 F0 83 EC 40` |
| 相机序列 | `sBioCamera` 单例 = `ds:[0x0186E23C]`；`sBioCamera::Update` = `0x00503880`，遍历 slot（`+0x30`，步长 `0x190`，8 个），把 16 个浮点写进 `mViewportCamera[k] = +0x12A0 + 0x60*k`（矩阵在 `+0x10`） |
| PushOrg | `0x004F9950`（把 src 的 +0x50/0x60/0x70/0x4C 拷进 `mCameraOrg`）；**`mCameraOrg` 是死路，全镜像唯一读者 `0x4FCB70`，不参与渲染** |
| 上一次实现里被证伪/踩过的坑 | 见 §5 |

## 2. 当前代码结构（改哪里）

- `src/cam_steer.cpp` — 头部追踪。钩 `0x00E6FD20`，在其内部把 `eye/target` 就地旋转；绝对映射（头角度 = 相机偏移）、佩戴倾斜标定、tripod 规则、F9 重取参考点、每帧对同一 target 只旋转一次（按写入值判重）。
- `src/d3d9_proxy.cpp` — 设备 vtable 补丁：`Present`(slot 17)、`EndScene`(slot 41)、`Reset`(slot 16)；`CreateDevice` 钩子；各探针安装入口都在 `hook_present()` 里。
- `src/openxr_bridge.cpp` — OpenXR 会话、D3D9→D3D11 共享纹理、**quad 合成器**（当前要改掉的部分）、`XrCompositionLayerProjection` 提交。
- `src/safe_mem.h` — **所有**内存访问走这里（`read/read32/readf32/writable/readable/plausible_pointer`）。**不要再用 `__try/__except`**：实测它会自己 fault 并杀掉进程。
- `scripts/deploy_config.py` — 一键写标记 + 校正日志目录指针 + 部署 DLL + 校验哈希。**改完代码用这个部署**，别手抄。

## 3. 方案 B 的落点（三步）

1. **每眼一份姿势**：给相机一个"眼索引"，左眼/右眼各自的 eye 位置（`base ± right * IPD/2`）与朝向。眼睛位置是 `+0x50`（现在故意不动它）。
2. **每眼一份投影**：`+0x4C` fov（度）、`+0x40` far、`+0x44` near —— 或者用头显每眼 FOV 反算。**注意**：RE6 的 fov 是**垂直还是水平**尚未确证，必须先量（离线脚本可算，见 §6）。
3. **提交路径**：把 quad 换成两个眼纹理直接提交：
   - 现在：`draw_quad(...)` 把游戏帧画进 swapchain 的两个切片，然后 `XrCompositionLayerProjection` 用眼位/眼 FOV 提交 swapchain —— 也就是"通过一块屏看游戏"。
   - 目标：继续用现成的 D3D9→D3D11 共享纹理路径，但**把游戏每眼的结果直接当作该眼的投影纹理**（`XrSwapchainSubImage` + 每眼 `XrPosef`/`XrFovf` 来自头显），不再画 quad。
   - 关键点：**提交时 eye pose 必须用头显的**，而画面是用对应眼的游戏相机渲染的 —— 这样"画面范围 = 头朝向范围"才成立。

### 引擎层未知（子代理正在离线分析，报告将写到 `_work/stereo_render_report.md`）

- 引擎的画帧函数是谁、Present 由谁调用；
- **引擎是否本来就支持一帧两视图（分屏！）** —— 若支持，用它的双视图机制比"渲染两遍整帧"便宜得多，这是首选；
- 若必须渲染两遍：最小的"再渲染一次"调用点及其调用约定、是否可从 Present 钩子里调用（线程/重入/状态）；
- 是否存在独立的"眼睛偏移"（而非相机自身位置）——决定 IPD 该写哪里。

## 3.5 引擎结论（子代理离线报告 `_work/stereo_render_report.md`，717 行，已逐字节核对）

**帧结构与可用的缝：**

| 事实 | 地址/值 |
|---|---|
| Present 全镜像**只有一处调用** | `0x00F41D59`，在 `sRender` 帧末函数 **`0x00F41C20`** 内（`__thiscall`，`ecx` = sRender 单例 `0x0186E8BC`；prologue `51 55 56 8B F1 57 8D 8E A0 00 00 00`） |
| 帧驱动函数 | `0x00511D70`（app vtable `0x0151F670` slot 6）→ 在 `0x00512430` 调 `0x00F41C20` |
| **渲染阶段（一次完整 pass）** | **`0x00F3EA30`**（prologue `83 EC 10 55 57 8B F9 33 ED`，`__thiscall`，`ecx` = sRender）—— 它做完整套：建目标 → 设状态 → 相机更新 → 视图循环 → 绘制 `0x00F3A9E0` → resolve |
| **引擎自己就多 pass** | `0x00F3EA30` 内循环 `0x00F3ECB0..0x00F3ED38`，**按 display 逐个渲染整场景**，并把 display 索引 `push ebx` 传给相机更新 `0x00F3ECCF` |
| display 数量 | `sRender+0x462694`，全镜像只在 `0x00F4301E` 写一次（=1） |
| 第二条交换链机制**已存在** | `0x00F42317`（`IDirect3D9` slot 13 = CreateAdditionalSwapChain），每帧在 `0x00F41DBA` present |
| 视图区域循环（8 槽） | `0x00EFFA30`：遍历 `sBioCamera+0x30+0x190k`（`+0x00` 指针、`+0x04` 相机、`+0x0C` 必须 0、`+0x10` active 字节），只做**区域裁剪**（`0x00EF0200` 是纯 2D 矩形裁剪器，体内零调用）——**不是"画 8 个相机"** |

**必须记住的否定结论：**

- **`mViewportCamera`（`+0x12A0`，步长 0x60）全镜像无人读取** —— 不要再往它上面做立体；
- **不存在任何"眼睛偏移"字段**：`0x5F80B0` 把 `[ecx+0x50]` 原样交给 `0x00E6FD20`，而 `0x00E6FD20` 是**纯 look-at**（163 条指令、无常量偏移）→ **IPD 必须由我们自己在每次构视图时加到 `+0x50`/`+0x70` 上**；
- `0x00E6FD20` 有 71 个调用者（含角色/特效/阴影），**只挂它会把姿势施加到别人身上** → 必须按相机身份过滤（只处理 slot 表里那台：`[[0x186E23C]+0x34+0x190k+0x04]`，上次 dump 只有 slot 0、vtable `0x0152D620`=uCameraCtrl）；
- 单一标量 fov（`+0x4C`）**无法表达每眼非对称视锥** → 每眼投影不能只靠它，最终要在投影矩阵上传前改矩阵。

**推荐实现形状（子代理建议 + 我的判断）：**

1. **不要从 Present 钩子里再渲染一次**（那时 d3d9 的 Present 还在栈上，重入危险）；
2. 首选 **hook `0x00F3EA30`**：先让原始 pass 用左眼姿势跑完，再设右眼姿势**调用一次**——都在帧线程上；
3. 或者走引擎自己的多 pass：把 `displayCount` 提到 2 并给 `mDisplay[1]` 一条交换链/目标（引擎会自己渲两遍），代价是第二条交换链的捕获；
4. 每帧都要打印**引擎自己的证据**（8 个 active 字节、display 索引、`displayCount`），避免"静默无操作"被当成成功。

## 4. 现成的验收工具

- 日志：`_work\re6vr.log`（每次启动被截断；`python scripts\play.py` 会先归档到 `_work\_archive\`）。
- 部署/配置：`python scripts\deploy_config.py --config head|delta|observe|off`，`--gain/--range/--tripod`。
- 崩溃取证：`src/proxy_trace.h` 的 vectored handler 会写 `build\re6vr_crash.txt`（错误码 + 出错指令 + 访问地址）。
- 离线数学校验：`_work\replay_head_pipeline.py`（可直接吃日志文件回放；**注意它的采样密度陷阱**：死区按单步大小判定，稀疏快照会被判为"静止"）。
- 相机探针模式：`re6vr_steer.txt` 支持 `observe` / `pushorg` / `swing` / `head`（前三个只读，`pushorg` 会打印 PushOrg 实际收到的 src 对象）。

## 5. 被证伪的路线（别再走）

| 路线 | 结论与证据 |
|---|---|
| 写 `mCameraOrg`（`+0xE30/+0xE40`） | 死路。全镜像唯一读者 `0x4FCB70`，不参与渲染 |
| 扫描显存/常量流找 view matrix | 对"控制相机"没有帮助；已在 `re6vr_view.txt = off` 下停用 |
| 绕世界 Y 做 yaw | 视线非水平时扫出圆锥 → 画面斜着走 |
| 绕"视线水平投影"构造的轴做 yaw | **不水平也不垂直**：俯视 20° 时左转 90° 会让俯仰从 +20° 掉到 −70°（离线已算）。必须用 `up × gaze` |
| 用 `__try/__except` 保护内存读取 | 实测**自己 fault 并杀进程**（0xC0000005 出现在守卫指令本身）。已全换 VirtualQuery 页校验 |
| 在相机对象的 target 上"累加"角度 | 引擎每帧重建该字段 → 角度漂走/顶到限幅。必须无状态、每帧从引擎值出发 |
| 逐帧增量积分（drag 模型） | 死区会永久丢掉小动作 → 相机与头不"对应"。已改绝对映射 |
| 在 `0xE6FD20` 里给**所有**调用者的参数做旋转 | 该函数有 71 个调用者，其中多数不是相机 → 破坏引擎数据、过场闪退（`execute at DEDEDE`）。已按返回地址白名单只处理 `GetViewMatrix` 的调用点 |

## 6. 下一步的具体待办（按顺序）

1. 读 `_work/stereo_render_report.md`，确定第 3 节的三个引擎层未知。
2. 量 fov 语义（垂直/水平）—— 用 `_work/replay_head_pipeline.py` 同风格的离线脚本，或从引擎调用 `0xE6FD20` 之后构投影矩阵的代码里读出来。
3. 做**单眼直投**最小验证：一次渲染 → 直接用该眼提交，不画 quad。**先证明"方向与范围对应"**，再加第二只眼和 IPD。
4. 双眼：`eye_index` 决定 `eye = base ± right*IPD/2` 与每眼 fov，两次渲染（或引擎双视图），分别提交。
5. 回归检查清单：静止不漂、左右转纯水平、点头 1:1、大角度不倾斜、过场不闪退、非 VR 模式（`re6vr_steer.txt = off`）仍能正常出画面。

## 7. 工作方式（用户明确要求过）

- **不要**在 live 路径上连续"改一处、试一局"：先用离线/观测模式取证，再改可写路径；每次只改一个变量。
- 用户的每一条体感反馈都要在**日志里找到对应数字**再动手（"不动却飘走"→ 日志显示每帧叠加了 2~3 次旋转）。
- 本项目非 ASCII 文本**只能用文件工具写**（PowerShell here-string / Set-Content 会把 UTF-8 转成本机 ANSI 并永久损坏）。

## 8. 进度日志（按轮次追加）

### 轮次 2-3（2026-09-25 晚，本文件作者）

- **相机身份过滤已实现**：`camera_may_be_steered()`（`src/cam_steer.cpp`）。只有 sBioCamera slot 表里的相机可以被头显驱动；同族 8 个类（quake/blur/veh/animation/QFPS/...）一律跳过并打日志。表为空时**不阻断**（否则会静默失效）。注意：该过滤依赖 `g_cam_last`，而它由 `GetViewMatrix` 钩子填充 —— 所以那个钩子在 builder 模式下**必须始终安装**（曾经写成"只在有 FOV 注入时安装"，导致过滤静默失效，已修）。
- **FOV 注入已实现**：`re6vr_fov.txt`（度）写进相机 `+0x4C`。用途：①一局定性 fov 是垂直还是水平（静态搜索未定论）；②每眼投影的落点。
- **每眼 IPD 偏移已实现**：`re6vr_ipd.txt`（米）+ `re6vr_eye_index.txt`（0=左 1=右，-1=关）。偏移沿相机自身右轴 `right = gaze × up`，**eye 与 target 同步平移**（只动 eye 会变成旋转）。默认 ipd=0，完全惰性。**这是双眼渲染的地基**，因为报告已证明引擎里不存在眼睛偏移字段。
- **渲染阶段只读探针已实现**：`re6vr_render_probe.txt = 1` 钩 `0x00F3EA30`，每秒打印引擎自己的 `displayCount` / `Stereo` 标志 / 设备 / primaryScene。**等一次游戏局的数据来决定双眼走哪条路**（引擎自带 Stereo / 多 display / 自己 hook 跑两遍）。
- **部署一键化**：`python scripts\deploy_config.py --config head`（写标记 + 校正 `build\re6vr_logdir.txt` + 拷贝 DLL + 校验哈希）。当前部署 MD5 `9075066E13D71A5DF55B579D1A0855F9`。

**下一步（等数据）**：读 `steer: RENDER phase call ...` 那一行；
- `Stereo flag = 1` → 追查引擎把第二眼画到哪，直接接过来（最便宜）；
- 否则若 `displayCount` 可改 → 走多 display 路；
- 否则 → hook `0x00F3EA30`（帧线程内，**不要**从 Present 钩子调）跑两遍，用下面的单眼测试先验证 IPD 生效。

### 轮次 4-5（同一晚）

**双眼第二 pass 已实现**（默认惰性，`re6vr_stereo.txt = 1` 才启用）：

- 钩 `0x00F3EA30`，`call` + `ret`（**先核对过调用约定**：该函数返回点是裸 `ret` @ `0x00F3ED75`，即 `__thiscall` 由调用者清栈 —— 上一轮在 `0xE6FD20` 上没做这个检查，代价是两局闪退）；
- pass 1 用当前眼跑完原始渲染阶段，pass 2 把 `g_current_eye` 设为 1 再调用一次；
- 三重保护：重入守卫 `g_in_render`、眼索引保存/恢复、只在第一遍正常返回后才做第二遍；
- **两遍目前画进同一个 render target**，所以这一版不会产生立体画面；它证明的是"引擎可以被驱动成每帧渲两遍、每次用不同眼睛姿势"——方案 B 的必要条件。缺的是每眼独立目标 + 各自提交。

**所有已实现能力的开关一览（默认值都是惰性的）**：

| 标记 | 默认 | 作用 |
|---|---|---|
| `re6vr_steer.txt` | head | 主模式（head/observe/swing/pushorg/off） |
| `re6vr_render_probe.txt` | 1 | 渲染阶段只读探针（displayCount / Stereo flag） |
| `re6vr_fov.txt` | 0 | 覆盖相机 fov（度）；用来定性 fov 语义 + 每眼投影 |
| `re6vr_ipd.txt` | 0 | 瞳距（米）；0 = 关 |
| `re6vr_eye_index.txt` | -1 | 强制眼索引（0 左 / 1 右）；配合 ipd 做单眼偏移测试 |
| `re6vr_stereo.txt` | 0 | 双 pass 渲染（实验） |

**当前阻塞（唯一）**：需要一次游戏局来读 `steer: RENDER phase call N: ... displayCount N, Stereo flag N ...`，据此在三条候选路里选一条。在那之前继续写代码只是猜测。

### 轮次 6-7：阻塞确认 + 最后一块离线部件

- **新增：头显实际 fov 上报** —— `openxr_bridge.cpp` 每帧提交时把真正交给运行时的**垂直 fov（度）**发布为 `bridge_submitted_fov_v_deg()`，另配 `bridge_submitting()` 区分"没戴头显"与"戴了但还没提交帧"。用途：现在游戏按自己的 37° 垂直 fov 渲染、再贴到一块虚拟屏 → 玩家只看到中间一小块。要让画面**填满头显视野**，游戏 fov 必须被驱动到这个值，而这是唯一正确的来源。配合已有的 `re6vr_fov.txt`（写相机 `+0x4C`）即可做"fov 匹配"。
- **状态：从第 3 轮到第 7 轮，日志一直没有更新**（最后一局仍是 18:56:28）。方案 B 的设计岔路只能靠那次运行的数据消掉，因此目标被标记为 `blocked`，而不是继续猜着改渲染路径。

### 轮次 7 后的实测结论（2026-09-25 19:54 那局，阻塞已解除）

```
steer: RENDER phase call 1: sRender 1E1B5060, displayCount 1, Stereo flag 0, device 3DE77200,
       primaryScene 11E48DF0 | stereo off, second passes 0, guard refusals 0
steer: slot-table camera filter armed: first populated slot camera = 21BF8360
```

**判读 → 走 C 路（自己驱动双 pass），A 和 B 都不可用：**

| 候选路 | 判定 | 依据 |
|---|---|---|
| A. 引擎自带 Stereo | **不可用** | `Stereo flag = 0`（引擎的多 pass 路径没启用） |
| B. 多 display | **不可用** | `displayCount = 1`（虽然技术上可改，但没有第二条交换链/目标，成本高于 C） |
| C. 自己驱动双 pass | **采用** | 渲染阶段钩子已装好、`call 1..52` 稳定到达、`0x00F3EA30` 是裸 `ret` 所以 `call/ret` 安全 |

另外两条已确认的事实：`sRender = 1E1B5060`、`device = 3DE77200`（每秒打印，可直接用于将来的目标切换代码）；身份过滤已生效并且**唯一被驱动的相机是 `21BF8360`**（此前没有任何 `NOT steering camera` 行 —— 说明其它同族相机目前没走到那条路径，过滤器保持武装即可）。

### 新会话立刻要做的事（按顺序）

> **轮次 8 已把下面第 1、2 步做完并离线验收通过 —— 先读文件末尾的「轮次 8」一节，
> 那里有当前部署、离线自检命令和四种判据行。这一节保留为当时的计划原文。**

1. **确认双 pass 真的跑起来**（最小实验，一局）：把 `re6vr_stereo.txt` 设为 `1`、`re6vr_ipd.txt` 设为 `0.063`、`re6vr_eye_index.txt` 设为 `0`，然后 `python scripts\deploy_config.py --config head`（会覆盖标记，注意：该脚本会把这三个标记重置为默认，**要么改脚本的 head 配置，要么部署后手工改标记**），进关卡看日志：
   - `second passes N` 必须递增 → 证明"每帧渲两遍"成立；
   - `IPD 0.063 m applied to eye 0: eye moved ...` → 证明眼睛偏移真的写进了渲染器；
   - 画面会是两遍叠加/闪烁（两遍共用同一个 render target），**这是预期的**，不是 bug。
2. **每眼独立目标**（方案 B 的主体工程）：需要定位引擎"绑定 display 目标"的那个函数（sRender 渲染阶段里 `0x00F3ECB9` 附近 `call 0x0109D7F0` 的调用点，报告 §(a)3 有引用），在 pass 2 之前把目标换到第二张纹理；两张纹理再分别作为两眼提交。
3. **提交路径**：`openxr_bridge.cpp` 里 `draw_quad(...)` → 改成"该眼纹理直接作为该眼投影"。已备好的接口：`bridge_submitted_fov_v_deg()`（头显实际垂直 fov，用来把游戏 fov 驱动到匹配，否则画面只占中间一小块）。
4. **回归检查**：`re6vr_stereo.txt = 0` 时必须回到当前已验证的行为（静止不漂、左右纯水平、点头 1:1、大角度不倾斜、过场不闪退）。

### 历史部署（轮次 7 起点，已被轮次 8 的 stereo 部署取代）

MD5 `A37EA9CEDC374852E2A92BF08034AB19`；标记全部为惰性默认：`steer=head`、`method=1`、`absolute=1`、`tripod=1`、`gain=1.0`、`range=60`、`deadzone=1.5`、`render_probe=1`、`fov=0`、`ipd=0`、`eye_index=-1`、`stereo=0`、`view=off`。

（下面这四步是**已经完成**的历史步骤，留作记录：阻塞的那局数据已于 2026-09-25 19:54 拿到，见上一节。）

1. 确认部署：`python scripts\deploy_config.py --config head`（会自动校正日志目录指针并校验哈希）。
2. 让玩家**进关卡玩十几秒**（不需要头显）。—— 已完成
3. 读日志里这两行：—— 已读到，结论 = C 路
   - `steer: RENDER phase call N: sRender 1E1B5060, displayCount 1, Stereo flag 0, ...`
   - `steer: slot-table camera filter armed: first populated slot camera = 21BF8360`
4. 按 `displayCount` / `Stereo flag` 选路 —— 已选定 **C（自己驱动双 pass）**，见上一节的"新会话立刻要做的事"。

---

### 轮次 8（2026-09-25 晚）：四步里的第 1、2 步已实现并离线验证

**先修掉一个会让第 1 步"白跑"的 bug**（这是本轮最重要的一条）：`builder_record` 里
`if (extra_yaw == 0 && extra_pitch == 0) return;` 排在 IPD 那一段**之前**——而头部正前方时
published yaw/pitch 正是 0。也就是说：**只要玩家不转头，眼偏移一次都不会写**，日志里连一行
`IPD ... applied` 都不会有，而那看起来和"引擎忽略了眼偏移"一模一样（本项目最贵的那类假结论）。
现在提前返回的条件是"既没有旋转也没有眼偏移"，并且加了一条硬证据行：

```
steer: IPD 0.063 m applied to eye 0 (#N write(s)): eye (...) -> (...), right axis (...), pose source: engine refresh
steer: eye 0 pose call: engine refresh | eye (...) target (...) | neither is ours
steer: STEREO: second passes 1234, guard refusals 0, eye-offset writes 2468, active eye 0 (ipd 0.0630 m)
```

**每眼一份姿势缓存**（`builder_record`）：姿势去重原来是按"target 指针"做的，而双 pass 渲染
会**两次构建同一台相机的姿势**，于是 pass 2 走进 pass 1 的缓存、判定"这是我们自己写的、
引擎没刷新"然后直接 return —— 右眼会静默复用左眼的姿势，立体画面是假的。现在缓存键是
`(target, eye)`，每个眼各有一份基线，并且每次调用都把三种情况之一打进日志：
`engine refresh` / `our own output` / `first sighting of this (pose, eye)`。
**已知残余风险（下一次跑数据说话）**：如果引擎两个 pass **复用同一个姿势缓冲区且不重建**，
那么某个眼第一次被看到时读到的是**另一只眼注入后的值**。日志里 `first sighting` 与
`engine refresh` 的行数比例就是判据。

**每眼独立目标 + 每眼一份纹理（第 2 步）**，无需与引擎的目标切换机制搏斗：

| 眼 | 取帧时刻 | 在哪取 | 为什么 |
|---|---|---|---|
| eye 0 | pass 1 返回后、pass 2 之前 | `cam_steer` 的渲染阶段 detour（新 `render_capture_left_eye`） | pass 1 的画面**只存在到 pass 2 覆盖它为止** |
| eye 1 | EndScene | 现有的 `on_end_scene` | 那时设备里就是 pass 2 的画面 |

桥接侧（`openxr_bridge.cpp`）：`re6vr_stereo.txt=1` 时在**设备创建阶段**就建好两套完整链路
（D3D9 staging 纹理 → SYSTEMMEM 回读面 → D3D11 动态纹理 → SRV），因为 MT Framework 在帧内
创建资源会 `ERR09`。合成器按眼选 SRV：**两眼同时切换**（一只眼没抓到就两只眼都退回单纹理路，
绝不出现"右眼显示左眼画面"）。回退发生时画面是普通游戏帧 —— 不立体，但永远不是错配的一对。

**离线验收（不需要游戏，不需要头显）**：新增 `scripts\stereo_selftest.py`，它让代理在真实的
D3D9 设备上建真实的纹理，把两个已知颜色画进 render target，跑**真的** `capture_eye`，再把每个
眼自己的纹理读回来比对。日志里的判据行：

```
selftest-stereo: eye 0 copied frame read back at the centre = FF102040 (put 00102040), 2 capture(s)
selftest-stereo: eye 1 copied frame read back at the centre = FFA0C0E0 (put 00A0C0E0), 2 capture(s)
selftest-stereo: eye0 1056832 eye1 10535136, differ=1 correct=1, eye srv=1, uncaptured eye falls back=1 -> PASS
```

一键全跑：`python scripts\check_all.py`（stereo 抓帧 / 面板渲染 / 真实几何 / 相机探针，当前**全 PASS**）。
两个踩过的坑记在这里：① 这个自检**必须从 EndScene 调**，从 Present 里调会因为帧内创建 D3D9
资源而崩（`ERR09` 的那条规矩同样适用于自检）；② 面板自检和抓帧自检会抢 `shared_srv`，
`frame_srv_for_eye()` 在 `in_selftest` 期间必须交回原样，否则面板自检会误报"四个象限全黑"。

**部署与配置**：`python scripts\deploy_config.py --config stereo`（新增）会写
`steer=head`、`render_probe=1`、**`ipd=0.063`、`eye_index=0`、`stereo=1`**；
`--ipd <米>` 可单独改瞳距。当前已部署 MD5 `AB7C44BE9819A8D8955216B329923074`（stereo 配置）。

**读一局结果的工具**：`python scripts\stereo_report.py`（默认读 `_work\re6vr.log`），
把"引擎是否渲了两遍 / 两次是否用了不同眼姿势 / 是否抓到两张不同的图 / 每眼是否各采各的"
分成四条**独立**结论打印，并列出所有 WARN/FAIL。判读请照这四条，不要把其中一条当成另一条。

**第 3 步（直投提交）现状**：还没做。它需要先把**游戏 fov 驱动到头显 fov**，否则把 37° 的画面
铺满 ~100° 的显示只会得到几何拉伸。桥接侧的接口已经在了（`bridge_submitted_fov_v_deg()`），
而 `re6vr_screen_scale.txt` / `re6vr_screen_dist.txt` 可以把 quad 提交铺满显示（k=1）——
两者配合是本轮之后"画面范围 = 头朝向范围"的最小改动，但那是**下一个变量**，不与本轮一起改。

**第 4 步（回归）**：`re6vr_stereo.txt = 0` 时本轮的代码路径应当完全惰性：不建 per-eye 资源、
不装抓帧、`frame_srv_for_eye()` 直接返回原来的 `shared_srv`。离线面板自检在 stereo=0/1 两种
情况下都 PASS。**还没有用真机跑过**——第一局就跑 stereo=1，若画面异常，改用
`--config head` 重跑一局做对照。

---

### 轮次 9（2026-09-25 晚）：四局崩溃的结论 + 共享纹理改造（未验完）

#### 崩溃真相：**每一局都崩在"把画面从 GPU 读回 CPU"这条链上**

四局全部崩在同一个签名（系统 WER + LiveKernelEvent 141 GPU 引擎挂起）：

| 局 | 模式 | 崩溃模块 / 偏移 | 时刻 |
|---|---|---|---|
| 20:19 | 双渲染 + 帧中取帧 | `nvwgf2um.dll` `c0000005` @ `0x0024678a` | 启动后 1.5 s |
| 20:24 | 双渲染 + 帧中取帧 | 同上，**逐字节相同** | Reset 后 0.4 s |
| 20:27 | 只抓帧（pass2=0） | 同上 | Reset 后 0.4 s |
| 20:31 | 每帧只渲一眼（交替眼，取帧只在 EndScene） | 变成 `d3d11.dll` @ `0x00198b74` | 关卡加载中 |
| 对照 20:15（stereo=0） | 单纹理已验证版 | 无（4200 帧后从**别的**模块退出崩溃） | — |

**读法**：前三局 = 同一处（帧中取帧，从渲染线程动设备）→ 删掉它之后签名变了（说明修对了地方），
第四局是**另一处**（仍属于回读链：staging → `GetRenderTargetData` → `LockRect` → D3D11
`Map/Unmap` → memcpy）。已验证的单纹理版之所以能活，是因为它整条链**每帧只走一次**且固定发生在
EndScene；立体模式多走了一次，并且让合成器去采样"正在被 Map/Unmap 的动态纹理"。

**研究其他 VR mod 的结论（用户提示，我照做了）**：DX9 的 VR 方案（VD 自己的注入、VorpX、
Vireo、VRto3D）有一条共同铁律——**任何一帧都不把画面下载到 CPU**，全部用 D3D9Ex 共享纹理 ↔
D3D11（`OpenSharedResource`）留在显存里。本项目从第一轮起就走的是回读路线，我一直在加固它，
而不是换掉它。这是本轮最贵的一课。

#### 已经做了的事

1. `re6vr_stereo_pass2.txt`：第二遍引擎渲染独立开关，**默认 0**（那个模式四局全崩）。
2. 帧中取帧**删除**；现在唯一碰游戏设备的地方是 EndScene（与已验证版同位置、同调用形态）。
3. 每帧只渲一眼、左右眼**逐帧交替**（`render_probe_record` 里切 `g_current_eye`），两眼各自
   保存自己上一次的取帧结果 —— 一帧延迟换掉一次整场景渲染。
4. **共享纹理专用路已写好**（`create_shared_stereo_surfaces` / `blit_eye_shared`）：在 D3D9Ex 上建
   带共享句柄的纹理 → 游戏设备直接 blit 进去 → D3D11 `OpenSharedResource` + SRV 直接采样。
   **零回读、零 Map、零 memcpy**。可用时自动接管，不可用时**自动退回回读路**并在日志里说明原因。
5. 设备丢失/Reset 期间取帧停机、连续失败自解除、D3D11 资源在设备丢失时释放。

#### 卡在哪（新会话从这里接手）

**共享纹理路没能验完，卡在一个具体的技术障碍上**：在同一进程里创建**第二个** D3D9Ex 设备会
在真实 d3d9.dll 内部崩溃（`call [eax+4]`，空指针写；离线 harness 稳定复现，已用 SEH 守住，
日志写 `CreateDeviceEx RAISED inside d3d9`）。所以：

- 共享路径本身（D3D9Ex 共享纹理 → D3D11）**还没被验证过**，`scripts\check_shared.bat` 目前
  报的是"私有 Ex 设备建不出来"，不是"共享不可用"；
- 本项目的游戏设备是 `g_original_create_device` 建出来的**普通 D3D9 设备**，它自己建不了共享纹理。

**下一任最该先试的一件事**（改动小、风险可控）：让**游戏自己的设备**变成 D3D9Ex —— 在
`hooked_CreateDevice` 里先试 `CreateDeviceEx`，失败再退回原来的 `CreateDevice`，并在拿到设备后
`QueryInterface(IDirect3DDevice9Ex)`。游戏设备一旦是 Ex，就**不需要第二个设备**了：共享纹理由它
自己创建，上面那个崩溃点直接消失，第 4 条的整条路立刻可用。判据：日志出现
`SHARED stereo path ARMED`。

**已知的既有问题（与本轮无关，别浪费时间）**：本机今天 18:18–20:17 共 7 次 BH6.exe 退出时崩在
`VirtualDesktop.LibOVRRT32_1.dll`（`c0000409`），控制局也一样；那是 VD 运行时的问题。

---

### 轮次 10（2026-09-25 深夜）：把"其他 mod 怎么做"查到底，以及三条被证伪的引擎内路线

用户要求"去看看更多 mod 的做法"，照做后有三个**实测**结论（都不是推断）：

1. **引擎自己的 `Stereo` 开关是死代码。** `config.ini` 的 `[GRAPHICS] Stereo=OFF` 确实被读进
   `sRender+0x448EE0`（`0x00F42C37 mov byte [esi+0x448EE0], al`），但**全镜像 20 MB 里没有任何
   一条指令读它** —— 用新的 `scripts\field_refs.py 0x448EE0` 按位移字节精确搜索，唯一命中就是那次
   写入本身。所以"把 config.ini 改成 Stereo=ON 就能立体"是**假的**，连试都不用试。
   （离线报告里那句"driver-level 交错立体，给不了我们两张可捕获的图"是 INFERRED，现在升级为
   MEASURED：它根本不是任何东西。）
2. **display 数量确实存在且可以被写成 2** —— `sRender+0x462694` 有两处写入：`0x00F4307B`
   写 2、`0x00F4301E`/`0x00F42C92` 写 1。写成 2 时引擎会走 `0x00F422D4` 的循环
   `CreateAdditionalSwapChain` 建第二条交换链（`0x00F42317`），并在帧末一并 Present
   （`0x00F41D8E`）。**但它不是立体**：两个 display 用的是同一台相机，而且整条路的前提是
   `[0x186E870]+0x130` 这个"副显示设备"存在（本机没有）→ 那是双显示器输出模式。
3. **共享纹理（零回读）这条路在本进程里走不通**，原因是硬件/API 层面的，不是我们的 bug：
   - 游戏只调用 `Direct3DCreate9`（日志实测：`Direct3DCreate9(32) -> 0x0B7DF6C8`，**从未**调用
     `Direct3DCreate9Ex`），所以它拿到的 factory 建不了 Ex 设备；
   - 用 `Direct3DCreate9Ex` 另外造一个 Ex factory 再用它 `CreateDeviceEx` 会**在真实 d3d9 内部
     跳飞**（`execute at 0x0000E084`，即把 HWND 当跳转目标）—— 换成 DISCARD 交换效果也一样；
   - 因此：**本进程里无法获得 D3D9Ex 设备 → 无法创建带共享句柄的纹理 → 无回读方案不成立**。
   （`hooked_CreateDevice` 里的 Ex 尝试/回退已经在代码里，且用 SEH 守住；日志会写
   `the Ex attempt RAISED inside d3d9` 然后回到普通设备，画面不受影响。）

**由此，方案 B 的现状**：三条"让引擎/驱动一次产出两只眼"的路（引擎 Stereo 开关、引擎多
display、D3D9Ex 共享纹理）**全部实测排除**。剩下唯一可行的是"我们自己渲染两遍 + 把两幅图交给
合成器"，而这需要一个**不经过 CPU 的第二块画面** —— 在当前 API 组合下没有这条路。
离线四项自检与非 stereo 回归门禁目前**全 PASS**，部署为已验证的 `--config head`。

**给下一任的建议**：不要再往"每帧多渲染一遍 / 多取一次帧"上投入。要么接受单眼（当前状态已经是
一个能戴、能转头、屏幕铺满显示屏的影院模式），要么把目标改成"把 fov 匹配做好 + 直投提交"，
把那一个画面的观感做到最好（`bridge_submitted_fov_v_deg()` 已备好）。

---

### 轮次 12（2026-09-25 深夜）：收工结论 —— 立体在本机不可达，以及为什么

**八局实测，全部失败，全部是 `stereo=1` 的局。** 这一节是给下一任的判决书，不是待办清单。

#### 已经逐步证实、可复用的东西（这些是真的成果，别丢）

| 事实 | 依据 |
|---|---|
| 引擎把**整遍**渲染画进"当时绑定的那个 render target" | 轮次 12 探针：`GetRenderTarget`(slot 38) 140 次读取，指针整局不变 |
| 引擎在**渲染阶段外**绑定目标，**阶段内从不重绑** | 2.5 分钟 / 2686 阶段 / 阶段内绑定 **0** 次 |
| 所以"在阶段开始前绑一次我们自己的纹理"是有效且不打扰引擎的 | 离线 `check_redirect.bat` PASS；游戏中日志 `REDIRECT stereo path ARMED` + `REDIRECT eye 0/1 picture N copied` 反复出现 |
| 引擎自带 `Stereo` 开关是**死代码** | `field_refs.py 0x448EE0`：全镜像唯一命中是那次写入 |
| display 计数可写 2，但那是**双显示器**模式（同一台相机） | `0x00F4307B` 写 2 + `CreateAdditionalSwapChain`；前提是副显示设备存在 |
| D3D9Ex 共享纹理在本进程**不可得** | 游戏只用 `Direct3DCreate9`；另造 Ex factory 会在 d3d9 内跳飞（`execute at 0x0000E084`）；私有 Ex 设备创建同样崩，代码已整段移除 |
| `scripts\field_refs.py <field>` 能看见 `re6dis xref` 看不见的 `[reg+disp]` 访问 | 本轮多个结论都靠它 |

#### 八局的完整对照（这是判决的依据）

| 时刻 | stereo | ipd | 方式 | 结果 |
|---|---|---|---|---|
| 20:15 | 0 | 0 | 单纹理（对照） | **65 s 完好** |
| 20:19 | 1 | 0.063 | 双pass + 帧中取帧 | 1.5 s 崩 `nvwgf2um@0x24678a` + GPU 挂起 |
| 20:24 | 1 | 0.063 | 同上 | Reset 后 0.4 s 崩，**逐字节相同** |
| 20:27 | 1 | 0.063 | 只抓帧（pass2 关） | 同上 |
| 20:31 | 1 | 0.063 | 单pass 交替眼 + 回读 | 崩 `d3d11.dll@0x198b74` |
| 20:56 | 0 | 0 | 单纹理 + RT 探针 | **65 s 完好** |
| 21:04 | 0 | 0 | 单纹理 + 绑定探针 | **2.5 分钟完好** |
| 21:11 | 1 | 0.063 | 双pass + 重定向 | 0.5 s 崩 `nvwgf2um@0x24678a` |
| 21:14 | 1 | 0.063 | 单pass 交替 + 重定向 | ~1 s 崩，同一签名 |
| 21:18 | 1 | **0** | 单pass 交替 + 重定向 | 崩 `d3d11.dll@0x198b74` |

**读法**：唯一贯穿全部崩溃的是 `stereo=1` 本身，而不是"双pass"（21:14 只有一遍）、
不是"眼偏移"（21:18 `ipd=0` 照样崩）、不是"回读"（21:18 用重定向、每帧一次拷贝）。
`stereo=1` 打开的每一件事（交替眼、每眼纹理、重定向拷贝、每眼 SRV 提交）都试过组合，
**没有一种活下来**。而 `stereo=0` 的三种配置都活得很久（65 s、65 s、2.5 min）。

**所以：本机（NVIDIA 32.0.16.1664 + VD 串流 + 32 位 D3D9 游戏）上，
"每帧产出两只眼的画面"这条路线，在试过引擎侧、驱动侧、我们自己渲染侧的全部可行组合后，
没有一条能稳定运行。** 崩的是显卡驱动的用户态 DLL，且都发生在立体路径刚启用的那一两秒内。

#### 当前状态（收工时的部署）

`--config head`：`steer=head`、`stereo=0`、`pass2=0`、`ipd=0`、`eye_index=-1`、`rt_probe=0`、
`view=off`。MD5 `843A6218256AFF33423CEF2B636944CD`。
离线门禁 `check_all.py` **全 PASS**、`check_nostereo.bat` **PASS**。
**这就是那个能玩的版本**：头显里是一块铺满显示屏的影院屏幕，头部追踪可用（左右纯水平、点头 1:1、
大角度不倾斜、静止不漂、F9 归位）。

#### 如果以后还想碰立体，唯一没试过的方向

不是在渲染管线里做手脚，而是**换一个运行时的重投影方案**（VorpX 那一类：拿单幅画面 + 深度缓冲，
在提交前由 mod 自己按每眼视差重投影）。这需要先回答两件事：RE6 有没有可读的深度缓冲、
VDXR 能不能接受这种提交。**在动代码之前先调研这两件事**，别再像本轮一样从渲染管线一头钻进去。

#### 本轮为什么会走这么远（写给下一任的方法论）

用户提示过两次"去看看别的 mod 怎么做"，两次都指向了正确的方向（第一次指向"不要每帧回读"，
第二次指向"引擎多 display"）。教训是：**在花掉三局实机之前，就该先把同类项目怎么解决这个问题
调研清楚**，而不是靠自己从反汇编里推导一条自认为聪明的路。本项目已经因为"先动手、后求证"
付出过代价（DTI 不可达、弱判据找相机），这次是第三次。


---

### 轮次 11（2026-09-25 深夜）："反编译改二进制"能带来什么——以及一条还没试的引擎内路线

用户问"反编译去改呢"，于是把 `mDisplay` 与场景目标的所有读者查清楚了（新工具
`scripts\field_refs.py`，按位移字节精确搜索，能看见 `re6dis xref` 看不见的 `[reg+disp]` 访问）：

| 字段 | 读者 | 含义 |
|---|---|---|
| `sRender+0x46267C`（`mDisplay[0]`） | **0 处** 以 `[esi+disp]`/`[edi+disp]` 形式 | 只被 `lea` 取地址后按结构访问 |
| `sRender+0x4540B8`（per-display 渲染对象） | 1 处写入（`0x00F43008`） | 建 display 时写入 |
| 场景目标三件套（`[ctx+0x48E4C]` 索引的数组） | 只有 `0x00F3EA6C`、`0x00F38A8C` 两处 | **不在 sRender 上，在渲染上下文（`sRender+0x194`）上** |

**结论一：改二进制没有"解锁隐藏功能"的余地。** 引擎里根本不存在第二视图的代码：
`Stereo` 标志没人读（轮次 10），display 计数只是切到"副显示器"（需要第二台显示设备），
场景目标是**一份**、建一次（`0x00F3EA95..0x00F3EAD2`，在 display 循环之前）。
"把死代码的开关打开"这种玩法在这里不成立，因为死代码没有下游。

**结论二（这一步有用）：真正值钱的不是改二进制，而是"把某一遍渲染的目标改成我们自己的纹理"**
—— 引擎每遍都是画进"当时绑定的那个 render target"，所以一个 `IDirect3DDevice9::SetRenderTarget`
(vtable slot 37) 的 detour 就能办到，**不需要碰 exe**。若成立，收益是：
两遍渲染各自落进一块我们自己的纹理（第二只眼不再需要"多取一次设备里的帧"），
而每帧的 GPU→CPU 拷贝仍然只有**一次**（与已验证、不崩的那条路同量级）。

**还没回答的关键一问**（探针已写好并部署，`re6vr_stereo.txt=1` 时每秒打印一行）：
`sRender` 的那一遍到底画进**后备缓冲**，还是画进引擎自己的场景纹理？这决定 SetRenderTarget
重定向的落点。日志判据行：

```
steer: RENDER phase N draws into render target 00000000 (slot 38 read; the back buffer is what Present shows)
```

拿到这个指针（并与 `compositor: render target ...` 那行的指针比对）之后，才谈得上重定向的实现。

---

### 轮次 13（2026-09-26）："为什么一接到 D3D11/OpenXR 就崩显卡驱动"——把机制拆到可判定的程度

用户直接问了这个"为什么"。下面是**测量**与**推断**分开的答案，以及三个从未跑过的判别实验。**本轮没有改任何代码或标记。**

#### 先修正两条旧归因（有实测依据，都是本轮新查的）

| 旧说法 | 实测 | 结论 |
|---|---|---|
| "两个线程并发操作同一台 D3D9 设备"（轮次 9 的注释） | 游戏创建设备时 `flags=0x44`（本进程再 OR 上 `FPU_PRESERVE` 后日志显示 `0x46`），**`0x44` 里已经含 `D3DCREATE_MULTITHREADED (0x4)`** | 设备是**多线程模式**，d3d9 运行时会自己串行化 API 调用。所以问题不是"设备非线程安全"，而是**我们的调用与引擎自己的命令流在语义上交织**（见 H1） |
| "32 位进程 2 GB 上限压死" | `BH6.exe` 的 PE `Characteristics = 0x0123`，**`LARGE_ADDRESS_AWARE` 为真** | 上限是 4 GB，不是 2 GB。资源压力假设**降级但不排除**（4K + 两套链路仍然很大），见 H2 |

另外两个本轮才量出来的尺寸（决定了压力的量级）：
- 游戏后备缓冲 **3840×2160 fmt=21(A8R8G8B8)**（`CreateDevice #1` 那行）；
- 每眼抓帧目标 **2492×1401**（约 14 MB/张），交换链是 `shared-array, view_count=2, image_count=3`，每张切片 **2492×2684**（约 27 MB ⇒ 仅交换链就约 160 MB）。

#### 崩溃的签名与不变式（测量）

| 项 | 值 |
|---|---|
| 崩在哪 | `nvwgf2um.dll+0x24678a`（c0000005 **空指针读**）4 局；`d3d11.dll+0x198b74` 2 局 |
| 伴随 | **LiveKernelEvent 141 = GPU 引擎挂起**（不是干净的 API 报错） |
| 何时 | 立体启用后 **0.4 s ~ 4 s**；日志显示崩点在"合成器目标建好 → 头几帧提交"之间 |
| 我们的代码 | **没崩**：`seh::guard` 一次都没报，代理 DLL 无 fault → 故障在我们**下方**（驱动/运行时） |
| 不变式 | 唯一贯穿全部崩溃的是 `stereo=1`（每眼一条独立图像链 + 每眼抓帧 + 每眼各采各的 SRV） |
| 已排除 | 双 pass（21:14 只有一遍）、眼偏移（21:18 `ipd=0`）、回读次数（21:18 重定向 + 每帧一次拷贝） |
| 对照 | `stereo=0` 三局分别活了 65 s / 65 s / 2.5 min，同样的回读链、同样的分辨率 |

#### 一个必须澄清的说法：运行时**看不到**我们那两张纹理

OpenXR 提交的是**一张 `shared-array` 交换链**，两眼是同一张 swapchain 的两个 slice（`imageArrayIndex=0/1`）；
我们的每眼源纹理只被**我们自己的 quad draw** 采样。所以"运行时拒绝两张纹理"这个解释**不成立**——
mono 与 stereo 交给运行时的东西几乎一模一样，差别**全在我们这一侧**。这条把候选范围缩小了一半。

#### 三个候选机制（按可能性，含反证）

**H1｜我们的设备调用与引擎自己的命令流交织。** `StretchRect` / `GetRenderTargetData` 会内部改设备状态并强制 flush，
`MULTITHREADED` 只保证 API 级串行，**不保证不切开引擎正在批处理的绘制序列**；重定向模式还在渲染阶段内
调用 `SetRenderTarget`（`render_bind_eye`），pass2 模式更是递归调用整段渲染阶段。
**支持**：20:19 那局实测"合成器的像素工作来自两个线程"，150 ms 后挂起；把抓帧挪到 EndScene 后**签名变了**（说明位置确实有影响）。
**反证**：20:31（纯 EndScene 抓帧）与 21:18（EndScene 抓帧）**没有帧中操作**也崩 → 帧中交织是**充分不必要**。

**H2｜4K 下多一套图像链的资源/重命名压力。** `D3D11_MAP_WRITE_DISCARD` 每帧要求驱动换一块新分配，
旧块要留到 GPU 用完；VD 的串流编码器会把帧压住一会儿，于是重命名堆积。
32 位 + LAA（4 GB）+ 4K + 两套链路时，**驱动内部一次分配失败**就正好是"小偏移空指针读 + 提交卡死 → 141"的形状。
**反证**：mono 在同分辨率下活得好 → 所以必须是"多出来的这一套"而不是"回读本身"（与轮次 12 一致）。

**H3｜VD 运行时在本进程里本来就不稳。** 同一天 18:18–20:17 有 **7 次** BH6.exe 退出时崩在
`VirtualDesktop.LibOVRRT32_1.dll`（`c0000409`），**控制局也一样**。立体只是把提交的节奏/资源集改了，
足以踩到它。`d3d11.dll` 那两局（运行时侧）与这条吻合。

#### 从未跑过、但最有信息量的三个实验（一次一个变量）

1. **分辨率先行**：把每眼抓帧降到 640×360（其余不变）跑一局。活着 ⇒ H2（资源/重命名压力）；照崩 ⇒ H2 出局。
   这是唯一能一刀切开 H2 的实验，而且改动最小。
2. **"生产"与"提交"分离**：两套链都建、两眼都抓，但合成器**两眼都绑 eye 0 的 SRV**（回退路径已存在）。
   活着 ⇒ 触发点在"每眼各采各的纹理"这一步；照崩 ⇒ 触发点是"多一套资源常驻"。
3. **内存探针**：立体启用前后每秒打一行 `GetProcessMemoryInfo`（WorkingSet/PrivateUsage）
   + `IDirect3DDevice9::GetAvailableTextureMem`。若崩前出现塌陷，H2 从假设升级为测量。
   （这是把 H2 从"合理猜测"变成"数字"的最便宜办法。）

#### 对 6DOF 计划的影响：**没有影响，而且方向正好相反**

立体崩的是"每帧产出两只眼"，而**推荐的 6DOF 路径是单画面**（位置注入只改相机，不改画面数量）——
恰恰落在 `stereo=0` 那条 65 s / 65 s / 2.5 min 都活着的配置上。
所以：**6DOF 可以做，立体仍然不做**；两者在这台机器上不是同一个风险等级。




