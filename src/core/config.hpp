#pragma once
// Společná konfigurace modemu. GUI vlastní jednu instanci a při změně ji
// kopií posílá DSP vláknu (ReconfigureCmd) — žádný sdílený mutable stav.

namespace am {

struct ModemConfig {
    int    sample_rate = 48000;   // nativní frekvence PipeWire, bez převzorkování
    double baud        = 31.25;   // 48000/31.25 = 1536 vzorků/symbol (celé číslo)
    double amplitude   = 0.5;     // plná škála 1.0; 0.5 nechává rezervu proti klipování

    // 2-FSK: f0 = space (bit 0), f1 = mark (bit 1); OOK a DBPSK používají f0 jako nosnou
    double f0 = 1200.0;
    double f1 = 2200.0;

    // MFSK: tóny mfsk_base + k*mfsk_spacing, k = 0..mfsk_tones-1
    double mfsk_base    = 1000.0;
    double mfsk_spacing = 62.5;   // 2× baud → ortogonální tóny
    int    mfsk_tones   = 16;     // 4 bity/symbol

    int samplesPerSymbol() const { return static_cast<int>(sample_rate / baud + 0.5); }
};

// Fyzická preambule (nezávislá na modulaci) — viz docs/protocol.md
struct PreambleSpec {
    double warmup_s      = 0.200;  // ustálení reproduktoru/AGC
    double warmup_freq   = 1000.0;
    double chirp_s       = 0.100;  // lineární chirp pro synchronizaci
    double chirp_f_start = 800.0;
    double chirp_f_end   = 2800.0;
    double gap_s         = 0.020;  // ticho mezi chirpem a prvním symbolem
};

inline constexpr int kMaxPayload = 256;      // drží drift hodin < 0,5 % symbolu
inline constexpr unsigned kSyncWord = 0x2DD4; // 16 b, vyvážené 0/1 (seed OOK prahu)

// Referenční symboly (samé jedničky) před SYNC — jednotné pro všechna
// schémata: DBPSK z nich bere fázovou referenci, OOK vidí zapnutou nosnou
// pro seed prahu, FSK jim nevadí. Přijímač je dekóduje a zahodí.
inline constexpr int kRefSymbols = 1;

} // namespace am
