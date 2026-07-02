// Registr modemových schémat — spojuje tovární funkce ze schemes.hpp
// s jmény, která vidí uživatel v CLI (--scheme) a GUI (combo box).

#include "modem/modulator.hpp"

#include <cstring>

#include "modem/schemes.hpp"

namespace am {

namespace {

constexpr ModemScheme kSchemes[] = {
    { "2-FSK",  &makeFsk2Mod,   &makeFsk2Demod },
    { "OOK",    &makeOokMod,    &makeOokDemod },
    { "DBPSK",  &makeDbpskMod,  &makeDbpskDemod },
    { "16-FSK", &makeMfsk16Mod, &makeMfsk16Demod },
    { "Q-FSK",  &makeQfskMod,   &makeQfskDemod },
};

} // namespace

std::span<const ModemScheme> modemRegistry() {
    return kSchemes;
}

const ModemScheme* findScheme(const char* name) {
    for (const auto& s : kSchemes) {
        if (std::strcmp(s.name, name) == 0) return &s;
    }
    return nullptr;
}

} // namespace am
