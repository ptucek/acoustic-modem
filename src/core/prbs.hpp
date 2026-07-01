#pragma once
// PRBS (pseudonáhodná bitová sekvence) pro měření skutečné BER: vysílač
// posílá známou sekvenci (payload_type = kPayloadPrbs), přijímač generuje
// tutéž a porovnává bit po bitu. LFSR x^15 + x^14 + 1 (PRBS-15) — perioda
// 32767 bitů, standard z telekomunikačních měření.

#include <cstdint>
#include <span>
#include <vector>

namespace am {

class Prbs15 {
public:
    explicit Prbs15(uint16_t seed = 0x7FFF) : state_(seed ? seed : 0x7FFF) {}

    bool nextBit() {
        const uint16_t bit = ((state_ >> 14) ^ (state_ >> 13)) & 1u;
        state_ = uint16_t((state_ << 1) | bit) & 0x7FFF;
        return bit != 0;
    }

    uint8_t nextByte() {
        uint8_t b = 0;
        for (int i = 0; i < 8; ++i) b |= uint8_t(nextBit()) << i; // LSB-first
        return b;
    }

    std::vector<uint8_t> generate(size_t n_bytes) {
        std::vector<uint8_t> out(n_bytes);
        for (auto& b : out) b = nextByte();
        return out;
    }

private:
    uint16_t state_;
};

// Porovnání přijatého PRBS payloadu s očekávaným: vrací počet chybných bitů.
// Oba generátory startují ze stejného seedu → stačí vygenerovat stejný počet
// bajtů a spočítat popcount XORu.
inline size_t countBitErrors(std::span<const uint8_t> received,
                             std::span<const uint8_t> expected) {
    size_t errors = 0;
    const size_t n = std::min(received.size(), expected.size());
    for (size_t i = 0; i < n; ++i) {
        uint8_t x = uint8_t(received[i] ^ expected[i]);
        while (x) {
            errors += x & 1u;
            x >>= 1;
        }
    }
    // chybějící/přebývající bajty počítej jako plně chybné
    errors += 8 * (std::max(received.size(), expected.size()) - n);
    return errors;
}

} // namespace am
