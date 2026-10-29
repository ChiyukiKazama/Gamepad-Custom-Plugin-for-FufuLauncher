# 手柄自定义插件（GamepadCustomPlugin）

用于[芙芙启动器](https://github.com/FufuLauncher/FufuLauncher)的原神 Xbox / DualSense（PS5）手柄自定义插件，插件文件为 `GamepadCustomPlugin.dll`。

当前版本：**1.1.0**。

## 功能

- 将打开地图绑定到 Xbox 或 DualSense 单个按键，不模拟键盘输入；
- 根据已连接的手柄类型，自动切换配置页下拉框中候选的键位库；
- 将手柄镜头灵敏度乘以自定义系数，键鼠灵敏度保持原样；
- 地图开关、地图按键和灵敏度系数支持配置热重载。

## 安装与更新

1. 完全退出游戏，确认启动器的主插件已安装并启用。
2. 将发布包解压到启动器的 `Plugins/GamepadCustomPlugin` 目录，DLL 与 `config.ini` 放在同一层。不建立重复插件目录，也不覆盖主插件文件。
3. 在启动器的插件页面启用本插件，进入其设置页面修改参数，再启动游戏。

更新 DLL 必须重启游戏，运行中仅支持热重载快捷键和镜头灵敏度系数。升级时保留原有地图开关、按键和灵敏度值，不直接用发布包默认配置覆盖个人设置。

从旧 `XboxGamepadPlugin` 升级时，完全退出游戏，用新 DLL 替换旧 `XboxGamepadPlugin.dll`，并将原配置 `[General]` 的 `Name` 改为 `手柄自定义插件`、`File` 改为 `GamepadCustomPlugin.dll`。功能节名及个人 `Value` 保持原样；旧目录可以继续使用，也可改名为 `GamepadCustomPlugin`。确保启动器的插件目录内只保留一份本插件，避免同时加载两个版本。

## 设置

| 配置节 | 默认值 | 作用 | 热重载 |
| --- | --- | --- | --- |
| `MapBinding` | `1` | 启用自定义地图快捷键 | 支持 |
| `MapButton` | `View` | 自定义地图快捷键键位 | 支持 |
| `GamepadSensitivityMultiplier` | `1.0` | 手柄专用镜头灵敏度系数 | 支持 |
| `DualSenseEdgeView` | `1` | 把 DualSense Edge 呈现为标准 DualSense | 启动时读取 |

在启动器设置页修改后自动保存。三项设置会在文件保存完成后自动应用；手动编辑同目录 `config.ini` 也适用。`DualSenseEdgeView` 只在游戏启动时读取，改动后需重启游戏。改绑或切换地图开关时，已按住的键需先松开再按，避免误触。

地图按键使用通用配置协议 `Type = enum`、`OptionsSource = gamepad`，需要支持该协议的启动器。Xbox 连接时显示 Xbox 按键名，标准 DualSense 连接时显示 DS 按键名；两者同时可见或都未连接时显示合并名称，插拔后刷新。换库保留已有绑定，只有主动选择才保存。旧配置只需修改这两个元数据字段，保留原 Value；旧启动器仍可用文本输入编辑按键名称。

### DualSense 连接

适配接入游戏的原生 DualSense 驱动与 Windows HID 路径；游戏需先识别手柄。匹配 PS5 设备 `VID 054C / PID 0CE6`，以及 `PID 0DF2` 的 DualSense Edge，不包含 DS4 适配。只按产品 ID 选择 DualSense 支持的宿主会忽略 Edge，它的陀螺仪校准、触觉与自适应扳机因此不会初始化；`DualSenseEdgeView` 打开时，本进程内的 `HidD_GetAttributes` 与产品名会把 Edge 呈现为标准 DualSense，使该宿主创建完整的 DualSense 驱动；标准 DualSense、DS4 与其他设备保持原身份。原生驱动支持 USB 与蓝牙报告布局；Windows HID 的 USB 输入在游戏读取到的报告副本中转换绑定按键，使后续原始报告读取和 HID 解码看到同一状态。蓝牙报告仍沿用解码适配，不改写其校验码；该路径的改绑效果需在实际蓝牙设备上确认。灵敏度复用手柄模式，多个手柄的按键状态独立。设备描述按手柄缓存，连续输入不会重新查询或解析描述。

身份呈现只作用于本进程的 `hid.dll` 查询，不改动设备本身：其他进程、设备管理器与 Raw Input 设备路径仍显示物理设备的 `0DF2` 身份。报告内容、摇杆与陀螺仪数据、震动与自适应扳机输出保持原样，只有身份字段不同。

通过 DS4Windows / Steam 等转换为虚拟 Xbox 控制器时，沿用原有 XInput 路径；虚拟 XInput 不提供独立触摸板按键。插件跟随游戏已有的输入读取，不注册额外 HID 接收窗口或向设备发送输入。USB 转换仅改变游戏报告副本中的绑定按键和中转按键，保留摇杆、陀螺仪、触摸坐标和厂商字段；震动与自适应扳机输出保持原样。

候选库以 Windows 当前识别到的设备为准；系统隐藏的物理手柄无法用于识别。候选刷新不代表游戏已使用该输入后端，也不会自动改写地图绑定。使用前请在游戏设置中选择手柄控制设备。

### 地图按键

支持 `View`、`Menu`、`A`、`B`、`X`、`Y`、`LB`、`RB`、`LS`、`RS`、`Up`、`Down`、`Left`、`Right`、`LT`、`RT`，不区分大小写。

`LS`/`RS` 表示摇杆按下；方向名称表示十字键。兼容 `Back` = `View`、`Start` = `Menu` 和 `DPadUp/Down/Left/Right`。`None`、空值或无效名称表示不绑定，不回退到默认键。Xbox Logo 键、独立背键、组合键和摇杆方向不支持。绑定按键在触发地图时会占用其原有动作，地图是否能打开仍遵循游戏本身的限制。

DualSense 名称也可用于配置，不区分大小写。`X` 仍表示 Xbox X / PS Square；PS 的叉键请填写 `Cross`。

| DualSense 名称 | 现有名称 |
| --- | --- |
| `Create` / `Share` | `View` / `Back` |
| `Options` | `Menu` / `Start` |
| `Cross` / `Circle` / `Square` / `Triangle` | `A` / `B` / `X` / `Y` |
| `L1` / `R1`、`L2` / `R2`、`L3` / `R3` | `LB` / `RB`、`LT` / `RT`、`LS` / `RS` |
| `Touchpad` | DualSense 触摸板按下，原生驱动与 Windows HID 解码路径支持 |

在 `[MapButton]` 节设置 `Value = Touchpad` 可将触摸板按下作为地图快捷键，`Value = R3` 则绑定右摇杆按下。地图中转使用默认聊天按键动作：Xbox 使用 View，DualSense 使用 Touchpad。绑定其他按键时只占用该按键的原动作；未绑定的 Touchpad 仍按游戏原来的聊天键处理。游戏内自定义聊天按键映射需另行核对。

### 灵敏度系数

`1.0` 为原速，`0.5` 为半速，`2.0` 为两倍，必须是正的有限小数。仅改变手柄镜头的原生横纵轴灵敏度，不改变死区、角色移动、菜单滚动或游戏保存的灵敏度设置。

启动时配置读取失败，则保持地图绑定关闭、灵敏度系数为 `1.0`，直到收到有效保存；热重载时配置不完整、文件正在写入或系数无效，则保留上一份有效配置。不要重命名配置节或删除三个热重载参数。

## 性能与诊断

配置更新使用 Windows [文件变更通知](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)，合并保存事件后读取一次，不增加配置轮询或线程。插件只处理游戏已有的手柄输入读取，不主动轮询手柄状态。地图入口初始化一次，灵敏度 Hook 按需初始化一次，后续配置更新不重复扫描或拆装 Hook。

文件通知不可用时，重启游戏应用配置，不设置轮询兜底。正式默认配置不启用输入诊断，也不写插件日志。排障配置示例保留在源码的 `diagnostics.example.ini`，不进入发布包；诊断开关只在游戏启动时读取。

## 构建与发布

工程文件为 `GamepadCustomPlugin.vcxproj`。`src/XInputBackend.cpp/.h` 负责 Xbox 及虚拟 Xbox 的 XInput 接入，`src/DualSenseBackend.cpp/.h` 负责原生 DualSense 驱动接入，`src/DualSenseHidBackend.cpp/.h` 负责 PS5 的 Windows HID 报告副本及解码接入，`src/DualSenseIdentity.cpp/.h` 负责把 DualSense Edge 的身份呈现为标准 DualSense。`DualSenseState.h` 负责通用状态转换，`DualSenseHidState.h` 负责解码数据与按键路由之间的转换。入口、配置、地图、灵敏度与按键路由使用通用文件名。

需要 Windows、PowerShell 7、Visual Studio 2026 C++ 构建工具、Windows SDK、`v145` 工具集，以及相邻的 `FufuLauncher.UnlockerIsland` 源码。通过 `vswhere` 查找 MSBuild，只读取主插件的 Patterns 定义和 MinHook 库，不重新编译或修改主插件。

在源码目录双击 `publish.cmd`，或运行：

```powershell
.\publish.ps1
```

此命令仅在本地构建、压缩、校验和打包，不上传 GitHub 或商城。包生成到 `dist/GamepadCustomPlugin-<版本号>.zip`，ZIP 根目录只有 `GamepadCustomPlugin.dll` 和仓库默认 `config.ini`。DLL 使用官方 UPX 5.2.0 的 `--best --ultra-brute` 压缩，下载附件先核对固定 SHA256，再执行完整性测试、解压与 x64 PE/版本检查、独立非游戏宿主加载检查并确认体积减小。解压后的 DLL 不要求与原文件哈希相同。此检查不代表游戏内功能验收。

源码基础版本为 `1.1.0`，`config.ini` 与 `store/metadata.json` 的版本必须一致；商城名称、简介和开发者展示名可独立维护，不要求与插件设置页文案相同。`-Version 1.2.3` 可指定本次产物版本；只生成 DLL 版本资源、配置和商城信息，不回写源码，不自动提交或递增版本。DLL 的 FileVersion / ProductVersion 文本均为三段；PE 固定版本结构保留的第四个 WORD 恒为零，不参与版本名、tag 或包名。商城 ID 保留 `XboxGamepadPlugin`，安装脚本仍只调用 `install.download_plugin("XboxGamepadPlugin")`。

输出目录还包含商城信息、安装脚本、SHA256 和压缩报告，均在 ZIP 外。脚本拒绝覆盖非空输出目录或复用已有压缩目录；再次构建时指定新的 `-OutputDirectory` 与 `-CompressionDirectory`。不读取本地部署配置，也不修改任何用户安装目录。仅编译请运行 `build.cmd` 或 `build.ps1`。完整指引见 [BUILDING.md](BUILDING.md)。

### GitHub Actions

- 发布：进入 **Actions → Build and release → Run workflow**，选择默认分支 `master`，填写必需的 `version`，例如 `1.2.3`。
- 校验全部通过后，创建或复用 `v1.2.3`，Release 标题为 `GamepadCustomPlugin 1.2.3`；先创建草稿，只上传本次构建的成品 ZIP，确认上传状态、大小和 SHA256 后公开。商城信息、脚本、哈希和报告仅留在 Actions artifact 中，不自动发布到商城。
- 已公开的同版本直接跳过；发现同版本未完成草稿则停止，需维护者手动处理。已有 tag 即使指向其他提交也会直接复用，保留原指向；只有 tag 不存在时才以本次构建提交创建。复用旧 tag 时，GitHub 自动生成的源码归档对应原 tag，ZIP 对应本次构建提交；发布前仍会确认 tag 的目标提交未发生变化。

## 许可

AGPL-3.0，详见 `LICENSE`；MinHook 的许可见 `THIRD_PARTY_NOTICES.md`。发布时同时提供对应版本源码，保留许可证和第三方声明。游戏文件仅用于本地分析，不分发。
