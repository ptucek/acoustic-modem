// GUI aplikace milníku 3: SDL3 + OpenGL3 + ImGui + ImPlot skořápka, která
// řídí DspThread (audio I/O + DSP běží v samostatném vlákně) a vykresluje
// panely definované v panels.hpp/panels.cpp.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h" // GL prototypy (glViewport, glClear, ...)
#include "implot.h"

#include "app/ui_font.hpp"
#include "app/dsp_thread.hpp"
#include "app/panels.hpp"
#include "core/config.hpp"
#include "modem/modulator.hpp"

namespace {

void printGuiUsage() {
    std::printf(
        "modem_gui — GUI akustického modemu\n\n"
        "Použití: modem_gui [--scheme NÁZEV] [--baud N] [--sample-rate N]\n"
        "                   [--f0 N] [--f1 N] [--amp N]\n\n"
        "  --scheme  2-FSK | OOK | DBPSK | 16-FSK | Q-FSK  (výchozí 2-FSK)\n"
        "  --baud    symbolová rychlost v Bd (Q-FSK výchozí 62.5, jinak 31.25)\n"
        "  --f0/--f1 tóny 2-FSK/nosná (Hz); --amp amplituda 0..1\n"
        "  Vše lze měnit i za běhu v ovládacím panelu GUI.\n");
}

int schemeIndexByName(const char* name) {
    const auto schemes = am::modemRegistry();
    for (size_t i = 0; i < schemes.size(); ++i)
        if (std::strcmp(schemes[i].name, name) == 0) return int(i);
    return -1;
}

// Naparsuje přepínače do cfg + scheme_index. Vrací false při chybě (vypíše ji).
// Stejná schémata/logika jako modem_cli, aby se GUI dalo spustit rovnou na
// konkrétní modulaci (např. `modem_gui --scheme Q-FSK`).
bool parseGuiArgs(int argc, char** argv, am::ModemConfig& cfg, int& scheme_index) {
    std::string scheme_name = "2-FSK";
    bool baud_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](double& out) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "modem_gui: %s vyžaduje hodnotu\n", a.c_str());
                return false;
            }
            out = std::strtod(argv[++i], nullptr);
            return true;
        };
        if (a == "--help" || a == "-h") {
            printGuiUsage();
            std::exit(0);
        } else if (a == "--scheme") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "modem_gui: --scheme vyžaduje název\n");
                return false;
            }
            scheme_name = argv[++i];
        } else if (a == "--baud") {
            double v;
            if (!value(v)) return false;
            cfg.baud = v;
            baud_set = true;
        } else if (a == "--sample-rate") {
            double v;
            if (!value(v)) return false;
            cfg.sample_rate = int(v);
        } else if (a == "--f0") {
            double v;
            if (!value(v)) return false;
            cfg.f0 = v;
        } else if (a == "--f1") {
            double v;
            if (!value(v)) return false;
            cfg.f1 = v;
        } else if (a == "--amp") {
            double v;
            if (!value(v)) return false;
            cfg.amplitude = v;
        } else {
            std::fprintf(stderr, "modem_gui: neznámý argument: %s\n", a.c_str());
            printGuiUsage();
            return false;
        }
    }

    const int idx = schemeIndexByName(scheme_name.c_str());
    if (idx < 0) {
        std::fprintf(stderr, "modem_gui: neznámé schéma modulace \"%s\"\n",
                     scheme_name.c_str());
        return false;
    }
    scheme_index = idx;

    // Q-FSK je navržená pro 62,5 Bd (4×16-FSK = 1 kbit/s); ostatní jedou na
    // 31,25 Bd. Výchozí baud podle schématu, explicitní --baud ho přebije.
    if (!baud_set && scheme_name == "Q-FSK") cfg.baud = 62.5;

    if (!(cfg.baud > 0.0) || cfg.baud > double(cfg.sample_rate) / 16.0) {
        std::fprintf(stderr,
                     "modem_gui: --baud musí být kladné a nejvýše sample_rate/16 "
                     "(%.1f), zadáno %.3f\n",
                     double(cfg.sample_rate) / 16.0, cfg.baud);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    am::ModemConfig start_cfg{};
    int start_scheme_index = 0;
    if (!parseGuiArgs(argc, argv, start_cfg, start_scheme_index)) return 2;

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

    // České glyfy: výchozí font (ProggyClean) obsahuje jen ASCII + Latin-1,
    // takže znaky s háčkem/kroužkem (ř č ž ě ů ň š ď ť …) z bloku Latin
    // Extended-A chyběly a vykreslovaly se jako „?". Načteme proto zabudovaný
    // font Roboto (Apache-2.0), který češtinu pokrývá. Font je vestavěný v
    // binárce (base85) → stejné vykreslení na macOS i Linuxu bez závislosti na
    // systémových fontech. Rozsah glyfů necháváme prázdný (nullptr) — ImGui
    // 1.92+ rasterizuje glyfy on-demand, takže se natáhne cokoli z fontu.
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 2;
    if (io.Fonts->AddFontFromMemoryCompressedBase85TTF(
            UiFont_compressed_data_base85, 16.0f, &font_cfg, nullptr) == nullptr) {
        io.Fonts->AddFontDefault(); // nouzový fallback (bez českých glyfů)
    }

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
    ui.cfg = start_cfg;                     // z CLI přepínačů (nebo výchozí)
    ui.scheme_index = start_scheme_index;   // --scheme (výchozí 2-FSK)

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
