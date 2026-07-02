#pragma once
// Práce s bitovým proudem. Pořadí bitů: LSB-first v rámci bajtu
// (bit 0 bajtu se vysílá jako první) — konvence platí pro celý projekt.

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

namespace am {

class BitBuffer {
public:
    BitBuffer() = default;

    void pushBit(bool b) { bits_.push_back(b ? 1 : 0); }

    // n nejnižších bitů hodnoty v, LSB první
    void pushBits(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) pushBit((v >> i) & 1u);
    }

    void pushByte(uint8_t b) { pushBits(b, 8); }
    void pushBytes(std::span<const uint8_t> data) {
        for (uint8_t b : data) pushByte(b);
    }

    bool   bit(size_t i) const { return bits_[i] != 0; }
    size_t size() const { return bits_.size(); }
    bool   empty() const { return bits_.empty(); }

    // i-tá skupina n bitů jako číslo (pro M-ární modulace), LSB první.
    // Návratový typ uint64_t → schémata s >32 b/symbol (W-FSK: 44 b).
    uint64_t symbol(size_t i, int n) const {
        uint64_t v = 0;
        for (int k = 0; k < n; ++k) v |= uint64_t(bits_[i * n + k]) << k;
        return v;
    }

    // doplnění nulami na násobek n bitů (celé symboly)
    void padToMultiple(int n) {
        while (bits_.size() % size_t(n)) pushBit(false);
    }

    // Zahodí prvních n bitů (posune začátek). Přijímač po přečtení pole
    // rámce zahodí jen jeho bity a případný přesah symbolu, který pole
    // překročil, ponechá pro pole následující — nutné, když bitsPerSymbol
    // nedělí délky polí (např. 16 b/symbol u Q-FSK: hlavička = 2,5 symbolu).
    void dropFront(size_t n) {
        if (n >= bits_.size()) bits_.clear();
        else bits_.erase(bits_.begin(), bits_.begin() + std::ptrdiff_t(n));
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
