# 输入与授权边界

- 每个 MCP 进程启动后为 Disarmed。绑定、解绑、切换模式、断连和关闭面板都会取消授权并清理逻辑按键。已授权的目标失效时自动停止。清理失败会反映在状态的 `cleanup_delivery_uncertain`。
- Desktop 与 Game 只向绑定的窗口树发送各自协议消息。窗口过程可以自行调用设置焦点、捕获鼠标或修改光标的 API。派发时检测到目标进程成为前台即停止。Desktop 还会检查接收线程的鼠标捕获状态，并在发现捕获时停止。
- Desktop 对有权限的 Win32 窗口提供按键和字符消息。键盘按下、释放和 Ctrl/Shift 鼠标修饰标志属于 AIMouse 的应用层状态。Windows 按键轮询、系统快捷键和真实组合键路由由系统维护。
- `SendMessageTimeout` 每次最多等待 75 ms，输入工具预算为 350 ms。超时可能发生在目标已经开始处理消息以后，工具以 `delivery_uncertain` 报告，并尽力释放已经记录的键和按钮。异常终止时，接收端也应按会话所有者消失处理残留状态。
- Isolated 的全局输入只在显式启动 `--vm-stdio` 的 VMware Windows 客户机内启用。它会改变客户机系统光标和键盘状态，清理释放也走客户机输入流。
- MCP 使用继承的 stdin/stdout 管道，信任发起进程和同一用户桌面的操作系统权限。面板 Arm 需要本地 UI 操作，其作用是操作授权。拥有同一 Windows 用户执行权限的代码可模拟 UI。UIPI 仍由 Windows 管理。
- 日志位于 `%LOCALAPPDATA%\AIMouse\logs`，记录 UTC 时间、工具名、结果、授权状态和目标 HWND。屏幕图像、键入文本和参数值不会写入操作日志。
