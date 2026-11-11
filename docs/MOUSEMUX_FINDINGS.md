# MouseMux V3 3.0.19 输入架构发现

下文地址对应[分析报告](REVERSE_ENGINEERING.md)中的 x64 主程序哈希和首选基址。表中的行为来自静态控制流与数据流。实际设备、焦点、游戏接收情况需要运行验证。

## 输入捕获与设备区分

- `0x14016b540` 构造 RAWINPUTDEVICE 数组并调用 `RegisterRawInputDevices`。`0x140127290` 和 `0x140127b40` 在处理 `WM_INPUT` 时用 `GetRawInputData(RID_INPUT)` 读包，前者转入共用分派，后者为鼠标数据打时间戳并分发运动、滚轮。缓冲线程 `0x140127890` 等待消息和停止信号，常规消息循环跳过 `WM_INPUT` 区间，再用 `GetRawInputBuffer` 消费对齐后的记录。证据 `ev_d70f9f2e65772c00e3de95a9ef5a509634b81f6507bad123586e09be4a5355a1`、`ev_2d75acc8f22319369af3723eb60214bc42bba7b877e5e1f54a9a498e88114d30`、`ev_bcc37126aa54b095490ce57adb88d97e6003b245d19ae1ae774ddade23b2620d`、`ev_39044e27b42c1346667ae7bb56d3578e6441d664f6c05d6090bf900487f6ac1e`。
- 共用分派 `0x140150a10` 按鼠标、键盘和 HID 分类。鼠标处理 `0x14016b9c0` 读取 RAWINPUTHEADER 的设备字段，按设备标识交给下游处理相对或绝对运动、按钮和两向滚轮。键盘有独立扫描码分支 `0x14016bcf0`。鼠标处理还专门检查无设备句柄记录的额外信息，并过滤带有与内部 SendInput helper 一致标记的记录。证据 `ev_f6c06d5ee167babd369260ce7fde8eb473f9c3b51e8ef47a92eabff5b012e786`、`ev_df6caba7e1ad5b972df851f6b6405f5cb06e3782ba2ecac2146dbd39f16c9abc`、`ev_18b869c1d453e4f84f3550349877f7aaf88b6d9eb92edbbb6cadfd3001547a4b`。

## 虚拟用户与额外光标

- `0x140028390` 分配逻辑用户辅助状态，以递增序号生成一对内部输入 ID，启动 `virtual.user` 消息循环 `0x140028450`。循环分派内部消息。内部虚拟输入端 `0x140128fe0` 启动 `mousemux-v3.devio.virtual` 监听，`0x140129160` 将其收到的内部事件交给设备分派。该创建路径使用软件 ID，无物理鼠标句柄依赖。证据 `ev_076f314efd8d748fff9506d9f807c7337ee5a752bbe6933fdeb66a269d3783c9`、`ev_b5f09196ec332930ab12f76c16cd6875c8a519f03d9fd0fa7f55a18cd6c66c96`、`ev_eb58c04a5bde72907dd60adf893a3fb522eb335af3a7af96649175033b6451e7`、`ev_a57b41b9aacca3862da91d98e2af8ec23c0a2f6ac1218f39b33c964a9885b26f`。
- 路由读取各用户状态中的屏幕坐标（偏移 `0x5170`、`0x5174`），以及 `0x8208` 附近的按钮字段。窗口路径在必要时调用 `ScreenToClient`。Switched 的 root 交接从新用户状态读取坐标，Multiplex 的按键处理可保留最后活跃用户。证据 `ev_8bab341a76ae70b36955483f3f2500f220821d91df69d01383a385b73f1e12b3`、`ev_0e5113f265f1e015e889a13e6264905bc7e20d1456dc5fd947b61f29df7364ad`、`ev_1439e552e4219871044180f67aed7116da26d82057d83e0e6ca46b41233af3e9`。
- GPU 光标初始化 `0x140105d30` 动态加载 `d3d11.dll`、`dcomp.dll`，解析 `DCompositionCreateDevice`，建立独立的光标合成窗口。由此确认额外光标有应用渲染路径。软件虚拟用户与合成光标本身都可独立于物理鼠标存在。未观察到这两个路径创建鼠标 HID。证据 `ev_c9def4fb7e86f3d5304ecbe11c568a1dd69a8abd186e75acb233ba037914eed3`。[厂商 V3 发布说明](https://www.mousemux.com/pages/releases/)
- DPI 与多显示器方面，主程序 `dumpbin /DISASM` 在 `0x1400e6904`、`0x1400e6a6b`、`0x1400e6b00` 将 API 名传入 `GetProcAddress`，地址分别解析至 `SetProcessDpiAwarenessContext`、`SetThreadDpiAwarenessContext`、`GetDpiForMonitor`，取得有效入口后有间接调用。绝对输出函数 `0x140021bb0` 有按虚拟桌面原点、宽高与 `MOUSEEVENTF_VIRTUALDESK` 转换的分支。实际跨不同 DPI 显示器时的行为尚待运行验证。绝对输出证据 `ev_d549311c4f97e0c1e28bee46fa507977b5c20ef11e9e119953712b15d3bb1953`。

## Switched 与 Multiplex

| 路径 | 静态调用链和约束 | REA 证据 |
| --- | --- | --- |
| 模式分派 | `0x140025770` 通过模式位将输入交给 Switched `0x140025940` 或 Multiplex `0x140025850`。 | `ev_55a0ea14f9e1ca8517cd1b91761fdc7ecb5e97fca91652e6e1ed7bedec020050` |
| Switched | 跟踪 root 用户，根据按钮状态仲裁切换。特定配置下交接会将系统光标移到新 root 的位置。 | `ev_0e5113f265f1e015e889a13e6264905bc7e20d1456dc5fd947b61f29df7364ad` |
| Multiplex | 一条分支调用 `0x140025b70`，根据 HWND、客户区与非客户区、按钮和滚轮状态构造不同消息。辅助函数调用 `PostMessageA` 和带 60 ms 参数的 `SendMessageTimeoutA`。另有按用户标志进入系统输出分派的分支。 | `ev_1439e552e4219871044180f67aed7116da26d82057d83e0e6ca46b41233af3e9`、`ev_8bab341a76ae70b36955483f3f2500f220821d91df69d01383a385b73f1e12b3`、`ev_a77b032a7c11a519e7b0958bb961f89e4af000cf1999fad02ebf722f74997967`、`ev_86fd3e845176d52d03a98fdc361820c1954a85dac291756a04935115c42f5830` |
| 键盘线程与焦点 | 键盘 handler `0x14000f2b0` 可调用 `0x14000ed40`，按目标窗口线程 ID 调用 `AttachThreadInput`，切换目标时有解绑旧线程分支。`0x1400b56d0` 的 `SetForegroundWindow` 调用发生在 MouseMux 自己的用户设置对话框路径，不能据此认定每次虚拟点击都会抢焦点。 | `ev_3171bf8aa2f865e66b741383176e9a8cd5af6202ed0571831d2032dd2419759c`、`ev_472e5e6d93757da80d6be591f6a39367b847ab721a8f3a99e283ba310c6dea1a`、`ev_f1643184c8702ee118889db3284348706515fe9cb22089256b1e7c2d30f90bc2` |

普通消息路由对目标 HWND 有明确边界。应用仍可以自行查看系统指针、键盘或捕获状态，也可能忽略消息。窗口消息投递成功只确认投递操作。当前证据未验证两个用户同时拖拽、应用主动捕获、目标关闭后的清理或物理用户同时键入时的 Windows 全局状态。

## 游戏与硬件输入设置

| 设置或路径 | 证据及可确认行为 |
| --- | --- |
| **Force hardware motion / dragging** | 包内 `schema/ui_output.json` 将 UI 映射至 `enable.hardware` / `enable.hardware.drag`。主程序 `0x1400320e0` 映射用户标志 `0x400` / `0x800`，`0x14000a500` 与 `0x14000a5f0` 将设置转到运行态 `0x400000`，拖拽设置按用户按钮状态更新该位。在 SendInput 类型的运动分派 `0x140021b50`，该运行位在 `+6` 表现为 `0x40`，为真选 `0x140021bb0`，否则走 `SetCursorPos`。证据 `ev_1337fdbfecd38ea779fd25f623f2861d098aca69f6800f58f9b808541c788f54`、`ev_4cc5336792bfb5d43ab0a29f4bee5dd4adc85478b5ceb899b12dcf5ac844cd62`、`ev_ac412ef44fc9dbfeb10b2ee5bcca9ac5630dd83b2edd78799c3666d5c725b35d`、`ev_9f7efb7ea7f4f5cdad3a2828133756986b93409d4d9ac943ac51d8219de3c484`。 |
| **Force root operation** | `enable.root` 对应 `0x200`。Multiplex 在同一字段为真时进入 `0x140025e20` 系统输出分派，后者通过用户所选输出类型分派。普通 HWND 路由和全局 SendInput 因此有明确的代码分支。证据 `ev_1337fdbfecd38ea779fd25f623f2861d098aca69f6800f58f9b808541c788f54`、`ev_1439e552e4219871044180f67aed7116da26d82057d83e0e6ca46b41233af3e9`、`ev_14f08576b55871e04112b5b4265e59134f85d7ffb44814dd0e6e869207297226`。 |
| **Force hardware keyboard** | 包内 `schema/ui_keyboard.json` 将该项映射至 `force.hardware`，主程序映射至键盘标志 `0x10`。`0x140008550` 读取该位，选中后走 `0x14000ec10` → `0x14000f930` → `SendInput(INPUT_KEYBOARD)`。该 API 进入全局输入流。证据 `ev_0d2baed7e45bb1dd29bca0643cddabc35d6d4f45c5eb7daeee4e2d05e333cef8`、`ev_fdd28631490e20a114ef9d015a74f5643403fddef3773df95324bfed5132261b`、`ev_2e10a18d896f96e0f4dd21957424dd064bd78bb5cea6e79180fc63b6e7c36ce1`、`ev_f2b21e7ef20c7e795468b945c9a2dd010ae5f86ee45710cf88a0bf0826c439f5`。[Microsoft SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) |
| **Enable delta motion** | `enable.delta` 对应 `0x2000`。`0x140062a00` 经 `0x140029570` 读取系统位置，比较满足条件的用户内部坐标，差值超过 100 时发位置同步事件。它处理外部光标位置变化，游戏重定位是厂商说明的使用情形。证据 `ev_1337fdbfecd38ea779fd25f623f2861d098aca69f6800f58f9b808541c788f54`、`ev_c41775dad7ea272a3f6caf974d2aaf9216c1660f47bc7a44a1d9557abae9a900`、`ev_2284494327e7671d871eb5a2a9c74d7794cf2388bacf74b9124cc407b9885f6b`。[厂商手册](https://www.mousemux.com/pages/manual/) |
| **相对捕获与输出** | Raw Input 处理有相对运动分支。已查证的 SendInput 常用鼠标 helper 将用户坐标归一化，使用绝对运动标志 `0x8001` 或虚拟桌面绝对运动标志 `0xc001`。相对 SendInput 另外出现于鼠标加速度校准过程 `0x140044cd0`。未确认存在独立的后台游戏相对输出路径。证据 `ev_df6caba7e1ad5b972df851f6b6405f5cb06e3782ba2ecac2146dbd39f16c9abc`、`ev_d549311c4f97e0c1e28bee46fa507977b5c20ef11e9e119953712b15d3bb1953`、`ev_7b118b2495c68808facf798dc0c1177ddc88e8a85356e1be5a6ab8995fe91b42`。[Microsoft MOUSEINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput) |
| **其他输出类型** | 合成指针 API 由运行时解析。观察到的设备类型参数为触摸和笔。ViGEm 字符串和调用功能涉及 Xbox 360/DS4 虚拟手柄。安装载荷未列出本产品的 `.sys` 驱动，当前样本也未证明使用 VHF 虚拟鼠标。证据 `ev_c53697d0c49c136d10741383fdc2f52e85194fb15cc385ad1b27bf79f8a9a4c0`、`ev_f3a3fbccfa78327886f1047bee8f8d4dd1a56cb47038986ab9b734abd0daf3e6`、`ev_7cb1fad20adc03c2f7ee66c5403196937044d5f075c8ccdf10f60103695e6525`。 |

## 服务与辅助进程

安装包 MSI 的服务自定义动作指向 x86/x64 程序集合里的用户态服务。Main、daemon、service 的导入表含会话、文件映射与命名管道接口。仅凭共享导入无法确认各 IPC 协议、权限或实际调用关系。公开的 Input Mapper 使用单独的应用与窗口级授权，安装载荷仅提供其 App 清单和说明。本次没有取得 MCP 服务载荷来验证实现。[PE 清单](../analysis/evidence/mousemux-v3-3.0.19-pe.json) · [MouseMux MCP 产品文档](https://www.mousemux.com/ai-agents/)

x64 daemon 的 SHA-256 为 `cf3171fa13102740c91016aac2992911a83627e0ef10a3efeb55b30b4d640a9d`。REA 在该程序观察到 1,799 个过程、2,881 个已定义字符串，概览证据 `ev_952f3410206e498eb084abc4b61cadc58c7adef6cfb5dadf264d616f3d4194a1`。

- Daemon `0x140006dc0` 为辅助组件生成文件名，加载 DLL 并读取导出的版本、能力和注入方式入口。Daemon `0x14000ad60` 有真实 `CreateRemoteThread` 调用，说明包内存在用户态跨进程能力。具体触发场景和游戏运行影响仍需验证。证据 `ev_301f1ba03bb558e7ae8964dd55df7d7559a0e33540806e57dc58f6a628e53813`、`ev_1aeacde8bb1508335679ffc5b24af31671faa45f39220d1dde0fe5f7a0a4e3c5`。
- Daemon `0x1400275e0`、`0x140027790` 把 `\\.\pipe\mousemux-v3.service` 传给服务通信辅助函数。此处的管道属于内部组件通信。其协议与公开 MCP 层的桥接关系未确认。证据 `ev_6fed1bd4a44021b931e5946da1790914452112e03447ed184ba431424021339c`、`ev_67ba8f8cf627e65e929f942cf3b813b43ec7581d8a8c2ac70d1ffc0e517bec21`。
- x64 service 的 `0x1400019b0` 经 `0x140001aa0` 创建 Task Scheduler 的 COM 对象，向任务文件夹注册任务定义。CLSID 和 IID 的字节位于 `0x140054380`。启动分支 `0x1400031b0` 经 `0x140009aa0`、`0x140019b00` 创建与 daemon 客户端对应的命名管道。服务调度器有独立的执行请求和状态消息。证据 `ev_c392ea846ac4a000fb1691765a8026599f846402892acc1a9bfeffde62b88a88`、`ev_dd0a76f413acd5be91fea84b3c78d23cbcdbf1ea269c8a7d4dbe7f05861c3f43`、`ev_8b05e074e39d6a5812c596d59db02bebd2f876b989b10d6cc0b29af7e423cec9`、`ev_2a9554da60b4f8e89fd8e2820215e3453b340fe489025e962f8ad5cdda379e17`、`ev_86ef552577e72d789955f89584e3391a8118003e4aef386ffa59cd2eaaab32be`、`ev_9a83bf653d8063391871a69c9a36b2d1f5748b587a4e8621e4bdf2a4c8743eb8`、`ev_cf002a3c8f340e1a2e20529f438fdc39c6041b05cc2d52db076b6f1355268d9c`。[Microsoft ITaskService](https://learn.microsoft.com/en-us/windows/win32/api/taskschd/nn-taskschd-itaskservice)
- WinHooks x64 中，`hook_start` 创建线程执行 `0x180001840`，向 `SetWindowsHookExW` 传入表项中的类型与回调，线程 ID 参数为 0，随后泵消息并在退出分支解绑。表起始 `0x18000d010` 中的类型为 14、2、13，对应 `WH_MOUSE_LL`、`WH_KEYBOARD`、`WH_KEYBOARD_LL`。键盘低级回调 `0x180001e50` 有按键和内部标记检查，可返回非零以抑制部分事件。低级鼠标回调的原始指令也含标记检查和返回 1 的分支，Ghidra 未将其识别为完整过程。实际何时启用每一种 Hook 仍需动态验证。证据 `ev_bbb218bf56dee49830a368201e691c7dfd8560d34cc2da44c43884c364c4cf2f`、`ev_8f3d83b27f54ec8eadcc73fd47719bd7a047381a2fb22846cb174c3c9f64c828`、`ev_c3b2398cdccf1b81e6687b44ee319e5e5cb243de8f6a87822741dcc6c75d7782`、`ev_dfa3336ac43f6ac1af86985f8e3acd313f57b59c412477aea191a54dddc3a391`、`ev_dc87e9e8b7b713a63c196132a0a19737612b1ab9ace7b788d7e7356e4119ff8b`。[Microsoft SetWindowsHookExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw)

## 对 AIMouse 的约束

独立逻辑状态与应用级光标渲染可与路由解耦。窗口消息投递与系统输入具有不同边界。在需要实际相对输入的 Raw Input 或 DirectInput 应用中，以目标程序观测到的移动、按键和视角为验收。需要全局输入时把执行放到独立环境，以实测检验宿主输入和游戏画面。公开 SDK 示例可参考协议与使用方式。[MouseMux WinSDK 示例](https://github.com/MouseMux/WinSDK---Native-Windows-SDK)
