// Streamovací přijímač rámců. Stavový automat:
//
//   SearchPreamble --chirp nalezen--> Sync --0x2DD4--> Header --CRC hlavičky OK-->
//   Payload --načteno payload_len bajtů--> Crc --> výsledek + návrat na začátek
//
// Symbolové hodiny: korelace chirpu určí hranici prvního symbolu, dál se
// krájí pevná okna samplesPerSymbol(). Drift hodin zvukových karet (~10 ppm)
// je při 31,25 Bd a rámci ≤ 256 B hluboko pod zlomkem symbolu — per-symbol
// tracking není potřeba (rozbor v docs/protocol.md).

#include "protocol/frame_receiver.hpp"

#include "protocol/crc16.hpp"

namespace am {

namespace {
// práh normalizované korelace chirpu; čistý signál dává ~1,0, přes
// reproduktor→mikrofon typicky 0,5–0,8, šum pod ~0,2
constexpr float kCorrThreshold = 0.40f;
} // namespace

void FrameReceiver::configure(const ModemConfig& cfg, const ModemScheme& scheme,
                              const PreambleSpec& pre) {
    cfg_ = cfg;
    pre_ = pre;
    scheme_ = &scheme;
    demod_ = scheme.makeDemod();
    demod_->configure(cfg);
    corr_.configure(pre, cfg.sample_rate);
    reset();
}

void FrameReceiver::reset() {
    buf_.clear();
    read_pos_ = 0;
    state_ = State::SearchPreamble;
    rx_bits_ = {};
    payload_len_ = 0;
    ver_flags_ = 0;
    snr_accum_ = 0.f;
    snr_count_ = 0;
    last_corr_peak_ = 0.f;
    results_.clear();
    if (demod_) demod_->reset();
}

void FrameReceiver::pushSamples(std::span<const float> x) {
    buf_.insert(buf_.end(), x.begin(), x.end());
    processBuffered();
}

std::optional<FrameReceiver::Result> FrameReceiver::poll() {
    if (results_.empty()) return std::nullopt;
    Result r = std::move(results_.front());
    results_.pop_front();
    return r;
}

bool FrameReceiver::takeSymbol(uint32_t& bits_out) {
    const size_t sps = size_t(cfg_.samplesPerSymbol());
    if (read_pos_ + sps > buf_.size()) return false;
    bits_out = demod_->demodSymbol({buf_.data() + read_pos_, sps}, &last_diag_);
    if (onSymbol) onSymbol(last_diag_, bits_out);
    snr_accum_ += last_diag_.snr_db;
    ++snr_count_;
    read_pos_ += sps;
    return true;
}

void FrameReceiver::restartSearch(size_t keep_from) {
    state_ = State::SearchPreamble;
    read_pos_ = keep_from;
    rx_bits_ = {};
    payload_len_ = 0;
    snr_accum_ = 0.f;
    snr_count_ = 0;
}

void FrameReceiver::processBuffered() {
    const int bps = demod_->bitsPerSymbol();

    // Preempce: nový chirp uprostřed rozpracovaného rámce (uříznutý rámec,
    // kolize) znamená, že starý rámec už nikdy nedoběhne — zahoď ho a začni
    // nový. Hledá se s ohlédnutím za read_pos_, protože demodulátor mohl
    // začátek nového chirpu už spolykat jako datové symboly. Chirp vlastního
    // rámce (frame_start_) se ignoruje. Kontrola jednou za push, ne za symbol.
    if (state_ != State::SearchPreamble) {
        const size_t lookback = size_t(corr_.chirpLen() + corr_.gapLen());
        const size_t from = read_pos_ > lookback ? read_pos_ - lookback : 0;
        float peak = 0.f;
        const long sym0 = corr_.search(buf_, from, kCorrThreshold, &peak);
        if (sym0 >= 0 &&
            size_t(sym0) > frame_start_ + size_t(cfg_.samplesPerSymbol()) / 2) {
            restartSearch(size_t(sym0));
            frame_start_ = size_t(sym0);
            demod_->reset();
            state_ = State::Sync;
        }
    }

    for (;;) {
        if (state_ == State::SearchPreamble) {
            float peak = 0.f;
            const long sym0 =
                corr_.search(buf_, read_pos_, kCorrThreshold, &peak);
            last_corr_peak_ = std::max(last_corr_peak_ * 0.9f, peak); // pomalý dojezd pro GUI metr
            if (sym0 < 0) {
                // Chirp nenalezen. Posuň hledání tak, aby příště stačilo
                // prohledat jen nová data + přesah, ve kterém by mohl ležet
                // teprve částečně přijatý chirp.
                const size_t overlap = size_t(corr_.chirpLen() + corr_.gapLen()) + 64;
                if (buf_.size() > overlap)
                    read_pos_ = std::max(read_pos_, buf_.size() - overlap);
                break;
            }
            // nalezena hranice prvního symbolu
            read_pos_ = size_t(sym0);
            frame_start_ = size_t(sym0);
            rx_bits_ = {};
            demod_->reset();
            snr_accum_ = 0.f;
            snr_count_ = 0;
            state_ = State::Sync;
        }

        uint32_t sym_bits = 0;
        if (!takeSymbol(sym_bits)) break; // čekáme na další vzorky
        rx_bits_.pushBits(sym_bits, bps);

        switch (state_) {
        case State::Sync: {
            const size_t need = size_t(kRefSymbols) * bps + 16;
            if (rx_bits_.size() < need) break;
            uint32_t sync = 0;
            for (int i = 0; i < 16; ++i)
                sync |= uint32_t(rx_bits_.bit(size_t(kRefSymbols) * bps + i)) << i;
            if (sync != kSyncWord) {
                // Falešná korelace nebo přerušený vysílač. Vrať se těsně ZA
                // začátek falešného locku — pravý chirp mohl dorazit během
                // symbolů, které jsme tu spolykali (~0,5 s), a skok na
                // read_pos_ by ho nenávratně přeskočil (nález z review,
                // reprodukováno na macu).
                restartSearch(frame_start_ +
                              size_t(cfg_.samplesPerSymbol()) / 2);
                break;
            }
            rx_bits_ = {}; // od teď sbíráme jen hlavičku
            state_ = State::Header;
            break;
        }
        case State::Header: {
            if (rx_bits_.size() < 40) break;
            const std::vector<uint8_t> h = rx_bits_.toBytes();
            const uint16_t hcrc = uint16_t(h[3]) << 8 | h[4];
            const size_t len = size_t(h[1]) | size_t(h[2]) << 8;
            if (crc16({h.data(), 3}) != hcrc || len > size_t(kMaxPayload)) {
                // stejné zdůvodnění jako u SYNC selhání
                restartSearch(frame_start_ +
                              size_t(cfg_.samplesPerSymbol()) / 2);
                break;
            }
            ver_flags_ = h[0];
            payload_len_ = len;
            rx_bits_ = {};
            state_ = State::Payload;
            break;
        }
        case State::Payload: {
            if (rx_bits_.size() < payload_len_ * 8) break;
            state_ = State::Crc;
            break;
        }
        case State::Crc: {
            if (rx_bits_.size() < payload_len_ * 8 + 16) break;
            std::vector<uint8_t> all = rx_bits_.toBytes();
            Result r;
            r.payload.assign(all.begin(), all.begin() + long(payload_len_));
            const uint16_t rx_crc =
                uint16_t(all[payload_len_]) << 8 | all[payload_len_ + 1];
            r.crc_ok = crc16(r.payload) == rx_crc;
            r.payload_type = uint8_t(ver_flags_ & 0x07);
            r.mean_snr_db = snr_count_ ? snr_accum_ / float(snr_count_) : 0.f;
            results_.push_back(std::move(r));
            restartSearch(read_pos_); // hledej další rámec za tímto
            break;
        }
        case State::SearchPreamble:
            break; // sem se nedostaneme (symbol bereme jen v datových stavech)
        }
    }

    // Zahoď zpracovaný prefix bufferu, ať neroste donekonečna. Ponech
    // rezervu pro zpřesnění korelační špičky — a ve stavech Sync/Header
    // navíc drž vše od začátku rámce, protože při selhání SYNC/hlavičky
    // se tam hledání vrací (viz restartSearch výše). Oba stavy jsou
    // omezené na ~27 symbolů, takže paměť je shora omezená.
    const size_t margin = size_t(corr_.chirpLen() + corr_.gapLen()) + 64;
    const bool early_stage = state_ == State::Sync || state_ == State::Header;
    const size_t anchor =
        early_stage ? std::min(frame_start_, read_pos_) : read_pos_;
    if (anchor > margin + 4096) {
        const size_t drop = anchor - margin;
        buf_.erase(buf_.begin(), buf_.begin() + long(drop));
        read_pos_ -= drop;
        frame_start_ -= std::min(frame_start_, drop);
    }
}

} // namespace am
