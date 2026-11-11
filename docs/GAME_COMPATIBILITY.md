# Windows 游戏输入兼容性

## AIMouse MCP 实测

在 Windows 11 Home 22631 的 Release x64 构建中，Desktop 路径让后台 Win32 探针实际接收移动、点击、双击、拖拽、滚轮、按键和字符。独立的第二探针收到零条同类事件。Windows Graphics Capture 返回 642 × 399 的窗口 PNG。系统光标、前台窗口以及抽查的物理键鼠状态在各采样点一致。

向 Raw Input FPS 发送 Desktop 消息后，探针收到传统移动和点击，相对轴和相机保持原值。切换到 Game 模式后，该探针因缺少协作协议而拒绝 Arm。协作 FPS 在后台收到 `dx=100, dy=-25` 后，yaw 为 15°，pitch 为 3.75°，并记录了五步拖拽、滚轮与持键。MCP 断连后收到 reset，逻辑按键清零。 [端到端结果](../analysis/evidence/aimouse-mcp-e2e.json)

## 接收端实验

测试环境为 Windows 11 Home x64，构建 22631。自建探针采用 MSVC 19.44、Windows SDK 10.0.26100.0 和 Release x64 构建，SDL 版本为 3.2.28，GLFW 版本为 3.4。探针默认以后台窗口运行，发送器仅向当前启动的探针进程发送窗口消息，发送前校验 HWND 所属 PID。

发送序列包含两次绝对移动、一次鼠标按下和释放、一次滚轮、一次 W 键按下和释放。第二次移动携带左键按下标志。表中按游戏真正读取的输入通道判断结果。窗口过程返回成功只记录为消息已发送，接收端计数和状态变化用来判断实际响应。详细计数、窗口模式、二进制哈希和输入采样见[接收端矩阵](../analysis/evidence/input-receiver-matrix.json)与[另一轮采样](../analysis/evidence/input-receiver-matrix-active.json)。

| 程序和运行方式 | 输入通道 | 移动 | 点击 | 拖拽 | 键盘 | 后台控制 | 用户输入隔离 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Win32 探针，窗口化和无边框 | Win32 消息 | PASS | PASS | PASS | PASS | PASS | PASS，限此探针 |
| Raw Input FPS，前台注册，实际失焦 | Raw Input | FAIL | FAIL | FAIL | FAIL | FAIL | 未通过此通道控制 |
| Raw Input FPS，后台接收注册 | Raw Input | FAIL | FAIL | FAIL | FAIL | FAIL | 未通过此通道控制 |
| DirectInput 8 探针，前台协作，实际失焦 | DirectInput | FAIL | FAIL | FAIL | FAIL | FAIL | 未获取设备 |
| DirectInput 8 探针，后台非独占 | DirectInput | FAIL | FAIL | FAIL | FAIL | FAIL | 已获取设备，窗口消息无法驱动设备 |
| SDL 3 Direct3D 11，窗口化和无边框 | SDL 绝对位置事件 | PARTIAL | PASS | FAIL | PASS | PARTIAL | PARTIAL，游戏坐标与拖拽状态存在差异 |
| GLFW OpenGL，窗口化和无边框 | GLFW 绝对位置回调 | PARTIAL | PASS | PASS | PASS | PARTIAL | PARTIAL，目标在按下期间获取 Windows 鼠标捕获 |
| SDL 3 相对模式、GLFW 禁用光标及 Raw Motion | 引擎相对输入 | NOT TESTED | NOT TESTED | NOT TESTED | NOT TESTED | NOT TESTED | NOT TESTED |
| Unity、Unreal Engine、SDL 2、自有授权游戏 | 依程序而定 | NOT TESTED | NOT TESTED | NOT TESTED | NOT TESTED | NOT TESTED | NOT TESTED |

- **Win32** 两种窗口模式各收到 2 次移动、1 次按下、1 次释放、1 次拖拽移动、1 次滚轮及完整 W 键按下和释放。窗口过程记录了目标客户区坐标，滚轮携带屏幕坐标。按下后探针线程的捕获 HWND 为 0。首次桌面静止测试的光标和前台采样保持一致。
- **Raw Input** 两种注册方式的传统消息计数都增加。首次桌面静止测试中，两种注册方式均为 0 个 Raw Input 鼠标和键盘事件，Yaw / Pitch 都是 0。外部桌面输入活动期间，后台注册样例收到来自同一个设备句柄的 148 个 `RIM_INPUTSINK` 鼠标包，Yaw / Pitch 变为 26.25° / 5.70°。因此，程序能用真实原始数据旋转视角。外部鼠标活动与本次窗口消息发送同时存在，这轮不能单凭最终计数归因。前台注册探针实际失焦，Raw Input 计数仍为 0。[Microsoft Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input)
- **DirectInput** 前台协作的样例保持失焦，鼠标和键盘都未获取。后台非独占样例成功获取两者，桌面静止时设备记录均为 0。另一次桌面鼠标活动期间读取到 205 条鼠标设备记录，验证后台观察路径可用。DirectInput 的设备获取和窗口消息计数分开记录。[Microsoft DirectInput 协作模式](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417921(v=vs.85))
- **SDL 3** 在两种窗口模式中都观察到 SDL 鼠标、滚轮和键盘回调，拖拽移动为 0。最后的 SDL 位置在不同轮次落到不同客户区边缘，未保持发送的终点 170、130。源码显示其 Windows 后端在部分鼠标路径根据系统状态修正按钮和位置，这些处理可以解释差异，单个分支的运行轨迹仍未跟踪。[SDL 3.2.28 Windows 后端](https://github.com/libsdl-org/SDL/blob/release-3.2.28/src/video/windows/SDL_windowsevents.c#L261)
- **GLFW** 在两种窗口模式中均收到 2 次位置回调、1 次拖拽移动、滚轮和 W 键回调。发送的客户区终点为 170、130，测得回调终点为 213、163，具体 DPI 转换路径仍待单独核对。按下后查询到捕获 HWND 指向探针，释放后归零。探针未成为采样中的前台窗口，捕获仍可能影响真实鼠标路由。[GLFW 3.4 Windows 后端](https://github.com/glfw/glfw/blob/3.4/src/win32_window.c#L742)

桌面活动轮次出现用户桌面坐标和其他前台窗口的变化，各探针保持后台。稳定采样只能说明采样点之间的观测条件，不能证明任意目标应用都没有短时副作用。GLFW 在鼠标按住期间取得的捕获就是一个独立于前台焦点的副作用。

## 合作式游戏协议

`aimouse_input_probe.exe cooperative` 提供可运行的第一人称样例。它使用一个明确接入的 `WM_COPYDATA` 协议处理相对位移、按钮状态、滚轮和按键。游戏代码应用相对位移后更新 Yaw / Pitch，持续按下 WASD 时积分角色移动。只有成功应用的帧才回复 ACK。样例独立记录合作协议和真实 Raw Input 的计数，适用范围限于实现该协议的应用。

样例在持有按钮或按键期间检查发送方的窗口、进程和线程。强制终止专用测试发送进程前，样例确认左键与 W 键均按下。发送窗口消失后，样例记录 1 次失联恢复，最终两个按下计数均为 0。此流程不依赖发送方退出时发送释放指令。

## 构建和复现

项目的 `BUILD_TESTING=ON` 构建 Win32、Raw Input、DirectInput 和合作式 FPS 共用的 `aimouse_input_probe.exe`。SDL 和 GLFW 探针可通过 `AIMOUSE_BUILD_ENGINE_PROBES=ON` 启用。为 SDL 指定包含 `SDL3Config.cmake` 的 `SDL3_DIR`。对于 GLFW，可使用已安装的 CMake 包，或者将 `AIMOUSE_GLFW_ROOT` 指向官方 Windows x64 压缩包的解压目录。CMake 将需要的运行时 DLL 复制到探针输出目录。[SDL 3.2.28](https://github.com/libsdl-org/SDL/releases/tag/release-3.2.28) · [GLFW 3.4](https://github.com/glfw/glfw/releases/tag/3.4)

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON -DAIMOUSE_BUILD_ENGINE_PROBES=ON -DSDL3_DIR=.deps/SDL3-3.2.28/cmake -DAIMOUSE_GLFW_ROOT=.deps/glfw-3.4.bin.WIN64
cmake --build build --config Release
pwsh -File tests/probes/measure.ps1 -ProbeDirectory build/tests/probes/Release -IncludeEngineProbes
```

- `aimouse_input_probe.exe raw --input-sink` 观察后台鼠标和键盘数据，输出设备数、相对轴、Yaw / Pitch 和角色位置。交互式前台样例使用 `raw --foreground`。添加 `--lock-cursor` 后，窗口获得焦点时限制并隐藏系统光标，失焦或退出时释放限制。
- `aimouse_input_probe.exe directinput --background` 使用后台非独占协作。显式交互测试可用 `directinput --foreground --exclusive`，从输出检查设备获取、轴增量、按键状态和缓冲区溢出。独占模式会改变鼠标捕获或可见性，测试完成可用 Esc 或 `--timeout-ms` 退出。
- `aimouse_sdl_probe.exe --foreground --relative` 和 `aimouse_glfw_probe.exe --foreground --relative` 用于交互式相对输入。两个探针支持 `--borderless`。SDL 的 `--foreground --capture` 允许库在鼠标按住时自动捕获，GLFW 的同名开关测试光标限制模式。
- 原生探针默认使用非激活工具窗口。`win32 --capturable` 创建普通顶层窗口并以非激活方式显示，可用于 Windows Graphics Capture 验证。普通顶层窗口仍允许用户主动点进来。

独占全屏、引擎相对模式、设备轮询率、实际游戏、不同 Windows 用户会话和虚拟机内游戏输入均尚未完成端到端验证。对需要独占前台或设备级输入的游戏，当前共享桌面测试只有上述限制结论。
