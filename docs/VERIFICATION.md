# 交付与验收记录

| 里程碑 | 交付 | 验证范围与待测项 |
| --- | --- | --- |
| 1. MouseMux 研究 | [PE 清单](../analysis/evidence/mousemux-v3-3.0.19-pe.json)、[REA 记录](../analysis/evidence/mousemux-v3-3.0.19-rea-index.json)和[架构报告](MOUSEMUX_FINDINGS.md) | 从签名 V3 3.0.19 安装包静态分析，跟踪 Raw Input、Multiplex、WinHooks、系统运动、键盘和服务。商业应用的实际并行输入与可选 Input Mapper 尚待运行验证 |
| 2. 独立光标 | C++20 状态机和分层光标窗口 | MCP 测试检查光标可见性，并对比系统光标采样。多显示器、不同刷新率仍待覆盖 |
| 3. 窗口操作 | 绑定、点击、拖拽、滚动和逻辑键盘 | 自建 Win32 后台窗口实测，第二窗口未收到路由事件。Office、浏览器和其他第三方软件待逐项验证 |
| 4. 游戏实验 | Raw Input FPS、DirectInput、SDL 3 / D3D11、GLFW / OpenGL、协作 FPS | [兼容性](GAME_COMPATIBILITY.md)和[吞吐](PERFORMANCE.md)。独占全屏、锁定光标、Unity、Unreal 和商业单机游戏待测 |
| 5. MCP | 27 个 stdio 工具、Codex 和 Claude Code 配置、Arm/Disarm/Stop | [端到端](../analysis/evidence/aimouse-mcp-e2e.json)覆盖 JSON-RPC 握手、输入、窗口截图、状态采样和断连清理。客户端配置提供示例，具体客户端连接待配置后确认 |
| 6. 客户机隔离 | VMware 客户机前台输入后端和[验证方案](ISOLATED_GAME.md) | 宿主拒绝启动 VM 输入路径。客户机系统尚未就绪，宿主与客户机并行输入、客体 GPU 和帧时待测 |
| 7. 系统交付 | GUI、审计日志、CMake/CTest、x64 便携包 | Windows 11 Home 上完成干净构建。便携包解压后二进制 SHA-256 与测试构建一致，并通过同一端到端测试。实体键鼠长时并行及持久压力验证待测 |

End-to-end 验证使用自建接收窗口，显示与路由使用实际 Windows API。采样期间如果操作者移动实体键鼠或切换窗口，严格的系统状态对比可能失败。结果记录通过条件下的测量。
