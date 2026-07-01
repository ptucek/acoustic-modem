#pragma once
// Minimální čtení/zápis mono 16bit PCM WAV — jen co potřebujeme pro
// offline testy a nahrávky z/pro zvukovou kartu. Žádná závislost na
// externí knihovně, formát je jednoduchý a dobře dokumentovaný.

#include <span>
#include <string>
#include <vector>

namespace am {

// Zapíše `samples` (rozsah -1..1) jako mono 16bit PCM WAV. Vzorky mimo
// rozsah se ořežou (clamp), nehází výjimku. Vrací false při chybě I/O.
bool writeWav(const std::string& path, std::span<const float> samples, int sample_rate);

// Načte WAV soubor do `samples` (float, -1..1) a vrátí vzorkovací
// frekvenci v `sample_rate`. Podporuje jen PCM (formát 1), 16 bitů;
// stereo se zprůměruje na mono. Neznámé chunky (např. LIST) se přeskočí.
// Při jakémkoliv poškozeném/nepodporovaném vstupu vrátí false (nehází).
bool readWav(const std::string& path, std::vector<float>& samples, int& sample_rate);

} // namespace am
