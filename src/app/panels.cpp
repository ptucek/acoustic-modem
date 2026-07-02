// Implementace panelů GUI. Rozvržení je bez dockovací větve ImGui —
// jednotlivá okna jsou napevno umístěná podle velikosti viewportu:
//
//   +---------------------------------------------------------+
//   |                     Ovládání (top bar)                  |
//   +------------------------------------+--------------------+
//   |                                     |                    |
//   |          Vodopád (~60 % šířky)      |      RX panel      |
//   |                                     |                    |
//   +--------------------+----------------+--------------------+
//   |     TX panel        |         Statistiky                 |
//   +--------------------+-------------------------------------+

#include "app/panels.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "implot.h"

#include "protocol/framer.hpp"

namespace am {

namespace {

constexpr double kBaudOptions[] = {18.75, 31.25, 46.875, 62.5, 93.75};

const char* rxStateLabel(FrameReceiver::State s) {
    switch (s) {
        case FrameReceiver::State::SearchPreamble: return "hledání preambule";
        case FrameReceiver::State::Sync:            return "synchronizace";
        case FrameReceiver::State::Header:          return "hlavička";
        case FrameReceiver::State::Payload:         return "payload";
        case FrameReceiver::State::Crc:             return "CRC";
    }
    return "?";
}

bool isPrintableUtf8(const std::vector<uint8_t>& v) {
    for (unsigned char c : v) {
        // Povolíme tisknutelná ASCII + běžné řídicí znaky (nový řádek, tab)
        // a byty >= 0x80 (součást vícebajtových UTF-8 sekvencí).
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
        if (c == 0x7f) return false;
    }
    return true;
}

std::string toHex(const std::vector<uint8_t>& v) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(v.size() * 3);
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back(' ');
        out.push_back(digits[v[i] >> 4]);
        out.push_back(digits[v[i] & 0xF]);
    }
    return out;
}

} // namespace

void pushRxEvent(UiState& ui, const RxFrameEvent& ev) {
    RxLogEntry e;
    e.t_received = ev.t_received;
    e.crc_ok = ev.crc_ok;
    e.snr_db = ev.snr_db;
    e.payload_type = ev.payload_type;
    e.payload_len = ev.payload.size();

    const bool printable = ev.payload_type == kPayloadText && isPrintableUtf8(ev.payload);
    e.show_hex = !printable;
    if (printable) {
        e.text.assign(ev.payload.begin(), ev.payload.end());
    } else {
        e.hex = toHex(ev.payload);
    }

    // POZOR: RxFrameEvent nenese schéma/baud, kterým byl rámec skutečně
    // demodulován — bereme aktuální konfiguraci UI v okamžiku příjmu jako
    // nejlepší dostupný odhad a ukládáme ji do logu, aby propustnost šla
    // dopočítat konzistentně i zpětně (viz RxLogEntry). Hodnota je proto
    // označena jako orientační.
    const int bits_per_symbol =
        std::max(1, modemRegistry()[size_t(ui.scheme_index)].makeDemod()->bitsPerSymbol());
    e.baud_at_receive = ui.cfg.baud;
    e.bits_per_symbol_at_receive = bits_per_symbol;

    if (ev.crc_ok) {
        ++ui.stats.frames_ok;
        const double payload_bits = double(ev.payload.size()) * 8.0;
        const double symbol_time = 1.0 / ui.cfg.baud;
        const double airtime_s = (payload_bits / double(bits_per_symbol)) * symbol_time;
        ui.stats.last_throughput_bps = airtime_s > 0.0 ? double(ev.payload.size()) / airtime_s : 0.0;
    } else {
        ++ui.stats.frames_fail;
    }

    ui.rx_log.push_back(std::move(e));
    while (ui.rx_log.size() > 200) ui.rx_log.pop_front();
}

void drawControlBar(UiState& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = vp->WorkSize.x;
    const float bar_h = 150.0f;

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(w, bar_h));
    ImGui::Begin("Ovládání", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse);

    bool changed = false;

    // --- zvuková zařízení ---
    if (!ui.devices_enumerated) {
        // enumerate() vrací obě směry dohromady — rozdělíme podle is_capture.
        const auto all = ui.dsp.audio().enumerate();
        ui.capture_devices.clear();
        ui.playback_devices.clear();
        for (const auto& d : all)
            (d.is_capture ? ui.capture_devices : ui.playback_devices).push_back(d);
        ui.devices_enumerated = true;
    }

    ImGui::Text("Vstupní zařízení:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(300);
    {
        std::string cur = ui.capture_index < 0 ? "výchozí" : "?";
        for (const auto& d : ui.capture_devices)
            if (d.index == ui.capture_index) cur = d.name;
        if (ImGui::BeginCombo("##capture_dev", cur.c_str())) {
            bool sel_default = ui.capture_index < 0;
            if (ImGui::Selectable("výchozí", sel_default)) {
                ui.capture_index = -1;
                ui.dsp.restartAudio(ui.capture_index, ui.playback_index);
            }
            for (const auto& d : ui.capture_devices) {
                bool sel = d.index == ui.capture_index;
                if (ImGui::Selectable(d.name.c_str(), sel)) {
                    ui.capture_index = d.index;
                    ui.dsp.restartAudio(ui.capture_index, ui.playback_index);
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Text("Výstupní zařízení:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(300);
    {
        std::string cur = ui.playback_index < 0 ? "výchozí" : "?";
        for (const auto& d : ui.playback_devices)
            if (d.index == ui.playback_index) cur = d.name;
        if (ImGui::BeginCombo("##playback_dev", cur.c_str())) {
            bool sel_default = ui.playback_index < 0;
            if (ImGui::Selectable("výchozí", sel_default)) {
                ui.playback_index = -1;
                ui.dsp.restartAudio(ui.capture_index, ui.playback_index);
            }
            for (const auto& d : ui.playback_devices) {
                bool sel = d.index == ui.playback_index;
                if (ImGui::Selectable(d.name.c_str(), sel)) {
                    ui.playback_index = d.index;
                    ui.dsp.restartAudio(ui.capture_index, ui.playback_index);
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(30, 0));
    ImGui::SameLine();

    // --- modulace ---
    ImGui::BeginGroup();
    ImGui::Text("Modulace:");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(150);
    {
        const auto schemes = modemRegistry();
        const char* cur_name = ui.scheme_index >= 0 && size_t(ui.scheme_index) < schemes.size()
                                   ? schemes[size_t(ui.scheme_index)].name
                                   : "?";
        if (ImGui::BeginCombo("##scheme", cur_name)) {
            for (size_t i = 0; i < schemes.size(); ++i) {
                bool sel = int(i) == ui.scheme_index;
                if (ImGui::Selectable(schemes[i].name, sel)) {
                    ui.scheme_index = int(i);
                    // Q-FSK je navržená pro 62,5 Bd (1 kbit/s) — nastav
                    // automaticky (stejně jako modem_cli); uživatel může
                    // baud přebít comboxem níž.
                    if (std::strcmp(schemes[i].name, "Q-FSK") == 0)
                        ui.cfg.baud = 62.5;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Text("Baud:");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(150);
    {
        char cur_label[32];
        std::snprintf(cur_label, sizeof cur_label, "%.3f Bd", ui.cfg.baud);
        if (ImGui::BeginCombo("##baud", cur_label)) {
            for (double b : kBaudOptions) {
                char label[32];
                std::snprintf(label, sizeof label, "%.3f Bd", b);
                bool sel = std::abs(ui.cfg.baud - b) < 1e-6;
                if (ImGui::Selectable(label, sel)) {
                    ui.cfg.baud = b;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(30, 0));
    ImGui::SameLine();

    ImGui::BeginGroup();
    // f0/f1 jsou double (ModemConfig) — DragFloat by type-punoval double jako
    // float* (UB, widget efektivně nefunguje). DragScalar s
    // ImGuiDataType_Double pracuje se skutečným typem, žádný cast není
    // potřeba. Reconfigure spustíme, jen když se hodnota opravdu změnila
    // (ne při každém volání, které DragScalar vrátí true).
    {
        constexpr double kFreqMin = 500.0, kFreqMax = 4000.0;
        ImGui::SetNextItemWidth(200);
        double f0_before = ui.cfg.f0;
        if (ImGui::DragScalar("f0 (Hz)", ImGuiDataType_Double, &ui.cfg.f0, 1.0f,
                               &kFreqMin, &kFreqMax, "%.0f")) {
            ui.cfg.f0 = std::clamp(ui.cfg.f0, kFreqMin, kFreqMax);
            if (ui.cfg.f0 != f0_before) changed = true;
        }
        ImGui::SetNextItemWidth(200);
        double f1_before = ui.cfg.f1;
        if (ImGui::DragScalar("f1 (Hz)", ImGuiDataType_Double, &ui.cfg.f1, 1.0f,
                               &kFreqMin, &kFreqMax, "%.0f")) {
            ui.cfg.f1 = std::clamp(ui.cfg.f1, kFreqMin, kFreqMax);
            if (ui.cfg.f1 != f1_before) changed = true;
        }
    }
    ImGui::SetNextItemWidth(200);
    {
        float amp = float(ui.cfg.amplitude);
        if (ImGui::SliderFloat("amplituda", &amp, 0.0f, 1.0f)) {
            ui.cfg.amplitude = amp;
            changed = true;
        }
    }
    ImGui::EndGroup();

    if (changed) {
        ui.dsp.reconfigure(ui.cfg, ui.scheme_index);
    }

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(30, 0));
    ImGui::SameLine();

    // --- stavové indikátory ---
    ImGui::BeginGroup();
    {
        const bool running = ui.status.audio_running;
        ImVec4 col = running ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f) : ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
        ImGui::TextColored(col, "%s", running ? "● zvuk běží" : "● zvuk neběží");
    }
    ImGui::Text("Stav RX: %s", rxStateLabel(ui.status.rx_state));
    ImGui::Text("Korelace preambule");
    ImGui::ProgressBar(std::clamp(ui.status.corr_peak, 0.f, 1.f), ImVec2(200, 0));
    ImGui::Text("Úroveň vstupu");
    ImGui::ProgressBar(std::clamp(ui.status.input_peak, 0.f, 1.f), ImVec2(200, 0));
    ImGui::EndGroup();

    ImGui::End();
}

void drawWaterfallPanel(UiState& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = vp->WorkPos.y + 150.0f;
    const float total_w = vp->WorkSize.x;
    const float total_h = vp->WorkSize.y - 150.0f;
    const float w = total_w * 0.6f;
    const float h = total_h * 0.6f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, top));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("Vodopád", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse);

    // Nový snímek vodopádu, pokud je k dispozici; jinak ponecháme starou
    // matici beze změny.
    std::vector<float> tmp;
    int rows = 0, bins = 0;
    if (ui.dsp.waterfall().snapshot(tmp, rows, bins)) {
        ui.waterfall_data = std::move(tmp);
        ui.waterfall_rows = rows;
        ui.waterfall_bins = bins;
    }

    if (ui.waterfall_rows > 0 && ui.waterfall_bins > 0 &&
        ImPlot::BeginPlot("##waterfall_plot", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        // maxHz() je nakonfigurovaný STROP zobrazení (display_max_hz), ale
        // skutečná šířka dat je waterfall_bins binů široká sample_rate/fft_size
        // Hz (fft_size je ve Waterfall pevně 1024) — díky zaokrouhlení počtu
        // binů se skutečný rozsah od nakonfigurovaného stropu mírně liší.
        // Použijeme skutečný rozsah, jinak markery f0/f1 i osa neodpovídají
        // reálným pozicím binů v datech.
        constexpr double kFftSize = 1024.0;
        const double sample_rate = double(ui.dsp.config().sample_rate);
        const double bin_hz = sample_rate / kFftSize;
        const double actual_max_khz = double(ui.waterfall_bins) * bin_hz / 1000.0;

        ImPlot::SetupAxes("f (kHz)", "čas", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, actual_max_khz, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, double(ui.waterfall_rows), ImPlotCond_Always);

        ImPlot::PushColormap(ImPlotColormap_Viridis);
        ImPlot::PlotHeatmap("##heat", ui.waterfall_data.data(), ui.waterfall_rows,
                             ui.waterfall_bins, -90.0, -10.0, nullptr,
                             ImPlotPoint(0, 0), ImPlotPoint(actual_max_khz, double(ui.waterfall_rows)));
        ImPlot::PopColormap();

        // Svislé čáry na f0 a f1 aktuální konfigurace (v kHz).
        double f0_khz = ui.cfg.f0 / 1000.0;
        double f1_khz = ui.cfg.f1 / 1000.0;
        ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
        ImPlot::PlotInfLines("f0", &f0_khz, 1);
        ImPlot::PopStyleColor();
        ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(1.0f, 0.4f, 0.4f, 0.6f));
        ImPlot::PlotInfLines("f1", &f1_khz, 1);
        ImPlot::PopStyleColor();

        ImPlot::EndPlot();
    }

    ImGui::End();
}

void drawRxPanel(UiState& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = vp->WorkPos.y + 150.0f;
    const float total_w = vp->WorkSize.x;
    const float total_h = vp->WorkSize.y - 150.0f;
    const float wf_w = total_w * 0.6f;
    const float h = total_h * 0.6f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + wf_w, top));
    ImGui::SetNextWindowSize(ImVec2(total_w - wf_w, h));
    ImGui::Begin("Příjem (RX)", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse);

    // Vypustit nové rámce z DSP vlákna do UI logu.
    for (const auto& ev : ui.dsp.drainFrames()) pushRxEvent(ui, ev);
    ui.last_diag = ui.dsp.lastSymbolDiag();

    if (!ui.rx_log.empty()) {
        const auto& last = ui.rx_log.back();
        ImGui::Text("Poslední rámec: SNR %.1f dB", double(last.snr_db));
    } else {
        ImGui::Text("Poslední rámec: --");
    }

    // Fázor nenulový => schéma s konstelací (DBPSK). Přidáme do kruhového
    // bufferu jen při změně (jinak bychom mezi symboly kreslili duplicity).
    const bool has_phasor = ui.last_diag.phasor.real() != 0.f || ui.last_diag.phasor.imag() != 0.f;
    if (has_phasor) {
        if (!ui.have_last_phasor || ui.last_diag.phasor != ui.last_phasor) {
            ui.constellation_re.push_back(ui.last_diag.phasor.real());
            ui.constellation_im.push_back(ui.last_diag.phasor.imag());
            while (ui.constellation_re.size() > UiState::kConstellationCap) {
                ui.constellation_re.pop_front();
                ui.constellation_im.pop_front();
            }
            ui.last_phasor = ui.last_diag.phasor;
            ui.have_last_phasor = true;
        }
    }

    if (has_phasor) {
        // Konstelační diagram: body u +1 (Re) = bit 0, u -1 = bit 1.
        if (ImPlot::BeginPlot("Konstelace", ImVec2(-1, 140), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("Re", "Im");
            ImPlot::SetupAxisLimits(ImAxis_X1, -1.5, 1.5, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.5, 1.5, ImPlotCond_Always);

            // Slabý referenční kříž nulou.
            double cross_x[2] = {-1.5, 1.5};
            double cross_y[2] = {0.0, 0.0};
            double vert_x[2] = {0.0, 0.0};
            double vert_y[2] = {-1.5, 1.5};
            ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
            ImPlot::PlotLine("##cross_h", cross_x, cross_y, 2);
            ImPlot::PlotLine("##cross_v", vert_x, vert_y, 2);
            ImPlot::PopStyleColor();

            if (!ui.constellation_re.empty()) {
                std::vector<float> re(ui.constellation_re.begin(), ui.constellation_re.end());
                std::vector<float> im(ui.constellation_im.begin(), ui.constellation_im.end());
                ImPlot::PlotScatter("##constellation", re.data(), im.data(), int(re.size()));
            }

            // Popisky bitů u referenčních bodů konstelace (+1 = bit 0, -1 = bit 1).
            ImPlot::PlotText("0", 1.0, 0.0, ImVec2(0, -18));
            ImPlot::PlotText("1", -1.0, 0.0, ImVec2(0, -18));

            ImPlot::EndPlot();
        }
    } else if (!ui.last_diag.tone_energy.empty()) {
        // Sloupcový graf energií tónů (SymbolDiag::tone_energy) — funguje pro
        // libovolný počet tónů (2 nyní, až 16 pro MFSK).
        if (ImPlot::BeginPlot("Energie tónů", ImVec2(-1, 140), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("tón", "energie");
            ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, double(ui.last_diag.tone_energy.size()) - 0.5,
                                     ImPlotCond_Always);
            ImPlot::PlotBars("##tone_energy", ui.last_diag.tone_energy.data(),
                              int(ui.last_diag.tone_energy.size()), 0.67);
            ImPlot::EndPlot();
        }
    }

    ImGui::Separator();
    ImGui::Text("Log přijatých rámců:");
    ImGui::BeginChild("##rx_log", ImVec2(0, 0), true);
    for (const auto& e : ui.rx_log) {
        ImGui::Text("[%7.2f s]", e.t_received);
        ImGui::SameLine();
        if (e.crc_ok)
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "CRC OK");
        else
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "CRC FAIL");
        ImGui::SameLine();
        ImGui::Text("SNR %.1f dB (%zu B)", double(e.snr_db), e.payload_len);
        if (e.show_hex) {
            ImGui::TextWrapped("  %s", e.hex.c_str());
        } else {
            ImGui::TextWrapped("  %s", e.text.c_str());
        }
        ImGui::Separator();
    }
    ImGui::EndChild();

    ImGui::End();
}

void drawTxPanel(UiState& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // TX panel je vlevo dole, pod vodopádem.
    const float total_w = vp->WorkSize.x;
    const float total_h = vp->WorkSize.y - 150.0f;
    const float wf_w = total_w * 0.6f;
    const float wf_h = total_h * 0.6f;
    const float bottom_top = vp->WorkPos.y + 150.0f + wf_h;
    const float bottom_h = total_h - wf_h;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, bottom_top));
    ImGui::SetNextWindowSize(ImVec2(wf_w, bottom_h));
    ImGui::Begin("Vysílání (TX)", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse);

    ui.status = ui.dsp.status();
    const bool transmitting = ui.status.tx_progress >= 0.f;

    ImGui::Text("Text k odeslání:");
    ui.tx_buffer.resize(UiState::kTxBufCap, '\0');
    ImGui::InputTextMultiline("##tx_text", ui.tx_buffer.data(), UiState::kTxBufCap,
                               ImVec2(-1, bottom_h - 90.0f));
    // Ořízni na skutečnou délku C řetězce pro pozdější použití.
    ui.tx_buffer.resize(std::strlen(ui.tx_buffer.c_str()));

    ImGui::BeginDisabled(transmitting || ui.tx_buffer.empty());
    if (ImGui::Button("Odeslat")) {
        ui.dsp.sendText(ui.tx_buffer);
    }
    ImGui::EndDisabled();

    if (transmitting) {
        ImGui::SameLine();
        ImGui::Text("vysílám...");
        ImGui::ProgressBar(ui.status.tx_progress, ImVec2(-1, 0));
    }

    ImGui::End();
}

void drawStatsPanel(UiState& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float total_w = vp->WorkSize.x;
    const float total_h = vp->WorkSize.y - 150.0f;
    const float wf_w = total_w * 0.6f;
    const float wf_h = total_h * 0.6f;
    const float bottom_top = vp->WorkPos.y + 150.0f + wf_h;
    const float bottom_h = total_h - wf_h;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + wf_w, bottom_top));
    ImGui::SetNextWindowSize(ImVec2(total_w - wf_w, bottom_h));
    ImGui::Begin("Statistiky", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Rámce OK: %llu", static_cast<unsigned long long>(ui.stats.frames_ok));
    ImGui::Text("Rámce CRC FAIL: %llu", static_cast<unsigned long long>(ui.stats.frames_fail));
    ImGui::Text("Efektivní propustnost posledního rámce (orientační): %.1f B/s",
                ui.stats.last_throughput_bps);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Počítáno z baudu/schématu aktuálního v okamžiku příjmu "
                           "v UI, ne z hodnot, kterými byl rámec skutečně vysílán.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Živá chybovost (M6)");

    if (ImGui::Checkbox("Vysílat PRBS test", &ui.ber_test_tx)) {
        ui.dsp.setBerTestTx(ui.ber_test_tx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset statistik")) {
        ui.dsp.resetErrorStats();
    }
    ImGui::SameLine();
    ImGui::Checkbox("sledovat", &ui.follow_error_plots);

    ui.error_stats = ui.dsp.errorStats();
    const ErrorStats& es = ui.error_stats;

    ImGui::Text("Rámce OK / CRC FAIL: %llu / %llu",
                static_cast<unsigned long long>(es.frames_ok),
                static_cast<unsigned long long>(es.frames_crc_fail));
    if (es.prbs_bits > 0) {
        const double ber = double(es.prbs_errors) / double(es.prbs_bits);
        ImGui::Text("Celková BER: %.3e (%llu / %llu bitů)", ber,
                    static_cast<unsigned long long>(es.prbs_errors),
                    static_cast<unsigned long long>(es.prbs_bits));
    } else {
        ImGui::Text("Celková BER: -- (žádná PRBS data)");
    }
    const uint64_t total_frames = es.frames_ok + es.frames_crc_fail;
    if (total_frames > 0) {
        const double fer = 100.0 * double(es.frames_crc_fail) / double(total_frames);
        ImGui::Text("FER: %.2f %%", fer);
    } else {
        ImGui::Text("FER: -- %%");
    }

    if (es.tx_dropped > 0) {
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
                            "Zahozeno rámců při vysílání: %llu — "
                            "rámec delší než TX buffer — sniž payload/zvyš baud",
                            static_cast<unsigned long long>(es.tx_dropped));
    }

    // Společné X rozmezí pro oba grafy chybovosti — posledních ~60 s.
    double x_max = 0.0;
    for (const auto& p : es.ber) x_max = std::max(x_max, double(p.first));
    for (const auto& p : es.quality) x_max = std::max(x_max, double(p.first));
    const double x_min = std::max(0.0, x_max - 60.0);

    if (ImPlot::BeginPlot("BER", ImVec2(-1, 130), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("t (s)", "BER");
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 1e-5, 1.0, ImPlotCond_Always);
        if (ui.follow_error_plots) {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImGuiCond_Always);
        }
        if (!es.ber.empty()) {
            std::vector<double> xs, ys;
            xs.reserve(es.ber.size());
            ys.reserve(es.ber.size());
            for (const auto& p : es.ber) {
                xs.push_back(double(p.first));
                // log osa neumí zobrazit 0 — nulovou chybovost promítneme na spodní mez.
                ys.push_back(p.second > 1e-5f ? double(p.second) : 1e-5);
            }
            ImPlot::PlotStems("##ber", xs.data(), ys.data(), int(xs.size()));
        }
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("Kvalita [dB]", ImVec2(-1, 130), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("t (s)", "dB");
        if (ui.follow_error_plots) {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImGuiCond_Always);
        }
        if (!es.quality.empty()) {
            std::vector<double> xs, ys;
            xs.reserve(es.quality.size());
            ys.reserve(es.quality.size());
            for (const auto& p : es.quality) {
                xs.push_back(double(p.first));
                ys.push_back(double(p.second)); // NaN = mezera, ImPlot to zvládá
            }
            ImPlot::PlotLine("##quality", xs.data(), ys.data(), int(xs.size()));
        }
        ImPlot::EndPlot();
    }

    ImGui::End();
}

} // namespace am
