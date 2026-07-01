// Testy odolnosti přijímače: uříznutý rámec, poškozená hlavička, dva rámce
// za sebou, falešný chirp — přijímač se vždy musí čistě zotavit.

#include <vector>

#include "doctest/doctest.h"
#include "dsp/channel_sim.hpp"
#include "modem/modulator.hpp"
#include "protocol/framer.hpp"
#include "protocol/frame_receiver.hpp"

namespace {

const am::ModemScheme& fsk() { return *am::findScheme("2-FSK"); }

std::vector<float> buildTx(std::span<const uint8_t> payload,
                           const am::ModemConfig& cfg = {}) {
    auto mod = fsk().makeMod();
    mod->configure(cfg);
    return am::Framer::buildFrame(payload, *mod, cfg);
}

void pushChunked(am::FrameReceiver& rx, std::span<const float> x) {
    for (size_t i = 0; i < x.size(); i += 4096)
        rx.pushSamples(x.subspan(i, std::min<size_t>(4096, x.size() - i)));
}

} // namespace

TEST_CASE("uříznutý rámec nic nevydá, další celý rámec se dekóduje") {
    const am::ModemConfig cfg;
    const std::vector<uint8_t> payload = {'a', 'h', 'o', 'j'};
    auto tx = buildTx(payload, cfg);

    am::FrameReceiver rx;
    rx.configure(cfg, fsk());

    // jen 60 % rámce a pak dlouhé ticho
    pushChunked(rx, {tx.data(), tx.size() * 6 / 10});
    std::vector<float> silence(size_t(cfg.sample_rate), 0.f);
    pushChunked(rx, silence);
    CHECK(!rx.poll().has_value());

    // po něm poslaný kompletní rámec musí projít
    pushChunked(rx, tx);
    auto r = rx.poll();
    REQUIRE(r.has_value());
    CHECK(r->crc_ok);
    CHECK(r->payload == payload);
}

TEST_CASE("poškozený payload → crc_ok=false, ale rámec se ohlásí") {
    const am::ModemConfig cfg;
    // samé 0xFF → vynulování libovolného celého symbolu payloadu zaručeně
    // překlopí bit 1 → 0 a CRC musí selhat
    const std::vector<uint8_t> payload(8, 0xFF);
    auto tx = buildTx(payload, cfg);

    // hrubě znič kus payloadu; pozor na tichý doběh 0,05 s za rámcem
    const size_t sps = size_t(cfg.samplesPerSymbol());
    const size_t tail = size_t(0.05 * cfg.sample_rate);
    const size_t data_start = tx.size() - tail - (payload.size() * 8 + 16) * sps;
    for (size_t i = 0; i < 3 * sps; ++i) tx[data_start + i] = 0.f;

    am::FrameReceiver rx;
    rx.configure(cfg, fsk());
    pushChunked(rx, tx);
    auto r = rx.poll();
    REQUIRE(r.has_value());
    CHECK(!r->crc_ok);
}

TEST_CASE("dva rámce těsně za sebou se dekódují oba") {
    const am::ModemConfig cfg;
    const std::vector<uint8_t> p1 = {'p', 'r', 'v', 'n', 'i'};
    const std::vector<uint8_t> p2 = {'d', 'r', 'u', 'h', 'y'};
    auto tx = buildTx(p1, cfg);
    auto tx2 = buildTx(p2, cfg);
    tx.insert(tx.end(), tx2.begin(), tx2.end());

    am::FrameReceiver rx;
    rx.configure(cfg, fsk());
    pushChunked(rx, tx);

    auto r1 = rx.poll();
    auto r2 = rx.poll();
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(r1->payload == p1);
    CHECK(r2->payload == p2);
    CHECK(r1->crc_ok);
    CHECK(r2->crc_ok);
}

TEST_CASE("šum bez signálu nevydá žádný rámec") {
    const am::ModemConfig cfg;
    std::vector<float> silence(size_t(3 * cfg.sample_rate), 0.f);
    am::ChannelParams ch;
    ch.snr_db = 0.0; // šum na úrovni „signálu“ (RMS ticha je 0 → jen šum)
    ch.seed = 5;
    // simulateChannel s nulovým vstupem nevyrobí šum (sigma z RMS=0),
    // vyrob šum ručně přes gain=0 a vlastní vektor jedniček
    std::vector<float> ones(size_t(3 * cfg.sample_rate), 0.1f);
    auto noise = am::simulateChannel(ones, ch, cfg.sample_rate);
    for (size_t i = 0; i < noise.size(); ++i) noise[i] -= ones[i] * float(ch.gain);

    am::FrameReceiver rx;
    rx.configure(cfg, fsk());
    pushChunked(rx, noise);
    CHECK(!rx.poll().has_value());
}
