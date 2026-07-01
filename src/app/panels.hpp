#pragma once
// Panely GUI aplikace: definice sdíleného stavu UI a funkcí, které kreslí
// jednotlivé panely (Ovládání, Vodopád, RX, TX, Statistiky). main_gui.cpp
// pouze inicializuje SDL/ImGui/ImPlot a v hlavní smyčce volá tyto funkce.

#include <complex>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "app/dsp_thread.hpp"
#include "audio/audio_engine.hpp"
#include "core/config.hpp"
#include "modem/modulator.hpp"

namespace am {

// Jeden záznam v logu přijatých rámců (RX panel).
struct RxLogEntry {
    double      t_received = 0.0;
    bool        crc_ok = false;
    float       snr_db = 0.f;
    uint8_t     payload_type = 0;
    std::string text;              // pro UTF-8 tisknutelný text
    std::string hex;               // pro binární/netisknutelný obsah
    bool        show_hex = false;
    size_t      payload_len = 0;
};

// Statistiky přijaté od startu.
struct RxStats {
    uint64_t frames_ok = 0;
    uint64_t frames_fail = 0;
    double   last_throughput_bps = 0.0; // B/s posledního rámce
};

// Celý stav UI — vlastní ho main_gui.cpp, panely ho jen čtou/mění.
struct UiState {
    explicit UiState(DspThread& dsp) : dsp(dsp) {}

    DspThread& dsp;

    ModemConfig cfg;      // aktuální (odeslaná) konfigurace
    int scheme_index = 0; // index do modemRegistry()

    // Zvuková zařízení.
    std::vector<DeviceInfo> capture_devices;
    std::vector<DeviceInfo> playback_devices;
    int capture_index = -1;
    int playback_index = -1;
    bool devices_enumerated = false;

    // Vodopád: perzistentní buffer pro snapshot.
    std::vector<float> waterfall_data;
    int waterfall_rows = 0;
    int waterfall_bins = 0;

    // RX log a diagnostika.
    std::deque<RxLogEntry> rx_log; // nejnovější na konci, cap 200
    SymbolDiag last_diag;
    RxStats stats;

    // Konstelační diagram (DBPSK) — kruhový buffer posledních fázorů.
    static constexpr size_t kConstellationCap = 200;
    std::deque<float> constellation_re;
    std::deque<float> constellation_im;
    std::complex<float> last_phasor{};   // pro detekci změny oproti minulému snímku
    bool have_last_phasor = false;

    // Statistiky chybovosti (M6).
    ErrorStats error_stats;
    bool ber_test_tx = false;      // zrcadlo přepínače v DspThread (pro tlačítko)
    bool follow_error_plots = true; // "sledovat" — automaticky posouvat X osu

    // TX panel.
    std::string tx_buffer; // UTF-8 text ke vstupu InputTextMultiline
    static constexpr size_t kTxBufCap = 4096;

    // Poslední známý stav DSP vlákna (aktualizováno jednou za snímek).
    DspStatus status;
};

// Vloží nový rámec do stavu UI (log + statistiky). Volá se z main smyčky
// po DspThread::drainFrames().
void pushRxEvent(UiState& ui, const RxFrameEvent& ev);

// Jednotlivé panely — každý si sám spočítá svou pozici/velikost podle
// aktuální velikosti viewportu.
void drawControlBar(UiState& ui);
void drawWaterfallPanel(UiState& ui);
void drawRxPanel(UiState& ui);
void drawTxPanel(UiState& ui);
void drawStatsPanel(UiState& ui);

} // namespace am
