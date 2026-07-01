// Testy práce s bitovým proudem (core/bits.hpp): BitBuffer round-trip,
// padToMultiple a Grayův kód.
#include <cstdint>
#include <vector>

#include "core/bits.hpp"
#include "doctest/doctest.h"

using namespace am;

TEST_CASE("BitBuffer pushByte je LSB-first") {
    BitBuffer bb;
    bb.pushByte(0xA5); // 1010 0101 -> LSB první: 1,0,1,0,0,1,0,1
    REQUIRE(bb.size() == 8);
    CHECK(bb.bit(0) == true);
    CHECK(bb.bit(1) == false);
    CHECK(bb.bit(2) == true);
    CHECK(bb.bit(3) == false);
    CHECK(bb.bit(4) == false);
    CHECK(bb.bit(5) == true);
    CHECK(bb.bit(6) == false);
    CHECK(bb.bit(7) == true);
}

TEST_CASE("BitBuffer symbol čte n bitů LSB-first jako číslo") {
    BitBuffer bb;
    bb.pushByte(0xA5); // 1010 0101
    // první 4bitový symbol = bity 0..3 = 1,0,1,0 -> hodnota 0b0101 = 5
    CHECK(bb.symbol(0, 4) == 0x5);
    // druhý 4bitový symbol = bity 4..7 = 0,1,0,1 -> hodnota 0b1010 = 0xA
    CHECK(bb.symbol(1, 4) == 0xA);
}

TEST_CASE("BitBuffer pushBytes/toBytes je round-trip identita") {
    const std::vector<uint8_t> input{0x00, 0xFF, 0xA5, 0x5A, 0x01, 0x80};
    BitBuffer bb;
    bb.pushBytes(input);
    CHECK(bb.size() == input.size() * 8);

    const std::vector<uint8_t> output = bb.toBytes();
    REQUIRE(output.size() == input.size());
    CHECK(output == input);
}

TEST_CASE("BitBuffer padToMultiple doplní nuly na násobek n") {
    BitBuffer bb;
    bb.pushBits(0b101, 3); // 3 bity
    bb.padToMultiple(8);
    CHECK(bb.size() == 8);
    // původní 3 bity zůstanou, zbytek jsou nuly
    CHECK(bb.bit(0) == true);
    CHECK(bb.bit(1) == false);
    CHECK(bb.bit(2) == true);
    for (size_t i = 3; i < 8; ++i) CHECK(bb.bit(i) == false);
}

TEST_CASE("BitBuffer padToMultiple je no-op, pokud už je násobek") {
    BitBuffer bb;
    bb.pushBits(0xFF, 8);
    bb.padToMultiple(8);
    CHECK(bb.size() == 8);
}

TEST_CASE("grayEncode/grayDecode round-trip pro 0..255") {
    for (uint32_t v = 0; v <= 255; ++v) {
        const uint32_t g = grayEncode(v);
        CHECK(grayDecode(g) == v);
    }
}

TEST_CASE("Grayovy kódy sousedních hodnot se liší přesně v jednom bitu") {
    for (uint32_t v = 0; v < 255; ++v) {
        const uint32_t g1 = grayEncode(v);
        const uint32_t g2 = grayEncode(v + 1);
        const uint32_t diff = g1 ^ g2;
        // diff musí mít nastavený přesně jeden bit -> mocnina dvou
        CHECK(diff != 0);
        CHECK((diff & (diff - 1)) == 0);
    }
}
