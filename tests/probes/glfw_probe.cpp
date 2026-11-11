#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine_probe.hpp"

#include <memory>

namespace {
using namespace aimouse_probe;
constexpr char mode[] = "glfw";

struct GlfwQuit {
    ~GlfwQuit() { glfwTerminate(); }
};

struct Context {
    Options options;
    Stats stats;
};

Context& context(GLFWwindow* window) {
    return *static_cast<Context*>(glfwGetWindowUserPointer(window));
}

void mode_for_focus(GLFWwindow* window, bool focused) {
    const auto& options = context(window).options;
    if (!options.relative && !options.capture) return;
    glfwSetInputMode(window, GLFW_CURSOR,
                     !focused ? GLFW_CURSOR_NORMAL :
                     options.relative ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_CAPTURED);
    if (options.relative) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, focused ? GLFW_TRUE : GLFW_FALSE);
}

void focused(GLFWwindow* window, int has_focus) {
    context(window).stats.focus(has_focus != GLFW_FALSE);
    mode_for_focus(window, has_focus != GLFW_FALSE);
}

void moved(GLFWwindow* window, double x, double y) {
    auto& stats = context(window).stats;
    const double dx = stats.has_position ? x - stats.x : 0.0;
    const double dy = stats.has_position ? y - stats.y : 0.0;
    stats.move(x, y, dx, dy);
}

void button(GLFWwindow* window, int index, int action, int) {
    if (index >= 0) context(window).stats.button(static_cast<unsigned>(index), action == GLFW_PRESS);
}

void wheel(GLFWwindow* window, double x, double y) {
    context(window).stats.scroll(x, y);
}

void key(GLFWwindow* window, int keycode, int, int action, int) {
    auto& state = context(window);
    if (action == GLFW_RELEASE) ++state.stats.key_ups;
    else ++state.stats.key_downs;
    if (keycode == GLFW_KEY_ESCAPE && action == GLFW_PRESS && state.options.foreground) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

void draw(GLFWwindow* window, Stats const& stats) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width < 1 || height < 1) return;
    glViewport(0, 0, width, height);
    glClearColor(0.055f, 0.09f, 0.14f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0.83f, 0.94f, 0.98f);
    const double length = 9.0;
    const double dx = length / width;
    const double dy = length / height;
    const double x = stats.has_position ? 2.0 * stats.x / width - 1.0 : 0;
    const double y = stats.has_position ? 1.0 - 2.0 * stats.y / height : 0;
    glBegin(GL_LINES);
    glVertex2d(x - dx, y);
    glVertex2d(x + dx, y);
    glVertex2d(x, y - dy);
    glVertex2d(x, y + dy);
    glEnd();
    glfwSwapBuffers(window);
}
}

int main(int argc, char** argv) {
    Context state;
    if (!parse(argc, argv, state.options)) return usage(argv[0]);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW %d: %s\n", error, description);
    });
    if (!glfwInit()) return 1;
    const GlfwQuit quit;
    if (state.options.relative && !glfwRawMouseMotionSupported()) {
        std::fputs("GLFW reports raw mouse motion unavailable.\n", stderr);
        return 1;
    }
    glfwWindowHint(GLFW_FOCUSED, state.options.foreground ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, state.options.foreground ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, state.options.borderless ? GLFW_FALSE : GLFW_TRUE);
    const std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(640, 360, "AIMouse GLFW OpenGL Probe", nullptr, nullptr), glfwDestroyWindow);
    if (!window) return 1;
    glfwMakeContextCurrent(window.get());
    glfwSwapInterval(0);
    glfwSetWindowUserPointer(window.get(), &state);
    glfwSetWindowFocusCallback(window.get(), focused);
    glfwSetCursorPosCallback(window.get(), moved);
    glfwSetMouseButtonCallback(window.get(), button);
    glfwSetScrollCallback(window.get(), wheel);
    glfwSetKeyCallback(window.get(), key);
    mode_for_focus(window.get(), glfwGetWindowAttrib(window.get(), GLFW_FOCUSED) == GLFW_TRUE);
    const HWND native = glfwGetWin32Window(window.get());
    if (!native) return 1;
    ready(mode, native, state.options);

    const auto started = Clock::now();
    auto last_sample = started;
    while (!glfwWindowShouldClose(window.get())) {
        glfwWaitEventsTimeout(0.016);
        const auto now = Clock::now();
        if (now - last_sample >= 250ms) {
            const int cursor_mode = glfwGetInputMode(window.get(), GLFW_CURSOR);
            sample("sample", mode, native, state.stats, cursor_mode == GLFW_CURSOR_DISABLED,
                   cursor_mode == GLFW_CURSOR_CAPTURED || cursor_mode == GLFW_CURSOR_DISABLED);
            last_sample = now;
        }
        if (state.options.timeout_ms && now - started >= std::chrono::milliseconds(state.options.timeout_ms)) {
            break;
        }
        draw(window.get(), state.stats);
    }
    mode_for_focus(window.get(), false);
    sample("final", mode, native, state.stats, false, false);
    return 0;
}
