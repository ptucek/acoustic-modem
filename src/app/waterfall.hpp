#pragma once
// Vodopádový (spektrogramový) model: STFT přijímaných vzorků → posuvná
// matice řádků v dB pro ImPlot::PlotHeatmap. Zápis z DSP vlákna, čtení
// z GUI vlákna. Ochrana mutexem — DSP vlákno není real-time kritické
// (to je jen audio callback), kopie ~100 KB při 60 fps je zanedbatelná.

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace am {

class Waterfall {
public:
    // fft_size=1024, hop=512 → ~93 řádků/s při 48 kHz; zobrazuje se
    // 0..display_max_hz (5 kHz pokryje celé používané pásmo).
    void configure(int sample_rate, int fft_size = 1024, int hop = 512,
                   double display_max_hz = 5000.0, int rows = 256);
    ~Waterfall();

    // DSP vlákno: přidej vzorky; každých `hop` vzorků vznikne nový řádek.
    void pushSamples(std::span<const float> x);

    // GUI vlákno: zkopíruje aktuální matici (rows × bins, dB, nejnovější
    // řádek poslední). Vrací false, když se od minulého čtení nic nezměnilo.
    bool snapshot(std::vector<float>& out, int& rows, int& bins);

    double maxHz() const { return display_max_hz_; }
    double rowsPerSecond() const;

private:
    void computeRow(); // volat s drženým mutexem

    int sample_rate_ = 48000, fft_size_ = 1024, hop_ = 512, rows_ = 256;
    int bins_ = 0; // počet zobrazených binů (<= fft_size/2+1)
    double display_max_hz_ = 5000.0;

    std::vector<float> window_; // Hann
    std::vector<float> acc_;    // akumulátor vzorků do dalšího okna
    struct Impl;                // kiss_fftr stav (mimo hlavičku)
    Impl* impl_ = nullptr;

    std::mutex mtx_;
    std::vector<float> matrix_; // rows_ * bins_, kruhově posouvaná
    uint64_t version_ = 0;      // roste s každým novým řádkem
    uint64_t last_read_version_ = 0;
};

} // namespace am
