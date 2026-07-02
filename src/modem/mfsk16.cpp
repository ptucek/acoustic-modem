// 16-FSK: 16 ortogonálních tónů mfsk_base + k·mfsk_spacing, 4 bity/symbol
// → 4× propustnost proti 2-FSK při stejné symbolové rychlosti.
//
// Index tónu je Grayovsky kódovaný: nejpravděpodobnější chyba (záměna za
// SOUSEDNÍ tón, např. vlivem šumu nebo dozvuku) pak poškodí jen jeden bit.
//
// Výchozí rozteč 62,5 Hz = 2 × baud → tóny jsou přes okno symbolu
// ortogonální; celá sada 1000–1937,5 Hz leží v rovné části odezvy
// notebookových reproduktorů i mikrofonů.

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/goertzel.hpp"
#include "modem/schemes.hpp"

namespace am {
namespace {

// Protokol (SYNC 16 b, hlavička 40 b, bajtový payload) předpokládá, že
// bitsPerSymbol dělí 8 — jinak se pole rámce nezarovnají na hranice
// symbolů (nález z review: 8 tónů = 3 bity/symbol tiše rozbije hlavičku).
// Povolené počty tónů jsou 2/4/16/256 (bps 1/2/4/8); jiné hodnoty se
// srazí na nejbližší nižší povolenou.
int sanitizeTones(int tones) {
    if (tones >= 256) return 256;
    if (tones >= 16) return 16;
    if (tones >= 4) return 4;
    return 2;
}

class Mfsk16Modulator final : public IModulator {
public:
    void configure(const ModemConfig& c) override {
        cfg_ = c;
        cfg_.mfsk_tones = sanitizeTones(c.mfsk_tones);
        bits_per_sym_ = 0;
        for (int t = cfg_.mfsk_tones; t > 1; t >>= 1) ++bits_per_sym_;
        reset();
    }
    int  bitsPerSymbol() const override { return bits_per_sym_; }
    void reset() override { phase_ = 0.0; }

    void modulate(const BitBuffer& bits, std::vector<float>& out) override {
        const int sps = cfg_.samplesPerSymbol();
        const size_t n_sym = bits.size() / size_t(bits_per_sym_);
        out.reserve(out.size() + n_sym * size_t(sps));
        for (size_t i = 0; i < n_sym; ++i) {
            // hodnota symbolu → Grayův kód → index tónu
            const uint32_t tone = grayEncode(bits.symbol(i, bits_per_sym_));
            const double f = cfg_.mfsk_base + tone * cfg_.mfsk_spacing;
            const double dphi = 2.0 * std::numbers::pi * f / cfg_.sample_rate;
            for (int s = 0; s < sps; ++s) {
                out.push_back(float(cfg_.amplitude * std::sin(phase_)));
                phase_ += dphi; // spojitá fáze přes hranice symbolů (CPFSK)
                if (phase_ > 2.0 * std::numbers::pi)
                    phase_ -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    ModemConfig cfg_;
    int bits_per_sym_ = 4;
    double phase_ = 0.0;
};

class Mfsk16Demodulator final : public IDemodulator {
public:
    void configure(const ModemConfig& c) override {
        const int tones = sanitizeTones(c.mfsk_tones);
        bits_per_sym_ = 0;
        for (int t = tones; t > 1; t >>= 1) ++bits_per_sym_;
        gs_.clear();
        for (int k = 0; k < tones; ++k)
            gs_.emplace_back(c.mfsk_base + k * c.mfsk_spacing, c.sample_rate,
                             c.samplesPerSymbol());
    }
    int  bitsPerSymbol() const override { return bits_per_sym_; }
    void reset() override {}

    uint64_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) override {
        float best = -1.f, second = 0.f;
        size_t best_k = 0;
        std::vector<float> energies(gs_.size());
        for (size_t k = 0; k < gs_.size(); ++k) {
            const float e = Goertzel::energy(gs_[k].run(sym));
            energies[k] = e;
            if (e > best) {
                second = best;
                best = e;
                best_k = k;
            } else if (e > second) {
                second = e;
            }
        }
        if (diag) {
            diag->tone_energy = std::move(energies);
            diag->phasor = {};
            // rezerva: vítězný tón vs. druhý nejsilnější
            diag->snr_db =
                10.f * std::log10((best + 1e-12f) / (second + 1e-12f));
        }
        return grayDecode(uint32_t(best_k)); // index tónu → hodnota symbolu
    }

private:
    int bits_per_sym_ = 4;
    std::vector<Goertzel> gs_;
};

} // namespace

std::unique_ptr<IModulator>   makeMfsk16Mod()   { return std::make_unique<Mfsk16Modulator>(); }
std::unique_ptr<IDemodulator> makeMfsk16Demod() { return std::make_unique<Mfsk16Demodulator>(); }

} // namespace am
