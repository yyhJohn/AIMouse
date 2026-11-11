#pragma once

#include "session.hpp"

#include <atomic>

namespace aimouse {

struct McpCall {
    std::string name;
    Json args;
    Json result;
    std::optional<WindowIdentity> capture_window;
    std::optional<std::uint64_t> capture_generation;
    bool capture_check{};
    bool capture_screen{};
    bool error{};
};

inline constexpr UINT wm_tool_call = WM_APP + 1;
inline constexpr UINT wm_disconnect = WM_APP + 2;

void serve_stdio(HWND window, const std::atomic_bool& stopping);
McpCall* claim_tool_call(LPARAM parameter);
void dispatch_tool(Session& session, McpCall& call);

struct Image {
    unsigned width{}, height{};
    std::string base64;
};

Image capture_image(const std::optional<WindowIdentity>& window);

} // namespace aimouse
