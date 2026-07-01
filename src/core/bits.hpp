#pragma once
// Práce s bitovým proudem. Pořadí bitů: LSB-first v rámci bajtu
// (bit 0 bajtu se vysílá jako první) — konvence platí pro celý projekt.

#include <cstdint>
#include <span>
#include <vector>

namespace am {

class BitBuffer {
public:
    BitBuffer() = default;

    void pushBit(bool b) { bits_.push_back(b ? 1 : 0); }

    // n nejnižších bitů hodnoty v, LSB první
    void pushBits(uint32_t v, int n) {
        for (int i = 0; i < n; ++i) pushBit((v >> i) & 1u);
    }

    void pushByte(uint8_t b) { pushBits(b, 8); }
    void pushBytes(std::span<const uint8_t> data) {
        for (uint8_t b : data) pushByte(b);
    }

    bool   bit(size_t i) const { return bits_[i] != 0; }
    size_t size() const { return bits_.size(); }
    bool   empty() const { return bits_.empty(); }

    // i-tá skupina n bitů jako číslo (pro M-ární modulace), LSB první
    uint32_t symbol(size_t i, int n) const {
        uint32_t v = 0;
        for (int k = 0; k < n; ++k) v |= uint32_t(bits_[i * n + k]) << k;
        return v;
    }

    // doplnění nulami na násobek n bitů (celé symboly)
    void padToMultiple(int n) {
        while (bits_.size() % size_t(n)) pushBit(false);
    }

    std::vector<uint8_t> toBytes() const {
        std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits_.size(); ++i)
            if (bits_[i]) out[i / 8] |= uint8_t(1u << (i % 8));
        return out;
    }

private:
    std::vector<uint8_t> bits_; // 1 bajt na bit — jednoduchost > paměť
};

// Grayův kód: sousední indexy tónů se liší jedním bitem → chyba na sousední
// tón v MFSK poškodí jen jeden bit
inline uint32_t grayEncode(uint32_t v) { return v ^ (v >> 1); }
inline uint32_t grayDecode(uint32_t g) {
    uint32_t v = g;
    for (uint32_t s = 1; s < 32; s <<= 1) v ^= v >> s;
    return v;
}

} // namespace am
