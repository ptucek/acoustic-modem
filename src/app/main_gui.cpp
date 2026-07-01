// GUI aplikace milníku 3: SDL3 + OpenGL3 + ImGui + ImPlot skořápka, která
// řídí DspThread (audio I/O + DSP běží v samostatném vlákně) a vykresluje
// panely definované v panels.hpp/panels.cpp.

#include <cstdio>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h" // GL prototypy (glViewport, glClear, ...)
#include "implot.h"

#include "app/dsp_thread.hpp"
#include "app/panels.hpp"
#include "core/config.hpp"
#include "modem/modulator.hpp"

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init selhal: %s\n", SDL_GetError());
        return 1;
    }

    // OpenGL profil: macOS podporuje jen Core profil a vyžaduje aspoň 3.2,
    // zatímco na Linuxu/Windows nám stačí kompatibilnější 3.0 (GLSL #version 130).
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // SDL_WINDOW_HIGH_PIXEL_DENSITY je nutný na macOS (Retina), na Linuxu
    // neškodí — necháváme zapnutý všude.
    const SDL_WindowFlags window_flags =
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window =
        SDL_CreateWindow("Akustický modem", 1280, 800, window_flags);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow selhal: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr) {
        std::fprintf(stderr, "SDL_GL_CreateContext selhal: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
#ifdef __APPLE__
    ImGui_ImplOpenGL3_Init("#version 150");
#else
    ImGui_ImplOpenGL3_Init("#version 130");
#endif

    // --- vlastní stav aplikace ---
    am::DspThread dsp;
    am::UiState ui(dsp);
    ui.cfg = am::ModemConfig{}; // výchozí konfigurace z core/config.hpp
    ui.scheme_index = 0;        // první schéma z registru (2-FSK)

    if (!dsp.start(ui.cfg, ui.scheme_index)) {
        std::fprintf(stderr, "Nepodařilo se spustit výchozí audio zařízení — "
                              "GUI poběží bez zvuku.\n");
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ui.status = dsp.status();

        am::drawControlBar(ui);
        am::drawWaterfallPanel(ui);
        am::drawRxPanel(ui);
        am::drawTxPanel(ui);
        am::drawStatsPanel(ui);

        ImGui::Render();

        int fb_w = 0, fb_h = 0;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    dsp.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
