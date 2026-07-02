// Q-FSK: 4 paralelní 16-FSK skupiny, 16 b/symbol, 1 kbit/s @ 62,5 Bd.
// Kromě řetězových testů (které Q-FSK dostávají automaticky přes registr
// v test_chain.cpp) hlídáme tady: 16 b/symbol, propustnost a zejména
// framing — hlavička (40 b) končí uprostřed symbolu, takže bez zobecnění
// FrameReceiveru by se ztrácel první bajt payloadu.

#include <algorithm>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "doctest/doctest.h"
#include "dsp/channel_sim.hpp"
#include "modem/modulator.hpp"
#include "protocol/framer.hpp"
#include "protocol/frame_receiver.hpp"

namespace {

am::ModemConfig qfskCfg() {
    am::ModemConfig c;
    c.baud = 62.5; // návrhový pracovní bod → 1 kbit/s
    return c;
}

std::vector<uint8_t> randomPayload(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> p(n);
    for (auto& b : p) b = uint8_t(rng());
    return p;
}

std::optional<am::FrameReceiver::Result>
runChain(std::span<const uint8_t> payload, const am::ChannelParams& ch,
         const am::ModemConfig& cfg) {
    const auto& scheme = *am::findScheme("Q-FSK");
    auto mod = scheme.makeMod();
    mod->configure(cfg);
    auto tx = am::Framer::buildFrame(payload, *mod, cfg);

    std::vector<float> line(size_t(0.3 * cfg.sample_rate), 0.f);
    line.insert(line.end(), tx.begin(), tx.end());
    line.insert(line.end(), size_t(0.2 * cfg.sample_rate), 0.f);

    auto rx_samples = am::simulateChannel(line, ch, cfg.sample_rate);

    am::FrameReceiver rx;
    rx.configure(cfg, scheme);
    for (size_t i = 0; i < rx_samples.size(); i += 4096) {
        const size_t n = std::min<size_t>(4096, rx_samples.size() - i);
        rx.pushSamples({rx_samples.data() + i, n});
    }
    return rx.poll();
}

} // namespace

TEST_CASE("Q-FSK: 16 bitů/symbol (4 skupiny × 4 bity)") {
    const auto& scheme = *am::findScheme("Q-FSK");
    const am::ModemConfig c = qfskCfg();
    auto mod = scheme.makeMod();
    mod->configure(c);
    CHECK(mod->bitsPerSymbol() == 16);
    auto demod = scheme.makeDemod();
    demod->configure(c);
    CHECK(demod->bitsPerSymbol() == 16);
}

TEST_CASE("Q-FSK: propustnost 1 kbit/s @ 62,5 Bd") {
    const am::ModemConfig c = qfskCfg();
    auto mod = am::findScheme("Q-FSK")->makeMod();
    mod->configure(c);
    CHECK(mod->bitsPerSymbol() * c.baud == doctest::Approx(1000.0));
}

TEST_CASE("Q-FSK: čistý round-trip, bit-přesný payload") {
    const auto payload = randomPayload(64, 123);
    auto r = runChain(payload, {}, qfskCfg());
    REQUIRE(r.has_value());
    CHECK(r->crc_ok);
    CHECK(r->payload == payload);
}

TEST_CASE("Q-FSK: maximální payload beze ztráty (framing bps=16)") {
    // Regrese na zarovnání polí rámce: hlavička 40 b = 2,5 symbolu, tj.
    // končí uprostřed symbolu. Dřív FrameReceiver zahazoval celý buffer
    // mezi poli a ztratil tak první bajt payloadu.
    const auto payload = randomPayload(size_t(am::kMaxPayload), 5);
    auto r = runChain(payload, {}, qfskCfg());
    REQUIRE(r.has_value());
    CHECK(r->crc_ok);
    CHECK(r->payload == payload);
}

TEST_CASE("Q-FSK: přežije šum a drift hodin") {
    const auto payload = randomPayload(64, 77);
    for (double snr : {30.0, 20.0}) {
        for (double drift : {0.0, 50.0, -50.0}) {
            CAPTURE(snr);
            CAPTURE(drift);
            am::ChannelParams ch;
            ch.snr_db    = snr;
            ch.drift_ppm = drift;
            ch.seed      = uint32_t(2000 + snr * 10 + drift);
            auto r = runChain(payload, ch, qfskCfg());
            REQUIRE(r.has_value());
            CHECK(r->crc_ok);
            CHECK(r->payload == payload);
        }
    }
}
