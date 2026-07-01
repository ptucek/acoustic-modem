// Implementace DSP vlákna — smyčka: rx ring → vodopád + FrameReceiver,
// obsluha rekonfigurace a TX fronty, publikace eventů pro GUI.

#include "app/dsp_thread.hpp"

#include <chrono>
#include <cmath>

#include "core/prbs.hpp"
#include "protocol/framer.hpp"

namespace am {

namespace {
constexpr size_t kChunk = 4096;        // vzorků na jednu obrátku smyčky
constexpr size_t kPrbsPayload = 128;   // bajtů PRBS na testovací rámec
constexpr size_t kSeriesCap = 4096;    // max bodů časových řad pro GUI

template <typename T>
void pushCapped(std::vector<T>& v, T value) {
    if (v.size() >= kSeriesCap) v.erase(v.begin(), v.begin() + long(kSeriesCap / 4));
    v.push_back(value);
}
} // namespace

DspThread::DspThread() : epoch_(std::chrono::steady_clock::now()) {}

DspThread::~DspThread() { stop(); }

double DspThread::nowSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         epoch_)
        .count();
}

void DspThread::drainRings() {
    std::vector<float> sink(4096);
    while (rx_ring_.pop(sink) > 0) {}
    while (tx_ring_.pop(sink) > 0) {}
}

bool DspThread::start(const ModemConfig& cfg, int scheme_index) {
    stop();
    drainRings(); // zbytky starého vysílání/příjmu nesmí přežít restart
    {
        std::lock_guard lock(mtx_);
        cfg_ = cfg;
        scheme_index_ = scheme_index;
        reconfigure_pending_ = false;
        rx_events_.clear();
        tx_queue_.clear();
        tx_total_ = 0;
    }
    if (!audio_.start(capture_index_, playback_index_, cfg.sample_rate,
                      rx_ring_, tx_ring_))
        return false;
    waterfall_.configure(cfg.sample_rate);
    running_ = true;
    thread_ = std::thread(&DspThread::run, this);
    return true;
}

void DspThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    audio_.stop();
}

void DspThread::reconfigure(const ModemConfig& cfg, int scheme_index) {
    std::lock_guard lock(mtx_);
    cfg_ = cfg;
    scheme_index_ = scheme_index;
    reconfigure_pending_ = true;
    tx_queue_.clear(); // rámce modulované starým schématem už nemají smysl
}

bool DspThread::restartAudio(int capture_index, int playback_index) {
    ModemConfig cfg;
    int scheme;
    {
        std::lock_guard lock(mtx_);
        capture_index_ = capture_index;
        playback_index_ = playback_index;
        cfg = cfg_;
        scheme = scheme_index_;
    }
    return start(cfg, scheme);
}

void DspThread::sendText(const std::string& utf8) {
    sendBytes({utf8.begin(), utf8.end()}, kPayloadText);
}

void DspThread::sendBytes(std::vector<uint8_t> payload, uint8_t payload_type) {
    if (payload.size() > size_t(kMaxPayload))
        payload.resize(size_t(kMaxPayload));
    buildAndEnqueue(payload, payload_type);
}

void DspThread::buildAndEnqueue(std::span<const uint8_t> payload,
                                uint8_t payload_type) {
    ModemConfig cfg;
    const ModemScheme* scheme;
    {
        std::lock_guard lock(mtx_);
        cfg = cfg_;
        scheme = &modemRegistry()[size_t(scheme_index_)];
    }
    // modulace (potenciálně miliony vzorků) běží bez zámku — GUI se nezasekne
    auto mod = scheme->makeMod();
    mod->configure(cfg);
    auto frame = Framer::buildFrame(payload, *mod, cfg, payload_type);

    std::lock_guard lock(mtx_);
    if (frame.size() > tx_ring_.capacity()) {
        // částečný push by odvysílal uříznutý rámec → protistrana by
        // dostala CRC FAIL bez vysvětlení; radši zahodit a započítat
        ++stats_.tx_dropped;
        return;
    }
    tx_queue_.push_back(std::move(frame));
}

void DspThread::setBerTestTx(bool on) {
    std::lock_guard lock(mtx_);
    ber_test_tx_ = on;
    stats_.ber_test_tx = on;
}

ErrorStats DspThread::errorStats() {
    std::lock_guard lock(mtx_);
    return stats_;
}

void DspThread::resetErrorStats() {
    std::lock_guard lock(mtx_);
    const bool tx = stats_.ber_test_tx;
    stats_ = {};
    stats_.ber_test_tx = tx;
}

std::vector<RxFrameEvent> DspThread::drainFrames() {
    std::lock_guard lock(mtx_);
    std::vector<RxFrameEvent> out(rx_events_.begin(), rx_events_.end());
    rx_events_.clear();
    return out;
}

DspStatus DspThread::status() {
    std::lock_guard lock(mtx_);
    return status_;
}

SymbolDiag DspThread::lastSymbolDiag() {
    std::lock_guard lock(mtx_);
    return last_diag_;
}

ModemConfig DspThread::config() {
    std::lock_guard lock(mtx_);
    return cfg_;
}

void DspThread::run() {
    FrameReceiver rx;
    {
        std::lock_guard lock(mtx_);
        rx.configure(cfg_, modemRegistry()[size_t(scheme_index_)]);
    }

    std::vector<float> chunk(kChunk);
    double last_qual_t = 0.0;

    while (running_) {
        // 1) rekonfigurace z GUI (změna modulace/parametrů)
        {
            std::lock_guard lock(mtx_);
            if (reconfigure_pending_) {
                rx.configure(cfg_, modemRegistry()[size_t(scheme_index_)]);
                reconfigure_pending_ = false;
            }
        }

        // 2) TX: je-li ring prázdný a čeká rámec, nasyp ho tam celý
        bool want_prbs = false;
        {
            std::lock_guard lock(mtx_);
            want_prbs =
                ber_test_tx_ && tx_queue_.empty() && tx_ring_.sizeApprox() == 0;
            if (!tx_queue_.empty() && tx_ring_.sizeApprox() == 0) {
                const auto& frame = tx_queue_.front();
                tx_total_ = frame.size();
                // kapacita ověřena při enqueue → push nemůže být částečný,
                // ring je v tuto chvíli prázdný
                tx_ring_.push(frame);
                tx_queue_.pop_front();
            }
            status_.tx_progress =
                tx_total_ == 0
                    ? -1.f
                    : 1.f - float(tx_ring_.sizeApprox()) / float(tx_total_);
            if (tx_ring_.sizeApprox() == 0 && tx_queue_.empty()) tx_total_ = 0;
        }
        // testovací režim BER: doplň frontu PRBS rámcem (modulace mimo zámek)
        if (want_prbs)
            buildAndEnqueue(Prbs15().generate(kPrbsPayload), kPayloadPrbs);

        // 3) RX: zpracuj dostupné vzorky
        const size_t got = rx_ring_.pop(chunk);
        if (got > 0) {
            const std::span<const float> x(chunk.data(), got);
            waterfall_.pushSamples(x);
            rx.pushSamples(x);
            while (auto r = rx.poll()) {
                RxFrameEvent ev;
                ev.payload = std::move(r->payload);
                ev.payload_type = r->payload_type;
                ev.crc_ok = r->crc_ok;
                ev.snr_db = r->mean_snr_db;
                ev.t_received = nowSeconds();
                std::lock_guard lock(mtx_);
                // FER + BER účetnictví
                if (ev.crc_ok) ++stats_.frames_ok;
                else ++stats_.frames_crc_fail;
                if (ev.payload_type == kPayloadPrbs && !ev.payload.empty()) {
                    const auto expected = Prbs15().generate(ev.payload.size());
                    const size_t errs = countBitErrors(ev.payload, expected);
                    const size_t bits = ev.payload.size() * 8;
                    stats_.prbs_bits += bits;
                    stats_.prbs_errors += errs;
                    pushCapped(stats_.ber, {float(ev.t_received),
                                            float(errs) / float(bits)});
                }
                rx_events_.push_back(std::move(ev));
                if (rx_events_.size() > 512) rx_events_.pop_front();
            }
        }

        // 4) stav pro GUI
        {
            std::lock_guard lock(mtx_);
            status_.rx_state = rx.state();
            status_.corr_peak = rx.lastCorrPeak();
            status_.input_peak = std::max(status_.input_peak * 0.95f,
                                          audio_.inputPeak());
            status_.audio_running = audio_.running();
            last_diag_ = rx.lastSymbolDiag();

            // časová řada „kvality" (~4 vzorky/s): rozhodovací rezerva
            // demodulátoru během příjmu rámce, NAN = mezera (nic se nepřijímá)
            const double now = nowSeconds();
            if (now - last_qual_t >= 0.25) {
                last_qual_t = now;
                const bool receiving =
                    rx.state() != FrameReceiver::State::SearchPreamble;
                pushCapped(stats_.quality,
                           {float(now), receiving ? last_diag_.snr_db
                                                  : std::nanf("")});
            }
        }

        // nic ke čtení → krátce spi, ať netočíme CPU naprázdno
        if (got < kChunk)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace am
