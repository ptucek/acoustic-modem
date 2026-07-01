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

// Speciální hodnota pro capture_index/playback_index u AudioEngine::start():
// daný směr se vůbec neotevře (žádné zařízení, žádné oprávnění OS). Použití:
// čisté vysílání (kNoDevice jako capture — na macOS nevyžádá mikrofon/TCC)
// nebo čistý příjem (kNoDevice jako playback). Duplex nastane, jen když jsou
// OBĚ hodnoty >= -1 (tj. -1 = výchozí zařízení, nebo konkrétní index >= 0).
inline constexpr int kNoDevice = -2;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Seznam zařízení pro GUI/CLI (playback i capture).
    std::vector<DeviceInfo> enumerate();

    // Spustí zvukový proud: mikrofon → rx_ring, tx_ring → reproduktor (mono
    // float32, sample_rate). Když je tx_ring prázdný, hraje ticho.
    // Index -1 = výchozí zařízení daného směru, index >= 0 = konkrétní
    // zařízení (musí existovat v enumerate(), jinak start() vrátí false).
    // kNoDevice = daný směr se vůbec neotevře (např. jen playback pro
    // send, jen capture pro listen) — plný duplex nastane jen tehdy, když
    // capture_index i playback_index jsou oba != kNoDevice.
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
