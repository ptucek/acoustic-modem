// OOK (on-off keying): bit 1 → nosná f0 zapnutá, bit 0 → ticho.
// Nejjednodušší modulace na pochopení, ale nejcitlivější na hluk — nemá
// druhý tón k porovnání, rozhoduje se proti adaptivnímu prahu.
//
// TX: raised-cosine rampa 5 ms na začátku/konci každého ON symbolu — bez ní
// by ostré hrany lupaly a rozmazávaly spektrum.
//
// RX: energie Goertzelem na f0; práh = geometrický průměr klouzavých odhadů
// energie „zapnuto" a „vypnuto". Seed: referenční symbol před SYNC je podle
// protokolu vždy jedničkový (nosná ON) — první symbol po resetu tedy
// inicializuje odhad ON. SYNC slovo 0x2DD4 obsahuje obě hodnoty bitů,
// takže se oba odhady rychle zpřesní.

#include <cmath>
#include <numbers>

#include "dsp/goertzel.hpp"
#include "modem/schemes.hpp"

namespace am {
namespace {

class OokModulator final : public IModulator {
public:
    void configure(const ModemConfig& c) override {
        cfg_ = c;
        reset();
    }
    int  bitsPerSymbol() const override { return 1; }
    void reset() override { phase_ = 0.0; }

    void modulate(const BitBuffer& bits, std::vector<float>& out) override {
        const int sps = cfg_.samplesPerSymbol();
        const int n_ramp = std::min(sps / 4, int(0.005 * cfg_.sample_rate));
        const double dphi = 2.0 * std::numbers::pi * cfg_.f0 / cfg_.sample_rate;
        out.reserve(out.size() + bits.size() * size_t(sps));
        for (size_t i = 0; i < bits.size(); ++i) {
            const bool on = bits.bit(i);
            // rampy jen na hranách (soused má jinou hodnotu) — uvnitř bloku
            // jedniček nosná běží nepřerušeně
            const bool ramp_in = on && (i == 0 || !bits.bit(i - 1));
            const bool ramp_out = on && (i + 1 == bits.size() || !bits.bit(i + 1));
            for (int s = 0; s < sps; ++s) {
                double env = on ? 1.0 : 0.0;
                if (ramp_in && s < n_ramp)
                    env *= 0.5 - 0.5 * std::cos(std::numbers::pi * s / n_ramp);
                if (ramp_out && s >= sps - n_ramp)
                    env *= 0.5 -
                           0.5 * std::cos(std::numbers::pi * (sps - s) / n_ramp);
                out.push_back(float(cfg_.amplitude * env * std::sin(phase_)));
                phase_ += dphi; // fáze běží i v tichu → koherentní návrat nosné
                if (phase_ > 2.0 * std::numbers::pi)
                    phase_ -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    ModemConfig cfg_;
    double phase_ = 0.0;
};

class OokDemodulator final : public IDemodulator {
public:
    void configure(const ModemConfig& c) override {
        g_ = Goertzel(c.f0, c.sample_rate, c.samplesPerSymbol());
        reset();
    }
    int  bitsPerSymbol() const override { return 1; }
    void reset() override {
        on_avg_ = -1.f; // neinicializováno — seedne první (referenční) symbol
        off_avg_ = 0.f;
    }

    uint32_t demodSymbol(std::span<const float> sym, SymbolDiag* diag) override {
        // Dozvuk místnosti: po ON symbolu nosná doznívá desítky ms a přelévá
        // se do začátku následujícího symbolu — izolovaná nula po jedničce
        // pak z celého okna vyjde nad prahem a čte se jako jednička (reálně
        // naměřeno vzduchem: ocas jen ~7 dB pod ON, práh je −10 dB). Energie
        // se proto měří až od kReverbSkip okna; u ON symbolu se tím nic
        // neztrácí (nosná běží celý symbol), u nuly po jedničce je dozvuk
        // v pozdější části citelně slabší.
        const size_t skip = size_t(double(sym.size()) * kReverbSkip);
        const float e = Goertzel::energy(g_.run(sym.subspan(skip)));
        if (on_avg_ < 0.f) {
            // Referenční symbol: podle protokolu je vždy ON. Seed OFF úrovně
            // je −13 dB od ON (ne −20 dB): počáteční práh tak leží −6,5 dB
            // pod ON a odmítne dozvukové ocasy hned od prvního symbolu —
            // s nižším seedem se práh k reálné úrovni dozvuku šplhal desítky
            // symbolů a začátek payloadu se četl s chybami 0→1 (misready
            // navíc adaptaci brzdí, protože aktualizují on_avg_ místo
            // off_avg_). EMA pak práh doladí podle skutečných podmínek.
            on_avg_ = e;
            off_avg_ = e * 0.05f;
        }
        // Práh vážený k ON (0,7/0,3 v dB): OFF energie tvoří bimodální směs
        // hlubokých nul a dozvukových ocasů po jedničkách — off_avg_ (EMA
        // přes obojí) leží mezi nimi a symetrický práh (geometrický průměr)
        // nechává ocasy nad sebou. ON je naproti tomu stabilní (±2–3 dB),
        // takže práh ~4–7 dB pod ON odmítne ocasy a jedničky neohrozí.
        const float thr = std::pow(on_avg_, 0.7f) *
                          std::pow(std::max(off_avg_, 1e-12f), 0.3f);
        const bool bit = e > thr;
        // klouzavý odhad (EMA) — sleduje pomalé změny hlasitosti/útlumu
        constexpr float a = 0.15f;
        if (bit) on_avg_ += a * (e - on_avg_);
        else     off_avg_ += a * (e - off_avg_);
        if (diag) {
            diag->tone_energy = {e, thr};
            diag->phasor = {};
            diag->snr_db = 10.f * std::log10((std::max(e, thr) + 1e-12f) /
                                             (std::min(e, thr) + 1e-12f));
        }
        return bit ? 1u : 0u;
    }

private:
    // Podíl začátku symbolového okna vynechaný z měření energie (dozvuk).
    static constexpr double kReverbSkip = 0.5;

    Goertzel g_;
    float on_avg_ = -1.f, off_avg_ = 0.f;
};

} // namespace

std::unique_ptr<IModulator>   makeOokMod()   { return std::make_unique<OokModulator>(); }
std::unique_ptr<IDemodulator> makeOokDemod() { return std::make_unique<OokDemodulator>(); }

} // namespace am
