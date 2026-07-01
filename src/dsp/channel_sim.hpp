#pragma once
// Simulátor akustického kanálu — pouze pro testy a CLI `chansim`.
// Umožňuje ověřit celý řetězec modulace→demodulace bez zvukové karty.

#include <cstdint>
#include <span>
#include <vector>

namespace am {

struct ChannelParams {
    double gain      = 1.0;   // útlum/zesílení
    double snr_db    = 99.0;  // AWGN vůči RMS signálu; >= 99 → bez šumu
    double drift_ppm = 0.0;   // rozdíl hodin zvukových karet (převzorkování)
    double echo_delay_s = 0.0; // jednoodbočkové echo (hrubý dozvuk)
    double echo_gain    = 0.0;
    uint32_t seed = 1;        // deterministický šum pro reprodukovatelné testy
};

std::vector<float> simulateChannel(std::span<const float> x, const ChannelParams& p,
                                   int sample_rate);

} // namespace am
