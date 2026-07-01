// modem_tap — síťový mostík mezi virtuálním TUN/TAP rozhraním a akustickým
// modemem. Umožňuje poslat přes zvukový kanál libovolný IP (nebo Ethernet)
// provoz — typicky s velmi malým MTU, viz --help.
//
// Architektura: AudioEngine plní/vyprazdňuje ring buffery v real-time
// callbacku, AcousticLink (CSMA MAC) nad nimi řeší poloduplexní přístup ke
// kanálu a rámcování/CRC, hlavní vlákno jen přelévá pakety mezi tap
// rozhraním (poll/read/write) a AcousticLink frontami.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>

#include "audio/audio_engine.hpp"
#include "core/config.hpp"
#include "core/spsc_ring.hpp"
#include "link/acoustic_link.hpp"
#include "link/tap_device.hpp"
#include "modem/modulator.hpp"

namespace {

using am::ModemConfig;

void printUsage() {
    std::cout <<
        "modem_tap — síťový mostík mezi virtuálním rozhraním (TUN/TAP)\n"
        "a akustickým modemem. Vyžaduje root (sudo) — otevírá /dev/net/tun\n"
        "(Linux) nebo utun control socket (macOS).\n"
        "\n"
        "Použití:\n"
        "  modem_tap [--mode tun|tap] [--ifname jmeno] [--scheme 16-FSK]\n"
        "            [--baud 31.25] [--f0 1200 --f1 2200] [--amp 0.5]\n"
        "            [--capture N] [--playback N]\n"
        "\n"
        "Přepínače:\n"
        "  --mode tun|tap   typ rozhraní (výchozí: tun; tap jen na Linuxu)\n"
        "  --ifname jmeno   požadované jméno rozhraní (výchozí: am0 na Linuxu,\n"
        "                   na macOS jméno přidělí systém, např. utun4)\n"
        "  --scheme name    modulační schéma (výchozí: 16-FSK)\n"
        "  --baud x         baud rate (symbolů/s)\n"
        "  --f0 x --f1 x    kmitočty pro 2-FSK/OOK/DBPSK nosnou\n"
        "  --amp x          amplituda vysílání 0..1 (výchozí 0.5)\n"
        "  --capture N      index nahrávacího (mikrofon) zařízení, -1 = výchozí\n"
        "  --playback N     index přehrávacího (reproduktor) zařízení, -1 = výchozí\n"
        "  --help           vypíše tuto nápovědu\n"
        "\n"
        "Po spuštění nastav IP adresu a MTU na vytvořeném rozhraní (viz\n"
        "vypsaný příkaz při startu) a spusť stejný program na druhé stanici.\n"
        "Ctrl+C ukončí program.\n";
}

// ---------------------------------------------------------------------------
// Ručně psaný parser argumentů (stejný styl jako modem_cli — malý projekt,
// nemá smysl kvůli pár přepínačům táhnout závislost).
// ---------------------------------------------------------------------------
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) items_.emplace_back(argv[i]);
        used_.assign(items_.size(), false);
    }

    std::optional<std::string> get(const std::string& key) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i] == key) {
                if (i + 1 >= items_.size()) {
                    throw std::runtime_error("chybí hodnota za přepínačem " + key);
                }
                used_[i] = true;
                used_[i + 1] = true;
                return items_[i + 1];
            }
        }
        return std::nullopt;
    }

    std::optional<double> getDouble(const std::string& key) {
        auto v = get(key);
        if (!v) return std::nullopt;
        return std::stod(*v);
    }

    std::optional<int> getInt(const std::string& key) {
        auto v = get(key);
        if (!v) return std::nullopt;
        return std::stoi(*v);
    }

    bool hasFlag(const std::string& key) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i] == key) {
                used_[i] = true;
                return true;
            }
        }
        return false;
    }

    std::optional<std::string> firstUnused(size_t skip_leading) const {
        for (size_t i = skip_leading; i < items_.size(); ++i) {
            if (!used_[i]) return items_[i];
        }
        return std::nullopt;
    }

private:
    std::vector<std::string> items_;
    std::vector<bool> used_;
};

std::atomic<bool> g_interrupted{false};

void onSigint(int) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

const char* macStateName(am::AcousticLink::MacState s) {
    switch (s) {
        case am::AcousticLink::MacState::Idle:         return "idle";
        case am::AcousticLink::MacState::Backoff:      return "backoff";
        case am::AcousticLink::MacState::Transmitting: return "vysilani";
        case am::AcousticLink::MacState::Guard:        return "guard";
    }
    return "?";
}

void printSetupHint(const std::string& ifname) {
#if defined(__linux__)
    std::cout << "Nastav rozhraní příkazy (v jiném terminálu, jako root):\n"
               << "  sudo ip addr add 10.44.0.1/24 dev " << ifname << "\n"
               << "  sudo ip link set " << ifname << " up\n"
               << "  sudo ip link set " << ifname << " mtu 200\n";
#elif defined(__APPLE__)
    std::cout << "Nastav rozhraní příkazem (v jiném terminálu, jako root):\n"
               << "  sudo ifconfig " << ifname << " 10.44.0.1 10.44.0.2 mtu 200 up\n";
#else
    (void)ifname;
#endif
    std::cout << "(na druhé stanici použij druhou adresu z /24, např. 10.44.0.2)\n";
}

} // namespace

int main(int argc, char** argv) {
    Args args(argc - 1, argv + 1);

    if (args.hasFlag("--help") || args.hasFlag("-h")) {
        printUsage();
        return 0;
    }

    try {
        std::string mode_str = args.get("--mode").value_or("tun");
        am::NetifMode mode;
        if (mode_str == "tun") {
            mode = am::NetifMode::Tun;
        } else if (mode_str == "tap") {
            mode = am::NetifMode::Tap;
        } else {
            std::cerr << "modem_tap: neznámý --mode \"" << mode_str
                       << "\" (očekáváno tun nebo tap)\n";
            return 1;
        }

        std::string ifname_hint = args.get("--ifname").value_or("");

        ModemConfig cfg;
        if (auto v = args.getDouble("--baud")) cfg.baud = *v;
        if (auto v = args.getDouble("--f0")) cfg.f0 = *v;
        if (auto v = args.getDouble("--f1")) cfg.f1 = *v;
        if (auto v = args.getDouble("--amp")) cfg.amplitude = *v;

        std::string scheme_name = args.get("--scheme").value_or("16-FSK");
        const am::ModemScheme* scheme = am::findScheme(scheme_name.c_str());
        if (!scheme) {
            std::cerr << "modem_tap: neznámé schéma modulace \"" << scheme_name << "\"\n";
            return 1;
        }

        int capture_index = args.getInt("--capture").value_or(-1);
        int playback_index = args.getInt("--playback").value_or(-1);

        if (auto extra = args.firstUnused(0)) {
            std::cerr << "modem_tap: neznámý argument: " << *extra << "\n";
            printUsage();
            return 1;
        }

        // --- otevři virtuální síťové rozhraní ---------------------------------
        am::TunTapDevice tap;
        if (!tap.open(mode, ifname_hint)) {
            std::cerr << "modem_tap: nepodařilo se otevřít " << mode_str
                       << " rozhraní (spouštíš pod sudo?)\n";
            return 1;
        }

        std::cout << "modem_tap: rozhraní " << tap.name()
                   << " (" << mode_str << ") otevřeno, schema=" << scheme->name << "\n";
        printSetupHint(tap.name());
        std::cout << "Doporučené MTU je 200 B (payload rámce je omezen na "
                   << am::kMaxPayload << " B). Ctrl+C ukončí.\n";

        // --- zvuk + MAC vrstva --------------------------------------------------
        am::SpscRing<float> rx_ring(1u << 20);
        am::SpscRing<float> tx_ring(1u << 22);

        am::AudioEngine engine;
        if (!engine.start(capture_index, playback_index, cfg.sample_rate, rx_ring, tx_ring)) {
            std::cerr << "modem_tap: nepodařilo se spustit zvukové zařízení\n";
            return 1;
        }

        am::AcousticLink link;
        am::AcousticLink::Params params;
        params.cfg = cfg;
        params.scheme = scheme;
        const bool link_ok = link.configure(
            params,
            /*pop*/ [&rx_ring](std::span<float> out) { return rx_ring.pop(out); },
            /*push*/ [&tx_ring](std::span<const float> in) { tx_ring.push(in); },
            /*txPending*/ [&tx_ring] { return tx_ring.sizeApprox(); });
        if (!link_ok) {
            std::cerr << "modem_tap: nepodařilo se nakonfigurovat spojení "
                         "(neplatné schéma modulace nebo parametry)\n";
            engine.stop();
            return 1;
        }

        std::signal(SIGINT, onSigint);
        g_interrupted.store(false, std::memory_order_relaxed);

        // --- hlavní smyčka --------------------------------------------------
        std::vector<uint8_t> pkt_buf(2048);
        const auto t0 = std::chrono::steady_clock::now();
        auto last_stats = t0;

        while (!g_interrupted.load(std::memory_order_relaxed)) {
            struct pollfd pfd;
            pfd.fd = tap.fd();
            pfd.events = POLLIN;
            pfd.revents = 0;

            int rc = ::poll(&pfd, 1, 10 /* ms */);
            if (rc > 0 && (pfd.revents & POLLIN)) {
                long len = tap.readPacket(pkt_buf);
                if (len > 0) {
                    if (size_t(len) > size_t(am::kMaxPayload)) {
                        std::cerr << "modem_tap: varování — paket " << len
                                   << " B přesahuje limit " << am::kMaxPayload
                                   << " B, zahazuji (doporučuji nastavit MTU 200)\n";
                    } else if (!link.sendPacket(std::span<const uint8_t>(pkt_buf.data(), size_t(len)))) {
                        std::cerr << "modem_tap: varování — paket (" << len
                                   << " B) odmítnut linkovou vrstvou, zahazuji\n";
                    }
                }
            }

            const double now_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            link.tick(now_s);

            while (auto p = link.receivePacket()) {
                tap.writePacket(*p);
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_stats).count() >= 10.0) {
                auto s = link.stats();
                std::cout << "stav=" << macStateName(link.macState())
                           << " tx_frames=" << s.tx_frames
                           << " rx_ok=" << s.rx_ok
                           << " rx_crc_fail=" << s.rx_crc_fail
                           << " backoffs=" << s.backoffs
                           << " tx_fronta=" << link.txQueueDepth() << "\n";
                last_stats = now;
            }
        }

        std::signal(SIGINT, SIG_DFL);
        engine.stop();
        tap.close();
        std::cout << "\nmodem_tap: ukončeno\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "modem_tap: chyba: " << e.what() << "\n";
        return 1;
    }
}
