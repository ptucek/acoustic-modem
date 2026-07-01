// DBPSK (diferenciální BPSK): bit se kóduje ZMĚNOU fáze nosné mezi
// sousedními symboly (1 = otočení o 180°, 0 = beze změny), ne fází samotnou.
//
// Proč diferenciální a ne koherentní BPSK: koherentní detekce vyžaduje
// obnovu fáze nosné (Costasova smyčka), která by přes akustický kanál
// musela ustát trvalou rotaci fáze z rozdílu hodin zvukových karet.
// Diferenciální detekce porovnává jen sousední symboly — pomalá rotace se
// vyruší. Rozpočet: rotace/symbol = 360°·Δf/baud; při ±0,5 Hz a 31,25 Bd
// je to ~6°/symbol proti rozhodovací rezervě 90°. Cena: 3 dB oproti
// koherentní PSK, při našich SNR nepodstatné; úspora: ~300 řádků smyčky.
//
// RX: fázor c_k Goertzelem na f0; bit = Re(c_k · conj(c_{k-1})) < 0.
// První symbol po resetu nemá referenci — to je protokolární referenční
// symbol, který přijímač zahazuje.

#include <cmath>
#include <numbers>

#include "dsp/goertzel.hpp"
#include "modem/schemes.hpp"

namespace am {
namespace {

class DbpskModulator final : public IModulator {
public:
    void configure(const ModemConfig& c) override {
        cfg_ = c;
        reset();
    }
    int  bitsPerSymbol() const override { return 1; }
    void reset() override {
        phase_ = 0.0;
        offset_ = 0.0;
    }

    void modulate(const BitBuffer& bits, std::vector<float>& out) override {
        const int sps = cfg_.samplesPerSymbol();
        const double dphi = 2.0 * std::numbers::pi * cfg_.f0 / cfg_.sample_rate;
        const int n_ramp = std::min(sps / 8, int(0.002 * cfg_.sample_rate));
        out.reserve(out.size() + bits.size() * size_t(sps));
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits.bit(i)) offset_ += std::numbers::pi; // bit 1 → otočení
            for (int s = 0; s < sps; ++s) {
                // krátká amplitudová prohlubeň kolem hranice symbolu změkčí
                // fázový skok (méně širokopásmového „cvaknutí")
                double env = 1.0;
                if (s < n_ramp)
                    env = 0.5 - 0.5 * std::cos(std::numbers::pi * s / n_ramp);
                else if (s >= sps - n_ramp)
                    env = 0.5 -
                          0.5 * std::cos(std::numbers::pi * (sps - s) / n_ramp);
                out.push_back(
                    float(cfg_.amplitude * env * std::sin(phase_ + offset_)));
                phase_ += dphi;
                if (phase_ > 2.0 * std::numbers::pi)
                    phase_ -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    ModemConfig cfg_;
    double phase_ = 0.0;  // fáze nosné (běží spojitě)
    double offset_ = 0.0; // datový fázový posuv (0 / π)
};

class DbpskDemodulator final : public IDemodulator {
public:
    void configure(const ModemConfig& c) override {
        const int sps = c.samplesPerSymbol();
        g_ = Goertzel(c.f0, c.sample_rate, sps);
        // Pozor: na symbol nemusí připadat celý počet cyklů nosné
        // (1200 Hz / 31,25 Bd = 38,4 cyklu). Fázor se pak mezi sousedními
        // okny deterministicky pootočí o 2π·frac(f0/baud) — bez kompenzace
        // by to diferenciální detekce četla jako datové otočení.
        const double cycles = c.f0 * sps / double(c.sample_rate);
        const double theta =
            2.0 * std::numbers::pi * (cycles - std::floor(cycles));
        rot_ = {float(std::cos(-theta)), float(std::sin(-theta))};
        reset();
    }
    int  bitsPerSymbol() const override { return 1; }
    void reset() override { prev_ = {0.f, 0.f}; }

    uint32_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) override {
        const std::complex<float> c = g_.run(sym);
        const std::complex<float> d = c * std::conj(prev_) * rot_;
        const bool bit = d.real() < 0.f; // otočení fáze → bit 1
        prev_ = c;
        if (diag) {
            // normalizovaný diferenciální fázor — bod „konstelace" pro GUI
            const float m = std::abs(d);
            diag->phasor = m > 1e-12f ? d / m : std::complex<float>{};
            diag->tone_energy = {std::norm(c)};
            // rezerva rozhodnutí: |Re| vs |Im| složka diferenciálního fázoru
            diag->snr_db =
                10.f * std::log10((std::abs(d.real()) + 1e-12f) /
                                  (std::abs(d.imag()) + 1e-12f));
        }
        return bit ? 1u : 0u;
    }

private:
    Goertzel g_;
    std::complex<float> prev_{0.f, 0.f};
    std::complex<float> rot_{1.f, 0.f}; // kompenzace necelých cyklů/symbol
};

} // namespace

std::unique_ptr<IModulator>   makeDbpskMod()   { return std::make_unique<DbpskModulator>(); }
std::unique_ptr<IDemodulator> makeDbpskDemod() { return std::make_unique<DbpskDemodulator>(); }

} // namespace am
