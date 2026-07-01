#include "protocol/crc16.hpp"

namespace am {

// Bitová (nikoliv tabulková) implementace — o řád pomalejší, ale pro
// velikost rámců v tomto projektu (desítky až stovky bajtů) je rychlost
// zanedbatelná a kód je čitelný bez generátoru tabulek.
uint16_t crc16(std::span<const uint8_t> data) {
    uint16_t crc = 0xFFFF;
    constexpr uint16_t kPoly = 0x1021;

    for (uint8_t byte : data) {
        crc ^= uint16_t(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = uint16_t((crc << 1) ^ kPoly);
            } else {
                crc = uint16_t(crc << 1);
            }
        }
    }

    return crc;
}

} // namespace am
