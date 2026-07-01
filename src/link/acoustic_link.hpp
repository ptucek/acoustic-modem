#pragma once
// Poloduplexní MAC vrstva nad akustickým kanálem (pro modem_tap).
//
// Oba stroje sdílejí jedno médium (vzduch) a neumí současně vysílat
// a přijímat (mikrofon slyší vlastní reproduktor). Řešení à la klasický
// Ethernet: CSMA — naslouchej před vysíláním, při obsazeném kanálu
// náhodný backoff, během vlastního vysílání se příjem zahazuje.
// Best-effort: ztráty řeší vyšší vrstvy (ICMP/TCP), jako u Ethernetu.
//
// Vrstva je schválně nezávislá na zvukové kartě — vzorky tečou přes
// injektované funkce, takže test_link ji prožene simulovaným kanálem
// bez hardwaru.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "core/config.hpp"
#include "modem/modulator.hpp"
#include "protocol/frame_receiver.hpp"

namespace am {

class AcousticLink {
public:
    struct Params {
        ModemConfig cfg;
        const ModemScheme* scheme = nullptr;
        double backoff_min_s = 0.3; // náhodný odklad při obsazeném kanálu
        double backoff_max_s = 1.5;
        // Krátký náhodný rozptyl startu PŘED každým vysíláním (à la WiFi):
        // dvě stanice, které se rozhodnou vysílat současně, se rozhodí
        // a pozdější už uslyší warm-up tón té první → místo kolize backoff.
        double dither_min_s = 0.02;
        double dither_max_s = 0.30;
        double guard_s = 0.15;      // ticho po vlastním vysílání (dozvuk)
        float  busy_input_rms = 0.02f; // energie vstupu ⇒ kanál obsazený
        uint32_t seed = 0;          // 0 = náhodný (různý backoff obou stran!)
    };

    // pop: přečti dostupné přijaté vzorky (vrací počet).
    // push: předej vzorky k odvysílání (celý rámec najednou).
    // txPending: kolik vzorků z posledního push ještě NEodehrálo
    //            (0 = odvysíláno) — u audia stav tx ringu, v testu čítač.
    using PopFn = std::function<size_t(std::span<float>)>;
    using PushFn = std::function<void(std::span<const float>)>;
    using TxPendingFn = std::function<size_t()>;

    // false = neplatné parametry (chybějící schéma)
    bool configure(const Params& p, PopFn pop, PushFn push, TxPendingFn pending);

    // false = paket odmítnut (> kMaxPayload) — tichý ořez by poslal
    // „validní" poškozený paket
    bool sendPacket(std::span<const uint8_t> data);
    std::optional<std::vector<uint8_t>> receivePacket(); // dekódované CRC-OK pakety

    // Jádro: volat periodicky (~10 ms). `now_s` = monotónní čas volajícího;
    // v testech se dá krokovat uměle.
    void tick(double now_s);

    enum class MacState { Idle, Backoff, Transmitting, Guard };
    MacState macState() const { return mac_; }
    size_t txQueueDepth() const { return tx_queue_.size(); }

    // statistiky pro CLI výpis / zprávu
    struct Stats {
        uint64_t tx_frames = 0, rx_ok = 0, rx_crc_fail = 0, backoffs = 0;
        uint64_t forced_tx = 0; // vysílání vynucená po vyčerpání backoffů
    };
    Stats stats() const { return stats_; }

private:
    bool channelBusy();
    double drawBackoff(double now_s);

    Params p_;
    PopFn pop_;
    PushFn push_;
    TxPendingFn tx_pending_;

    FrameReceiver rx_;
    std::deque<std::vector<uint8_t>> tx_queue_;
    std::deque<std::vector<uint8_t>> rx_queue_;

    MacState mac_ = MacState::Idle;
    double state_until_ = 0.0;      // konec backoff/guard intervalu
    int consecutive_backoffs_ = 0;  // ochrana proti vyhladovění TX fronty
    float input_rms_ = 0.f;    // klouzavá energie vstupu (carrier sense)
    std::mt19937 rng_;
    Stats stats_;
    std::vector<float> chunk_;
};

} // namespace am
