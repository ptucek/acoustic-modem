#pragma once
// Goertzelův algoritmus s komplexním výstupem — detekce energie a fáze
// jednoho tónu v okně vzorků. Levnější a didaktičtější než FFT, když nás
// zajímá jen pár frekvencí (2 u FSK, 16 u MFSK, 1 u OOK/DBPSK).

#include <complex>
#include <span>

namespace am {

class Goertzel {
public:
    Goertzel() = default;
    Goertzel(double freq_hz, double sample_rate, int n);

    // Komplexní výsledek: |.|² je energie tónu, arg(.) fáze (pro DBPSK).
    std::complex<float> run(std::span<const float> x) const;

    static float energy(const std::complex<float>& c) { return std::norm(c); }

private:
    double coeff_ = 0.0;   // 2*cos(w)
    double cos_w_ = 0.0, sin_w_ = 0.0;
    int    n_     = 0;
};

} // namespace am
