// N paralelních 16-FSK skupin → 4·N bitů/symbol → N-násobek propustnosti
// 16-FSK (250 b/s na skupinu @ 62,5 Bd). Skupiny leží v pásmech změřených
// sondáží kanálu (docs/measurements.md), vyhýbají se dozvukovým zářezům:
//
//   Q-FSK (Quad, 4 skupiny) — obousměrně ověřená pásma Fedora↔Mac:
//     G1 1050 / G2 2800 / G3 4300 / G4 6300 Hz → 16 b/symbol → 1 kbit/s.
//   W-FSK (Wideband, 11 skupin) — Q-FSK + G5–G11 nad 7240 Hz z VF sondáže
//     macu (sync/results/hf-sounding-mac.md, čistě až do 16 kHz):
//     G5 7500 / G6 8750 / G7 10000 / G8 11250 / G9 12500 / G10 13750 /
//     G11 15000 Hz → 44 b/symbol → 2,75 kbit/s @ 62,5 Bd.
//     POZOR: horní pásma ověřena jen self-loopbackem na macu; Fedora→Mac
//     klesá nad ~5,5 kHz → W-FSK může být na dvoustrojovém spoji asymetrická.
//
// Každá skupina nese 4 bity — index tónu Grayovsky kódovaný jako u 16-FSK,
// takže záměna za sousední tón poškodí jen jeden bit. Modulátor sečte N
// současných tónů; každý má amplitudu cfg.amplitude/N, takže součet nikdy
// nepřekročí cfg.amplitude (žádné klipování). Demodulátor běží Goertzel na
// všech N×16 tónech a v každé skupině vezme argmax nezávisle.
//
// Rozteč tónů = baud → přes okno symbolu (samplesPerSymbol vzorků) leží
// sousední tóny na sousedních DFT binech → ortogonální (nekoherentní
// detekce energie). Absolutní poloha binů nemusí být celočíselná;
// ortogonalitu drží celočíselný ODSTUP, ne absolutní poloha.

#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "dsp/goertzel.hpp"
#include "modem/schemes.hpp"

namespace am {
namespace {

constexpr int kTonesPerGroup = 16;
constexpr int kBitsPerGroup  = 4;   // log2(16)
constexpr int kMaxGroups     = 16;  // strop (64 b/symbol se vejde do uint64_t)

// Základny skupin (Hz) — změřená průchozí pásma bez dozvukových zářezů.
// Q-FSK = jen G1–G4 (obousměrně ověřené). W-FSK = G1–G11 (G5+ z VF sondáže).
constexpr std::array<double, 4> kQfskBase = {1050.0, 2800.0, 4300.0, 6300.0};
constexpr std::array<double, 11> kWfskBase = {
    1050.0,  2800.0,  4300.0,  6300.0,           // G1–G4 (jako Q-FSK)
    7500.0,  8750.0, 10000.0, 11250.0,           // G5–G8
    12500.0, 13750.0, 15000.0,                   // G9–G11
};

class NfskModulator final : public IModulator {
public:
    explicit NfskModulator(std::span<const double> bases) : bases_(bases) {}

    void configure(const ModemConfig& c) override {
        cfg_ = c;
        reset();
    }
    int  bitsPerSymbol() const override { return int(bases_.size()) * kBitsPerGroup; }
    void reset() override { phase_.fill(0.0); }

    void modulate(const BitBuffer& bits, std::vector<float>& out) override {
        const int    groups  = int(bases_.size());
        const int    bps     = groups * kBitsPerGroup;
        const int    sps     = cfg_.samplesPerSymbol();
        const size_t n_sym   = bits.size() / size_t(bps);
        const double amp     = cfg_.amplitude / double(groups); // součet ≤ amplitude
        const double spacing = cfg_.baud;                       // rozteč = 1 DFT bin
        out.reserve(out.size() + n_sym * size_t(sps));

        for (size_t i = 0; i < n_sym; ++i) {
            const uint64_t sym = bits.symbol(i, bps);
            std::array<double, kMaxGroups> dphi{};
            for (int g = 0; g < groups; ++g) {
                const uint32_t tone =
                    grayEncode(uint32_t((sym >> (g * kBitsPerGroup)) & 0xFu));
                const double f = bases_[size_t(g)] + double(tone) * spacing;
                dphi[size_t(g)] = 2.0 * std::numbers::pi * f / cfg_.sample_rate;
            }
            for (int s = 0; s < sps; ++s) {
                double acc = 0.0;
                for (int g = 0; g < groups; ++g) {
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
    std::span<const double>        bases_;
    ModemConfig                    cfg_;
    std::array<double, kMaxGroups> phase_{};
};

class NfskDemodulator final : public IDemodulator {
public:
    explicit NfskDemodulator(std::span<const double> bases) : bases_(bases) {}

    void configure(const ModemConfig& c) override {
        const double spacing = c.baud;
        gs_.clear();
        gs_.reserve(bases_.size() * kTonesPerGroup);
        for (double base : bases_)
            for (int k = 0; k < kTonesPerGroup; ++k)
                gs_.emplace_back(base + k * spacing, c.sample_rate,
                                 c.samplesPerSymbol());
    }
    int  bitsPerSymbol() const override { return int(bases_.size()) * kBitsPerGroup; }
    void reset() override {}

    uint64_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) override {
        const int          groups = int(bases_.size());
        std::vector<float> energies(gs_.size());
        uint64_t           value   = 0;
        float              snr_sum = 0.f;
        for (int g = 0; g < groups; ++g) {
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
            value |= uint64_t(nib) << (g * kBitsPerGroup);
            snr_sum += 10.f * std::log10((best + 1e-12f) / (second + 1e-12f));
        }
        if (diag) {
            diag->tone_energy = std::move(energies); // N×16 sloupců pro GUI
            diag->phasor      = {};
            diag->snr_db      = snr_sum / float(groups); // průměr rezerv skupin
        }
        return value;
    }

private:
    std::span<const double> bases_;
    std::vector<Goertzel>   gs_;
};

} // namespace

std::unique_ptr<IModulator>   makeQfskMod()   { return std::make_unique<NfskModulator>(kQfskBase); }
std::unique_ptr<IDemodulator> makeQfskDemod() { return std::make_unique<NfskDemodulator>(kQfskBase); }

std::unique_ptr<IModulator>   makeWfskMod()   { return std::make_unique<NfskModulator>(kWfskBase); }
std::unique_ptr<IDemodulator> makeWfskDemod() { return std::make_unique<NfskDemodulator>(kWfskBase); }

} // namespace am
