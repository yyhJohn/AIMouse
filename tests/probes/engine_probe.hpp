#pragma once

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace aimouse_probe {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Options {
    int timeout_ms{};
    bool foreground{};
    bool borderless{};
    bool relative{};
    bool capture{};
};

inline bool parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--foreground") options.foreground = true;
        else if (arg == "--borderless") options.borderless = true;
        else if (arg == "--relative") options.relative = true;
        else if (arg == "--capture") options.capture = true;
        else if (arg == "--timeout-ms" && index + 1 < argc) {
            const std::string_view value = argv[++index];
            const auto end = value.data() + value.size();
            const auto [position, error] = std::from_chars(value.data(), end, options.timeout_ms);
            if (error != std::errc{} || position != end || options.timeout_ms < 1) return false;
        } else {
            return false;
        }
    }
    return (!(options.relative || options.capture) || options.foreground) &&
           !(options.relative && options.capture);
}

inline int usage(const char* executable) {
    std::fprintf(stderr, "Usage: %s [--timeout-ms N] [--borderless] "
                         "[--foreground [--relative | --capture]]\n", executable);
    return 2;
}

inline unsigned long long hwnd_value(HWND hwnd) {
    return static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hwnd));
}

inline long long qpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline const char* json_bool(bool value) { return value ? "true" : "false"; }

struct Stats {
    unsigned long long moves{}, downs{}, ups{}, wheels{}, key_downs{}, key_ups{};
    unsigned long long drag_moves{}, focus_changes{}, double_clicks{};
    double x{}, y{}, dx{}, dy{}, wheel_x{}, wheel_y{};
    unsigned buttons{};
    bool has_position{};

    void move(double x_position, double y_position, double x_delta, double y_delta) {
        ++moves;
        if (buttons) ++drag_moves;
        x = x_position;
        y = y_position;
        dx += x_delta;
        dy += y_delta;
        has_position = true;
    }

    void button(unsigned index, bool pressed) {
        if (index >= 32) return;
        if (pressed) {
            ++downs;
            buttons |= 1u << index;
        } else {
            ++ups;
            buttons &= ~(1u << index);
        }
    }

    void scroll(double horizontal, double vertical) {
        ++wheels;
        wheel_x += horizontal;
        wheel_y += vertical;
    }

    void focus(bool focused) {
        ++focus_changes;
        if (!focused) {
            buttons = 0;
            has_position = false;
        }
    }
};

inline void ready(const char* mode, HWND window, Options const& options) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    RECT bounds{};
    GetClientRect(window, &bounds);
    POINT cursor{};
    const bool cursor_ok = GetCursorPos(&cursor) != FALSE;
    std::printf(
        "{\"event\":\"ready\",\"mode\":\"%s\",\"hwnd\":\"0x%llx\",\"qpc_frequency\":%lld,"
        "\"qpc\":%lld,\"foreground_hwnd\":\"0x%llx\",\"client_width\":%ld,\"client_height\":%ld,"
        "\"cursor_ok\":%s,\"cursor_x\":%ld,\"cursor_y\":%ld,\"borderless\":%s,"
        "\"relative_requested\":%s,\"capture_requested\":%s}\n",
        mode, hwnd_value(window), frequency.QuadPart, qpc(), hwnd_value(GetForegroundWindow()),
        bounds.right, bounds.bottom, json_bool(cursor_ok), cursor.x, cursor.y, json_bool(options.borderless),
        json_bool(options.relative), json_bool(options.capture));
}

inline void sample(const char* event, const char* mode, HWND window, Stats const& stats,
                   bool relative, bool cursor_confined) {
    POINT cursor{};
    const bool cursor_ok = GetCursorPos(&cursor) != FALSE;
    const HWND foreground = GetForegroundWindow();
    std::printf(
        "{\"event\":\"%s\",\"mode\":\"%s\",\"qpc\":%lld,\"foreground_hwnd\":\"0x%llx\","
        "\"active\":%s,\"cursor_ok\":%s,\"cursor_x\":%ld,\"cursor_y\":%ld,\"moves\":%llu,"
        "\"downs\":%llu,\"ups\":%llu,\"wheels\":%llu,\"drag_moves\":%llu,\"key_downs\":%llu,"
        "\"key_ups\":%llu,\"double_clicks\":%llu,\"focus_changes\":%llu,\"buttons\":%u,"
        "\"x\":%.3f,\"y\":%.3f,\"dx\":%.3f,\"dy\":%.3f,\"wheel_x\":%.3f,\"wheel_y\":%.3f,"
        "\"relative_active\":%s,\"cursor_confined\":%s,\"owns_capture\":%s}\n",
        event, mode, qpc(), hwnd_value(foreground), json_bool(foreground == window),
        json_bool(cursor_ok), cursor.x, cursor.y, stats.moves, stats.downs, stats.ups,
        stats.wheels, stats.drag_moves, stats.key_downs, stats.key_ups, stats.double_clicks,
        stats.focus_changes, stats.buttons, stats.x, stats.y, stats.dx, stats.dy, stats.wheel_x,
        stats.wheel_y, json_bool(relative), json_bool(cursor_confined), json_bool(GetCapture() == window));
}
}
