#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "engine_probe.hpp"

#include <memory>

namespace {
using namespace aimouse_probe;
constexpr char mode[] = "sdl3";

struct SdlQuit {
    ~SdlQuit() { SDL_Quit(); }
};

int error(const char* operation) {
    std::fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    return 1;
}

void draw(SDL_Renderer* renderer, Stats const& stats, bool relative) {
    SDL_SetRenderDrawColor(renderer, 14, 23, 35, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 225, 237, 248, 255);
    SDL_RenderDebugTextFormat(renderer, 18, 18, "SDL 3 / %s / %s",
                              SDL_GetRendererName(renderer), relative ? "relative" : "absolute");
    SDL_RenderDebugTextFormat(renderer, 18, 34, "Moves %llu  Down %llu  Up %llu  Wheel %llu",
                              stats.moves, stats.downs, stats.ups, stats.wheels);
    SDL_RenderDebugTextFormat(renderer, 18, 50, "Delta %.2f, %.2f  Drag %llu  Keys %llu/%llu",
                              stats.dx, stats.dy, stats.drag_moves, stats.key_downs, stats.key_ups);
    SDL_RenderPresent(renderer);
}
}

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) return usage(argv[0]);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, options.capture ? "1" : "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) return error("SDL_Init");
    const SdlQuit quit;
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
    if (!options.foreground) flags |= SDL_WINDOW_NOT_FOCUSABLE;
    if (options.borderless) flags |= SDL_WINDOW_BORDERLESS;
    const std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("AIMouse SDL 3 Direct3D 11 Probe", 640, 360, flags), SDL_DestroyWindow);
    if (!window) return error("SDL_CreateWindow");
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), "direct3d11"), SDL_DestroyRenderer);
    if (!renderer) return error("SDL_CreateRenderer");
    const auto native = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window.get()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!native) return error("SDL_GetWindowProperties");
    if (!SDL_ShowWindow(window.get())) return error("SDL_ShowWindow");
    ready(mode, native, options);

    Stats stats;
    const auto started = Clock::now();
    auto last_sample = started;
    bool running = true;
    bool relative = false;
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                stats.focus(true);
                if (options.relative) {
                    relative = SDL_SetWindowRelativeMouseMode(window.get(), true);
                    if (!relative) return error("SDL_SetWindowRelativeMouseMode");
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                stats.focus(false);
                if (relative && !SDL_SetWindowRelativeMouseMode(window.get(), false)) {
                    return error("SDL_SetWindowRelativeMouseMode");
                }
                relative = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                stats.move(event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button) {
                    stats.button(event.button.button - 1, event.button.down);
                    if (event.button.down && event.button.clicks == 2) ++stats.double_clicks;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                stats.scroll(event.wheel.x, event.wheel.y);
                break;
            case SDL_EVENT_KEY_DOWN:
                ++stats.key_downs;
                if (options.foreground && event.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_KEY_UP:
                ++stats.key_ups;
                break;
            default:
                break;
            }
        }
        const auto now = Clock::now();
        if (now - last_sample >= 250ms) {
            sample("sample", mode, native, stats, relative, relative);
            last_sample = now;
        }
        if (options.timeout_ms && now - started >= std::chrono::milliseconds(options.timeout_ms)) break;
        if (running) {
            draw(renderer.get(), stats, relative);
            SDL_Delay(16);
        }
    }
    if (relative) SDL_SetWindowRelativeMouseMode(window.get(), false);
    sample("final", mode, native, stats, SDL_GetWindowRelativeMouseMode(window.get()), false);
    return 0;
}
