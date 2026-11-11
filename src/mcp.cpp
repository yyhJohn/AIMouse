#include "mcp.hpp"

#include <algorithm>
#include <array>

namespace aimouse {
namespace {

constexpr std::size_t max_line = 65536;
constexpr std::string_view protocol = "2025-06-18";
std::atomic<McpCall*> pending_call{};

Json integer(int min, int max) {
    return {{"type", "integer"}, {"minimum", min}, {"maximum", max}};
}

Json string_choice(std::initializer_list<const char*> values) {
    return {{"type", "string"}, {"enum", values}};
}

const Json& catalog() {
    static const Json tools = [] {
        Json list = Json::array();
        const Json button = string_choice({"left", "right", "middle"});
        const Json coord = integer(0, 32767);
        const Json delta = integer(-32767, 32767);
        const Json boolean{{"type", "boolean"}};
        const auto add = [&](std::string_view name, std::string_view description,
                             Json props = Json::object(), Json required = Json::array()) {
            list.push_back({{"name", name}, {"description", description}, {"inputSchema", {
                {"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)},
                {"additionalProperties", false}}}});
        };
        add("ai_create_user", "Create the single independent logical AI user.");
        add("ai_destroy_user", "Disarm, release held input and remove the AI user.");
        add("ai_list_users", "List the physical human and logical AI users.");
        add("ai_get_status", "Read arm state, target, logical cursor, held input and last operation.");
        add("ai_list_windows", "List visible top-level windows and HWNDs in the interactive desktop.");
        add("ai_bind_window", "Bind one HWND from ai_list_windows. Changing targets disarms.",
            {{"hwnd", {{"type", "string"}, {"maxLength", 18}}}}, {"hwnd"});
        add("ai_unbind_window", "Release logical input and clear the binding.");
        add("ai_get_input_capabilities", "Report backend capabilities and the current target readiness.");
        add("ai_detect_input_backend", "Inspect the bound window's cooperative adapter advertisement. Raw Input and DirectInput stay unknown.");
        add("ai_select_mode", "Select Desktop, cooperative Game or isolated guest input. Isolated mode requires --vm-stdio in VMware.",
            {{"mode", string_choice({"desktop", "game", "isolated"})}}, {"mode"});
        add("ai_arm", "Request local approval for the current target. The human must click Arm in the panel.");
        add("ai_disarm", "Disarm and send best-effort releases for held logical input.");
        add("ai_emergency_stop", "Disarm and reset all held logical input immediately.");
        add("ai_set_cursor", "Set the logical Desktop cursor visibility and optional RGB color.",
            {{"visible", boolean}, {"rgb", integer(0, 0xffffff)}}, {"visible"});
        add("ai_move_absolute", "Move to physical pixels in the bound window's client area in Desktop mode.",
            {{"x", coord}, {"y", coord}}, {"x", "y"});
        add("ai_move_relative", "Move by logical pixels in Desktop mode or send relative axes to a cooperative game.",
            {{"dx", delta}, {"dy", delta}}, {"dx", "dy"});
        add("ai_mouse_down", "Hold one logical mouse button. Defaults to left.", {{"button", button}});
        add("ai_mouse_up", "Release one logical mouse button. Defaults to left.", {{"button", button}});
        add("ai_click", "Send down and up to the current target. Defaults to left.", {{"button", button}});
        add("ai_double_click", "Send the Win32 double-click sequence or two cooperative game clicks.",
            {{"button", button}});
        add("ai_scroll", "Send a signed wheel delta. One traditional notch is 120.",
            {{"delta", delta}, {"horizontal", boolean}}, {"delta"});
        add("ai_drag", "Drag to an absolute client point in Desktop mode or by a signed delta in Game mode.",
            {{"x", delta}, {"y", delta}, {"steps", integer(1, 24)}, {"button", button}}, {"x", "y"});
        add("ai_key_down", "Hold one virtual-key code at the application level.",
            {{"vk", integer(1, 254)}}, {"vk"});
        add("ai_key_up", "Release one application-level virtual-key code.",
            {{"vk", integer(1, 254)}}, {"vk"});
        add("ai_type_text", "Send Unicode character messages to the Desktop control under the logical cursor.",
            {{"text", {{"type", "string"}, {"maxLength", 96}}}}, {"text"});
        add("ai_capture_window", "Return a PNG frame of the armed bound window using Windows Graphics Capture.");
        add("ai_capture_screen", "Return a PNG of the primary display after local screen-capture consent.");
        return list;
    }();
    return tools;
}

void validate(std::string_view name, const Json& args) {
    const auto& tools = catalog();
    const auto it = std::find_if(tools.begin(), tools.end(), [&](const Json& tool) {
        return tool.at("name").get_ref<const std::string&>() == name;
    });
    if (it == tools.end()) throw ToolError("unknown_tool", "Unknown AIMouse tool.");
    if (!args.is_object()) throw ToolError("invalid_arguments", "Arguments must be a JSON object.");
    const auto& schema = it->at("inputSchema");
    const auto& properties = schema.at("properties");
    for (const auto& key : schema.at("required")) {
        const auto& required = key.get_ref<const std::string&>();
        if (!args.contains(required)) throw ToolError("invalid_arguments", "Missing argument " + required + ".");
    }
    for (auto arg = args.begin(); arg != args.end(); ++arg) {
        const auto field = properties.find(arg.key());
        if (field == properties.end()) throw ToolError("invalid_arguments", "Unexpected argument " + arg.key() + ".");
        const auto& type = field->at("type");
        bool valid = false;
        if (type == "integer") {
            if (arg->is_number_integer()) {
                const double value = arg->get<double>();
                valid = value >= field->at("minimum").get<int>() && value <= field->at("maximum").get<int>();
            }
        } else if (type == "boolean") {
            valid = arg->is_boolean();
        } else if (arg->is_string()) {
            const auto& value = arg->get_ref<const std::string&>();
            valid = value.find('\0') == std::string::npos;
            if (field->contains("maxLength")) {
                const auto count = std::count_if(value.begin(), value.end(), [](unsigned char ch) {
                    return (ch & 0xc0) != 0x80;
                });
                valid = valid && count <= field->at("maxLength").get<int>();
            }
            if (field->contains("enum")) {
                const auto& choices = field->at("enum");
                valid = valid && std::find(choices.begin(), choices.end(), *arg) != choices.end();
            }
        }
        if (!valid) throw ToolError("invalid_arguments", "Invalid argument " + arg.key() + ".");
    }
}

Json failure(const ToolError& error) {
    return {{"ok", false}, {"code", error.code}, {"message", error.what()}};
}

Json rpc_result(const Json& id, Json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json rpc_error(const Json& id, int code, std::string_view message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

bool on_ui(HWND window, McpCall& call) {
    pending_call = &call;
    const bool processed = SendMessageW(window, wm_tool_call, 0, reinterpret_cast<LPARAM>(&call)) == TRUE;
    pending_call = nullptr;
    return processed;
}

Json invoke(HWND window, const Json& params) {
    McpCall call{};
    call.name = params.at("name").get<std::string>();
    call.args = params.value("arguments", Json::object());
    if (!on_ui(window, call)) throw ToolError("disconnected", "The AIMouse panel has closed.");

    if (!call.error && call.capture_generation) {
        try {
            auto image = capture_image(call.capture_window);
            call.capture_check = true;
            if (!on_ui(window, call) || call.error) {
                throw ToolError("capture_revoked", "Capture authorization expired before delivery.");
            }
            const Json meta{{"ok", true}, {"width", image.width}, {"height", image.height},
                            {"source", call.capture_screen ? "primary_display" : "bound_window"}};
            return {{"content", Json::array({
                {{"type", "text"}, {"text", meta.dump()}},
                {{"type", "image"}, {"mimeType", "image/png"}, {"data", std::move(image.base64)}}})},
                {"structuredContent", meta}, {"isError", false}};
        } catch (const ToolError& error) {
            call.error = true;
            call.result = failure(error);
            call.capture_check = true;
            on_ui(window, call);
        }
    }
    return {{"content", Json::array({{{"type", "text"}, {"text", call.result.dump()}}})},
            {"structuredContent", std::move(call.result)}, {"isError", call.error}};
}

class Protocol {
public:
    explicit Protocol(HWND window) : window_(window) {}

    std::optional<Json> line(std::string_view bytes) {
        const auto req = Json::parse(bytes, nullptr, false);
        if (req.is_discarded()) return rpc_error(nullptr, -32700, "Parse error");
        if (!req.is_object() || !req.contains("jsonrpc") || req.at("jsonrpc") != "2.0" ||
            !req.contains("method") || !req.at("method").is_string()) {
            return rpc_error(nullptr, -32600, "Invalid Request");
        }
        const auto& method = req.at("method").get_ref<const std::string&>();
        if (!req.contains("id")) {
            if (method == "notifications/initialized" && initialized_) ready_ = true;
            return std::nullopt;
        }
        const auto& id = req.at("id");
        if (!(id.is_string() || id.is_number_integer())) return rpc_error(nullptr, -32600, "Invalid request ID");
        const auto params = req.value("params", Json::object());
        if (!params.is_object()) return rpc_error(id, -32602, "Params must be an object");
        if (method == "initialize") {
            if (initialized_) return rpc_error(id, -32600, "Already initialized");
            if (!params.contains("protocolVersion") || !params.at("protocolVersion").is_string()) {
                return rpc_error(id, -32602, "Missing protocolVersion");
            }
            const auto requested = params.at("protocolVersion").get<std::string>();
            const auto selected = requested == "2024-11-05" || requested == "2025-03-26" ? requested : protocol;
            initialized_ = true;
            return rpc_result(id, {{"protocolVersion", selected}, {"capabilities", {{"tools", Json::object()}}},
                {"serverInfo", {{"name", "AIMouse"}, {"version", "0.1.0"}}},
                {"instructions", "Bind a target, then get local approval in the AIMouse panel before sending input."}});
        }
        if (method == "ping") return rpc_result(id, Json::object());
        if (!ready_) return rpc_error(id, -32002, "MCP initialization required");
        if (method == "tools/list") return rpc_result(id, {{"tools", catalog()}});
        if (method == "tools/call") {
            if (!params.contains("name") || !params.at("name").is_string()) {
                return rpc_error(id, -32602, "Missing tool name");
            }
            try {
                return rpc_result(id, invoke(window_, params));
            } catch (const ToolError& error) {
                return rpc_error(id, -32000, error.what());
            } catch (const std::exception&) {
                return rpc_error(id, -32603, "AIMouse internal error");
            }
        }
        return rpc_error(id, -32601, "Method not found");
    }

private:
    HWND window_{};
    bool initialized_{};
    bool ready_{};
};

bool write_line(HANDLE pipe, const Json& message) {
    const std::string line = message.dump() + '\n';
    for (std::size_t written = 0; written < line.size();) {
        DWORD count{};
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(line.size() - written, 65536));
        if (!WriteFile(pipe, line.data() + written, chunk, &count, nullptr) || !count) return false;
        written += count;
    }
    return true;
}

} // namespace

McpCall* claim_tool_call(LPARAM parameter) {
    auto* call = reinterpret_cast<McpCall*>(parameter);
    return call && pending_call.compare_exchange_strong(call, nullptr) ? call : nullptr;
}

void dispatch_tool(Session& session, McpCall& call) {
    try {
        if (call.capture_check) {
            if (call.error) {
                session.audit(call.name, call.result.at("code").get<std::string>());
                return;
            }
            session.tick();
            if (!session.armed() || session.generation() != call.capture_generation ||
                (call.capture_screen && !session.screen_allowed())) {
                throw ToolError("capture_revoked", "The local authorization changed during capture.");
            }
            session.audit(call.name, "captured");
            return;
        }
        validate(call.name, call.args);
        if (call.name == "ai_capture_window" || call.name == "ai_capture_screen") {
            call.capture_screen = call.name == "ai_capture_screen";
            if (call.capture_screen) {
                if (!session.screen_allowed()) {
                    throw ToolError("screen_capture_denied", "Enable primary-display capture in the local panel.");
                }
            } else {
                call.capture_window = WindowIdentity::read(session.capture_target());
                if (!call.capture_window) throw ToolError("target_lost", "The bound window closed.");
            }
            call.capture_generation = session.generation();
            session.audit(call.name, "authorized");
            return;
        }
        call.result = session.call(call.name, call.args);
        call.result["ok"] = true;
        session.audit(call.name, "ok");
    } catch (const ToolError& error) {
        call.result = failure(error);
        call.error = true;
        session.audit(error.code == "unknown_tool" ? "unknown_tool" : call.name, error.code);
    } catch (const std::exception&) {
        call.result = {{"ok", false}, {"code", "internal_error"}, {"message", "AIMouse internal error."}};
        call.error = true;
        session.local_stop("internal_error");
        session.audit(call.name, "internal_error");
    }
}

void serve_stdio(HWND window, const std::atomic_bool& stopping) {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(in) != FILE_TYPE_PIPE || GetFileType(out) != FILE_TYPE_PIPE) {
        PostMessageW(window, wm_disconnect, 0, 0);
        return;
    }
    Protocol protocol_handler(window);
    std::array<char, 4096> buffer{};
    std::string line;
    bool oversized = false;
    while (!stopping) {
        DWORD count{};
        if (!ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || !count) break;
        for (DWORD i = 0; i < count && !stopping; ++i) {
            const char ch = buffer[i];
            if (ch == '\n') {
                try {
                    const auto reply = oversized ? std::optional(rpc_error(nullptr, -32700, "Frame exceeds 64 KiB"))
                                                 : protocol_handler.line(line);
                    if (reply && !write_line(out, *reply)) {
                        PostMessageW(window, wm_disconnect, 0, 0);
                        return;
                    }
                } catch (const std::exception&) {
                    if (!write_line(out, rpc_error(nullptr, -32603, "AIMouse internal error"))) {
                        PostMessageW(window, wm_disconnect, 0, 0);
                        return;
                    }
                }
                line.clear();
                oversized = false;
            } else if (line.size() < max_line) {
                line.push_back(ch);
            } else {
                oversized = true;
            }
        }
    }
    PostMessageW(window, wm_disconnect, 0, 0);
}

} // namespace aimouse
