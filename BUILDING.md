# 构建与发布指引

## 环境

- Windows；Visual Studio 2026 C++ Build Tools，安装 C++ 桌面开发组件、`v145` 工具集与 Windows SDK。
- PowerShell 7（`pwsh.exe`）；构建与打包脚本不再使用 Windows PowerShell 5.1。
- 默认源码布局：手柄插件仓库与 `FufuLauncher.UnlockerIsland` 放在同一父目录。工程文件为 `GamepadCustomPlugin.vcxproj`；本机仓库目录仍为 Codex 已绑定的 `C:\Repos\XboxGamepadPlugin`，目录名称不参与构建。

构建只读取主插件仓库内的 `Patterns/Patterns.h`、`MinHook/MinHook.h` 和 `MinHook/libMinHook.x64.lib`；不要求先编译主插件，也不会修改它。`.tools` 中的本地分析工具、真实游戏文件和旧部署文件均不是构建依赖。

## 修改文本

- `config.ini`：插件名称、描述、开发者、版本号、设置标题和默认值。可修改 `[General]` 的展示内容及各设置的 `Name`；保留节名、`File`、`Type` 及三个热重载参数。使用 UTF-8 保存。
- `README.md`：开源仓库中的使用说明。
- `src/`：运行实现与日志文本，仅在需要修改代码时编辑。
- `docs/input-backends-2026-10-14.md`：当前输入路径及验证记录，不进入发布包。
- `diagnostics.example.ini`：开发排障示例，不进入发布包；不要把两个诊断节加回正式 `config.ini`。

长期更改基础版本时同步修改 `config.ini`、`store/metadata.json` 和 `README.md`。构建时通过 `-Version x.y.z` 临时指定本次产物版本，省略时使用一致的源码基础版本。版本资源和配置写入中间目录，再复制到构建输出，不修改 `src/` 或根目录配置。打包名为 `GamepadCustomPlugin-x.y.z.zip`，商城信息与包内配置使用同一版本；不自动增加第四段、时间戳或提交哈希。

商城的 `name`、`description`、`developer` 展示文本可以与 `config.ini` 不同；调整商城文案无需同步改插件设置页。技术字段仍严格校验：商城 ID、DLL 文件名、基础版本及 ZIP 文件名。生成产物会保留商城原有文案，只替换本次版本和对应包名。

`MapButton` 的 `Type = enum` 和 `OptionsSource = gamepad` 是启动器通用候选协议。原生插件只读取 Value，因此不影响原来的别名与热重载；升级旧配置时保留个人 Value，只更新这两个元数据字段。不要把手柄按键改成保存键盘虚拟键码的 Type=key。

## 手动构建发布包

保存修改后，双击项目根目录的 **`publish.cmd`**。它执行 Release x64 构建、UPX 压缩及正式打包校验；失败时停止，不会上传或修改游戏安装目录。控制台保留结果，便于查看错误。首次压缩需要联网下载经过固定 SHA256 校验的官方 UPX 5.2.0。

也可在 PowerShell 中运行：

```powershell
Set-Location 'C:\Repos\XboxGamepadPlugin'
.\publish.ps1
```

`publish.cmd` 与 `build.cmd` 调用 PowerShell 7，不更改系统或用户执行策略。

脚本自动使用 Visual Studio 安装查询工具寻找 C++ 构建环境。无法找到时明确指定：

```powershell
.\publish.ps1 -MSBuildPath 'C:\VSTool\VSBuildTools2026\MSBuild\Current\Bin\MSBuild.exe'
```

主插件源码在其他位置时再加：

```powershell
.\publish.ps1 -UnlockerIslandRoot 'D:\Repos\FufuLauncher.UnlockerIsland'
```

只编译、不打包：双击 **`build.cmd`**，或运行 `.\build.ps1`。两个 `.cmd` 也允许传入上述参数。

## 产物

- `bin/x64/Release/GamepadCustomPlugin.dll`：编译的 DLL，旁边的配置来自项目根目录。
- `obj/x64/Release/`：编译中间文件及生成的版本资源和配置。
- `obj/release/`：UPX、压缩 DLL、解压检查副本和压缩报告。
- `dist/GamepadCustomPlugin-<版本号>.zip`：可上传的发布包。
- `dist/metadata.json`、`install.lua`、`package-hashes.json`、`compression.json`：商城填写材料和校验报告，不进入 ZIP 或 GitHub Release 附件。

脚本拒绝覆盖非空输出目录和复用已有压缩目录。重复打包可指定新目录，例如：

```powershell
.\publish.ps1 -Version 1.2.3 -OutputDirectory .\dist\1.2.3 -CompressionDirectory .\obj\release-1.2.3
```

CI 将构建、压缩和打包分为独立步骤，通过 `publish.ps1 -DllPath .\obj\release\GamepadCustomPlugin.dll` 打包已校验的压缩 DLL；要求旁边存在匹配的 `compression.json`。普通本地调用不需要传 `-DllPath`。

ZIP 根目录严格只包含：`GamepadCustomPlugin.dll`、`config.ini`。使用说明、`LICENSE` 和 `THIRD_PARTY_NOTICES.md` 保留在开源仓库，发布时提供对应版本源码及许可。正式配置里的新增节会随包发布，诊断节请留在 `diagnostics.example.ini` 中。

`bin/`、`obj/`、`dist/`、`.ci-dependencies/`、`.tools/`、根目录 `tests/` 和 `docs/` 等已在 `.gitignore` 中排除。`scripts/`、`store/` 和工作流需要提交；`scripts/tests/` 的离线发布状态测试随源码保留。`bin/backups/` 中旧文件保持可恢复，不作为发布输入。不要把 PDB、测试程序、日志或游戏文件手动加进发布包。

## 发布前验收

发布流程验证编译、压缩、x64 PE DLL 及版本资源、默认配置、ZIP 成员和各项 SHA256；Actions 还执行离线发布状态测试。这些检查不证明游戏内 Hook 和可见效果。加载检查在独立非游戏 PowerShell 子进程进行：当前 Worker 在安装 Hook 前返回，配置监视器析构时取消通知；DLL 保持加载直到进程退出，不调用不受支持的实时 FreeLibrary。更新 DLL 后须完全重启游戏，再验证：

1. 地图开关、按键在运行中修改后生效，按住旧绑定键时改绑不误触。
2. 从启动系数 `1.0` 热改为 `2.0`，再改回 `1.0`；手柄速度正确，键鼠不受影响。
3. 修改游戏内灵敏度、保存设置及进出菜单后，系数不叠乘、不污染原设置。
4. 正式配置仅显示地图开关、地图按键与灵敏度系数，不显示两个诊断项。

发布对应版本源码与许可证。用户安装到启动器的 `Plugins/GamepadCustomPlugin`；从旧 DLL 升级时，替换旧 `XboxGamepadPlugin.dll`，更新 `[General].Name` 与 `[General].File`，保留个人配置，避免两份插件同时加载。构建/打包入口不注入、不启动或关闭游戏，也不自动部署。

GitHub 自动构建与手动发布入口、草稿处理和权限说明见 [README.md](README.md#github-actions)。发布失败留下的草稿由维护者检查处理，后续运行不会自动删除或覆盖。更新主插件构建依赖时，需明确调整工作流中的固定提交并重新验证 Patterns 与 MinHook 兼容性；无需获取或分发游戏文件。
