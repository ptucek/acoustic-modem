#pragma once
// Malý FIR filtr (dolní propust, okno Hamming) + decimace — používá se
// pro zlevnění korelačního hledání chirpu.

#include <span>
#include <vector>

namespace am {

// Návrh dolní propusti metodou okna: mezní frekvence fc (Hz), počet tapů lichý.
std::vector<float> designLowpass(double fc_hz, double sample_rate, int taps);

// Konvoluce + decimace: y[k] = sum taps[j] * x[k*decim + j]. Vrací
// vzorky pouze tam, kde je filtr plně překrytý (bez okrajů).
std::vector<float> filterDecimate(std::span<const float> x,
                                  std::span<const float> taps, int decim);

} // namespace am
