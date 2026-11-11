#pragma once

#include <windows.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace aimouse {

using Json = nlohmann::json;

class ToolError : public std::runtime_error {
public:
    ToolError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};

std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::wstring title(HWND window);
std::wstring window_class(HWND window);
std::string hwnd_string(HWND window);

struct WindowIdentity {
    HWND hwnd{};
    DWORD pid{};
    DWORD tid{};
    std::wstring klass;
    std::uint64_t process_created{};

    static std::optional<WindowIdentity> read(HWND window);
    bool alive() const;
};

enum class Mode { desktop, game, isolated };

class Session {
public:
    Session(HWND control, bool guest_enabled);
    ~Session();

    Json call(std::string_view operation, const Json& args);
    void local_arm();
    void local_stop(std::string_view reason);
    void tick();
    void audit(std::string_view operation, std::string_view result);

    HWND control() const { return control_; }
    HWND target() const { return target_ ? target_->hwnd : nullptr; }
    HWND capture_target() const;
    bool screen_allowed() const { return armed_ && screen_allowed_; }
    void set_screen_allowed(bool allowed) { screen_allowed_ = armed_ && allowed; }
    bool armed() const { return armed_; }
    std::uint64_t generation() const { return generation_; }
    bool arm_requested() const { return arm_requested_; }
    bool has_user() const { return user_; }
    bool cursor_visible() const { return cursor_visible_; }
    Mode mode() const { return mode_; }
    std::optional<POINT> overlay_point() const;
    COLORREF cursor_color() const { return cursor_color_; }
    std::string_view last_event() const { return last_event_; }
    std::string_view last_result() const { return last_result_; }

private:
    struct HeldButton {
        WindowIdentity recipient;
        POINT point{};
    };
    struct HeldKey {
        WindowIdentity recipient;
        UINT scan{};
    };

    Json status() const;
    Json capabilities() const;
    void bind(HWND window);
    WindowIdentity require_target();
    WindowIdentity recipient_at(POINT point);
    void require_armed();
    void move(POINT point);
    void move_relative(int dx, int dy);
    void mouse_down(int button, bool double_click = false);
    void mouse_up(int button);
    void key(int vk, bool down);
    void wheel(int delta, bool horizontal);
    void release_all();
    void send(const WindowIdentity& recipient, UINT msg, WPARAM wp, LPARAM lp, bool* attempted = nullptr);
    void send_game(std::uint32_t op, int a = 0, int b = 0, int c = 0);
    void send_guest(INPUT event);
    WPARAM button_flags() const;
    LPARAM client_lparam(HWND recipient, POINT point) const;
    Json delivered() const;

    HWND control_{};
    bool guest_enabled_{};
    std::optional<WindowIdentity> target_;
    std::map<int, HeldButton> buttons_;
    std::map<int, HeldKey> keys_;
    POINT point_{};
    bool user_{};
    bool armed_{};
    bool arm_requested_{};
    bool screen_allowed_{};
    bool cursor_visible_{true};
    COLORREF cursor_color_{RGB(0, 192, 233)};
    Mode mode_{Mode::desktop};
    std::chrono::steady_clock::time_point deadline_{};
    std::ofstream audit_file_;
    std::filesystem::path audit_path_;
    std::string last_event_;
    std::string last_result_;
    bool release_pending_{};
    HWND capture_seen_after_dispatch_{};
    std::uint64_t generation_{};
};

inline constexpr wchar_t game_property[] = L"AIMouse.GameInput.v1";
inline constexpr ULONG_PTR game_message = 0x41494D31;
inline constexpr LRESULT game_ack = 0x41494D32;

} // namespace aimouse
