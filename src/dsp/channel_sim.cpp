// Simulátor akustického kanálu pro offline testy: zisk → drift hodin
// (převzorkování) → echo → AWGN. Deterministický díky pevnému seedu.

#include "dsp/channel_sim.hpp"

#include <cmath>
#include <random>

namespace am {

std::vector<float> simulateChannel(std::span<const float> x, const ChannelParams& p,
                                   int /*sample_rate*/) {
    std::vector<float> y(x.begin(), x.end());

    for (float& v : y) v = float(v * p.gain);

    // Drift hodin: přijímač vzorkuje na (1 + ppm*1e-6) násobku frekvence
    // vysílače → lineární interpolace na novém rastru.
    if (p.drift_ppm != 0.0) {
        const double ratio = 1.0 + p.drift_ppm * 1e-6;
        std::vector<float> r;
        r.reserve(y.size());
        for (double pos = 0.0; pos + 1.0 < double(y.size()); pos += ratio) {
            const size_t i = size_t(pos);
            const double frac = pos - double(i);
            r.push_back(float(y[i] * (1.0 - frac) + y[i + 1] * frac));
        }
        y = std::move(r);
    }

    // Jednoodbočkové echo — hrubá aproximace dozvuku místnosti
    if (p.echo_gain != 0.0 && p.echo_delay_s > 0.0) {
        const size_t d = size_t(p.echo_delay_s * 48000.0 + 0.5);
        for (size_t i = d; i < y.size(); ++i)
            y[i] += float(p.echo_gain) * y[i - d];
    }

    // AWGN vztažený k RMS signálu: sigma = rms / 10^(SNR/20)
    if (p.snr_db < 99.0) {
        double e = 0.0;
        for (float v : y) e += double(v) * v;
        const double rms = std::sqrt(e / double(y.empty() ? 1 : y.size()));
        const double sigma = rms / std::pow(10.0, p.snr_db / 20.0);
        std::mt19937 rng(p.seed);
        std::normal_distribution<double> noise(0.0, sigma);
        for (float& v : y) v = float(v + noise(rng));
    }

    return y;
}

} // namespace am
