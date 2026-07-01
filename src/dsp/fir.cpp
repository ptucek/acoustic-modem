// Návrh FIR dolní propusti metodou okna (sinc × Hamming) a decimace.

#include "dsp/fir.hpp"

#include <cmath>
#include <numbers>

namespace am {

std::vector<float> designLowpass(double fc_hz, double sample_rate, int taps) {
    // taps liché → celočíselné zpoždění (taps-1)/2
    if (taps % 2 == 0) ++taps;
    std::vector<float> h(static_cast<size_t>(taps));
    const double fc = fc_hz / sample_rate; // normovaná mezní frekvence (0..0.5)
    const int mid = (taps - 1) / 2;
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const int k = i - mid;
        // ideální dolní propust: 2*fc*sinc(2*fc*k)
        double v = (k == 0) ? 2.0 * fc
                            : std::sin(2.0 * std::numbers::pi * fc * k) /
                                  (std::numbers::pi * k);
        // Hammingovo okno potlačí zvlnění z useknutí sincu
        v *= 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * i / (taps - 1));
        h[size_t(i)] = float(v);
        sum += v;
    }
    for (float& v : h) v = float(v / sum); // jednotkový zisk na DC
    return h;
}

std::vector<float> filterDecimate(std::span<const float> x,
                                  std::span<const float> taps, int decim) {
    std::vector<float> y;
    if (x.size() < taps.size()) return y;
    const size_t n_out = (x.size() - taps.size()) / size_t(decim) + 1;
    y.reserve(n_out);
    for (size_t k = 0; k < n_out; ++k) {
        double acc = 0.0;
        const float* xp = x.data() + k * size_t(decim);
        for (size_t j = 0; j < taps.size(); ++j) acc += double(taps[j]) * xp[j];
        y.push_back(float(acc));
    }
    return y;
}

} // namespace am
