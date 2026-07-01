#include "core/wav_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace am {

namespace {

// Pomocné funkce pro little-endian zápis/čtení — WAV je vždy LE,
// nezávisle na endianitě hostitele.

void putU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(uint8_t(v & 0xFF));
    buf.push_back(uint8_t((v >> 8) & 0xFF));
    buf.push_back(uint8_t((v >> 16) & 0xFF));
    buf.push_back(uint8_t((v >> 24) & 0xFF));
}

void putU16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(uint8_t(v & 0xFF));
    buf.push_back(uint8_t((v >> 8) & 0xFF));
}

uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint16_t readU16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}

} // namespace

bool writeWav(const std::string& path, std::span<const float> samples, int sample_rate) {
    if (sample_rate <= 0) return false;

    constexpr int kBitsPerSample = 16;
    constexpr int kChannels = 1;
    const uint32_t data_bytes = uint32_t(samples.size()) * (kBitsPerSample / 8);
    const uint32_t byte_rate = uint32_t(sample_rate) * kChannels * (kBitsPerSample / 8);
    const uint16_t block_align = kChannels * (kBitsPerSample / 8);

    std::vector<uint8_t> header;
    header.reserve(44);

    // RIFF chunk
    header.push_back('R'); header.push_back('I'); header.push_back('F'); header.push_back('F');
    putU32(header, 36 + data_bytes); // velikost celého souboru - 8
    header.push_back('W'); header.push_back('A'); header.push_back('V'); header.push_back('E');

    // fmt chunk
    header.push_back('f'); header.push_back('m'); header.push_back('t'); header.push_back(' ');
    putU32(header, 16);              // velikost fmt chunku
    putU16(header, 1);                // formát 1 = PCM
    putU16(header, uint16_t(kChannels));
    putU32(header, uint32_t(sample_rate));
    putU32(header, byte_rate);
    putU16(header, block_align);
    putU16(header, uint16_t(kBitsPerSample));

    // data chunk header
    header.push_back('d'); header.push_back('a'); header.push_back('t'); header.push_back('a');
    putU32(header, data_bytes);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    f.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    if (!f) return false;

    // Vzorky se konvertují po menších blocích, aby se nealokoval jeden
    // velký buffer pro celý soubor.
    constexpr size_t kBlock = 4096;
    std::vector<int16_t> block;
    block.reserve(kBlock);
    for (size_t i = 0; i < samples.size(); i += kBlock) {
        const size_t n = std::min(kBlock, samples.size() - i);
        block.clear();
        for (size_t k = 0; k < n; ++k) {
            float s = std::clamp(samples[i + k], -1.0f, 1.0f);
            block.push_back(int16_t(s * 32767.0f));
        }
        f.write(reinterpret_cast<const char*>(block.data()),
                std::streamsize(block.size() * sizeof(int16_t)));
        if (!f) return false;
    }

    return true;
}

bool readWav(const std::string& path, std::vector<float>& samples, int& sample_rate) {
    samples.clear();
    sample_rate = 0;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    uint8_t riff_hdr[12];
    f.read(reinterpret_cast<char*>(riff_hdr), 12);
    if (!f || f.gcount() != 12) return false;
    if (std::memcmp(riff_hdr, "RIFF", 4) != 0) return false;
    if (std::memcmp(riff_hdr + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    bool have_data = false;

    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sr = 0;

    std::vector<uint8_t> data_bytes;

    // Iterace přes chunky, dokud nedojde soubor. Neznámé chunky (LIST,
    // fact, ...) se přeskočí podle udané délky.
    for (;;) {
        uint8_t chunk_hdr[8];
        f.read(reinterpret_cast<char*>(chunk_hdr), 8);
        if (!f || f.gcount() != 8) break; // konec souboru, nic dalšího ke čtení

        char id[5] = {};
        std::memcpy(id, chunk_hdr, 4);
        uint32_t chunk_size = readU32(chunk_hdr + 4);

        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (chunk_size < 16) return false;
            std::vector<uint8_t> fmt(chunk_size);
            f.read(reinterpret_cast<char*>(fmt.data()), std::streamsize(chunk_size));
            if (!f || size_t(f.gcount()) != chunk_size) return false;

            format_tag = readU16(fmt.data() + 0);
            channels = readU16(fmt.data() + 2);
            sr = readU32(fmt.data() + 4);
            bits_per_sample = readU16(fmt.data() + 14);
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_bytes.resize(chunk_size);
            f.read(reinterpret_cast<char*>(data_bytes.data()), std::streamsize(chunk_size));
            if (!f || size_t(f.gcount()) != chunk_size) return false;
            have_data = true;
        } else {
            // neznámý chunk — přeskočit
            f.seekg(std::streamoff(chunk_size), std::ios::cur);
            if (!f) break;
        }

        // WAV chunky jsou vyrovnané na sudý počet bajtů (padding byte,
        // pokud je velikost lichá).
        if (chunk_size % 2 == 1) {
            f.seekg(1, std::ios::cur);
        }

        if (have_fmt && have_data) break;
    }

    if (!have_fmt || !have_data) return false;
    if (format_tag != 1) return false;      // jen nekomprimované PCM
    if (bits_per_sample != 16) return false;
    if (channels != 1 && channels != 2) return false;
    if (sr == 0) return false;

    const size_t bytes_per_frame = size_t(channels) * 2;
    if (bytes_per_frame == 0 || data_bytes.size() % bytes_per_frame != 0) return false;
    const size_t frame_count = data_bytes.size() / bytes_per_frame;

    samples.resize(frame_count);
    const uint8_t* p = data_bytes.data();
    for (size_t i = 0; i < frame_count; ++i) {
        if (channels == 1) {
            int16_t s = int16_t(readU16(p));
            samples[i] = float(s) / 32768.0f;
            p += 2;
        } else {
            int16_t l = int16_t(readU16(p));
            int16_t r = int16_t(readU16(p + 2));
            samples[i] = (float(l) + float(r)) / 2.0f / 32768.0f;
            p += 4;
        }
    }

    sample_rate = int(sr);
    return true;
}

} // namespace am
