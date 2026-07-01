// Implementace AudioEngine nad miniaudio. Tato translační jednotka je
// jediná, která definuje MINIAUDIO_IMPLEMENTATION — vygeneruje se sem celé
// tělo knihovny (miniaudio.h je jinak jen deklarace).
//
// Datový callback běží v real-time audio vlákně: žádné zámky, žádné
// alokace, žádné DSP. Jen kopírování mezi zvukovým zařízením a lock-free
// SpscRing buffery (viz core/spsc_ring.hpp).

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio/audio_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace am {

struct AudioEngine::Impl {
    ma_context context{};
    ma_device  device{};
    bool context_initialized = false;
    bool device_initialized  = false;

    SpscRing<float>* rx_ring = nullptr; // mikrofon -> rx_ring (producer v callbacku)
    SpscRing<float>* tx_ring = nullptr; // tx_ring -> reproduktor (consumer v callbacku)

    std::atomic<float> input_peak{0.f};

    // ID zařízení z posledního enumerate() volání — indexovaná stejně jako
    // DeviceInfo::index, aby start() mohl vybrat konkrétní zařízení.
    std::vector<ma_device_id> playback_ids;
    std::vector<ma_device_id> capture_ids;

    // Datový callback volaný miniaudio z real-time audio vlákna. Statická
    // členská funkce Impl má přirozený přístup ke svým vlastním datům, aniž
    // by bylo potřeba zpřístupňovat privátní typ Impl mimo AudioEngine.
    static void dataCallback(ma_device* device, void* output, const void* input,
                              ma_uint32 frame_count) {
        auto* impl = static_cast<Impl*>(device->pUserData);
        float* out = static_cast<float*>(output);
        const float* in = static_cast<const float*>(input);

        // TX: vytáhni vzorky z tx_ring do výstupu; zbytek dorovnej tichem.
        if (out != nullptr) {
            size_t got = 0;
            if (impl->tx_ring != nullptr) {
                got = impl->tx_ring->pop(std::span<float>(out, frame_count));
            }
            if (got < frame_count) {
                std::memset(out + got, 0, (frame_count - got) * sizeof(float));
            }
        }

        // RX: zapiš vstup do rx_ring; při přetečení vzorky zahodíme (nikdy
        // neblokujeme audio vlákno).
        if (in != nullptr) {
            if (impl->rx_ring != nullptr) {
                impl->rx_ring->push(std::span<const float>(in, frame_count));
            }

            float peak = impl->input_peak.load(std::memory_order_relaxed);
            for (ma_uint32 i = 0; i < frame_count; ++i) {
                const float a = std::fabs(in[i]);
                if (a > peak) peak = a;
            }
            impl->input_peak.store(peak, std::memory_order_relaxed);
        }
    }
};

AudioEngine::AudioEngine() : impl_(new Impl()) {}

AudioEngine::~AudioEngine() {
    stop();
    delete impl_;
}

std::vector<DeviceInfo> AudioEngine::enumerate() {
    std::vector<DeviceInfo> result;

    bool own_context = false;
    if (!impl_->context_initialized) {
        if (ma_context_init(nullptr, 0, nullptr, &impl_->context) != MA_SUCCESS) {
            return result;
        }
        impl_->context_initialized = true;
        own_context = true;
    }
    (void)own_context;

    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&impl_->context, &playback_infos, &playback_count,
                                &capture_infos, &capture_count) != MA_SUCCESS) {
        return result;
    }

    impl_->playback_ids.clear();
    impl_->capture_ids.clear();

    for (ma_uint32 i = 0; i < playback_count; ++i) {
        DeviceInfo di;
        di.name = playback_infos[i].name;
        di.is_capture = false;
        di.index = static_cast<int>(i);
        result.push_back(std::move(di));
        impl_->playback_ids.push_back(playback_infos[i].id);
    }

    for (ma_uint32 i = 0; i < capture_count; ++i) {
        DeviceInfo di;
        di.name = capture_infos[i].name;
        di.is_capture = true;
        di.index = static_cast<int>(i);
        result.push_back(std::move(di));
        impl_->capture_ids.push_back(capture_infos[i].id);
    }

    return result;
}

bool AudioEngine::start(int capture_index, int playback_index, int sample_rate,
                         SpscRing<float>& rx_ring, SpscRing<float>& tx_ring) {
    stop(); // pro jistotu — bezpečné zavolat i na nespuštěném enginu

    const bool want_capture = capture_index != kNoDevice;
    const bool want_playback = playback_index != kNoDevice;
    if (!want_capture && !want_playback) {
        return false; // nemá smysl otevírat proud bez obou směrů
    }

    if (!impl_->context_initialized) {
        if (ma_context_init(nullptr, 0, nullptr, &impl_->context) != MA_SUCCESS) {
            return false;
        }
        impl_->context_initialized = true;
    }

    // Potřebujeme seznam ID zařízení, pokud volající chce konkrétní index.
    if ((capture_index >= 0 || playback_index >= 0) &&
        impl_->playback_ids.empty() && impl_->capture_ids.empty()) {
        enumerate();
    }

    // Index mimo rozsah je chyba volajícího (typicky --device z CLI) —
    // dřív se tiše spadlo na výchozí zařízení, což vede k matoucímu chování
    // (nahraje/přehraje se jiné zařízení, než uživatel čekal).
    if (playback_index >= 0 && static_cast<size_t>(playback_index) >= impl_->playback_ids.size()) {
        return false;
    }
    if (capture_index >= 0 && static_cast<size_t>(capture_index) >= impl_->capture_ids.size()) {
        return false;
    }

    impl_->rx_ring = want_capture ? &rx_ring : nullptr;
    impl_->tx_ring = want_playback ? &tx_ring : nullptr;
    impl_->input_peak.store(0.f, std::memory_order_relaxed);

    const ma_device_type dev_type = (want_capture && want_playback) ? ma_device_type_duplex
                                     : want_playback                ? ma_device_type_playback
                                                                     : ma_device_type_capture;

    ma_device_config config = ma_device_config_init(dev_type);
    config.sampleRate = static_cast<ma_uint32>(sample_rate);
    config.capture.format   = ma_format_f32;
    config.capture.channels = 1;
    config.playback.format   = ma_format_f32;
    config.playback.channels = 1;
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = impl_;

    if (want_playback && playback_index >= 0) {
        config.playback.pDeviceID = &impl_->playback_ids[static_cast<size_t>(playback_index)];
    }
    if (want_capture && capture_index >= 0) {
        config.capture.pDeviceID = &impl_->capture_ids[static_cast<size_t>(capture_index)];
    }

    if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
        impl_->rx_ring = nullptr;
        impl_->tx_ring = nullptr;
        return false;
    }
    impl_->device_initialized = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
        impl_->rx_ring = nullptr;
        impl_->tx_ring = nullptr;
        return false;
    }

    running_ = true;
    return true;
}

void AudioEngine::stop() {
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
    }
    if (impl_->context_initialized) {
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
    }
    impl_->rx_ring = nullptr;
    impl_->tx_ring = nullptr;
    running_ = false;
}

float AudioEngine::inputPeak() {
    return impl_->input_peak.exchange(0.f, std::memory_order_relaxed);
}

} // namespace am
