# AIMouse 架构

## 进程和线程

`AIMouse.exe` 启动一个 Win32 控制面板和一个分层光标窗口。MCP 的 stdio 读取线程接受逐行 JSON-RPC，控制面板线程串行处理状态变更和窗口消息。Windows Graphics Capture 在 stdio 线程采集单帧，面板仍能处理紧急停止。画面返回前复查授权代次。

`Session` 为 AI 保存客户区坐标、按下的按钮、按下的键和顶层窗口身份。输入前核对 HWND、进程和线程 ID、窗口类名、进程创建时间。Desktop 沿客户区查找同一进程的可用子窗口，在按下时记住接收窗口，后续移动和释放继续发往同一个子窗口。光标用 Per Monitor V2 DPI 感知的非激活、点击穿透覆盖层绘制。

## 后端

- **Desktop** 通过带超时的同步 Win32 消息派发。返回 `window_proc_returned` 时仅能确认窗口过程已经返回。鼠标位置和状态由 AIMouse 维护。直接键消息适用于处理这些消息的窗口，应用通过系统异步键状态读取的组合键不适用。
- **Game** 要求目标窗口显式声明 `AIMouse.GameInput.v1`，并对每次 `WM_COPYDATA` 返回协议确认。相对增量直接进入游戏适配器，游戏可在失焦时处理自己的输入。Raw Input、DirectInput 与厂商引擎的输入路径仍由目标应用决定。
- **Isolated** 在 VMware Windows 客户机显式启动 `--vm-stdio` 后可选。输入走客户机的全局 `SendInput`，有前台和授权门控。响应 `guest_input_queued` 表示客户机 Windows 接受排队。

`ai_detect_input_backend` 报告可观察的适配器声明。任意外部窗口采用 Raw Input 或 DirectInput 的情况保持 `unknown`。`ai_get_input_capabilities` 描述后端派发能力和目标准备状态，实际效果通过探针计数或画面观察验证。

## Game 适配器

目标为有权修改源码的应用。窗口使用 `SetPropW(hwnd, L"AIMouse.GameInput.v1", HANDLE(1))` 发布支持。接收 `WM_COPYDATA` 后校验 `dwData == 0x41494D31`、`cbData == 16`、字段范围和会话所有权。字段按顺序为 `uint32_t op`、`int32_t a`、`int32_t b`、`int32_t c`，`c` 保留为零。完成应用状态更新后返回 `0x41494D32`。

| op | 含义 | a | b |
| --- | --- | --- | --- |
| 1 | 相对运动 | dx | dy |
| 2 | 鼠标状态 | 左/右/中分别为 1/2/3 | 1 按下，0 释放 |
| 3 | 键状态 | Win32 VK | 1 按下，0 释放 |
| 4 | 滚轮 | 有符号增量 | 1 水平，0 垂直 |
| 5 | 清理当前会话状态 | 0 | 0 |

接收端应记录发送窗口，并在发送窗口关闭后释放该会话的逻辑状态。探针源码位于 `tests/probes/win32_probe.cpp`。
