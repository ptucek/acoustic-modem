#pragma once
// TX strana protokolu: payload → kompletní vektor vzorků
// [warm-up][chirp][mezera][SYNC][HEADER][PAYLOAD][CRC-16]
// Formát viz docs/protocol.md.

#include <cstdint>
#include <span>
#include <vector>

#include "core/config.hpp"
#include "modem/modulator.hpp"

namespace am {

// ver_flags v hlavičce rámce (bit 0..2 typ payloadu, zbytek rezervy pro FEC/whitening)
enum PayloadType : uint8_t {
    kPayloadText  = 0, // UTF-8 text
    kPayloadPrbs  = 1, // PRBS testovací vzor pro měření BER
    kPayloadEther = 2, // ethernetový rámec (modem_tap)
};

class Framer {
public:
    // Sestaví kompletní rámec včetně fyzické preambule. `mod` musí být
    // nakonfigurovaný stejným `cfg`. payload.size() <= kMaxPayload.
    static std::vector<float> buildFrame(std::span<const uint8_t> payload,
                                         IModulator& mod, const ModemConfig& cfg,
                                         uint8_t payload_type = kPayloadText,
                                         const PreambleSpec& pre = {});

    // Bitový obsah rámce bez preambule (SYNC..CRC) — sdílí TX i testy.
    static BitBuffer buildBits(std::span<const uint8_t> payload, uint8_t payload_type,
                               int bits_per_symbol);
};

} // namespace am
