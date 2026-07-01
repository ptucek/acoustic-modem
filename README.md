# Akustický modem

Školní projekt: přenos digitálních dat zvukem mezi dvěma zařízeními
(reproduktor → vzduch → mikrofon), bez jakéhokoli kabelu nebo rádia.
Cílem je ukázat celý řetězec od bitů přes modulaci, fyzickou vrstvu
(synchronizační preambule, CRC) až po jednoduchou aplikační vrstvu
(textové zprávy, případně přemostění Ethernetu přes `modem_tap`).

Projekt vznikl jako semestrální práce, důraz je na čitelnost kódu a
srozumitelnost architektury před maximálním výkonem.

## Stavba

- **`modemcore`** — statická knihovna: DSP (chirp, Goertzel, simulace
  kanálu), modulace/demodulace (2-FSK, později OOK/DBPSK/16-FSK),
  protokol rámců (SYNC/HEADER/PAYLOAD/CRC), WAV I/O.
- **`modem_cli`** — příkazová řádka: kódování textu do WAV, dekódování
  WAV zpět na text, simulace akustického kanálu (šum, echo, drift hodin)
  pro testování bez reálného mikrofonu/reproduktoru.
- **`modem_tests`** — jednotkové a integrační testy (doctest).
- **`modem_gui`** *(volitelné, vyžaduje SDL3)* — grafické rozhraní s
  živým spektrem, konstelačním diagramem a grafy kvality signálu.
- **`modem_tap`** *(volitelné, jen Linux)* — přemostění TAP síťového
  rozhraní přes akustický modem (Ethernet rámce jako payload).

## Sestavení

Na Fedoře je potřeba nainstalovat toolchain a (nepovinně) závislosti GUI:

```sh
sudo dnf install gcc-c++ cmake ninja-build SDL3-devel mesa-libGL-devel
```

Konfigurace a sestavení (mimo zdrojový strom, adresář `build/`):

```sh
cmake -B build -G Ninja
cmake --build build
```

Bez SDL3 (nebo bez `SDL3-devel`) se `modem_gui` automaticky vynechá —
CMake to oznámí zprávou `message(STATUS ...)`, sestavení ostatních
cílů to neovlivní.

Spuštění testů:

```sh
ctest --test-dir build
```

### Build na macOS

Nejprve vývojářské nástroje Apple a poté toolchain přes Homebrew:

```sh
xcode-select --install
brew install cmake ninja sdl3
```

Konfigurace a sestavení jsou stejné jako na Linuxu:

```sh
cmake -B build -G Ninja
cmake --build build
```

Poznámky:

- Při prvním spuštění `listen` (nebo čehokoli, co otevře mikrofon) macOS
  zobrazí systémový dotaz na oprávnění k mikrofonu (TCC — Transparency,
  Consent and Control). Je potřeba jej potvrdit, jinak audio vstup
  nebude fungovat; oprávnění lze později spravovat v
  Nastavení → Soukromí a bezpečnost → Mikrofon.
- `modem_gui` na macOS běží nad SDL3 a OpenGL 3.2 Core profilem (macOS
  jiný profil pro GL nenabízí) — `main_gui.cpp` si podle platformy sám
  vybere správné atributy kontextu i verzi GLSL shaderů pro ImGui.
- `modem_tap` je zatím jen pro Linux (používá TUN/TAP rozhraní jádra).
  Podpora macOS (`utun` zařízení) je plánována na milník M7.

## Použití CLI

Zakódování textu do WAV souboru:

```sh
build/modem_cli tx --text "Ahoj, svete!" --out tx.wav
```

Simulace přenosu akustickým kanálem (přidá šum, případně echo a drift
hodin) bez nutnosti reálného reproduktoru/mikrofonu:

```sh
build/modem_cli chansim --in tx.wav --out rx.wav --snr 15
```

Dekódování přijatého (nebo simulovaného) WAV souboru zpět na text:

```sh
build/modem_cli rx --in rx.wav
```

Nápověda se všemi přepínači:

```sh
build/modem_cli --help
```

Formát rámce a volba parametrů je popsána v [`docs/protocol.md`](docs/protocol.md).

## Struktura projektu

```
src/
  core/       konfigurace, bitový proud, WAV I/O, SPSC ring buffer
  dsp/        chirp, Goertzelův filtr, FIR, simulace kanálu
  modem/      modulátory/demodulátory + registr schémat
  protocol/   CRC-16, sestavení a příjem rámců
  cli/        modem_cli (main_cli.cpp)
  app/        modem_gui (plánováno)
  net/        modem_tap (plánováno)
tests/        jednotkové testy (doctest)
docs/         specifikace protokolu
third_party/  doctest, kissfft, imgui, implot, miniaudio
```

## Stav milníků

| Milník | Popis | Stav |
|--------|-------|------|
| M1 | Jádro: konfigurace, bity, WAV I/O, CRC-16, 2-FSK, rámce, CLI | probíhá |
| M2 | Simulace kanálu, testovací sada (BER, chain testy) | plánováno |
| M3 | Reálný zvukový vstup/výstup (miniaudio), `modem_tap` | plánováno |
| M4 | GUI (ImGui/ImPlot): živé spektrum, konstelace, metriky | plánováno |
| M5 | Další modulace: OOK, DBPSK, 16-FSK | plánováno |
