// Test poloduplexní MAC vrstvy: dvě AcousticLink stanice propojené
// simulovaným médiem (vzduchem) — bez zvukové karty i bez TAP.
//
// Simulace média: krokovaná po 10 ms; co stanice „přehrává", to druhá
// „slyší" (přes simulovaný útlum), vlastní vysílání stanice neslyší
// (MAC ho stejně zahazuje).

#include <deque>
#include <vector>

#include "doctest/doctest.h"
#include "link/acoustic_link.hpp"
#include "modem/modulator.hpp"

namespace {

struct SimStation {
    am::AcousticLink link;
    std::deque<float> mic;      // co stanice slyší (fronta vzorků)
    std::vector<float> playing; // co právě hraje reproduktor
    size_t play_pos = 0;

    void wire(const am::AcousticLink::Params& p) {
        link.configure(
            p,
            /*pop*/
            [this](std::span<float> out) {
                const size_t n = std::min(out.size(), mic.size());
                for (size_t i = 0; i < n; ++i) {
                    out[i] = mic.front();
                    mic.pop_front();
                }
                return n;
            },
            /*push*/
            [this](std::span<const float> x) {
                playing.assign(x.begin(), x.end());
                play_pos = 0;
            },
            /*txPending*/
            [this] { return playing.size() - play_pos; });
    }
};

struct SimAir {
    SimStation a, b;
    double t = 0.0;
    static constexpr int kDt = 480; // 10 ms při 48 kHz

    void step() {
        // reproduktor → mikrofon protistanice (útlum 0.5)
        auto radiate = [](SimStation& from, SimStation& to) {
            for (int i = 0; i < kDt; ++i) {
                float s = 0.f;
                if (from.play_pos < from.playing.size())
                    s = 0.5f * from.playing[from.play_pos++];
                to.mic.push_back(s);
            }
        };
        radiate(a, b);
        radiate(b, a);
        t += 0.010;
        a.link.tick(t);
        b.link.tick(t);
    }

    void run(double seconds) {
        const int steps = int(seconds / 0.010);
        for (int i = 0; i < steps; ++i) step();
    }
};

am::AcousticLink::Params params(uint32_t seed) {
    am::AcousticLink::Params p;
    p.scheme = am::findScheme("16-FSK"); // rychlejší testy než 2-FSK
    p.seed = seed;
    return p;
}

std::vector<uint8_t> packet(size_t n, uint8_t base) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(base + i);
    return v;
}

} // namespace

TEST_CASE("link: jednosměrný paket A → B") {
    SimAir air;
    air.a.wire(params(11));
    air.b.wire(params(22));

    const auto pkt = packet(60, 1);
    air.a.link.sendPacket(pkt);
    air.run(8.0); // 60 B @ 16-FSK ≈ 4,3 s + preambule

    auto got = air.b.link.receivePacket();
    REQUIRE(got.has_value());
    CHECK(*got == pkt);
    CHECK(air.b.link.stats().rx_ok == 1);
    CHECK(!air.a.link.receivePacket().has_value()); // A neslyší sám sebe
}

TEST_CASE("link: obousměrně po sobě (half-duplex)") {
    SimAir air;
    air.a.wire(params(11));
    air.b.wire(params(22));

    const auto pa = packet(40, 10);
    const auto pb = packet(40, 90);
    air.a.link.sendPacket(pa);
    air.run(6.0);
    air.b.link.sendPacket(pb);
    air.run(6.0);

    auto at_b = air.b.link.receivePacket();
    auto at_a = air.a.link.receivePacket();
    REQUIRE(at_b.has_value());
    REQUIRE(at_a.has_value());
    CHECK(*at_b == pa);
    CHECK(*at_a == pb);
}

TEST_CASE("link: CSMA — současné odeslání se serializuje backoffem") {
    SimAir air;
    air.a.wire(params(101)); // různé seedy → různé backoffy
    air.b.wire(params(202));

    const auto pa = packet(30, 10);
    const auto pb = packet(30, 90);
    // obě stanice chtějí vysílat ve stejný okamžik
    air.a.link.sendPacket(pa);
    air.b.link.sendPacket(pb);
    air.run(20.0); // dost času na kolizi/backoff/opakování

    auto at_b = air.b.link.receivePacket();
    auto at_a = air.a.link.receivePacket();
    // aspoň jeden směr musí projít; při šťastném rozhoz­ení backoffů oba
    const int delivered = int(at_b.has_value()) + int(at_a.has_value());
    CHECK(delivered >= 1);
    if (at_b) CHECK(*at_b == pa);
    if (at_a) CHECK(*at_a == pb);
}

TEST_CASE("link: carrier sense — B odloží vysílání, když A vysílá") {
    SimAir air;
    air.a.wire(params(11));
    air.b.wire(params(22));

    air.a.link.sendPacket(packet(60, 1));
    air.run(1.0); // A začne vysílat (preambule už hraje)
    air.b.link.sendPacket(packet(20, 50));
    air.run(0.5);
    // B musí být v backoffu nebo idle s frontou — ne Transmitting
    CHECK(air.b.link.macState() != am::AcousticLink::MacState::Transmitting);
    air.run(12.0);
    // nakonec projdou oba pakety
    CHECK(air.b.link.receivePacket().has_value());
    CHECK(air.a.link.receivePacket().has_value());
}
