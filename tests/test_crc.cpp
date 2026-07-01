// Testy CRC-16/CCITT-FALSE (protocol/crc16.hpp).
#include <cstdint>
#include <vector>

#include "doctest/doctest.h"
#include "protocol/crc16.hpp"

using namespace am;

TEST_CASE("crc16 standardní testovací vektor \"123456789\"") {
    const std::vector<uint8_t> data{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16(data) == 0x29B1);
}

TEST_CASE("crc16 prázdná data vrátí init hodnotu") {
    // Bez jediného bajtu se registr nezmění, zůstává na init 0xFFFF.
    CHECK(crc16({}) == 0xFFFF);
}

TEST_CASE("crc16 druhý známý vektor (nezávislé přepočítání)") {
    // Hodnota přepočítána referenční Pythonovou implementací stejného
    // algoritmu (poly 0x1021, init 0xFFFF, bez reflexe/XOR) — nezávislá
    // kontrola na jiných datech než standardní "123456789".
    const std::vector<uint8_t> data{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02};
    CHECK(crc16(data) == 0x8FA2);
}

TEST_CASE("crc16 nad daty s připojeným CRC (big-endian) je nulové") {
    // Vlastnost CRC: připojíme-li kontrolní součet za data ve stejném
    // pořadí bajtů, jako ho generuje algoritmus (big-endian, MSB první),
    // CRC nad rozšířenými daty vyjde 0 — typická kontrola na přijímací
    // straně rámce.
    const std::vector<uint8_t> data{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    const uint16_t c = crc16(data);

    std::vector<uint8_t> extended = data;
    extended.push_back(uint8_t((c >> 8) & 0xFF));
    extended.push_back(uint8_t(c & 0xFF));

    CHECK(crc16(extended) == 0x0000);
}
