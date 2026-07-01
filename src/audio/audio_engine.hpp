#pragma once
// Duplexní zvukové rozhraní nad miniaudio. Real-time callback pouze
// kopíruje vzorky mezi zařízením a lock-free ring buffery — žádné zámky,
// žádné alokace, žádné DSP (to patří do DSP vlákna).

#include <string>
#include <vector>

#include "core/spsc_ring.hpp"

namespace am {

struct DeviceInfo {
    std::string name;
    bool is_capture = false;
    int  index = -1; // index v rámci enumerate() daného směru
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Seznam zařízení pro GUI/CLI (playback i capture).
    std::vector<DeviceInfo> enumerate();

    // Spustí duplexní proud: mikrofon → rx_ring, tx_ring → reproduktor
    // (mono float32, sample_rate). Když je tx_ring prázdný, hraje ticho.
    // Indexy -1 = výchozí zařízení. Ringy musí přežít až do stop().
    bool start(int capture_index, int playback_index, int sample_rate,
               SpscRing<float>& rx_ring, SpscRing<float>& tx_ring);
    void stop();
    bool running() const { return running_; }

    // Špičková úroveň vstupu od posledního čtení (pro level metr), 0..1.
    float inputPeak();

private:
    struct Impl; // skrývá miniaudio typy mimo hlavičku
    Impl* impl_ = nullptr;
    bool running_ = false;
};

} // namespace am
