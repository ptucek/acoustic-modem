#pragma once
// CRC-16/CCITT-FALSE — kontrolní součet rámce (viz docs/protocol.md).
// Polynom 0x1021, init 0xFFFF, bez reflexe vstupu/výstupu, bez finálního XOR.

#include <cstdint>
#include <span>

namespace am {

// Testovací vektor: crc16 nad ASCII "123456789" (bez uvozovek) musí
// vyjít 0x29B1 — standardní kontrolní vektor pro CRC-16/CCITT-FALSE.
uint16_t crc16(std::span<const uint8_t> data);

} // namespace am
