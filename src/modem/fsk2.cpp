// 2-FSK: bit 0 → tón f0 (space), bit 1 → tón f1 (mark).
//
// TX je spojitá fáze (CPFSK): fázový akumulátor běží přes hranice symbolů,
// takže signál nemá skoky → žádné lupance a úzké spektrum (na rozdíl od
// přepínání dvou nezávislých oscilátorů).
//
// RX: Goertzel na f0 a f1 přes okno symbolu, bit = argmax energie. Rozteč
// tónů je násobkem baud (1000 Hz = 32 × 31,25), takže tóny jsou přes okno
// symbolu ortogonální — energie jednoho nepřetéká do binu druhého.

#include <cmath>
#include <numbers>

#include "dsp/goertzel.hpp"
#include "modem/schemes.hpp"

namespace am {
namespace {

class Fsk2Modulator final : public IModulator {
public:
    void configure(const ModemConfig& c) override {
        cfg_ = c;
        reset();
    }
    int  bitsPerSymbol() const override { return 1; }
    void reset() override { phase_ = 0.0; }

    void modulate(const BitBuffer& bits, std::vector<float>& out) override {
        const int sps = cfg_.samplesPerSymbol();
        out.reserve(out.size() + bits.size() * size_t(sps));
        for (size_t i = 0; i < bits.size(); ++i) {
            const double f = bits.bit(i) ? cfg_.f1 : cfg_.f0;
            const double dphi = 2.0 * std::numbers::pi * f / cfg_.sample_rate;
            for (int s = 0; s < sps; ++s) {
                out.push_back(float(cfg_.amplitude * std::sin(phase_)));
                phase_ += dphi;
                if (phase_ > 2.0 * std::numbers::pi)
                    phase_ -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    ModemConfig cfg_;
    double phase_ = 0.0;
};

class Fsk2Demodulator final : public IDemodulator {
public:
    void configure(const ModemConfig& c) override {
        const int sps = c.samplesPerSymbol();
        g0_ = Goertzel(c.f0, c.sample_rate, sps);
        g1_ = Goertzel(c.f1, c.sample_rate, sps);
    }
    int  bitsPerSymbol() const override { return 1; }
    void reset() override {}

    uint64_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) override {
        const float e0 = Goertzel::energy(g0_.run(sym));
        const float e1 = Goertzel::energy(g1_.run(sym));
        const bool bit = e1 > e0;
        if (diag) {
            diag->tone_energy = {e0, e1};
            diag->phasor = {};
            const float win = bit ? e1 : e0, lose = bit ? e0 : e1;
            diag->snr_db = 10.f * std::log10((win + 1e-12f) / (lose + 1e-12f));
        }
        return bit ? 1u : 0u;
    }

private:
    Goertzel g0_, g1_;
};

} // namespace

std::unique_ptr<IModulator>   makeFsk2Mod()   { return std::make_unique<Fsk2Modulator>(); }
std::unique_ptr<IDemodulator> makeFsk2Demod() { return std::make_unique<Fsk2Demodulator>(); }

} // namespace am
