# MCP 接入

将配置示例中的绝对路径改成已解压的 `AIMouse.exe`。这些文件用于合并到现有客户端配置。

## Codex

将 [Codex 示例](../examples/codex-mcp.toml) 的表项合并到 Codex 配置。配置生效后重新连接客户端。也可在目标环境运行以下命令。

```powershell
codex mcp add aimouse -- 'C:\Tools\AIMouse\AIMouse.exe' --stdio
```

## Claude Code

将 [Claude Code 示例](../examples/claude-mcp.json) 用作项目的 `.mcp.json` 配置条目，或运行以下命令。

```powershell
claude mcp add --transport stdio aimouse -- 'C:\Tools\AIMouse\AIMouse.exe' --stdio
```

[Claude Code 的 stdio 配置](https://code.claude.com/docs/en/mcp#option-3-add-a-local-stdio-server)

## 工具调用

客户端启动后执行 MCP 握手和工具发现。主流程为 `ai_create_user`、`ai_list_windows`、`ai_bind_window`、`ai_arm`、面板 Arm、`ai_get_status`、输入工具、`ai_disarm`。每个进程维护一个 AI 用户。

| 示例调用 | 参数 |
| --- | --- |
| `ai_bind_window` | `{"hwnd":"0x10678"}` |
| `ai_move_absolute` | `{"x":120,"y":90}` |
| `ai_click` | `{"button":"left"}` |
| `ai_scroll` | `{"delta":-120}` |
| `ai_drag` | `{"x":300,"y":150,"steps":12}` |
| `ai_key_down` / `ai_key_up` | `{"vk":87}` |
| `ai_move_relative` | `{"dx":100,"dy":-25}` |

MCP 返回 `structuredContent.ok`、错误码和可显示文本。调用窗口截图时，工具也会返回 `image/png` 内容。整屏截图需要在已 Arm 的面板另勾选 **Allow primary display capture**，采集范围为主显示器。操作日志路径见 `ai_get_status`。

可识别的协议版本为 2024-11-05、2025-03-26 和 2025-06-18。使用更新版本的客户端会收到 2025-06-18 协商响应。 [MCP stdio 规范](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
