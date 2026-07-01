// Testy Goertzelova detektoru: energie na binu, ortogonalita tónů
// s roztečí k*fs/N a smysluplná fáze pro DBPSK.

#include <cmath>
#include <numbers>
#include <vector>

#include "doctest/doctest.h"
#include "dsp/goertzel.hpp"

namespace {

std::vector<float> tone(double f, double fs, int n, double amp = 1.0,
                        double phase0 = 0.0) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        x[size_t(i)] = float(
            amp * std::sin(2.0 * std::numbers::pi * f * i / fs + phase0));
    return x;
}

} // namespace

TEST_CASE("Goertzel: energie čistého tónu na binu") {
    const double fs = 48000;
    const int n = 1536; // 31,25 Bd symbol
    const double f = 1200; // 1200 = 38,4 * 31,25 … není bin; použij 1218,75? ne:
    // 1200/31,25 = 38,4 → není celý násobek binu. Pro test vezmi přesný bin:
    const double fbin = 40 * fs / n; // 1250 Hz, přesně 40. bin
    (void)f;
    auto x = tone(fbin, fs, n, 0.7);
    am::Goertzel g(fbin, fs, n);
    const float mag = std::abs(g.run(x));
    CHECK(mag == doctest::Approx(0.7).epsilon(0.02)); // |y| ≈ amplituda
}

TEST_CASE("Goertzel: ortogonální tón nepřetéká") {
    const double fs = 48000;
    const int n = 1536;
    const double f_bin = 40 * fs / n;   // 1250 Hz
    const double f_other = 72 * fs / n; // 2250 Hz — jiný bin
    auto x = tone(f_other, fs, n, 1.0);
    am::Goertzel g(f_bin, fs, n);
    CHECK(std::abs(g.run(x)) < 0.01); // > 40 dB potlačení
}

TEST_CASE("Goertzel: výchozí frekvence f0/f1 jsou přes symbol ortogonální") {
    // f0=1200, f1=2200 při 31,25 Bd: 1200/31,25=38,4 … tóny NEJSOU přesně na
    // binech okna, ale jejich ROZDÍL 1000 Hz = 32*31,25 je celý násobek → mezi
    // sebou jsou ortogonální, což je to, na čem FSK demodulátoru záleží.
    const double fs = 48000;
    const int n = 1536;
    auto x = tone(2200, fs, n, 1.0);
    am::Goertzel g0(1200, fs, n);
    am::Goertzel g1(2200, fs, n);
    const float leak = std::abs(g0.run(x));
    const float sig = std::abs(g1.run(x));
    CHECK(sig > 0.9f);
    CHECK(leak < 0.05f * sig);
}

TEST_CASE("Goertzel: fáze sleduje posun signálu (základ DBPSK)") {
    const double fs = 48000;
    const int n = 1536;
    const double fbin = 40 * fs / n;
    auto a = tone(fbin, fs, n, 1.0, 0.0);
    auto b = tone(fbin, fs, n, 1.0, std::numbers::pi); // otočená fáze
    am::Goertzel g(fbin, fs, n);
    const auto ca = g.run(a);
    const auto cb = g.run(b);
    // otočení o 180°: skalární součin fázorů je záporný
    CHECK((ca * std::conj(cb)).real() < 0.f);
    // stejná fáze: kladný
    CHECK((ca * std::conj(ca)).real() > 0.f);
}
