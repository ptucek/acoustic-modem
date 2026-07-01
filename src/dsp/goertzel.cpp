// Goertzelův algoritmus — rekurzivní výpočet jedné DFT složky.
// Rekurence: s[n] = x[n] + 2*cos(w)*s[n-1] - s[n-2]
// Po N vzorcích:  y = s[N-1] - e^{-jw} * s[N-2]
// Normalizace 2/N: čistý sinus o amplitudě A na přesné frekvenci dá |y| ≈ A.

#include "dsp/goertzel.hpp"

#include <cmath>
#include <numbers>

namespace am {

Goertzel::Goertzel(double freq_hz, double sample_rate, int n) : n_(n) {
    const double w = 2.0 * std::numbers::pi * freq_hz / sample_rate;
    cos_w_ = std::cos(w);
    sin_w_ = std::sin(w);
    coeff_ = 2.0 * cos_w_;
}

std::complex<float> Goertzel::run(std::span<const float> x) const {
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) {
        const double s0 = double(v) + coeff_ * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cos_w_;
    const double im = s2 * sin_w_;
    const double scale = 2.0 / double(n_ > 0 ? n_ : x.size());
    return {float(re * scale), float(im * scale)};
}

} // namespace am
