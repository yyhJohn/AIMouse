# VMware 客户机隔离

将游戏、AIMouse 和 MCP 客户端运行在同一台 Windows 客户机的交互桌面。游戏在客户机前台接收输入。释放 VMware 控制台的键鼠捕获后，可继续在宿主应用中工作。VMware 的虚拟显卡能力由虚拟硬件、客户机驱动和宿主 GPU 共同决定。 [Broadcom VMware 图形加速说明](https://knowledge.broadcom.com/external/article/329348/the-display-option-in-the-virtual-machin.html)

## 部署和验证

1. 在 VMware Workstation 中准备有授权的 Windows 客户机，安装 VMware Tools，确认 `dxdiag` 中的图形功能和设备。
2. 将 AIMouse 便携包复制到客户机。在客户机 MCP 客户端将参数改为 `--vm-stdio`。启动参数会核对客户机的 VMware SMBIOS 标识。
3. 在 MCP 中创建用户，选择 `isolated`，绑定测试窗口。在客户机面板 Arm 后，手动将同一个窗口置前台。
4. 优先运行自建 `aimouse_input_probe.exe raw --foreground` 和 `directinput --foreground`，确认接收端的轴、按钮、键和相机实际变化。以 `ai_capture_window` 检验画面是否可采集。
5. 释放 VMware 控制台的键鼠捕获。在宿主编辑器移动和打字，同时在客户机通过 MCP 驱动探针。采集客户机输入、宿主物理光标与前台状态，测量 GPU 帧时和动作成功率。

隔离后端在每个事件发送前检查客户机目标是否仍在前台，按下鼠标按钮或滚动前也检查当前指针是否落在目标窗口。Windows 前台切换与系统输入队列存在竞争窗口，因此需要在专用客户机内使用。收到 Windows 排队确认后，仍需观测目标是否接受合成输入。

## 本机状态

本机为 Windows 11 Home 22631，使用 RTX 4060 Laptop GPU。VMware Workstation 25 已有一台开启虚拟 3D 的 Windows 10 配置。其历史 EFI 日志报未找到操作系统，当前虚拟盘文件合计约 7.9 MB，VM 未运行。宿主验证可覆盖模式拒绝和配置边界，客户机输入、游戏画面、GPU 加速、宿主与客户机并行输入和性能损耗均待有系统的客户机实测。
