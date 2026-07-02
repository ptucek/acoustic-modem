cmake_minimum_required(VERSION 3.24)
project(acoustic_modem CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

option(BUILD_GUI "Build GUI" ON)

# ---------------------------------------------------------------------------
# Statická knihovna modemcore — DSP, protokol, modulace, jádro. Sdílí ji
# CLI, testy i (později) GUI a modem_tap.
# ---------------------------------------------------------------------------
set(MODEMCORE_SOURCES
    src/core/wav_io.cpp
    src/dsp/chirp.cpp
    src/dsp/fir.cpp
    src/dsp/channel_sim.cpp
    src/dsp/goertzel.cpp
    src/modem/fsk2.cpp
    src/modem/ook.cpp
    src/modem/dbpsk.cpp
    src/modem/mfsk16.cpp
    src/modem/qfsk.cpp
    src/modem/registry.cpp
    src/protocol/crc16.cpp
    src/protocol/framer.cpp
    src/protocol/frame_receiver.cpp
    src/link/acoustic_link.cpp
)

# KissFFT (C knihovna) — Goertzelův algoritmus jej nepotřebuje přímo, ale
# budoucí spektrální GUI zobrazení (vodopád) na něm staví.
set(KISSFFT_SOURCES
    third_party/kissfft/kiss_fft.c
    third_party/kissfft/kiss_fftr.c
)

add_library(modemcore STATIC
    ${MODEMCORE_SOURCES}
    ${KISSFFT_SOURCES}
)

target_include_directories(modemcore PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_features(modemcore PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# modemaudio — tenká statická knihovna nad miniaudio (real audio I/O).
# Oddělená od modemcore, aby jednotky bez potřeby zvukového HW (testy DSP
# apod.) nemusely táhnout miniaudio a jeho systémové závislosti.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(modemaudio STATIC
    src/audio/audio_engine.cpp
)
target_include_directories(modemaudio PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)
target_link_libraries(modemaudio PUBLIC modemcore)
target_link_libraries(modemaudio PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
# modemaudio linkuje modemcore PUBLIC — netahat modemcore podruhé
# (ld na macOS si stěžoval na duplicitní knihovnu)
add_executable(modem_cli src/cli/main_cli.cpp)
target_link_libraries(modem_cli PRIVATE modemaudio)

# ---------------------------------------------------------------------------
# Testy (doctest, hlavičkově-only třetí strana)
# ---------------------------------------------------------------------------
add_executable(modem_tests
    tests/test_main.cpp
    tests/test_crc.cpp
    tests/test_bits.cpp
    tests/test_ring.cpp
    tests/test_goertzel.cpp
    tests/test_chain.cpp
    tests/test_qfsk.cpp
    tests/test_frame.cpp
    tests/test_prbs.cpp
    tests/test_link.cpp
    tests/test_device_select.cpp
)
target_link_libraries(modem_tests PRIVATE modemcore)

enable_testing()
add_test(NAME modem_tests COMMAND modem_tests)

# ---------------------------------------------------------------------------
# GUI (nepovinná, vyžaduje SDL3 + OpenGL) — src/app/* ještě neexistuje,
# proto je celý cíl zagrogovaný existencí main_gui.cpp.
# ---------------------------------------------------------------------------
if(BUILD_GUI AND EXISTS ${CMAKE_SOURCE_DIR}/src/app/main_gui.cpp)
    find_package(SDL3 QUIET)
    if(SDL3_FOUND)
        find_package(OpenGL REQUIRED)

        file(GLOB APP_SOURCES ${CMAKE_SOURCE_DIR}/src/app/*.cpp)

        set(IMGUI_SOURCES
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/imgui_demo.cpp
            third_party/imgui/backends/imgui_impl_sdl3.cpp
            third_party/imgui/backends/imgui_impl_opengl3.cpp
        )

        file(GLOB IMPLOT_SOURCES ${CMAKE_SOURCE_DIR}/third_party/implot/*.cpp)

        add_executable(modem_gui
            ${APP_SOURCES}
            ${IMGUI_SOURCES}
            ${IMPLOT_SOURCES}
        )

        target_include_directories(modem_gui PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/imgui
            ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
            ${CMAKE_SOURCE_DIR}/third_party/implot
        )

        target_link_libraries(modem_gui PRIVATE modemaudio SDL3::SDL3 OpenGL::GL)
    else()
        message(STATUS "SDL3 nenalezeno — modem_gui se nebude sestavovat")
    endif()
else()
    message(STATUS "src/app/main_gui.cpp neexistuje — modem_gui se nebude sestavovat")
endif()

# ---------------------------------------------------------------------------
# modem_tap (Linux + macOS, síťový TUN/TAP mostík mezi virtuálním rozhraním
# a akustickým modemem). acoustic_link.cpp je už součástí modemcore, tady
# se přidává jen tap_device.cpp (platformové otevření rozhraní) a main.
# Na Windows zatím záměrně chybí — tap_device.cpp používá POSIX rozhraní
# (/dev/net/tun na Linuxu, utun sockety na macOS), na Windows by ekvivalent
# šel jen přes Wintun (viz TODO v src/link/tap_device.cpp).
# ---------------------------------------------------------------------------
if((CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
   AND EXISTS ${CMAKE_SOURCE_DIR}/src/net/main_tap.cpp)
    add_executable(modem_tap
        src/net/main_tap.cpp
        src/link/tap_device.cpp
    )
    target_link_libraries(modem_tap PRIVATE modemaudio)
else()
    message(STATUS "modem_tap se nebude sestavovat (nepodporovaná platforma, nebo src/net/main_tap.cpp neexistuje)")
endif()
