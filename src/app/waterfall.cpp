// STFT vodopád nad kiss_fftr: Hannovo okno 1024, hop 512, výstup v dB.

#include "app/waterfall.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "kissfft/kiss_fftr.h"

namespace am {

struct Waterfall::Impl {
    kiss_fftr_cfg cfg = nullptr;
    std::vector<kiss_fft_scalar> in;
    std::vector<kiss_fft_cpx> out;
};

void Waterfall::configure(int sample_rate, int fft_size, int hop,
                          double display_max_hz, int rows) {
    std::lock_guard lock(mtx_);
    sample_rate_ = sample_rate;
    fft_size_ = fft_size;
    hop_ = hop;
    rows_ = rows;
    display_max_hz_ = display_max_hz;
    bins_ = std::min(fft_size / 2 + 1,
                     int(display_max_hz * fft_size / sample_rate) + 1);

    window_.resize(size_t(fft_size));
    for (int i = 0; i < fft_size; ++i)
        window_[size_t(i)] = float(
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (fft_size - 1)));

    acc_.clear();
    matrix_.assign(size_t(rows_) * size_t(bins_), -100.f);
    version_ = 0;
    last_read_version_ = 0;

    if (impl_) {
        kiss_fftr_free(impl_->cfg);
        delete impl_;
    }
    impl_ = new Impl;
    impl_->cfg = kiss_fftr_alloc(fft_size, 0, nullptr, nullptr);
    impl_->in.resize(size_t(fft_size));
    impl_->out.resize(size_t(fft_size / 2 + 1));
}

Waterfall::~Waterfall() {
    if (impl_) {
        kiss_fftr_free(impl_->cfg);
        delete impl_;
    }
}

void Waterfall::pushSamples(std::span<const float> x) {
    std::lock_guard lock(mtx_);
    if (!impl_) return;
    acc_.insert(acc_.end(), x.begin(), x.end());
    while (int(acc_.size()) >= fft_size_) {
        computeRow();
        acc_.erase(acc_.begin(), acc_.begin() + hop_);
    }
}

void Waterfall::computeRow() {
    for (int i = 0; i < fft_size_; ++i)
        impl_->in[size_t(i)] = acc_[size_t(i)] * window_[size_t(i)];
    kiss_fftr(impl_->cfg, impl_->in.data(), impl_->out.data());

    // posuň matici o řádek nahoru (nejstarší řádek zmizí)
    std::memmove(matrix_.data(), matrix_.data() + bins_,
                 (size_t(rows_) - 1) * size_t(bins_) * sizeof(float));
    float* row = matrix_.data() + (size_t(rows_) - 1) * size_t(bins_);
    const float norm = 2.f / float(fft_size_); // amplituda vůči oknu
    for (int b = 0; b < bins_; ++b) {
        const auto& c = impl_->out[size_t(b)];
        const float mag = std::sqrt(c.r * c.r + c.i * c.i) * norm;
        row[b] = 20.f * std::log10(mag + 1e-6f); // dBFS, podlaha -120 dB
    }
    ++version_;
}

bool Waterfall::snapshot(std::vector<float>& out, int& rows, int& bins) {
    std::lock_guard lock(mtx_);
    rows = rows_;
    bins = bins_;
    if (version_ == last_read_version_) return false;
    last_read_version_ = version_;
    out = matrix_;
    return true;
}

double Waterfall::rowsPerSecond() const {
    return double(sample_rate_) / double(hop_);
}

} // namespace am
