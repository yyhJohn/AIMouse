#include "mcp.hpp"

#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <thread>

namespace aimouse {
namespace {

constexpr wchar_t panel_class[] = L"AIMouse.ControlPanel";
constexpr wchar_t cursor_class[] = L"AIMouse.CursorOverlay";
constexpr int arm_id = 101;
constexpr int disarm_id = 102;
constexpr int stop_id = 103;
constexpr int screen_id = 104;
constexpr int stop_hotkey = 1;

LRESULT CALLBACK overlay_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, msg, wp, lp);
}

class Overlay {
public:
    explicit Overlay(HINSTANCE instance) {
        WNDCLASSW klass{};
        klass.hInstance = instance;
        klass.lpfnWndProc = overlay_proc;
        klass.lpszClassName = cursor_class;
        RegisterClassW(&klass);
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE, cursor_class, L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (window_) SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);
    }

    ~Overlay() { if (window_) DestroyWindow(window_); }

    void render(std::optional<POINT> point, COLORREF color) {
        if (!window_) return;
        if (!point) {
            if (shown_) ShowWindow(window_, SW_HIDE);
            shown_ = false;
            return;
        }
        if (shown_ && point->x == position_.x && point->y == position_.y && color == color_) return;
        SetWindowPos(window_, HWND_TOPMOST, point->x, point->y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        const int size = std::clamp(MulDiv(23, GetDpiForWindow(window_), 96), 16, 72);
        if (shown_ && size == size_ && color == color_) {
            position_ = *point;
            return;
        }
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = size;
        bitmap.bmiHeader.biHeight = -size;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        void* raw{};
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return;
        HBITMAP dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &raw, nullptr, 0);
        if (dib) {
            auto* pixels = static_cast<std::uint32_t*>(raw);
            const auto body = 0xff000000u | (static_cast<unsigned>(GetRValue(color)) << 16) |
                (static_cast<unsigned>(GetGValue(color)) << 8) | GetBValue(color);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const int px = x * 23 / size;
                    const int py = y * 23 / size;
                    const bool edge = px <= py && py <= 20 - px / 2 && px < 13;
                    const bool inside = px > 0 && px + 1 < py && py < 19 - px / 2 && px < 11;
                    pixels[y * size + x] = inside ? body : edge ? 0xff102631u : 0u;
                }
            }
            const auto old = SelectObject(dc, dib);
            POINT source{};
            SIZE dimensions{size, size};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            if (UpdateLayeredWindow(window_, nullptr, &*point, &dimensions, dc, &source, 0, &blend, ULW_ALPHA)) {
                if (!shown_) ShowWindow(window_, SW_SHOWNOACTIVATE);
                position_ = *point;
                color_ = color;
                size_ = size;
                shown_ = true;
            }
            SelectObject(dc, old);
            DeleteObject(dib);
        }
        DeleteDC(dc);
    }

private:
    HWND window_{};
    POINT position_{};
    COLORREF color_{};
    int size_{};
    bool shown_{};
};

class Panel {
public:
    Panel(HINSTANCE instance, bool mcp, bool guest) : instance_(instance), mcp_(mcp), guest_(guest) {}

    bool open() {
        WNDCLASSW klass{};
        klass.hInstance = instance_;
        klass.lpfnWndProc = &Panel::window_proc;
        klass.lpszClassName = panel_class;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&klass);
        const UINT dpi = GetDpiForSystem();
        RECT rect{0, 0, MulDiv(580, dpi, 96), MulDiv(298, dpi, 96)};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
        window_ = CreateWindowExW(0, panel_class, L"AIMouse", style, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance_, this);
        if (!window_) return false;
        session_ = std::make_unique<Session>(window_, guest_);
        overlay_ = std::make_unique<Overlay>(instance_);
        status_ = control(L"STATIC", L"", SS_LEFT, 0);
        arm_ = control(L"BUTTON", L"Arm bound window", BS_PUSHBUTTON | WS_TABSTOP, arm_id);
        disarm_ = control(L"BUTTON", L"Disarm", BS_PUSHBUTTON | WS_TABSTOP, disarm_id);
        stop_ = control(L"BUTTON", L"Emergency stop", BS_PUSHBUTTON | WS_TABSTOP, stop_id);
        screen_ = control(L"BUTTON", L"Allow primary display capture", BS_AUTOCHECKBOX | WS_TABSTOP, screen_id);
        help_ = control(L"STATIC", L"", SS_LEFT, 0);
        hotkey_ = RegisterHotKey(window_, stop_hotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F11);
        layout();
        refresh();
        SetTimer(window_, 1, 33, nullptr);
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        return true;
    }

    int run() {
        std::thread worker;
        if (mcp_) worker = std::thread(serve_stdio, window_, std::cref(stopping_));
        MSG message{};
        BOOL result{};
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        stopping_ = true;
        if (worker.joinable()) {
            const auto native = worker.native_handle();
            while (WaitForSingleObject(native, 10) == WAIT_TIMEOUT) CancelSynchronousIo(native);
            worker.join();
        }
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    }

    ~Panel() { if (font_) DeleteObject(font_); }

private:
    HWND control(const wchar_t* klass, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    }

    void layout() {
        const auto dpi = GetDpiForWindow(window_);
        if (font_) DeleteObject(font_);
        font_ = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        const auto place = [dpi, this](HWND window, int x, int y, int width, int height) {
            MoveWindow(window, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
                MulDiv(width, dpi, 96), MulDiv(height, dpi, 96), TRUE);
            SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        };
        place(status_, 18, 16, 544, 135);
        place(arm_, 18, 159, 165, 36);
        place(disarm_, 192, 159, 118, 36);
        place(stop_, 319, 159, 164, 36);
        place(screen_, 18, 204, 450, 26);
        place(help_, 18, 242, 544, 52);
    }

    void refresh() {
        session_->tick();
        overlay_->render(session_->overlay_point(), session_->cursor_color());
        const auto target = session_->target();
        const auto window_title = target ? title(target).substr(0, 65) : L"No window bound";
        const auto arm_status = session_->armed() ? L"Armed" : session_->arm_requested()
            ? L"Disarmed (approval requested)" : L"Disarmed";
        const bool desktop = session_->mode() == Mode::desktop;
        const bool isolated = session_->mode() == Mode::isolated;
        std::wstring status = L"Human user   Physical mouse and keyboard\r\nAI user   ";
        status += session_->has_user() ? L"Created" : L"Inactive";
        status += L"   ·   ";
        status += arm_status;
        status += desktop ? L"\r\nDesktop   ·   Window messages" : isolated
            ? L"\r\nIsolated   ·   Global input inside this VMware guest" : L"\r\nGame   ·   Cooperative input";
        status += L"\r\nTarget   " + window_title + L"\r\nAI cursor   ";
        status += desktop && session_->cursor_visible() && target ? L"Visible" : L"Hidden";
        status += mcp_ ? L"   ·   MCP stdio\r\n" : L"   ·   MCP disconnected\r\n";
        if (!session_->last_event().empty()) {
            status += L"Last operation   " + wide(session_->last_event()) + L"  /  " + wide(session_->last_result());
        }
        if (status != last_status_) {
            SetWindowTextW(status_, status.c_str());
            last_status_ = std::move(status);
        }
        EnableWindow(arm_, target && !session_->armed());
        EnableWindow(disarm_, session_->armed() || session_->arm_requested());
        EnableWindow(screen_, session_->armed());
        SendMessageW(screen_, BM_SETCHECK, session_->screen_allowed() ? BST_CHECKED : BST_UNCHECKED, 0);
        const wchar_t* info = isolated
            ? (hotkey_ ? L"Guest system cursor and key state can change. Focus the bound game.\r\nCtrl+Alt+Shift+F11 stops AI input."
                : L"Guest system cursor and key state can change. Focus the bound game.\r\nUse Emergency stop. The global shortcut is unavailable.")
            : mcp_
            ? (hotkey_ ? L"Window-scoped input. A target may change its own focus.\r\nCtrl+Alt+Shift+F11 stops AI input."
                : L"Window-scoped input. A target may change its own focus.\r\nUse Emergency stop. The global shortcut is unavailable.")
            : L"Configure the MCP client to launch AIMouse.exe with --stdio.";
        if (last_help_ != info) {
            SetWindowTextW(help_, info);
            last_help_ = info;
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case wm_tool_call:
            if (auto* call = claim_tool_call(lp); call && session_) {
                dispatch_tool(*session_, *call);
                refresh();
                return TRUE;
            }
            return FALSE;
        case WM_COMMAND:
            if (!session_ || HIWORD(wp) != BN_CLICKED) break;
            switch (LOWORD(wp)) {
            case arm_id: session_->local_arm(); break;
            case disarm_id: session_->local_stop("local_disarm"); break;
            case stop_id: session_->local_stop("local_emergency_stop"); break;
            case screen_id:
                session_->set_screen_allowed(SendMessageW(screen_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                session_->audit("local_screen_consent", session_->screen_allowed() ? "allowed" : "revoked");
                break;
            default: return 0;
            }
            refresh();
            return 0;
        case WM_HOTKEY:
            if (wp == stop_hotkey && session_) {
                session_->local_stop("hotkey_emergency_stop");
                refresh();
            }
            return 0;
        case WM_TIMER:
            if (session_) refresh();
            return 0;
        case WM_DPICHANGED: {
            const auto* rect = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (status_) layout();
            return 0;
        }
        case wm_disconnect:
            if (session_) session_->local_stop("mcp_disconnected");
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            stopping_ = true;
            KillTimer(window_, 1);
            if (hotkey_) UnregisterHotKey(window_, stop_hotkey);
            if (session_) session_->local_stop("panel_closed");
            overlay_.reset();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, msg, wp, lp);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
    }

    HINSTANCE instance_{};
    HWND window_{}, status_{}, arm_{}, disarm_{}, stop_{}, screen_{}, help_{};
    HFONT font_{};
    std::unique_ptr<Session> session_;
    std::unique_ptr<Overlay> overlay_;
    std::wstring last_status_;
    std::wstring last_help_;
    bool mcp_{}, guest_{}, hotkey_{};
    std::atomic_bool stopping_{};
};

bool vmware_guest() {
    constexpr wchar_t bios[] = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    wchar_t manufacturer[128]{}, product[128]{};
    DWORD size = sizeof(manufacturer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, bios, L"SystemManufacturer", RRF_RT_REG_SZ,
                     nullptr, manufacturer, &size) != ERROR_SUCCESS) return false;
    size = sizeof(product);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, bios, L"SystemProductName", RRF_RT_REG_SZ,
                     nullptr, product, &size) != ERROR_SUCCESS) return false;
    return std::wstring_view(manufacturer).starts_with(L"VMware") &&
           std::wstring_view(product).starts_with(L"VMware");
}

} // namespace
} // namespace aimouse

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count{};
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    const std::wstring_view arg = args && count == 2 ? std::wstring_view(args[1]) : L"";
    const bool guest = arg == L"--vm-stdio";
    const bool mcp = arg == L"--stdio" || guest;
    if (args) LocalFree(args);
    if (guest && !aimouse::vmware_guest()) return 2;
    aimouse::Panel panel(instance, mcp, guest);
    return panel.open() ? panel.run() : 1;
}
