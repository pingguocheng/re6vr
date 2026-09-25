# re6vr — Resident Evil 6 VR mod (experimental)
> **About paths in this repository.** The author's own absolute paths have been replaced: the
> project root is written as `C:\re6vr`, and the game path uses the standard Steam default
> (`C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6`). Adjust them to your own
> layout. Nothing else was changed.
>
> **仓库里的路径说明**：作者本机的绝对路径已替换为占位符——工程根目录写作 `C:\re6vr`，游戏目录用
> Steam 的标准默认位置。请按你自己的目录调整；其余内容未作改动。


A from-scratch VR mod for **Resident Evil 6 (PC 2012, 32-bit Direct3D 9, Capcom MT Framework 2)**.
It is a `d3d9.dll` proxy: it hooks the engine's camera object and submits the rendered frame to an
OpenXR runtime (developed against Virtual Desktop's runtime, Quest over streaming).

**What works today**
- **Head rotation** (3DOF "tripod"): yaw + pitch only, never roll, eye position fixed.
  Implemented by rewriting the pose the renderer actually reads (inside the engine's look-at
  builder), so it is stateless and cannot drift. `F9` = recentre. Verified playable.
- One picture presented as a cinema screen filling the display. `--config head` is the verified
  deployment (`python scripts\deploy_config.py --config head`).

**What does not work, and why (measured, not guessed)**
- **Per-eye stereo.** Ten in-game runs: every `stereo=1` configuration crashed inside the GPU
  user-mode driver within **0.4–4 s** (`nvwgf2um.dll+0x24678a` null read, or `d3d11.dll+0x198b74`,
  both with **LiveKernelEvent 141** GPU-engine hangs), while all three `stereo=0` configurations
  ran for 65 s / 65 s / 2.5 min. Dual pass, eye offset, readback count and mid-frame capture were
  each ruled out by a dedicated run. See `_work/HANDOFF-stereo-vr.md` for the full comparison table.
- **Real 6DOF translation** is not implemented yet. The plumbing is understood and costed:
  four small edits, plus a units bug (the existing eye-offset path treats metres as game units in a
  centimetre-scale world — a factor of 100). See `_work/HANDOFF-6dof.md`.

**Read these first**
| File | What it is |
|---|---|
| `_work/HANDOFF-stereo-vr.md` | the stereo verdict: ten-run table, what was excluded, three untried experiments |
| `_work/HANDOFF-6dof.md` | what real 6DOF needs: the 4 edits, the unit bug, staged plan, verification list |
| `_work/BRIEF-for-external-review.md` | self-contained briefing (environment, hard facts, open questions) |
| `_research/` | engine research (MT Framework camera internals) and a survey of how other mods do it |

**Build / deploy**: `scripts\build.bat` (fetches MinHook + OpenXR loader through
`scripts\stage_deps.py`), then `python scripts\deploy_config.py --config head`.
`scripts\uninstall.bat` removes the proxy from the game directory.
Needs an owned copy of the game, Windows, an OpenXR runtime and the 32-bit VC++ runtime.

**Legal**: no Capcom assets are included (no models, textures, audio or data tables). Requires a
legitimately owned copy of the game. Not affiliated with or endorsed by Capcom.
Third-party components and their licences: `THIRD_PARTY.md`. Licensed MIT — see `LICENSE`.

## Status: **unfinished** — and exactly where it is stuck

This is a research project that got one genuine result and then hit three walls. It is published so
that both the result and the walls are on the record — not because it is a finished mod.

**Working**: head rotation (3DOF tripod — stateless, cannot drift), F9 recentre, and a single
picture presented as a cinema screen filling the display. `--config head` is the verified
deployment and `scripts\check_all.py` passes.

**Stuck on, with measurements:**

1. **Per-eye stereo is out of reach on the development machine.** Ten in-game runs: every
   `stereo=1` configuration died inside the GPU's user-mode driver within **0.4–4 s**
   (`nvwgf2um.dll+0x24678a` null read, or `d3d11.dll+0x198b74`), both accompanied by
   **LiveKernelEvent 141** (GPU engine hang). All three `stereo=0` configurations ran for
   65 s / 65 s / 2.5 min. Ruled out by dedicated runs: dual pass, eye offset (`ipd=0` crashed too),
   readback count, mid-frame capture. What remains lives *below* this code — inside the driver and
   the streaming runtime — where a mod can neither observe nor fix it.
2. **Real 6DOF (head translation) is not implemented.** It is four small edits away and the
   plumbing is documented (`_work/HANDOFF-6dof.md`), but it has to be done together with a units
   bug: the existing eye-offset path feeds **metres into a centimetre-scale world** (engine
   constants `near 16.000 / far 4000000.00`, shoulder camera `fwd len 320.16` units) — **a factor
   of 100**.
3. **There is no zero-readback path in this process.** GPU-side texture sharing needs a D3D9Ex
   device and the game only ever calls `Direct3DCreate9`; building a separate Ex factory jumps into
   unmapped memory inside `d3d9.dll` (`execute at 0x0000E084`). So every frame has to come back
   through the CPU — exactly the cost the stereo path cannot afford.

**Dead ends already verified** (so nobody repeats them): the engine's own `Stereo` flag is **dead
code** (the only reference in the whole image is the write itself); `displayCount = 2` is a
second-monitor mode with one shared camera, not stereo; `mCameraOrg` (+0xE30) is **never read** by
the renderer; MT Framework has **no Lua** (zero Lua C-API strings in `BH6.exe`); and the
data-driven cameras (`.ahc`, `CAMtrack*.fsm`) are **coupled to gameplay** — deleting them breaks
partner-AI pathing.

**Still open**: the view builder is called for ~48 different camera objects per session, all
carrying the same template pose, so "which object is the player's camera" is answered by the
`sBioCamera` slot table plus a 700 ms liveness rule rather than by class identity. The docs also
record **five earlier false conclusions** (weak criteria; a radians/degrees mix-up that silently
disabled a probe), which is why every claim here is tagged MEASURED or INFERRED.

**Next steps, if anyone picks this up**: (1) the four edits for head translation, starting with a
fixed 20-unit offset to prove translation itself; (2) three discriminating experiments to decide
whether the stereo wall is resource volume or per-object switching; (3) route D — an engine-native
`uFreeCamera` bound to a viewport, which the same engine family (DMC4) already proves out.

## 中文说明

下面这份 README 是本项目从第一天起的**完整开发与交接记录**（含地址、偏移、命令、判据与实测对照表）。
两点必须知道：

1. **2026-09-25 的编码事故**：用 PowerShell here-string 插入内容时，整个文件被按本机 ANSI 代码页
   重新编码，中文段落**不可恢复地损坏**（显示为 `?`）。好消息是**操作性数据全部完好**——
   所有地址、字段偏移、命令、文件路径、代码块、表格都在；同期补写的英文注记也在。
   **请把 `?` 串当作丢失的叙述，而不是数据。**
2. **英文摘要在上方**，包含"现在能做什么 / 什么做不到以及为什么（十局实测）"。

### 未完成状态与具体困境

**这是一个未完成的实验性项目**，不是成品 mod。公开它的目的是把"已经拿到的结果"和"撞到的墙"都留个记录。

**已经能用的**：头部旋转追踪（3DOF 三脚架，无状态、不漂移）、F9 归位、单画面铺满显示屏的影院模式。
`--config head` 是已验证的部署配置，离线自检 `scripts\check_all.py` 全 PASS。

**卡住的地方（每条都带实测依据）**：

1. **立体（每帧两只眼）在本机不可达。** 十局实测：`stereo=1` 的六局**全部**在 **0.4–4 秒**内崩在显卡用户态驱动
   （`nvwgf2um.dll+0x24678a` 空指针读，或 `d3d11.dll+0x198b74`），均伴随 **LiveKernelEvent 141（GPU 引擎挂起）**；
   而 `stereo=0` 的三种配置分别活了 65 秒 / 65 秒 / 2.5 分钟。已用专门的对照局逐条排除：**双 pass**（21:14 只有一遍）、
   **眼偏移**（21:18 `ipd=0` 照样崩）、**回读次数**（21:18 用重定向、每帧仅一次拷贝）、**帧中取帧**。
   **剩下的变量全在这份代码之下**——在驱动与串流运行时内部，一个 mod 既观测不到也修不了。
2. **真 6DOF（头部平移）尚未实现。** 只差 4 处接线（方案见 `_work/HANDOFF-6dof.md`），但必须与一个**单位 bug**
   一起修：现有眼偏移通道把**米**喂进了**厘米尺度**的世界（引擎常量 `near 16.000 / far 4000000.00`、
   过肩机位 `fwd len 320.16` 单位）——**差 100 倍**。
3. **本进程里没有"零回读"通路。** GPU 侧共享纹理需要 D3D9Ex 设备，而游戏只调用过 `Direct3DCreate9`；
   另造 Ex factory 会在 `d3d9.dll` 内部跳进未映射内存（`execute at 0x0000E084`）。所以每帧都得经 CPU 回读——
   这恰好是立体路径负担不起的那笔开销。

**已经验证过的死路**（免得有人重走）：引擎自带的 `Stereo` 开关是**死代码**（全镜像唯一引用就是那次写入本身）；
`displayCount = 2` 是**双显示器**模式、两个 display 共用一台相机；`mCameraOrg`（`+0xE30`）**渲染器从不读**；
MT Framework **没有 Lua**（`BH6.exe` 里 Lua C-API 字符串 0 命中）；数据驱动的相机（`.ahc`、`CAMtrack*.fsm`）
**与玩法耦合**——删掉会让伙伴 AI 寻路失效。

**仍未解决的问题**：视图构建函数每局被约 48 台不同相机调用，且都带同一套模板姿势（`eye (0,200,-700)`），
所以"哪一台是玩家相机"是靠 `sBioCamera` 槽表 + "存活 ≥700 ms" 两道过滤选出来的，**而非类身份确证**。
另外，文档里如实记录了本项目**五次假结论**（弱判据把"形状像 view 矩阵"当成身份、把 fov 的弧度与度混用
导致整个探针静默失效等），这也是为什么这里每条结论都标了 MEASURED / INFERRED。

**下一步（若有人接手）**：① 4 处改动接上头部平移，先用固定 20 单位偏移证明"平移本身"有效；
② 三个判别实验判定立体那堵墙属于"资源量"还是"每对象切换"；③ 路线 D——用引擎自带的 `uFreeCamera`
挂到 viewport 上，作为更干净的底座（同引擎家族的 DMC4 已走通这条路）。

---

# RE6 VR Mod - handoff guide

> **README damage note (2026-09-25, honest record).** A PowerShell here-string used to insert one
> section re-encoded this whole file to the machine's ANSI code page. The UTF-8 bytes of the
> non-ASCII text were converted and are **not recoverable**; they have been replaced by `?`.
> What survived intact is everything that matters operationally: **all addresses, field offsets,
> commands, file paths, code blocks and tables**, plus the overall structure (99 headings).
> The English notes added in the same sessions are also intact. So treat the `?` runs as lost
> prose, not as data.
>
> **Current state, in one place** (the rest of this file is still the reference for everything
> built before 2026-09-25):

# RE6 VR Mod �?接手指南

> **给新会话**：一切都在磁盘上，与对话无关。本文件是唯一入口，先读完这一节再动代码�?
## 30 秒了解现�?
> **📌 2026-09-24 收工状态（下次从这一段开始读�?*
>
> **能用�?*：把 Steam �?RE6 的画面投�?Quest 3 头显里，双眼各一份、无重影�?> 屏幕大小/距离/观感缩放/比例都正确且可通过标记文件调�?*游戏目录只装 `d3d9.dll` +
> `openxr_loader.dll`**，所有标�?*全部未设**�? 默认 4.0 m / 4.6 m，屏幕铺满显示屏）�?> 部署校验：两�?DLL 的哈希与 `build\` 一致。想恢复这一状态：
> `scripts\deploy.bat`（会顺带移除 dinput8 代理�? `scripts\markers.bat reset`�?>
> **不能用的**�?*游戏视角不跟头动**（仍是虚拟影院）�?>
> **�?2026-09-24 第二轮：静态分析做到底了�?* 本项目现在有**一个经过真值验证的
> 反汇编器**（`scripts\disasm_lib\`，用 ml.exe + dumpbin 做往返测试）�?> 全镜像递归下降�?*1 873 873 条指令�?3 722 个函数�? 个无法解码的字节**�?> 并且�?RE6 �?*反射系统整个解出来了**�?*3372 �?MtDti �?*，类�?静态地址/实例大小/父类
> 全部静态可读，相机是其�?20 个类的家族�?> 判据、相机类地图、以�?*哪些是实测哪些是假设**�?> [「RE6 的类地图拿到了」](#-2026-09-24-第二轮re6-的类地图拿到�?372-个类--相机全家�? 一节�?> **最要紧的两�?*�?> 1. 旧的「无静�?MtDti / DTI 不可达」是**错的**——现在有 3372 个类的静态锚点；
> 2. 相机管理�?`uCameraCtrl`�?x4B70 字节）的候选全局�?**`0x017D270C`**�?>    �?*它是不是实例指针还没证实**（一次内存读即可定论，见该节「没有验证的东西」）�?>
> **本次会话新增的可复用工具**（都�?`scripts\`）：`disasm_lib\`（PE 加载�?+ x86 解码�?+
> 递归下降分析�?+ **`dti.py` 类地�?* + **`managers.py` 工厂/context 地图** +
> **`camera.py` 相机地图**）、`re6dis.py`（`disasm` / `func` / `xref` / `str` / `stats` /
> **`dti`** / **`manager`**）、`analyze.bat`（重建分析缓存）、`tests\run_tests.py`（解码器真值测试）�?> ⚠️ `x86_disasm.py` / `xrefs.py` / `dti_walk.py` 已被取代（前者自证不可靠，`dti_walk.py`
> 找错了字段偏�?`name-4`，实�?`name` �?`+4`），只用 `re6dis.py` �?`disasm_lib`�?>
> **�?第三轮（同一晚）：每个类的字段偏移也静态恢复了�?* `disasm_lib\propmap.py` �?*属性注�?> 指令�?*里读�?`(字段�? 类型, 字节偏移)`�?37 张表 / 4608 条字�?/ **404 个类连类名一起定下来**�?> `sBioCamera` �?**`mCameraOrg[i]`（步�?0x40，i=0..7）`cameraPos +0xE30`、`targetPos +0xE40`�?> `cameraUp +0xE50`、`fov +0xE60`** �?README 里那份手工解析逐项一致，**所�?相机字段偏移�?> �?DMC4 借来的猜�?这条警告作废**。新命令 `python re6dis.py props <类名>` /
> `python re6dis.py field <字段�?`。见
> [「相机字段偏移已经静态核实」](#-2026-09-24-第三轮相机字段偏移已经静态核实不再依�?dmc4-的猜�? 一节�?>
> **�?第四轮：运行时探针也写好并部署了（`src\mem_cam.cpp`，标�?`re6vr_cam.txt = 1`）�?*
> 它按**类指�?*（不是形状）在内存里找活�?`sBioCamera`，对每个命中�?*几何自检**�?> 搜索逻辑本身�?*合成自检**（`python scripts\cam_selftest.py`，不需要游戏，当前 **PASS**）�?> **只差跑一局：正常启动游戏、进关卡，然后看日志里的 `cam:` 段�?* �?> [「运行时探针 `mem_cam.cpp`」](#-2026-09-24-第四轮运行时探针-mem_camcpp已部署等你跑一局) 一节�?
�?Steam �?Resident Evil 6�?2 �?DX9 / MT Framework 2.x）做�?VR 注入层�?
**已经做到�?*：游戏画面出现在 Quest 3 头显里，双眼各画一份、读回、提交全链路打通，
每帧提交无失败；重影（每眼非对称 FOV）已解决；屏幕大�?距离/观感缩放可调（标记文件，
�?屏幕大小调不动的根因"一�?—�?2026-09-23 晚才真正修好）�?
**还没有做到的**�?*游戏视角不跟头动**。当前是"把游戏画面贴到世界里一块银幕上"�?也就是虚拟影院，不是 VR。要变成�?VR，必须让**游戏相机**跟着头转�?
**当前路线**�?*已放�?改顶点着色器常量里的矩阵"**（实测证伪，见下节）�?转为**�?RE6 的相机对象并写它的参�?*（`mTargetPos` / `mCameraUp`），
这也�?REFramework 唯一走通的方式�?
```
代码     C:\re6vr\
部署     C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\d3d9.dll + openxr_loader.dll
日志     C:\re6vr\_work\re6vr.log     （由 re6vr_logdir.txt 指定目录�?开�?    scripts\markers.bat              （不带参数即显示当前所有开关）
```

> ⚠️ Steam 校验文件或重装游戏会删掉这两�?DLL，需要重�?`scripts\deploy.bat`�?> ⚠️ **Steam 不传环境变量**，所以所有开关都用标记文件；`markers.bat` 会写�?> `re6vr_logdir.txt` 指的那个目录，写错地方等于开关不存在�?> ⚠️ **标记目录曾经不一致，坑了一整轮**�?026-09-23）：游戏跑的�?> `_work`（日志里 `screen distance from C:\re6vr\_work\...` 可证），�?> `build\re6vr_logdir.txt` 当时指着 `_logs\matrix_regress`�?> 于是 `markers.bat` 写的开�?*游戏根本读不�?*（日志里 `head: candidate ... chosen=0`）�?> 现在两处都指 `_work`�?*怀疑某个开关没反应时，先确认这一�?* —�?> 判据：日志开头应出现 `... from C:\re6vr\_work\re6vr_xxx.txt` 这一行�?
## �?已放弃的路线：改顶点着色器常量里的矩阵�?026-09-23 定论�?
**不要重走这条路�?* 结论是实测得出的�?
### 判决性推理（先看这条�?
**�?view 矩阵乘旋转永远不会拉�?*——正交阵乘旋转仍是正交阵。所以旋转一个真正的
view 矩阵只有两种结果�?*视角转过�?*，或�?*什么都不发�?*。两者都不含"变形"�?
实测三件事：

| 实验 | 结果 |
|---|---|
| `pick = -1`（全�?view 候选）�?60° | **画面毫无变化** |
| `pick = 9` �?60° | **画面毫无变化**（且事后确认该次可能根本没写入，见下�?|
| 早先"只有变形/拉伸" | 转到�?UI / 屏幕空间矩阵（平移是 `(0,0,1)`、`(1023,1662,17)`�?|

**结论**：顶点常量流里没�?引擎级统一�?view 矩阵槽位"，这与调研一�?（MT Framework �?*逐材�?shader**，同一 `c` 槽位在不同材质里语义不同）�?
### 相机候选的实测轨迹（供后人参考，别再重复测量�?
�?`cam_find.py` / `reg_compare.py` 分析一局真实游戏�?trace，三�?会动"的候选：

```
reg 1   屏幕空间：z 分量恒为 17.0，从不变化（(1920,1005,17), (1920,1176,17)�?reg 9   轨迹最像走路：(3247,-413,-3042) �?(3268,-413,-3018) �?(3269,-411,-3015)
                     �?(3177,-408,-3108) �?(3052,-412,-3230)   步长 3.9 �?290
reg 27  剧烈瞬移，步长中位数 4950
```

reg 9 是三者中最像世界相机的�?*但旋转它画面依然不变**�?
### 五次判据全部失败的记录（这是本节最该读的部分）

| # | 判据 | 结果 |
|---|---|---|
| 1 | 转所有分类为 view 的矩�?| 转到 UI 矩阵 �?画面拉伸 |
| 2 | 平移量级 �?50 | 选中视口矩阵 `(1920,�?` |
| 3 | 按累计位移排�?| 仍选中视口矩阵（它跳得最多） |
| 4 | 平移"最小分�? �?50 | 选中 reg 27（某帧噪声碰巧胜出） |
| 5 | 步长平滑性（整段轨迹�?| 阈值把连续走动与瞬移混在一起，仍分不开 |

**模式很清�?*：一直在用启发式去猜一�?*从矩阵形状上无法区分**的东�?（相�?/ UI / 视口 / 骨骼矩阵都是"正交 3×3 + 平移"）�?*换判据不是解法�?*

### 沿途踩到的静默失败（症状全都一样：*开关读了、日志说已启用、什么都没发�?�?
| # | Bug | 后果 |
|---|---|---|
| 1 | 安装守卫�?`g_head_enabled` | 钩子根本不装 |
| 2 | `pick = -2` 预扫描把窗口当候选计�?| 永远选不�?|
| 3 | 函数体守卫漏 `g_trace_enabled` | 整局游戏�?trace 输出 |
| 4 | `pick = N` �?N �?帧内候选序�?而非寄存器号 | `pick=9` 永远选不中（序号只到 5�?|
| 5 | 周期性报告挂�?只在某开关开启时才递增"的计数器�?| 写入审计永不打印 |
| 6 | 候选日志被 `g_head_hits < 3` 限制在前 3 次调�?| 关键寄存器（reg 9）从未被记录，导�?*�?没写�?误判�?写了没效�?** |

**现在的强制规�?*：凡�?改引擎参�?的开关，**必须有一条日志证明写入真的发生了**�?已加�?`head: write audit`（每 600 帧报告每个寄存器的写入次数，写入为零时明确说
`(NONE - head look never wrote anything)`）�?*没有这行证据�?没反�?的结论不成立�?*

## 正路：写相机对象（进行中�?
MT Framework 的相机是 **look-at 参数化相�?*（`uCamera`：`mCameraPos` / `mTargetPos` /
`mCameraUp` + FOV），**对象上不存矩�?*，view 矩阵每帧由虚函数现算�?所以正确做法是 **hook 相机�?update，改它的参数**，而不是改渲染时用的矩阵�?
已确认的前置条件�?- **RE6 �?exe �?DTI 反射类名**（`sBioCamera::ViewportCamera`、`sCamera::Viewport`�?  `uCameraCtrl`、`uCoord` 等），exe **未加�?*�? 个节，`.text` 原始=虚拟大小）�?- �?**2026-09-23 晚修�?*：原�?**没有任何静态指针指向这些类�?*"—�?*实测是错�?*�?  `BH6.exe` 里共�?10741 �?`push imm32 <字符串指�?`（`68 xx xx xx xx`），其中�?34 �?  推的就是相机类名，每个类�?*只有一个调用点**，即 DTI 注册点（已逐个反汇编核对）�?
  | 类名 | 字符�?RVA | 注册�?RVA |
  |---|---|---|
  | `sBioCamera` | `0x111A9DC` | `0x104DA40` |
  | `sBioCamera::ViewportCamera` | `0x111A9E8` | `0x104DA7D` |
  | `uCameraCtrl` | `0x112D6C8` | `0x1053E30` |
  | `uCameraBase` | `0x112C778` | `0x1053860` |
  | `cCameraParam` | `0x112AA04` | `0x104DABD` |
  | `uCoord` | `0x12E926C` | `0x10D4180` |
  | `uCamera` | `0x12EDFD8` | `0x10D61B0` |
  | `uFreeCamera` | `0x12EE084` | `0x10D6230` |
  | `sCamera` / `sCamera::Viewport` | `0x12E27F8` / `0x12E27E4` | `0x10D2B10` / `0x10D2AD0` |

  （文件偏�?= RVA �?0x1000 + 0x400，ImageBase `0x400000`。核对方法：
  `python` �?exe，在注册点取 6 字节，`68` �?4 字节即字符串 VA。）
  �?**不必移植 MHW-ClassPropDump，也不必运行时遍�?DTI 哈希�?*：属性表就是 `.text`
  里的静态代码（`C7 44 24 10 <字段�?` + `C7 44 24 14 <类型枚举>`，类�?  `Vector3=0x14 / float=0xC / int32=0x6 / bool=0x3 / ptr=0x2 / Coord=0x13`，共 8669 条，
  字段偏移由前一�?`lea reg,[ebx+disp32]` 反解）�?- **`sBioCamera::ViewportCamera` 的布局**（DTI �?RVA `0xFA591`–`0xFC57E`，自洽）�?  `mCameraOrg[i]`�?*步长 `0x40`**，`i = 0..7`，每�?  `cameraPos/targetPos/cameraUp/fov/nearPlane/farPlane` =
  **`+0x00 / +0x10 / +0x20 / +0x30 / +0x34 / +0x38`**；`i=0` 基址 **`+0xE30`**
  �?`i=7` 末字�?`+0x1028`�?×0x40 严格递增，互证）。另�?`mScreenType +0xCE0`�?  `mCameraParam +0xD80`、`mViewportCamera +0x12A0`、`CameraPatch_TransFov +0x1610`�?  `CameraPatch_ReadyFov +0x1614`、`CameraPatch_ReadyOffsetZ +0x1618`�?  **这正�?look-at 相机要找的东�?*：改 `mCameraOrg[i].targetPos` / `cameraUp` 就是转头�?- **头显姿态链路已验证可用**（`pose: orientation_valid=1 ... yaw` 随头动）�?  所以一旦接上相机，头部数据是现成的�?- 内存扫描�?`src/mem_scan.cpp`（`re6vr_scan.txt`）已能：抓两个机位的 view 矩阵 �?  反算相机世界坐标 `p = -Rᵀ·t` �?全内存找**同时持有两个位置**的字段�?  它已定位到候选地址，但还没有可用地识别出相机的参数字段（打到的多是屏幕空间数据）�?
**下一步（已换成有唯一目标的做法）**：把 `mem_scan.cpp` 的扫描判据从"同时持有两个位置"
改成**�?`mCameraOrg` 签名**——连�?8 组、步�?`0x40`、每组前三个�?`{pos, target, up}`（两组单位正交向�?+ 一个位置），且 `cameraPos �?-Rᵀ·t`（这个位�?扫描器已经会算）。命中即拿到相机对象，随后写 `targetPos`/`cameraUp`�?不确定性：引擎可能每帧覆盖 `mCameraOrg`（`VR-headtracking-research.md:780,792` 有警告）�?那就退�?hook 它的读取点�?
**2026-09-23 晚：上面这步已经落地一�?* —�?`mem_scan.cpp` 里新�?`check_camera_org()`：对内存扫描出的每个"相机世界坐标"候选地址，按 DTI 偏移把它�?`mCameraOrg[i]` 读一遍并�?*与偏移表无关的几何校�?*
（`up` 是不是单位向量、`forward = target - pos` 是不是单位且�?`up` 垂直�?`right = up × forward` 是否等于 view 矩阵的第一列、`fov` 是否落在 0.05�?.2 rad�?`pos` 是否等于 `-Rᵀ·t`）。全过就打印�?
```
scan: *** CAMERA ORG ENTRY CONFIRMED at 0x1A2B3C4D *** ...
```

不过就打印哪一条判据没过（`up!unit` / `right!=view-row` / `pos!=view` …）�?用来区分"偏移错了"�?这个候选根本不是相�?�?
**用法**（`scan` 会因�?Steam 不传环境变量而用标记文件）：

```bat
scripts\markers.bat scan 1        :: 然后启动游戏，进关卡后【走�?5 秒以上并转头�?python scripts\...                :: 扫描结果在日志里，搜 "CAMERA-ORG" / "candidate"
```

**必须先动起来**：扫描器要等相机位移超过 60 单位才会开扫（停着不动时，
一个位置会撞上上千个巧合地址）�?
**2026-09-23 深夜第二轮：抓取改成"动起来才�?（连废两局后）**

原设计是"�?pose 1�? 秒后�?pose 2"，结�?*连续两局都抓在菜�?读盘�?*（相�?3 秒内
只挪 0.6�?.2 单位，而要�?400），两次抓取其实是同一姿势 �?扫描必然一无所获，
日志还会写成"nothing holds the camera's world position"这种**听起来像结论的假结论**�?
现在改成**运动触发**�?
```
captured camera pose 1/2 - translation (...)   �?相机第一�?像个世界坐标"时就�?  Pose 2 is NOT taken on a timer: it is taken as soon as the camera has MOVED 60 units
captured camera pose 2/2 - translation (...), N units from pose 1 (needed 60)
  - conditions met, the scan can run on a real pair
```

- pose 1 抓完最多等 **60 �?*；期间没动就**丢掉 pose 1 重新武装**（不再让一整局白跑）；
- 阈值从 400 降到 **60**：实测菜单里 3 秒只�?0.6 单位，而走动时远超 60�?- 条件不满足时**拒绝出结�?*并明确说原因（`REFUSING TO SCAN`），不再打印
  "conditions met" 这种与事实相反的行�?
**用户操作因此变成一句话：进关卡后正常走动即�?*，不需要掐时间。转头仍要累计超�?20°
（这是搜�?相机朝向"用的，不满足也能定位位置，只是少了朝向那一半证据）�?
**2026-09-23 深夜：开关已开（`_work\re6vr_scan.txt = 1`），等一局实机数据�?*
操作（现在只需一条）�?*进关卡后正常走动、并转头累计超过 20°**。抓取是运动触发的，
不用掐时间——pose 2 会在相机真的移动 60 单位后立刻抓取�?
日志里要看的三种行：

```
scan: pose 1 (..), pose 2 (..), camera moved N units, head turned M deg - conditions met, scanning
scan: *** CAMERA ORG ENTRY CONFIRMED at 0x........ ***     <- 想要的就是这�?scan:     camera-org check at 0x........: not a match ( right!=view-row pos!=view )   <- 有用：说明差在哪
```

**命中后下一�?*就是�?`mCameraOrg[i].targetPos` / `cameraUp`（偏�?+0x10 / +0x20�?步长 0x40），或�?hook 它的读取点�?
### �?2026-09-23 深夜第三局：喂给扫描器的矩�?*不是刚体**，搜索注定零命中

运动触发生效了（这次真的抓到两个不同姿势：`4512.6 units apart, head turned 73 deg`），
但四路搜索全�?0 命中�?*问题在输入，不在方法**�?
```
pose 1 translation (448.8 -155.6 -484.2)      -> p = -R^T·t = (-5.29 339.60 -587.15)
pose 2 translation (-3806.9 -680.7 -1890.3)   -> p = -R^T·t = (1890.32 680.69 -3806.92)

平移行移动了 4512.6，�?p 只移动了 3751.9 —�?比�?1.20
```

**同一个刚体变换必然满�?`|p2 - p1| = |t2 - t1|`**（因�?`p2 - p1 = -Rᵀ(t2 - t1)`�?旋转不改变长度）。两者不等，说明被喂进来的矩�?*根本不是相机位姿**（很可能�?view×projection 或带缩放的合成矩阵）。既不刚体，内存里当然没有任何地址持有由它推出�?"位置"，于是搜索无论写得多对都只会打印 `nothing holds the camera's world position` —�?**又是一条听起来像结论的假结�?*�?
而探针选它的唯一理由�?平移每个分量都大�?50"—�?*和之前被证伪的三个判据一样弱**�?
**改法（本次）**�?1. `mem_scan.cpp` 新增 `is_rotation_3x3()`�?*行、列都必须是单位正交**，否则拒收并打印
   `scan: refused a matrix that is not a rigid pose (basis not orthonormal)`�?2. 探针不再自己挑一个寄存器�?*每个候�?view 寄存器都交给扫描�?*，由扫描器用刚体判据
   选第一个合格的�?最大平�?这种启发式在这里已经没有立足之地）；
3. 于是"选谁"这件�?*不再依赖猜测**：不合格的会在日志里被点名拒收，合格的才会进�?   pose 1/2 抓取�?
> **本项目的第四�?弱判�?事故**：形状像 view �?平移量级�?�?每个分量 > 50 �?> 现在这条"必须刚体"�?*每次都是同一个错误：拿一个必要条件当充分条件�?*
> 唯一没有翻车的是**几何身份**（`|p2-p1| == |t2-t1|`、正交性），因为它们是真必要条�?> 且可证伪�?
### �?第四局：刚体筛选生效了，但选中�?**viewport 矩阵**�?920, 1005, 17�?
```
scan: offering reg 1 (translation 1920.0 1005.0 17.0, travelled 0)
scan: captured camera pose 1/2 - translation (1920.0 1005.0 17.0)
scan: captured camera pose 2/2 - translation (1920.0 609.0 17.0), 396.0 units ...
scan: the camera's world position was (-1920.00 -1005.00 -17.00), then (-1920.00 -609.00 -17.00)
```

`1920` 是渲染宽度、`17` 是常�?—�?这是**屏幕布局**，不是世界坐标。它能通过刚体判据�?而且确实在动（视口随画面滚动），又因�?*总是第一个被交给扫描�?*，就赢下�?pose 1�?于是又白跑一局�?
**改法**：新�?`looks_world_space()` —�?**两个以上分量是精确整�?*、或**有分量被钉在
25 以下的小常量**、或模长不足 200 的，一律拒收并打印
`scan: refused a screen-space translation (...)`�?
> 到这一局为止，这�?找相�?的路线已�?*连续四局**没有产出，四次失败原因各不相同�?> 但都属于同一类错误（弱判据）�?*下一局是这条路的最后一次尝�?*：若合格的寄存器仍然
> 搜不到位置，就说明位置不是以三个连续 float 存放的，应当放弃"搜内�?�?> 改为 hook DTI 注册点（RVA `0x104DA7D`）直接拿对象指针�?
### ⛔⛔ 第五局=收尾：顶点常量流�?*从来没有**相机矩阵（搜内存路线到此为止�?
屏幕空间筛子生效了（`refused a screen-space translation (1920.0 1005.0 17.0)`），
扫描器终于拿到一�?*世界空间**的候选对�?
```
captured camera pose 1/2 - translation (331.6 -285.8 -549.3)
captured camera pose 2/2 - translation (449.0 -155.7 -484.0), 187.0 units from pose 1
the camera's world position was (-134.35 168.26 -668.57), then (-5.29 339.60 -587.15)
```

**这一对同样不满足刚体一致�?*：平移行移动 `187.0`，而由它推出的世界位置移动 `229.4`
（比�?`0.815`，必须为 `1.000`）。加上第三局�?`1.20`，两个不同寄存器、两个不同姿势对�?**比值都不是 1** —�?这不是巧合，是结论：

> **喂进来的矩阵都不是相机位姿。`SetVertexShaderConstantF` 的常量流里没有主相机矩阵�?*

这也解释了为什�?转头看画面不�?�?搜内存零命中"是同一个原因的两个侧面：那条流里根�?没有相机。四次失败的判据（形状像 view / 平移�?/ 分量>50 / 刚体 / 世界空间）都在试图从
**不含目标的数�?*里认出目标，所以每一次都只是�?下一个假目标"筛出来�?
**这条路（在顶点常量流里找相机、搜内存定位相机对象）到此结�?*，不要再加判据重跑�?
### ⛔⛔�?第六次尝试：DTI 代码**不可�?*（`0x104DA40` 那一片没有任何调用者）

第五局之后转向 "hook DTI 注册点拿对象指针"。静态分析（`scripts/xrefs.py`，只做精确的
编码匹配，不做通用反汇编）�?`RVA 0x104DA40`–`0x104DB00` 这一片的结果�?
| 查什�?| 结果 |
|---|---|
| 绝对 dword 引用（函数指针表 / vtable / 数据引用�?| **0 �?* |
| 相对 `E8` call（目标落在该片内�?| **0 �?* |
| 相对 `E9` jmp / 任何跳入该片的分�?| **0 �?* |
| 整个 `0x104D000`–`0x104E000` 页的入边分支 | **0 �?* |

字节本身是真实的：`68 e8 a9 51 01`（`push "sBioCamera::ViewportCamera"`）�?`b9 a4 31 7c 01`（`mov ecx, 0x017C31A4`）、`e8 ...`（call），前后各有 `59 c3`（pop/ret�?�?`cc` 对齐填充�?*看起来像一组合格的注册函数** —�?但全二进制里没有任何一条指令或数据
指向它们�?
> **结论：`BH6.exe` 里的 DTI 类名与属性表是开发版残留代码，正�?build 里没有任何路�?> 能到它�?* 这也解释了为什么前面的推理会走偏：那些字符串和字段表是**真的**（ASCII�?> 可解析、偏移自洽），但"真实存在"不等�?运行时可�?�?> **一次可达性检查（xrefs）本该在动手实现之前就做** —�?这是本轮最贵的一课�?
**于是"找相机对�?的两条子路线（搜内存 / hook DTI）全部判死�?* 不要再在这上面花时间�?
### �?但输入路线是通的：游戏用 **DINPUT8**（不�?XInput�?
同一�?PE 导入表（只读解析）：游戏导入 `DINPUT8.dll`�?*没有 XInput 系列**。而右摇杆控制
镜头正是�?DirectInput �?游戏自己的相机控制器。这意味着**不需要相机对�?*也能让画面跟
头转：把 `DINPUT8.dll` 也做成代理（�?`d3d9.dll` 完全一样的做法），�?`IDirectInputDevice8::GetDeviceState` 返回前，�?*右摇杆的水平�?*叠加一个来自头�?yaw �?增量 —�?游戏会把它当成玩家在推右摇杆，于�?*内置的相机控制器**替我们完成转视角�?包括它自己的插值、限位、碰撞与脚本接管�?
这条路线的优点：不碰引擎内存、不依赖任何"像不像相�?的判据、可离线先验证（读导入表
+ 反查 `GetDeviceState` 的调用者）�?需要先确认的一件事�?*水平轴到底是哪个（lX 还是 lZ / 哪个 dwOfs�?*，办法是代理里把每个�?的原始值打进日志，进游戏推一次右摇杆，看哪个字段跟着动�?
## 头部视角注入（Stage 3）历史记�?
**2026-09-23 真机第二轮：画面不跟着转，只会拉伸变形�?* 日志定位到两个原因（都已修）�?
### 原因 1：两个渲染线程共享状�?�?旋转自己叠自己（已修�?
游戏�?*两个线程**（实�?`t=31312` �?`t=20856`）上传常量，�?`g_frames_seen`�?view 计数、锚点全�?*全局变量**：一个线程把另一个的计数清零、把已经转过的矩�?重新当成"引擎原�?锚定，于是旋转逐帧累积 —�?日志�?4 �?`COMPOUNDING`�?
修法：所�?当前�?状态改�?`thread_local`，帧边界不再来自 Present（Present 只在一�?线程上跑），而是**从上传流本身推断**：引擎每帧重建同一批常量缓冲区，所�?`(start, count)` 序列是循环的�?*看到一个本帧出现过的批次就说明新的一帧开始了**�?
> 顺带修掉一�?*假告�?*：原来的累积自检比较的是"本帧结果 vs 上一帧结�?�?> 而转头时结果本来就该�?—�?这个检查在真机上只会一直误报。现在改�?> **从锚点算出应有的值再比对**，这才分得清"累积"�?转头"�?
### 原因 2：`pick = -1` �?UI/精灵矩阵也转了（已修�?
日志证据�?
```
head: frame 16 view#3 reg 4 rotated ... translation (0.000 0.000 1.000)     <- 平移 (0,0,1)
head: frame 1091 view#4 reg 1 rotated ... translation (312.000 1956.000 17.000)
```

游戏世界坐标�?*几千**量级，这两个都不是世界相机，�?HUD/精灵的摆放矩阵�?**"形状�?view 矩阵"无法区分相机�?UI —�?两者都是刚体变换�?* �?**"会拉�?本身就是判据**：真正的 view 矩阵乘旋转仍然是正交的，永远不会拉伸�?会拉伸说明转到了别的矩阵�?
**修法：按行为找相机，而不是靠猜寄存器�?* `pick` 现在支持 `auto`�?观测每个候选的旋转逐帧变化量，**主相机会随玩家转头而变，UI 矩阵不会**�?把冠军锁定为相机�?*只转�?*。校准期间不写任何东西（否则会污染被测量的信号）�?找到后日志会明说�?
```
head: auto - the camera is the matrix at reg 27 (it was slot #2, rotation moves 0.021 ...)
```

**调参**：`re6vr_head_view.txt = <增益> <目标>`，目标取�?`auto`（默认，按行为找相机�? `all`（全部，已证明会拉伸�? `<序号>`（手工指定）�?增益为负则反向（`head -1 auto`）�?
### 配套工具

```bat
:: 只读测量：每帧记录每�?view 候选的旋转/平移变化，用来判定哪个寄存器是相�?scripts\markers.bat trace 1
python scripts\trace_check.py _work\re6vr.log
::   判据�?平移位移"：世界相机走动会移动上千单位，HUD 的平移恒定不�?```

### 参考：其他 VR mod 是怎么做的�?026-09-23 调研�?
- **REFramework**（RE Engine 全系 VR�?*完全不碰着色器常量**。它 hook 引擎自己�?  `via::Transform::updateTransform`，在引擎更新**之后**直接写相机对象的
  `position` / `angles`(四元�? / `worldTransform`（`FreeCam.cpp` �?  `transform->get_world_transform() = m_last_camera_matrix;`）�?- **Vireio Perception**（开�?DX9 VR 驱动，场景与本项目最接近）改的是**投影矩阵**�?  而且�?`inMatrix * invertProjection * transform * reProject` —�?因为它面对的�?  World×View×Projection 合体矩阵�?*必须先把投影除掉**�?- **MT Framework 的相机是 look-at 参数化相�?*（`dmc4_hook` �?`uCamera`�?  `mCameraPos` / `mCameraUp` / `mTargetPos` + FOV�?*对象上不存矩�?*，view 矩阵�?  由这些现算的）。所�?转头"�?MTF 里的正确杠杆点是**�?target / up**�?  矩阵是引擎每帧重建的产物�?
**结论**：本项目的常量路径是"够用但脆�?的一层适配（靠行为识别而非形状识别），
因为 RE6 �?32 位且没有 REFramework 那样�?SDK。`auto` 是这条路线上能做到的
最稳做法；若它仍不成功，正路是顺着 `uCamera` 的思路�?RE6 的相机对象�?
### ⚠️ 为什�?形状�?view 矩阵"这个判据从根上不成立

完整调研�?**`_research\VR-headtracking-research.md`**�?55 行，带源码引用与验证等级）�?其中最要命的一条，直接否定了本项目原来的做法：

> **任何正交矩阵乘一个旋转，结果仍是正交矩阵�?*
> 所以如果真的改到了 view 矩阵�?3×3，无论乘法顺序对错，**结果都还是一个合法旋�?* —�?> 最坏是转反了或绕错轴，**绝不会拉�?*�?> **⇒「画面拉伸」本身就�?改到的不是纯 view 矩阵"的判决性证据�?*

�?正交 3×3 + 世界尺度平移 = 相机"这个判据�?*同时命中**�?
| 也会被命中的东西 | 转了它的后果 |
|---|---|
| **骨骼蒙皮矩阵调色�?*（DX9 就是�?`SetVertexShaderConstantF` 批量上传�?| **角色/物件变形** |
| **VP / WVP 合体矩阵** | 透视除法�?w 不再�?view 深度的纯函数 �?逐像素缩放不�?= **非均匀拉伸** |
| 法线矩阵 / inverse-transpose（含 1/scale�?| 剪切 |
| 相机**世界**矩阵 C（不�?view 矩阵 V，两者互为逆） | 方向相反；且与用 V �?pass 不一�?�?"像被撕开" |
| 物体�?world 矩阵、阴�?反射相机 | 部分 pass 转了部分没转 �?扭曲 |

而且 **MT Framework 没有"引擎级统一 view 矩阵槽位"**：MTF �?*逐材�?shader**�?同一�?`c` 槽位在不同材质里语义不同（DMC4 里只�?pixel shader �?`c0`=相机位置�?`c7`=相机前向是实测到的）。所�?扫一遍常量找相机"这件事在 MTF �?*原理上就没有稳定的目�?*�?
**这也是为什�?Vireio（开�?DX9 VR 驱动）改的是投影矩阵而不�?view 矩阵**�?而且必须 `inMatrix * invertProjection * transform * reProject` —�?先把投影除掉�?
### 正路：改相机对象，别改矩�?
MT Framework 的相机是 **look-at 参数化相�?*（`uCamera`：`mCameraPos` /
`mTargetPos` / `mCameraUp` + FOV�?*对象上不存矩�?*，view 矩阵是每帧现算的）�?所�?转头"�?MTF 里的正确杠杆点是�?
1. **�?`mTargetPos`（yaw/pitch�? `mCameraUp`（roll�?*，或直接加在引擎�?   yaw/pitch 标量上（DMC4 实测相机 yaw �?`[esi+0x268]`、yaw 目标 `[esi+0x260]`）；
2. **hook 点�?读取相机字段的那条指�?**，而不是写指令 —�?这样不会被引擎后续覆盖；
3. 还要**抑制引擎的自动回�?锁定校正**，否则头部旋转会被每帧拉回�?
**定位 RE6 相机的路�?*（未验证，是下一步的工作）：
MTF 2.0 �?exe 里内�?**DTI 反射信息**（类�?+ 字段�?+ 偏移 + 虚表地址）�?�?RE6 也有，改一下公开�?`MHW-ClassPropDump`�?4 �?�?32 位）就能**直接 dump �?相机类名与字段偏移，彻底不用�?*。这是最省时间的路�?

## 头部视角注入（Stage 3）早期记�?—�?以下机制已废弃，保留供追�?
> ⚠️ **本节描述�?旋转视图矩阵"机制已被证伪**，见文首"已放弃的路线"�?> 保留它是因为里面�?*失败模式**（预扫描计数、`pick` 语义、守卫漏开关）
> 仍然适用于今后任�?hook 引擎参数的工作�?> 另注：下面写�?`pick = >=0 指定序号` �?*旧的错误语义**（帧内候选序号）�?> 现在的含义是**寄存器号**�?
**目标**：转头时游戏里也跟着看。`src/matrix_probe.cpp` �?`g_head_enabled` 分支会把
主相机的**视图矩阵 3×3 �?*按头部姿态旋转（平移不变，绕自身转，不会滑走），
姿态来�?`views[0].pose.orientation`（与屏幕锚定同一�?LOCAL 空间）�?
**开�?*：`re6vr_head_view.txt` = `<增益> [<pick>]`�?`scripts\markers.bat head 1 -1` 一条命令设好�?
### 2026-09-23 真机"无反�?的根因（已定位并修复，无需再猜�?
上一轮日志（`_work\re6vr.log`�?3 MB / 604,506 行）其实已经把答案写在里面了�?**�?1 �?确认注入有没有发�?不需要再跑游�?*�?
```
cap: hook call 1 (reg 1, 1 vec4) enabled=0 shift=0 head=1     <- 钩子装了、在调用
head: candidate kind=VIEW index=67811 chosen=0 count=128 reg 4  <- 分类器认出了相机
```

分类�?*是对�?*（`reg 4/6/27` = `VIEW`，`reg 1` = `PROJ`），�?*每一个候选都�?`chosen=0`**。于�?604,300 次调用全部走空，一个矩阵都没被改过 —�?这就�?无反�?�?
**Bug（三处，都在 `pick = -2` 这条路上�?*�?
1. **预扫描计数器算错了�?* �?最后一�?view"的预扫描�?`n` �?*每个寄存器窗�?*自增�?   而主循环�?`index` 只对**真正�?view 候�?*自增。真实批次里 view �?reg 27�?   预扫描得�?`last = 23`，主循环最多只�?`index = 2` �?**永远不相�?*�?   这就�?每次调用�?`chosen=0`"的直接来源。症状是**完全静默**：没有报错、没有告警�?2. **view 序号�?帧内"的，不是"批内"的�?* 实测同一帧里：`reg 0` 批给�?   `view#2, view#3`，`reg 24` 批给�?`view#5`。所�?帧内最后一�?view"
   **在上传它的那一批里根本不可能知�?* —�?那时它还没出现�?3. **`if (!g_head_apply_valid)` 把它锁死在第一个值上�?* 即使算对了，这个守卫也会
   让目标槽位永远停在第一次算出的那个（实测卡�?slot #3，而真正的最后一个是 #5）�?
**修法**：`pick = -2` 改为**延迟一帧执�?* —�?这一帧记�?最后上传的 view 槽位"�?下一帧再旋转它（引擎每帧重新上传同样的矩阵，所以槽位稳定）。用帧内顺序数组
`g_head_order[]` 按批次累积，天然得到真正�?最后一�?�?
**同时修掉�?*�?
| 问题 | 后果 |
|---|---|
| 分类器把"identity 旋转 + 平移"标成 `UI/world-axis`，选择逻辑只认 `VIEW` | **没转头的相机永远选不�?*（标题画面相机正�?identity�?|
| 头动分支的锚点从不复�?| 锚点会锁在某一帧的值上，转头时旋转�?*累积**（转 90° 实际�?180°�?|
| `pick = -1`（全转）在旧代码里是唯一能生效的档位 | 现在 `-1` / `-2` / `>=0` 都经过离线验�?|

**`pick` 该用哪个**：真机先�?**`-1`（全转）**。游戏每帧上传的 view 矩阵不止一�?（实�?`reg 1/4/6/27` 都会出现，且同一矩阵会被上传两次），在没有确认哪一个才是主相机
之前，`-1` 是唯一不依赖猜测的档位；`-2` 也已经修好，可以接着对比�?
### 离线回归（不需要头显，也不需要跑游戏�?
```bat
:: 三台相机、两个批次，正是当初出错的形�?build\harness.exe build\d3d9.dll 4 1280 720 vsviews
python scripts\head_check.py _logs\matrix_regress\re6vr.log --pick -2
::   PASS: the frame's last view matrix was selected, the rotation is rigid,
::         and it is identical every frame (no compounding).
```

`head_check.py` 检查三件事，每一件都**曾经是错�?*：选中帧内最后一�?view 矩阵�?旋转不累积（同一输入逐帧完全一致）、identity 相机也能被选中�?
**乘法顺序也已用非 identity 相机实测确认**（`vsviews` / `camconvention` 模式）：
代码算的�?`anchor × R`（R 作用在相机自身坐标系里），这正是视图矩阵该有的顺�?（`V' = V·R`）。手�?C 相机结果与日志逐位一致�?
**已知会干扰的现象**：`yaw_follow` 会在头部偏转超过�?45° 时把屏幕重新锚定到正前方 —�?测试头部视角时它会让画面"�?一下。现在它**也支持标记文�?*�?
```bat
scripts\markers.bat follow 0     :: 关掉 yaw_follow，观察更干净（本轮已设）
```

### 修掉的两�?看起来像功能没实�?的守�?bug（上一轮）

| Bug | 后果 |
|---|---|
| 安装守卫 `if (!g_enabled && !g_shift_enabled)` 漏了 `g_head_enabled` | 只开头动开关时**钩子根本不装** |
| 外层守卫 `if (g_enabled \|\| g_shift_enabled)` 同样漏了�?| 钩子装了，但整个头动块被**跳过** |

另外把分类器一个脆弱的启发式换掉了：原来用"平移�?�?50 世界单位"区分视图矩阵�?投影矩阵，相机靠近原点就失效（harness 合成矩阵平移只有 7.76，被判成 `PROJ(rigid)`）�?现在�?**两种读法都正交归一 �?刚体变换**"，这是可靠的数学判据�?
> **教训（本次最贵的一条）**：`pick = -2` 的三�?bug 加起来的效果�?> "**功能完全没生效，日志里却一切正�?*"（钩子装了、开关读了、分类器认对了）�?> 凡是"改引擎参�?的实验，必须有一条日志能证明**写入真的发生�?*�?> 等价地：`chosen` 这类字段要能被判读，不能恒真也不能恒假�?

## 重影的根因：每眼非对�?FOV�?026-09-23 解决�?
**现象**：每只眼单独看都正常，双眼一起看 `CAPCOM` 变成 `CAPCAPCOM`；横向平移能对齐一部分�?但永远差一点；歪头会变�?`XX`；缩�?旋转也修不掉�?
**根因**：运行时的每�?FOV �?*非对�?*的（本机 VD �?FOV 设为 100 时：
左眼 `angleLeft=-54, angleRight=+40`，右�?`-40, +54` —�?因为每片镜片在面板上偏心）�?合成器做重投�?畸变�?*假定画面是用它声明的那个视锥渲染�?*，而本项目的画面是�?**面板自身矩形**的视锥渲染的。于是它对不匹配的内容做了重投影，且两眼�?不匹�?*镜像不同** �?同一画面在两眼里被掰开约一个眼距�?
**修法**：两眼提�?*同一个对称视�?*（取两眼 FOV 的并集）。不对称消失，重影随之消失�?实测结果：两个十字完全重合，**无论怎么歪头都不再重�?*�?
**为什么之前十几轮都白�?*：横向／纵向／缩放／旋转都是**平移与线性变�?*�?而错位是合成�?*按每只眼分别施加**�?—�?平移在数学上不可能抵消它�?一个重要反证：把「两�?view 用同一�?pose 与同一�?FOV」的假图层放到最上层时，
画面�?*干净的单个十�?*（见 `re6vr_vd_dummy.txt` 实验）——那时两眼的 FOV 相同�?
| 开�?| 默认 | 说明 |
|---|---|---|
| （无�?| **共享对称 FOV 已默认开�?* | 这是修复本身 |
| `re6vr_asym_fov.txt` | 存在即关闭修�?| 回到运行时的每眼非对�?FOV（会重现重影），仅供对照 |

**保留的诊断工�?*（现在都不再需要，但对以后有用）：
`re6vr_uv_shift.txt = keys` 按键微调（←→↑�?位置、`[` `]` 缩放、`Q` `E` 反向旋转、`R`/`Z` 复位），
面板底部两条读数条实时显示数值；`re6vr_test_solid.txt = cross` 红绿十字＋读数；
`= stripes` 条纹尺；`= grid` 双向网格�?

> **本节先前写的"所以重影只能来自提交之后（运行时侧�?已被下面的源码修正推翻一半�?*
> 保留在这里是为了记住推理错在哪：当时只证明了"我们画出来的两张眼图相同"�?> 就跳到了"提交之后的运行时"。实际源码里有两处把提交路径弄坏了，
> 完全能独立解释同样的现象，不需要假设运行时行为�?
### 证据（可复现�?
用户实测反馈�?
- **显示器上干净**，只有头显里有重�?�?游戏输出与抓帧链路无关�?- **单眼看干净，两眼之间有左右错位** �?问题只在两眼之间，不在单眼内部�?
据此做了三组**离线实测**（都不需要头显）�?
| 检�?| 方法 | 结果 |
|---|---|---|
| 提交给运行时的两张眼睛图 | `re6vr_eye_left.bgra` vs `_right.bgra` | **字节完全相同** |
| 我们渲染的双眼视�?| 新增 `RE6VR_SELFTEST_STEREO=1`（两眼位姿差 126 mm），�?`scripts\eye_shift.py` 互相�?| **0 px，残�?0.00** |
| 游戏帧本身是否被画两�?| �?150/200/250/300 导入 + `eye_ghost.py` | 干净，无局部极�?|

**为什么视差必然是 0（这是设计使然，不是 bug�?*：`draw_quad` 的视锥是�?**每只眼各�?*看到的面板矩形算的（`tan_l/tan_r/tan_d/tan_u` 来自 `view_panel`），
所以面板在两眼图像里都被映射到**�?NDC** —�?两张图逐像素相同。验证只需看日志：
`frustum eye0` �?`frustum eye1` 的角度完全一致�?
### 源码里查出的两处错误�?026-09-22 晚，已修�?
**错误 1 �?�?swapchain 模式�?RTV 索引不匹配（这才�?两眼错位"的直接来源）**

创建时按 `rtv[eye * image_count + image]` 排列，�?`draw_quad` 一律用
`rtv[image * view_count + eye]` 取。两个布局�?`two_swapchains` 下完全不同：

```
创建布局            �?draw_quad 取到�?eye0 img0 = 0        eye0 img0 -> 0   一�?eye0 img1 = 1        eye0 img1 -> 2   错（那是 eye1 �?img0�?eye1 img0 = 2        eye1 img0 -> 1   错（那是 eye0 �?img1�?eye1 img1 = 3        eye1 img1 -> 3   一�?```

后果正是"每只眼单独看都对、合起来错位"：一只眼写进了属于另一�?swapchain
image �?RTV。已�?`sc_images_shared` 分开索引�?
**错误 2 �?`xrEndFrame` 传的�?`instance` 而不�?`session`（三处）**

`xrEndFrame(XrSession, const XrFrameEndInfo*)`。传 `instance` �?loader 的句�?类型校验就会拒掉（`XR_SESSION_MAGIC_NUMBER != XR_INSTANCE_MAGIC_NUMBER`），
返回 `XR_ERROR_HANDLE_INVALID`，根本到不了 runtime —�?**每帧都提交失�?*�?日志证据：上一轮真机跑�?900 帧，�?*从未打印�?"600 frames submitted"**
（源�?600 帧一报），�?"1 frames submitted" 一定打过，说明
`frames_submitted_` 卡在 1 再没增长�?
**教训**：`frames_submitted_` 只在成功分支里自增，而失败计数只在第一�?`xrWaitFrame` 失败时才打印 —�?所�?每帧都在失败"在日志里几乎是隐形的�?以后凡是"画面不对"的排查，先确认这一行在涨�?
### 下一步：单变�?A/B（正在做�?
同时改其他任何东西都会让结论不可读，所以本�?*�?*保留�?
```
re6vr_two_swapchains.txt = 1
re6vr_ipd_scale.txt      = 0
re6vr_uv_shift.txt       = 0 0      （把上轮加的补偿全部关掉�?```

判读只看两件事：

- **A. CAPCAPCOM 消失** �?问题就集中在 array swapchain / `imageArrayIndex` /
  slice 路径，双 swapchain 是可用布局�?- **B. 仍然存在** �?按用户列的顺序继续：`pose` / `fov` / `imageRect` /
  左右�?swapchain 是否真的分别提交 / 运行时是否横向偏�?/
  绘制时的 pose·FOV 与提交时�?pose·FOV 是否存在"双重视角偏移"�?
每种布局都会打印一�?*提交审计**（新增，见下），A/B 可以直接读日�?而不是靠猜�?
### 备选手段（当前**关闭**）：每眼水平补偿（`re6vr_uv_shift.txt`�?
上一轮基�?错位一定来自运行时"的判断加的开关。既然上面两处提交路径的�?已经能独立解释现象，**本轮把它设成 `0 0` 完全关掉**，避免它污染 A/B 判读�?
如果 A/B 两个结果都需要它，它的数学是干净�?—�?纯视空间平移，所需偏移量与
视距无关�?
```
uv_shift = 眼睛相对中点的横向偏�?/ 面板宽度      （图像自身单位）
```

标记文件 `re6vr_uv_shift.txt`（放在日志目录，见下）：

| 内容 | 含义 |
|---|---|
| `auto` | 由眼距自动算，正�?|
| `auto-` | 同上，反号（若第一版方向反了就用这个） |
| `<�? <�?` | 手调，单位是"面板半角"的百分比，例�?`-0.7 0.7` |
| `0 0` | 关闭（本轮就是这个） |

另一个诊断档：把 `re6vr_ipd_scale.txt` 设为 **9**（≥1.5 的档位），此时两�?**提交的位姿也相同**。用来单独判�?错位是否由位姿驱�?�?
### 标记文件不再需要提�?
游戏目录属于 Steam，普通用户写不进去，以前每个开关都要提权复制一次。现�?**代理旁边一�?`re6vr_logdir.txt`**（内容是目录路径）就能把日志�?*所�?*标记
文件搬到那个目录�?
```bat
scripts\markers.bat            :: 显示当前所有开�?scripts\markers.bat auto       :: 打开每眼补偿
scripts\markers.bat ipd 1      :: 两眼真实眼距
scripts\markers.bat reset      :: 全部清掉
```

标记文件目录当前�?`C:\re6vr\_work`（`_work\re6vr_logdir.txt` 的内容）�?
## 三条命令

```bat
scripts\build.bat      编译 32 �?d3d9.dll
scripts\deploy.bat     部署到游戏目录（需要写权限，工作区外要提权�?scripts\uninstall.bat  卸载
```

**启动必须�?Steam**（`BH6.exe` 直接跑会 `Failed to initialize Steam.` 然后退出）�?
## 屏幕"大小调不�?的根因（2026-09-23 晚定位并修复�?
**现象**：`re6vr_screen_dist.txt` / `re6vr_screen_width.txt` 改了，日志也读了，但头显�?屏幕**一点没�?*。这不是参数没生�?—�?是生效了却没有任何路径能影响观感�?
**根因（一句话�?*：画面的 NDC 铺满它自己的视锥，而提交给 runtime 的视�?**正是面板自己那个矩形**，两端严丝合�?�?无论屏幕多大、多远，compositor 收到�?都是"这块画面正好覆盖你视野这么一�?，映射到显示屏的**永远是同一块区�?*�?`screen_dist` / `screen_width` 只改了面板在世界里的位置和渲染分辨率，观感被
完全抵消�?
**证据**（`_work\re6vr.log`，同一局的两�?`place_panel`）：

```
frustum eye0: submitting 45.4 x 26.5 deg (mode 0), panel 4.60 x 2.59 m at 5.50 m
submit: eye0 fov angleLeft=-54.00 angleRight=54.00 angleUp=44.00 angleDown=-55.00 deg
quad check eye1 corner0..3 ndc=(-1..+1)                       <- 画面正好铺满整块 slice
```

提交的视锥只�?`panel_w/panel_h` 与世界位置有关，�?屏幕该占显示屏多�?毫无关系�?提交角度 *就是* 屏幕的观感大小，而它从来没被这两个参数算进去过�?
**修法**：把**提交**的视锥与面板自己的矩形解耦。`draw_quad` 照旧把画面铺满整�?slice（不动），只�?这块 slice 代表多少角度"按系�?`k` 缩小�?
```c
k = (4.0 / screen_dist) * (panel_w / 4.6) * RE6VR_SCREEN_SCALE
proj_views[i].fov.angle* /= k      // 只缩放提交，不缩放绘�?```

- 缩放绘制是没用的：面板被摆成刚好铺满自己的视锥，任何 `view*proj` 的整体缩放都�?  在画面里被抵消掉 —�?这也是当�?以为改了大小"却看不出差别的原因�?- `k = 1`（默认几�?4.0 m / 4.6 m）时屏幕**铺满显示�?*；`k = 1.38` 时占 73% 宽�?- 现在 `dist` �?`width` **一�?*决定观感大小，像真的一块屏：距离翻�?�?宽度减半�?  距离和宽度同时翻�?�?大小不变。这是有物理含义的，不是随手凑的�?- 新增 `re6vr_screen_scale.txt`（`markers.bat scale 1.5`）：距离宽度都不动，单纯�?  屏幕放大/缩小，用来在头显里定你喜欢的尺寸�?- 日志新增一行，**这就�?大小真的传到显示屏了�?的证�?*�?.5 m 下的实算值，�?  下面 Python 复算一致）�?
```
apparent: screen 5.50 m away, 4.60 m wide -> k=0.73, submitting 124.3 x 90.9 deg
          of the display's 108.0 x 73.7 deg = 138% of its width (scale 1.00)
```

`138%` 读作"提交的锥比显示屏本身还宽"，屏幕因此只占显示屏宽度�?`1/0.73 = 73%`
（多出来的角度落在视野外）。摘要里的百分比就是 `100/k`，它随标记变化即修复生效�?�?OpenXR 的映射规则，提交�?`tan` 与显示屏�?`tan` 是一一对应的，所以这个比例是
**几何结论**，不依赖 runtime 额外做什么�?
**代价（真实存在，别当�?bug�?*：提交区域变�?= 同样一�?slice 分到更少的显示屏
像素，屏幕越小越软（`k = 2` 时线性分辨率约减半）。想保持锐利就往**�?*调（slice 变成
被缩小而不是被放大），或者把 `re6vr_swapchain.txt` 设成 `2492/k` 左右�?
**天花�?*：`k` 不能小于 1 —�?即屏幕最�?= 铺满显示屏。再"�?就是放大像素（`k = 0.5`
等于�?slice 放大�?2 倍宽），只会更糊而不会更清楚。所以想更小就加�?`dist`�?想更大只能接受变糊；**基准 4.0 m / 4.6 m 就是那个"最大且清晰"的点**�?
**验证用的实算�?*（同一段算术在 `python` 里复算过，改标记后日志应逐一对上）：

| dist | width | scale | k | 提交�?| 屏幕占显示屏�?|
|---|---|---|---|---|---|
| 4.0 | 4.6 | 1.00 | 1.00 | 108.0° | **100%**（铺满） |
| 5.5 | 4.6 | 1.00 | 0.73 | 124.3° | **73%** |
| 8.0 | 4.6 | 1.00 | 0.50 | 140.1° | **50%** |
| 4.0 | 9.2 | 1.00 | 2.00 | 69.1° | **200%**（超出显示屏�?|

**三行命令的头显验�?*（标记只在进程启动时读一次，所以每次都要重启游戏）�?
```bat
scripts\markers.bat                        :: 全默�?4.0/4.6 -> 屏幕铺满显示屏（k=1, 100%�?scripts\markers.bat dist 5.5               :: 再启�?-> 屏幕缩到 73% 宽（k=0.73�?scripts\markers.bat dist 8                 :: 再启�?-> 再缩�?50% 宽（k=0.50�?```

最直接的判据：**后两次启动之间，屏幕宽度应肉眼可见地变小**�?3% �?50%）�?
### 同一处第二个 bug：提交区域的比例�?026-09-23 深夜，已修）

**测出来的**：眼�?dump（`_work\re6vr_eye_left.bgra`�?492×2684）里画面**铺满整块
slice**——按 NDC 构造必然如此。所�?比例错没�?只可能错在提交给 runtime 的那个矩形上�?对着日志一看就露馅�?*提交�?tan-aspect �?1.778�?6:9），�?union �?108 × 99 deg
（tan-aspect 1.150�?*�?
**后果**：compositor 把这块区域按声明铺到显示屏上，于�?*水平被拉长约 1.15 �?*
�?6:9 的画面放�?tan-aspect 1.150 的区域），圆看起来是横向的椭圆�?这不�?观感偏好"，是几何错误�?
**修法**：union 的四个角里，**水平必须保持最�?*（它就是消除重影的那一刀，见
`shared_fov`），所以收窄的�?*垂直**：以眼睛的垂直中心为基准，把垂直跨度改成
`水平跨度 / (16/9)`�?
```c
cy     = (tanUp + tanDown) / 2;                     // 保留眼睛自己的垂直中�?half_v = (tanRight - tanLeft) / 2 / (16/9);
angleUp = atan(cy + half_v);  angleDown = atan(cy - half_v);
```

实测这台头显：`108.0 × 99.0`（比�?1.150）→ **`108.0 × 73.7`（比�?1.778�?*�?垂直中心保持�?�?3.0°�?
> 顺带修掉一�?证据行本身算�?�?bug：`apparent:` 里的显示屏尺寸原来写�?> `atan(r - l)`，把 108° 打成 62.1°，现在改成与其他地方一致的 `atan(r) - atan(l)`�?> **一行用来当证据的日志自己算错，比没有这行更�?*——这一轮就是靠它才发现比例问题的�?
**修复后的实机观测�?026-09-23 22:52 那一局，用户确�?左右能看到更多内容了"�?*�?
```
union: eye0 raw L-0.9425 R0.9425 U0.7679 D-0.9599 | eye1 raw L-0.6980 R0.6980 ... 
       -> union L-0.9425 R0.9425 U0.7679 D-0.9599 (86.6 x 81.4 deg, tan-aspect 1.091)
openxr: eye 0 runtime FOV 78.2 deg wide (43.3 left 34.9 right), 81.4 deg tall (37.5 up 43.8 down)
```

- **修之�?*�?6:9 的画面被塞进 `tan-aspect 1.091` 的区�?�?左右各裁掉约 7%
  （`tan43.3 / tan43.96 = 0.928`）�?*用户报的"左右能看到更多内�?正是这部分回来了�?*
- **修之�?*（本版）：reshape 不再多套 `tanf`，提交区域变�?  `86.6 x 51.5 deg`（`tan-aspect` = `1.885 / 1.0604 = 1.7778` = 16:9），
  画面不再被拉伸也不再被裁�?- 垂直中心保持�?�?3° 附近（runtime �?`angleUp/Down` 本身不对称：+37.5 / �?3.8）�?
> ⚠️ **runtime 报的 FOV 每局会变**�?2:39 那局每眼 `±54°`（union 108 × 99），22:45 起是
> `43.3/34.9, 37.5/�?3.8`（union 86.6 × 81.4）。`xrLocateViews` 是只读的�?*这不是我�?> 改的**，是 Virtual Desktop 侧的 FOV 设置。所以每局都要按当次的 `runtime FOV` 行读
> `apparent:`，别拿上一局的数字比�?
> ⚠️ **runtime 报的 FOV 会变**：更早那一局�?2:39）同一�?runtime 报的�?> 每眼 `±54° / +44° / �?5°`（union 108 × 99），22:45 报的�?`43.3/34.9, 37.5/�?3.8`
> （union 78.2 × 81.4）�?*这不是我们改�?*（`xrLocateViews` 是只读的），�?> Virtual Desktop / runtime 侧的 FOV 设置变了。所�?*每局�?`apparent:` 行都要按当次�?> `runtime FOV` 行来�?*，别拿上一局的数字比�?
### �?已解释：`tanf()` 把切空间当成角度�?026-09-23 深夜，已修）

上一节记�?三个数对不上"，根因是**`XrFovf` 的字段是正切值，不是角度**（OpenXR 规范�?换算就是 `angle = atan(tangent)`，本文件其他读法都套�?`atanf`，只�?reshape 那一�?先套了一�?`tanf`）：

```c
// 错：字段已经�?tan，又套一�?tanf，水平被静默改变
const float tan_l = tanf(f.angleLeft),  tan_r = tanf(f.angleRight);
// 对：
const float tan_l = f.angleLeft,        tan_r = f.angleRight;
```

�?32 位浮点逐位复算（`_work\stage_check.cpp`，可 `cl /O2` 直接跑）确认�?
```
raw union    L-0.9425 R+0.9425 U+0.7679 D-0.9599 -> 86.6 x 81.3 deg  tan-aspect 1.0910
OLD reshape（多套一�?tanf�?                      -> 86.6 x 64.7 deg  tan-aspect 1.4662
NEW reshape（直接用切值）                          -> 86.6 x 51.5 deg  tan-aspect 1.9452
日志里的 reshape �?U+0.4974 D-0.7881 = OLD 那一行（逐位吻合�?```

也就是说�?*上一�?修比�?的代码比例仍然是错的**�?.4662，不�?16:9），只是�?108×99 换成�?86.6×64.7。现在这一段改成直接用切值，16:9 需�?`half_v` 满足
`half_h / half_v = 1.7778`�?
> **教训（值得记）**：`atanf(tanf(x))` 看着像恒等，实际不是；它让一个错误的水平�?> 看起来合理，正是它把这个问题藏了两轮�?*混合单位的日志比没有日志更坏** —�?> 上一版那行同时打印了"除以 k 的�?�?套了两层三角的�?，所以三个数字互相矛盾却
> 各自�?像对�?。现�?`apparent:` 只打印同一单位（角度）�?*三段**�?> `raw union -> reshaped to 16:9 -> submitting`，最后一段必须是
> `raw × (1/k)`，第三段�?tan-aspect 必须�?1.778�?
## �?新路线（已实现，待一局验证）：代理 `DINPUT8.dll`，让**右摇�?*替我们转�?
**为什么这条路不一�?*：前面三条路线都在找"相机"这个东西，而它们全部失败的原因已经清楚
——相机对象不可达（DTI 是残留代码）或不在常量流里�?*右摇杆不需要找任何东西**：游戏自�?有一套成熟的相机控制器（插值、限位、碰撞、脚本接管），我们只要让游戏以为玩家在推右摇杆�?
**证据**：`BH6.exe` 的导入表�?`DINPUT8.dll` **只导入一个函�?*�?
```
=== DINPUT8.dll
   DirectInput8Create
```

（同时确认：**没有 XInput**，`USER32` 只有 `GetCursorPos`/`GetAsyncKeyState`，说明手柄走
DirectInput，�?Steam 输入若开启会在更外层拦掉。）

**已实现的机制**（两�?DLL，同进程，命名共享内存通信）：

```
d3d9.dll（桥�?    每帧�?头部相对起始姿势的偏航角"写进命名映射 re6vr_head_pose
dinput8.dll（代理）hook IDirectInput8::CreateDevice �?打补�?IDirectInputDevice8 �?                   vtable �?9/10（GetDeviceState / GetDeviceData），
                   在返回前把头部偏航换算成摇杆增量加到指定轴上
```

- 新文件：`src/dinput8_proxy.cpp` + `src/dinput8.def` + `scripts/build_dinput8.bat`
- 桥侧新增：`openxr_bridge.cpp` 里发�?`re6vr_head_pose`（发�?*相对起始姿势**的偏航，
  因为绝对偏航在这台头显上不是 0，直接喂会把画面永久转过去）
- 开关（`markers.bat`）：`di off|on`、`di axis <偏移>`、`di gain <g>`、`di scale <s>`
- 部署：`deploy.bat` 现在会把 `dinput8.dll` 一起装进游戏目录（缺了也不影响游戏�?
**第一局的目的是量，不是注入**（注入默�?**off**）：代理会每秒打印一次每个轴的原始值：

```
di: state[120] lX=-24 lY=13 lZ=0 lRx=0 lRy=0 lRz=0 rglSlider0=0 rglSlider1=0 rgdwPOV0=-1
```

**推一次右摇杆**，看哪个字段跟着�?—�?那就是要注入的轴（`lX`/`lZ`/`lRz`�?见上面顺序：
`0=lX 4=lY 8=lZ 12=lRx 16=lRy 20=lRz`）。然�?`markers.bat di axis <那个偏移>` +
`markers.bat di on` 再跑一局，画面就应该跟着头转�?
> ⚠️ 若日志里出现 `THE GAME USES BUFFERED MODE`，说明游戏读的是 `GetDeviceData`
> 的缓冲事件而不是轮询状态，注入要改成改�?`DIDEVICEOBJECTDATA.dwOfs` 条目 —�?> 代理已经把这条路径也 hook 上并会明确报告，不用猜�?
### �?第一次部署就把游戏打崩了（已定位、已修）：代�?*递归调用自己** �?栈溢�?
**现象**：装�?`dinput8.dll` 后游戏进不去。Windows 事件日志给出了确切死因：

```
Faulting application name: BH6.exe
Faulting module name: DINPUT8.dll, version: 0.0.0.0, time stamp: 0x6ab3ef16
Exception code: 0xc00000fd      <- STACK OVERFLOW
Fault offset: 0x000010d5
```

**根因**：`DirectInput8Create` 里要调用"真正�?dinput8"，代码用

```c
GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, MAX_PATH, L"\\dinput8.dll");
HMODULE m = LoadLibraryExW(sys, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
g_real_create = GetProcAddress(m, "DirectInput8Create");   // �?拿到的是我们自己的导�?```

**Windows 的模块是按文件名（basename）唯一�?*：进程里已经有叫 `dinput8.dll` 的模�?（就是代理自己）时，`LoadLibraryExW` �?System32 全路径的请求也会**返回那个已加载的模块**�?于是 `GetProcAddress` �?*我们自己�?`DirectInput8Create`** 交回来，函数调用自己直到栈耗尽�?**全路径不足以保证拿到系统 DLL —�?必须校验解析结果�?*

**修法（三重）**�?
1. **路径校验**：把返回模块�?`GetModuleFileNameW` 与自身路径比较，相同就拒绝；
2. **指针校验**：解析出的函数地址若落�?*本模块映像范围内**（读 PE `SizeOfImage` 判断），
   拒绝调用�?3. **重入护栏**：`DirectInput8Create` �?`InterlockedIncrement` 计数，重入立刻返回失败并
   打印一�?`RE-ENTERED`�?*�?栈溢出、崩溃报告只�?DINPUT8.dll"这种不可诊断的死�?   变成一条日�?*�?
修完的实测日志（`build\di_harness.exe`，不需要游戏）�?
```
di: load_system_dinput8: real DirectInput8 at C:\WINDOWS\system32\dinput8.dll
di: DirectInput8Create: real function 7CD76FA0 from the system dinput8.dll
di: DirectInput8Create ok: IDirectInput8 vtable patched (CreateDevice was 7CD74B20)
di: CreateDevice(keyboard): vtable patched (GetDeviceState was 7CD7DE80, GetDeviceData 7CD7DCD0)
```

**流程上也改了**：`deploy.bat` 现在**默认不装** `dinput8.dll`（`deploy.bat di` 才装），
第一次那个版本用嵌套 `if/else` 写，cmd.exe 直接�?`else was unexpected` �?*静默跳过删除
步骤** —�?已重写成只用前向 `goto`。崩溃后急救只需：`scripts\deploy.bat`�?
### �?二次修复�?026-09-23 深夜）：�?REFramework 的同型故障修，并补齐全部导出

**外部证据（一手源码）**：praydog/REFramework �?`src/Main.cpp` 注释记录�?*完全相同**的崩溃：

> *"overlays like EOS hook DirectInput8Create in the system DLL, and their hook calls back into
> our export, causing **infinite recursion / stack overflow**"* —�?修法�?> `static thread_local bool s_in_call` 重入标志，重入时直接调用真实函数�?
这与本机现象完全吻合：崩溃发生在 `DirectInput8Create` 返回**之后** 1�? 秒（Steam overlay /
Steam Input 也挂这个导出），而我原来�?路径校验 + 指针校验"挡不住—�?*递归不是从我的入�?进去的，是别人的钩子回调进我的导�?*。旁证：[dxwrapper#476](https://github.com/elishacloud/dxwrapper/issues/476)
标题�?"Dinput8Wrapper breaks Steam Input"，[TiltedHooks PR#5](https://github.com/tileds?/TiltedHooks/pull/5)
标题�?"Prevent other programs and DLLs from detouring DirectInput8Create"�?
**本次三处修改**�?
1. **重入护栏**（照 REFramework）：`static thread_local bool s_in_call`，嵌套进入时
   **直接调用真实函数**并立即返回，不碰 vtable、不写日志—�?*�?栈溢出、崩溃报告只说是
   DINPUT8.dll"变成一条日�?*�?2. **�?DLL 改名 + 运行时解�?*：真 DLL 部署�?`dinput8_orig.dll`（唯一 basename，消�?   按名解析咬到自己的前提），代理从�?`GetProcAddress` 取真函数，并校验返回指针**不在本模�?   映像�?*�?3. **补齐全部导出**：同名代理必须替�?DLL 回答**整张导出�?*，不能只回答游戏导入的那一�?   ——进程里别的加载器（Steam overlay 就是）会 `GetProcAddress` 其余名字。真 DLL �?6 �?   导出，现�?1 个是自己的包装函数�?*5 个用 .def 转发**�?
   ```
   ordinal hint RVA      name
         1    0 00001D10 DirectInput8Create
         2    1          DllCanUnloadNow     (forwarded to dinput8_orig.DllCanUnloadNow)
         3    2          DllGetClassObject   (forwarded to dinput8_orig.DllGetClassObject)
         4    3          DllRegisterServer   (forwarded to dinput8_orig.DllRegisterServer)
         5    4          DllUnregisterServer (forwarded to dinput8_orig.DllUnregisterServer)
         6    5          GetdfDIJoystick     (forwarded to dinput8_orig.GetdfDIJoystick)
   ```

**离线证据**（`build\di_harness.exe`，不需要游戏）�?
```
di: resolve_real_create: real DirectInput8Create at 79696FA0 from dinput8_orig.dll
di: DirectInput8Create ok: IDirectInput8 vtable patched (CreateDevice was 796C4B20)
di: CreateDevice(keyboard): vtable patched (GetDeviceState was 796CDE80, GetDeviceData 796CDCD0)
```

> **两次"转发"都是错的，都记在这里**：① 链接�?forwarder
> `/export:DirectInput8Create=dinput8_orig...` 会让导出**直接指向�?DLL**，我的包装函�?> 根本不执行（harness 抓到：`DirectInput8Create` 成功但没有任�?vtable 补丁日志）；
> �?用生成的 import lib �?*静态导�?*被链接器整个丢掉（dumpbin 只列�?KERNEL32），
> 首次调用就会拿到空指针。现在走"运行时从改名 DLL 解析"�?
### 🔎 2026-09-24：外部调研推翻了"相机对象路线已死"的结论（判据已重写）

**外部证据（一手源码，MT Framework 同族 32 �?DX9 项目 `muhopensores/dmc4_hook`�?*�?
1. **MT Framework 的相机对象里没有视图矩阵�?* `uCamera` �?**look-at 三元�?*�?   `mCameraPos +0x30`、`mCameraUp +0x40`、`mTargetPos +0x50`，外�?`mFov +0x24`�?   `mNearPlane +0x1C`、`mFarPlane +0x18`、`mAspect +0x20`�?   权威的视图矩阵在渲染器的 `sCamera::ViewPort` 里（`mViewMat +0xB0`、`mProjMat +0xF0`�?   `mpCamera +0x04`，结构步�?`0x590`），而且**通过指针链访�?*，不是稳定直址�?   �?**这解释了本项�?5 局零命�?：我一直在找矩阵，而那个对象里根本没有矩阵�?*
2. **Vireio Perception 源码更正�?改顶点常�?路线的能力边�?*：着色器注入只能�?   立体分离、头部位置、roll�?*yaw/pitch 在代码里不存�?*（`UpdatePosition()` 只用 yaw/pitch
   旋转位移向量，输出是纯平移矩阵）。它�?yaw/pitch �?*鼠标模拟**�?**VRBoost 直接写游�?   内存地址**。→ **yaw/pitch 必须在相机对象层或输入层交付，不可能在着色器层�?*
3. **XInput 路线�?RE6 不适用**：本项目 `scripts\imports_of.py xinput` 实测 **RE6 不导�?   XInput**（`DINPUT8.dll` 只导�?`DirectInput8Create`）。调研方也已**撤回**"dmc4_hook hook
   �?XInputGetState"的说法（该仓库里没有 XInput 代码，`m_XInput` 只是引擎的一�?bool 字段）�?
**判据变更（本次代码改动）**：`mem_scan.cpp` 新增 **viewport 签名搜索**，在原来�?"找捕获矩�?位置"之前先跑，并�?*在同一�?pass 里试所有可能的布局**（一次运行覆盖多种假设，
而不是每局赌一种）�?
```
view 形状�?4x4（仿射：末行 (tx,ty,tz,1)，基向量非零�?projection 形状�?4x4（末行平移为 0、m[15]=0、m[11]�? 的透视项）
两者相距正�?0x40�?*顺序两种都试**（D3D 行向量约定可能让两块前后互换�?view 的偏移试 0xB0 / 0xA0 / 0xC0 / 0x90 / 0xD0 / 0x80 / 0xE0 / 0x100 / 0x70 / 0x110
```

**为什么必须试多种**：RE6 自己的反射字符串�?*�?`mProjMat` / `mPrevViewMat` / `mFrustum`**�?而同�?DMC4 有——说明结构至少有一处不同。只�?`+0xB0/+0xF0` 赌一次，就是把上一轮的
"单一假设"错误再犯一遍。日志会打印命中时的**实际偏移**，那个偏移本身就是答案的一半�?
**这个签名的价值在于不需要知道任何地址、也不需要捕获�?*—�?两个形状不同�?4x4 恰好相距
0x40"几乎不可能偶然成立�?
### �?同日：两条静态路线已排除，别再花时间

1. **DTI 对象�?RE6 里没有静态引�?*（`scripts/dti_walk.py` 实测）：类名字符串在 `.data` �?   **没有任何 dword 等于 (名字地址 �?4)**（MtDti �?`name` 位于 +0x04），�?   `uCamera`/`sCamera`/`uFreeCamera` �?*都没�?MtDti 对象指着它们**；每个名字唯一的引用就�?   前面找到�?注册�?`push 字符串`"。→ **"DTI 对象已存在于二进制中、可从活对象走到"这个
   说法�?RE6 不成�?*（对 MHW 成立，但那是 MT Framework 2.0 �?64 位版本，表是运行时构造的）�?2. **本机没有包索引访�?*（`pip download capstone` 超时 180 s），装不�?Capstone/Ghidra�?   因此**不再用手写反汇编器猜代码形状**——本轮它已经让我错判两次（漂移、模式猜错）�?   需要真反汇编时，这是一个明确的、需要用户侧网络的前提�?
> **XInput 路线�?RE6 关闭**（已实测：`scripts\imports_of.py xinput` 无输出，RE6 只从
> `DINPUT8.dll` 导入 `DirectInput8Create`）。调研方最后又更正�?dmc4_hook 确实 hook �?> XInputGetState"（`src/XInputHook.cpp`，在 `src/` 根而不�?`src/sdk/`）——但**那对 RE6 无用**�?> 因为 RE6 根本不加�?XInput�?
## �?2026-09-24：真反汇编器建成（把静态分析这条路打开�?
上一节把「真反汇编器」列�?*需要你侧网�?*才能做的事—�?*这个前提是错�?*。本机没有网络，
�?*本机�?Visual Studio 2022 Build Tools，里面有 `ml.exe`（真汇编器）�?`dumpbin.exe`
（真反汇编器�?*。真值不需要从网上拿�?
### 工具（`scripts\disasm_lib\` + `scripts\re6dis.py`�?
| 文件 | 作用 |
|---|---|
| `disasm_lib/pe.py` | PE32/PE32+ 加载器：段映射、导入、导出、TLS、重定位 |
| `disasm_lib/x86.py` | **完整�?x86-32 解码�?*（不是长度解码器）：�?opcode 表、ModRM/SIB、全部前缀、x87、MMX/SSE/SSE2/SSE3�?F 38 / 0F 3A |
| `disasm_lib/analyze.py` | 递归下降分析：函数边界、调用图、交叉引用、字符串归属 |
| `re6dis.py` | 命令行：`disasm` / `func` / `xref` / `str` / `stats` |
| `analyze.bat` | 重建分析缓存（约 2 分钟�?|
| `tests/run_tests.py` | **解码器真值测�?*（ml.exe + dumpbin 往返） |
| `tests/make_probe_fixture.py` | 生成 1063 �?opcode 探针，逐条�?dumpbin 问答�?|

```
python re6dis.py disasm 0x4F8D50 30     反汇编（�?RVA�?-va 则给虚拟地址�?python re6dis.py func   0x4F8D50        整个函数 + 调用�?被调用�?python re6dis.py xref   0x17C3164       谁引用了这个地址（数�?dword + 代码�?python re6dis.py str    uCamera         找字符串
python re6dis.py stats                  分析概览
```
> 地址一律是**虚拟地址**（镜像基址 `0x400000`）；`disasm` 之类默认�?RVA 要加 `--va`
> 才是虚拟地址—�?*输出里第一列永远是虚拟地址**�?
### 为什么这个解码器可以信（判据可复现）

1. **真值来�?ml.exe + dumpbin**：`tests/fixture.asm` �?297 条手写指令经 MASM 汇编�?   dumpbin 反汇编，逐条比对**长度和文�?*；dumpbin 印出它消耗的字节数，所以两边都不是循环论证�?2. **1063 �?opcode 探针**：对每个 opcode 用固�?ModRM 编码一条探针，�?dumpbin 报出助记符，
   结果写回 `.asm` 的注释里（见 `tests/probe_*.asm`）。这张表�?*数据**，不是记忆�?   **当前�? 处不一�?*�?90 �?*明确的、有理由�?*不覆盖（全部�?VMX/SGX/MPX/2013+ 指令�?   �?32 位老游戏里不可能出现）�? 处记录在案的双方分歧（都印在测试输出里）�?3. **规则：不在表里的 opcode 报错，绝不猜�?* 这正是旧 `x86_disasm.py` 的病根—�?   它把未知 opcode 当指令印出来，于�?看起来是反汇�?�?4. **全镜像实�?*：BH6.exe 17 MB `.text`，递归下降得到 **1 873 873 条指令�?3 722 个函数�?   0 个无法解码的地址**。旧文档�?手写反汇编器让我错判两次"的风险不存在了�?
### 由此推翻/修正的旧结论

| 旧结�?| 新判�?| 更正 |
|---|---|---|
| 「hook DTI 注册点：该页�?xref �?不可达（开发残留）�?| `0x17C3164` �?`.text` 里有 **4 处读取点**（`0x4F8D71`、`0x4FA561`、`0x4FF990`、`0x504A01`�? `.rdata` �?1 �?| �?**旧判据错�?*。DTI 注册点本身确实没有直�?`call`（`0x144DA40` 零引用、`0x144DA70` 只有 `.rdata` 一�?dword），**但它写进去的那个 `.data` 地址被真实游戏代码读**�?|
| 「`uCamera` 等名字在 `.data` 里无任何 dword = 名字地址�? �?RE6 无静�?MtDti�?| 该等式依然成立：`[0x17C3164+4]` 读不出来，因�?**`0x17C3164` �?`.data` �?BSS 尾部（vsize > rawsize），磁盘上根本没有字�?* | �?**结论成立，但原因不同**：不�?没有 DTI"，而是**DTI 对象在运行时才建�?*。旧脚本�?BSS 会读�?*别的段的数据**（`rva_to_off` 当时不检�?rawsize），所�?没有 dword 指向名字"这个观测本身也不可靠�?|
| 「`sCamera::ViewportCamera` / `mProjMat` 等字符串缺失�?| 字符�?*都在**：`sBioCamera`@`0x151A9DC`、`sBioCamera::ViewportCamera`@`0x151A9E8`，还�?`uCameraBase`、`uCameraCtrl`、`uCameraLERP`、`uCameraQuake`、`uCameraFovQuake`、`uCameraAnimation`、`uCameraCallback`�?| �?类名锚点�?*有的**，只是它们由注册代码**在运行时**写进 BSS�?|

### 已经拿到手的具体线索（下一步从这里接）

`0x17C3164` 是一�?*固定的静态地址**，游戏启动时往里写 DTI 对象，之后真实代码反复读它：

```
008F8D50  56                    push esi
008F8D51  8bf1                  mov  esi, ecx
008F8D53  8b4e18                mov  ecx, [esi+0x18]        ; �?指针来自 DTI 对象
008F8D56  8b01                  mov  eax, [ecx]             ; vtable
008F8D58  f30f1081f4410000      movss xmm0, [ecx+0x41F4]    ; 一�?float（相机相关？�?008F8D60  f30f5905b8055101      mulss xmm0, ds:[0x15105B8]  ; 乘一个全局常量
008F8D68  8b9094020000          mov  edx, [eax+0x294]       ; vtable �?0x294/4
008F8D74  ffd2                  call edx                    ; 虚调用，参数是那�?float
008F8D79  8d89b0030000          lea  ecx, [ecx+0x3B0]
008F8D7F  e8ecf36a00            call 0x00FA8170
```
另有 `0x4FA550` 附近一�?*�?3D 数学**：对 `[eax+0xF0]`/`[eax+0xF4]`/`[eax+0xF8]` 三个 float
做差、平方、求和、和一个全局阈�?`ucomiss` 比较—�?*这是"点到相机的距�?角度"形状的代�?*�?`+0xF0..+0xF8` 恰好�?DMC4 `sCamera::ViewPort` �?position 的位置�?
**下一步（不需要人操作 GUI，纯静态）**�?1. �?`0x17C3164` 记为「bioCamera DTI 静态地址」，�?`re6dis.py func 0x4F8D50` �?4 个读取点
   所在函数各自读一遍，找出 `[esi+0x18]` 的来源（谁把 DTI 对象塞进那个结构）；
2. �?`re6dis.py xref 0x151A334`（注册时写进 DTI �?vtable）反�?`sBioCamera` �?*实例构造点**�?3. 找到实例后用 `xref` �?`+0xF0/+0x294/+0x41F4` 这些偏移追写入者—�?   **能写就能�?*，这才是 Stage 3 需要的那一个写入点�?
### ⚠️ 分析器已知的不足（别被数字骗了）

- **函数数量偏高**：指针表里的条目会指向函�?*内部**地址，于是同一个函数体被记成好几个
  "函数"。`stats` �?`functions` 因此虚高（当�?53 722，其中标�?`alias_of` 的只�?  一部分），`no caller` 同理�?*判据**：`func` 输出�?`blocks` 数和 `instructions` 明显
  不匹配、或几个相邻地址给出同一段代码时，就是这种情况�?- **覆盖�?41.6%**�?7.8 MB �?7.4 MB）不代表"解码器有�?：`0 个无法解码的地址`才是解码器的
  指标；未覆盖的字节是**没有被任何代码根到达**的区域（内嵌数据、间接跳转目标、未被识别的
  函数）。要往上提就得继续加根（更多表、导�?thunk、`.rdata` 里其它形状的指针表）�?- 这两条都�?*已知且有判据**的，不影�?`disasm` / `xref` / `str` 的结论：
  它们是逐条/逐地址查询，不依赖函数边界�?
### 仍然正确的旧结论（不要重测）

- 顶点着色器常量�?*给不�?yaw/pitch**（Vireio 源码已证），改矩阵路线继续作废；
- 捕获�?view 矩阵**不在内存停留**（两姿势精确匹配 0 命中）——这条是运行时测量，与本轮无关；
- 屏幕大小/距离/比例、每�?FOV、提交链路：全部正常，别动�?
## �?2026-09-24 第二轮：RE6 的类地图拿到了（3372 个类 + 相机全家桶）

上一节说「静态分析不再是瓶颈」，这一节把静态分�?*做到�?*：把 RE6 �?*反射系统整个解出�?*�?结论�?*RE6 有静态的类锚点，而且�?DMC4 的还�?*�?
### 一句话结论

RE6 �?MT Framework �?`MtDti` 反射�?*3372 个类**，每个都在二进制里以固定指令序列登记�?**类名 + 静态地址 + 实例大小 + 父类**全都能静态读出来。相机是其中一�?20 个类的家族，
根是 `uCameraBase`，管理类�?`uCameraCtrl`�?x4B70 字节）�?
### 判据（都可复现）

| 事实 | 怎么读出来的 |
|---|---|
| 3372 个类 | 每条登记都编译成固定序列：`push <大小> / push <父类DTI> / push <类名字符�? / mov ecx,<静态地址> / call 0xE67920`。扫 `B9 <imm32> E8 <rel32�?xE67920>` 就全部枚举出�?|
| 对象布局 | `+00` vtable、`+04` 类名指针、`+14` 0、`+18` 打包�?size\|hash、`+1C` 父类指针（`0xE67920` �?`mov [esi+4], eax` 等可直接读出�?|
| 解析函数 | `0xE67C50` = `hash = ([obj+0x18] >> 0x17) & 0x3F; return table[hash]`（表�?`0x1814338`�?4 桶）�?*7678 �?*调用�?|
| 工厂 | `push <DTI> / call 0xE67C50 / mov edx,[eax] / mov edx,[edx+0x1C] / push <ctx> / push 0x10 / push <size> / call edx`。`[edx+0x1C]` 就是工厂函数�?*3324 个类**有这种调用点 |
| 相机建立 | 关卡加载器（就是格式�?`"soft\stage\s%04d"` 的那个函数，�?`0x4DD440`）分�?`0x4B70` 字节、调 `uCameraCtrl` 构�?`0x60A730`、把指针存到 `[stage对象+0x640]` |
| 共有 3 处构�?| `0x4DD4BC`（关卡加载器）、`0x60C9DA`、`0x60CC4B`（两个工厂包装） |

### 相机类地图（`python -m disasm_lib.camera`�?
| �?| MtDti | 实例大小 | 工厂 context 全局 |
|---|---|---|---|
| `uCameraBase` | `0x017D1DD0` | 0x1B0 | `0x017D1DEC` |
| **`uCameraCtrl`** | `0x017D26F0` | **0x4B70** | **`0x017D270C`** |
| `uCameraQFPS` | `0x017D2A00` | 0x1B80 | `0x017D2A1C` |
| `uCameraAnimation` | `0x017D1D44` | 0x1400 | `0x017D1D60` |
| `sCamera`（渲染器相机�?| `0x0186E260` | 0xCE0 | `0x0186E27C` |
| **`sBioCamera`** | `0x017C3164` | **0x1620** | `0x017C3180` |

`uCameraBase` 底下还有 19 个子类（`uCameraRail` / `uCameraTurret` / `uCameraVeh` /
`uCameraNavi` / `uCameraQuake` / `uCameraFovQuake` / `uCameraHideBox` / `uCameraWep00/01` …）�?**`sBioCamera` 的父类是 `sCamera`**（渲染器相机）。用 `python -m disasm_lib.dti --tree uCameraBase`
看全树�?
### 命令

```
python -m disasm_lib.dti camera                列出相机类（名字/地址/大小/父类�?python -m disasm_lib.dti --tree uCameraBase    打印相机继承�?python -m disasm_lib.managers uCamera          列出按标识查找的类及�?context 全局
python -m disasm_lib.camera                    一份带"哪些是实测、哪些是假设"的相机地�?python re6dis.py dti <子串>                    同上，走统一 CLI
python re6dis.py manager <子串>
```

### ⚠️ 明确**没有**验证的东西（别当结论用）

1. **`[0x017D270C]` 是不�?`uCameraCtrl` 实例，没有证实�?* 它只是工厂调用读�?context 参数�?   �?`[DTI+0x1C]` 这个字段是登记时写入�?*类名 hash**。`.text` 里提到这个地址�?8 �?*全是�?*�?   没有一处用绝对地址写实例指针（写会走计算地址）�?*一次内存读就能定论**�?   `[0x017D270C]` 若指向一�?0x4B70 字节、头�?vtable 属于 `uCameraCtrl` 的对象，则成立�?   在那之前它是**带地址的假�?*�?2. **相机实例内部的字段偏移没验证�?* DMC4 那套（`mCameraPos +0x30`、`mCameraUp +0x40`�?   `mTargetPos +0x50`、`mFov +0x24`）是**待验证假�?*，BH6 没对过。别直接往这些偏移写�?   > �?**2026-09-24 第三轮已解决**（本节保留原文供追溯）：BH6 **自己的属性注册表**给出�?   > 真实偏移 —�?`sBioCamera` �?`mCameraOrg[i]`（步�?0x40）`cameraPos +0xE30` /
   > `targetPos +0xE40` / `cameraUp +0xE50` / `fov +0xE60`，见
   > [「相机字段偏移已经静态核实」](#-2026-09-24-第三轮相机字段偏移已经静态核实不再依�?dmc4-的猜�?�?   > 注意那条 DMC4 表里�?`+0x30/+0x40/+0x50` **�?BH6 上并不成�?*，别再引用它�?3. **view 矩阵在哪仍然未知�?* 旧结�?捕获的矩阵不停留"如果成立，说明矩阵是**每帧�?   pos/up/target 现算**的——那么钩子应该在**算矩阵的那个函数**上，而那个函数还没找到�?
### 下一步应该做什么（按性价比排序）

1. **一次内存读**定论 `[0x017D270C]`（读 4 字节 + 看头 4 字节是不�?vtable）。这一步不需要反汇编�?   也不需要改游戏代码：现�?mod 已经有读内存的能力�?2. 顺着 `[stage对象+0x640]` 那条路（`0x17CF454` 是关卡代码满地用的全局）做同样的定论�?3. 定论之后，找**每帧�?pos/up/target 变成矩阵的函�?*：`uCameraBase` 家族�?0x1B0 字节�?   矩阵 0x40 字节，用"�?pos/up、写 4x4"的形状去 `.text` 里筛，候选不会多�?4. 只在�?3 步失败时才回到输入注入（B 计划）�?
## �?2026-09-24 第三轮：**相机字段偏移已经静态核�?*（不再依�?DMC4 的猜测）

上一节留下的�?2 条不确定性是「相机实例内部的字段偏移没验证，DMC4 那套（`mCameraPos +0x30`�?`mCameraUp +0x40`…）别直接往这些偏移写」�?*这条现在有答案了，而且不是�?DMC4 借的**�?BH6 自己的代码里就写着每个字段的偏移�?
### 一句话结论

RE6 的每个类都用**属性注册模�?*声明自己的字段，模板编译成固定指令形状，
**字段名、类型、字节偏移全都静态可�?*。把整个镜像过一遍：**437 个属性表�?608 条字�?*�?其中 **404 个类**连表带类名一起定下来了。相机这边最要紧的一条：

```
sBioCamera                dti=0x017C3164  size=0x1620  ctor=0x004FA570  (59 fields)
    mCameraOrg[0].cameraPos      Vector3  +0xE30
    mCameraOrg[0].targetPos      Vector3  +0xE40
    mCameraOrg[0].cameraUp       Vector3  +0xE50
    mCameraOrg[0].fov            float    +0xE60
    mCameraOrg[0].nearPlane      float    +0xE64
    mCameraOrg[0].farPlane       float    +0xE68
    ... 步长 0x40，i = 0..7�?8 �?= 8 �?× 6 字段�?```

**这份表和 README 上面那份"手工解析 DTI �?得到的表逐项一�?*（`+0xE30/+0xE40/+0xE50`�?步长 `0x40`、`fov/nearPlane/farPlane`），但这次是**程序从指令流里读出来�?*�?并且�?14 个别的字段在同一张表里互证（`mViewportCamera +0x12A0`、`mCameraParam +0xD80`�?`mScreenType +0xCE0`、`mScreenPlNo +0xCF0`、`mPartnerCamFlg +0xDE4`、`mDispCtrlFlag +0x15A0`�?`mWipe +0x15B0`、`CameraPatch_TransFov/ReadyFov/ReadyOffsetZ +0x1610/+0x1614/+0x1618`
都是 README 里已经点过名的那些）�?*所�?偏移是猜�?这条警告可以撤掉�?* —�?写入点的偏移现在有据可依�?
### 判据（都可复现）

模板的字节顺序是这样�?*注意 `lea` 在名字之�?*�?
```
lea  edx, [ebx+0xE30]              ; 8D /r mod=10：字段地址 �?栈上局�?mov  ecx, edi                      ; 属性表
mov  dword [esp+0x10], <名字VA>    ; C7 44 24 10 <imm32>
mov  dword [esp+0x14], 0x14        ; 类型�?x14 = Vector3�?xC = float
mov  dword [esp+0x18], ebx         ; 对象基址
mov  dword [esp+0x1C], edx         ; 字段地址
...
call 0x00E6E210                    ; 把这条属性追加进�?```

两个坑，都踩过：

1. **配对方向**：字�?i �?`lea` 出现在字�?i-1 的名字之后（模板两半分居 `mov ecx, edi`
   两侧）。按"名字往前找最近的 `lea`"配对�?*整张表会错位一�?*�?   正确做法是把同一函数里的 `lea` 与名字各收成一条流�?*按地址顺序配对**�?2. **不能按字节扫**：`text.find(b"\xc7\x44\x24\x10")` 在整�?`.text` 上有 **9374 处命�?*�?   绝大多数�?*别的指令的操作数字节**（立即数/位移里恰好出现这四个字节）�?   只在递归下降已经到达的指令边界上解码（`analyze.load_analysis` �?`Function.blocks`�?   才能把这一类假命中全部去掉�?
**类名归属**不靠猜字段名前缀，走两条有据可查的路，`props` 会把用的是哪条打出来�?
- **类自己的 vtable**（主要路径，404 个类里的绝大多数）：MTDti getter 本身�?vtable 槽位
  （`mov eax,<DTI>; ret`）。`sBioCamera` �?vtable `0x0151A380` 一�?run 里就�?  `004FA560`（getter）、`004FA570`（构造函数，注册 59 个字段）、`004FC610`（析构）�?  run 里有多个候选时�?*字段�?*取最多的那个，并把候选全打出来（便于发现选错）�?- **�?DTI 的那个函�?*（后备）：类记录由构造函数写进对象头；`mov reg,<DTI>` 的所在函�?  顺着 tail call 追下去（BH6 �?getter 常编译成 `mov eax,<DTI>` + `jmp <真身>`�?  例如 `0060AA60 �?009768F0`）�?
### 新增工具

```
python re6dis.py props <类名子串>     打印该类的字段表（名�?类型/偏移 + 归属证据�?python re6dis.py field <字段名子�?   反查"哪个类有这个字段、偏移多�?
python -m disasm_lib.propmap [子串]   �?props，独立入口；--limit 控制打印多少张表
```

配套改动：`analyze.load_analysis(cache, exe)` 现在真的能把分析缓存**读回�?Analysis**
（原来是�?`return None` 的占位），`analyze.instruction_at` / `iter_instructions`
提供"只在真实指令边界上迭�?的接�?—�?上面�?2 个坑就是靠它避免的�?
### ⚠️ 本轮**没有**解决的问题（别当结论用）

1. **`[0x017D270C]` 仍然只是"DTI getter 用的地址"**，不是实例指针。一个类**�?*属性表
   不代表它**�?*实例，更不代表那个全局里放着实例。这条依然要一次内存读来定论�?2. **不是所有类都恢复了偏移**�?04/3372。其余类要么属性表在递归下降没到达的代码�?   （覆盖率 41.6%，见上文），要么本来就不注册字段�?*没恢复的类不要用这张表推偏移�?*
3. **`uCameraCtrl`�?x4B70 字节，相机管理器）没有属性表**：即使追�?getter 的真身也没有�?   所�?管理器里哪个字段是当前相�?这件事，静态这条路给不出答案�?4. **字段偏移 �?写入�?*。知�?`targetPos` �?`+0xE40` 只说�?写这里引擎是认的"�?   仍然需要找�?*每帧写它的函�?*（或者确认引擎自己每帧覆盖，那就�?hook 读取点）�?
### 追加�?*�?`mCameraOrg` 的那个函数找到了**（`0x004FF9B0`，sBioCamera vtable �?10�?
有了字段偏移，就能反过来�?谁写这些偏移"。全镜像�?�?`+0xE30/+0xE40/+0xE50/+0xE60/
+0xE64/+0xE68` 为位移的存储指令"�?*只有一个函数六个字段全�?*�?
```
fn 0x004FF9B0 .. 0x0050055F   463 条指�?  sBioCamera vtable 0x0151A380 的槽 10
   0x004FFA91  movss [esi+0xE30], xmm0     ; mCameraOrg[0].cameraPos
   0x004FFADF  movss [esi+0xE40], xmm0     ; mCameraOrg[0].targetPos
   0x004FFB9F  fstp  [esi+0xE50]           ; mCameraOrg[0].cameraUp
   0x004FFBDD  movss [esi+0xE60]           ; fov
   0x004FFBED  movss [esi+0xE64]           ; nearPlane
   0x004FFC05  movss [esi+0xE68]           ; farPlane
```

而且�?*不是随手写几个字段，是整表搬�?*：`mCameraOrg[i]`（i=0..7，步�?`0x40`�?�?**120 个字段（8×15 �?float）全�?*由同一�?`esi` 基址写一遍，源是同对象上
**偏移 `+0x1030` 起的并行一�?*（`+0x1030 �?+0xE30`、`+0x1040 �?+0xE40`�?`+0x1050 �?+0xE50`，逐项对应、逐项�?`0x1200`）：

```
mCameraOrg[i]  =  +0xE30 + 0x40*i     （cameraPos/targetPos/cameraUp/fov/near/far�?源组           =  +0x1030 + 0x40*i     （同样的 6 个字段，每个 4 float 一步）
```

**这条链把"参数从哪来、到哪去"接上�?*�?
```
uCameraCtrl�?x60C9F0 / 0x60CA60 / 0x60CAC7 / 0x60CB1E�?    └─ call 0x004F9950(entry_index, 源对�?      ; 源对�?+0x50..+0x78 �?sBioCamera +0xE30
                                                  ;（索�?ebx 直接�?[ebx*0x40 + 0xE30]，越界检�?cmp ebx,8�?sBioCamera::<slot10> 0x004FF9B0                  ; +0x1030 �?�?mCameraOrg[�?8 组]
```

**这为什么重�?*：头部跟踪要写的就是这条链上的参数，而现�?*三个候选写点都有确定的偏移**�?
| 写点 | 地址 | 说明 |
|---|---|---|
| 官方字段 | `[sBioCamera+0xE40]` | `targetPos`，属性表认证的偏移；引擎自己也会�?`0x4FF9B0` �?|
| 搬运�?| `[sBioCamera+0x1040]` | `0x4FF9B0` 的输入；改这里要赶在它跑之前 |
| 搬运函数 | `0x004FF9B0` | hook 它、在写入之后覆盖 = 每帧最后一次发言权（推荐�?|

> ⚠️ **仍然没有验证**（静态到此为止）：`0x4FF9B0` 是不�?*每帧**都跑�?> 每次跑之前引擎会不会**用自己的值重新覆�?* `+0x1030` 组、以及渲染时读的�?> `mCameraOrg` 还是别的副本。三条都只能靠一局实机的读�?写入审计回答 —�?> �?*"该写哪个偏移"这件事不再是猜测�?*�?
### 追加：`[0x017D270C]` �?8 处读取点已经列出来了（`xref` 修好之后�?
顺手修掉一个工�?bug：`re6dis.py xref` 从写出来那天起就�?*崩的**
（`cmd_xref` �?`args.max_functions`，�?xref 子命令没定义这个参数 �?每次 `AttributeError`�?所�?谁引用这个地址"这个问题一直是**问不出来**，而不�?没人引用"）。修好后�?
```
xref 0x017D270C  ->  8 处，全在 .text�?   0053607B  0053615A  005B24E5  005FD51C
   0060C9BC  0060CC2C  00CB23B9  00CC5B3B     （前 4 �?+ �?4 处各成一对）
```

值得注意的两点（**都还不是结论**）：

- 其中 `005FD51C` 就在 `uCameraBase` 构�?工厂区（`0x5FBDxx`–`0x5FDxxx`），
  `0060C9BC`/`0060CC2C` �?**`uCameraCtrl` 自己的函数里**（`0x60C9xx`–`0x60CCxx`�?  就是上面调用 `0x4F9950` 的那几个）→ �?这个全局�?*管理器自己的方法**里被当对象基址�?
  的形态一致；
- 但形态一�?*不等�?*它是实例指针（它同样可以只是被当作工厂的 context 参数读一次）�?  **判据还是�?4 个字�?*：见下面「下一步」的第一条（�?`[0x017D270C]` 与对象头）�?
## 🛑 2026-09-24 定论：RE6 �?找相机对�?这条路已穷尽（附证据�?
> ⚠️ **本节结论已被上面两节更正，保留原文仅供追�?*�?> 「DTI 不可达」「无类名锚点」两�?*不成�?*�?372 个类全都静态可查）�?> 「捕获矩阵不停留�?*仍成�?*，且它现在指向正确的问题：要找的�?*算矩阵的函数**，不是字段�?> 本节其余内容（顶点常量层作废、屏幕参数正常）继续有效�?
七局实测 + 静态分析后，可以明确写下结论�?*每一条都带可复现的判据，不要再重复测量�?*

| 尝试 | 判据（可复现�?| 结果 |
|---|---|---|
| 旋转顶点常量里的 view 矩阵 | 写入审计确认写入发生 | �?画面不动；且 Vireio 源码证明该层**给不�?yaw/pitch** |
| 扫内存找捕获�?view 矩阵 | 两姿势交叉匹�?| �?0 命中 |
| 扫内存找相机世界坐标 `p = -Rᵀt` | 同上 | �?0 命中 |
| 扫内存找**捕获的平移行本身**（不推导、不猜形状） | 两姿势精确浮点匹�?| �?**0 命中 �?矩阵每帧重建后即丢弃，不在内存里** |
| viewport 形状签名（view 4x4 �?proj 4x4 相距 0x40�?| 1525 MB / 128 候选，**无一**的平移等于捕获�?| �?全是 `0`/`±1` 单位阵噪�?|
| hook DTI 注册�?| 该页�?xref（直�?相对/数据�?| �?不可达（开发残留）�?**已被 `0x17C3164` �?4 处读取点推翻** |
| �?DTI 对象按类名找实例 | `uCamera` 等名字在 `.data` �?*无任�?dword = 名字地址�?** | �?RE6 无静�?MtDti �?**观测本身�?BSS 读取 bug 影响，见上一�?* |
| 按类 vtable 找实�?| 全镜像只�?**6 �?RTTI 类型描述符，且无 camera 相关** | �?�?RTTI 可依 |
| dinput8 代理注入右摇�?| 两次栈溢出（已修：改名真 DLL + 重入护栏 + 补齐 6 个导出） | ⚠️ 未再�?|

**为什�?RE6 与同族不�?*：DMC4 那种"静态指�?+ 两级�?依赖 exe 里存�?*静态的相机管理�?指针**�?*注册好的 DTI**；RE6 的反射代码是开发残留（不可达）�?*类名�?`.data` 里没�?MtDti
指向**�?*全镜像只�?6 �?RTTI 描述�?*。所�?RE6 既没�?按名�?也没�?按类型找"的锚点，
而按值找又被"每帧重建"否决�?
**还剩下什么（都需要目前不具备的资源）**�?
1. **输入层注�?*——唯一不需要相机对象的路线，代码已写好且离线验证通过
   （`deploy.bat di` 安装），但要再冒一�?代理影响游戏启动"的风险；
2. **数据断点**（Cheat Engine / x64dbg�?什么改写了这个字段"）——调研里排最后但从不失手�?   需要人�?GUI 里操作；
3. **真反汇编�?*（Capstone / Ghidra headless）分析相机更新路径——本�?`pip` 无索引访问，
   需要你侧网络�?
## 屏幕设置�?026-09-22 重做�?
| 变量 | 默认 | 作用 |
|---|---|---|
| `RE6VR_SCREEN_DIST` | `4.0` | 玩家到屏幕的距离（米）�?*现在真的改变观感大小**（见上一节） |
| `RE6VR_SCREEN_WIDTH` | `4.6` | 屏幕宽度（米），高按 16:9 自动算�?*和距离一起决定观感大�?* |
| `RE6VR_SCREEN_SCALE` | `1.0` | 距离宽度不动，单纯缩放观感大小（`0.05`～`8`）。`markers.bat scale 1.5` |
| `RE6VR_IPD_SCALE` | `0` | `0`=两眼共用视点（消除重影），`1`=真实眼距 |
| `RE6VR_SCREEN_FOLLOW` | `1` | 头部偏转超过�?45° 时把屏幕重新锚定到正前方 |

**`dist` / `width` / `scale` 三者关�?*：观感大�?�?`scale × dist / width`（即显示屏宽度的
`1/k`）。`4.0 m / 4.6 m / 1.0` 是基准档 = 铺满显示屏；`5.5 m` 下是 73% 宽�?
> 别和**世界张角**搞混：世界张角只�?`width / dist` 决定（默�?59.8° × 35.8°），
> 它决定渲染分辨率；观感大小由提交的视锥决定（`apparent:` 那一行）。两者现在是分开的，
> 这正是本次修复的核心�?
默认值下屏幕**世界张角** 59.8° × 35.8°，`draw_quad` 每帧会打印实测�?（注意：**这一行是绘制用的视锥**，不是提交给 runtime �?—�?提交的那个看
`apparent:` 那一行）�?
```
frustum eye0: submitting 59.8 x 35.8 deg (mode 0), panel 4.60 x 2.59 m at 4.00 m,
              slice 998x2148 aspect 0.465, submitted tan-aspect 1.778
```

`RE6VR_FRUSTUM_ASPECT_MODE` 是诊断开关，保持 `0`：`0` = 视锥取面板矩形（提交区域
16:9，画面完整），`1` = 视锥取视口比例（画面只占视口中间一条，上下黑边）�?两种模式离线渲染出来的差别见 `_logs/cmp_mode0.png` / `cmp_mode1.png`�?
## 最重要的技术结论（读代码看不出来的那些�?
### 1. `IDirect3DSurface9` �?vtable �?`d3d9.h` 多一个槽 �?最关键

运行时对象在�?11 有一�?*不属于公开接口的保留槽**，后面全部后移一位：

```
d3d9.h 文档      实际运行�?GetDesc  = 11     Reserved11 = 11
LockRect = 12     GetDesc    = 12
UnlockRect = 13   LockRect   = 13
GetDC    = 14     UnlockRect = 14
                  GetDC      = 15
```

我按文档写槽位时，每一�?"LockRect" 实际调用的是 `GetDesc`，它往调用者的
`D3DLOCKED_RECT`�? 字节）里�?32 字节�?`D3DSURFACE_DESC` —�?踩掉调用者栈帧�?这就是那�?NULL `pBits`、`structured exception`、以�?跳到 0x16"的全部来源�?**修正已在 `src/d3d9_min.h` 落地**，用 `build\_copytest\copyprobe.exe` 逐槽位实测确认�?
### 2. MT Framework 不允许在帧内创建 D3D9 纹理

帧内 `CreateTexture` 会让引擎�?`ERR09 : Unsupported function.` 直接致命退出�?所有表面必须在**第一帧渲染之�?*建好�?
### 3. 后台缓冲不能直接作为读回�?
`StretchRect(backbuffer �?texture)` 返回 `D3DERR_INVALIDCALL`�?`LockRect(backbuffer)` 抛异常。可用路径是�?**同尺寸同格式�?`StretchRect` �?`GetRenderTargetData` �?`LockRect`**�?
### 4. 绝不能包�?`IDirect3D9Ex`

它比 `IDirect3D9` �?4 个槽。现�?Ex 工厂直接透传，只补丁设备**自己那份 vtable
副本**�?`Present` / `EndScene` / `Reset`�?
### 5. 会聚点必须在屏幕前方

`ipd_scale=0` 时若把会聚点放在屏幕平面上，面板视空间深度为 0，frustum 表达不了
�?全黑。会聚点必须在屏幕前方，�?*距离必须等于 `screen_dist`**（见�?29）�?
### 6. MinHook 已移�?
它的 trampoline 在第一�?`Present` 就崩（fault offset 0x4C89008B）�?改为直接补丁设备 vtable 副本�?
### 7. 眼睛 dump 的真实磁盘布局�?026-09-22 踩到�?
`dump_eye_image()` 把整�?swapchain 纹理 `CopyResource` �?staging，再
`Map(subresource(0, eye, 1))` 取一�?array slice，然�?*按纹理宽�?`d.Width`
写行、按 `d.Height` 迭代行数**。所以文件是�?
```
�?= sc_width（单眼宽，实�?998�?�?= sc_height x 2（两�?slice 上下堆叠，实�?2148*2 = 4296�?```

**不是左右并排�?1996×2148�?* �?1996 宽去读会把两�?slice 的行交错混在一起，
得到一�?看起来挺合理"的假�?—�?我据此错误推断了两轮，白绕很久�?正确工具：`scripts\eye_raw.py`（按真实布局切开）、`scripts\eye_cover.py`（量覆盖）�?
### 8. 视口比例与面板比例无关，且这不影响观�?
单眼 slice �?998×2148（比�?0.465，竖长条），源图�?16:9�?.778）�?缓冲区里画面�?竖直拉伸"�?3.8 倍，方格测试图能直接看出来�?�?*运行时是按提交的 `fov` 把图像矩形映射到显示�?*：提�?59.8°×35.8°（tan 比例
1.778）时，缓冲里 0.465 的比例在显示上就�?1.778，方格是方的�?所�?mode 0 下画面不变形、不裁切 —�?缓冲的像素比例只�?像素不是方的"，不是错误�?
## ⚠️ 血泪教�?
1. **眼睛 dump 的布局必须先确认再解读**（见结论 7）。布局猜错会让你在错误数据�?   建立一整套自洽的错理论�?2. **`place_panel()` 的调用点被脚本改写删掉过三次**，每次症状都是全黑。改完确认这行还在：
   ```cpp
   if (!panel_placed) place_panel(real_eye, fwd);
   ```
3. **不要�?字符串切�?批量改写 C++**。用行号精确替换，改完立刻读回来确认�?4. **渲染结果必须实测**，不能靠读代码推断。用离线的两个自检（见下）�?5. **"数学正确"�?结果正确"**。关键路径要打日志比对�?6. **PowerShell 写含中文的文件会破坏编码**。用 Python + `encoding="utf-8"` 写�?7. **别在缓冲区像素比例上做判�?*。缓冲区是竖长条不代表显示是竖长条；
   要看"提交�?fov 角度"�?面板的角尺寸"�?8. **测试里的硬编码采样点会随着可调参数失效**。经典自检�?面板外应该是�?�?   ±0.95 NDC 采样点，在视距变成可配置后就落到画面内容上了，于是总报 FAIL�?   现在探测点直接按源图 UV 取（象限中心），不再依赖面板多大�?9. **Steam 启动的游戏不继承 shell 的环境变量�?* 这是本项目所�?`RE6VR_*` 开关的
   共同陷阱，也�?Stage 2 第一次侦察白跑的原因：设�?`RE6VR_CAPTURE_VS=1` 再启动，
   日志里连"探针已安�?都没�?—�?�?*探针静默不安装看起来�?游戏里没有矩�?一模一�?*�?   现在关键开关支�?*标记文件**（放在游戏目录旁边），Steam 启动也能用�?   凡是"这个开关没生效"的怀疑，先确认进程里到底有没有读到它�?10. **告警/统计日志不能每帧打�?* 计时日志曾写�?计数器归零后也算一次报�?�?    结果 90 秒产�?8182 �?`readback` 日志�?.3 MB），把真正有用的信息全淹了�?    报告与归零必须绑在同一个条件上�?
## 已知遗留

- OpenXR 首次初始化会阻塞游戏线程一次（runtime 冷启动约 12 秒，已延�?1.2 秒）�?- **读回成本已实测（2026-09-22 真机）：18.17 ms/帧，�?51 fps�?* 日志行为
  `compositor: readback 18.17 ms/frame` / `copy thread ~51.6 frames/s`�?  这是整个管线里最贵的一环，也是 Stage 2 的性能预算基线：真双视角要把场景渲�?  两遍，必须在这个数字之上再算�?- **性能瓶颈在读回，不在眼睛缓冲�?* 每帧 3840×2160 要走
  `StretchRect �?GetRenderTargetData �?LockRect �?memcpy`�?.7 MB/帧出总线）；
  D3D11 那边只是"一�?Clear + 一�?quad"�?14 万像素在 4070 Ti 上约 1 GB/s 填充�?  相对总线传输可忽略�?*帧率有问题时先看这个，不要先缩眼睛缓冲�?*
  已加计时日志 `compositor: readback X ms/frame` / `copy thread ~X frames/s`�?  但需要合成激活（即需要头显），离线量不到，待真机�?- `RE6VR_SWAPCHAIN_W/H` 可把单眼 slice 从推荐的 998×2148 缩到�?545×800：按
  **运行时自身像素密�?*换算�?98×59.8/110�?148×35.8/96），画质不变、像素数降到 21%�?  **风险**：非推荐尺寸能否�?Virtual Desktop 接受尚未验证（需会话，无头显测不了）�?  不接受时 `xrCreateSwapchain` 会失败并打日志，去掉变量即可恢复�?  注意**不要**�?561 高——那会让竖直像素密度掉到与水平相同（�?16 px/度）�?  低于运行时对这个张角的建议密度，会糊�?- **Stage 2（真双视角立体渲染）已开始，卡在"取到相机矩阵"这一步�?* 见下�?  但矩阵探针现�?*确实能认出主相机**了（真机日志 `reg 4/6/27` = `VIEW`），
  所以限制不�?取矩�?，而在"改了之后画面没变�?——见文首 Stage 3 一节的pick 分析�?- **Stage 3（头部朝向注入）代码已完成并离线验收**，等一次真机确认�?  见文�?下一步：头部视角注入"�?
## Stage 2：真双视角立体渲染（进行中）

目标：把"一张平面画面贴到世界里"换成"两个真有视差的相�?，即每只眼看到略微不同的
几何�?*难点不在渲染，在于必须让 MT Framework 用它自己的相机渲染两�?*——引擎闭源，
矩阵不公开�?
**当前假设**：MT Framework 每帧通过 `SetVertexShaderConstantF` 上传视图/投影矩阵�?所以在这一层拦截即可拿到。若成立，第二个眼的做法是：拦截 `SetVertexShaderConstantF`�?把视图矩阵按 IPD 平移（视图矩阵是刚体变换，平移一行即可），让引擎把场景画两遍�?
**已验证的工具**：`src/matrix_probe.cpp`，用 `RE6VR_CAPTURE_VS=1` 开启。它挂在
设备 vtable �?`SetVertexShaderConstantF`（槽 94）上，对每批常量做分类：

| 判据 | 结论 |
|---|---|
| 3×3 部分正交归一（三行单位长、两两垂直）+ 第四行有平移 | `VIEW` |
| 第四列除透视项外�?0，第四行�?(0,0,1,0) | `PROJ` |
| 两者都不满足但 3×3 明显非正交、且带平�?| `VIEW*PROJ`（合并矩阵，很多引擎只传这个�?|

**分类器已用已知矩阵标定过**（不是猜的）�?
```bat
set RE6VR_CAPTURE_VS=1
build\harness.exe build\d3d9.dll 3 1280 720 vsmatrix
:: 期望日志：cap: reg 0 looks like VIEW (score 1.00)
::           cap: reg 8 looks like PROJ (score 1.00)
```

**第一次真机采集结果（2026-09-22 21:00，标题画面）**�?
```
cap: SetVertexShaderConstantF(reg 1, 4 vec4)     <- 投影矩阵，独立上�?cap: reg 1 = [1.3580 0 0 0][0 2.4142 0 0][0 0 -1.0001 -1.0000][0 0 -1.0001 0]
cap: SetVertexShaderConstantF(reg 0, 128 vec4)  <- 128 寄存器大块，视图矩阵在其�?cap: reg 5 = [1 0 0 42][0 1 0 -30][0 0 1 -100][1 0 0 0]
```

**结论**�?
1. **两个矩阵都在**，且都走着色器常量路径 —�?不需要改�?`SetTransform`�?2. `2.4142 = 1/tan(22.5°)`�?0° 透视）是投影矩阵的指纹�?3. **视图矩阵是列主序**：寄存器里每�?float4 是一**�?*，所以平�?   `(42, -30, -100)` 出现在寄存器 5/6/7 �?`.w` 分量上。按行读会看不出平移�?   （这也是最初把它误判成 `VIEW*PROJ` 的原因，分类器已改为同时接受两种布局。）
4. 每帧各上传一次；`reg 0,128` 那个大块里除了视图矩阵还有别的东西，
   需要靠原始寄存�?dump 定位边界（探针已加）�?5. **标题画面下视图矩阵完全不�?*（四次快照平移都�?`(42,-30,-100)`），
   所以第一批数据里拿不�?相机移动时矩阵怎么�?。探针已改为**相机平移超过
   0.05 m 就重新采�?*，进关卡走动即可拿到序列�?
**第二次真机采集结果（2026-09-22 21:06，关卡内走动�?*�?
寄存器布局**不是固定�?*，随着色器变化�?
```
VIEW 出现�?reg 1, 4, 6, 9, 27
PROJ 出现�?reg 1, 5, 13
```

视图矩阵采样（世界坐标，证明跟踪的是真相机）�?
```
frame  2412:   3990.270  -1185.540    626.589
frame  3056:  -3376.760   -882.441  -2757.520
frame  3595:  -3576.390   -617.601  -1941.830
frame  5918:   1286.120   -425.041   -417.585
```

**这决定了两件�?*�?
1. **不能靠固定寄存器号注�?*。必�?*按特征识�?*"这一批里哪个是视图矩�?
   （正交归一 3×3 + 第四行有平移）。固�?reg 号会同时漏掉大部�?draw 并破�?   该位置上的其他数据�?2. 投影矩阵紧跟在视图矩阵后�?4 个寄存器（view reg N，proj reg N+4），
   这是可用的结构性线索�?
配套工具 `scripts/matrix_map.py` 自动解析 `cap:` 日志、重建布局并列出相机轨迹，
不用手搓 grep�?
**安全实验第一次真机结果（2026-09-22 21:09）—�?Stage 2 最大的未知已解�?*�?
> 用户反馈�?*没有崩溃**，画面出�?*水平复制错位**（卡普空 logo 看起来像"卡卡普普空空"）�?
这直接回答了两个问题�?
1. **帧内改写视图矩阵是安全的** —�?引擎没有像帧�?`CreateTexture` 那样 `ERR09` 退出�?2. **画面按预期整体水平偏�?* —�?正是每眼偏移需要的行为，而不是只偏一部分
   （那只偏一部分才说明命中了副相机）�?
**但日志暴露了一个我自己�?bug**：游�?*复用同一个常量缓冲区**，导致原地改�?**累积**�?
```
shift: reg 4 translation 0.000 -> 0.030
shift: reg 4 translation 0.030 -> 0.060     <- 同一个缓冲区被改了第二次
```

所以请求的 3 cm 实际变成�?6 cm，而用户看到的"每个字母复制一�?正是这个
6 cm 错位造成的。已修：**每帧每个缓冲区只改一�?*（按指针去重），并加每帧
汇总日�?`N view matrix buffer(s) shifted`—�?*正确的运行应该恒�?1**�?数字变大就是这个 bug 复发�?
本地验证：`shift: frame 0: 1 view matrix buffer(s) shifted by 0.030 m (total 1 since start)`�?
**副作�?*：这次意外反而证明了偏移量是可控且可见的—�? cm 在标题画面视距下
产生的字母错位肉眼明显，说明**每眼 ±3 cm（IPD 的一半）的偏移量级是合适的**�?
**第二次安全实验（2026-09-22 21:18）—�?找到�?复制"的真�?*�?
用户反馈"依旧 cacapcom"（字母仍然成对复制）。日志揭示了两层原因，都不是崩溃�?
1. **每帧不止一个视图矩阵�?* 场里有主相机 + 阴影/反射等副相机，我�?*全部**
   都偏移了，于是同一物体被几个不同相机各画一�?�?复制�?2. **原地累加会叠加�?* 游戏每帧重建缓冲区但**每帧上传两次**视图矩阵�?   我按帧去重只挡住其一，于�?3 cm 变成 6 cm�?
**修法（两层）**�?
- 改写方式�?累加"改为**锚定**：记下引擎自己的值，每次�?  `锚定�?+ 偏移`。这�?*无论被调用多少次都不会叠�?*�?- 锚定**按视图矩阵索引分别保�?*：一帧里有多个矩阵时，共用一个锚点会被它�?  轮流覆盖，导致偏移时有时无�?- 新增**选择模式**（标记文件第二行）：`-1` = 全部，`-2` = 最后一个（推测是主相机），
  `0/1/…` = 指定第几个。默�?`-1` 保持与首次实验一致�?
**第三次安全实验（2026-09-22 21:22）—�?找到�?选错矩阵"的真�?*�?
用户反馈"�?（复制没消失）。日志显�?`pick=-2` 选中的东西是�?
```
shift: frame 316 view#0 reg 1 translation 1920.0000 -> 1920.0300
```

**1920 = 屏幕宽度**。`reg 1` 不是相机，而是**投影矩阵**——投影矩阵的 3×3 部分
也是正交归一的（只有 xy 缩放），所以我�?正交归一 + 有平�?判据把它当成了视图矩阵�?**真相机一次都没被碰过**，画面当然照旧�?
原始寄存器给出了铁证（帧 16 �?128 寄存器批）：

```
reg 1..4   = 投影矩阵，平移是 (1/3840, 1/2160) = (0.000260417, 0.000462963)
reg 5..8   = 视图矩阵，平�?(42, -30, -100)
reg 8..11  = 同一个视图矩阵的转置（引擎两种布局都传�?```

**教训**：投影矩阵的"平移"�?*屏幕尺寸的倒数**，是个普普通通的小数字，
任何"世界量级"过滤都挡不住。真正的判据是它配着**近单位的 3×3**�?
**修法**：判据识�?近单�?3×3 + 极小平移"�?归为投影；选择逻辑排除
`VIEW^T`（那是同一矩阵的转置副本，计入索引会让 `pick` 的含义随批次布局漂移）�?
**离线验证（`vsdecoy` 模式，不必跑游戏�?*：harness 上传"诱饵 + 真相�?同批�?复刻游戏布局�?
```
cap: reg 0 looks like VIEW^T      <- 诱饵
cap: reg 4 looks like VIEW        <- 真相�?cap: frame 0 view#0 (batch reg 0) translation 1920.0000 -30.0000 4430.0000  <-- SHIFTED
```

`pick=0` 现在选中的是真相机。配�?`scripts/pick_check.py` 可在**已有日志**�?复算选择逻辑，不用再启动游戏�?
**第四次安全实验（2026-09-22 21:29）—�?选中的是 UI 精灵，不是相�?*�?
用户反馈"依旧 capcapcom"。日志显示：

```
cap: frame 16 view#0 (batch reg 0) translation 0.0000 0.0000 1.0000   <-- SHIFTED
cap: frame 16 view#1 (batch reg 0) translation 0.0300 0.0000 1.0000
```

**平移 = `(0, 0, 1)`，而游戏的世界坐标在数千单�?*（实测相机平�?4375..4468）�?所�?`pick=0` 选中的是**平移到原点附近的 UI/精灵矩阵**，不是相机。而且
`view#1` �?`view#0` 只差我刚写进去的 0.03 —�?引擎会回读常量块，于�?*同一份数�?作为第二个候选再次出�?*，一个被改一个没改，这个 0.03 的错位就�?capcapcom"�?
**三次失败的共同根�?*：我�?正交归一 + 有平�?判据太宽，把它先后误判成
相机的东西依次是 —�?屏幕倒数型投影（平移 0.00026）、视口型矩阵（平�?1920）�?UI 精灵（平�?1）�?
**现在的判据（两条都必须满足）**�?
1. **平移是世界量�?*（≥50 �?�?e6）：相机在世界里，UI 在原点附近�?2. **3×3 是非平凡旋转**：相机会旋转世界，�?identity / 纯屏幕缩放不�?—�?   这一条才挡得住携�?`(1920, 1080)` 这类"平移"的矩阵（那是屏幕尺寸，不是位置）�?
**离线验证（`scripts/pick_check.py`�?*：改判据不再需要跑游戏。用真实游戏日志复算�?
```
frame 16-18:  reg4(PROJ/UI,1) reg6(PROJ/UI,1)   <- 平移=1 的伪影，已排�?frame 1517:   reg1(VIEW,4468)  translation (3267, -413.9, -3019)
frame 1875:   reg9(VIEW,4468)  translation (518.5, -1169, -4281)
```

**每一帧恰好一个真视图矩阵**，`pick=0` 因此是明确的�?
**第五次安全实验（2026-09-22 21:33）—�?判据终于选对了矩阵，但复制仍�?*�?
```
cap: frame 1500 view#0 (batch reg 27) translation 331.72 -286.09 -549.06
                                      rot0 -0.7597 -0.1263 -0.6379
```

旋转是真正的视角旋转、平移连续变�?—�?**这次选中的确实是相机**。但用户反馈
"还复�?�?
**新的怀疑（此前从未验证�?*：复制也�?*本来就在**，与我的平移无关。证据是日志�?**两个线程**�?6392 �?23000）都在处理同一个视图矩阵：

```
[t=16392] cap: frame 1500 view#1 (batch reg 27) translation 331.75 ...
[t=23000] shift: frame 1501 ... anchor 331.7167 + 0.030 m
```

两个线程各自提交视图矩阵，符�?**游戏自己每帧把场景画两遍**"（双眼渲染）�?若成立，我们的合成器只是把已经复制的画面照搬出去 —�?那样再调矩阵判据永远无效�?
**验证手段（新加）**：`re6vr_dump_at.txt`（内�?= 帧号）让合成器在指定帧把
**游戏原始�?*导出�?`re6vr_frame_at.bgra`。这是第一次能直接�?游戏交给我们�?画面里有没有复制"，而不是从头显的拷贝去反推。已部署�?1500（标题画面）�?
**注意**：帧导出需�?OpenXR 会话（合成器启动才有帧），所以无头显时测不了�?
**教训**：前四轮都在�?哪个矩阵是相�?，却从未验证�?复制是不是我的平移造成�?�?这个前提一旦不成立，四轮实验全部无效�?
**第六次实验（2026-09-22 21:38）—�?基线确认：复制与�?Mod 无关**�?
�?*完全不改写任何矩�?*的配置下，用户反�?还复�?。这是决定性结论：

> **复制是游戏自己产生的，我们的合成器只是照搬�?* 前四轮调"哪个矩阵是相�?
> 方向完全错了�?
**同一轮里发现的两个问�?*�?
1. **我的日志在撒谎�?* `pick = -1` �?`chosen` 恒为真，而日志把 `chosen` 当作
   "已改�?来标注，于是**基线运行看起来像在改引擎**。已修：只有真正写入
   （`chosen && 实验开启`）才�?`<-- WRITTEN`。日志必须可信到能区�?基线"�?实验"�?2. **两个线程各自上传略微不同的视图矩�?*�?
   ```
   frame 1543 reg 27 translation 331.30 -284.51 -550.14
   frame 1544 reg 27 translation 331.47 -284.23 -550.18
   ```

   同一帧内两套相机，符�?**游戏自己把场景渲染两�?*"。若它们画进同一个后台缓冲，
   画面自带幽灵 —�?这就是复制的机制�?
**帧导出的价�?*：`re6vr_dump_at.txt` 现在接受**一串帧�?*（一次运行抓多个画面�?因为标题画面只出现很短一段，单帧号猜一次要跑一趟）。帧 1500 已确认是自动保存
提示、文字干净 —�?但那不是好样本（UI 提示可能不参与双份渲染）。需�?*标题画面**
那一帧：`re6vr_frame_at_<帧号>.bgra`�?
**注意**：帧导出需�?OpenXR 会话，无头显时测不了�?
**第七次实验（2026-09-22 21:41）—�?复制不在游戏帧里，也不在眼睛图里**�?
�?`re6vr_dump_at.txt` 一次导�?8 帧，找到标题画面（帧 1400/1600/2200）并�?`scripts/ghost_find.py` 正式检测（�?远离零点、且明显高于自身邻域"的局部极大，
而不是像早期版本那样把平滑衰减当成复制）�?
```
game frame 1400/1600/2200 : NO duplication detected
left eye image (1996x2148) : NO duplication detected
两眼逐字节相同（ipd_scale=0 下应当如此）
```

**因此提交给头显的东西是干净�?*：游戏帧干净、左眼图干净、两眼一致�?既然画面里没有复制，复制只能来自**提交之后的环�?*（运行时/头显侧）�?
**当前最强嫌疑：换链比例与提�?FOV �?1.91 �?*

| | 宽高�?|
|---|---|
| 换链（每眼） | 1996 × 2148 �?**0.93** |
| 提交�?FOV | 59.8° × 35.8° �?**1.778** |

这条以前被我�?运行时按 FOV 映射图像矩形，所以缓冲像素不是方的也没关�?
**解释掉了，从没实�?*。若 Virtual Desktop 的畸变网格是按换链尺寸生成的�?这个失配就可能让画面竖直压缩、横向重�?—�?正是复制的样子�?
**测试**：`re6vr_swapchain.txt`（内�?`<�? <�?`，高�?0 = 自动匹配 16:9）现�?可用标记文件设置，因�?*环境变量�?Steam 启动无效**。已设为 `1996 0` �?1996×1123�?若复制消失，则确认是换链比例问题；若依旧，则问题在运行时侧，需要换验证路径�?
**第八次实验（2026-09-22 21:45）—�?上一轮测试是坏的，且"只渲染一只眼"的怀疑被排除**�?
**1. 换链比例测试完全无效（我�?bug�?*

```
openxr: swapchain override from re6vr_swapchain.txt: 1996 x 0 (height 0 = match 16:9)
openxr: swapchain size override -> 1996x64
```

`1996x0`（高 0 = 自动�?16:9）本意是 1996×1123，但**"解析派生�?被放在了"下限钳位"之后**�?高度先被钳成 64，于是造出一个高 64 像素的换链。日志看起来还挺正常�?已修：派生值必须在钳位之前算�?*教训：派生与钳位/校验的顺序要有意识，不能顺手写�?*

**2. 两只眼都在正常工�?*

```
slice0 sha256 : 2d9ae626...
slice1 sha256 : 44897d3b...
slice0 == slice1: False     差异 6,360,699 字节
```

两个 array slice 内容不同 �?两眼各写了内容，不存�?只渲染一只眼、复制两�?�?（两�?dump **文件**曾经哈希相同，那是上一轮的坏换链造成的，不能作为"两眼相同"的证据。）

**3. 眼睛 dump 的正确布局再确�?*：文件大�?17149632 同时等于 998×4296×4 �?1996×2148×4�?*光看字节数无法分�?*。按 998×4296 读会得到**斜切错位**的画�?（文字被截断、logo 撕裂）；�?1996×2148 读则是完整正确的标题画面�?所以正确布局�?**1996 �?× 2148 高，两个 slice 垂直堆叠**（即纹理�?1996）�?`WriteFile(..., d.Width * 4, ...)` 里的 `d.Width` �?1996 —�?这与日志一致�?
### 9. 眼睛图是"更多"像素，但画面却更�?—�?分辨率被丢在了别处（2026-09-22 查官方文档后找到�?
**官方结论**（[Vulkan Documentation Project, Stereo Viewport & Scissor Management](https://vulkan.lunarg.com/doc/view/latest/windows/antora/tutorial/latest/OpenXR_Vulkan_Spatial_Computing/04_Dynamic_Rendering/03_stereo_viewport_scissor.html)）：

> The runtime knows the physical curvature of the lenses. It provides a viewport that is
> typically **larger** than the physical display panel resolution (**often 1.4x**) to
> account for the detail lost during the "barrel distortion" warping process.

也就是说推荐的每眼分辨率**本身已含�?1.4 倍过采样**，不是浪费�?
**真正的糊来自分辨率链路，�?FOV 无关**�?
```
游戏后台缓冲 3840x2160
   �?StretchRect 降采�?staging 1280x720          <-- 这里是硬编码�?1280（丢�?89% 像素�?   �?再放�?眼睛纹理 ~1996 �?```

`create_shared_surface()` �?`const uint32_t work_w = w > 1280 ? 1280 : w;` —�?4K 画面�?压到 720p，合成器再把 720p **放大**进眼睛纹理。这条链路里丢掉的像素永远回不来�?
**修法**：staging 尺寸默认跟随眼睛纹理宽度（不再固�?1280），且不超过源分辨率�?可用 `re6vr_capture_w.txt`（或 `RE6VR_CAPTURE_W`）覆盖，日志会打�?`capture staging WxH ... (N% of the source pixels)`�?
**教训**：一直盯着 FOV 和视锥比例找"�?，而糊�?*分辨�?*问题�?先量链路每一级的分辨率，再谈几何�?
**第九次实验（2026-09-22 21:53）—�?两眼分离度：数据上是 0**�?
```
converge eye0: player (-0.03 -1.31 0.00) -> eye (-0.03 -1.31 0.00)
converge eye1: player ( 0.03 -1.31 0.00) -> eye (-0.03 -1.31 0.00)   <- 收敛到同一�?```

两眼�?±0.03 �?*不同**实际位置收敛�?*完全相同**的相机位置，两张眼睛�?MD5 一�?（`89066db99a4d4c92`）�?*从我们的输出看，两眼分离度为 0，不存在"隔得太远"�?*

若头显里仍有水平错开，它只能来自**提交之后的环�?*——运行时的位姿重投影�?运行时按每眼的预测位姿对图层重投影，而两眼位姿天生相差一�?IPD�?
**对照实验**：`re6vr_ipd_scale.txt`�? = 两眼共用视点�? = 真实眼距）现在可用标记文�?设置（环境变量对 Steam 启动无效）。测 0 �?0.25 �?1.0 就能判定看到的错开量级是否就是
正常立体视差�?
**顺带记下一个观�?*：日志里 `frustum` 行的面板距离会在 4.00 m �?3.11 m 之间�?（`screen (-0.09 -0.01 -4.01)`），因为 `yaw_follow` 在头部转动后重新锚定屏幕�?而重新锚定的距离基准与首次放置不同。这不是 bug 但会�?屏幕多大"随动作变化，
值得后续统一�?
**第十次实验（2026-09-22 22:0x）—�?重影的量级指向运行时位姿重投�?*�?
用户描述：错开�?*一个字母宽**。据此定量：

| �?| �?|
|---|---|
| 标题字幕字距�?280 宽画面内�?| �?9 px/字母 |
| 换算到眼睛纹理（1996 宽） | �?26 px |
| **IPD 63 mm �?4 m 屏幕处的视差**�?280 宽画面） | �?**66 px** |

两眼的真实位姿差所产生的视差，换算到眼睛缓�?*恰好也是一个字母宽量级**�?而我们提交的两眼�?*同一张图**（MD5 一致），所以这个错�?*不是我们渲染出来�?*�?
**推断**：运行时对图层做**位姿重投�?*时，假设图像�?*从每只眼各自的位�?*渲染的，
按每眼位姿采样。而我们把相机放在�?*两眼中间**（`ipd_scale = 0` 的设计意图）�?于是运行时额外引入了一�?IPD 量级的错位�?
**这解释了为什�?消除重影"的招反而制造了重影**：为了去掉几何视差，我把相机放到中间�?却让运行时的采样假设失配�?
**测试**：`re6vr_ipd_scale.txt` 改为 **1**（每眼用真实位姿），使我们的渲染与运行时�?采样假设一致。若重影消失或显著减轻，则确认该推断，并把它设为默认�?
**第十一次实验（2026-09-22 21:59）—�?重影与两眼分离度无关（关键负面结果）**�?
`re6vr_ipd_scale.txt = 1` 确实生效了：

```
converge eye0: player (-0.03 -1.96 0.00) -> eye (-0.03 -1.96 0.00)
converge eye1: player ( 0.03 -1.96 0.00) -> eye ( 0.03 -1.96 0.00)   <- 两眼各自真实位姿
```

把两眼分离度�?**0 改成 63 mm**，用户反�?**没有变化**"。因此：

> **重影与两眼分离度无关�?* 既不是立体视差，也不是运行时按位姿重投影造成的�?
**这条排除了两个此前的主要假设**，价值很高。剩下的可能只有两类�?
1. 我们提交的图像本身在**那一�?*就有重影（此前的"眼睛图干净"结论测的是第 800 帧，
   与用户看的画面不是同一时刻）；
2. 运行时做了与位姿无关的几何处理（例如按换链尺寸与提交 FOV 的失配去采样）�?
**下一步：把两个导出对齐到同一帧�?* `re6vr_dump_at.txt` 的第一项现在同时作�?**眼睛图的导出�?*，于是同一次运行会产出**可对比的一�?*（游戏原始帧 + 该帧提交的眼睛图）�?这一对能直接判定重影是在**合成环节**引入的，还是�?*提交之后**�?
**第十二次实验�?026-09-22 22:02）—�?我们的绘制路径全部排除，改做纯色判别**�?
用户反馈"**依旧**，而且**从第一步能显示画面时就是这�?*"。这句时间是关键�?它说明重影不是今晚任何改动引入的，而是**从最开始就存在**的东西�?
**今晚已确证的事实**�?
| 检查项 | 结果 |
|---|---|
| 游戏原始帧（多帧�?| **干净**（`ghost_find.py` 无局部极大） |
| 像素着色器 | 单次采样，UV 就是 [0,1] |
| 采样器寻址 | `D3D11_TEXTURE_ADDRESS_CLAMP`（不可能纹理回绕�?|
| 图层提交 | 1 个投影图层�? 个视图、各�?`imageArrayIndex = i` |
| IPD 0 �?63 mm | **无变�?* |
| 分辨�?| 已修复（`capture staging 1996x1122`，清晰度应改善） |

**落在我们这边的环节全部排除了�?*

**还发现一个矛�?*：`ipd_scale=0` �?`ipd_scale=1` 两次运行导出的眼睛图
**MD5 完全相同**（`89066db99a4d4c92`）。�?`ipd_scale=1` 时两眼视图必然不�?（�?.03 的相机位置差）。所�?*眼睛图导出很可能一直没在区分两�?slice**�?此前所有基�?两眼�?的判断都不可靠�?
**判别实验（已部署�?*：`re6vr_test_solid.txt` 让合成图层显�?*纯绿�?*�?完全忽略游戏画面。纯色没有内部细节可供复制，所以：

- 头显里看�?*两块�?* �?合成路径把面板画了两�?- 看到**一块绿 + 一份游戏画�?* �?头显侧显示了游戏窗口/桌面镜像（与我们无关�?- 看到**一块绿** �?图层本身正常，问题在别处

**第十三次实验�?026-09-22 22:1x）—�?纯色实验必须分左右眼不同�?*�?
用户反馈�?*"你得给左右眼不同颜色给我判断，左右都一样我只能看出是一块绿�?**�?
这个反馈本身就是一个重要结论：**同一个颜色时，人无法区分"每眼各看到自己那一�?�?"两眼看到同一�?** —�?而这正是实验要回答的问题。实验设计有缺陷�?
`re6vr_test_solid.txt = pereye` 现在�?*左眼纯红、右眼纯�?*。判读方式：

| 头显里看�?| 结论 |
|---|---|
| **融合成黄�?橙黄** | 两眼各得自己的图�?—�?合成路径正确，问题在别处 |
| 同一只眼里同时有红和绿，或两块色�?| 某只眼同时收到了另一只眼的图层（**这就是重�?*�?|
| 全红或全�?| 两眼拿到同一�?slice |

**顺带**：用户此前只看到"一块绿�?，至少说�?*图层只出现一�?*——如果是两块绿，
他会�?两块�?。所�?我们的图层被画两�?这一假设基本被排除�?
**第十四次实验�?026-09-22 22:08）—�?异色实验做了，但红通道我写错了**�?
用户看到"**左边一大截蓝色，右边一大截绿色，中间一小块混合**"�?
**先认�?*：我本意是左红右绿，但把 `0x00FF0000` �?ARGB 去移位写�?BGRA 纹理�?得到的内存值是 `0x000000FF`—�?*红通道根本没写进去**，所以显示成蓝色�?已改�?*按名字显式取通道**（`0x00RRGGBB`），不再在两种约定之间转换�?
**这个观察的有效信�?*（颜色标错不影响结论）：

1. **两眼各自看到了自己的图层**（左眼一色、右眼一色），没有被对调或共用�?2. **两眼图像显著错开**——如果几乎重合，看到的应该是"全部混合"而不�?两大�?中间一小块"�?
�?2 点与 `ipd_scale=1` 下应有的几何�? m �?0.06 m 眼距 �?0.86°�?*对不�?*�?0.86° �?59.8° 的视场里只有�?1.5%，不该出�?两大�?�?
**又一次发现自检在骗�?*：`quad check` 打出的角点全�?`ndc=(0.00 0.00) w=0.00`，另一帧是 `ndc=(-5.20 -5.53)` —�?**这个自检的数学是坏的**，此前用它判�?四角落在视口�?的结论不可信�?
**同时确认**：帧 300 已是 logo 之后的画面、帧 500 是淡出黑场，
所以此�?游戏帧干净"的样�?*并不包含 Capcom logo �?*�?已把导出帧改�?60/90/120/150/200/250（logo 刚出现）�?
**第十五次实验�?026-09-22 22:12）—�?Capcom logo 帧干净，游戏排除了**�?
把导出帧调到 logo 刚出现的阶段�?0/90/120/150/200/250），�?150/200/250 正是
**Capcom logo**。视觉上"CAPCOM"清晰无复制；数值上三帧都是�?
```
frame 150/200/250: strongest local maximum prominence +0.000 -> NO duplication
```

**所以：游戏自己每帧并没有把画面画两遍�?*
（此�?游戏帧干净"的样本不�?logo 段，这次的样本才对上。）

**因此重影只能来自**：我们提交的眼睛图，或头显侧�?
**唯一无歧义的测试（已部署�?*：`re6vr_test_solid.txt = half` 让合成器显示一�?**左半�?/ 右半�?/ 中间一条白竖线**的图案（烘进一�?64×2 小纹理，因为我们的着色器
直接采样纹理、不�?uniform 颜色）�?
| 头显里看�?| 结论 |
|---|---|
| **两条�?*（红 \| 绿，中间一条白线） | 眼睛图只被画了一�?—�?合成路径正确 |
| **四条�?*（红绿红绿） | 眼睛图被画了两次�?*重影的机�?*�?|
| 白线**出现多条** | 画面被水平平�?|
| **全红**�?*全绿** | 两眼拿到同一�?slice |

**为什么必须用图案而不是纯�?*：纯色的两份拷贝看起来和一份完全一样—�?这正是上一�?一块绿�?无法给出结论的原因�?
**第十六次实验�?026-09-22 22:20）—�?每只眼单独看都是对的，错位在两眼之间**�?
�?左半�?/ 右半�?/ 中央白线"的图案（`re6vr_test_solid.txt = half`）问到两个关键答案：

1. **闭上一只眼�?* �?"中央一条白线、左红右�?�?   **每只眼拿到的图像本身是正确的�?*
2. **轮流闭眼比较白线位置** �?"左眼白线偏左，右眼白线偏�?�?   **两眼图像之间存在位移**（而非内部重复）�?
而日志确�?`ipd_scale = 0` 生效（两眼收敛到同一�?`(-0.03 -1.96)`），
**我们提交的两眼数据完全相同、内部视差为 0**。所以这个位�?*不是我们渲染出来�?*�?
**这解释了"capcapcom"的真�?*：每只眼单独看都是清晰的 "capcom"�?合起来因为两眼错位而变�?"capcapcom"�?*它不�?图像里有两份"，而是"两眼各一份、位置不�?�?*

**唯一还没排除的机制：我们�?swapchain �?一个纹理、两�?array slice"**�?如果运行时没有正确按 `imageArrayIndex` �?slice，就可能把两�?slice 都送进同一只眼
或按错误的基准对位——症状恰好是"每眼都对、合起来错位"，且改任何渲染参数都无效�?
**已实现第二种合法布局**（`re6vr_two_swapchains.txt`）：`arraySize = 1`�?**每只眼一个独�?swapchain**，`imageArrayIndex` 恒为 0�?这需要把提交路径改成成对 acquire/release 与按眼选择 swapchain，已完成�?离线回归全过（两种自检 + reset）�?
**看图工具**：`scripts/eye_sbs.py` 把两眼按头显的配对方�?*并排**输出�?磁盘上眼�?dump 的两�?slice �?*上下堆叠**的，�?有没有重�?错位"�?*水平**判断�?必须左右放着看：

```bat
:: 离线自检（每眼一个文件）
python scripts\eye_sbs.py build
e6vr_real_eye0.bgra build
e6vr_real_eye1.bgra _logs\sbs.png
:: 游戏内采集（单文件双 slice�?python scripts\eye_sbs.py "C:\...\Resident Evil 6
e6vr_eye_left.bgra" _logs\sbs_game.png 998 2148
```

其余侦察/分析脚本�?
| 脚本 | 用�?|
|---|---|
| `matrix_map.py` | 解析 `cap:` 日志，重建寄存器布局与相机轨�?|
| `eye_raw.py` | 按真实布局（上下堆叠）切开眼睛 dump |
| `eye_cover.py` | 量面板在单眼视口里的覆盖与黑�?|
| `eye_sbs.py` | 两眼并排成一张图 |
| `compare.bat` | 一次渲染多种屏幕配置做对比 |



要回答的问题，按重要性排序：

1. **矩阵在哪些寄存器�?*（`cap: reg N looks like VIEW/PROJ`�?2. **是分开�?VIEW+PROJ，还是合并的 VIEW*PROJ�?* 合并的话必须先把两者分解出来�?3. **每帧上传几次�?* 出现多次说明有多个相�?阴影 pass/后处理，得挑出主相机那一次�?4. **能不能在帧内改写而不崩？** 这是最大风险：本项目已�?MT Framework 对帧�?   `CreateTexture` 会直�?`ERR09` 致命退出，对常量改写是否同样敏�?*未知**�?   **必须先在单眼上试**（把视图矩阵平移一点点，看画面是否偏移而不是崩溃）�?   确认安全再做双眼�?
**风险提示**：真双视角意味着场景渲染两遍（D3D9 那边翻倍），加上本文档上文测到�?读回瓶颈，帧率可能不够。先做单眼平移验证，再评估性能�?
## 不加头显就能验证渲染（务必用这两个）

```bat
:: 1) 经典合成自检：四象限方向 + 双眼都画到了
set RE6VR_SELFTEST=1
build\harness.exe build\d3d9.dll 6 1280 720

:: 2) 真实几何自检�?98x2148 竖长�?+ 16:9 源图，双眼图落盘
set RE6VR_SELFTEST=1
set RE6VR_SELFTEST_REAL=1
build\harness.exe build\d3d9.dll 4 1280 720
:: 产物 build\re6vr_real_eye0.bgra / eye1.bgra
```

�?2 个是本次新加的：它用真实切片比例渲染，所以能离线量出"面板占多少视野�?有没有裁�?。配套脚本：

```bat
python scripts\eye_raw.py   build\re6vr_real_eye0.bgra _logs\real 998 2148 4
python scripts\eye_cover.py build\re6vr_real_eye0.bgra 998 2148
python scripts\eye_png.py   build\re6vr_real_eye0.bgra _logs\eye.png 2 998 2148
```

一条命令跑三种屏幕配置对比：`scripts\compare.bat`�?
### 3) 矩阵探针 / 头部视角的离线回归（harness �?`argv[5]` 模式�?
harness 会在每帧�?GPU 上传**人造矩�?*，所以分类器、`pick` 选择、旋转数学全�?可以离线验证，不必猜、也不必跑游戏：

```bat
:: 标定分类器：一�?view + 一�?proj
build\harness.exe build\d3d9.dll 3 1280 720 vsmatrix
::   期望 cap: reg 0 looks like VIEW / reg 8 looks like PROJ

:: 诱饵：屏幕倒数�?投影"排在其相机前面，pick 0 必须落在相机�?build\harness.exe build\d3d9.dll 3 1280 720 vsdecoy

:: 三台相机�?*两个批次** —�?正是当初 pick=-2 出错的形态（回归用这个）
build\harness.exe build\d3d9.dll 4 1280 720 vsviews
::   相机位于 reg 0 / 4 / 24，帧内序�?2 / 3 / 5

:: 乘法顺序：identity 相机 + 同一台相机转�?R，用来判定算的是 anchor×R 还是 R×anchor
build\harness.exe build\d3d9.dll 3 1280 720 camconvention
```

配合标记文件（放�?`re6vr_logdir.txt` 指到的目录，�?`_work`）：

```
re6vr_head_test.txt = 0 0 1 0 1 0 -1 0 0    :: 90° 偏航当姿�?re6vr_head_view.txt = 1 -2                   :: gain 1，选帧内最后一�?view
re6vr_capture_vs.txt = 1                     :: 打印分类与候�?```

然后让脚本判读，不用自己 grep�?
```bat
python scripts\head_check.py _work\re6vr.log --pick -2
```

> ⚠️ �?harness 之前确认 `build\re6vr_logdir.txt` 指向哪个目录 —�?> `markers.bat` 也读这同一个文件，所以它�?harness **必须看到同一份标�?*�?> 测完记得把它指回 `_work`，否则游戏里读不到开关�?
## 环境变量

| 变量 | 作用 |
|---|---|
| `RE6VR_SELFTEST=1` | 离线渲染自检，不需要头�?|
| `RE6VR_SELFTEST_REAL=1` | 真实几何自检 + 双眼图落盘（配合上一个用�?|
| `RE6VR_DUMP_FRAME=n` | �?n 帧导出游戏帧与双眼图（默�?800�?=关） |
| `RE6VR_NO_XR=1` | 纯直通：只记日志、不合成 |
| `RE6VR_FRUSTUM_ASPECT_MODE` | 诊断用，保持 0（见上） |
| `RE6VR_SWAPCHAIN_W` / `_H` | 单眼 slice 尺寸覆盖，实验性（�?已知遗留"�?|
| `re6vr_swapchain.txt` | 同上，内�?`<�? <�?`，高�?0 = 自动匹配 16:9（Steam 启动用这个） |
| `re6vr_capture_w.txt` | 抓帧 staging 宽度�?/缺省 = 跟随眼睛纹理（决定清晰度，见结论 9�?|
| `RE6VR_CAPTURE_VS=1` | Stage 2 侦察：分类引擎上传的顶点着色器矩阵（只读） |
| `re6vr_capture_vs.txt` | 同上，放在游戏目录�?*Steam 启动用这�?*（见�?9�?|
| `re6vr_shift_view.txt` | Stage 2 安全实验：第一�?= 平移米数，第二行 = 选择哪个视图矩阵<br>`-1` 全部（默认）／`-2` 本批最后一个／`0,1,…` 指定帧内序号 |
| `re6vr_head_view.txt` | **Stage 3 头部视角**：`<增益> [<pick>]`，pick 含义同上�?br>`scripts\markers.bat head 1 -1` 直接设好；`head off` 关掉 |
| `re6vr_head_test.txt` | 离线用：�?9 个数当头部姿态（行主�?3×3），不需要头�?|
| `re6vr_screen_follow.txt` | `1`／`0` 开�?`yaw_follow`（转头超�?45° 时把屏幕重新锚定）�?br>测头部视角时�?`0`，画面更干净。`markers.bat follow 0` |
| `re6vr_screen_scale.txt` | **屏幕观感缩放**（一个数，`0.05`～`8`），不动距离和宽度。基�?1.0 = 4.0 m/4.6 m 铺满显示屏。`markers.bat scale 1.5` |
| `scripts\head_check.py` | 离线校验头部视角的选择逻辑（选中的是不是帧内最后一�?view�?br>旋转有没有累积），读 harness 的日志，不需要头�?|
| `re6vr_uv_shift.txt` | **每眼水平补偿**，抵消运行时按位姿造成的双重影。`auto` / `auto-` / `<�? <�?`（百分比）／`0 0` 关闭 |
| `RE6VR_LOG_DIR` / `re6vr_logdir.txt` | 把日�?*和所有标记文�?*搬到该目录（一行路径，放在代理旁边）。游戏目录普通用户写不进去，这是不用提权就能调开关的办法 |
| `RE6VR_SELFTEST_STEREO=1` | 离线自检的第二眼�?*不同位姿**（默�?126 mm），用来量我们自己的双眼视差 |
| `RE6VR_SELFTEST_STEREO_IPD` | 上面那个自检的眼距，单位�?|

`re6vr_ipd_scale.txt` 的�?**�?1.5** �?会聚诊断"档：两眼�?*提交的位姿都相同**�?用它来判定重影是否由位姿驱动 —�?若重影消失，则是；若依旧，则与位姿无关�?
### 提交审计（每次运行自动打�?3 组，无需开关）

`on_frame` 里对每只眼打印一�?swapchain 句柄 / `imageArrayIndex` / `imageRect`�?一�?pose，一�?fov；`draw_quad` 里打印它**实际写入**�?RTV 槽位�?两个布局的期望值完全不同，一眼可辨：

```
submit: layout=two-swapchains, view_count=2, image_count=3, acquire[eye0]=i acquire[eye1]=i
submit: eye0 swapchain=0xAAAA imageArrayIndex=0 imageRect=(0,0 1996x2148)
submit: eye1 swapchain=0xBBBB imageArrayIndex=0 imageRect=(0,0 1996x2148)
draw:   layout=two-swapchains eye0 image=i -> rtv slot i   of 6
draw:   layout=two-swapchains eye1 image=i -> rtv slot 3+i of 6

submit: layout=shared-array, ...
submit: eye0 swapchain=0xAAAA imageArrayIndex=0 ...
submit: eye1 swapchain=0xAAAA imageArrayIndex=1 ...     <- 同一句柄，索引不�?draw:   layout=shared-array eye0 -> slot i*2+0
draw:   layout=shared-array eye1 -> slot i*2+1
```

判读要点：`two-swapchains` 下两眼必须是**两个不同句柄**�?`imageArrayIndex` 都是
**0**，�?`draw` 的槽位必须落在该眼自己的那一段（eye0: `0..2`，eye1: `3..5`）；
`shared-array` 下两眼必须是**同一个句�?*、索引分别为 0 �?1�?
`xrEndFrame` 的成败也要看这一行（600 帧一报，失败只在首次打印）：

```
openxr: N frames submitted (M failed)
```

**它必须一直在涨�?* 卡住不动就说明每帧都在失败，画面根本到不�?runtime�?

## 下一�?
**2026-09-24 状�?*：屏幕（大小/比例）已修好并验证；**头部跟踪的判据已按外部一手资料重�?*
（见"外部调研推翻了相机对象路线已�?一节）。原来的三条死路结论**部分作废**�?"搜内存找相机"死的�?*判据**（找矩阵），不是路线本身 —�?MT Framework 的相机是 look-at 三元组�?**第三轮之后还有个好消�?*：要写的**字段偏移已经不是猜的�?*
（`sBioCamera` �?`mCameraOrg[i]`，见"相机字段偏移已经静态核�?一节）�?而且**写它的函数（`0x004FF9B0`）和它的输入组（`+0x1030`）也定位到了**�?所以现在缺�?*只剩"运行时确�?**，不再是"找东�?�?
- [x] **已完成（第四轮）**：这个探针写好了�?*离线自检 PASS**、已编译�?`d3d9.dll` 并部署到游戏目录�?  标记 `re6vr_cam.txt = 1` 也已设好�?*现在只差你跑一局**（进关卡即可，探针只读）�?  完整判据、日志行、三种结局的含义见
  [「运行时探针 `mem_cam.cpp`」](#-2026-09-24-第四轮运行时探针-mem_camcpp已部署等你跑一局) 一节�?  离线验证：`python scripts\cam_selftest.py`（不需要游戏、不需要头显）�?
      > �?**2026-09-24 第四轮：这个探针已经写好、编译进 DLL、部署到游戏目录�?*
      > （`re6vr_cam.txt` 标记已设 = `1`）。它比上面这一条做得更多：除了读那三个全局�?      > 还会**按类指针在内存里找活�?`sBioCamera`**，并对每个命中做几何自检�?      > 跑法就在下面�?
## 🔎 2026-09-24 第四轮：运行时探�?`mem_cam.cpp`（已部署，等你跑一局�?
### 它回答什�?
| 问题 | 判据（都在日志里�?|
|---|---|
| `[0x017D270C]` �?`uCameraCtrl` 实例吗？ | 读出的�?+ 该对�?`+4` 的类记录是否等于 `0x017D26F0`�?*实测：不是，见下**�?|
| `[0x17CF454]` / `[stage+0x640]` 那条链通不通？ | 同上�?*实测：整局都是 0**�?|
| **有没有活�?`sBioCamera`？在哪？** | 全内存扫"`+4` 等于 `0x017C3164`"�?*对象**（还要有指向 `.text` �?vtable�?|
| **`mCameraOrg[0]` 那套偏移对不对？** | 几何自检：`up` 单位长、`forward` 单位长且�?`up` 垂直、`fov�?0.05,2.2)`、`near<far` |
| 一个都没找到时，引擎到底造了哪些相机�?| 相机类普查：六个相机类各有多少个带代�?vtable 的对�?|

### 为什么按"类指�?找，而不是按形状�?
这是本项目第五次判据事故的教训换来的�?*形状不是身份**。正交三元组能匹配一堆东�?（UI、视口、骨骼矩阵都�?正交 3×3 + 平移"），�?MT Framework 的实�?`+0x04` 就是它自己的
类记录指�?—�?`sBioCamera` �?`0x017C3164`（随模块重定位）�?*一个值，不是一种形状�?*

再加上两�?*必要**条件的合取：
1. `+0x00` 必须是一个指向镜像内的可读指针（vtable）；
2. `mCameraOrg[0]` 的六个字段必须构成一个合�?look-at 位姿�?
命中这两条的地址，不可能是巧合�?
### 跑法（一条命�?+ 一局游戏�?
```bat
scripts\markers.bat cam 1        :: 已设好（本次就是 1）；探针只读，不写任何游戏内�?```

然后**正常启动游戏，进关卡（读取存�?开始一关）**。探针在 D3D9 创建�?*�?20 �?*
（要给关卡加载留时间）才开始扫，扫 1~3 秒。日志里看这几行�?
```
cam: BH6.exe module base = XXXXXXXX (the static notes assume 00400000, so the delta is +N)
cam: uCameraCtrl slot   [017D270C] = XXXXXXXX -> vtable XXXXXXXX  class sBioCamera/uCameraCtrl
cam: stage object       [017CF454] = XXXXXXXX -> ...
cam: [stage + 0x640] = XXXXXXXX -> vtable XXXXXXXX  class ...
cam: *** N live sBioCamera object(s) with a valid camera pose ***
cam: obj XXXXXXXX  vtable=XXXXXXXX  class=sBioCamera  mCameraOrg[0]:
cam:     cameraPos (x y z)  targetPos (x y z)  cameraUp (x y z)
cam:     fov 0.9000 rad (51.6 deg)  near 0.100  far 500.00   forward len 1.00
cam:     geometry OK - so this object is a LIVE look-at camera and +0xE40 (targetPos) /
          +0xE50 (cameraUp) are the fields to write for head look
```

**判读**（三种结局都有明确含义，不会出�?看起来像结论"的假结论）：

- **`geometry OK` + 一�?obj 地址** �?相机对象、类地图、字段偏移三者互证，
  **下一步就是写入实�?*（hook `0x004FF9B0` 或在它之后覆�?`+0xE40`）�?- **`class-pointer match ... but geometry failed: <哪一�?`** �?找到�?`sBioCamera` 类型的对象，
  但那些偏移上不是位姿 �?说明**活的那个不是�?*（比如是别的 entry），日志会给出失败的具体判据�?- **`NO live sBioCamera object found`** �?它自己会说明原因（没进关�?/ 20 秒太早）�?  并且**先说明扫描本身已验证�?*（见下），所以这个零是有意义的零�?
### 这个探针的搜索逻辑**已经离线验证�?*（`RE6VR_CAM_SELFTEST=1`�?
"探针什么都没找�?�?探针坏了"长得一模一样，所以搜索本身有一�?*合成自检**�?在堆上种一�?*必须找到**的对象（类指针正�?+ 位姿合法）和一�?*必须拒绝**的对�?（类指针正确 + 位姿非法），然后�?*真正�?*搜索函数，只看结果集对不对：

```
python scripts\cam_selftest.py          :: 一条命令，不需要游戏、不需要头�?cam: selftest: must-find object FOUND, must-reject object correctly rejected -> PASS
```

**这个自检立刻抓到了三个真问题**（每一个都会让实机那一局白跑）：

| # | 症状 | 根因 |
|---|---|---|
| 1 | 扫了 **0 MB**，报�?没找�? | 自检没设 `g_running`，而搜索循环以它为�?�?循环体一次都没进 |
| 2 | 种下的对象被�?几何失败"，字段是 `0xCDCDCDCD` | 夹具�?`span` 不够长：对象�?`+0x1800`，而相机字段在 `+0xE30` �?**字段落在分配之外** |
| 3 | 探针�?`Direct3DCreate9` 里直�?*崩掉**（`0xC0000005`�?| 夹具用了�?3.6 KB 填充�?*栈上结构�?*；改成堆上按字节写就正常 |

> 这正是本项目最贵的那条教训的正面用法：**一个会�?什么都没找�?的工具，必须先用
> 一�?它必须找�?的样本证明自己能找到�?* 三次都是自检抓的，不是实机�?
### ⚠️ 仍然只是假设的部�?
- 探针**只读**：确认了对象和偏移，**没有**写入实验。头部跟踪的实际效果仍未验证�?- 扫到多个 `sBioCamera` 时，**哪一个对应玩家视�?*还没判据（日志会列出�?6 个）�?- `0x4FF9B0` 是不�?*每帧**跑、引擎会不会在它之后又覆�?`mCameraOrg`，仍然未知�?
### 📊 第一次实机跑�?026-09-24 23:36）：三个答案 + 一处判据缺�?
那一局没戴头显（`xrGetSystem: FORM_FACTOR_UNAVAILABLE`），而且**没进关卡**—�?但探针照样给出了结论，而且**没有一个是无用�?*�?
**�?`[0x017D270C]` 不是实例�?—�?旧结论确认了�?*

```
cam: uCameraCtrl slot  [017D270C] = 5614BCA1
cam:   -> vtable 00000000  class (not a camera class record)
cam:   does not point at a uCameraCtrl (record reads 00000000, expected 017D26F0)
```

它确实是�?*指针**（堆地址 0x5614BCA1），但指向的对象头里**没有类记�?*�?所�?`managers.py` 一直坚持的那句话是对的�?*它是工厂�?context 参数，不是单例槽�?*
这条假设可以彻底放弃了�?
**�?阶段门槛（`[0x17CF454] + 0x640`）是有效的，只是当时还没有关�?*

```
cam: stage object        [017CF454] = 20174060 -> vtable 01521970
cam: [stage + 0x640] = 0  -> no camera controller there (yet?)
```

入口那一局�?*标题画面**（进�?23:36:14 起来，探�?23:36:34 扫）——`+0x640` 就是 0�?**"扫描需要一个已经加载的关卡"这条警告不是猜测，是实测�?*

**�?判据缺陷：按"类指�?找会撞上引擎自己的类记录表�?* 这是本轮最有价值的发现�?
```
cam:   class-pointer match at 01814840 (head 017C1ABC 017C3164 017D2780 017C1674) ...
cam:   class-pointer match at 0186E268 (head 01871438 017C3164 0186E5D4 00000000) ...
```

两个命中�?`+4` 都是 `017C3164`，但**它们不是对象**�?
- `01814840` 的头部是 `017C1ABC / 017C3164 / 017D2780 / 017C1674` —�?**四个 MtDti 记录�?  步长正好 0x20**，这是一�?DTI 表（而且是相机家族那几张），不是实例�?- `0186E268` 落在 **`sCamera` 的类记录里面**（`sCamera` �?DTI 就是 `0186E260`）�?
也就是说�?*"某个地址 +4 等于类记�?既能匹配实例，也能匹�?提到这个类的�?�?*
判据因此收紧�?*两条同时成立**�?
| 条件 | 为什么必�?|
|---|---|
| `+4` == `sBioCamera` �?MtDti | 类身份（原来的判据，不够�?|
| `+0` �?*指向镜像 `.text` 的指�?* | vtable 必须指向**代码**；类记录表里那一格指向的�?`.rdata` 里的数据，直接被排除 |

判据本身�?PE 节表判断（`read_pe_sections`），日志会打印解析出来的节�?> ⚠️ 顺手记一条差点犯的错�?*不能"跳过镜像内的区域"**——游戏的堆恰恰是在镜像范围内分配的，
> 按区域跳过会把真正的对象也跳过。只跳过 `.text`（可执行节，里面不可能有对象）�?
**�?探针不再靠定时器，改成等引擎自己说话�?* 那一局的教训很直接�?"固定 20 秒后扫一�?在标题画面必然落空。现在：

- **等阶�?*：轮�?`[0x17CF454] + 0x640`，非 0 才开扫（最多等 5 分钟）；
- **扫描重试**：第一次扫不到就每 15 秒重扫一次，最�?8 次�?  **所以你不需要掐时间**——进关卡、正常玩，探针自己会等到那一刻�?
**下一次要跑的就一件事**：正常启动（**戴上头显**）、进关卡、随便走走�?日志里会出现�?`cam: a stage is loaded (the camera controller exists) - scanning now`�?然后就是要看�?`geometry OK` 那一段�?
### 📊 第二次实机跑�?026-09-24 23:42）：头显在、真进度�?*探针一次都没扫**

这一次条件齐了：头显接上（`xrBeginSession -> XR_SUCCESS`）�?*4800 帧全部提交成�?*
（`0 failed`）、头部姿态实时有效（yaw �?-34.8° �?+11.3°）、合成器 10500 �?60 fps�?**但整�?`cam:` 段只�?已布�?那几行，一次扫描都没有�?*

原因是我自己加的那道门：探针在等 `[0x17CF454] + 0x640` 变成�?0，�?*它整局都没�?*�?**一道会静默吞掉整局的门，比没有门更糟�?* 已经改掉�?
1. **不再依赖那道�?*：探�?*�?20 秒自己扫一�?*（最�?12 次），门上不上只是决定第一次要不要提前�?   每次尝试都会打一�?`cam: scan attempt n/12 (stage [...] = set, [stage+0x640] = zero/... )`
   —�?**门开没开本身也是要被采集的数�?*，不再是一个静默的开关�?2. **扫描失败不再只说"没找�?**：会接着跑一�?*相机类普�?*（`find_instances(..., count_only)`），
   �?`sBioCamera / sCamera / uCameraCtrl / uCameraBase / uCameraQFPS / uCameraAnimation`
   **各自有多少个带代�?vtable 的对�?*打出来。这样下一次的结论�?引擎造了 A 没�?B"�?   而不是又一个需要猜的零�?
> ⚠️ **同时这也推翻了静态笔记里的一条假�?*：`[stage对象+0x640] = uCameraCtrl 实例指针`
> 这条链在**真正进了关卡**的一局里始终是 0（`[0x17CF454]` 本身�?0，是 `20174060`�?> �?`+0x640` 一直是 0）。所以要么偏移不同，要么相机管理器挂在别处�?> **这条链不能再当作"等关卡加�?的判据�?*

### 📊 第三次实机跑�?026-09-24 23:49）：**12 次扫描�?653 MB、六类相机全�?0**

这一局 8400 帧全部提交成功、头显姿态全程有效，探针按新策略扫了 **12 �?*（每�?1.65 GB�?各约 2.3 秒）。结果：

```
cam: scan finished: 1651 MB examined, 14 raw class-pointer hit(s)
cam: census: sBioCamera (017C3164): 0 object(s) with a code vtable
cam: census: uCameraCtrl / uCameraBase / uCameraQFPS / uCameraAnimation ...: 0
```

**"�?`+4` == 类记�?这条键，两局、十几次全量扫描、一�?0 命中 —�?连引擎不可能没有�?`uCameraAnimation` 都是 0�?* 找不到任何东西，说的不是相机，是**找法**�?
（顺带：日志�?`census` 命中的唯一一个是 `1576CF1C`，它�?vtable"�?`014A439D` —�?**低字�?0x9D，不�?4 字节对齐**，一看就不是对象；几何自检也把它拒了�?这类假命中正�?vtable 检查存在的理由。）

### �?于是加了**第二把钥匙：vtable 地址**

静态分析其实还有一�?*独立**的类身份线索：`propmap.py` 是靠 **vtable** 把属性表归属到类�?（vtable＝`.rdata` 里包含该�?MtDti getter 的那个运行段）。把这份映射导出�?（`_work\build_vtables.py` �?`src\mem_cam_tables.h`）：**17 个相机类�?vtable + 类记�?+ 大小**�?例如 `sBioCamera vtable=0x0151A380 dti=0x017C3164 size=0x1620`�?
**vtable 是对象的第一�?dword**，所以直接按它扫，一遍内存就能回答：
**"这台引擎到底实例化了哪些相机类、各多少�?**，并且每个命中都会把 `+4` 的真实值报出来 —�?**`+4` 到底是不是类记录，从此是实测而不是假设�?*

自检（`python scripts\cam_selftest.py`）依�?**PASS**。顺便它又抓到一个夹具自己的错误�?夹具原本借用"�?sBioCamera �?vtable + 基址�?，那�?*游戏�?*是对的、在**离线 harness �?*
是另一个镜�?�?种下的对象因为一个与搜索无关的原因被拒，自检 FAIL�?已改成用**本模块自己的可执行地址**�?
### 🎮 游戏节奏（玩家实测，直接决定探针什么时候该扫）

> **读档之后先是一段镜�?CG，然后才是可操作时间；走到门口也会停下来对话�?*

这条比任何猜测都值钱�?*计时扫描必然把预算花�?相机还不存在"的状态上**�?所以扫描调度改成跟着**引擎自己的阶段切�?*走：

1. **等阶段变�?*（`[0x17CF454]` �?`[stage+0x640]` 的合成值一变即为一次切换）—�?   阶段一变就是相机对象被造出来的时刻�?2. 变化之后才计入重试预算（最�?12 次，整体 15 分钟上限）；
3. **退出游戏时允许最后扫一�?*，这样一局结束时日志里留下的是**答案**而不是沉默�?
`[stage+0x640]` 仍然**只当作变化检测的一部分**，不再当�?关卡已加�?的判�?—�?因为实测它在游戏里也�?0（见上）�?
### 🗄 日志不再被覆盖（这条吃过亏）

代理启动时会**截断** `re6vr.log`，所�?*每跑一局就毁掉上一局的证�?* —�?23:49 那局�?12 次扫描原始日志，就是被我自己的离线自检覆盖掉的。现在：

```bat
python scripts\play.py            :: 先把上一份日志归档到 _work\_archive\re6vr_<时间�?.log，再启动游戏
python scripts\play.py --no-launch :: 只归�?python scripts\cam_selftest.py    :: 自检写到 _work\harness_log\，不碰游戏日志（并在结束后把指针还原�?```

顺带修掉两个自己踩的坑：`play.bat` 在本�?locale 下生成空时间戳（`re6vr_.log`），
改成 Python；自检改写 `build\re6vr_logdir.txt` �?*必须还原**，否则下一局游戏会在
`harness_log` 里找标记 �?**探针静默失效**�?
### 📊 第四次实机跑�?026-09-25 00:03）：**换了一道门，还是没�?*

改了调度之后那一局：探针布防、读全局、然后打�?
```
cam: waiting for the stage to CHANGE before the first real scan ... Stage marker now 20294060
```

**然后整局那个 marker 一动没动，一次扫描都没有�?* 日志�?00:07:06 结束�?
**这是第三次栽�?�?�?*�?0 秒定�?�?`[stage+0x640]` �?阶段变化），三次的形态一模一样：
**一道我以为有意义的信号，静默地把整局吞掉�?* 教训写死在这里：

> **探针的任�?等待"都必须是可超时、可降级的；任何"信号"都只能增加信息，不能阻止扫描�?*

**所以这一版把门全部拆掉：**

- **不再有任�?gate**�?0 秒后开始扫，之后每 30 秒一次，最�?20 次（覆盖 10 分钟）；
- **两把钥匙同时�?*，每次尝试都报两边的结果�?  - **vtable 钥匙**（`+0x00` == 静态导出表里的某个相机 vtable）—�?这是**本该第一个做**的判据：
    它不依赖 `+4` 的布局假设，而且每个命中都会�?`+4` 的真实值打出来�?  - **类记录钥�?*（`+4` == sBioCamera 的记录）—�?保留作为对照观察�?- 每次尝试都顺带报�?stage 指针状态，**信号变成被采集的数据**�?
自检覆盖两把钥匙（种一个合�?+ 一个非法位姿，两个都带�?vtable）：

```
cam: selftest: vtable key found 1 instance(s) (12 hit(s), 11 rejected):
     must-find FOUND, must-reject correctly rejected -> PASS
```

### 📸 2026-09-25�?*扫描时自动截�?+ 判定"这一帧该不该�?**

三次栽在"�?上的根因，说到底是一�?*判断我没有数据可�?*：某一刻游戏到底在菜单�?在读盘、在 CG，还是在可操作的关卡里�?*截图不是猜测 —�?它就是游戏正在画的那一帧�?*

- 探针每次扫描�?*请求一张截�?*（`src\screenshot.cpp`）；桥在渲染线程�?  **后缓冲还锁着的时�?*抓（`screenshot_service`），写成 BMP 到日志目录，
  同时在日志里给出一句判定；
- 判定只用一次稀疏采样（�?4×4 取一点，4K 帧约 1 ms）算四个数：
  **量化后的不同颜色�?/ 全黑比例 / 亮像素比�?/ 肤色比例**�?- 判定句直接写进日志，和这次扫描的结果挨着�?
```
shot: 'scan03' 3840x2160  distinct=3120  black=4%  lit=91%  skin=7%  mean luma=88
      -> rich colour range - this looks like RENDERED GAMEPLAY, a scan here is meaningful
shot: 'scan04' ...        distinct=42    black=88% lit=6%   skin=0%  mean luma=9
      -> few colours, mostly dark - a MENU or loading screen, no playable camera
```

这样"没找到相�?第一次可以被读成**"那一刻本来就没有相机"**，而不是又一次沉默�?
**判定逻辑自己也有自检**（`RE6VR_SHOT_SELFTEST=1`）：把一张渐变图�? 游戏画面）和
一张黑�?浅色面板�? 菜单）喂�?*真正�?*采样函数，比对判定结果，并且真的写一�?BMP
再用 Python 验回来：

```
shot: selftest gameplay frame: distinct=4421 black=0% -> ... RENDERED GAMEPLAY ...
shot: selftest menu frame:     distinct=2    black=87% -> ... a MENU or loading screen ...
shot: selftest: gameplay verdict correct, menu verdict correct -> PASS
```

（BMP �?Python 验过：`BM` 头�?4bpp�?40×360、行对齐 1920 字节、总长 691254�?黑底与中央浅色面板的像素位置都对。）







- [ ] **一次实机读数（同一局就能做完�?*：把 `sBioCamera` 实例地址（上一步的
      `[[0x017D270C]+?]` �?`[[0x17CF454]+0x640]` 链上那个）上的三组数打出来，
      �?`mCameraOrg` 做一�?*几何自检**（和 `mem_scan.cpp` �?`check_camera_org()` 同一套判据）�?      `up` 单位长、`forward = target - pos` 单位长且�?`up` 垂直、`fov �?0.05..2.2 rad`�?      全过 �?**这个对象就是活的相机**，`+0xE40` 就是要写的字段，头部跟踪可以直接进写入实验�?      不过 �?打印是哪条判据不过（这正�?`check_camera_org()` 已经会做的事）�?- [ ] **写入实验（上一步通过之后�?*：三个写点任选，推荐 hook `0x004FF9B0` 在它写完之后
      覆盖 `mCameraOrg[0].targetPos`（每帧最后一次发言权）；退而求其次�?`+0x1040` 组�?      **必须**照老规矩打写入审计（本项目五次"弱判�?事故的教训）：日志要能证明写入真的发生�?- [ ] **原来�?`scan` 路线（viewport 签名）可以继�?*，但它现在是**备用**：字段偏移已定，
      不需要再从内存里猜相机对象了�?
- [ ] **需要你跑一局（当前构建，`scan` 已开�?*：进关卡**正常走动十几�?+ 转头超过 20°**�?      抓取是运动触发的（相机真的移�?60 单位才抓 pose 2），不用掐时间�?      要看的行�?      ```
      scan: viewport signature search examined ... MB, found N candidate(s)
      scan: --- viewport 1 at 0x...: mViewMat translation (..), mProjMat m11=.., mpCamera=0x...
      scan: --- viewport 2 at 0x...: ...   <-- THIS IS THE CAPTURED CAMERA (translation matches)
      ```
      - **找到并且有候选的平移等于捕获�?* �?拿到相机 viewport �?`mpCamera`，下一步就�?        走指针读/�?look-at 三元组（`mCameraPos/mCameraUp/mTargetPos`），真头部跟踪即可落地�?      - **找到候选但都不匹配** �?说明 RE6 �?viewport 布局与同族不同，需要按候选的 dump
        重新认偏移�?      - **N = 0** �?签名不成立，下一步是**装真反汇编器**（Capstone）定�?RE6 �?        `sCamera`/`SMediator` 等价物，不再靠猜�?- [ ] **头部跟踪的写点（拿到相机对象之后�?*：每帧用头显 yaw/pitch 构�?lookAt，写
      `mTargetPos`/`mCameraUp`（保�?`mCameraPos` 或按位置追踪写它）。这是唯一被证明能同时
      给出 yaw/pitch/位置/roll 的机制（着色器层给不了 yaw/pitch）�?- [ ] **输入层后备（若相机对象路线再失败�?*：hook `IDirectInputDevice8::GetDeviceState`
      �?COM vtable（设备对象已可得）比代理 dinput8 **更少冲突�?*；但注意它只能给
      "跟随�?转头（引擎自己的阻尼与限位），不�?1:1�?- [x] **已完�?*：屏幕大�?+ 比例（`apparent:` 三段自洽，tan-aspect 1.778）；标记目录坑；
      运动触发抓取；刚�?世界空间两把筛子；`tanf` 误用；dinput8 代理的递归崩溃（已修但暂不装，
      `deploy.bat di` 才装）�?- [x] **已废**：`pick = 4/6/27`（写入发生而画面不动）；在顶点常量流里找相机（Vireio 证明
      那层给不�?yaw/pitch）；XInput 路线（RE6 不导�?XInput）�?
- [ ] **选项 A（已实现，等一局）：DINPUT8 代理 �?头转�?*
      - 已部�?`dinput8.dll`�?63840 字节�? 桥侧共享内存发布，注入默�?**off**�?      - **第一局只要做一件事：进游戏推一次右摇杆**，然后把日志里这几行发回来：
        - `di: DirectInput8Create ok: IDirectInput8 vtable patched ...`
        - `di: CreateDevice(keyboard/mouse/device{...}): vtable patched ...`
        - `di: state[N] lX=.. lY=.. lZ=.. lRx=.. lRy=.. lRz=.. ...`（推摇杆时哪个在动）
        - `di: head: sharing 're6vr_head_pose' open, yaw now ...`（桥那边应有
          `head: publishing yaw for the dinput8 proxy through 're6vr_head_pose' (mapped)`�?      - 拿到"哪个�?之后：`markers.bat di axis <偏移>` + `markers.bat di on`，再跑一局看画�?        是否跟着头转�?*这是目前唯一还能推进头部跟踪的路线�?*

- [ ] **选项 B：把现有"VR 影院"磨利（不需要新路线�?*
      - `compositor: readback 11.5 ms/frame` 是最大开销�?0 fps 预算�?70%）。抓帧尺�?        现在 2492×1401；配�?`re6vr_swapchain.txt` 降尺寸可省下可观时间，改完直接对比日志数值�?      - `dist` / `width` / `scale` 现在都真的生效（`apparent:` 三段可验证）�?
- [x] **已完�?*：大小与**比例**（`apparent:` 三段自洽，`tan-aspect 1.778`）；标记目录坑；
      运动触发扫描；刚�?/ 世界空间两把筛子；`tanf` 误用；DTI 可达性判定�?- [x] **已废**：`pick = 4/6/27`；在顶点常量流里找相机；搜内存；DTI 注册点�?
## 本次改动�?026-09-22 下午�?
1. **�?`screen_dist` 无效**：`place_panel` 原来把面板放�?`玩家 + screen_dist + 1.6`�?   �?`draw_quad` 又把虚拟相机放在面板前硬编码�?1.6 m。两者自洽，但结果是
   **屏幕看起来多大完全不�?`RE6VR_SCREEN_DIST` 影响**，真实视距恒�?1.6 m�?   现在两处都用 `screen_dist`，虚拟相机正好落在玩家位置�?   日志证据：改�?`screen 4.0 m ahead ... panel at 1.60 m`，改�?`at 4.00 m`�?2. **默认值改为影院档** 4.0 m / 4.6 m（张�?59.8°×35.8°）�?3. **新增真实几何离线自检** `RE6VR_SELFTEST_REAL`，双眼图落盘�?4. **新增运行时每�?FOV 日志**，作�?屏幕该做多大"的标尺�?5. **修复经典自检的失效探测点**（硬编码 ±0.95 NDC �?按源�?UV 取象限中心）�?6. **纠正眼睛 dump 布局的认�?*（结�?7），并补上正确解读它的脚本�?

## HANDOFF 2026-09-25 - start here

**What changed tonight**: the camera object is no longer hunted in memory. The engine's own
`mCameraOrg` writer is hooked, and it hands over the object address directly.

```
marker files:  re6vr_cam.txt = 1        (the memory-scan probe)
               re6vr_camhook.txt = 1    (the writer hook - the one that matters)
               re6vr_camwrite.txt      (write experiment: "sweep" or "test")

run:           python scripts\play.py --sweep     (archives the old log, arms the sweep, launches)
```

### Established by measurement (do not re-derive)

| fact | evidence in the log |
|---|---|
| writer function = `BH6.exe + 0xFF9B0`, hook installs fine | `camhook: target 004FF9B0 ... first bytes 83 EC 10 53 56 57` |
| `this` is stable for a whole session (e.g. `1F47A060`) | every call reports the same address |
| it is NOT per-frame: 2 calls per session (all-zero at process start, real pose at level build) | `00:57:08` then `01:00:23` |
| the property offsets ARE right - the engine writes a valid look-at pose itself | `pos (1840.25 502.31 -3716.55) ... up len 1.000 fov 37.0 deg` |
| writing `+0xE40` (targetPos) has NO effect on the rendered view | write audit clean, picture static (`00:44` run) |
| never identify the object by its header | `vtable` reads `0151A394` (= vtable+0x14), `+4` = `FFFFFFFF` |
| a level takes ~3 minutes to build after launch (read save -> cutscene -> level) | hook call #2 timestamps |

### Next step (one command, one observation)

```
python scripts\play.py --sweep
```

Load a save, **get into the level and walk around for 1-2 minutes**. The sweep cycles 5 candidate
write offsets, 6 s each (30 s per round), and the log labels every step:

```
camwrite: the engine has a camera pose - THE SWEEP STARTS NOW. 5 candidates, 6.0 s each (30 s per round).
camwrite: NOW TESTING 1/5 (0-6s):   hooked object +0xE40  (measured no effect)
camwrite: NOW TESTING 2/5 (6-12s):  hooked object +0x1050 (the copy's source for entry 0)
camwrite: NOW TESTING 3/5 (12-18s): hooked object +0x40   (sibling-title targetPos offset)
camwrite: NOW TESTING 4/5 (18-24s): source object +0x50   (what 0x4F9950 copies in - strongest)
camwrite: NOW TESTING 5/5 (24-30s): source object +0x60   (the same object's up vector)
```

* **picture swings -> note the step number**: that offset is the write point, and head tracking is
  then only "replace the swing with the headset's yaw".
* watch `[survived N frame(s), overwritten M]`: an M that keeps rising means the write lands but the
  engine rewrites that field every frame - a different problem with a different fix.
* **nothing moves in 5 steps -> the renderer reads a third copy.** Then stop writing parameters and
  go at the view matrix: `matrix_probe` already classifies the matrices the engine uploads, and the
  lesson from tonight is to use *the engine's own uploaded matrix*, never a guessed shape.

### Traps hit tonight (all fixed - do not repeat)

1. `set VAR=1` then launching through Steam does **not** pass the environment on: the experiment
   silently never ran. Write switches as **marker files**.
2. Do not decide *when* to start with a timer: the signal now is **the engine putting a valid pose
   in the object**, which is an observed transition, and the sweep cycles so timing stops mattering.
3. Each run **truncates** the previous log: `python scripts\play.py` archives it to
   `_work\_archive\` first.
4. Do not edit this project's files with PowerShell `Set-Content` / here-strings - that is what
   damaged this README. Use the file tools, which write UTF-8.
