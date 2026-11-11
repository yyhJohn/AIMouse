# 游戏输入后端

Win32 消息适用于通过窗口过程消费传统消息的应用。Raw Input 由系统提供 HID 事件和设备句柄。DirectInput 的采集和独占规则由设备协作级别决定。AIMouse 在宿主对前两种设备级接口仅做接收端探针实验。 [Microsoft Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input) · [DirectInput 协作级别](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416848(v=vs.85))

## 引擎

- **SDL 3 和 GLFW** 已构建 D3D11 和 OpenGL 样例，窗口化和无边框测试见[兼容性矩阵](GAME_COMPATIBILITY.md)。相对模式、独占和捕获的行为随应用设置变化。
- **Unity** 的 Input System 支持 `<Mouse>/delta`、光标锁定行为和输入设备状态。后续用自行编写的项目测量输入动作和相机增量，分别运行窗口化、锁定、失焦和客户机前台路径。Unity 程序尚未实测。 [Unity Input System 文档](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.4/manual/Mouse.html)
- **Unreal Engine** 的 CommonUI 文档描述了从 Windows 平台层进入 Slate 和 UMG 的输入路径。RawInput 插件用于配置特定设备。后续需在具体项目验证 UI 点击和 Gameplay 相机输入。Unreal 程序尚未实测。 [Epic CommonUI 输入](https://dev.epicgames.com/documentation/en-us/unreal-engine/commonui-input-technical-guide-for-unreal-engine) · [Epic RawInput 插件](https://dev.epicgames.com/documentation/unreal-engine/rawinput-plugin-in-unreal-engine)

## 可选 VHF 验证设计

在有授权的独立测试机或客户机内构建和签名 KMDF HID 源驱动。鼠标集合定义相对 X/Y、滚轮和按钮位，键盘集合定义修饰键及按键报告。通过设备接口接收限长的版本化事件，ACL 授权本机测试控制进程。驱动保存每集合状态，在写端断开或停止时提交全释放报告。通过 `VhfReadReportSubmit` 将报告交给 VHF。 [Microsoft VHF](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-) · [VhfReadReportSubmit](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfreadreportsubmit)

验证顺序为 PnP 枚举、Raw Input 设备身份和相对轴、DirectInput 协作级别、窗口焦点与宿主光标。分别记录 Windows 对虚拟设备的接收、应用对后台事件的接收和真实游戏效果。虚拟 HID 设备仍参与同一桌面系统输入路由。AIMouse 0.1.0 中 VHF 为设计，驱动尚未实现或加载。
