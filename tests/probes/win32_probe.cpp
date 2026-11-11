#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <windowsx.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <numbers>
#include <string_view>
#include <unordered_set>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum class Mode { win32, raw, directinput, cooperative };

constexpr ULONG_PTR cooperative_protocol = 0x41494D31;
constexpr LRESULT cooperative_ack = 0x41494D32;
constexpr wchar_t cooperative_property[] = L"AIMouse.GameInput.v1";

struct CooperativeInput {
    uint32_t op;
    int32_t a;
    int32_t b;
    int32_t c;
};
static_assert(sizeof(CooperativeInput) == 16);

struct CooperativeOwner {
    HWND window{};
    DWORD process{};
    DWORD thread{};
};

struct Options {
    Mode mode{};
    int timeout_ms{};
    bool foreground{};
    bool borderless{};
    bool capturable{};
    bool input_sink{};
    bool background{};
    bool lock_cursor{};
    bool exclusive{};
    bool quiet{};
};

constexpr wchar_t const* window_class(Mode mode) {
    switch (mode) {
    case Mode::win32: return L"AIMouse.Win32Probe";
    case Mode::raw: return L"AIMouse.RawInputProbe";
    case Mode::directinput: return L"AIMouse.DirectInputProbe";
    case Mode::cooperative: return L"AIMouse.CooperativeProbe";
    }
    std::abort();
}

constexpr char const* mode_name(Mode mode) {
    switch (mode) {
    case Mode::win32: return "win32";
    case Mode::raw: return "raw";
    case Mode::directinput: return "directinput";
    case Mode::cooperative: return "cooperative";
    }
    std::abort();
}

bool parse_args(int argc, char** argv, Options& options) {
    if (argc < 2) return false;
    std::string_view mode = argv[1];
    if (mode == "raw") options.mode = Mode::raw;
    else if (mode == "directinput") options.mode = Mode::directinput;
    else if (mode == "cooperative") options.mode = Mode::cooperative;
    else if (mode != "win32") return false;

    for (int index = 2; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--foreground") options.foreground = true;
        else if (arg == "--borderless") options.borderless = true;
        else if (arg == "--capturable") options.capturable = true;
        else if (arg == "--input-sink") options.input_sink = true;
        else if (arg == "--background") options.background = true;
        else if (arg == "--lock-cursor") options.lock_cursor = true;
        else if (arg == "--exclusive") options.exclusive = true;
        else if (arg == "--quiet") options.quiet = true;
        else if (arg == "--timeout-ms" && index + 1 < argc) {
            const std::string_view value = argv[++index];
            const auto end = value.data() + value.size();
            const auto [position, error] = std::from_chars(value.data(), end, options.timeout_ms);
            if (error != std::errc{} || position != end || options.timeout_ms < 1) return false;
        } else {
            return false;
        }
    }
    return (!options.input_sink || options.mode == Mode::raw) &&
           (!options.background || options.mode == Mode::directinput) &&
           (!options.exclusive || (options.mode == Mode::directinput && options.foreground &&
                                   !options.background)) &&
           (!options.lock_cursor || (options.mode == Mode::raw && options.foreground &&
                                     !options.input_sink));
}

long long qpc() {
    LARGE_INTEGER count{};
    QueryPerformanceCounter(&count);
    return count.QuadPart;
}

unsigned long long hwnd_value(HWND hwnd) {
    return static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hwnd));
}

char const* boolean(bool value) {
    return value ? "true" : "false";
}

struct Point3 {
    double x{};
    double y{};
    double z{};
};

struct Camera {
    double yaw{};
    double pitch{};
    Point3 position{0, 1.6, -4};

    double yaw_radians() const { return yaw * std::numbers::pi / 180.0; }
    double pitch_radians() const { return pitch * std::numbers::pi / 180.0; }

    void look(long dx, long dy) {
        constexpr double sensitivity = 0.15;
        yaw = std::remainder(yaw + dx * sensitivity, 360.0);
        pitch = std::clamp(pitch - dy * sensitivity, -88.0, 88.0);
    }

    void step(int ahead, int right, double seconds) {
        const double length = std::hypot(ahead, right);
        if (length == 0) return;
        const double speed = 4.0 * seconds / length;
        const double angle = yaw_radians();
        position.x += (std::sin(angle) * ahead + std::cos(angle) * right) * speed;
        position.z += (std::cos(angle) * ahead - std::sin(angle) * right) * speed;
    }
};

struct View {
    double cy{}, sy{}, cp{}, sp{};
    Point3 position{};
    LONG width{}, height{};
    double focal{};

    View(Camera const& camera, RECT rect)
        : cy(std::cos(camera.yaw_radians())), sy(std::sin(camera.yaw_radians())),
          cp(std::cos(camera.pitch_radians())), sp(std::sin(camera.pitch_radians())),
          position(camera.position), width(rect.right), height(rect.bottom),
          focal(0.75 * (std::min)(width, height)) {}

    Point3 camera_space(Point3 point) const {
        const double x = point.x - position.x;
        const double y = point.y - position.y;
        const double z = point.z - position.z;
        const double forward = x * sy + z * cy;
        return {x * cy - z * sy, y * cp - forward * sp, y * sp + forward * cp};
    }

    static void clip_near(Point3& near_point, Point3 far_point, double plane) {
        const double fraction = (plane - near_point.z) / (far_point.z - near_point.z);
        near_point.x += (far_point.x - near_point.x) * fraction;
        near_point.y += (far_point.y - near_point.y) * fraction;
        near_point.z = plane;
    }

    POINT project(Point3 point) const {
        return {static_cast<LONG>(std::clamp(width / 2.0 + point.x * focal / point.z,
                                            -1000000.0, 1000000.0)),
                static_cast<LONG>(std::clamp(height / 2.0 - point.y * focal / point.z,
                                            -1000000.0, 1000000.0))};
    }

    void line(HDC dc, Point3 start, Point3 end) const {
        constexpr double near_plane = 0.08;
        start = camera_space(start);
        end = camera_space(end);
        if (start.z < near_plane && end.z < near_plane) return;
        if (start.z < near_plane) clip_near(start, end, near_plane);
        if (end.z < near_plane) clip_near(end, start, near_plane);
        const POINT a = project(start);
        const POINT b = project(end);
        MoveToEx(dc, a.x, a.y, nullptr);
        LineTo(dc, b.x, b.y);
    }
};

struct DirectInput {
    IDirectInput8W* factory{};
    IDirectInputDevice8W* mouse{};
    IDirectInputDevice8W* keyboard{};
    HRESULT mouse_setup{DIERR_NOTINITIALIZED};
    HRESULT keyboard_setup{DIERR_NOTINITIALIZED};
    HRESULT mouse_status{DIERR_NOTACQUIRED};
    HRESULT keyboard_status{DIERR_NOTACQUIRED};
    bool mouse_acquired{};
    bool keyboard_acquired{};
    bool modal{};
    bool background{};
    uint64_t mouse_events{}, key_events{}, overflows{};
    int64_t dx{}, dy{}, wheel{};
    unsigned buttons{}, wasd{};

    ~DirectInput() {
        unacquire();
        if (mouse) mouse->Release();
        if (keyboard) keyboard->Release();
        if (factory) factory->Release();
    }

    void unacquire() {
        if (mouse && mouse_acquired) mouse->Unacquire();
        if (keyboard && keyboard_acquired) keyboard->Unacquire();
        mouse_acquired = keyboard_acquired = false;
        mouse_status = keyboard_status = DIERR_NOTACQUIRED;
        buttons = wasd = 0;
    }

    HRESULT init_device(IDirectInputDevice8W*& device, REFGUID type, LPCDIDATAFORMAT format,
                        HWND window, DWORD cooperative) {
        HRESULT result = factory->CreateDevice(type, &device, nullptr);
        if (FAILED(result)) return result;
        result = device->SetCooperativeLevel(window, cooperative);
        if (FAILED(result)) return result;
        result = device->SetDataFormat(format);
        if (FAILED(result)) return result;
        DIPROPDWORD buffer{};
        buffer.diph.dwSize = sizeof(buffer);
        buffer.diph.dwHeaderSize = sizeof(buffer.diph);
        buffer.diph.dwHow = DIPH_DEVICE;
        buffer.dwData = 256;
        return device->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph);
    }

    bool init(HINSTANCE instance, HWND window, Options const& options) {
        background = options.background;
        const auto result = DirectInput8Create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                                reinterpret_cast<void**>(&factory), nullptr);
        if (FAILED(result)) {
            mouse_setup = keyboard_setup = result;
            return false;
        }
        const DWORD cooperative = (background ? DISCL_BACKGROUND : DISCL_FOREGROUND) |
                                  (options.exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE);
        mouse_setup = init_device(mouse, GUID_SysMouse, &c_dfDIMouse2, window, cooperative);
        keyboard_setup = init_device(keyboard, GUID_SysKeyboard, &c_dfDIKeyboard, window, cooperative);
        return SUCCEEDED(mouse_setup) && SUCCEEDED(keyboard_setup);
    }

    template <class Handler>
    HRESULT drain(IDirectInputDevice8W* device, bool& acquired, Handler consume) {
        if (!acquired) {
            const HRESULT result = device->Acquire();
            if (FAILED(result)) return result;
            acquired = true;
        }
        std::array<DIDEVICEOBJECTDATA, 256> events{};
        DWORD count = static_cast<DWORD>(events.size());
        const HRESULT result = device->GetDeviceData(sizeof(events.front()), events.data(), &count, 0);
        if (FAILED(result)) {
            acquired = false;
            return result;
        }
        if (result == DI_BUFFEROVERFLOW) ++overflows;
        for (DWORD index = 0; index < count; ++index) consume(events[index]);
        return result;
    }

    void poll(bool active) {
        if (modal || (!active && !background)) {
            unacquire();
            return;
        }
        mouse_status = drain(mouse, mouse_acquired, [&](DIDEVICEOBJECTDATA const& event) {
            ++mouse_events;
            const LONG delta = std::bit_cast<LONG>(event.dwData);
            switch (event.dwOfs) {
            case offsetof(DIMOUSESTATE2, lX): dx += delta; return;
            case offsetof(DIMOUSESTATE2, lY): dy += delta; return;
            case offsetof(DIMOUSESTATE2, lZ): wheel += delta; return;
            default: break;
            }
            if (event.dwOfs >= DIMOFS_BUTTON0 && event.dwOfs <= DIMOFS_BUTTON7) {
                const unsigned bit = 1u << (event.dwOfs - DIMOFS_BUTTON0);
                if (event.dwData & 0x80) buttons |= bit;
                else buttons &= ~bit;
            }
        });
        keyboard_status = drain(keyboard, keyboard_acquired, [&](DIDEVICEOBJECTDATA const& event) {
            ++key_events;
            unsigned bit = 0;
            switch (event.dwOfs) {
            case DIK_W: bit = 1; break;
            case DIK_A: bit = 2; break;
            case DIK_S: bit = 4; break;
            case DIK_D: bit = 8; break;
            default: break;
            }
            if (event.dwData & 0x80) wasd |= bit;
            else wasd &= ~bit;
        });
        if (!mouse_acquired) buttons = 0;
        if (!keyboard_acquired) wasd = 0;
    }
};

struct Probe {
    explicit Probe(Options selected) : options(selected) {}

    Options options;
    DirectInput direct;
    HWND window{};
    Clock::time_point started{}, last_sample{}, last_frame{};
    Camera camera{};
    std::array<bool, 256> raw_keys{};
    std::array<bool, 256> cooperative_keys{};
    CooperativeOwner cooperative_owner{};
    std::unordered_set<UINT_PTR> raw_devices;
    RECT clipped_to{};
    bool cursor_clipped{};
    bool running{};
    uint64_t moves{}, downs{}, ups{}, double_clicks{}, wheels{}, drag_moves{}, key_downs{},
        key_ups{}, texts{}, raw_mouse{}, raw_key{}, raw_invalid{}, raw_sink{}, raw_buttons{};
    int64_t raw_dx{}, raw_dy{}, raw_wheel{};
    uint64_t cooperative_events{}, cooperative_drag_moves{}, cooperative_resets{},
        cooperative_orphan_resets{};
    int64_t cooperative_dx{}, cooperative_dy{}, cooperative_wheel{}, cooperative_hwheel{};
    unsigned cooperative_buttons{};
    int last_x{}, last_y{};

    bool active() const { return GetForegroundWindow() == window; }

    void unlock() {
        if (!cursor_clipped) return;
        RECT current{};
        if (GetClipCursor(&current) && EqualRect(&current, &clipped_to)) ClipCursor(nullptr);
        cursor_clipped = false;
    }

    void update_lock() {
        if (!options.lock_cursor || !active() || IsIconic(window)) {
            unlock();
            return;
        }
        RECT bounds{};
        GetClientRect(window, &bounds);
        POINT top_left{bounds.left, bounds.top};
        POINT bottom_right{bounds.right, bounds.bottom};
        if (!ClientToScreen(window, &top_left) || !ClientToScreen(window, &bottom_right)) return;
        RECT desired{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
        if (IsRectEmpty(&desired)) return;
        if (ClipCursor(&desired)) {
            clipped_to = desired;
            cursor_clipped = true;
        }
    }

    bool initialize_input(HINSTANCE instance) {
        if (options.mode == Mode::cooperative) {
            if (SetPropW(window, cooperative_property, reinterpret_cast<HANDLE>(UINT_PTR{1}))) {
                return true;
            }
            std::printf("{\"event\":\"setup_error\",\"mode\":\"cooperative\",\"win32_error\":%lu}\n",
                        GetLastError());
            return false;
        }
        if (options.mode == Mode::directinput) {
            if (direct.init(instance, window, options)) return true;
            std::printf("{\"event\":\"setup_error\",\"mode\":\"directinput\","
                        "\"mouse_hresult\":\"0x%08lx\",\"keyboard_hresult\":\"0x%08lx\"}\n",
                        static_cast<unsigned long>(direct.mouse_setup),
                        static_cast<unsigned long>(direct.keyboard_setup));
            return false;
        }
        if (options.mode == Mode::raw) {
            const DWORD flags = options.input_sink ? RIDEV_INPUTSINK : 0;
            RAWINPUTDEVICE devices[] = {{1, 2, flags, window}, {1, 6, flags, window}};
            if (!RegisterRawInputDevices(devices, 2, sizeof(devices[0]))) {
                std::printf("{\"event\":\"setup_error\",\"mode\":\"raw\",\"win32_error\":%lu}\n",
                            GetLastError());
                return false;
            }
        }
        return true;
    }

    void clear_cooperative_holds() {
        cooperative_buttons = 0;
        cooperative_keys.fill(false);
        cooperative_owner = {};
    }

    void expire_cooperative_owner() {
        if (!cooperative_owner.window) return;
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(cooperative_owner.window, &process);
        if (thread == cooperative_owner.thread && process == cooperative_owner.process) return;
        clear_cooperative_holds();
        ++cooperative_orphan_resets;
        if (!options.quiet) {
            std::printf("{\"event\":\"cooperative_owner_lost\",\"qpc\":%lld}\n", qpc());
        }
    }

    LRESULT receive_cooperative(HWND sender, COPYDATASTRUCT const* frame) {
        if (!frame || frame->dwData != cooperative_protocol || !frame->lpData ||
            frame->cbData != sizeof(CooperativeInput)) {
            return FALSE;
        }
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(sender, &process);
        if (!thread || !process) return FALSE;
        expire_cooperative_owner();
        if (cooperative_owner.window && cooperative_owner.window != sender) return FALSE;
        CooperativeInput input{};
        std::memcpy(&input, frame->lpData, sizeof(input));
        if (input.c != 0) return FALSE;
        const bool state_valid = input.b == 0 || input.b == 1;
        switch (input.op) {
        case 1:
            cooperative_dx += input.a;
            cooperative_dy += input.b;
            camera.look(input.a, input.b);
            if (cooperative_buttons) ++cooperative_drag_moves;
            break;
        case 2: {
            if (input.a < 1 || input.a > 3 || !state_valid) return FALSE;
            const unsigned bit = 1u << (input.a - 1);
            if (input.b) cooperative_buttons |= bit;
            else cooperative_buttons &= ~bit;
            break;
        }
        case 3:
            if (input.a < 1 || input.a >= static_cast<int>(cooperative_keys.size()) || !state_valid) {
                return FALSE;
            }
            cooperative_keys[input.a] = input.b != 0;
            break;
        case 4:
            if (!state_valid) return FALSE;
            if (input.b) cooperative_hwheel += input.a;
            else cooperative_wheel += input.a;
            break;
        case 5:
            clear_cooperative_holds();
            ++cooperative_resets;
            break;
        default:
            return FALSE;
        }
        const auto keys_down = static_cast<std::size_t>(
            std::count(cooperative_keys.begin(), cooperative_keys.end(), true));
        cooperative_owner = cooperative_buttons || keys_down ? CooperativeOwner{sender, process, thread}
                                                              : CooperativeOwner{};
        ++cooperative_events;
        if (!options.quiet) {
            std::printf(
                "{\"event\":\"cooperative_input\",\"op\":%u,\"a\":%d,\"b\":%d,"
                "\"qpc\":%lld,\"yaw\":%.6f,\"pitch\":%.6f,\"cooperative_buttons\":%u,"
                "\"cooperative_keys_down\":%zu}\n",
                input.op, input.a, input.b, qpc(), camera.yaw, camera.pitch, cooperative_buttons, keys_down);
        }
        return cooperative_ack;
    }

    void read_raw(WPARAM code, LPARAM handle) {
        RAWINPUT packet{};
        UINT size = sizeof(packet);
        const UINT read = GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &packet,
                                         &size, sizeof(RAWINPUTHEADER));
        if (read == static_cast<UINT>(-1) || read < sizeof(RAWINPUTHEADER) ||
            packet.header.dwSize != read) {
            ++raw_invalid;
            return;
        }
        if (packet.header.hDevice) {
            raw_devices.insert(reinterpret_cast<UINT_PTR>(packet.header.hDevice));
        }
        if (GET_RAWINPUT_CODE_WPARAM(code) == RIM_INPUTSINK) ++raw_sink;
        if (packet.header.dwType == RIM_TYPEMOUSE &&
            read >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) {
            ++raw_mouse;
            auto const& mouse = packet.data.mouse;
            if (mouse.usButtonFlags != 0) ++raw_buttons;
            if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                raw_wheel += std::bit_cast<SHORT>(mouse.usButtonData);
            }
            if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                raw_dx += mouse.lLastX;
                raw_dy += mouse.lLastY;
                camera.look(mouse.lLastX, mouse.lLastY);
            }
        }
        if (packet.header.dwType == RIM_TYPEKEYBOARD &&
            read >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)) {
            ++raw_key;
            const auto key = packet.data.keyboard.VKey;
            if (key < raw_keys.size()) raw_keys[key] = (packet.data.keyboard.Flags & RI_KEY_BREAK) == 0;
        }
    }

    void trace_mouse(char const* event, WPARAM state, LPARAM packed, char const* button,
                     int delta = 0) const {
        if (options.quiet) return;
        POINT cursor{};
        const bool cursor_ok = GetCursorPos(&cursor) != FALSE;
        std::printf("{\"event\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
                    "\"buttons\":%u,\"delta\":%d,\"qpc\":%lld,\"foreground_hwnd\":\"0x%llx\","
                    "\"cursor_ok\":%s,\"cursor_x\":%ld,\"cursor_y\":%ld}\n",
                    event, GET_X_LPARAM(packed), GET_Y_LPARAM(packed), button,
                    static_cast<unsigned>(LOWORD(state)), delta, qpc(),
                    hwnd_value(GetForegroundWindow()), boolean(cursor_ok), cursor.x, cursor.y);
    }

    void trace_key(char const* event, WPARAM value, LPARAM packed) const {
        if (options.quiet) return;
        std::printf("{\"event\":\"%s\",\"value\":%llu,\"flags\":%lu,\"qpc\":%lld}\n", event,
                    static_cast<unsigned long long>(value), static_cast<unsigned long>(packed), qpc());
    }

    void ready() const {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        POINT cursor{};
        const bool cursor_ok = GetCursorPos(&cursor) != FALSE;
        RECT bounds{};
        GetClientRect(window, &bounds);
        std::printf("{\"event\":\"ready\",\"mode\":\"%s\",\"hwnd\":\"0x%llx\","
                    "\"qpc_frequency\":%lld,\"qpc\":%lld,\"foreground_hwnd\":\"0x%llx\","
                    "\"client_width\":%ld,\"client_height\":%ld,\"cursor_ok\":%s,"
                    "\"cursor_x\":%ld,\"cursor_y\":%ld,\"borderless\":%s,\"input_sink\":%s,"
                    "\"background\":%s,\"exclusive\":%s}\n",
                    mode_name(options.mode), hwnd_value(window), frequency.QuadPart, qpc(),
                    hwnd_value(GetForegroundWindow()), bounds.right, bounds.bottom,
                    boolean(cursor_ok), cursor.x, cursor.y, boolean(options.borderless),
                    boolean(options.input_sink), boolean(options.background), boolean(options.exclusive));
    }

    void sample(char const* event) const {
        POINT cursor{};
        const bool cursor_ok = GetCursorPos(&cursor) != FALSE;
        std::printf(
            "{\"event\":\"%s\",\"mode\":\"%s\",\"qpc\":%lld,\"foreground_hwnd\":\"0x%llx\","
            "\"active\":%s,\"cursor_ok\":%s,\"cursor_x\":%ld,\"cursor_y\":%ld,"
            "\"moves\":%llu,\"downs\":%llu,\"ups\":%llu,\"double_clicks\":%llu,"
            "\"wheels\":%llu,\"drag_moves\":%llu,\"key_downs\":%llu,\"key_ups\":%llu,\"texts\":%llu,"
            "\"raw_mouse\":%llu,\"raw_key\":%llu,\"raw_invalid\":%llu,\"raw_sink\":%llu,"
            "\"raw_buttons\":%llu,\"raw_devices\":%zu,\"raw_dx\":%lld,\"raw_dy\":%lld,\"raw_wheel\":%lld,"
            "\"yaw\":%.6f,\"pitch\":%.6f,\"camera_x\":%.6f,\"camera_z\":%.6f,"
            "\"cursor_locked\":%s,\"di_mouse_acquired\":%s,\"di_keyboard_acquired\":%s,"
            "\"di_mouse_status\":\"0x%08lx\",\"di_keyboard_status\":\"0x%08lx\","
            "\"di_mouse_events\":%llu,\"di_key_events\":%llu,\"di_overflows\":%llu,"
            "\"di_dx\":%lld,\"di_dy\":%lld,\"di_wheel\":%lld,\"di_buttons\":%u,\"di_wasd\":%u,"
            "\"cooperative_events\":%llu,\"cooperative_buttons\":%u,\"cooperative_keys_down\":%zu,"
            "\"cooperative_dx\":%lld,\"cooperative_dy\":%lld,\"cooperative_wheel\":%lld,"
            "\"cooperative_hwheel\":%lld,\"cooperative_drag_moves\":%llu,\"cooperative_resets\":%llu,"
            "\"cooperative_orphan_resets\":%llu}\n",
            event, mode_name(options.mode), qpc(), hwnd_value(GetForegroundWindow()),
            boolean(active()), boolean(cursor_ok), cursor.x, cursor.y,
            moves, downs, ups, double_clicks, wheels, drag_moves, key_downs, key_ups, texts,
            raw_mouse, raw_key, raw_invalid, raw_sink, raw_buttons, raw_devices.size(), raw_dx,
            raw_dy, raw_wheel, camera.yaw, camera.pitch, camera.position.x, camera.position.z,
            boolean(cursor_clipped), boolean(direct.mouse_acquired), boolean(direct.keyboard_acquired),
            static_cast<unsigned long>(direct.mouse_status),
            static_cast<unsigned long>(direct.keyboard_status), direct.mouse_events,
            direct.key_events, direct.overflows, direct.dx, direct.dy, direct.wheel, direct.buttons,
            direct.wasd, cooperative_events, cooperative_buttons,
            static_cast<std::size_t>(std::count(cooperative_keys.begin(), cooperative_keys.end(), true)),
            cooperative_dx, cooperative_dy, cooperative_wheel, cooperative_hwheel,
            cooperative_drag_moves, cooperative_resets, cooperative_orphan_resets);
    }

    void tick() {
        const auto now = Clock::now();
        if (options.mode == Mode::directinput) direct.poll(active());
        if (options.mode == Mode::cooperative) expire_cooperative_owner();
        if (options.mode == Mode::raw || options.mode == Mode::cooperative) {
            const auto& keys = options.mode == Mode::raw ? raw_keys : cooperative_keys;
            const int ahead = keys['W'] - keys['S'];
            const int right = keys['D'] - keys['A'];
            camera.step(ahead, right,
                        std::min(std::chrono::duration<double>(now - last_frame).count(), 0.05));
        }
        last_frame = now;
        if (now - last_sample >= 250ms) {
            sample("sample");
            last_sample = now;
        }
        if (options.timeout_ms && now - started >= std::chrono::milliseconds(options.timeout_ms)) {
            DestroyWindow(window);
            return;
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void draw_scene(HDC dc, RECT rect) const {
        const View view(camera, rect);
        const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, RGB(43, 71, 99));
        for (int coordinate = -16; coordinate <= 16; ++coordinate) {
            const double x = coordinate;
            view.line(dc, {x, 0, -16}, {x, 0, 16});
            view.line(dc, {-16, 0, x}, {16, 0, x});
        }
        SetDCPenColor(dc, RGB(60, 201, 199));
        for (double center : {-4.0, 0.0, 4.0}) {
            constexpr std::array<Point3, 4> corners = {{{-1, 0, 5}, {1, 0, 5}, {1, 0, 7}, {-1, 0, 7}}};
            for (std::size_t i = 0; i < corners.size(); ++i) {
                auto a = corners[i];
                auto b = corners[(i + 1) % corners.size()];
                a.x += center;
                b.x += center;
                view.line(dc, a, b);
                const auto top = Point3{a.x, 2, a.z};
                view.line(dc, a, top);
                view.line(dc, top, {b.x, 2, b.z});
            }
        }
        SetDCPenColor(dc, RGB(242, 246, 250));
        const LONG x = rect.right / 2;
        const LONG y = rect.bottom / 2;
        MoveToEx(dc, x - 6, y, nullptr);
        LineTo(dc, x + 7, y);
        MoveToEx(dc, x, y - 6, nullptr);
        LineTo(dc, x, y + 7);
        SelectObject(dc, old_pen);
    }

    void draw(HDC target) const {
        RECT rect{};
        GetClientRect(window, &rect);
        if (rect.right < 1 || rect.bottom < 1) return;
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, rect.right, rect.bottom) : nullptr;
        HGDIOBJ previous = bitmap ? SelectObject(buffer, bitmap) : nullptr;
        HDC dc = previous ? buffer : target;
        SetDCBrushColor(dc, RGB(14, 23, 35));
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (options.mode == Mode::raw || options.mode == Mode::cooperative) draw_scene(dc, rect);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(225, 237, 248));
        SetBkColor(dc, RGB(14, 23, 35));
        wchar_t status[480]{};
        if (options.mode == Mode::cooperative) {
            std::swprintf(status, std::size(status),
                L"Cooperative FPS  |  AIMouse GameInput v1\r\n"
                L"Yaw %.2f   Pitch %.2f   Position %.2f, %.2f\r\n"
                L"Acknowledged events %llu   Drag moves %llu",
                camera.yaw, camera.pitch, camera.position.x, camera.position.z,
                cooperative_events, cooperative_drag_moves);
        } else if (options.mode == Mode::raw) {
            std::swprintf(status, std::size(status),
                L"Raw Input FPS  |  %ls\r\n"
                L"Yaw %.2f   Pitch %.2f   Position %.2f, %.2f\r\n"
                L"Mouse %llu   Keyboard %llu   Devices %zu\r\n"
                L"WASD / mouse in foreground mode. Esc closes.",
                options.input_sink ? L"background observation" : L"foreground input",
                camera.yaw, camera.pitch, camera.position.x, camera.position.z,
                raw_mouse, raw_key, raw_devices.size());
        } else if (options.mode == Mode::directinput) {
            std::swprintf(status, std::size(status),
                L"DirectInput 8  |  %ls  |  %ls\r\n"
                L"Acquired mouse %ls   keyboard %ls\r\n"
                L"Relative axes %lld, %lld   Wheel %lld\r\n"
                L"Mouse records %llu   Keyboard records %llu   Overflows %llu",
                options.background ? L"background" : L"foreground",
                options.exclusive ? L"exclusive" : L"nonexclusive",
                direct.mouse_acquired ? L"yes" : L"no", direct.keyboard_acquired ? L"yes" : L"no",
                direct.dx, direct.dy, direct.wheel, direct.mouse_events, direct.key_events,
                direct.overflows);
        } else {
            std::swprintf(status, std::size(status),
                L"Win32 message receiver\r\n"
                L"Client coordinates %d, %d   Moves %llu\r\n"
                L"Down %llu   Up %llu   Double click %llu   Drag %llu   Wheel %llu\r\n"
                L"Key down %llu   Key up %llu   Text %llu",
                last_x, last_y, moves, downs, ups, double_clicks, drag_moves, wheels,
                key_downs, key_ups, texts);
        }
        RECT text_rect{18, 16, rect.right - 12, rect.bottom - 10};
        DrawTextW(dc, status, -1, &text_rect, DT_LEFT | DT_TOP | DT_NOPREFIX);
        if (previous) {
            BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(buffer, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
    }

    LRESULT message(UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
        case WM_MOUSEMOVE:
            ++moves;
            if (wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) {
                ++drag_moves;
            }
            last_x = GET_X_LPARAM(lparam);
            last_y = GET_Y_LPARAM(lparam);
            trace_mouse("mouse_move", wparam, lparam, "");
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
            return button_message(msg, wparam, lparam);
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            ++wheels;
            trace_mouse(msg == WM_MOUSEWHEEL ? "mouse_wheel" : "mouse_hwheel", wparam, lparam, "",
                        GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            ++key_downs;
            trace_key("key_down", wparam, lparam);
            if (wparam == VK_ESCAPE && options.foreground && active() &&
                (InSendMessageEx(nullptr) == ISMEX_NOSEND)) {
                DestroyWindow(window);
            }
            if (msg == WM_SYSKEYDOWN) break;
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP:
            ++key_ups;
            trace_key("key_up", wparam, lparam);
            if (msg == WM_SYSKEYUP) break;
            return 0;
        case WM_CHAR:
            ++texts;
            trace_key("text", wparam, lparam);
            return 0;
        case WM_INPUT:
            if (options.mode == Mode::raw) read_raw(wparam, lparam);
            if (GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUT) {
                return DefWindowProcW(window, msg, wparam, lparam);
            }
            return 0;
        case WM_COPYDATA:
            if (options.mode == Mode::cooperative) {
                return receive_cooperative(reinterpret_cast<HWND>(wparam),
                                           reinterpret_cast<COPYDATASTRUCT const*>(lparam));
            }
            break;
        case WM_TIMER:
            if (running) tick();
            return 0;
        case WM_SETFOCUS:
            if (running) update_lock();
            return 0;
        case WM_KILLFOCUS:
            unlock();
            raw_keys.fill(false);
            if (!direct.background) direct.unacquire();
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (cursor_clipped) update_lock();
            break;
        case WM_ENTERSIZEMOVE: case WM_ENTERMENULOOP:
            direct.modal = true;
            direct.unacquire();
            unlock();
            return 0;
        case WM_EXITSIZEMOVE: case WM_EXITMENULOOP:
            direct.modal = false;
            update_lock();
            return 0;
        case WM_SETCURSOR:
            if (cursor_clipped && LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            if (dc) draw(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINT: case WM_PRINTCLIENT:
            if (wparam) draw(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_DESTROY:
            if (running) sample("final");
            running = false;
            if (options.mode == Mode::cooperative) RemovePropW(window, cooperative_property);
            unlock();
            direct.unacquire();
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, msg, wparam, lparam);
    }

    LRESULT button_message(UINT msg, WPARAM wparam, LPARAM lparam) {
        const bool extended = msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK;
        const char* button = "left";
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK) button = "right";
        if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MBUTTONDBLCLK) button = "middle";
        if (extended) button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? "x1" : "x2";
        const bool is_up = msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
                           msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
        const bool is_double = msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                               msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK;
        if (is_up) {
            ++ups;
            trace_mouse("mouse_up", wparam, lparam, button);
        } else if (is_double) {
            ++double_clicks;
            trace_mouse("mouse_double_click", wparam, lparam, button);
        } else {
            ++downs;
            trace_mouse("mouse_down", wparam, lparam, button);
        }
        return extended ? TRUE : 0;
    }
};

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW const*>(lparam);
        auto* probe = static_cast<Probe*>(create->lpCreateParams);
        probe->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return probe ? probe->message(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}
}

int main(int argc, char** argv) {
    Options options{};
    if (!parse_args(argc, argv, options)) {
        std::fputs("Usage: aimouse_input_probe win32|raw|directinput|cooperative [--timeout-ms N] "
                   "[--foreground] [--borderless] [--capturable] [--quiet]\n"
                   "Raw: --input-sink or --foreground --lock-cursor\n"
                   "DirectInput: --background or --foreground --exclusive\n", stderr);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Probe probe(options);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = window_class(options.mode);
    WNDCLASSW window_spec{};
    window_spec.style = CS_DBLCLKS;
    window_spec.lpfnWndProc = window_proc;
    window_spec.hInstance = instance;
    window_spec.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_spec.lpszClassName = name;
    if (!RegisterClassW(&window_spec)) {
        std::fprintf(stderr, "RegisterClassW failed: %lu\n", GetLastError());
        return 1;
    }
    const DWORD style = options.borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const DWORD extended_style = options.foreground || options.capturable
        ? 0 : WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    RECT dimensions{0, 0, 640, 360};
    AdjustWindowRectEx(&dimensions, style, FALSE, extended_style);
    const HWND window = CreateWindowExW(extended_style, name, name, style, 90, 90,
                                        dimensions.right - dimensions.left,
                                        dimensions.bottom - dimensions.top,
                                        nullptr, nullptr, instance, &probe);
    if (!window) {
        std::fprintf(stderr, "CreateWindowExW failed: %lu\n", GetLastError());
        return 1;
    }
    if (!probe.initialize_input(instance)) {
        DestroyWindow(window);
        return 1;
    }
    ShowWindow(window, options.foreground ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    probe.started = probe.last_sample = probe.last_frame = Clock::now();
    probe.running = true;
    probe.update_lock();
    probe.ready();
    if (!SetTimer(window, 1, 16, nullptr)) {
        std::fprintf(stderr, "SetTimer failed: %lu\n", GetLastError());
        DestroyWindow(window);
        return 1;
    }
    MSG message{};
    int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result < 0 ? 1 : 0;
}
