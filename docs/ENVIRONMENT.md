# 本机开发与验证环境

| 项目 | 观察值 |
| --- | --- |
| 系统 | Windows 11 家庭中文版 x64，10.0.22631 |
| CPU / GPU | Intel Core i7-13650HX，NVIDIA RTX 4060 Laptop GPU |
| 显示刷新率 | 本次检测 100 Hz |
| 设备安全 | WMI 显示 VBS 运行，`SecurityServicesConfigured` 和 `SecurityServicesRunning` 均为 `{0}`，未观测到 HVCI 服务运行 |
| Git | 2.45.1.windows.1 |
| Node.js / npm | 24.14.1 / 11.11.0 |
| Codex CLI | 0.167.0-alpha.4.1 |
| REA | 已连接的 MCP 与本机 CLI，6.5.0。Ghidra 12.1.4 的 Windows x64 PE 静态分析可用 |
| IDA / Hopper | IDA MCP 未配置，Hopper provider 不支持本机 Windows |
| JDK | 23.0.1 x64 |
| Python | 已有 Unreal 工具链内嵌 3.11.8，常规 PATH 上为 Windows 应用别名 |
| C++ | Visual Studio 2022 Build Tools 17.14，MSVC 19.44，Windows SDK 10.0.26100.0 |
| CMake | 3.31.12 x64 便携工具放在项目 `.deps`，校验 Kitware 发布的 SHA-256 后使用 |
| VMware | Workstation 25，现有 Windows 10 配置未启动，EFI 日志报未找到操作系统 |

项目构建依赖以便携归档或源码形式置于工作目录。MouseMux 使用厂商 3.0.19 安装包的离线提取样本，详见[逆向报告](REVERSE_ENGINEERING.md)。
