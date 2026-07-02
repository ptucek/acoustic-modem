#pragma once
// Tovární funkce jednotlivých schémat — implementace v fsk2.cpp, ook.cpp,
// dbpsk.cpp, mfsk16.cpp. Registruje je registry.cpp.

#include <memory>

#include "modem/modulator.hpp"

namespace am {

std::unique_ptr<IModulator>   makeFsk2Mod();
std::unique_ptr<IDemodulator> makeFsk2Demod();

std::unique_ptr<IModulator>   makeOokMod();
std::unique_ptr<IDemodulator> makeOokDemod();

std::unique_ptr<IModulator>   makeDbpskMod();
std::unique_ptr<IDemodulator> makeDbpskDemod();

std::unique_ptr<IModulator>   makeMfsk16Mod();
std::unique_ptr<IDemodulator> makeMfsk16Demod();

std::unique_ptr<IModulator>   makeQfskMod();
std::unique_ptr<IDemodulator> makeQfskDemod();

std::unique_ptr<IModulator>   makeWfskMod();
std::unique_ptr<IDemodulator> makeWfskDemod();

} // namespace am
