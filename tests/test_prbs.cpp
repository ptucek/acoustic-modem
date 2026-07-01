// Testy PRBS-15 generátoru a počítání bitových chyb.

#include "core/prbs.hpp"
#include "doctest/doctest.h"

TEST_CASE("PRBS-15: perioda 32767 bitů") {
    am::Prbs15 g;
    // stav po celé periodě se musí vrátit k výchozímu → sekvence bitů
    // se opakuje přesně po 2^15 - 1 krocích
    std::vector<bool> first(100);
    for (auto&& b : first) b = g.nextBit();
    am::Prbs15 h;
    for (int i = 0; i < 32767; ++i) h.nextBit();
    for (int i = 0; i < 100; ++i) CHECK(h.nextBit() == first[size_t(i)]);
}

TEST_CASE("PRBS-15: stejný seed → stejná sekvence, jiný → jiná") {
    am::Prbs15 a(123), b(123), c(999);
    auto va = a.generate(64);
    auto vb = b.generate(64);
    auto vc = c.generate(64);
    CHECK(va == vb);
    CHECK(va != vc);
    // vyváženost: zhruba polovina jedničkových bitů
    size_t ones = am::countBitErrors(va, std::vector<uint8_t>(64, 0));
    CHECK(ones > 64 * 8 / 3);
    CHECK(ones < 64 * 8 * 2 / 3);
}

TEST_CASE("countBitErrors") {
    std::vector<uint8_t> x = {0x00, 0xFF, 0x0F};
    std::vector<uint8_t> y = {0x01, 0xFF, 0x00};
    CHECK(am::countBitErrors(x, y) == 1 + 0 + 4);
    // rozdílná délka: chybějící bajt = 8 chyb
    std::vector<uint8_t> shorter = {0x00, 0xFF};
    CHECK(am::countBitErrors(x, shorter) == 0 + 0 + 8);
    CHECK(am::countBitErrors(x, x) == 0);
}
