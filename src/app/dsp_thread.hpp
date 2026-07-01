#pragma once
// DSP vlákno GUI aplikace: čte mikrofonní vzorky z ring bufferu, krmí
// vodopád a FrameReceiver, obsluhuje příkazy z GUI (rekonfigurace, odeslání
// zprávy) a publikuje eventy zpět. GUI strana není real-time → mutexy stačí;
// jediný lock-free úsek je audio callback (SpscRing v AudioEngine).

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.hpp"
#include "app/waterfall.hpp"
#include "core/config.hpp"
#include "core/spsc_ring.hpp"
#include "modem/modulator.hpp"
#include "protocol/frame_receiver.hpp"

namespace am {

struct RxFrameEvent {
    std::vector<uint8_t> payload;
    uint8_t payload_type = 0;
    bool crc_ok = false;
    float snr_db = 0.f;
    double t_received = 0.0; // sekundy od startu DSP vlákna
};

// Živá chybovost (M6): BER z PRBS testovacích rámců, FER z CRC, průběžná
// „kvalita" = rozhodovací rezerva demodulátoru v dB (funguje i mezi rámci).
struct ErrorStats {
    uint64_t frames_ok = 0;
    uint64_t frames_crc_fail = 0;
    uint64_t prbs_bits = 0;    // celkem porovnaných bitů
    uint64_t prbs_errors = 0;  // celkem chybných bitů
    // časové řady (x = sekundy od startu vlákna) pro ImPlot
    std::vector<std::pair<float, float>> ber;     // BER jednotlivých PRBS rámců
    std::vector<std::pair<float, float>> quality; // odhad rezervy/SNR [dB]
    bool ber_test_tx = false; // právě se opakovaně vysílá PRBS
};

struct DspStatus {
    FrameReceiver::State rx_state = FrameReceiver::State::SearchPreamble;
    float corr_peak = 0.f;   // metr korelace chirpu (0..1)
    float input_peak = 0.f;  // špička vstupu (0..1)
    float tx_progress = -1.f; // 0..1 při vysílání, -1 = nevysílá se
    bool audio_running = false;
};

class DspThread {
public:
    DspThread();
    ~DspThread();

    // Spustí audio (výchozí zařízení) a zpracovací vlákno.
    bool start(const ModemConfig& cfg, int scheme_index);
    void stop();

    // ---- příkazy z GUI (thread-safe) ----
    void reconfigure(const ModemConfig& cfg, int scheme_index);
    bool restartAudio(int capture_index, int playback_index);
    void sendText(const std::string& utf8);
    void sendBytes(std::vector<uint8_t> payload, uint8_t payload_type);

    // Testovací režim BER: opakovaně vysílá rámce se známou PRBS-15
    // sekvencí; přijatá strana (tento i druhý stroj) je porovnává bit po
    // bitu. Zapíná se nezávisle na běžném provozu.
    void setBerTestTx(bool on);
    ErrorStats errorStats(); // kopie statistik pro GUI
    void resetErrorStats();

    // ---- čtení z GUI (thread-safe) ----
    std::vector<RxFrameEvent> drainFrames();
    DspStatus status();
    SymbolDiag lastSymbolDiag(); // pro sloupce energií tónů / konstelaci
    Waterfall& waterfall() { return waterfall_; }
    AudioEngine& audio() { return audio_; }
    ModemConfig config(); // aktuální platná konfigurace

private:
    void run(); // tělo vlákna

    AudioEngine audio_;
    Waterfall waterfall_;
    SpscRing<float> rx_ring_{1u << 20}; // ~22 s
    SpscRing<float> tx_ring_{1u << 22}; // ~87 s — celý rámec najednou

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex mtx_; // chrání vše níže
    ModemConfig cfg_;
    int scheme_index_ = 0;
    bool reconfigure_pending_ = false;
    std::deque<std::vector<float>> tx_queue_; // hotové rámce k odvysílání
    std::deque<RxFrameEvent> rx_events_;
    SymbolDiag last_diag_;
    DspStatus status_;
    size_t tx_total_ = 0; // vzorků aktuálního vysílání (pro progress)
    int capture_index_ = -1, playback_index_ = -1;

    ErrorStats stats_;
    bool ber_test_tx_ = false;

    void enqueueFrameLocked(std::span<const uint8_t> payload,
                            uint8_t payload_type); // volat s drženým mtx_
};

} // namespace am
