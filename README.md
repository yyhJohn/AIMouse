# AIMouse

AIMouse 在 Windows 上给 AI 一个独立逻辑光标。Human User 继续使用物理键鼠。AI 通过本地 MCP 绑定窗口，在控制面板获得授权后操作兼容的应用。

**Windows 11 x64** · [下载便携版](https://github.com/yyhJohn/AIMouse/releases/latest) · [MIT 许可](LICENSE)

AIMouse gives AI agents an independent logical cursor on Windows 11. It routes input to a locally approved window through MCP and includes a sample game adapter for background relative input.

| 模式 | 操作路径 | 使用条件 |
| --- | --- | --- |
| Desktop | 指定窗口的 Win32 鼠标、键盘和字符消息 | 目标应用需处理对应消息。逻辑光标和系统光标各自移动 |
| Game | 应用主动注册的协作协议，传递相对轴和按键状态 | 游戏需要 AIMouse 适配器，仓库提供 FPS 样例 |
| Isolated | Windows VMware 客户机内的前台 `SendInput` | 需在运行中的 VMware Windows 客户机启动并授权。待客户机实测 |

## 使用

1. 解压便携包。将 MCP 客户端配置为运行 `AIMouse.exe --stdio`，参见 [MCP 接入](docs/MCP_SETUP.md)。
2. 用 `ai_create_user` 创建 AI 用户，调用 `ai_list_windows` 找到目标，调用 `ai_bind_window` 绑定其 HWND。
3. 核对 AIMouse 控制面板上的目标名称，再点击 **Arm bound window**。MCP 的 `ai_arm` 只发出请求。
4. Desktop 使用以绑定窗口客户区左上角为原点的物理像素。先移动，再点击、拖拽或滚动。窗口截图返回 PNG。
5. 点击 **Emergency stop** 或按 **Ctrl+Alt+Shift+F11** 停止 AI。如果组合键已被占用，面板会提示。

便携包含自建探针。先启动 `tools\aimouse_input_probe.exe win32 --capturable` 测试 Desktop。测试 Game 时启动 `tools\aimouse_input_probe.exe cooperative`，选择 Game 并绑定对应窗口，再调用 `ai_move_relative` 观察相机变化。

## 构建

需要 MSVC x64、包含 C++/WinRT 头的 Windows SDK、CMake 3.24+，端到端测试使用 Node.js。发布目标为 Windows 11 x64，已在 Windows 11 Home 实测。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -C Release -B dist
```

测试会打开自建窗口并模拟本地 Arm 按钮。运行时保持鼠标与前台窗口稳定，方便比较系统输入状态。默认测试只对自建窗口发消息并采集该窗口画面。

## 证据与范围

- [MouseMux V3 逆向过程](docs/REVERSE_ENGINEERING.md)和[输入架构发现](docs/MOUSEMUX_FINDINGS.md)
- [参考项目与 Windows 文档](docs/REFERENCES.md)
- [AIMouse 架构](docs/ARCHITECTURE.md)和[输入与授权边界](docs/SECURITY.md)
- [游戏兼容性矩阵](docs/GAME_COMPATIBILITY.md)、[输入后端](docs/GAME_INPUT_BACKENDS.md)和[隔离运行](docs/ISOLATED_GAME.md)
- [里程碑验收](docs/VERIFICATION.md)、[测试环境](docs/ENVIRONMENT.md)和[性能测量](docs/PERFORMANCE.md)
