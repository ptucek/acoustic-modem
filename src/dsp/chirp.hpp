#pragma once
// Lineární chirp pro synchronizaci rámce. Autokorelace chirpu je jediná
// ostrá špička (na rozdíl od střídavých tónů, které korelují každou
// periodu), a přežije frekvenčně selektivní odezvu místnosti i dozvuk.

#include <span>
#include <vector>

#include "core/config.hpp"

namespace am {

// Vygeneruje vzorky lineárního chirpu f_start → f_end (Hannova obálka
// proti lupancům na krajích).
std::vector<float> makeChirp(const PreambleSpec& pre, int sample_rate,
                             double amplitude);

// Normalizovaný křížový korelátor proti známému chirpu, streamovací.
// CPU trik: vstup se filtruje FIR dolní propustí a decimuje ×4 (48→12 kHz),
// korelace běží na 12 kHz, poté se špička zpřesní na plném rozlišení.
class ChirpCorrelator {
public:
    void configure(const PreambleSpec& pre, int sample_rate);

    // Prohledá `x` (celý dostupný buffer od `from`). Když najde špičku
    // normalizované korelace >= threshold, vrátí index PRVNÍHO vzorku
    // za koncem chirpu + mezery (= hranice prvního symbolu) v souřadnicích
    // `x`; jinak -1. `peak_out` vrací hodnotu špičky (0..1) pro GUI metr.
    long search(std::span<const float> x, size_t from, float threshold,
                float* peak_out) const;

    int chirpLen() const { return int(template_48k_.size()); }
    int gapLen() const { return gap_len_; }

private:
    std::vector<float> template_48k_;  // chirp na plné frekvenci
    std::vector<float> template_dec_;  // decimovaný chirp (12 kHz)
    std::vector<float> lp_taps_;       // FIR dolní propust pro decimaci
    int decim_ = 4;
    int gap_len_ = 0;
    int sample_rate_ = 48000;
};

} // namespace am
