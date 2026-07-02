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

**2-FSK — `send --prbs 2` (2 rámce à 128 B):**

| Rámec | CRC | SNR (dB) | BER |
|---|---|---|---|
| 1 | FAIL | 23,3 | 436/1024 = 0,426 |
| 2 | OK | 31,8 | 0/1024 |

**FER 1/2.** BER 0,426 u rámce #1 odpovídá prakticky náhodným datům, což
při SNR 23 dB není vysvětlitelné šumem — ukazuje na ztrátu zarovnání
symbolových hodin, ne na chybu jednotlivých bitů.

*Pozn. (oprava z rána 2026-07-02):* přijímací strana původně čekala tři
rámce a reportovala „rámec #3 nenalezen“ — ráno se z TX logu ukázalo, že
před nočním přerušením byly odvysílány jen **dva** rámce. Třetí rámec tedy
nebyl ztracen kanálem ani přijímačem; skóre výše je opravené.

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
v chování live přijímače nad ním. Ranní ověření opraveným `FrameReceiver`
(obnova po falešně zamčené/přerušené preambuli, popsáno v
`docs/architecture.md`) na témže záznamu dává rovněž 2 rámce — v nahrávce
žádný falešný lock není a oprava tuto vlastnost kryje samostatným regresním
testem, nikoli tímto měřením.

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

### PRBS BER měření #2 — Linux → Mac, dopoledne 2026-07-02

Stejné podmínky (stejná místnost, hlasitost ~70 %), tentokrát **bez
souběžné zátěže na vysílacím stroji** (poučení z xrunu výše). Naslouchací
okna 240 s.

**DBPSK — 3 rámce à 128 B:**

| Rámec | CRC | SNR (dB) | BER |
|---|---|---|---|
| 1 | OK | 18,5 | 0/1024 |
| 2 | OK | 20,0 | 0/1024 |
| 3 | OK | 18,5 | 0/1024 |

**FER 0/3, BER 0/3072** — čisté i při SNR o ~5 dB nižším, než měla noční
16-FSK měření.

**OOK — 3 rámce à 128 B:**

| Rámec | CRC | SNR (dB) | BER |
|---|---|---|---|
| 1 | OK | 21,6 | 0/1024 |
| 2 | OK | 21,7 | 0/1024 |
| 3 | OK | 22,3 | 0/1024 |

**FER 0/3, BER 0/3072.**

Metodické poznámky z tohoto kola:

1. **Velikost naslouchacího okna:** u schémat s 1 bit/symbol trvá jeden
   128B rámec ~36 s; okno 150 s nestačí na handshake (Dropbox latence
   v řádu jednotek až desítek sekund) + 3 rámce s mezerami — první běh
   DBPSK stihl jen rámec #1 (CRC OK, BER 0/1024, SNR 19,0 dB; do statistik
   výše nezapočten). Pro pomalá schémata používáme okno **240 s**.
2. **Limit TX bufferu:** `send --prbs 3` u 1bit/symbol schémat překročí
   kapacitu výstupního ring bufferu (5,1 M > 4,19 M vzorků) a od
   zapracování oprav z code review korektně **odmítne vysílat** místo
   tichého oříznutí. Tři rámce se proto vysílají jako `--prbs 2` +
   `--prbs 1` bezprostředně za sebou (mezera ~2 s navíc mezi rámci #2
   a #3 — na BER/rámec nemá vliv).

**2-FSK retest (3. běh, okno 300 s):**

| Rámec | CRC | SNR (dB) | BER |
|---|---|---|---|
| 1 | OK | 40,0–41,4 | 0/1024 |
| 2 | OK | 40,0–41,4 | 0/1024 |
| 3 | OK | 40,0–41,4 | 0/1024 |

**FER 0/3, BER 0/3072.** Tím je matice Linux → Mac kompletní a noční
FER 1/2 u 2-FSK definitivně přisouzen TX xrunu, nikoli modemu.

Cestou k tomu výsledku vyšla třetí metodická poznámka: 2. běh retestu
zachytil jen 1 rámec (čistý, SNR 32,4 dB), protože vysílání začalo až
~186 s po otevření 240s okna — latence propagace handshake statusu přes
Dropbox je proměnná v řádu až minut a okno musí krýt nejhorší případ
(od té doby 300 s), případně handshake vést jiným kanálem.

### Souhrn dosavadních měření

| Schéma | FER | BER (z přijatých rámců) | SNR | Kolo |
|---|---|---|---|---|
| 16-FSK | 0/8 | 0/8192 | 23,2–24,7 dB | #1 (noc) |
| DBPSK | 0/3 | 0/3072 | 18,5–20,0 dB | #2 (dopoledne) |
| OOK | 0/3 | 0/3072 | 21,6–22,3 dB | #2 (dopoledne) |
| 2-FSK | 0/3 | 0/3072 | 40,0–41,4 dB | #2 (retest; kolo #1: FER 1/2 kvůli TX xrunu) |

### Směr Mac → Linux — nález: hlasové DSP v capture cestě (probíhá)

První pokus o opačný směr (16-FSK, 3 rámce) skončil **0 nalezenými rámci**,
přestože záznam vysílání obsahoval — a vedl k nálezu, který stojí za
zdokumentování jako samostatné ponaučení:

Výchozím nahrávacím zařízením na Fedoře byl **mikrofon webkamery**
(konferenční kamera Cisco) s vestavěným hlasovým DSP (AGC, potlačení
šumu, echo cancellation). Takový řetězec ničí přesně to, na čem modem
stojí: **transientní preambule projde** (warmup tón i chirp jsou v
záznamu vidět), ale **stacionární FSK tóny potlačovač šumu průběžně
„odečítá"** — amplituda pumpuje 5–15×, spektrum datové části je rozmazané
mimo tónovou mřížku a korelace chirpu klesne z obvyklých ~0,8 na ~0,09.
Selhání se projeví jako „vysílání je slyšet, ale receiver nikdy nezamkne".

Po přepnutí na vestavěný mikrofon (bez DSP) tentýž směr okamžitě funguje
(lokální ověření: CRC OK, SNR 30,8 dB; první rámce z macu: BER ~2 % při
SNR 13,4 dB — nízká úroveň, ladí se hlasitost/zisk, chybové pozice se mezi
rámci opakují ⇒ frekvenčně selektivní útlum cesty, ne šum).

**Ponaučení: před měřením vždy ověřit capture cestu lokálním loopback
testem vzduchem** — a nepoužívat „chytrá" konferenční zařízení.
Související opravy v CLI: `--device` přijímá i část jména zařízení
(indexy se přeskupují, když se objeví/ztratí síťová zařízení typu
AirPlay) a `listen` má 5s watchdog na zařízení, které nedodává vzorky.

### Směr Mac → Linux — výsledky (po vyladění úrovně)

Po zvýšení úrovně TX (mac: hlasitost 90 %, `--amp 0.85`) a natočení RX
notebooku ke zdroji (mikrofonní pole má směrovost — viz plán měření
orientace):

| Schéma | FER | BER | SNR | Pozn. |
|---|---|---|---|---|
| 16-FSK | 0/3 | 0/3072 | 21,4–21,5 dB | |
| 2-FSK | 0/3 | 0/3072 | 29,7–32,2 dB | |
| DBPSK | 0/3 | 0/3072 | 11,8–12,3 dB | čisté i při nejnižším SNR dne |
| OOK | 0/6 | 0/6144 | ~16 dB | offline po opravě demodulátoru (viz níže) |

Poznámka k 16-FSK: první pokus na plné úrovni zachytil 2/3 rámců čistě
a třetí ztratil — uživatel během něj otáčel RX notebook (časově proměnný
kanál). Opakování se stabilní polohou dalo 0/3. Mezikrok při nízké úrovni
(SNR 13,4 dB) dával BER ~2 % s chybami na opakovaných pozicích
(frekvenčně selektivní útlum) — 16-FSK potřebuje ~18+ dB rezervu.

### Nález: dozvuk místnosti vs. OOK (a oprava demodulátoru)

OOK jako jediné schéma zpočátku selhalo úplně (0 rámců při silném i
slabém signálu), a to **jen ve směru Mac → Linux**. Rozbor záznamů:

- Preambule (warmup + chirp) v záznamu čistá, klíčování 1200 Hz jasně
  viditelné — receiver se přesto nikdy nezamkl na SYNC.
- Příčina: **dozvuk místnosti**. Nosná po ON symbolu doznívá ~20 dB za
  60 ms; izolovaná nula po jedničce (SYNC 0x2DD4 je obsahuje) měla přes
  celé symbolové okno energii jen ~7 dB pod ON, zatímco adaptivní práh
  ležel −10 dB pod ON → nula se četla jako jednička → SYNC nikdy
  nesedl.
- Proč to ve směru Linux → Mac prošlo: na macu byl signál slabší
  (SNR ~22 dB) a dozvukový ocas zapadl pod šum mikrofonu. Na Fedoře
  (citlivější mikrofon, SNR >30 dB) ocas nad šumem přežil — **příliš
  čistý signál OOK paradoxně škodil**. Snížení hlasitosti nepomohlo:
  dozvuk škáluje se signálem.

Oprava demodulátoru (`src/modem/ook.cpp`), laděná offline proti dvěma
reálným nahrávkám (hlasité i tiché) se známou PRBS:

1. **Energie z druhé poloviny symbolového okna** — dozvuk z předchozího
   symbolu doznívá na jeho začátku; ON symbol nese nosnou celou dobu,
   takže se nic neztrácí. (Samotné: SYNC začal zamykat, ale začátek
   payloadu měl stále chyby 0→1.)
2. **Práh vážený k ON**: `thr = on^0,7 · off^0,3` (v dB ~4–7 dB pod ON)
   místo geometrického průměru. OFF energie je bimodální směs hlubokých
   nul a dozvukových ocasů — EMA průměr leží mezi nimi a symetrický práh
   nechával ocasy nad sebou. ON je stabilní (±2–3 dB), posun prahu
   k ON jedničky neohrožuje.
3. **Seed OFF úrovně −13 dB** od referenčního symbolu (dřív −20 dB) —
   práh je rozumný od prvního symbolu, EMA doladí.

Po opravě obě nahrávky dekódují 3/3 rámců s CRC OK (BER 0), včetně
rámce, který původní receiver ani nedetekoval. Regresní data: nahrávky
jsou archivovány na RX stroji (`~/builds/acoustic-modem/recordings/`).
Obecné ponaučení pro zprávu: OOK bez reference (druhého tónu či fáze)
je strukturálně citlivé na kanál s pamětí — dozvuk je forma
mezisymbolové interference, kterou FSK/DBPSK řeší už konstrukcí.

## Plánovaná měření

- **Dokončení Mac → Linux** — po vyladění úrovně (hlasitost mac /
  mic gain Fedora) celá matice 16-FSK, 2-FSK, DBPSK, OOK.
- **Orientace × SNR** — mikrofonní pole notebooku má patrně beamforming
  (citlivost mimo osu displeje klesá): po natočení RX notebooku ke zdroji
  vyskočilo SNR o několik dB. Mini-měření: stejná hlasitost, několik úhlů
  natočení, SNR/BER na 16-FSK.
- **Matice vzdálenost × hlasitost** — systematické měření SNR/BER v
  závislosti na vzdálenosti reproduktor–mikrofon a nastavené hlasitosti,
  pro všechna čtyři schémata.
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
