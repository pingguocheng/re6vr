import io

path = r"C:\re6vr\README.md"
old = open(path, "rb").read().decode("utf-8", errors="replace")
# Keep the original body from its first real section heading onwards; the mojibake
# header is replaced below. Find where the directory listing starts.
marker = "## "
idx = old.find("```\nre6vr/")
body_start = old.rfind("\n## ", 0, idx) if idx > 0 else 0
body = old[body_start:] if body_start > 0 else old

header = """# RE6 VR Mod — 接手指南

> **给新会话**：一切都在磁盘上，与对话无关。本文件是唯一入口，先读完这一节再动代码。

## 30 秒了解现状

给 Steam 版 Resident Evil 6（32 位 DX9 / MT Framework 2.x）做的 VR 注入层。
**游戏画面已经能出现在 Quest 3 头显里**（Stage 1 打通），当前在收尾"左右眼一块屏、
不重影、不黑"的几何问题。

```
代码     C:\\re6vr\\
部署     C:\\Program Files (x86)\\Steam\\steamapps\\common\\Resident Evil 6\\d3d9.dll
         （205824 字节，MD5 前缀 BF98ED04）+ openxr_loader.dll
```

> ⚠️ Steam 校验文件或重装游戏会删掉这两个 DLL，需要重新 `scripts\\deploy.bat`。

## 三条命令

```bat
scripts\\build.bat      编译 32 位 d3d9.dll
scripts\\deploy.bat     部署到游戏目录（需要写权限）
scripts\\uninstall.bat  卸载
```

**启动必须走 Steam**（`BH6.exe` 直接跑会 `Failed to initialize Steam.` 然后退出）。

## 不加头显就能验证渲染（务必用这个，别靠"启动一次看看"）

```bat
set RE6VR_SELFTEST=1
build\\harness.exe "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Resident Evil 6\\d3d9.dll" 6 3840 2160
```

它用真实管线离屏渲染并测量面板的 NDC 范围与四象限方向，结果写在游戏目录 `re6vr.log`。
本项目绝大多数几何 bug 都是它定位的。

## 环境变量

| 变量 | 作用 |
|---|---|
| `RE6VR_SELFTEST=1` | 离线渲染自检，不需要头显 |
| `RE6VR_DUMP_FRAME=n` | 第 n 帧导出双眼图像 `.bgra`（用 `scripts\\bgra_to_png.py` 转 PNG 看） |
| `RE6VR_NO_XR=1` | 纯直通：只记日志、不合成 |
| `RE6VR_IPD_SCALE` | 默认 `0`：两眼共用视点，消除重影；`1` = 真实眼距 |
| `RE6VR_SCREEN_DIST` | 屏幕距离（米），默认 2.5 |
| `RE6VR_SCREEN_WIDTH` | 屏幕宽度（米），默认 2.9（自动 16:9） |
| `RE6VR_SCREEN_FOLLOW` | 默认 `1`：头部偏转超过约 45° 时把屏幕重新锚定到正前方 |

## 最重要的技术结论（读代码看不出来的那些）

### 1. `IDirect3DSurface9` 的 vtable 比 `d3d9.h` 多一个槽 ← 最关键

运行时对象在槽 11 有一个**不属于公开接口的保留槽**，后面全部后移一位：

```
d3d9.h 文档      实际运行时
GetDesc  = 11     Reserved11 = 11
LockRect = 12     GetDesc    = 12
UnlockRect = 13   LockRect   = 13
GetDC    = 14     UnlockRect = 14
                  GetDC      = 15
```

我按文档写槽位时，每一次 "LockRect" 实际调用的是 `GetDesc`，它往调用者的
`D3DLOCKED_RECT`（8 字节）里写 32 字节的 `D3DSURFACE_DESC` —— 踩掉调用者栈帧。
这就是那些 NULL `pBits`、`structured exception`、以及"跳到 0x16"的全部来源。
**修正已在 `src/d3d9_min.h` 落地**（`Reserved11` 那条注释），用
`build\\_copytest\\copyprobe.exe` 逐槽位实测确认。

### 2. MT Framework 不允许在帧内创建 D3D9 纹理

帧内 `CreateTexture` 会让引擎以 `ERR09 : Unsupported function.` 直接致命退出。
所有表面必须在**第一帧渲染之前**建好：`CreateDevice` 时排队，OpenXR 就绪那一刻创建。

### 3. 后台缓冲不能作为读回源

`StretchRect(backbuffer → texture)` 返回 `D3DERR_INVALIDCALL`，
`LockRect(backbuffer)` 抛异常，`GetRenderTargetData(backbuffer → sysmem)` 抛异常。
可用路径是：**同尺寸同格式的 `StretchRect` → `GetRenderTargetData` → `LockRect`**，
已实测可用（`probe: GetRenderTargetData ... WORKS`）。

### 4. 绝不能包装 `IDirect3D9Ex`

它比 `IDirect3D9` 多 4 个槽，只转发 17 槽的包装对象会让 Ex 专有调用跳进未初始化内存。
现在 Ex 工厂直接透传，真实设备指针原样交给游戏，只补丁设备**自己那份 vtable 副本**的
`Present` / `EndScene`。

### 5. 会聚点必须在屏幕前方

`ipd_scale=0` 时若把会聚点放在屏幕**平面上**，面板视空间深度为 0，
投影要描述"距离 0 处的 1.45m 半宽"（≈88° 半角），frustum 表达不了 → quad 落到视口外 → 全黑。
现在会聚点在屏幕前方 `eye_setback = 1.6m`（即玩家位置）。

### 6. MinHook 已移除

它的 trampoline 在第一次 `Present` 就崩（fault offset 0x4C89008B）。
改为直接补丁设备 vtable 副本。

## ⚠️ 血泪教训

1. **`place_panel()` 的调用点被脚本改写删掉过三次**，每次症状都是全黑
   （`panel_center` 停在 `(0,0,0)` → 投影退化）。改完务必确认这行还在：
   ```cpp
   if (!panel_placed) place_panel(real_eye, fwd);
   ```
2. **不要用"字符串切片"批量改写 C++**（`t[s:e]` 那种）。用行号精确替换，改完立刻读回来确认。
3. **渲染结果必须实测**，不能靠读代码推断。用 `RE6VR_SELFTEST=1` 或 `RE6VR_DUMP_FRAME`。
4. **"数学正确"≠"结果正确"**。缩放式 lerp 的收敛代码看着没问题却完全没生效，
   把 `real`/`used` 打出来才发现。关键路径要打日志比对。
5. **PowerShell 写含中文的文件会破坏编码**。用 Python + `encoding="utf-8"` 写。

## 已知遗留

- `RE6VR_SELFTEST` 的 6 个探测点像素换算仍有 bug（`outside` 采样落在屏幕内、
  `BL` 落在屏幕外，自相矛盾），所以总报 FAIL。**面板范围**与**四象限方向**两项结论可信。
- OpenXR 首次初始化会阻塞游戏线程一次（runtime 冷启动实测约 12 秒，已延后 1.2 秒但仍会阻塞），
  期间游戏画面静止。
- **Stage 2（真双视角立体渲染）与 Stage 3（头部瞄准 / 姿态注入）尚未开始。**

"""

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(header + body)

print("README rewritten: %d bytes" % len(header + body))
