// Řetězové testy: payload → Framer → simulovaný kanál → FrameReceiver.
// Regresní síť pro veškeré pozdější ladění DSP: každé schéma v registru
// musí projít při kombinacích SNR × drift hodin.

#include <random>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "dsp/channel_sim.hpp"
#include "modem/modulator.hpp"
#include "protocol/framer.hpp"
#include "protocol/frame_receiver.hpp"

namespace {

std::vector<uint8_t> randomPayload(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> p(n);
    for (auto& b : p) b = uint8_t(rng());
    return p;
}

// pošle rámec kanálem a vrátí první dekódovaný výsledek
std::optional<am::FrameReceiver::Result>
runChain(const am::ModemScheme& scheme, std::span<const uint8_t> payload,
         const am::ChannelParams& ch, const am::ModemConfig& cfg = {}) {
    auto mod = scheme.makeMod();
    mod->configure(cfg);
    auto tx = am::Framer::buildFrame(payload, *mod, cfg);

    // ticho před i za rámcem — přijímač musí umět čekat
    std::vector<float> line(size_t(0.3 * cfg.sample_rate), 0.f);
    line.insert(line.end(), tx.begin(), tx.end());
    line.insert(line.end(), size_t(0.2 * cfg.sample_rate), 0.f);

    auto rx_samples = am::simulateChannel(line, ch, cfg.sample_rate);

    am::FrameReceiver rx;
    rx.configure(cfg, scheme);
    // streamování po kouscích jako z ring bufferu
    for (size_t i = 0; i < rx_samples.size(); i += 4096) {
        const size_t n = std::min<size_t>(4096, rx_samples.size() - i);
        rx.pushSamples({rx_samples.data() + i, n});
    }
    return rx.poll();
}

} // namespace

TEST_CASE("řetězec: každé schéma přežije kombinace SNR × drift") {
    const auto payload = randomPayload(64, 42);
    for (const auto& scheme : am::modemRegistry()) {
        CAPTURE(scheme.name);
        for (double snr : {30.0, 15.0, 8.0}) {
            for (double drift : {0.0, 100.0, -100.0}) {
                CAPTURE(snr);
                CAPTURE(drift);
                am::ChannelParams ch;
                ch.snr_db = snr;
                ch.drift_ppm = drift;
                ch.seed = uint32_t(1000 + snr * 10 + drift);
                auto r = runChain(scheme, payload, ch);
                REQUIRE(r.has_value());
                CHECK(r->crc_ok);
                CHECK(r->payload == payload);
            }
        }
    }
}

TEST_CASE("řetězec: bez šumu je BER nulová i pro maximální payload") {
    const auto payload = randomPayload(size_t(am::kMaxPayload), 7);
    for (const auto& scheme : am::modemRegistry()) {
        CAPTURE(scheme.name);
        auto r = runChain(scheme, payload, {});
        REQUIRE(r.has_value());
        CHECK(r->crc_ok);
        CHECK(r->payload == payload);
    }
}

TEST_CASE("řetězec: útlum a echo (hrubý dozvuk)") {
    const auto payload = randomPayload(32, 99);
    am::ChannelParams ch;
    ch.gain = 0.2;          // slabý signál
    ch.snr_db = 20.0;
    ch.echo_delay_s = 0.010; // 10 ms odraz
    ch.echo_gain = 0.3;
    for (const auto& scheme : am::modemRegistry()) {
        CAPTURE(scheme.name);
        auto r = runChain(scheme, payload, ch);
        REQUIRE(r.has_value());
        CHECK(r->crc_ok);
    }
}

TEST_CASE("řetězec: UTF-8 text projde beze změny") {
    const std::string text = "Příliš žluťoučký kůň úpěl ďábelské ódy";
    std::vector<uint8_t> payload(text.begin(), text.end());
    const auto& scheme = *am::findScheme("2-FSK");
    am::ChannelParams ch;
    ch.snr_db = 12.0;
    auto r = runChain(scheme, payload, ch);
    REQUIRE(r.has_value());
    CHECK(r->crc_ok);
    CHECK(std::string(r->payload.begin(), r->payload.end()) == text);
}
