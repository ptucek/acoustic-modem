#pragma once
// Zásuvná rozhraní modulátorů/demodulátorů.
//
// Zásadní rozhodnutí architektury: symbolové ČASOVÁNÍ neřeší demodulátor,
// ale sdílený FrameReceiver — korelace na chirp preambuli fixuje hranici
// prvního symbolu, dál běží pevné symbolové hodiny. Demodulátor tak dostává
// vždy přesně jedno časově zarovnané okno symbolu a je to čistá funkce
// „vzorky → bity". Nová modulace = ~60–100 řádků.

#include <complex>
#include <memory>
#include <span>
#include <vector>

#include "core/bits.hpp"
#include "core/config.hpp"

namespace am {

class IModulator {
public:
    virtual ~IModulator() = default;
    virtual void configure(const ModemConfig& c) = 0;
    virtual int  bitsPerSymbol() const = 0;
    virtual void reset() = 0; // vynulování fázového akumulátoru apod.

    // Připojí vzorky pro daný bitový proud na konec `out`.
    // Volající zajistí bits.size() % bitsPerSymbol() == 0 (padToMultiple).
    virtual void modulate(const BitBuffer& bits, std::vector<float>& out) = 0;
};

// Diagnostika posledního symbolu — krmí GUI grafy (sloupce energií tónů,
// fázový scatter) a odhad kvality signálu pro graf chybovosti.
struct SymbolDiag {
    std::vector<float>  tone_energy;   // FSK: 2, MFSK: 16 hodnot
    std::complex<float> phasor{};      // DBPSK: bod „konstelace"
    float               snr_db = 0.f;  // vítězný tón vs. zbytek okna
};

class IDemodulator {
public:
    virtual ~IDemodulator() = default;
    virtual void configure(const ModemConfig& c) = 0;
    virtual int  bitsPerSymbol() const = 0;
    virtual void reset() = 0; // např. fázová reference DBPSK

    // `sym` má přesně ModemConfig::samplesPerSymbol() vzorků, časově
    // zarovnaných FrameReceiverem. Vrací bitsPerSymbol() bitů (LSB první).
    // `diag` může být nullptr (offline testy).
    virtual uint32_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) = 0;
};

// Registr schémat — plní combo box v GUI a --scheme v CLI.
struct ModemScheme {
    const char* name; // "2-FSK", "OOK", "DBPSK", "16-FSK"
    std::unique_ptr<IModulator>   (*makeMod)();
    std::unique_ptr<IDemodulator> (*makeDemod)();
};

std::span<const ModemScheme> modemRegistry();
const ModemScheme* findScheme(const char* name); // nullptr pokud neexistuje

} // namespace am
