#pragma once
// RX strana protokolu: streamovací stavový automat vzorky → rámce.
// Přijímá libovolně velké bloky vzorků (z ring bufferu i z WAV souboru),
// hledá chirp korelací, pak krájí okna symbolů pevnými hodinami a krmí
// jimi demodulátor. Po CRC se vrací do hledání preambule.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/config.hpp"
#include "dsp/chirp.hpp"
#include "modem/modulator.hpp"

namespace am {

class FrameReceiver {
public:
    enum class State { SearchPreamble, Sync, Header, Payload, Crc };

    struct Result {
        std::vector<uint8_t> payload;
        uint8_t payload_type = 0;
        bool  crc_ok = false;
        float mean_snr_db = 0.f;   // průměr přes symboly rámce
    };

    void configure(const ModemConfig& cfg, const ModemScheme& scheme,
                   const PreambleSpec& pre = {});
    void reset();

    void pushSamples(std::span<const float> x);
    std::optional<Result> poll(); // dokončené rámce (fronta)

    // Diagnostika pro GUI
    State state() const { return state_; }
    float lastCorrPeak() const { return last_corr_peak_; }
    const SymbolDiag& lastSymbolDiag() const { return last_diag_; }

    // Volitelný hook: zavolá se po každém demodulovaném symbolu (živá
    // chybovost, konstelace). Volá se z DSP vlákna.
    std::function<void(const SymbolDiag&, uint64_t bits)> onSymbol;

private:
    void processBuffered();
    bool takeSymbol(uint64_t& bits_out); // false = zatím není dost vzorků
    void restartSearch(size_t keep_from);

    ModemConfig cfg_;
    PreambleSpec pre_;
    const ModemScheme* scheme_ = nullptr;
    std::unique_ptr<IDemodulator> demod_;
    ChirpCorrelator corr_;

    std::vector<float> buf_;      // pracovní buffer vzorků
    size_t read_pos_ = 0;         // začátek dalšího symbolu v buf_
    size_t frame_start_ = 0;      // hranice 1. symbolu aktuálního rámce (souř. buf_)
    State state_ = State::SearchPreamble;

    // rozpracovaný rámec
    BitBuffer rx_bits_;
    size_t payload_len_ = 0;
    uint8_t ver_flags_ = 0;
    float snr_accum_ = 0.f;
    int   snr_count_ = 0;

    float last_corr_peak_ = 0.f;
    SymbolDiag last_diag_;
    std::deque<Result> results_;
};

} // namespace am
