// Generátor chirpu a streamovací korelátor pro synchronizaci rámce.
//
// Proč chirp: jeho autokorelace má jedinou ostrou špičku, zatímco střídavé
// tóny korelují každou periodu. Navíc chirp přejede celé pásmo 800–2800 Hz,
// takže synchronizace přežije i frekvenčně selektivní odezvu místnosti.
//
// CPU trik: signál i šablona se filtrují stejnou dolní propustí a decimují
// ×4 (48 → 12 kHz). Protože jsou obě strany zpracované identicky, skupinová
// zpoždění filtru se v poloze korelační špičky vyruší. Hrubá špička se pak
// zpřesní ±8 vzorků na plných 48 kHz.

#include "dsp/chirp.hpp"

#include <cmath>
#include <numbers>

#include "dsp/fir.hpp"

namespace am {

std::vector<float> makeChirp(const PreambleSpec& pre, int sample_rate,
                             double amplitude) {
    const int n = int(pre.chirp_s * sample_rate + 0.5);
    std::vector<float> out(static_cast<size_t>(n));
    const double T = pre.chirp_s;
    const double k = (pre.chirp_f_end - pre.chirp_f_start) / T; // Hz/s
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / sample_rate;
        // okamžitá fáze lineárního chirpu: 2π (f0 t + k t²/2)
        const double phase =
            2.0 * std::numbers::pi * (pre.chirp_f_start * t + 0.5 * k * t * t);
        // Hannova obálka: bez lupanců na krajích, čistší spektrum; šablona
        // v korelátoru je identická, takže tvar obálky detekci nevadí
        const double env =
            n > 1 ? 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (n - 1))
                  : 1.0;
        out[size_t(i)] = float(amplitude * env * std::sin(phase));
    }
    return out;
}

namespace {

// normalizovaná korelace šablony `t` se signálem `x` od pozice `at`
double normCorrAt(std::span<const float> x, size_t at, std::span<const float> t,
                  double t_norm) {
    double dot = 0.0, e = 0.0;
    for (size_t j = 0; j < t.size(); ++j) {
        const double v = x[at + j];
        dot += v * double(t[j]);
        e += v * v;
    }
    const double denom = std::sqrt(e) * t_norm;
    return denom > 1e-12 ? dot / denom : 0.0;
}

double vecNorm(std::span<const float> v) {
    double e = 0.0;
    for (float s : v) e += double(s) * s;
    return std::sqrt(e);
}

} // namespace

void ChirpCorrelator::configure(const PreambleSpec& pre, int sample_rate) {
    sample_rate_ = sample_rate;
    template_48k_ = makeChirp(pre, sample_rate, 1.0);
    gap_len_ = int(pre.gap_s * sample_rate + 0.5);
    // mezní frekvence nad koncem chirpu, bezpečně pod Nyquistem po decimaci
    lp_taps_ = designLowpass(std::min(pre.chirp_f_end * 1.15,
                                      0.45 * sample_rate / decim_),
                             sample_rate, 63);
    template_dec_ = filterDecimate(template_48k_, lp_taps_, decim_);
}

long ChirpCorrelator::search(std::span<const float> x, size_t from,
                             float threshold, float* peak_out) const {
    if (peak_out) *peak_out = 0.f;
    if (from >= x.size()) return -1;
    const std::span<const float> tail = x.subspan(from);
    if (tail.size() < template_48k_.size() + lp_taps_.size()) return -1;

    // 1) hrubé hledání v decimované doméně
    const std::vector<float> xd = filterDecimate(tail, lp_taps_, decim_);
    if (xd.size() < template_dec_.size()) return -1;
    const double t_norm_dec = vecNorm(template_dec_);

    double best = 0.0;
    size_t best_k = 0;
    for (size_t k = 0; k + template_dec_.size() <= xd.size(); ++k) {
        const double c = normCorrAt(xd, k, template_dec_, t_norm_dec);
        if (c > best) {
            best = c;
            best_k = k;
        }
    }
    if (peak_out) *peak_out = float(best);
    if (best < threshold) return -1;

    // 2) zpřesnění špičky na plných 48 kHz (±2*decim vzorků kolem hrubé polohy)
    const double t_norm_full = vecNorm(template_48k_);
    const long coarse = long(from + best_k * size_t(decim_));
    double best_full = -1.0;
    long best_pos = coarse;
    for (long p = coarse - 2 * decim_; p <= coarse + 2 * decim_; ++p) {
        if (p < 0 || size_t(p) + template_48k_.size() > x.size()) continue;
        const double c = normCorrAt(x, size_t(p), template_48k_, t_norm_full);
        if (c > best_full) {
            best_full = c;
            best_pos = p;
        }
    }
    // hranice prvního symbolu = začátek chirpu + délka chirpu + mezera
    return best_pos + long(template_48k_.size()) + gap_len_;
}

} // namespace am
