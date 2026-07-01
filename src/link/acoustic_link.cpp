// Poloduplexní CSMA MAC — implementace.
//
// Stavový automat:
//   Idle ── fronta neprázdná & kanál volný ──> Transmitting
//   Idle ── fronta neprázdná & kanál obsazen ──> Backoff (náhodný interval)
//   Backoff ── čas vypršel ──> Idle (nový pokus)
//   Transmitting ── tx dohráno ──> Guard (nech dozvuk odeznít)
//   Guard ── čas vypršel ──> Idle
//
// Během Transmitting a Guard se přijaté vzorky ZAHAZUJÍ — mikrofon slyší
// hlavně náš vlastní reproduktor; bez zahazování bychom dekódovali sami sebe.

#include "link/acoustic_link.hpp"

#include <cmath>

#include "protocol/framer.hpp"

namespace am {

namespace {
constexpr size_t kChunk = 4096;
} // namespace

bool AcousticLink::configure(const Params& p, PopFn pop, PushFn push,
                             TxPendingFn pending) {
    if (!p.scheme || !pop || !push || !pending) return false;
    p_ = p;
    pop_ = std::move(pop);
    push_ = std::move(push);
    tx_pending_ = std::move(pending);
    rx_.configure(p.cfg, *p.scheme);
    chunk_.resize(kChunk);
    rng_.seed(p.seed ? p.seed : std::random_device{}());
    mac_ = MacState::Idle;
    consecutive_backoffs_ = 0;
    tx_queue_.clear();
    rx_queue_.clear();
    stats_ = {};
    return true;
}

bool AcousticLink::sendPacket(std::span<const uint8_t> data) {
    if (data.size() > size_t(kMaxPayload)) return false; // MTU vrstvy výš
    tx_queue_.emplace_back(data.begin(), data.end());
    return true;
}

std::optional<std::vector<uint8_t>> AcousticLink::receivePacket() {
    if (rx_queue_.empty()) return std::nullopt;
    auto pkt = std::move(rx_queue_.front());
    rx_queue_.pop_front();
    return pkt;
}

bool AcousticLink::channelBusy() {
    // Kanál je obsazený, když přijímač rozpracovává cizí rámec, nebo když
    // je na vstupu výrazná energie (něco vysílá, i když to (ještě) neumíme
    // dekódovat — cizí preambule, kolize, hluk).
    return rx_.state() != FrameReceiver::State::SearchPreamble ||
           input_rms_ > p_.busy_input_rms;
}

double AcousticLink::drawBackoff(double now_s) {
    std::uniform_real_distribution<double> d(p_.backoff_min_s, p_.backoff_max_s);
    return now_s + d(rng_);
}

void AcousticLink::tick(double now_s) {
    // 1) přijaté vzorky: v Idle/Backoff je zpracuj, při vysílání zahoď
    for (;;) {
        const size_t got = pop_(chunk_);
        if (got == 0) break;
        const std::span<const float> x(chunk_.data(), got);

        double e = 0.0;
        for (float v : x) e += double(v) * v;
        const float rms = float(std::sqrt(e / double(got)));
        input_rms_ += 0.3f * (rms - input_rms_);

        if (mac_ == MacState::Idle || mac_ == MacState::Backoff) {
            rx_.pushSamples(x);
            while (auto r = rx_.poll()) {
                if (r->crc_ok && r->payload_type == kPayloadEther) {
                    rx_queue_.push_back(std::move(r->payload));
                    ++stats_.rx_ok;
                } else if (!r->crc_ok) {
                    ++stats_.rx_crc_fail;
                }
            }
        }
        if (got < chunk_.size()) break;
    }

    // 2) MAC stavový automat
    switch (mac_) {
    case MacState::Idle:
        if (tx_queue_.empty()) break;
        // před KAŽDÝM vysíláním krátký náhodný rozptyl startu — během něj
        // se projeví případné vysílání protistanice (carrier sense)
        mac_ = MacState::Backoff;
        {
            std::uniform_real_distribution<double> d(p_.dither_min_s,
                                                     p_.dither_max_s);
            state_until_ = now_s + d(rng_);
        }
        break;

    case MacState::Backoff:
        if (now_s < state_until_) break;
        // Ochrana proti vyhladovění: falešný lock přijímače na šum (nebo
        // vadně dekódovaná délka payloadu) může držet channelBusy() dlouhé
        // sekundy. Po vyčerpání limitu backoffů se vysílání vynutí —
        // riziko kolize je menší zlo než mrtvá TX fronta.
        if (channelBusy() && consecutive_backoffs_ < 8) {
            state_until_ = drawBackoff(now_s); // kanál obsazen → delší odklad
            ++stats_.backoffs;
            ++consecutive_backoffs_;
            break;
        }
        if (consecutive_backoffs_ >= 8) ++stats_.forced_tx;
        consecutive_backoffs_ = 0;
        {
            const auto& scheme = *p_.scheme;
            auto mod = scheme.makeMod();
            mod->configure(p_.cfg);
            const auto samples = Framer::buildFrame(
                tx_queue_.front(), *mod, p_.cfg, kPayloadEther);
            push_(samples);
            tx_queue_.pop_front();
            ++stats_.tx_frames;
            mac_ = MacState::Transmitting;
            rx_.reset(); // zahoď rozpracovaný příjem — teď slyšíme sami sebe
        }
        break;

    case MacState::Transmitting:
        if (tx_pending_() == 0) {
            mac_ = MacState::Guard;
            state_until_ = now_s + p_.guard_s;
        }
        break;

    case MacState::Guard:
        if (now_s >= state_until_) {
            rx_.reset(); // čistý start po vlastním vysílání a dozvuku
            mac_ = MacState::Idle;
        }
        break;
    }
}

} // namespace am
