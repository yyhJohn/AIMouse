#include "session.hpp"

#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <limits>

namespace aimouse {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct ButtonMessages { UINT down, up, double_click, flag; };
constexpr ButtonMessages mouse_messages[] = {
    {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, MK_LBUTTON},
    {WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK, MK_RBUTTON},
    {WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK, MK_MBUTTON}
};
constexpr DWORD guest_down[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
constexpr DWORD guest_up[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};

INPUT guest_mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi = {dx, dy, data, flags, 0, 0};
    return event;
}

INPUT guest_key(int vk, UINT scan, bool down) {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    const DWORD extended = (scan & 0xff00) == 0xe000 ? KEYEVENTF_EXTENDEDKEY : 0;
    event.ki = {static_cast<WORD>(vk), static_cast<WORD>(scan & 0xff),
                extended | (down ? 0 : KEYEVENTF_KEYUP), 0, 0};
    return event;
}

int button_id(const Json& args) {
    const auto value = args.value("button", std::string("left"));
    if (value == "right") return 2;
    if (value == "middle") return 3;
    return 1;
}

bool cloaked(HWND window) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
}

bool usable(HWND window) {
    return IsWindowVisible(window) && IsWindowEnabled(window) && !IsIconic(window) &&
        !cloaked(window) && GetAncestor(window, GA_ROOT) == window && window != GetDesktopWindow();
}

bool cooperative(HWND window) {
    return GetPropW(window, game_property) == reinterpret_cast<HANDLE>(1);
}

bool within(POINT p, RECT r) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

HWND thread_capture(DWORD tid) {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(tid, &info) ? info.hwndCapture : nullptr;
}

bool same_process(HWND window, DWORD pid) {
    DWORD actual{};
    return GetWindowThreadProcessId(window, &actual) && actual == pid;
}

LPARAM pack_point(POINT p) {
    if (p.x < SHRT_MIN || p.x > SHRT_MAX || p.y < SHRT_MIN || p.y > SHRT_MAX) {
        throw ToolError("coordinate_range", "Win32 message coordinates must fit signed 16-bit fields.");
    }
    return MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y));
}

LPARAM key_bits(UINT scan, bool released) {
    return 1 | ((scan & 0xff) << 16) | (((scan & 0xff00) == 0xe000) ? (1u << 24) : 0) |
        (released ? (3u << 30) : 0);
}

HWND parse_hwnd(const std::string& text) {
    std::uintptr_t value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    if (text.starts_with("0x")) begin += 2;
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 || value == 0xffff) {
        throw ToolError("invalid_window", "Use a hexadecimal HWND from ai_list_windows.");
    }
    return reinterpret_cast<HWND>(value);
}

Json describe_window(HWND window) {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    RECT rect{};
    GetClientRect(window, &rect);
    return {{"hwnd", hwnd_string(window)}, {"pid", pid}, {"title", utf8(title(window))},
            {"class", utf8(window_class(window))}, {"client_width", rect.right},
            {"client_height", rect.bottom}, {"foreground", window == GetForegroundWindow()},
            {"cooperative_game", cooperative(window)}};
}

BOOL CALLBACK enumerate(HWND window, LPARAM data) {
    auto& result = *reinterpret_cast<Json*>(data);
    if (result.size() >= 100) return FALSE;
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() || !usable(window) || title(window).empty()) return TRUE;
    result.push_back(describe_window(window));
    return TRUE;
}

std::uint64_t creation_time(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    return ok ? (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime : 0;
}

std::string iso_utc() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[40]{};
    snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
             time.wSecond, time.wMilliseconds);
    return buffer;
}

} // namespace

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw ToolError("invalid_text", "Text must be valid UTF-8.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring title(HWND window) {
    wchar_t buffer[512]{};
    const int size = GetWindowTextW(window, buffer, static_cast<int>(std::size(buffer)));
    return {buffer, static_cast<std::size_t>(size)};
}

std::wstring window_class(HWND window) {
    wchar_t buffer[256]{};
    const int size = GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
    return {buffer, static_cast<std::size_t>(size)};
}

std::string hwnd_string(HWND window) {
    char buffer[2 + sizeof(window) * 2]{};
    buffer[0] = '0';
    buffer[1] = 'x';
    const auto end = std::to_chars(buffer + 2, buffer + sizeof(buffer),
                                   reinterpret_cast<std::uintptr_t>(window), 16).ptr;
    return {buffer, end};
}

std::optional<WindowIdentity> WindowIdentity::read(HWND window) {
    DWORD pid{};
    const auto tid = GetWindowThreadProcessId(window, &pid);
    if (!tid || pid == GetCurrentProcessId()) return std::nullopt;
    auto klass = window_class(window);
    const auto created = creation_time(pid);
    if (klass.empty() || !created) return std::nullopt;
    return WindowIdentity{window, pid, tid, std::move(klass), created};
}

bool WindowIdentity::alive() const {
    DWORD actual_pid{};
    return GetWindowThreadProcessId(hwnd, &actual_pid) == tid && actual_pid == pid &&
        window_class(hwnd) == klass && creation_time(pid) == process_created;
}

Session::Session(HWND control, bool guest_enabled) : control_(control), guest_enabled_(guest_enabled) {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) return;
    const auto path = std::filesystem::path(folder) / L"AIMouse" / L"logs";
    CoTaskMemFree(folder);
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) return;
    audit_path_ = path / (L"session-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    audit_file_.open(audit_path_, std::ios::app);
}

Session::~Session() {
    release_all();
}

void Session::audit(std::string_view operation, std::string_view result) {
    last_event_ = operation;
    last_result_ = result;
    if (!audit_file_) return;
    audit_file_ << Json({{"time", iso_utc()}, {"operation", operation}, {"result", result},
                         {"armed", armed_}, {"target", hwnd_string(target())}}).dump() << '\n';
    audit_file_.flush();
}

Json Session::status() const {
    return {{"armed", armed_}, {"arm_requested", arm_requested_}, {"user_exists", user_},
            {"mode", mode_ == Mode::desktop ? "desktop" : mode_ == Mode::game ? "game" : "isolated"},
            {"target", target_ ? describe_window(target_->hwnd) : Json(nullptr)},
            {"logical_position", {{"x", point_.x}, {"y", point_.y}, {"space", "target_client"}}},
            {"cursor_visible", cursor_visible_}, {"held_buttons", buttons_.size()},
            {"held_keys", keys_.size()}, {"screen_capture_allowed", screen_allowed()},
            {"cleanup_delivery_uncertain", release_pending_},
            {"audit_log", audit_file_ ? Json(utf8(audit_path_.wstring())) : Json(nullptr)},
            {"last_operation", last_event_}, {"last_result", last_result_}};
}

Json Session::capabilities() const {
    const bool game = mode_ == Mode::game;
    const bool guest = mode_ == Mode::isolated;
    const bool ready = target_ && target_->alive() && usable(target_->hwnd) &&
        (!game || cooperative(target_->hwnd)) && (!guest || target_->hwnd == GetForegroundWindow());
    auto modes = Json::array({"desktop", "game"});
    if (guest_enabled_) modes.push_back("isolated");
    return {{"backend", guest ? "guest_sendinput" : game ? "cooperative_game" : "window_messages"},
            {"target_ready", ready}, {"requires_target_support", true},
            {"supports_background_click", ready && !guest}, {"supports_relative_motion", ready && (game || guest)},
            {"supports_relative_cursor_coordinates", ready && !game && !guest},
            {"supports_keyboard", ready},
            {"keyboard_semantics", guest ? "guest_global" : game ? "application_state" : "window_messages"},
            {"supports_concurrent_input", ready && !guest},
            {"changes_foreground_focus", guest}, {"changes_system_cursor", guest},
            {"receiver_can_change_focus_or_cursor", true}, {"requires_admin", false},
            {"requires_driver", false}, {"injects_raw_input", false}, {"injects_directinput", false},
            {"system_input_isolation", guest ? "guest_os_boundary" : "no_global_injection_by_backend"},
            {"available_modes", std::move(modes)}, {"isolated_mode", guest_enabled_ ? "guest_available" : "requires_guest"}};
}

void Session::bind(HWND window) {
    if (!user_) throw ToolError("no_user", "Create the AI user first.");
    auto identity = WindowIdentity::read(window);
    if (!identity || !usable(window)) {
        throw ToolError("invalid_window", "Choose a visible, enabled, accessible top-level window.");
    }
    local_stop("binding_changed");
    target_ = std::move(identity);
    RECT rect{};
    GetClientRect(window, &rect);
    point_ = {rect.right / 2, rect.bottom / 2};
}

void Session::local_arm() {
    if (!user_ || !target_ || !target_->alive() || !usable(target_->hwnd) ||
        (mode_ == Mode::game && !cooperative(target_->hwnd))) {
        audit("local_arm", "target_unavailable");
        return;
    }
    armed_ = true;
    arm_requested_ = false;
    audit("local_arm", "armed");
}

void Session::local_stop(std::string_view reason) {
    ++generation_;
    armed_ = false;
    arm_requested_ = false;
    screen_allowed_ = false;
    release_all();
    audit("disarm", reason);
}

void Session::tick() {
    if (target_ && (!target_->alive() || !usable(target_->hwnd))) {
        local_stop("target_lost");
        target_.reset();
    }
    if (armed_ && mode_ == Mode::isolated && (!buttons_.empty() || !keys_.empty()) &&
        GetForegroundWindow() != target()) {
        local_stop("guest_focus_lost");
    }
}

WindowIdentity Session::require_target() {
    tick();
    if (!target_) throw ToolError("no_target", "Bind a compatible target window.");
    return *target_;
}

void Session::require_armed() {
    if (!user_) throw ToolError("no_user", "Create the AI user first.");
    require_target();
    if (!armed_) throw ToolError("disarmed", "Arm this target in the AIMouse control panel.");
    if (mode_ == Mode::isolated && GetForegroundWindow() != target()) {
        throw ToolError("guest_foreground_required", "Focus the bound game in the guest to send input.");
    }
}

HWND Session::capture_target() const {
    if (!armed_ || !target_ || !target_->alive() || !usable(target_->hwnd)) {
        throw ToolError("disarmed", "Arm the bound window before capturing it.");
    }
    return target_->hwnd;
}

std::optional<POINT> Session::overlay_point() const {
    if (!user_ || !cursor_visible_ || !target_ || mode_ != Mode::desktop) return std::nullopt;
    POINT p = point_;
    if (!ClientToScreen(target_->hwnd, &p)) return std::nullopt;
    if (!MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) return std::nullopt;
    return p;
}

WindowIdentity Session::recipient_at(POINT point) {
    auto root = require_target();
    RECT bounds{};
    if (!GetClientRect(root.hwnd, &bounds) || !within(point, bounds)) {
        throw ToolError("outside_target", "Move the AI cursor into the current client area.");
    }
    POINT screen = point;
    if (!ClientToScreen(root.hwnd, &screen)) throw ToolError("coordinate_error", "Cannot map target coordinates.");
    HWND current = root.hwnd;
    for (int depth = 0; depth < 16; ++depth) {
        POINT local = screen;
        if (!ScreenToClient(current, &local)) break;
        const HWND child = ChildWindowFromPointEx(current, local,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == current) break;
        DWORD pid{};
        GetWindowThreadProcessId(child, &pid);
        if (pid != root.pid) break;
        current = child;
    }
    auto receiver = WindowIdentity::read(current);
    if (!receiver || receiver->pid != root.pid || GetAncestor(current, GA_ROOT) != root.hwnd) {
        throw ToolError("target_changed", "The target control changed before dispatch.");
    }
    return *receiver;
}

LPARAM Session::client_lparam(HWND recipient, POINT point) const {
    if (!ClientToScreen(target(), &point) || !ScreenToClient(recipient, &point)) {
        throw ToolError("coordinate_error", "Cannot convert target coordinates.");
    }
    return pack_point(point);
}

void Session::send(const WindowIdentity& recipient, UINT msg, WPARAM wp, LPARAM lp, bool* attempted) {
    if (!recipient.alive() || GetAncestor(recipient.hwnd, GA_ROOT) != target()) {
        throw ToolError("target_changed", "The bound target or recipient was replaced.");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now());
    if (remaining <= 0ms) throw ToolError("timeout", "Operation time budget expired.");
    const auto timeout = static_cast<UINT>(std::min(remaining, 75ms).count());
    const HWND previous = GetForegroundWindow();
    if (thread_capture(recipient.tid)) {
        throw ToolError("target_busy_capture", "The recipient thread already owns a Windows mouse capture.");
    }
    if (attempted) *attempted = true;
    SetLastError(ERROR_SUCCESS);
    if (!SendMessageTimeoutW(recipient.hwnd, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                             timeout, nullptr)) {
        const DWORD error = GetLastError();
        throw ToolError(error == ERROR_ACCESS_DENIED ? "access_denied" : "delivery_uncertain",
                        "Window message did not complete. Win32 error " + std::to_string(error) + ".");
    }
    capture_seen_after_dispatch_ = thread_capture(recipient.tid);
    const HWND foreground = GetForegroundWindow();
    if (foreground != previous && same_process(foreground, recipient.pid)) {
        throw ToolError("focus_changed", "The target became foreground during dispatch. AIMouse disarmed.");
    }
    if (capture_seen_after_dispatch_) {
        throw ToolError("target_captured", "The target acquired Windows mouse capture during dispatch. AIMouse disarmed.");
    }
}

void Session::send_game(std::uint32_t op, int a, int b, int c) {
    const auto root = require_target();
    if (!cooperative(root.hwnd)) {
        throw ToolError("unsupported_backend", "Target does not advertise AIMouse.GameInput.v1.");
    }
    struct Event { std::uint32_t op; std::int32_t a, b, c; };
    static_assert(sizeof(Event) == 16);
    Event event{op, a, b, c};
    COPYDATASTRUCT data{game_message, sizeof(event), &event};
    DWORD_PTR ack{};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now());
    if (remaining <= 0ms) throw ToolError("timeout", "Operation time budget expired.");
    const auto timeout = static_cast<UINT>(std::min(remaining, 75ms).count());
    const HWND previous = GetForegroundWindow();
    if (!SendMessageTimeoutW(root.hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(control_),
            reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, timeout, &ack)) {
        throw ToolError("delivery_uncertain", "The game did not complete the event within 75 ms.");
    }
    if (ack != game_ack) throw ToolError("receiver_rejected", "The game did not acknowledge the event.");
    const HWND foreground = GetForegroundWindow();
    if (foreground != previous && same_process(foreground, root.pid)) {
        throw ToolError("focus_changed", "The game became foreground during dispatch. AIMouse disarmed.");
    }
}

void Session::send_guest(INPUT event) {
    const auto root = require_target();
    if (!guest_enabled_ || !armed_ || root.hwnd != GetForegroundWindow()) {
        throw ToolError("guest_focus_lost", "The bound guest window lost focus before dispatch.");
    }
    if (Clock::now() >= deadline_) throw ToolError("timeout", "Operation time budget expired.");
    if (event.type == INPUT_MOUSE && (event.mi.dwFlags &
        (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_MIDDLEDOWN |
         MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL))) {
        POINT point{};
        if (!GetCursorPos(&point) || GetAncestor(WindowFromPoint(point), GA_ROOT) != root.hwnd) {
            throw ToolError("guest_pointer_outside", "Place the guest pointer inside the bound foreground game.");
        }
    }
    if (SendInput(1, &event, sizeof(INPUT)) != 1) {
        throw ToolError("delivery_uncertain", "Windows did not accept the guest input.");
    }
}

WPARAM Session::button_flags() const {
    WPARAM flags{};
    for (const auto& [button, held] : buttons_) flags |= mouse_messages[button - 1].flag;
    if (keys_.contains(VK_SHIFT) || keys_.contains(VK_LSHIFT) || keys_.contains(VK_RSHIFT)) flags |= MK_SHIFT;
    if (keys_.contains(VK_CONTROL) || keys_.contains(VK_LCONTROL) || keys_.contains(VK_RCONTROL)) flags |= MK_CONTROL;
    return flags;
}

void Session::move(POINT point) {
    if (mode_ != Mode::desktop) throw ToolError("unsupported", "The game backend accepts relative movement.");
    RECT bounds{};
    if (!GetClientRect(target(), &bounds) || !within(point, bounds)) {
        throw ToolError("outside_target", "Point must be inside the bound client area.");
    }
    auto receiver = buttons_.empty() ? recipient_at(point) : buttons_.begin()->second.recipient;
    const LPARAM coordinates = client_lparam(receiver.hwnd, point);
    send(receiver, WM_MOUSEMOVE, button_flags(), coordinates);
    point_ = point;
    for (auto& [button, held] : buttons_) held.point = point;
}

void Session::move_relative(int dx, int dy) {
    if (mode_ == Mode::isolated) {
        send_guest(guest_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE, dx, dy));
        return;
    }
    if (mode_ == Mode::game) {
        send_game(1, dx, dy);
        return;
    }
    move({point_.x + dx, point_.y + dy});
}

void Session::mouse_down(int button, bool double_click) {
    if (buttons_.contains(button)) throw ToolError("already_held", "This AI mouse button is already held.");
    const bool game = mode_ != Mode::desktop;
    const auto receiver = game ? require_target() :
        (buttons_.empty() ? recipient_at(point_) : buttons_.begin()->second.recipient);
    const auto lp = game ? 0 : client_lparam(receiver.hwnd, point_);
    if (mode_ == Mode::isolated) send_guest(guest_mouse(guest_down[button - 1]));
    buttons_.emplace(button, HeldButton{receiver, point_});
    if (mode_ == Mode::isolated) return;
    if (game) {
        send_game(2, button, 1);
        return;
    }
    const auto& msg = mouse_messages[button - 1];
    bool attempted{};
    try {
        send(receiver, double_click ? msg.double_click : msg.down, button_flags(), lp, &attempted);
    } catch (...) {
        if (!attempted) buttons_.erase(button);
        throw;
    }
}

void Session::mouse_up(int button) {
    const auto held = buttons_.find(button);
    if (held == buttons_.end()) throw ToolError("not_held", "This AI mouse button is not held.");
    if (mode_ == Mode::isolated) {
        send_guest(guest_mouse(guest_up[button - 1]));
    } else if (mode_ == Mode::game) {
        send_game(2, button, 0);
    } else {
        const auto& msg = mouse_messages[button - 1];
        send(held->second.recipient, msg.up, button_flags() & ~static_cast<WPARAM>(msg.flag),
             client_lparam(held->second.recipient.hwnd, point_));
    }
    buttons_.erase(held);
}

void Session::key(int vk, bool down) {
    auto held = keys_.find(vk);
    if (down && held != keys_.end()) throw ToolError("already_held", "This AI key is already held.");
    if (!down && held == keys_.end()) throw ToolError("not_held", "This AI key is not held.");
    if (down && keys_.size() >= 8) throw ToolError("held_key_limit", "At most eight AI keys may be held.");
    if (down) {
        const auto receiver = mode_ != Mode::desktop ? require_target() :
            (keys_.empty() ? recipient_at(point_) : keys_.begin()->second.recipient);
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        if (!scan) throw ToolError("invalid_key", "This virtual key has no supported scan code.");
        if (mode_ == Mode::isolated) {
            send_guest(guest_key(vk, scan, true));
            keys_.emplace(vk, HeldKey{receiver, scan});
            return;
        }
        held = keys_.emplace(vk, HeldKey{receiver, scan}).first;
    }
    if (mode_ == Mode::isolated) send_guest(guest_key(vk, held->second.scan, down));
    else if (mode_ == Mode::game) send_game(3, vk, down ? 1 : 0);
    else {
        bool attempted{};
        try {
            send(held->second.recipient, down ? WM_KEYDOWN : WM_KEYUP, vk,
                 key_bits(held->second.scan, !down), &attempted);
        } catch (...) {
            if (down && !attempted) keys_.erase(vk);
            throw;
        }
    }
    if (!down) keys_.erase(held);
}

void Session::wheel(int delta, bool horizontal) {
    if (delta == 0) throw ToolError("invalid_delta", "Wheel delta must be nonzero.");
    if (mode_ == Mode::isolated) {
        send_guest(guest_mouse(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(delta)));
        return;
    }
    if (mode_ == Mode::game) {
        send_game(4, delta, horizontal ? 1 : 0);
        return;
    }
    auto receiver = buttons_.empty() ? recipient_at(point_) : buttons_.begin()->second.recipient;
    POINT p = point_;
    if (!ClientToScreen(target(), &p)) throw ToolError("coordinate_error", "Cannot resolve wheel screen coordinates.");
    send(receiver, horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL,
         MAKEWPARAM(button_flags(), static_cast<short>(delta)), pack_point(p));
}

void Session::release_all() {
    if (buttons_.empty() && keys_.empty()) {
        capture_seen_after_dispatch_ = nullptr;
        return;
    }
    release_pending_ = false;
    if (mode_ == Mode::isolated && guest_enabled_) {
        // Focus may already be gone. Clear AIMouse's held states in the guest input stream.
        INPUT events[11]{};
        UINT count{};
        for (const auto& [button, held] : buttons_) events[count++] = guest_mouse(guest_up[button - 1]);
        for (const auto& [vk, held] : keys_) events[count++] = guest_key(vk, held.scan, false);
        release_pending_ = SendInput(count, events, sizeof(INPUT)) != count;
    } else if (mode_ == Mode::game) {
        if (target_ && target_->alive() && cooperative(target_->hwnd)) {
            // Reset may be attempted even if the game window became hidden or disabled.
            struct Reset { std::uint32_t op{5}; std::int32_t rest[3]{}; } reset;
            COPYDATASTRUCT data{game_message, sizeof(reset), &reset};
            DWORD_PTR ack{};
            release_pending_ = !SendMessageTimeoutW(target_->hwnd, WM_COPYDATA,
                reinterpret_cast<WPARAM>(control_), reinterpret_cast<LPARAM>(&data),
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 75, &ack) || ack != game_ack;
        }
    } else {
        for (const auto& [button, held] : buttons_) {
            if (!held.recipient.alive() || !target_ || !target_->alive() ||
                GetAncestor(held.recipient.hwnd, GA_ROOT) != target()) continue;
            const HWND capture = thread_capture(held.recipient.tid);
            if (capture && capture != capture_seen_after_dispatch_) {
                release_pending_ = true;
                continue;
            }
            try {
                release_pending_ |= !PostMessageW(held.recipient.hwnd, mouse_messages[button - 1].up, 0,
                    client_lparam(held.recipient.hwnd, held.point));
            } catch (const ToolError&) {
                release_pending_ = true;
            }
        }
        for (const auto& [vk, held] : keys_) {
            if (!held.recipient.alive() || !target_ || !target_->alive() ||
                GetAncestor(held.recipient.hwnd, GA_ROOT) != target()) continue;
            release_pending_ |= !PostMessageW(held.recipient.hwnd, WM_KEYUP, vk, key_bits(held.scan, true));
        }
    }
    buttons_.clear();
    keys_.clear();
    capture_seen_after_dispatch_ = nullptr;
}

Json Session::delivered() const {
    return {{"delivery", mode_ == Mode::isolated ? "guest_input_queued" :
                            mode_ == Mode::game ? "receiver_acknowledged" : "window_proc_returned"},
            {"receiver_ack", mode_ == Mode::game}, {"effect_verified", false},
            {"x", point_.x}, {"y", point_.y},
            {"held_buttons", buttons_.size()}, {"held_keys", keys_.size()}};
}

Json Session::call(std::string_view op, const Json& args) {
    tick();
    if (op == "ai_get_status") return status();
    if (op == "ai_get_input_capabilities") return capabilities();
    if (op == "ai_list_users") return {{"users", user_ ? Json({"human", "ai"}) : Json({"human"})}};
    if (op == "ai_create_user") {
        if (user_) throw ToolError("user_exists", "The single AI user already exists.");
        user_ = true;
        cursor_visible_ = true;
        cursor_color_ = RGB(0, 192, 233);
        return {{"user", "ai"}};
    }
    if (op == "ai_destroy_user") {
        local_stop("user_destroyed");
        user_ = false;
        target_.reset();
        return status();
    }
    if (op == "ai_list_windows") {
        auto windows = Json::array();
        EnumWindows(enumerate, reinterpret_cast<LPARAM>(&windows));
        return {{"windows", std::move(windows)}};
    }
    if (op == "ai_bind_window") {
        bind(parse_hwnd(args.at("hwnd").get<std::string>()));
        return status();
    }
    if (op == "ai_unbind_window") {
        local_stop("unbound");
        target_.reset();
        return status();
    }
    if (op == "ai_select_mode") {
        const auto mode = args.at("mode").get<std::string>();
        if (mode == "isolated" && !guest_enabled_) {
            throw ToolError("guest_required", "Start AIMouse with --vm-stdio inside an interactive VMware Windows guest.");
        }
        local_stop("mode_changed");
        mode_ = mode == "desktop" ? Mode::desktop : mode == "game" ? Mode::game : Mode::isolated;
        return capabilities();
    }
    if (op == "ai_detect_input_backend") {
        const auto root = require_target();
        return {{"target", describe_window(root.hwnd)}, {"raw_input", "unknown"},
                {"directinput", "unknown"}, {"window_messages", "unverified"},
                {"cooperative_game", cooperative(root.hwnd) ? "advertised" : "not_advertised"}};
    }
    if (op == "ai_arm") {
        require_target();
        if (mode_ == Mode::game && !cooperative(target())) {
            throw ToolError("unsupported_backend", "Select a game with an AIMouse.GameInput.v1 adapter.");
        }
        arm_requested_ = !armed_;
        return status();
    }
    if (op == "ai_disarm" || op == "ai_emergency_stop") {
        local_stop(op);
        return status();
    }
    if (op == "ai_set_cursor") {
        if (!user_) throw ToolError("no_user", "Create the AI user first.");
        cursor_visible_ = args.at("visible").get<bool>();
        if (args.contains("rgb")) {
            const auto rgb = args.at("rgb").get<unsigned>();
            cursor_color_ = RGB(rgb >> 16, rgb >> 8, rgb);
        }
        return status();
    }

    require_armed();
    deadline_ = Clock::now() + 350ms;
    try {
        if (op == "ai_move_absolute") move({args.at("x"), args.at("y")});
        else if (op == "ai_move_relative") move_relative(args.at("dx"), args.at("dy"));
        else if (op == "ai_mouse_down") mouse_down(button_id(args));
        else if (op == "ai_mouse_up") mouse_up(button_id(args));
        else if (op == "ai_click" || op == "ai_double_click") {
            const int button = button_id(args);
            mouse_down(button);
            mouse_up(button);
            if (op == "ai_double_click") {
                mouse_down(button, true);
                mouse_up(button);
            }
        } else if (op == "ai_scroll") {
            wheel(args.at("delta"), args.value("horizontal", false));
        } else if (op == "ai_key_down" || op == "ai_key_up") {
            key(args.at("vk"), op == "ai_key_down");
        } else if (op == "ai_drag") {
            const int button = button_id(args);
            if (!buttons_.empty()) throw ToolError("already_held", "Release held mouse buttons before dragging.");
            const bool game = mode_ != Mode::desktop;
            const POINT start = game ? POINT{} : point_;
            const POINT end{args.at("x"), args.at("y")};
            const int steps = args.value("steps", 8);
            if (!game) {
                RECT rect{};
                if (!GetClientRect(target(), &rect) || !within(end, rect)) {
                    throw ToolError("outside_target", "Drag endpoint must be inside the bound client area.");
                }
            }
            mouse_down(button);
            POINT last = start;
            for (int i = 1; i <= steps; ++i) {
                const POINT next{start.x + (end.x - start.x) * i / steps,
                                 start.y + (end.y - start.y) * i / steps};
                if (game) move_relative(next.x - last.x, next.y - last.y);
                else move(next);
                last = next;
            }
            mouse_up(button);
        } else if (op == "ai_type_text") {
            if (mode_ != Mode::desktop) throw ToolError("unsupported", "The game backend supports key states only.");
            const auto receiver = recipient_at(point_);
            for (wchar_t ch : wide(args.at("text").get<std::string>())) send(receiver, WM_CHAR, ch, 1);
        } else {
            throw ToolError("unknown_tool", "Unknown operation.");
        }
    } catch (const ToolError& error) {
        if (error.code == "delivery_uncertain" || error.code == "timeout" ||
            error.code == "target_changed" || error.code == "focus_changed" ||
            error.code == "access_denied" || error.code == "receiver_rejected" ||
            error.code == "guest_focus_lost" || error.code == "guest_pointer_outside" ||
            error.code == "target_captured" || error.code == "target_busy_capture") {
            local_stop(error.code);
        }
        throw;
    }
    return delivered();
}

} // namespace aimouse
