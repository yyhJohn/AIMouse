# AIMouse 性能验证

## Win32 接收端基线

在 Windows 11 Home x64，系统构建 22631，Core i7-13650HX 上测试。探针使用 MSVC 19.44 和 SDK 10.0.26100.0 编译为 Release x64。测试器对自建后台窗口逐次调用 `SendMessageTimeoutW(WM_MOUSEMOVE)`，用 Windows 高精度时钟控制发送间隔，接收端关闭逐事件日志。延迟为调用到目标窗口过程返回的往返耗时，包含线程调度。实际频率以发送次数除以实测时间计算。详细数据、进程工作集和前台采样见[原始结果](../analysis/evidence/input-receiver-throughput.json)。

| 设定频率 | 实际频率 | 发送 / 收到 | P50 | P95 | P99 | 最大 | 探针 CPU 时间 | 探针工作集 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 125 Hz | 124.98 Hz | 375 / 375 | 91.4 µs | 372.0 µs | 774.5 µs | 1,433.8 µs | 0 ms | 10.60 MiB |
| 500 Hz | 498.88 Hz | 1,500 / 1,500 | 65.4 µs | 743.1 µs | 1,583.5 µs | 4,098.5 µs | 31.25 ms | 10.63 MiB |
| 1,000 Hz | 995.33 Hz | 3,000 / 3,000 | 54.5 µs | 507.0 µs | 905.8 µs | 5,232.4 µs | 46.88 ms | 10.61 MiB |

每组约 3 秒。同步调用逐次返回，接收计数等于发送计数。发送线程短时忙等以避免改变系统计时器分辨率，表中的 CPU 时间只计接收进程，从启动计至该组发送结束。125 Hz 的 0 ms 是系统 CPU 计数粒度下的读数。1,000 Hz 组采样到桌面焦点与主光标位置变化，适用于接收吞吐基线，隔离结论使用独立的端到端测试。高分位和最长耗时属于此轮调度条件，跨频率比较需要重复测试。

## MCP 端到端

Release x64 通过本地 stdio 串行发送 250 次 `ai_move_absolute`。接收端计到全部 250 次。通过的一轮中，从 Node 客户端发起请求到收到响应，P50 为 1.569 ms，P95 为 2.365 ms，P99 为 2.969 ms。该时间包含 JSON 编解码、控制线程调度、窗口过程和审计写入。它无法用于推算硬件轮询率或实际显示延迟。原始记录见[端到端结果](../analysis/evidence/aimouse-mcp-e2e.json)。

## 复现

在已编译探针的同一交互式 Windows 桌面，用 PowerShell 7 运行以下命令。

```powershell
pwsh -File tests/probes/measure.ps1 -ProbeDirectory build/tests/probes/Release -Benchmark
```

`-DurationSeconds` 设置每组时长，`-OutputPath` 保存包含二进制哈希、采样数据与分位数的 JSON。实际鼠标轮询率、显示刷新率、AI 光标呈现延迟、设备级事件丢失、虚拟机和长时间运行仍需专门测试。
