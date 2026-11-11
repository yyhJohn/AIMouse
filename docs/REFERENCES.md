# MouseMux 与 Windows 输入参考

核对日期为 2026-11-11。方案给出的 GitHub 链接指向 REA。另选 MouseMux 官方 V2 SDK 和 openMouse 的固定提交作比较。以下记录来自厂商公开说明、开源示例和 Windows API 文档。MouseMux V3 本机二进制证据见 [逆向记录](REVERSE_ENGINEERING.md)。

## MouseMux V3 厂商说明

| 功能 | 手册描述 | 对 AIMouse 的设计意义 |
|---|---|---|
| Switched | 每用户有自己的位置。按下鼠标按钮的用户担任 root，其他用户的应用点击等待当前操作结束。 | 轮流获得主光标会改变人的系统输入，超出桌面模式的输入隔离目标。 |
| Multiplex | 用户点击应用以开始向该窗口路由，厂商描述支持在不同兼容窗口同时点击、拖动和悬停。 | 明确绑定窗口，独立跟踪坐标、按钮状态和拖动目标。每种应用的接收效果需要实测。 |
| Force root operation | 绕过 Switched 仲裁或 Multiplex 窗口路由，走系统输入路径并移动同一系统光标。 | 游戏兼容路径需要显示其全局影响，且由用户单独授权。 |
| Force hardware motion / dragging | 前者把所有移动送到系统输入层。后者在按钮按住时使用该路径。 | 手册给出输出策略。具体 API、设备标识、驱动参与及 Raw Input 接收仍需静态或运行时证据。 |
| Enable delta motion | 外部应用改变 Windows 光标位置后，root 光标重新同步。 | 这是跟踪外部重新定位的功能。游戏接收相对增量需要独立验收。 |
| Force hardware keyboard | 常规多键盘路由向窗口发消息。硬件选项经系统输入栈进入前台窗口，此时失去窗口和修饰键隔离。 | 窗口文本消息、窗口按键消息、系统键状态和硬件采样须独立报告。 |
| Thread Attach | 可关联目标输入队列以处理私有修饰键，手册同时记录可能导致慢应用停顿并提供关闭选项。 | 给 HWND 发消息与共享输入队列是不同机制。隔离模式应测量前台和键盘状态。 |
| Replicator / Input Mapper | Replicator 可创建新的虚拟用户。Input Mapper 提供默认关闭的本机 MCP 连接和独立的 Arm 开关。 | 用户生命周期、MCP 连接和输入授权可以独立建模。虚拟用户名称或 SDK ID 本身无法证明 Windows 存在新的 HID 枚举项。 |

资料来源为 [MouseMux V3 手册](https://www.mousemux.com/pages/manual/)、[Input Mapper](https://www.mousemux.com/pages/apps/webapp-input-mapper-beta/) 和 [MouseMux SDK 入口](https://www.mousemux.com/pages/sdk/)。[Web SDK 手册](https://www.mousemux.com/pages/sdk-web/) 仍列出 V2 运行要求。其客户端虚拟用户示例支持事件消费和绘制，客户端 ID 的实际系统含义需要服务端证据。

## GitHub 源码核对

- **MouseMux 官方 V2 SDK，MIT**。示例注册 `mousemux.api.*` 自定义 Windows 消息，由 `mousemux.api.target` 的窗口过程分发。移动与按钮处理从参数解出用户 ID 和坐标，按用户维护绘图状态并将屏幕坐标换算为客户区坐标。键盘处理函数是空体，水平滚轮的消息表项调用了垂直滚轮处理函数。可参考其中的接收端分离和坐标处理。示例覆盖配合 SDK 的程序，V3 商业引擎及通用窗口输出属于另外的分析对象。 [消息定义](https://github.com/MouseMux/WinSDK---Native-Windows-SDK/blob/51d0722dbd6af1cc405a4507932c928835fd88b2/main.c#L43-L93) · [分发与窗口](https://github.com/MouseMux/WinSDK---Native-Windows-SDK/blob/51d0722dbd6af1cc405a4507932c928835fd88b2/main.c#L229-L332) · [许可证](https://github.com/MouseMux/WinSDK---Native-Windows-SDK/blob/51d0722dbd6af1cc405a4507932c928835fd88b2/LICENSE)
- **openMouse，MIT**。`Seat` 保存虚拟桌面坐标、最近点击窗口、五个按钮的状态和点击时间。覆盖窗口使用透明的分层窗口样式和 `WS_EX_NOACTIVATE`，用 `UpdateLayeredWindow` 绘制，适合作为独立渲染的参考。其系统输入策略通过 `WH_MOUSE_LL` 抑制物理输入，由 `WM_INPUT` 标识设备，使用 `SetCursorPos` 或光标限制控制系统光标，并通过 `SendInput` 投递。前台操作还涉及 `AttachThreadInput` 与 `SetForegroundWindow`。这些系统输入机制会接管用户鼠标，违反 AIMouse 桌面模式的验收要求。 [用户状态](https://github.com/alstonmendonca/openMouse/blob/dee7810ec23aac5de13c544a57e9e519b176062c/src/openmouse.h#L44-L71) · [渲染](https://github.com/alstonmendonca/openMouse/blob/dee7810ec23aac5de13c544a57e9e519b176062c/src/overlay.cpp#L181-L235) · [系统输出](https://github.com/alstonmendonca/openMouse/blob/dee7810ec23aac5de13c544a57e9e519b176062c/src/main.cpp#L190-L309) · [捕获](https://github.com/alstonmendonca/openMouse/blob/dee7810ec23aac5de13c544a57e9e519b176062c/src/main.cpp#L707-L861) · [许可证](https://github.com/alstonmendonca/openMouse/blob/dee7810ec23aac5de13c544a57e9e519b176062c/LICENSE)
- **REA**。方案指定的开源逆向工具提供 Windows 原生 PE 的 Ghidra 分析能力。实际可用操作应以当前 REA 会话及绑定提供者的工具报告为准。 [REA](https://github.com/morluto/rea)

## Windows API 与游戏实验

- **Raw Input**。应用先注册设备集合，再从 `WM_INPUT` 读取设备句柄和 `RAWMOUSE` 等数据。`RIDEV_INPUTSINK` 扩展的是接收条件。兼容测试需要在目标内记录 `GetRawInputData` 返回的相对轴、设备和前后台标记。可见指针移动与普通窗口消息均需单独计数。 [Microsoft Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input)
- **DirectInput**。鼠标和键盘都受合作级别及获取状态约束。应用失焦时前台合作级别自动失去设备获取状态。鼠标和键盘不支持后台独占组合。分别用前台非独占、后台非独占和前台独占读取相对轴及按键状态。 [Microsoft 合作级别](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416848(v=vs.85)) · [Microsoft 鼠标数据](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418672(v=vs.85))
- **系统输入**。`SendInput` 将事件插入系统输入流，受到 UIPI 限制。即使 API 返回事件已插入，目标窗口是否消费了它仍需目标内的观测证据。 [Microsoft SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
- **VHF**。Microsoft 提供 KMDF HID 源驱动向系统提交 HID 报告的官方路径。新设备可能参与 Windows 的共享输入路由，后台接收条件仍由应用及 Windows 输入模式决定。需要在授权的独立环境中验证签名、设备标识、焦点、实际游戏接收与宿主隔离。 [Microsoft VHF](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)
- **虚拟机图形**。Workstation 的 3D 加速配置描述的是虚拟显示设备的宿主渲染路径。客体能力需在可启动的 Windows 客体内用 `dxdiag` 和测试程序测量。宿主显卡规格无法给出客体帧率、独占全屏支持或 GPU 直通结论。 [Broadcom Workstation 3D 加速](https://knowledge.broadcom.com/external/article/329348/the-display-option-in-the-virtual-machin.html)
