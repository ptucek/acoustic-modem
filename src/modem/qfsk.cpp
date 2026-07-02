// Q-FSK: 4 paralelní 16-FSK skupiny → 16 bitů/symbol → 8× propustnost
// 16-FSK (1 kbit/s @ 62,5 Bd). Skupiny leží v pásmech změřených sondáží
// kanálu (docs/measurements.md), vyhýbají se dozvukovým zářezům obou směrů:
//   G1 1050 / G2 2800 / G3 4300 / G4 6300 Hz, každá 16 tónů á baud (Hz).
//
// Každá skupina nese 4 bity — index tónu Grayovsky kódovaný jako u 16-FSK,
// takže záměna za sousední tón poškodí jen jeden bit. Modulátor sečte 4
// současné tóny; každý má amplitudu cfg.amplitude/skupiny, takže součet
// nikdy nepřekročí cfg.amplitude (žádné klipování). Demodulátor běží
// Goertzel na všech 4×16 tónech a v každé skupině vezme argmax nezávisle.
//
// Rozteč tónů = baud → přes okno symbolu (samplesPerSymbol vzorků) leží
// sousední tóny na sousedních DFT binech → ortogonální (nekoherentní
// detekce energie). Absolutní poloha binů nemusí být celočíselná;
// ortogonalitu drží celočíselný ODSTUP, ne absolutní poloha.

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/goertzel.hpp"
#include "modem/schemes.hpp"

namespace am {
namespace {

constexpr int kGroups        = 4;
constexpr int kTonesPerGroup = 16;
constexpr int kBitsPerGroup  = 4;             // log2(16)
constexpr int kBitsPerSymbol = kGroups * kBitsPerGroup; // 16

// Základny skupin (Hz) — změřená průchozí pásma bez dozvukových zářezů
// (obousměrně, viz docs/measurements.md „Sondáž kanálu").
constexpr std::array<double, kGroups> kGroupBase = {1050.0, 2800.0, 4300.0, 6300.0};

class QfskModulator final : public IModulator {
public:
    void configure(const ModemConfig& c) override {
        cfg_ = c;
        reset();
    }
    int  bitsPerSymbol() const override { return kBitsPerSymbol; }
    void reset() override { phase_.fill(0.0); }

    void modulate(const BitBuffer& bits, std::vector<float>& out) override {
        const int    sps     = cfg_.samplesPerSymbol();
        const size_t n_sym   = bits.size() / size_t(kBitsPerSymbol);
        const double amp     = cfg_.amplitude / double(kGroups); // součet ≤ amplitude
        const double spacing = cfg_.baud;                        // rozteč = 1 DFT bin
        out.reserve(out.size() + n_sym * size_t(sps));

        for (size_t i = 0; i < n_sym; ++i) {
            const uint32_t sym = bits.symbol(i, kBitsPerSymbol);
            std::array<double, kGroups> dphi{};
            for (int g = 0; g < kGroups; ++g) {
                const uint32_t tone = grayEncode((sym >> (g * kBitsPerGroup)) & 0xFu);
                const double   f    = kGroupBase[size_t(g)] + double(tone) * spacing;
                dphi[size_t(g)] = 2.0 * std::numbers::pi * f / cfg_.sample_rate;
            }
            for (int s = 0; s < sps; ++s) {
                double acc = 0.0;
                for (int g = 0; g < kGroups; ++g) {
                    acc += std::sin(phase_[size_t(g)]);
                    phase_[size_t(g)] += dphi[size_t(g)]; // spojitá fáze přes symboly
                    if (phase_[size_t(g)] > 2.0 * std::numbers::pi)
                        phase_[size_t(g)] -= 2.0 * std::numbers::pi;
                }
                out.push_back(float(amp * acc));
            }
        }
    }

private:
    ModemConfig cfg_;
    std::array<double, kGroups> phase_{};
};

class QfskDemodulator final : public IDemodulator {
public:
    void configure(const ModemConfig& c) override {
        const double spacing = c.baud;
        gs_.clear();
        gs_.reserve(size_t(kGroups) * kTonesPerGroup);
        for (int g = 0; g < kGroups; ++g)
            for (int k = 0; k < kTonesPerGroup; ++k)
                gs_.emplace_back(kGroupBase[size_t(g)] + k * spacing, c.sample_rate,
                                 c.samplesPerSymbol());
    }
    int  bitsPerSymbol() const override { return kBitsPerSymbol; }
    void reset() override {}

    uint32_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) override {
        std::vector<float> energies(gs_.size());
        uint32_t value   = 0;
        float    snr_sum = 0.f;
        for (int g = 0; g < kGroups; ++g) {
            float best = -1.f, second = 0.f;
            int   best_k = 0;
            for (int k = 0; k < kTonesPerGroup; ++k) {
                const size_t idx = size_t(g) * kTonesPerGroup + size_t(k);
                const float  e   = Goertzel::energy(gs_[idx].run(sym));
                energies[idx] = e;
                if (e > best) { second = best; best = e; best_k = k; }
                else if (e > second) { second = e; }
            }
            const uint32_t nib = grayDecode(uint32_t(best_k)); // index tónu → 4 bity
            value |= nib << (g * kBitsPerGroup);
            snr_sum += 10.f * std::log10((best + 1e-12f) / (second + 1e-12f));
        }
        if (diag) {
            diag->tone_energy = std::move(energies); // 4×16 sloupců pro GUI
            diag->phasor      = {};
            diag->snr_db      = snr_sum / float(kGroups); // průměr rezerv skupin
        }
        return value;
    }

private:
    std::vector<Goertzel> gs_;
};

} // namespace

std::unique_ptr<IModulator>   makeQfskMod()   { return std::make_unique<QfskModulator>(); }
std::unique_ptr<IDemodulator> makeQfskDemod() { return std::make_unique<QfskDemodulator>(); }

} // namespace am
