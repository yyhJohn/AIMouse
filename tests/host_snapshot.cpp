#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

struct Windows {
    DWORD pid{};
    HWND panel{};
    HWND overlay{};
};

BOOL CALLBACK lookup(HWND window, LPARAM context) {
    auto& windows = *reinterpret_cast<Windows*>(context);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != windows.pid) return TRUE;
    wchar_t klass[64]{};
    GetClassNameW(window, klass, 64);
    if (std::wstring_view(klass) == L"AIMouse.ControlPanel") windows.panel = window;
    if (std::wstring_view(klass) == L"AIMouse.CursorOverlay") windows.overlay = window;
    return TRUE;
}

bool held(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Windows windows{strtoul(argv[1], nullptr, 10)};
    EnumWindows(lookup, reinterpret_cast<LPARAM>(&windows));
    if (argc == 3) {
        if (!windows.panel) return 2;
        const std::string_view action(argv[2]);
        if (action == "close") return PostMessageW(windows.panel, WM_CLOSE, 0, 0) ? 0 : 3;
        const int id = action == "arm" ? 101 : action == "stop" ? 103 : 0;
        if (!id) return 4;
        HWND button = GetDlgItem(windows.panel, id);
        if (!button || !IsWindowEnabled(button)) return 5;
        return SendMessageTimeoutW(windows.panel, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
            reinterpret_cast<LPARAM>(button), SMTO_ABORTIFHUNG, 500, nullptr) ? 0 : 6;
    }
    POINT cursor{};
    RECT rect{};
    GetCursorPos(&cursor);
    const bool overlay = windows.overlay && IsWindowVisible(windows.overlay) && GetWindowRect(windows.overlay, &rect);
    printf("{\"x\":%ld,\"y\":%ld,\"foreground\":\"%p\",\"buttons\":[%d,%d,%d],"
           "\"keys\":[%d,%d,%d],\"overlay\":%d,\"overlay_x\":%ld,\"overlay_y\":%ld}\n",
           cursor.x, cursor.y, static_cast<void*>(GetForegroundWindow()), held(VK_LBUTTON), held(VK_RBUTTON),
           held(VK_MBUTTON), held('W'), held(VK_SHIFT), held(VK_CONTROL),
           overlay, rect.left, rect.top);
}
