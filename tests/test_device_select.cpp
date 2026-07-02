// Testy matchDevices (audio/audio_engine.hpp): výběr zvukového zařízení
// indexem i podřetězcem jména. Čistá funkce nad DeviceInfo — nepotřebuje
// zvukový hardware ani linkovat modemaudio.
#include <string>
#include <vector>

#include "audio/audio_engine.hpp"
#include "doctest/doctest.h"

using namespace am;

namespace {

// Realistický seznam: síťové zařízení (AirPlay), webkamera, monitory
// sinků a vestavěné mikrofony. Indexy jsou per směr, jak je dává enumerate().
std::vector<DeviceInfo> sampleDevices() {
    return {
        {"Monitor of PT-M4Pro", true, 0},
        {"Cisco Desk Camera 1080p Analogové stereo", true, 1},
        {"Monitor of HD Audio Speaker", true, 2},
        {"HD Audio Stereo Microphone", true, 3},
        {"HD Audio Digital Microphone", true, 4},
        {"PT-M4Pro", false, 0},
        {"HD Audio Speaker", false, 1},
    };
}

} // namespace

TEST_CASE("matchDevices: číselná specifikace je přímo index daného směru") {
    const auto devs = sampleDevices();

    CHECK(matchDevices(devs, "4", true) == std::vector<int>{4});
    CHECK(matchDevices(devs, "1", false) == std::vector<int>{1});
    // index existuje jen v opačném směru → žádná shoda
    CHECK(matchDevices(devs, "4", false).empty());
    // mimo rozsah → žádná shoda (start() by dřív tiše vzal default)
    CHECK(matchDevices(devs, "9", true).empty());
    // nesmyslně dlouhé číslo nesmí přetéct
    CHECK(matchDevices(devs, "99999999999999999999", true).empty());
}

TEST_CASE("matchDevices: jméno je case-insensitivní podřetězec") {
    const auto devs = sampleDevices();

    CHECK(matchDevices(devs, "digital", true) == std::vector<int>{4});
    CHECK(matchDevices(devs, "DIGITAL micro", true) == std::vector<int>{4});
    CHECK(matchDevices(devs, "speaker", false) == std::vector<int>{1});
    // směr filtruje: kamera není playback
    CHECK(matchDevices(devs, "Cisco", false).empty());
}

TEST_CASE("matchDevices: nejednoznačnost vrací všechny kandidáty") {
    const auto devs = sampleDevices();

    // "microphone" sedí na stereo i digital — rozhodnutí je na volajícím
    CHECK(matchDevices(devs, "microphone", true) == std::vector<int>{3, 4});
    // "PT-M4Pro" je capture monitor i playback sink — per směr jednoznačné
    CHECK(matchDevices(devs, "pt-m4pro", true) == std::vector<int>{0});
    CHECK(matchDevices(devs, "pt-m4pro", false) == std::vector<int>{0});
}

TEST_CASE("matchDevices: prázdná specifikace nevrací nic") {
    const auto devs = sampleDevices();

    CHECK(matchDevices(devs, "", true).empty());
    CHECK(matchDevices(devs, "", false).empty());
}
