# re6vr — 真 6DOF（头部平移）交接文档

> 写于 2026-09-25 夜，起因：用户问"看看生化危机6 VR mod 怎么实现真的 6DOF"。
> **本次会话没有改动任何 live 路径**：没改源码、没重编译、没写标记文件、没部署。下面是纯取证 + 方案，
> 每条结论都写了证据出处（日志行、源码行号或引擎常量），可以逐条复核。

## 0. 一句话结论

**头部"位置"运行时一直在给，我们只用掉了"朝向"。** 相机写入点（`0x00E6FD20` 里就地改写 eye/target 两个向量）
本来就支持"整体平移"（每眼 IPD 就是同一段代码干的），所以真 6DOF 不是要新开一条通路，而是**把已经存在的位置数据
接到已经存在的平移写入点上**——共 4 处小改动。

顺带查出一个**真 bug**：那条平移通道把"米"直接当成游戏单位用了，而这个世界是**厘米尺度**
（引擎常量 `near 16.000 / far 4000000.00`、肩后机位 `fwd len 320.16`）。所以哪怕之前开过 IPD，
平移量也只有应有值的 **1/100** —— 这是"两只眼看起来没差别"的第二个独立原因（第一个是切片布局，
见 `_work/HANDOFF-stereo-vr.md`）。

## 1. 路线：别的 mod 是怎么拿到 6DOF 的

核心区分：**"每眼重渲染" vs "单幅画面 + 相机平移" vs "深度重投影"**，外加一条**同引擎家族已经走通的架构路线**（§1.4）。

> **先排除一个误会（2026-09-25 核实）**：中文圈那条"《生化危机》VR 模组一键安装器……支持 6DoF"
> （`vryouxi.site/news/resident-evil-vr-mod-installer-1540`，开发者 MrSurviv0r）**与 RE6 无关**。
> 它明列的支持范围是 **RE2 / RE3 / RE4 / RE7 / RE8 / 粉丝版 RE9**，全部是 RE Engine + praydog 的 REFramework；
> 文章里的 "6DoF" 指**体感/动作控制器输入**（REFramework 的 "Generic 6DOF VR support"），
> 不是"RE6 的相机怎么做平移"。同一轮检索：GitHub 上 `resident evil 6 vr` 只有 1 个仓库（一个粉丝小游戏，不是 mod），
> `MT Framework VR` **0 个仓库**，REFramework 的游戏列表里没有任何 MT Framework 作品
> → **RE6 没有公开的 VR mod，更没有公开的真 6DOF 实现**。所以"RE6 怎么做到真 6DOF"这件事上，
> 我们不是在追一个已有实现，而是在**没有先例的引擎上开一条路**；能借的只有同引擎家族（DMC4）的做法（§1.4）。

### 1.1 路线 A：每眼重渲染（真 VR 的标准做法）
每个眼睛用自己的位姿渲染一遍场景，双眼位姿都来自头显（含平移）。RE Engine 那批 RE 系 VR mod
（REFramework 系）就是这条路；它对引擎的要求是"能指定相机并渲染两遍"。
**本项目状态**：引擎的相机和姿势都在我们手上，但"一帧两视图"在这台机器上**已实测全线崩溃**
（八局对照，全部死在显卡用户态 DLL，见 `_work/HANDOFF-stereo-vr.md` §0/§实测表），且 `stereo=0` 的配置都活得久。
→ 路线 A 在本机**暂时不可用**，但它并不影响 6DOF 本身：6DOF 是"相机位置跟头走"，立体是"两只眼看到不同画面"，
两件事可以分开做。

### 1.2 路线 B：单幅画面 + 相机位置跟头走（本机可行的"单眼 6DOF"）
只渲染一遍，但**把头部平移量注入游戏相机的位置**。玩家得到的是**运动视差**（motion parallax）：
侧身/前倾时近处物体相对远处物体的位移量与真实世界一致——这是 6DOF 的主要深度线索之一，
少了的是双眼视差（立体感）。影院/单画面类 VR 化方案（vorpX 式的"full VR"、各种 DX9 时代的 VR wrapper）
基本都落在这一档。
**本项目状态**：写入点、去重逻辑、"不累积"的设计都已存在，缺的只是数据接线（§3）。

### 1.3 路线 C：深度重投影（VorpX / SuperDepth3D 式）
单幅画面 + 深度缓冲，按每眼视差重投影出两只眼。它能造出**立体**，但**平移视差是伪造的**
（重投影只能按深度做水平位移，转头的几何是真几何，侧身前倾的几何不是），且有典型失效模式：
遮挡区空洞（disocclusion）、没有背面数据、HUD 被一起拉开。
**本项目状态**：未调研 RE6 的深度缓冲可读性；即便可行也只能给"立体"，给不了"真 6DOF 几何"，优先级低于 1.2。

### 1.4 路线 D（同引擎家族已走通的架构做法）：用引擎自带的自由相机 + 换视口相机

外部证据：**`muhopensores/dmc4_hook`**（DMC4，DX9/32 位，同一 MT Framework 2 家族）里的 DebugCam / PhotoMode
就是这么做的，而且它**从不写 view/proj 矩阵、也不碰 fov/near/far**：

1. 用引擎自己的构造函数 new 一个 `uFreeCamera`；
2. 把它挂进引擎的 spawn / 移动线列表（DMC4 是 `sUnit::mMoveLine[23]`）；
3. **把某个 viewport 的 `mpCamera` 换成它**（`viewport->mpCamera = cam; viewport->mAttr = 0x17;`；`mActive` 控制原视口显隐）；
4. 之后每帧只写 `mCameraPos / mCameraUp / mTargetPos`，矩阵交给引擎自己算。

它这么做的理由，正是我们这半个月反复撞到的同一堵墙：MT Framework 2 **没有**全局 view 矩阵槽位，
矩阵在 `sCamera_ViewPort` 上（含 `mViewMatrix / mProjMatrix / mPrevViewMatrix / mPrevProjMatrix`）——
**改矩阵就必须同时维护 prev 矩阵**（否则拖影/时域效果出错），而**换相机对象就完全不用管**。

**这条路在我们 RE6 上可查，而且关键零件已经就位**：

| 零件 | RE6 (BH6.exe) 的值 | 来源 |
|---|---|---|
| `uFreeCamera` 类 | vtable `0x016EE018`、DTI `0x01870324`、size `0xB0`；**ctor VA `0x0109B190`（RVA `0x00C9B190`）**，210 条指令，序言 `55 8B EC 83 E4 F8 83 EC 2C 53 56 57 8B 7D 08`（`__fastcall`-ish，1 个栈参）；另有一个就地初始化入口 VA `0x0109AAB0`（`56 8B F1 E8 ..` 之后 `mov [esi], 0x16EE018`、`movss [esi+0xAC], xmm0`） | `src/mem_cam_tables.h` + `python scripts\re6dis.py props uFreeCamera` / `func 0x0109B190 --va`（**注意：`props` 打印的是 VA，`func` 默认把参数当 RVA**） |
| 它自己的字段 | `mpParent +0x80` / `mParentNo +0x84` / `mpTarget +0x88` / `mTargetNo +0x8C` / `mControlPad +0x90` / `mControlSpeed Vector3` | 同上（引擎**自己注册**的属性表；构造函数里确实引用了这几个属性名字符串，交叉印证） |
| 它继承的 `uCamera` 姿势字段 | pos `+0x50` / up `+0x60` / target `+0x70` / fov `+0x4C` / near `+0x44` / far `+0x40` | 本项目实测 |
| 换相机的落点 | 槽记录（`sBioCamera+0x30+0x190k`）的 **`+0x04` 就是相机指针**（`kSlotCamera = 0x04`） | `src/cam_steer.cpp:63`，`HANDOFF-stereo-vr.md` §3.5 |

**独立佐证（外部，很强）**：DMC4 的 `uCamera` 是
`mFarPlane 0x18 / mNearPlane 0x1C / mAspect 0x20 / mFov 0x24 / mCameraPos 0x30 / mCameraUp 0x40 / mTargetPos 0x50` ——
**字段顺序（far, near, aspect, fov, pos, up, target）与我们 RE6 的（far `0x40`, near `0x44`, `0x48`=?, fov `0x4C`,
pos `0x50`, up `0x60`, target `0x70`）完全一致**，而且 pos→up、up→target 的间距都是 `0x10`（`MtVector3` 补齐到 4 个 float）。
两个互不相关的逆向工程得到同一字段序 → 我们的偏移从"猜"变成"有外部对照"。
另外 `uCamera` 继承 `cUnit`：**对象开头 0x18 字节（32 位）是 unit 记账字段，不是相机数据**。

**代价与取舍**：路线 D 干净（不再跟玩法相机控制器抢姿势；矩阵、prev 矩阵、裁剪、LOD 全由引擎维护），
但要新写"造对象 + 挂进引擎列表"的代码，并且必须决定**玩法相机怎么办**——碰撞 setback、锁定取景、过场接管
都长在那台相机上。它同时是将来做立体时的正确架构（两个 viewport / 两台相机）。
路线 B（§3）是"最小改动、马上能看到视差"。两者不冲突：**B 先落地验证视差，D 作为后续架构**。

## 2. 本次实测到的决定性事实（都可复核）

| # | 事实 | 证据 |
|---|---|---|
| 1 | 运行时**确实提供头部位置**（不是只有朝向） | 日志里 `position_valid=1` 共 **731** 行，例：`pose: yaw -7.7 deg pitch 1.5 deg (orientation_valid=1 position_valid=1)`（`_work/re6vr.log` 21:38:55 起） |
| 2 | 头部位置**从未被消费**：桥只把朝向交出去 | `src/openxr_bridge.cpp:5375` `matrix_probe_set_head_rotation(r)`；`src/matrix_probe.cpp:1216/1222` 的接口是 3×3 旋转；`views[0].pose.position` 只在 5384–5391 的日志里出现过 |
| 3 | 相机位置的注入点**已经存在且已证明不累积** | `src/cam_steer.cpp:781-786`：`memcpy(eye, ne)` + `memcpy(target, nt2)`，同一向量加给 eye 和 target（现在装的是 IPD）；启动日志原话："The rotation is applied to the vectors this call is using right now, so nothing accumulates anywhere."（`_work/re6vr_20260925_2017_head_control.log` 20:15:21） |
| 4 | 当前部署**一次平移都没写过** | `kOffPos (0x50)` 在整个 `src/` 里只有 `read_vec3`，**没有一处写**；且 ipd=0 / eye_index=-1 让 IPD 分支整体失效。`_work/re6vr_ipd.txt = 0`、`re6vr_eye_index.txt = -1` |
| 5 | 世界是**厘米尺度** | 引擎自报 `near 16.000 far 4000000.00` + 肩后机位 `fwd len 320.16`（`re6vr.log` 21:40:17）。米制解释＝16 m 近裁剪面 + 320 m 机位（不可能）；厘米制解释＝16 cm 近面 + 3.2 m 机位（正是 RE6 的过肩机位） |
| 6 | 平移通道**单位用错**：0.063 m 瞳距被当成 0.03 游戏单位 | `src/cam_steer.cpp:1129` `g_ipd = read_float_marker(L"re6vr_ipd.txt", 0, 0, 0.5)`（米），`:778` `d = sign * g_ipd / rlen` 直接当世界单位用；日志实证 `eye (0.03 200.00 -700.00) ... IPD writes 1`（21:11:03）→ 世界位移 0.3 mm |
| 7 | 头部数据的坐标系适合直接做门框映射 | `src/openxr_bridge.cpp:5354-5357` 原话：eye 0 的姿势就在"屏幕锚定的同一个 LOCAL 空间"里，中立位是单位旋转。LOCAL 空间＝右手系、+Y 上、-Z 前、单位米，原点在会话开始时的头位 |

## 3. 缺的到底是什么：4 处改动

1. **传位置**：`openxr_bridge.cpp` 在 5375 行旁边新增一次 `matrix_probe_set_head_position(views[0].pose.position)`（同样无条件传，理由与朝向一致：消费方自己决定要不要用）。
2. **存位置**：`matrix_probe.cpp/.h` 加一对 setter/getter（照抄朝向那对的形状：`g_head_pos[3]` + `g_head_pos_valid`），并给出"相对参考点"的接口。
3. **注入平移**：`cam_steer.cpp` 在现有 IPD 那段（756–802 行）旁边加一条通路：`T_world = 房间基（最近一次 F9/起手时抓的相机水平基）× (头位移) × 尺度 × 增益`，然后 `ne = e + T`、`nt2 = t + T` —— **eye 和 target 必须同加**（只动 eye 会变成转头，这正是 676 行注释里写过的坑）。
4. **F9 语义**：现有 F9 只重取"角度参考"（`g_head.abs_ref_*`），要把**位置参考**一起重取（"坐正、按 F9"＝这一刻是原点）。

### 3.1 一个必须照抄的既有设计（否则会漂）
`cam_steer.cpp:834-867` 有一段"这个姿势是不是我们自己刚写的？"的去重：如果是自己的输出，它会把上次的偏移**减回去**再重新施加。现在只有 IPD 一项偏移，减一次就够；加了平移之后，这段必须**同时减掉两项**（或者干脆把引擎原始姿势缓存在 `pc->` 里，比"减回去"更稳）。漏掉这一步的后果是平移量逐帧叠加、画面持续漂走——正是本项目已经吃过一次的"写干净但看起来在漂"。

### 3.2 最小可用 6DOF 的现成配方（外部**源码级**参照，照形状抄）

调研到的四个真实实现，做的是同一件事（都"重渲染"，没有一个用深度重投影）：

| 项目 | 平移写在哪 | 关键细节 |
|---|---|---|
| REFramework（RE Engine）`VR::apply_hmd_transform` | 写游戏相机对象的 transform | `pos = game_cam_pos + game_cam_rot * (rot_offset * (hmd_pos - standing_origin))`；`m_positional_tracking` **只门控平移**；每帧 `update_camera_origin()` 写、**渲染后 `restore_camera()` 还原** → 游戏逻辑/碰撞永远看不到 VR 偏移 |
| Source 引擎 `CClientVirtualReality::OverrideView`（Valve 源码） | `pViewMiddle->origin = ...` | 平移写在**中眼（mono）视图**上，双眼由它派生；`vr_translation_limit` 默认 **10 单位**硬夹；`vr_projection_znear_multiplier` 默认 **0.3**；躯干 roll/pitch 显式清零 |
| UEVR | 引擎每帧自己调用的 `CalculateStereoViewOffset` | `CustomZNear` / `WorldScale` / `DisableHZBOcclusion` / `DisableInstanceCulling` |
| Doom 3 BFG VR | 引擎相机 | `vr_clipPositional`（夹住平移）、`vr_blink`（头撞墙黑屏）、`vr_nodalX/Z`（颈部模型，避免点头拖动身体） |

**归纳成我们的实现形状（每一步都有上述实现对应）**：
减掉站立原点 → **只用 yaw** 把追踪空间映射到相机空间（用全 pitch 会得到"吊臂"效应：低头把相机往前推）
→ 加到相机位置上 → **夹住幅度**（0.3–0.5 m）→ **投影矩阵与 fov 完全不动** → 渲染后还原；
若还原不了，就每帧无条件重写（正是我们现在对 target 的做法）。

**另一个关键情报**：MT Framework 的相机是"**相机 + 瞄准点**"两个东西（DMC4 相机 hack 作者原话："In order to move the camera, we need to change the positions of both the camera itself and it's aim"），
这与我们实测的 `pos(+0x50)` / `target(+0x70)` 成对结构一致——**平移必须两个一起加**，否则会变成转头（与 §3 第 3 条同一个坑）。
同一位作者还确认：**过场用的是不同的相机对象**，且"场上有 boss 时引擎会自动把相机切到对着它的那台"——这解释了为什么我们必须保留 `camera_may_be_steered()` + "存活 ≥700 ms"两道过滤。

## 4. 需要拍板的设计选择（改代码前先定）

1. **要不要动"眼睛位置"**：你之前明确要的是 **3DOF 三脚架**（`re6vr_head_tripod.txt = 1`，"眼睛位置固定"）。头部平移与它直接冲突——这是设计变更，不是 bug 修复，所以先问。
2. **画面怎么呈现**：当前是"虚拟空间里一块屏"（`screen_dist` 4.0 m、`re6vr_screen_scale.txt` 默认为 1，张角 59.8°×35.8°，而头显 78.2°×81.4°）。屏幕不铺满时，头部平移会让**屏幕里的画面**横向滑动，观感是"电视里的世界在动"而不是"我在动"。要让平移读起来像 6DOF，屏幕需要**铺满视野**（`re6vr_screen_scale.txt` 调大即可，面板宽 4.6 m；这是**另一个独立变量**，按"一次只改一个"应当分开试）。
3. **尺度与增益**：默认给 `100 单位/米`（§2 事实 5 的结论），另配一个增益标记（像 `re6vr_head_gain.txt` 那样先 0.5 再调）。范围要有上限（例如 ±0.4 m），否则人会"从自己身体里走出去"，也更容易把相机顶进墙里。
4. **竖直分量要不要**：坐着时头的上下浮动是真信息，但游戏相机上下移动会改变与角色/掩体的关系。建议默认保留但增益更低，或先只开水平两轴。

## 5. 分步实施（严格一次一个变量；前两步都可写"零风险"）

| 步 | 做什么 | 判据（写进日志，不靠感觉） |
|---|---|---|
| S1 | **只传不写**：把头部位置打进日志（含 position_valid、每分钟最大位移、范围） | 日志出现位移数值且随人真实移动；相机一动不动 |
| S2 | **离线自检**：给平移映射写一个纯函数 + 离线脚本（房间基/尺度/范围/去重），跑既有 harness | `scripts\check_all.py` 全 PASS；映射的数值断言可复核 |
| S3 | **一个固定量**：不看头，只在按下某个键时把相机整体平移一个**固定 20 单位**（0.2 m），打印写入审计 | 画面明显平移且不崩、不漂；松开/再按能回到原位 |
| S4 | **接真数据**：水平两轴 + 尺度 100 + 增益 0.5 + 范围 ±0.4 m，F9 重取位置参考 | 侧身前倾有正确方向的视差；静止时不漂 |
| S5 | **呈现**：把屏幕铺满视野（`re6vr_screen_scale.txt`），确认"影院感 → 我在里面"的转变 | 屏幕边缘不再出现在视野里；画面比例仍对（fov match 日志） |
| S6（可选，架构） | **路线 D 的纯离线第一步**：查清 `uFreeCamera`（VA `0x0109B190`）由谁构造/挂进哪条更新列表，以及 `sBioCamera` 槽记录 `+0x04` 换成自由相机后引擎是否照常走完全套（矩阵/prev 矩阵/裁剪）。**纯静态分析 + 离线 harness，不启动游戏、不改 live 代码** | 离线脚本给出构造调用链与槽记录替换的可行性判据；不动游戏进程 |

### 5.1 平移落地后要专门验的三件事（外部：MT Framework 渲染管线证据）

RE6 是**混合渲染器**——Capcom 自己的 MT Framework 工程师在访谈里原话：不完全的 Light Pre-Pass，
与 forward 系技术混合（`game.watch.impress.co.jp/docs/series/3dcg/538452.html`）。
所以"**一个相机对象被所有 pass 读**"这件事**不能假设**。与本改动直接相关的三条：

| pass | 已知事实 | 平移后要验什么 |
|---|---|---|
| 阴影 | **光源空间** matrix（light-space shadow map；RE5 单张 1024²，LP2 最多 3 级联）→ 与相机位置无关 | 转头/侧身时阴影应当**仍贴在世界里**；若阴影跟着头动，说明有别的 pass 在读我们改的相机 |
| 反射 | RE5 用的是**动态生成的 cube map**，渲染原点与主相机不同 | 侧身时反射内容是否与主画面一致。不一致属于"平移只在主 pass 生效"的预期内，不是 bug |
| 粒子/烟 | ¼ 分辨率缓冲，用**它自己那张 ¼ 深度**做 Z 测试 | 平移时粒子是否"不跟世界走"（这一层的经典症状） |

**视图相关 vs 视图无关（决定平移后哪些东西会坏）**：这些作品的环境光/GI 是**视图无关**的——
每个区域手工摆放的环境 cube map，用 9 系数（3 阶）球谐在像素着色器里应用、区域之间插值
（RE5 的 "Shadow Mask" 让被遮罩像素只用环境 SH 着色，所以阴影保留颜色）。
→ **平移相机不会破坏环境光/IBL**；会坏的只有视图相关的那两样：**反射 cube map** 与 **¼ 分辨率粒子缓冲**。
与上表的验收清单一致。

**外部否证 + 本机核实（2026-09-26）**：

- **RE6 没有 Lua**：外部调研找不到任何 MT Framework 使用 Lua 的证据（那是 RE Engine/REFramework 的印象，
  且反证需要文件访问）；**我们直接在本机求证了**：`BH6.exe` 里搜 `luaL_newstate` / `lua_pcall` /
  `luaL_loadbuffer` / `lua_open` / `lua_close` / `Lua 5.` / `LuaJIT` / `lua_pushnumber` / `lua_gettop` /
  `luaL_register` —— **全部 0 命中**（全文 ASCII 串中唯一含 "lua" 的是 `...Evalua...` 这类误命中）。
  → **没有 Lua 路线**，原生 hook 是唯一路线。
- **`.ahc` / `CAMtrack*.fsm` 不要去碰**：外部证据显示 `.ahc` 的相机数据**与玩法耦合**——删掉它会使
  伙伴 AI 寻路失效（Ustanak 2-3、HAOS 逃亡段）。它不是"纯表现层"的数据。
- **`BH6.exe` 是 LAA**：PE `Characteristics = 0x0123`（`LARGE_ADDRESS_AWARE` 置位）→ 地址空间上限 **4 GB**，
  不是 2 GB。（外部由"社区流行打 4GB 补丁"反推出"原版不是 LAA"，已由本机实测推翻。）

**另一条方法论储备（上游杠杆 vs 下游杠杆）**：外部可读的 MT Framework 相机 cheat 源码
（Dragon's Dogma 的 `ddda-dinput8`）显示，那股社区是在**控制器上游**动手的——改的是**俯仰夹紧常量**
（把 ±89.9 覆写进 xmm0）和**自动回正标志字节**（`esi+0x2F0` / `edx+0x2F1` 那类），而不是位置向量。
**我们不需要它**：我们写的位置在渲染阶段内的 look-at 构造器里，已经在控制器、夹紧、自动回正的**下游**——
这也是为什么我们的旋转写入能"无状态、不累积"地生效。但若将来槽表/相机对象路线失效（换版本、换游戏），
这两个字节是值得回头找的备用杠杆。

## 6. 风险与已经确认的边界

- **culling / 阴影 / LOD 会跟着走吗？** 会。我们写的是**引擎自己的姿势指针**（`[cam+0x50]`/`[cam+0x70]`），不是另造的矩阵，所以凡是读这台相机的引擎侧逻辑（视锥裁剪、阴影、LOD、特效）与渲染看到的是一致的。这是"写相机对象"相对"写着色器常量"的核心优势（README 里第五次判据事故的教训：形状不是身份，但对象指针是）。
- **相机穿墙**：平移施加在引擎碰撞/setback **之后**，所以贴墙前倾可能把相机推进几何体。缓解＝范围上限；不做碰撞查询。
- **过场/特效相机**：`camera_may_be_steered()` 与"存活 ≥700 ms"两道过滤已经在（`cam_steer.cpp:2156-2185`），平移必须走同一道门，不能绕过。
- **相机的来源不止一个，但我们站在它们下游**：外部证据（RE6 mod "Less Intrusive Cameras"，archon）指出 RE6 的**强制/情境镜头是数据驱动**的——
  每段的 `.ahc`（在 `.arc` 里，"most of the cameras in a given segment" 的主文件）以及 `CAMtrack0-3.fsm` 轨道；
  而社区对"引擎每帧重置相机"的答案是**用 trainer 冻结数值**（wilsonso），比"换相机对象"（路线 D）差。
  我们 hook 的 `BH6.exe+0x1F80B0` 在**所有**这些相机来源的下游，所以它是稳的拦截点——这也是路线 B 值得先做的原因之一。
- **不累积**：见 §3.1，必须复用 `pc->` 去重。
- **立体仍然不做**：平移解决的是"相机位置跟头"，不解决"每眼一张图"。本机的立体崩溃结论（八局对照）不受本文档影响。

## 7. 本次没有做的事（免得下一轮误判）

没改源码、没重编译、没写标记、没部署、没启动游戏；`dist/` 为空；`build/` 里只有日志指针与崩溃记录。
当前部署仍是已验证的 `--config head`（`steer=head / method=1 / absolute=1 / tripod=1 / ipd=0 / stereo=0 / eye_index=-1 / fov 35.8`），
也就是说：**现在戴上去，头转视角转，人动视角不动。**
