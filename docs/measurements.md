# Měření chybovosti a přenosu

Tento dokument shrnuje dosavadní reálná měření akustického kanálu (milník
M6) — metodiku, výsledky a jejich reprodukci. Měření probíhala mezi dvěma
fyzickými stroji (Fedora Linux a MacBook Pro), koordinovanými dvěma Claude
agenty přes Dropbox — protokol spolupráce je popsán v
[`docs/dva-agenti.md`](dva-agenti.md).

## Metodika

### PRBS-15 pro měření BER

`modem_cli send --prbs N` vygeneruje jeden rámec s payloadem typu
`kPayloadPrbs` naplněným pseudonáhodnou sekvencí PRBS-15 (LFSR
`x^15 + x^14 + 1`, perioda 32767 bitů, standardní generátor z
telekomunikačních měření — `src/core/prbs.hpp`) a odešle ho N-krát za sebou
(stejný obsah, stejný seed na obou stranách). `modem_cli listen` na
přijímací straně u každého úspěšně nalezeného PRBS rámce vygeneruje tutéž
očekávanou sekvenci a spočítá bitovou chybovost XORem
(`countBitErrors`), nezávisle na tom, jestli rámcové CRC vyšlo — i CRC-FAIL
rámec tak dá užitečné číslo, kolik bitů se v něm reálně pokazilo.

### Záznam pro offline analýzu

`modem_cli listen --record cesta.wav` ukládá surový signál z mikrofonu do
WAV souboru navíc k běžnému zpracování naživo. To umožňuje:

- opakovaně pouštět stejný signál přes `modem_cli rx --in cesta.wav` po
  opravě v kódu a ověřit, jestli oprava skutečně mění výsledek na tomtéž
  vstupu (vyloučit, že šlo o jednorázový šum),
- forenzní rozbor konkrétní anomálie (viz níže).

### Handshake přes `sync/`

Live testy vyžadují, aby druhý stroj poslouchal přesně v okamžiku vysílání
— bez toho by test proběhl naprázdno. Koordinace neběží přes pevné časy
(Dropbox synchronizace má proměnnou latenci v řádu sekund), ale přes
**handshake nad stavovým souborem**: strana, která bude poslouchat, spustí
`listen` a *bezprostředně poté* zapíše řádek do svého `sync/status-*.md`
(např. „Naslouchám: od `<čas>` po 90 s"). Druhá strana svůj status
periodicky sleduje a po zachycení změny zahájí vysílání s malým zpožděním.
Podrobnosti protokolu (role, vlastnictví souborů, ACK zprávy) jsou v
[`docs/dva-agenti.md`](dva-agenti.md) a normativně v `sync/README.md`.

## Výsledky

### Přenos #1 — Linux → Mac vzduchem

Podmínky: oba stroje ve stejné místnosti, hlasitost ~70 %, vestavěný
reproduktor Fedora → vestavěný mikrofon MacBook Pro.

| Test | Schéma | CRC | SNR | corr peak | Poznámka |
|---|---|---|---|---|---|
| #1 | 2-FSK | **OK** | 28,1 dB | 0,82 | text „Ahoj Macu, tady Fedora #1" — přesná shoda |
| #2 (1. pokus) | 16-FSK | **FAIL** | 22,3 dB | 0,83 | payload bajt po bajtu správně (34/34 B), CRC přesto FAIL |
| #2 (opakování) | 16-FSK | **OK** | 23,3 dB | 0,84 | text „Ahoj Macu, tady Fedora #2 (16-FSK)" — přesná shoda |

**Rozbor CRC FAIL u testu #2 (1. pokus):** Payload dorazil bajt po bajtu
identický s očekávaným textem, ale kontrolní součet nesouhlasil — u
náhodného šumu na kanálu (a SNR 22 dB je stále komfortní) by se čekalo
poškození znaků v textu samotném. Offline loopback na macu
(`tx --scheme 16-FSK` → `rx --in ... --scheme 16-FSK` bez akustického
kanálu) dal CRC OK při SNR 109,9 dB — deterministická chyba v kódu tedy
vyloučena, poškození vzniklo až na akustickém kanálu. Protože CRC pole leží
na samém konci rámce, nejpravděpodobnějším vysvětlením je přechodový jev
(doznívání/echo) zasahující právě poslední symboly — při SNR na hraně
(~22–23 dB) je 16-FSK citlivější na takový jednorázový zásah než 2-FSK.
Opakování proběhlo bez problému.

### PRBS BER měření #1 — Linux → Mac

Stejné podmínky jako přenos #1. Provedeno po rebuildu s opravami z code
review (viz `docs/dva-agenti.md`).

**16-FSK — `send --prbs 8` (8 rámců à 128 B):**

| Rámec | CRC | SNR (dB) | BER |
|---|---|---|---|
| 1 | OK | 23,4 | 0/1024 |
| 2 | OK | 23,4 | 0/1024 |
| 3 | OK | 23,2 | 0/1024 |
| 4 | OK | 23,3 | 0/1024 |
| 5 | OK | 23,8 | 0/1024 |
| 6 | OK | 24,1 | 0/1024 |
| 7 | OK | 24,3 | 0/1024 |
| 8 | OK | 24,7 | 0/1024 |

**FER 0/8, celkové BER 0/8192.** 16-FSK je při SNR ~23–25 dB zcela čisté i
při 4× vyšší propustnosti než 2-FSK.

**2-FSK — `send --prbs 3` (3 rámce à 128 B):**

| Rámec | CRC | SNR (dB) | BER |
|---|---|---|---|
| 1 | FAIL | 23,3 | 436/1024 = 0,426 |
| 2 | OK | 31,8 | 0/1024 |
| 3 | — nenalezen — | | |

**FER 2/3.** BER 0,426 u rámce #1 odpovídá prakticky náhodným datům, což
při SNR 23 dB není vysvětlitelné šumem — ukazuje na ztrátu zarovnání
symbolových hodin, ne na chybu jednotlivých bitů.

#### Forenzní analýza rámce #1 (BER 0,43)

Bloková analýza payloadu (64bitové bloky, hledání lokálního posunu vůči
očekávané PRBS-15 sekvenci):

| blok bitů | nejlepší posun | chyb |
|---|---|---|
| 0–63 | 0 | 0/64 |
| 64–127 | −6 (přechod) | 13/64 |
| 128–511 | −6 | 0/64 všude |

**Závěr:** do vysílaného signálu bylo uprostřed rámce vloženo ~6 symbolů
(~0,19 s) navíc — od toho místa je datový proud opět bezchybný, jen posunutý
o 6 bitů vůči pevným symbolovým hodinám přijímače. Nejde o šum ani o chybu
demodulátoru: příčinou byl **xrun (záškub) přehrávání na vysílací straně** —
v době vysílání běžel na Fedoře paralelně plný rebuild projektu (ninja na
všech jádrech CPU), který připravil audio vlákno o čas a vynutil vložení
prázdných/opakovaných vzorků do výstupního proudu. 16-FSK měření (proběhlo
před rebuildem) i druhý 2-FSK rámec byly čisté — časová shoda s buildem
potvrzuje diagnózu.

Offline re-analýza zaznamenaného signálu (`prbs2-mac.wav`,
`modem_cli rx --in prbs2-mac.wav --scheme 2-FSK`) dala identický výsledek
(2 rámce, #1 FAIL, #2 OK) — poškození bylo už v zachyceném signálu, ne
v chování live přijímače nad ním. To zároveň ukázalo na druhou souvislost:
chování při ztrátě/posunu symbolů uprostřed proudu je přesně scénář, který
odhalilo souběžné review kódu `FrameReceiver` (obnova po falešně
zamčené/přerušené preambuli, popsáno v `docs/architecture.md`) — třetí,
zcela ztracený rámec je s touto hypotézou konzistentní.

**Ponaučení:**

1. Během akustického vysílání nespouštět na TX stroji souběžnou zátěž CPU
   (nebo zvýšit prioritu audio vlákna / velikost bufferu zvukového
   zařízení) — xrun na vysílači se do éteru propíše jako vložené/vynechané
   symboly, které pevné symbolové hodiny přijímače (viz
   `docs/architecture.md`, sekce o driftu) neumí překlenout.
2. Je to zároveň ukázková ilustrace, proč reálné akustické/rádiové systémy
   používají průběžné sledování časování (per-symbol tracking) namísto
   jen jednorázového zámku na preambuli — pevné hodiny po chirpu neumí
   opravit díru vzniklou uprostřed přenosu.
3. BER matice pro 2-FSK se plánuje přeměřit v klidu, bez souběžné zátěže
   na vysílacím stroji.

### Souhrn dosavadních měření

| Schéma | FER | BER (z přijatých rámců) | SNR |
|---|---|---|---|
| 16-FSK | 0/8 | 0/8192 | 23,2–24,7 dB |
| 2-FSK | 2/3 (1× xrun, 1× ztracen) | 0/1024 (jediný čistý rámec) | 23–32 dB |
| DBPSK | — | — | měření zatím neproběhlo |
| OOK | — | — | měření zatím neproběhlo |

## Plánovaná měření

- **DBPSK a OOK** — PRBS BER měření analogicky k výše uvedeným (okno bylo
  otevřeno, ale přerušeno nočním klidem před dokončením).
- **Opakování 2-FSK** bez souběžné zátěže na vysílacím stroji, s opraveným
  `FrameReceiver` (viz `docs/architecture.md`) na obou stranách.
- **Matice vzdálenost × hlasitost** — systematické měření SNR/BER v
  závislosti na vzdálenosti reproduktor–mikrofon a nastavené hlasitosti,
  pro všechna čtyři schémata.
- **Směr Mac → Linux** — dosavadní měření šla jen jedním směrem; opačný
  směr ověří, že výsledky nejsou artefaktem konkrétní zvukové cesty jednoho
  stroje.
- **`modem_tap` ping** — end-to-end ICMP přes akustický kanál; vyžaduje
  `sudo` na obou strojích současně, tedy koordinaci s uživatelem (viz
  `docs/dva-agenti.md`).

## Jak reprodukovat

Offline (bez zvukové karty, deterministické):

```sh
modem_cli tx --text "test" --out tx.wav --scheme 16-FSK
modem_cli chansim --in tx.wav --out rx.wav --snr 20 --seed 1
modem_cli rx --in rx.wav --scheme 16-FSK
```

Živě mezi dvěma stroji:

```sh
# přijímač — nahrává si i surový signál pro pozdější rozbor
modem_cli listen --scheme 16-FSK --seconds 120 --record prbs16.wav

# vysílač
modem_cli send --prbs 8 --scheme 16-FSK
```

`listen` vypisuje pro každý přijatý PRBS rámec `BER: chyby/bity`; celkovou
FER a souhrnné BER je potřeba sečíst ručně z výstupu (nebo z uloženého
záznamu přehráním přes `modem_cli rx`). Pro opakovanou offline analýzu
uloženého záznamu stačí `modem_cli rx --in prbs16.wav --scheme 16-FSK` —
užitečné po každé opravě `FrameReceiver`/demodulátoru pro ověření, že
stejný vstup teď dává jiný (lepší) výsledek.
