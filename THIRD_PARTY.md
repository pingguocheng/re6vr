# 第三方组件（THIRD PARTY）

本仓库**只包含本项目的源码、脚本与文档，不含任何第三方二进制**。
构建时需要的依赖由 `scripts\stage_deps.py` 从官方发布页下载到 `_third_party\`（该目录被 `.gitignore` 排除）。

| 组件 | 用途 | 许可证 | 来源 |
|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 | 内联 hook（相机函数、设备 vtable） | BSD-2-Clause | GitHub Releases |
| [OpenXR SDK / loader](https://github.com/KhronosGroup/OpenXR-SDK-Source) 1.1.63 | OpenXR 头文件与 `openxr_loader.dll`（x86） | Apache-2.0 | GitHub Releases |
| Microsoft Direct3D 9 / DXGI | 系统 API，头文件与运行库随 Windows SDK 提供 | — | 系统 |

本仓库**不包含任何 Capcom 的游戏资源**（无模型、贴图、音频、数据表）。
仓库中的文档确实包含对 `BH6.exe` 的静态分析结论（函数地址、结构体偏移、字段语义），
这是互操作性/模组开发所需的信息，属于本项目的原创逆向成果。

使用者需要自备正版《生化危机 6》。本项目与 Capcom 无任何关联，也未获其授权或认可。
