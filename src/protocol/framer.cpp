// Sestavení vysílaného rámce: fyzická preambule (warm-up tón, chirp, mezera)
// + modulovaná část (REF, SYNC, HEADER, PAYLOAD, CRC). Viz docs/protocol.md.

#include "protocol/framer.hpp"

#include <cassert>
#include <cmath>
#include <numbers>

#include "dsp/chirp.hpp"
#include "protocol/crc16.hpp"

namespace am {

BitBuffer Framer::buildBits(std::span<const uint8_t> payload, uint8_t payload_type,
                            int bits_per_symbol) {
    // Kontrola i v Release (assert v NDEBUG zmizí): oversized payload by
    // vyrobil rámec, který přijímač po celé odvysílané airtime tiše zahodí
    // (len > kMaxPayload v hlavičce). Oříznout a assertnout v Debug.
    assert(payload.size() <= size_t(kMaxPayload));
    if (payload.size() > size_t(kMaxPayload))
        payload = payload.first(size_t(kMaxPayload));

    BitBuffer bits;
    // referenční symboly (samé jedničky) — viz komentář u kRefSymbols
    for (int i = 0; i < kRefSymbols * bits_per_symbol; ++i) bits.pushBit(true);

    bits.pushBits(kSyncWord, 16);

    // hlavička: ver_flags | payload_len LE | CRC-16 hlavičky (BE)
    const uint8_t header[3] = {
        payload_type,
        uint8_t(payload.size() & 0xFF),
        uint8_t((payload.size() >> 8) & 0xFF),
    };
    bits.pushBytes(header);
    const uint16_t hcrc = crc16(header);
    bits.pushByte(uint8_t(hcrc >> 8));
    bits.pushByte(uint8_t(hcrc & 0xFF));

    bits.pushBytes(payload);
    const uint16_t pcrc = crc16(payload);
    bits.pushByte(uint8_t(pcrc >> 8));
    bits.pushByte(uint8_t(pcrc & 0xFF));

    bits.padToMultiple(bits_per_symbol);
    return bits;
}

std::vector<float> Framer::buildFrame(std::span<const uint8_t> payload,
                                      IModulator& mod, const ModemConfig& cfg,
                                      uint8_t payload_type,
                                      const PreambleSpec& pre) {
    std::vector<float> out;

    // 1) warm-up tón: ustálení reproduktoru, mikrofonního AGC a squelche,
    //    s náběhovou/doběhovou rampou proti lupancům
    const int n_warm = int(pre.warmup_s * cfg.sample_rate + 0.5);
    const int n_ramp = int(0.005 * cfg.sample_rate);
    for (int i = 0; i < n_warm; ++i) {
        double env = 1.0;
        if (i < n_ramp) env = double(i) / n_ramp;
        if (i > n_warm - n_ramp) env = double(n_warm - i) / n_ramp;
        const double t = double(i) / cfg.sample_rate;
        out.push_back(float(cfg.amplitude * env *
                            std::sin(2.0 * std::numbers::pi * pre.warmup_freq * t)));
    }

    // 2) synchronizační chirp
    const std::vector<float> chirp = makeChirp(pre, cfg.sample_rate, cfg.amplitude);
    out.insert(out.end(), chirp.begin(), chirp.end());

    // 3) mezera (ticho) — odděluje chirp od prvního symbolu
    out.insert(out.end(), size_t(pre.gap_s * cfg.sample_rate + 0.5), 0.f);

    // 4) modulovaná data
    mod.reset();
    const BitBuffer bits = buildBits(payload, payload_type, mod.bitsPerSymbol());
    mod.modulate(bits, out);

    // krátký doběh ticha, ať poslední symbol nekončí uříznutě v přehrávači
    out.insert(out.end(), size_t(0.05 * cfg.sample_rate), 0.f);
    return out;
}

} // namespace am
