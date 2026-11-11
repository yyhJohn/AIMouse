# MouseMux V3 静态分析

## 样本和环境

2026-11-11 在 Windows 11 家庭中文版 x64（22631）上检查了已安装应用注册表、进程、服务、计划任务、驱动名称和常见安装、下载目录。这些检查没有定位到 MouseMux V3 安装实例。本报告的二进制来自官网下载页链接的 3.0.19 安装包。下载文件的 Windows Authenticode 状态为 `Valid`，签名主体为 `The MouseMux Company`，SHA-256 为 `50c2a5567647c57f17b7e93f8007d3660e12b30b326d3aae46d4719256ef0ed5`。[官网下载页](https://www.mousemux.com/download/)

安装包是 x86 Advanced Installer 引导程序。按文件尾的 `ADVINSTSFX` 记录表读取嵌入数据，验证条目范围，处理对应记录头部的位取反后，使用本机已有的 7-Zip 26.01 解开两个 7z 载荷。文件载荷有 2,413 个文件，另一个载荷含 MSI 与安装界面数据。MSI 使用 Windows Installer COM 数据库只读模式查询。安装包、MSI 自定义操作和 MouseMux 组件均未执行。[容器和 MSI 证据](../analysis/evidence/mousemux-v3-3.0.19-container.json) · [Advanced Installer 的 LZMA 打包说明](https://docs.advancedinstaller.com/configuration-tab.html) · [XArchive 的记录表解析参考](https://github.com/horsicq/XArchive/blob/master/installers/xadvancedinstaller.cpp)

REA MCP 为 6.5.0，Windows 原生 PE 由 Ghidra 12.1.4 与 64 位 JDK 23.0.1 分析。主程序 x64 SHA-256 为 `74eef230e09c262088a9dfcb3855e671c090933abbdc43d51ee9e5c4d4c04565`。REA 静态概览识别 5,653 个过程、11,232 个已定义字符串，首选基址为 `0x140000000`，证据 `ev_104f381de2fc127833b80d8ab35bf11f01b8c418d2a50a7bf54c30b297c4cb6c`。REA PE 资源检查解析 111 条资源，包含位图、图标、对话框、图标组、版本与清单，证据 `ev_319495d856360990ad01ca1f1002a2720ba0da4da14405ab16c1675b0b50308f`。

主程序清单（文件偏移 3,223,504，879 字节）请求 `highestAvailable` 和 `uiAccess="true"`。这反映载荷声明。是否获得对应令牌要看启动条件和 Windows 安装位置等要求，本机没有进行这项运行验证。

[PE 清单](../analysis/evidence/mousemux-v3-3.0.19-pe.json)记录 47 个 EXE/DLL 的载荷路径、版本、PE 架构、SHA-256、签名主体和静态导入。20 个为 x64，27 个为 x86，全部签名状态为 `Valid`。`dumpbin` 对六个辅助 DLL 报错，经带区段边界检查的只读 PE 目录解析器补齐导入数据。目录含 `bin32`、`bin64`、主程序的 JSON schema/flags、光标主题、用户文档和可选 App 清单。文件载荷清单没有以 `.sys`、`.inf` 或 `.cat` 结尾的项目。[输入调用图](../analysis/callgraphs/mousemux-v3-3.0.19.md)

## 模块和依赖

| 模块 | 已观察到的结构或联系 |
| --- | --- |
| 引导程序和 MSI | MSI `CustomAction` 的服务安装、移除分别通过 loader 请求执行 `service` 组件。初始化参数等待并检测服务，再请求启动 main。卸载流程向 main、32/64 位 daemon 和 service 等组件发退出请求。 |
| `main.exe` | x64 版 26 个可解析静态导入库。输入主路由、窗口消息、Raw Input、Win32 系统输入和 GPU 光标都存在实质代码引用。GPU 光标动态加载 `d3d11.dll` 和 `dcomp.dll`。 |
| `daemon.exe` | 有 x86/x64 两套。x64 导入包括 `CreateRemoteThread`、共享映射、会话 API 和 Raw Input API。见[具体调用点](MOUSEMUX_FINDINGS.md#服务与辅助进程)。 |
| `service.exe` | MSI 调度的 Windows 用户态 EXE。静态确认安装动作使用 Task Scheduler COM 注册任务，进程启动后可通过命名管道接收请求。当前没有已注册、运行的 MouseMux 组件实例。 |
| `winhooks32/64.dll`、`reloader*`、`unloader*` | 以独立签名 PE 随包分发。`dumpbin` 对部分 DLL 解析导入或导出失败。REA/Ghidra 成功解析 x64 WinHooks 的多个函数和 Hook 类型表。具体错误保留在 PE 清单。 |
| `device-probe.exe` | 静态导入 Raw Input、HID 设备查询和光标控制 API。本次未运行探针。 |
| `webhost.exe`、App Library、App Installer | 独立 PE。载荷内有 Input Mapper Beta 的 App 清单、图标和说明。此范围未取得 Input Mapper 的运行载荷。 |
| 调试支持 DLL、安装器与更新等辅助程序 | `dbghelp`、`exchndl`、`mgwhelp`、`symsrv` 和各辅助 EXE 的精确依赖与哈希见 PE 清单。 |

## 复核方法

1. 从官网取得对应版本，先核对签名与 SHA-256，再在独立工作目录离线读取安装容器。签名文件与提取出的商业载荷不进入项目发布物。
2. 在 REA 使用 `open_binary` 指定 `ghidra`，再调用 `binary_overview`。对具体 API 用 `search_procedures` 找到外部过程，随后用 `xrefs` 查实际调用点。动态调用从字符串地址反查，用 `resolve_containing_procedure` 核对函数边界，再读取目标过程。
3. 以[发现记录](MOUSEMUX_FINDINGS.md)所列首选虚拟地址复核。[REA 证据索引](../analysis/evidence/mousemux-v3-3.0.19-rea-index.json)记录样本哈希、工具操作与查询参数。Ghidra 生成的过程名和地址只适用于对应哈希。
4. 用 `dumpbin /HEADERS` 和 `/IMPORTS` 交叉核对 PE 元数据。用安装包内 JSON schema 和厂商文档交叉核对设置标签、标志位和产品术语。静态结果只证明调用分支存在，具体运行路径和兼容效果需要对同一版本做受控动态验证。

## 仍需验证

- 没有本机已安装实例，无法比较安装后的文件、服务实例、驱动栈、配置、运行时线程、跨进程窗口消息顺序与被动事件。
- x86/x64 二进制均做了 PE 清点。深度调用链集中在 x64 主程序、daemon、service 和 WinHooks，尚未逐一跟完每个 DLL。
- 单次静态分析没有覆盖全部间接调用。针对窗口关闭、模式切换、同时拖拽、失焦和按键清理的行为仍需实测。
- 需要取得同版本 Input Mapper 的合法载荷，才能用 REA 检查 MCP 工具与 SDK 桥接实现。现有载荷中的 App 清单与厂商文档只能定位组件边界。
- 不以导入表中的 API 缺席推导能力不存在。尤其是动态装载的窗口 Hook、指针与图形接口。
