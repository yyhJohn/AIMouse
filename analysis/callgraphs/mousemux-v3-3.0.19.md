# MouseMux V3 x64 输入调用图

该图描述 [3.0.19 样本](../../docs/REVERSE_ENGINEERING.md)的静态调用关系。地址是模块的首选 VA，跨路径边只表示分支存在。图中省略部分中间函数。

```mermaid
flowchart LR
    Win["Windows Raw Input"]
    Msg["WM_INPUT · 0x140127290"]
    Buffer["缓冲线程 · 0x140127890"]
    Packet["按设备种类分派 · 0x140150a10"]
    Mouse["解析鼠标事件 · 0x14016b9c0"]
    Keyboard["解析扫描码 · 0x14016bcf0"]
    Virtual["内部虚拟输入 · 0x140129160"]
    Internal["设备事件分派 · 0x1400dd1a0"]
    Route["指针模式分派 · 0x140025770"]
    Switched["Switched · 0x140025940"]
    Multiplex["Multiplex · 0x140025850"]
    Hwnd["窗口消息 · 0x140025b70"]
    System["输出类型分派 · 0x140025e20"]
    Move["SendInput 类型的运动分支 · 0x140021b50"]
    Cursor["SetCursorPos"]
    Send["SendInput · 0x140021bb0"]
    Win --> Msg --> Packet
    Win --> Buffer --> Packet
    Packet --> Mouse --> Internal
    Packet --> Keyboard
    Virtual --> Internal
    Route --> Switched
    Route --> Multiplex
    Switched --> Hwnd
    Switched --> System
    Multiplex --> Hwnd
    Multiplex -->|Force root 等条件| System
    System -.->|所选输出类型为 SendInput| Move
    Move -->|硬件运动位未设置| Cursor
    Move -->|硬件运动位已设置| Send
```

输入解析到指针路由之间的完整调度链尚未确认。独立的键盘硬件路径为 `0x140008550` → `0x14000ec10` → `0x14000f930` → `SendInput`。代码点和证据 ID 见[发现记录](../../docs/MOUSEMUX_FINDINGS.md)。
