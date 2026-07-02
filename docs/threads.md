# Vlákna a kritické sekce

Tento dokument popisuje **všechna** vlákna, která projekt v jednotlivých
aplikacích spouští, a všechny sdílené stavy, kde je potřeba synchronizace.
Doplňuje [`architecture.md`](architecture.md) (vláknový model `modem_gui` je
tam shrnutý diagramem) — tady je cíl vyjmenovat úplně všechno, včetně
`modem_cli` a `modem_tap`, a u každého zámku/atomiku napsat přesně, co chrání
a proč je zvolený zrovna daný mechanismus.

## Přehled podle aplikace

| Aplikace | Vlastní vlákna | Poznámka |
|---|---|---|
| `modem_cli tx/rx/chansim/devices` | **1** (main) | čistě offline — žádný zvuk, žádné další vlákno |
| `modem_cli send/listen` | **2** (main + audio callback) | main vlákno jen pollí ring buffery ve smyčce se `sleep_for` |
| `modem_gui` | **3** (GUI/main + DSP + audio callback) | jediná aplikace se skutečným worker vláknem |
| `modem_tap` | **2** (main + audio callback) | main vlákno = `poll()` smyčka nad TAP fd + `AcousticLink::tick()` |

Audio callback vlákno **nezakládá aplikace sama** — vytváří a řídí ho OS
zvukový subsystém (WASAPI/CoreAudio/ALSA/PulseAudio) přes miniaudio;
aplikace jen zaregistruje callback funkci (`AudioEngine::start()`).

## Vlákna podrobně

### 1. Audio callback vlákno (real-time, ve všech aplikacích se zvukem)

- Kód: `src/audio/audio_engine.cpp`, `Impl::dataCallback`.
- Vlastník: ovladač zvukové karty přes miniaudio — běží s tvrdým časovým
  rozpočtem (typicky řádu jednotek milisekund na blok, jinak xrun/glitch).
- Dělá **jen** memcpy: `tx_ring → výstupní buffer` (TX), `vstupní buffer →
  rx_ring` (RX), plus aktualizaci `input_peak` pro level metr.
- **Žádné zámky, žádné alokace, žádné DSP** — to je záměrná investice do
  spolehlivosti: cokoliv, co by tohle vlákno mohlo zablokovat (mutex, malloc,
  syscall), riskuje slyšitelný výpadek zvuku uprostřed rámce (viz reálný
  nález xrunu v `docs/measurements.md` a `docs/zprava.md` §4.1).
- Když je `rx_ring` plný, nové vzorky se tiše zahazují (`SpscRing::push`
  vrací méně, než dostal) — nikdy neblokuje. Když je `tx_ring` prázdný,
  výstup se dorovná tichem.

### 2. DSP vlákno (jen `modem_gui`)

- Kód: `src/app/dsp_thread.cpp`, `DspThread::run()`; založeno v `start()`,
  joinováno v `stop()`.
- Není real-time kritické (o pár ms zpoždění nikdo nepozná) — proto může
  bez rizika používat `std::mutex`.
- Smyčka (`kChunk = 4096` vzorků na obrátku, `sleep_for(10 ms)`, když není
  co číst):
  1. vyzvedne případnou rekonfiguraci z GUI (`reconfigure_pending_`),
  2. je-li `tx_ring` prázdný a čeká rámec ve `tx_queue_`, nasype ho tam
     celý najednou,
  3. přečte, co je v `rx_ring`, nakrmí tím `Waterfall` a `FrameReceiver`,
     vytěží dokončené rámce do `rx_events_` a průběžně počítá BER/FER,
  4. zapíše aktuální `DspStatus`/`SymbolDiag` pro GUI.
- Vlastní `FrameReceiver` a `Waterfall::pushSamples()` volá **jen toto
  vlákno** — nejsou sdílené s nikým jiným, takže je nechrání žádný zámek
  (nemají co chránit).

### 3. GUI vlákno (`modem_gui`, = hlavní vlákno procesu)

- Kód: `src/app/main_gui.cpp` — SDL event loop, ImGui rendering (~60 fps),
  `src/app/panels.cpp` (obsah panelů).
- Do `DspThread` sahá **výhradně** přes jeho veřejné, zamykané metody
  (`status()`, `drainFrames()`, `errorStats()`, `lastSymbolDiag()`,
  `waterfall().snapshot()`, `sendText()`, `reconfigure()`, `setBerTestTx()`,
  `restartAudio()`) — nikdy přímo do jeho privátního stavu.
- Nemá vlastní zámek — z pohledu GUI vlákna je "kritická sekce" jen doba
  trvání jednoho volání do `DspThread`/`Waterfall`.

### 4. Hlavní vlákno `modem_cli`

- Kód: `src/cli/main_cli.cpp`, funkce `cmdSend`/`cmdListen`.
- `tx`/`rx`/`chansim`/`devices` běží čistě nad daty v paměti/na disku —
  žádné audio vlákno se vůbec nezakládá.
- `send`/`listen` **nezakládají žádné vlastní worker vlákno** — jediná
  paralelita je audio callback. Hlavní vlákno jen pollí `tx_ring`/`rx_ring`
  ve smyčce s `sleep_for` (100 ms u `send` při čekání na vyprázdnění, 5 ms
  u `listen` při čekání na nová data) a čte `std::atomic<bool>
  g_interrupted` nastavovaný signal handlerem (`SIGINT`).

### 5. Hlavní vlákno `modem_tap`

- Kód: `src/net/main_tap.cpp` — smyčka `poll()` nad file descriptorem TAP
  zařízení (`src/link/tap_device.cpp`), s timeoutem, aby se pravidelně
  stihlo zavolat i `AcousticLink::tick()` (řídí CSMA stavový automat,
  potřebuje běžet i bez příchozích paketů).
- `AcousticLink` (`src/link/acoustic_link.hpp/.cpp`) **sám o sobě není
  thread-safe a ani nemusí být** — je navržený jako čistá synchronní třída
  řízená jedním vláknem (i v testech, viz `tests/test_link.cpp`, kde ho
  žene simulovaný kanál bez jakéhokoli vlákna navíc). Se zvukovou vrstvou
  komunikuje jen přes injektované `PopFn`/`PushFn`/`TxPendingFn` (typicky
  lambdy nad `rx_ring`/`tx_ring`) — jediná skutečná hranice mezi vlákny je
  tedy znovu `SpscRing` sdílený s audio callbackem, ne `AcousticLink` sám.
- Signalizace `SIGINT` stejná jako u `modem_cli`: `std::atomic<bool>
  g_interrupted`.

## Kritické sekce — kompletní seznam

| Chráněný stav | Mechanismus | Kde | Kdo píše / kdo čte |
|---|---|---|---|
| `rx_ring_`, `tx_ring_` (vzorky mezi audio callbackem a zbytkem aplikace) | `SpscRing<float>` — bezzámkový, jeden atomický pár `head_`/`tail_` (acquire/release) | `src/core/spsc_ring.hpp` | audio callback (producer RX / consumer TX) ↔ DSP vlákno (`modem_gui`) nebo hlavní vlákno (`modem_cli`, `modem_tap`) |
| `Impl::input_peak` (špička vstupu pro level metr) | `std::atomic<float>`, `memory_order_relaxed` | `src/audio/audio_engine.cpp` | audio callback píše, `inputPeak()` čte/nuluje (`exchange`) — nepřesnost při závodu je neškodná (jen UI metr) |
| `DspThread::mtx_` — `cfg_`, `scheme_index_`, `reconfigure_pending_`, `tx_queue_`, `rx_events_`, `last_diag_`, `status_`, `stats_`, `tx_total_`, `capture_index_`/`playback_index_` | `std::mutex` + `std::lock_guard` | `src/app/dsp_thread.{hpp,cpp}` | DSP vlákno (píše ve `run()`) ↔ GUI vlákno (čte/píše přes veřejné metody) |
| `Waterfall::matrix_`, `version_`, `last_read_version_` | vlastní `std::mutex` (odděleně od `DspThread::mtx_`) | `src/app/waterfall.{hpp,cpp}` | DSP vlákno píše (`pushSamples`), GUI vlákno čte (`snapshot`) — vlastní zámek, protože jde o logicky nezávislý stav volaný v každé obrátce smyčky |
| `running_` (má DSP vlákno běžet) | `std::atomic<bool>` | `src/app/dsp_thread.hpp` | GUI vlákno nastavuje `false` ve `stop()`, DSP vlákno čte v podmínce `while (running_)` |
| `g_interrupted` (Ctrl+C) | `std::atomic<bool>` | `src/cli/main_cli.cpp`, `src/net/main_tap.cpp` | signal handler (libovolný kontext) píše, hlavní vlákno čte v hlavní smyčce |

Žádná jiná sdílená proměnná v projektu mezi vlákny neputuje — `AcousticLink`,
`FrameReceiver`, `Framer`, modulátory/demodulátory i `ChirpCorrelator` jsou
vždy vlastněné a volané jedním konkrétním vláknem po celou dobu své
existence.

## Proč dvě různé strategie (lock-free vs. mutex)

Audio callback má tvrdý real-time rozpočet — zablokování na mutexu (např.
protože GUI vlákno zrovna čte konfiguraci) by způsobilo slyšitelný xrun.
Proto jediné místo v celém projektu, kde vzorky přecházejí mezi vlákny
(hranice audio callback ↔ zbytek aplikace), používá bezzámkový `SpscRing`.
Všude jinde (DSP vlákno ↔ GUI vlákno) real-time nárok není — pár milisekund
čekání na zámek nikdo nepozná — takže tam obyčejný `std::mutex` je
jednodušší a čitelnější řešení bez ztráty na kvalitě běhu aplikace.
