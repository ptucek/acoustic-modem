// Příkazová řádka akustického modemu. Ručně psaný parser argumentů —
// projekt je malý, nemá smysl kvůli pár přepínačům přidávat závislost.
//
// Podpříkazy:
//   modem_cli tx       --text "..." --out tx.wav [...]
//   modem_cli rx       --in rx.wav [...]
//   modem_cli chansim  --in tx.wav --out ch.wav [...]
//   modem_cli send     --text "..." [...]           (živý zvuk)
//   modem_cli listen   [...]                        (živý zvuk)
//   modem_cli devices                               (výpis zvukových zařízení)

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.hpp"
#include "core/config.hpp"
#include "core/prbs.hpp"
#include "core/spsc_ring.hpp"
#include "core/wav_io.hpp"
#include "dsp/channel_sim.hpp"
#include "modem/modulator.hpp"
#include "protocol/frame_receiver.hpp"
#include "protocol/framer.hpp"

namespace {

using am::ModemConfig;

void printUsage() {
    std::cout <<
        "modem_cli — akustický modem, ovládání z příkazové řádky\n"
        "\n"
        "Použití:\n"
        "  modem_cli tx --text \"zpráva\" --out tx.wav [--scheme 2-FSK] [--baud 31.25]\n"
        "               [--f0 1200 --f1 2200]\n"
        "      Zakóduje text do WAV souboru (vzniklý zvukový rámec).\n"
        "\n"
        "  modem_cli rx --in rx.wav [--scheme 2-FSK] [--baud 31.25] [--f0 1200 --f1 2200]\n"
        "      Dekóduje WAV soubor, vypíše nalezené rámce a jejich obsah.\n"
        "\n"
        "  modem_cli chansim --in tx.wav --out ch.wav [--snr 15] [--drift 0]\n"
        "                    [--gain 1.0] [--echo-delay 0 --echo-gain 0] [--seed 1]\n"
        "      Simuluje akustický kanál (šum, drift hodin, útlum, echo).\n"
        "\n"
        "  modem_cli send --text \"zpráva\" [--scheme 2-FSK] [--baud 31.25]\n"
        "                 [--f0 1200 --f1 2200] [--amp 0.5] [--device N]\n"
        "      Odešle text živě přes zvukovou kartu (reproduktor).\n"
        "\n"
        "  modem_cli listen [--scheme 2-FSK] [--baud 31.25] [--f0 1200 --f1 2200]\n"
        "                   [--device N] [--seconds S]\n"
        "      Naslouchá mikrofonu a vypisuje přijaté rámce. Ctrl+C ukončí.\n"
        "\n"
        "  modem_cli devices\n"
        "      Vypíše dostupná zvuková zařízení (přehrávání i nahrávání).\n"
        "      Přepínač --list-devices funguje i u send/listen samostatně.\n"
        "\n"
        "  modem_cli --help\n"
        "      Vypíše tuto nápovědu.\n";
}

// Jednoduchý přístup k argumentům: seznam řetězců + pomocné funkce hledající
// hodnotu za daným klíčem.
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) items_.emplace_back(argv[i]);
        used_.assign(items_.size(), false);
    }

    // Vrátí hodnotu přepínače "--key" (posune index o 2), nebo nullopt.
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
        size_t pos = 0;
        double result;
        try {
            result = std::stod(*v, &pos);
        } catch (const std::exception&) {
            throw std::runtime_error("neplatná číselná hodnota u přepínače " + key +
                                     ": \"" + *v + "\"");
        }
        // stod tiše parsuje jen prefix ("31,25" -> 31) — zbytek řetězce musí
        // být prázdný, jinak jde o chybu (typicky česká desetinná čárka).
        if (pos != v->size()) {
            throw std::runtime_error("neplatná číselná hodnota u přepínače " + key +
                                     ": \"" + *v + "\" (použij tečku jako desetinný oddělovač)");
        }
        return result;
    }

    std::optional<int> getInt(const std::string& key) {
        auto v = get(key);
        if (!v) return std::nullopt;
        size_t pos = 0;
        int result;
        try {
            result = std::stoi(*v, &pos);
        } catch (const std::exception&) {
            throw std::runtime_error("neplatná celočíselná hodnota u přepínače " + key +
                                     ": \"" + *v + "\"");
        }
        if (pos != v->size()) {
            throw std::runtime_error("neplatná celočíselná hodnota u přepínače " + key +
                                     ": \"" + *v + "\"");
        }
        return result;
    }

    // Bezhodnotový přepínač (např. --list-devices) — vrací true, pokud je
    // přítomen, a označí jej jako použitý.
    bool hasFlag(const std::string& key) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i] == key) {
                used_[i] = true;
                return true;
            }
        }
        return false;
    }

    // Vypíše první nepoužitý (neznámý) argument, pokud existuje.
    std::optional<std::string> firstUnused(size_t skip_leading) const {
        for (size_t i = skip_leading; i < items_.size(); ++i) {
            if (!used_[i]) return items_[i];
        }
        return std::nullopt;
    }

    size_t count() const { return items_.size(); }

private:
    std::vector<std::string> items_;
    std::vector<bool> used_;
};

// Ověří rozsahy číselných hodnot v cfg. Vypíše česky pojmenovanou chybu
// (s jménem přepínače) a vrátí false, pokud je něco mimo rozumný rozsah —
// bez toho např. --baud 0 spadne na dělení nulou v samplesPerSymbol()
// (cast +inf na int je UB) a záporná --f0/--f1/--amp vedou k nesmyslnému
// signálu bez jakékoli diagnostiky.
bool validateConfig(const ModemConfig& cfg) {
    if (cfg.sample_rate < 8000 || cfg.sample_rate > 192000) {
        std::cerr << "chyba: --sample-rate musí být v rozsahu 8000..192000 Hz (zadáno "
                   << cfg.sample_rate << ")\n";
        return false;
    }
    if (!(cfg.baud > 0.0) || cfg.baud > double(cfg.sample_rate) / 16.0) {
        std::cerr << "chyba: --baud musí být kladné a nejvýše sample_rate/16 = "
                   << (double(cfg.sample_rate) / 16.0) << " (zadáno " << cfg.baud << ")\n";
        return false;
    }
    const double nyquist = double(cfg.sample_rate) / 2.0;
    if (!(cfg.f0 > 0.0) || cfg.f0 >= nyquist) {
        std::cerr << "chyba: --f0 musí být v rozsahu (0, " << nyquist
                   << ") Hz (zadáno " << cfg.f0 << ")\n";
        return false;
    }
    if (!(cfg.f1 > 0.0) || cfg.f1 >= nyquist) {
        std::cerr << "chyba: --f1 musí být v rozsahu (0, " << nyquist
                   << ") Hz (zadáno " << cfg.f1 << ")\n";
        return false;
    }
    if (!(cfg.amplitude > 0.0) || cfg.amplitude > 1.0) {
        std::cerr << "chyba: --amp musí být v rozsahu (0, 1] (zadáno "
                   << cfg.amplitude << ")\n";
        return false;
    }
    return true;
}

// Naplní ModemConfig ze společných přepínačů (--scheme se řeší zvlášť,
// protože ovlivňuje výběr schématu, ne jen čísla). --sample-rate se musí
// aplikovat před --baud/--f0/--f1, protože jejich validace na něm závisí.
void applyCommonConfig(Args& args, ModemConfig& cfg) {
    if (auto v = args.getInt("--sample-rate")) cfg.sample_rate = *v;
    if (auto v = args.getDouble("--baud")) cfg.baud = *v;
    if (auto v = args.getDouble("--f0")) cfg.f0 = *v;
    if (auto v = args.getDouble("--f1")) cfg.f1 = *v;
}

std::string schemeNameOrDefault(Args& args) {
    if (auto v = args.get("--scheme")) return *v;
    return "2-FSK";
}

// Ověří, že všechny argumenty byly rozpoznány (spotřebovány některým z
// get/getInt/getDouble/hasFlag volání výše). MUSÍ se volat před jakoukoliv
// pozorovatelnou akcí příkazu (zápis souboru, spuštění zvuku, vysílání) —
// jinak překlep jako --f00 tiše proběhne s výchozí hodnotou a chyba se
// vypíše, až když je akce (např. tx do WAV) nevratně hotová.
bool checkNoUnknownArgs(Args& args, const char* cmd_name) {
    if (auto extra = args.firstUnused(0)) {
        std::cerr << cmd_name << ": neznámý argument: " << *extra << "\n";
        printUsage();
        return false;
    }
    return true;
}

// Rozhodne, jestli má smysl vypsat payload jako text (všechny bajty
// tisknutelné ASCII nebo běžné UTF-8 řídící znaky) — jinak hexdump.
bool looksLikeText(const std::vector<uint8_t>& data) {
    if (data.empty()) return true;
    for (uint8_t b : data) {
        if (b == '\n' || b == '\r' || b == '\t') continue;
        if (b < 0x20 || b == 0x7F) return false;
        // Bajty >= 0x80 mohou být platné UTF-8 pokračování — necháme projít,
        // finální rozhodnutí je jen heuristika pro pohodlný výpis.
    }
    return true;
}

void printHex(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        std::printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0) std::printf("\n");
    }
    if (data.size() % 16 != 0) std::printf("\n");
}

// ---------------------------------------------------------------------------
// tx
// ---------------------------------------------------------------------------
int cmdTx(Args& args) {
    auto text = args.get("--text");
    auto out = args.get("--out");
    if (!text || !out) {
        std::cerr << "tx: vyžaduje --text a --out\n";
        printUsage();
        return 1;
    }

    ModemConfig cfg;
    applyCommonConfig(args, cfg);
    if (!validateConfig(cfg)) return 1;

    std::string scheme_name = schemeNameOrDefault(args);
    const am::ModemScheme* scheme = am::findScheme(scheme_name.c_str());
    if (!scheme) {
        std::cerr << "tx: neznámé schéma modulace \"" << scheme_name << "\"\n";
        return 1;
    }
    if (!checkNoUnknownArgs(args, "tx")) return 1;

    auto mod = scheme->makeMod();
    mod->configure(cfg);

    std::vector<uint8_t> payload(text->begin(), text->end());
    if (payload.size() > am::kMaxPayload) {
        std::cerr << "tx: text je příliš dlouhý (" << payload.size()
                   << " B, max " << am::kMaxPayload << " B)\n";
        return 1;
    }

    std::vector<float> samples =
        am::Framer::buildFrame(payload, *mod, cfg, am::kPayloadText);

    if (!am::writeWav(*out, samples, cfg.sample_rate)) {
        std::cerr << "tx: nepodařilo se zapsat " << *out << "\n";
        return 1;
    }

    double duration_s = double(samples.size()) / cfg.sample_rate;
    std::cout << "tx: schema=" << scheme->name
              << " payload=" << payload.size() << " B"
              << " trvani=" << duration_s << " s"
              << " -> " << *out << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// rx
// ---------------------------------------------------------------------------
int cmdRx(Args& args) {
    auto in = args.get("--in");
    if (!in) {
        std::cerr << "rx: vyžaduje --in\n";
        printUsage();
        return 1;
    }

    ModemConfig cfg;
    applyCommonConfig(args, cfg);
    if (!validateConfig(cfg)) return 1;

    std::string scheme_name = schemeNameOrDefault(args);
    const am::ModemScheme* scheme = am::findScheme(scheme_name.c_str());
    if (!scheme) {
        std::cerr << "rx: neznámé schéma modulace \"" << scheme_name << "\"\n";
        return 1;
    }
    if (!checkNoUnknownArgs(args, "rx")) return 1;

    std::vector<float> samples;
    int file_sample_rate = 0;
    if (!am::readWav(*in, samples, file_sample_rate)) {
        std::cerr << "rx: nepodařilo se přečíst " << *in << "\n";
        return 1;
    }
    // Mismatch mezi vzorkovacím kmitočtem souboru a konfigurací není jen
    // kosmetika — FrameReceiver by dekódoval špatnou rychlostí a tiše
    // nenašel žádný rámec bez vysvětlení příčiny. --sample-rate dovoluje
    // override, ale jen pokud po něm kmitočty skutečně sedí.
    if (file_sample_rate != cfg.sample_rate) {
        std::cerr << "rx: chyba — vzorkovací kmitočet souboru ("
                   << file_sample_rate << " Hz) neodpovídá konfiguraci ("
                   << cfg.sample_rate << " Hz); přidej --sample-rate "
                   << file_sample_rate << "\n";
        return 1;
    }

    am::FrameReceiver rx;
    rx.configure(cfg, *scheme);

    constexpr size_t kChunk = 4096; // simuluje streamování ze zvukové karty
    int frame_count = 0;
    bool any_ok = false;

    for (size_t pos = 0; pos < samples.size(); pos += kChunk) {
        size_t n = std::min(kChunk, samples.size() - pos);
        rx.pushSamples(std::span<const float>(samples.data() + pos, n));

        while (auto result = rx.poll()) {
            ++frame_count;
            any_ok = any_ok || result->crc_ok;

            std::cout << "--- ramec #" << frame_count << " ---\n";
            std::cout << "CRC: " << (result->crc_ok ? "OK" : "FAIL") << "\n";
            std::cout << "typ payloadu: " << int(result->payload_type) << "\n";
            std::cout << "prumerne SNR: " << result->mean_snr_db << " dB\n";
            std::cout << "delka payloadu: " << result->payload.size() << " B\n";

            if (looksLikeText(result->payload)) {
                std::cout << "text: "
                          << std::string(result->payload.begin(), result->payload.end())
                          << "\n";
            } else {
                std::cout << "hex:\n";
                printHex(result->payload);
            }
        }
    }

    std::cout << "\ncelkem nalezeno ramcu: " << frame_count << "\n";
    return any_ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// chansim
// ---------------------------------------------------------------------------
int cmdChansim(Args& args) {
    auto in = args.get("--in");
    auto out = args.get("--out");
    if (!in || !out) {
        std::cerr << "chansim: vyžaduje --in a --out\n";
        printUsage();
        return 1;
    }

    am::ChannelParams params;
    if (auto v = args.getDouble("--snr")) params.snr_db = *v;
    if (auto v = args.getDouble("--drift")) params.drift_ppm = *v;
    if (auto v = args.getDouble("--gain")) params.gain = *v;
    if (auto v = args.getDouble("--echo-delay")) params.echo_delay_s = *v;
    if (auto v = args.getDouble("--echo-gain")) params.echo_gain = *v;
    if (auto v = args.getInt("--seed")) params.seed = static_cast<uint32_t>(*v);
    if (!checkNoUnknownArgs(args, "chansim")) return 1;

    std::vector<float> samples;
    int sample_rate = 0;
    if (!am::readWav(*in, samples, sample_rate)) {
        std::cerr << "chansim: nepodařilo se přečíst " << *in << "\n";
        return 1;
    }

    std::vector<float> out_samples = am::simulateChannel(samples, params, sample_rate);

    if (!am::writeWav(*out, out_samples, sample_rate)) {
        std::cerr << "chansim: nepodařilo se zapsat " << *out << "\n";
        return 1;
    }

    std::cout << "chansim: snr=" << params.snr_db << " dB"
              << " drift=" << params.drift_ppm << " ppm"
              << " gain=" << params.gain
              << " echo_delay=" << params.echo_delay_s << " s"
              << " echo_gain=" << params.echo_gain
              << " seed=" << params.seed
              << " -> " << *out << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// společné pomůcky pro živý zvuk (send/listen/devices)
// ---------------------------------------------------------------------------

// Globální atomický příznak pro Ctrl+C — nastavuje jej signal handler,
// listen jej pravidelně kontroluje ve své smyčce.
std::atomic<bool> g_interrupted{false};

void onSigint(int) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

void printDevices(am::AudioEngine& engine) {
    auto devices = engine.enumerate();
    std::cout << "prehravaci zarizeni (playback):\n";
    for (const auto& d : devices) {
        if (!d.is_capture) {
            std::cout << "  [" << d.index << "] " << d.name << "\n";
        }
    }
    std::cout << "nahravaci zarizeni (capture):\n";
    for (const auto& d : devices) {
        if (d.is_capture) {
            std::cout << "  [" << d.index << "] " << d.name << "\n";
        }
    }
}

// Textová úroveň signálu 0..1 jako pruh znaků — pro stavový řádek listen.
std::string levelBar(float level, int width = 20) {
    int filled = static_cast<int>(std::clamp(level, 0.f, 1.f) * width + 0.5f);
    std::string bar(static_cast<size_t>(width), '-');
    for (int i = 0; i < filled && i < width; ++i) bar[static_cast<size_t>(i)] = '#';
    return bar;
}

const char* stateName(am::FrameReceiver::State s) {
    switch (s) {
        case am::FrameReceiver::State::SearchPreamble: return "hledani preambule";
        case am::FrameReceiver::State::Sync:            return "sync";
        case am::FrameReceiver::State::Header:          return "hlavicka";
        case am::FrameReceiver::State::Payload:         return "payload";
        case am::FrameReceiver::State::Crc:             return "crc";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// send — živé vysílání přes zvukovou kartu
// ---------------------------------------------------------------------------
int cmdSend(Args& args) {
    if (args.hasFlag("--list-devices")) {
        am::AudioEngine engine;
        printDevices(engine);
        return 0;
    }

    auto text = args.get("--text");
    std::optional<int> prbs_count = args.getInt("--prbs");

    ModemConfig cfg;
    applyCommonConfig(args, cfg);
    if (auto v = args.getDouble("--amp")) cfg.amplitude = *v;
    std::optional<int> device_index = args.getInt("--device");
    if (!validateConfig(cfg)) return 1;

    if (!text && !prbs_count) {
        std::cerr << "send: vyžaduje --text, nebo --prbs <počet rámců>\n";
        printUsage();
        return 1;
    }

    std::string scheme_name = schemeNameOrDefault(args);
    const am::ModemScheme* scheme = am::findScheme(scheme_name.c_str());
    if (!scheme) {
        std::cerr << "send: neznámé schéma modulace \"" << scheme_name << "\"\n";
        return 1;
    }
    if (!checkNoUnknownArgs(args, "send")) return 1;

    auto mod = scheme->makeMod();
    mod->configure(cfg);

    std::vector<float> samples;
    if (prbs_count) {
        // Měřicí režim: N rámců se známou PRBS-15 sekvencí (BER na přijímači).
        // Rámce jsou identické (pevný seed) — vygeneruje se jeden a N× přehraje.
        const auto one =
            am::Framer::buildFrame(am::Prbs15().generate(128), *mod, cfg,
                                   am::kPayloadPrbs);
        for (int i = 0; i < *prbs_count; ++i)
            samples.insert(samples.end(), one.begin(), one.end());
        std::cerr << "send: PRBS test, " << *prbs_count << " ramcu po 128 B\n";
    } else {
        std::vector<uint8_t> payload(text->begin(), text->end());
        if (payload.size() > am::kMaxPayload) {
            std::cerr << "send: text je příliš dlouhý (" << payload.size()
                       << " B, max " << am::kMaxPayload << " B)\n";
            return 1;
        }
        samples = am::Framer::buildFrame(payload, *mod, cfg, am::kPayloadText);
    }

    am::SpscRing<float> tx_ring(1u << 22);
    if (samples.size() > tx_ring.capacity()) {
        std::cerr << "send: příliš dlouhé vysílání pro tx buffer ("
                   << samples.size() << " vzorků) — sniž --prbs nebo zvyš baud\n";
        return 1;
    }
    am::SpscRing<float> rx_ring(1u << 20); // nepoužitý pro RX, ale start() jej vyžaduje

    size_t pushed = tx_ring.push(std::span<const float>(samples));
    if (pushed != samples.size()) {
        std::cerr << "send: varování — tx ring nepobral celý rámec ("
                   << pushed << "/" << samples.size() << " vzorků)\n";
    }

    am::AudioEngine engine;
    int playback_index = device_index.value_or(-1);
    // Capture se vůbec neotevírá — čisté vysílání nepotřebuje mikrofon a na
    // macOS by si jinak vyžádalo TCC oprávnění, i když se nic nenahrává.
    if (!engine.start(am::kNoDevice, playback_index, cfg.sample_rate, rx_ring, tx_ring)) {
        std::cerr << "send: nepodařilo se spustit zvukové zařízení "
                     "(zkontroluj --device, případně spusť \"modem_cli devices\")\n";
        return 1;
    }

    std::cerr << "send: schema=" << scheme->name
               << " vzorku=" << samples.size() << "\n";

    // Detekce zaseknutí: pokud se tx_ring přestane vyprazdňovat (odpojené
    // zařízení, ovladač zamrzlý apod.), smyčka bez toho točí navěky. Sledujeme
    // poslední pokles velikosti fronty a stav enginu.
    const size_t total = samples.size();
    constexpr auto kStallTimeout = std::chrono::seconds(5);
    size_t last_remaining = tx_ring.sizeApprox();
    auto last_progress_time = std::chrono::steady_clock::now();
    bool stalled = false;

    while (true) {
        size_t remaining = tx_ring.sizeApprox();
        double pct = total > 0 ? 100.0 * double(total - remaining) / double(total) : 100.0;
        std::cerr << "\rsend: postup " << int(pct) << " %   " << std::flush;
        if (remaining == 0) break;

        if (!engine.running()) {
            std::cerr << "\nsend: chyba — zvukové zařízení přestalo běžet uprostřed vysílání\n";
            stalled = true;
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (remaining < last_remaining) {
            last_remaining = remaining;
            last_progress_time = now;
        } else if (now - last_progress_time >= kStallTimeout) {
            std::cerr << "\nsend: chyba — vysílání se zaseklo (žádný postup "
                       << std::chrono::duration_cast<std::chrono::seconds>(kStallTimeout).count()
                       << " s), zkontroluj zvukové zařízení\n";
            stalled = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (stalled) {
        engine.stop();
        return 1;
    }

    // Margin, ať dohraje i poslední zaplněný hardwarový buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cerr << "\rsend: postup 100 %   \n";

    engine.stop();
    std::cerr << "send: hotovo\n";
    return 0;
}

// ---------------------------------------------------------------------------
// listen — živý příjem přes zvukovou kartu
// ---------------------------------------------------------------------------
int cmdListen(Args& args) {
    if (args.hasFlag("--list-devices")) {
        am::AudioEngine engine;
        printDevices(engine);
        return 0;
    }

    ModemConfig cfg;
    applyCommonConfig(args, cfg);
    std::optional<int> device_index = args.getInt("--device");
    std::optional<double> seconds = args.getDouble("--seconds");
    if (!validateConfig(cfg)) return 1;

    std::string scheme_name = schemeNameOrDefault(args);
    const am::ModemScheme* scheme = am::findScheme(scheme_name.c_str());
    if (!scheme) {
        std::cerr << "listen: neznámé schéma modulace \"" << scheme_name << "\"\n";
        return 1;
    }

    // --record <wav>: ukládej surový signál z mikrofonu pro offline
    // analýzu (diagnostika chyb, které se dějí jen přes vzduch). Musí se
    // rozpoznat před checkNoUnknownArgs, jinak by se hlásil jako neznámý.
    std::optional<std::string> record_path = args.get("--record");

    if (!checkNoUnknownArgs(args, "listen")) return 1;

    am::FrameReceiver rx;
    rx.configure(cfg, *scheme);

    am::SpscRing<float> rx_ring(1u << 20);
    am::SpscRing<float> tx_ring(1u << 12); // nepoužitý pro RX, ale start() jej vyžaduje

    am::AudioEngine engine;
    int capture_index = device_index.value_or(-1);
    // Playback se vůbec neotevírá — čistý příjem nepotřebuje reproduktor.
    if (!engine.start(capture_index, am::kNoDevice, cfg.sample_rate, rx_ring, tx_ring)) {
        std::cerr << "listen: nepodařilo se spustit zvukové zařízení "
                     "(zkontroluj --device, případně spusť \"modem_cli devices\")\n";
        return 1;
    }

    std::signal(SIGINT, onSigint);
    g_interrupted.store(false, std::memory_order_relaxed);

    std::cerr << "listen: schema=" << scheme->name
               << " naslouchani... (Ctrl+C pro ukonceni)\n";

    std::vector<float> buf(4096);
    int frame_count = 0;
    bool any_ok = false;

    std::vector<float> recording;
    if (record_path) recording.reserve(size_t(cfg.sample_rate) * 120);

    const auto start_time = std::chrono::steady_clock::now();
    auto last_status = start_time;

    while (!g_interrupted.load(std::memory_order_relaxed)) {
        if (seconds) {
            double elapsed = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - start_time)
                                  .count();
            if (elapsed >= *seconds) break;
        }

        size_t got = rx_ring.pop(std::span<float>(buf));
        if (got > 0) {
            if (record_path)
                recording.insert(recording.end(), buf.begin(),
                                 buf.begin() + long(got));
            rx.pushSamples(std::span<const float>(buf.data(), got));

            while (auto result = rx.poll()) {
                ++frame_count;
                any_ok = any_ok || result->crc_ok;

                std::cout << "\n--- ramec #" << frame_count << " ---\n";
                std::cout << "CRC: " << (result->crc_ok ? "OK" : "FAIL") << "\n";
                std::cout << "typ payloadu: " << int(result->payload_type) << "\n";
                std::cout << "prumerne SNR: " << result->mean_snr_db << " dB\n";
                std::cout << "delka payloadu: " << result->payload.size() << " B\n";

                // PRBS rámec: spočti skutečnou bitovou chybovost
                if (result->payload_type == am::kPayloadPrbs &&
                    !result->payload.empty()) {
                    const auto expected =
                        am::Prbs15().generate(result->payload.size());
                    const size_t errs =
                        am::countBitErrors(result->payload, expected);
                    const size_t bits = result->payload.size() * 8;
                    std::cout << "BER: " << errs << "/" << bits << " = "
                              << double(errs) / double(bits) << "\n";
                }

                if (looksLikeText(result->payload)) {
                    std::cout << "text: "
                              << std::string(result->payload.begin(), result->payload.end())
                              << "\n";
                } else {
                    std::cout << "hex:\n";
                    printHex(result->payload);
                }
                std::cout << std::flush;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_status).count() >= 0.5) {
            float peak = engine.inputPeak();
            std::cerr << "\rlevel [" << levelBar(peak) << "] "
                       << "stav: " << stateName(rx.state())
                       << " corr=" << rx.lastCorrPeak()
                       << "        " << std::flush;
            last_status = now;
        }
    }

    engine.stop();
    std::signal(SIGINT, SIG_DFL);

    if (record_path) {
        if (am::writeWav(*record_path, recording, cfg.sample_rate))
            std::cerr << "\nlisten: zaznam ulozen do " << *record_path << " ("
                       << recording.size() / size_t(cfg.sample_rate) << " s)\n";
        else
            std::cerr << "\nlisten: zaznam se nepodarilo ulozit!\n";
    }

    std::cerr << "\nlisten: ukonceno, nalezeno ramcu: " << frame_count << "\n";
    // exit kody: 0 = aspon jeden validni ramec, 2 = ramce jen s vadnym CRC,
    // 1 = zadny ramec (pro skriptovani a mereni)
    if (any_ok) return 0;
    return frame_count > 0 ? 2 : 1;
}

// ---------------------------------------------------------------------------
// devices — výpis zvukových zařízení
// ---------------------------------------------------------------------------
int cmdDevices(Args&) {
    am::AudioEngine engine;
    printDevices(engine);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        printUsage();
        return 0;
    }

    // Args parsuje jen argumenty ZA podpříkazem.
    Args args(argc - 2, argv + 2);

    int rc = 0;
    try {
        if (cmd == "tx") {
            rc = cmdTx(args);
        } else if (cmd == "rx") {
            rc = cmdRx(args);
        } else if (cmd == "chansim") {
            rc = cmdChansim(args);
        } else if (cmd == "send") {
            rc = cmdSend(args);
        } else if (cmd == "listen") {
            rc = cmdListen(args);
        } else if (cmd == "devices") {
            rc = cmdDevices(args);
        } else {
            std::cerr << "neznámý příkaz: " << cmd << "\n";
            printUsage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "chyba: " << e.what() << "\n";
        return 1;
    }

    if (auto extra = args.firstUnused(0)) {
        std::cerr << "neznámý argument: " << *extra << "\n";
        printUsage();
        return 1;
    }

    return rc;
}
