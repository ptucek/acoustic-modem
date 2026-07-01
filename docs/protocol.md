# Formát rámce akustického modemu

Tento dokument popisuje přesný formát dat, která modem přenáší zvukem.
Implementace: `src/protocol/framer.hpp`, `src/protocol/frame_receiver.hpp`,
`src/dsp/chirp.hpp`, `src/modem/*`.

## Přehled rámce

Rámec se skládá ze dvou částí:

1. **Fyzická preambule** — nemodulovaná, stejná pro všechny modulace.
   Slouží k probuzení přijímače a k přesné synchronizaci časování symbolů.
2. **Modulovaná část** — SYNC slovo, HEADER, PAYLOAD, CRC-16. Tato část už
   je nesená zvolenou modulací (2-FSK, OOK, DBPSK, 16-FSK).

```
[ warm-up 200 ms ][ chirp 100 ms ][ mezera 20 ms ][ SYNC | HEADER | PAYLOAD | CRC ]
        fyzická preambule                              modulovaná data
```

## Fyzická preambule

| Část     | Délka  | Popis |
|----------|--------|-------|
| Warm-up  | 200 ms | Tón 1000 Hz. Ustálí reproduktor, mikrofon a případné AGC v cestě signálu (např. levný USB zvukový adaptér potřebuje čas na náběh). Nenese žádnou informaci, přijímač jej ignoruje. |
| Chirp    | 100 ms | Lineární frequency sweep 800 → 2800 Hz, s Hannovou obálkou na okrajích (potlačení lupanců/spektrálního rozlití). Slouží k detekci začátku rámce a přesné synchronizaci hranice prvního symbolu pomocí normalizované křížové korelace. |
| Mezera   | 20 ms  | Ticho. Oddělí konec chirpu (jehož energie by jinak zasahovala do prvního symbolu) od začátku modulovaných dat. |

**Proč chirp, a ne třeba opakující se tón?** Autokorelace lineárního chirpu
je jedna ostrá, úzká špička — na rozdíl od periodického tónu, který
koreluje sám se sebou při každém posunu o celočíselný násobek periody
(množství falešných špiček). Chirp navíc pokrývá širokou část spektra,
takže korelační špička přežije frekvenčně selektivní odezvu místnosti
(dozvuk, hrany pásma reproduktoru/mikrofonu) lépe než úzkopásmový signál.

Přijímač (`ChirpCorrelator`) korelaci počítá na signálu decimovaném 4×
(48 kHz → 12 kHz přes FIR dolní propust) z výkonových důvodů a teprve
okolí nalezené špičky doupřesní na plném vzorkovacím kmitočtu.

## Modulovaná část

Bitový obsah (SYNC..CRC) sestavuje `Framer::buildBits` a je **shodný pro
všechny modulace** — liší se jen to, jak se výsledné bity převedou na
zvuk. Pořadí bitů v rámci bajtu je **LSB-first** (nejméně významný bit
bajtu se vysílá jako první) — konvence platí pro celý projekt, viz
`core/bits.hpp`.

```
 2 B        1 B          2 B            0–256 B         2 B
┌────────┬──────────┬─────────────┬ ─ ─ ─ ─ ─ ─ ─ ─ ┬──────────┐
│  SYNC  │ ver_flags│ payload_len │     PAYLOAD     │  CRC-16  │
│ 0x2DD4 │          │  (LE)       │                 │  (BE)    │
└────────┴──────────┴─────────────┴ ─ ─ ─ ─ ─ ─ ─ ─ ┴──────────┘
          └─────── HEADER ───────┘
          └── nad těmito 3 B se počítá CRC hlavičky ──┘  ↑ nad
              (viz níže)                                  PAYLOAD
```

### SYNC (16 bitů)

Konstanta `0x2DD4` (`am::kSyncWord`). Zvolena, protože má vyvážený počet
jedniček a nul a nemá dlouhé souvislé běhy stejného bitu — dobrý „seed"
pro adaptivní OOK práh a snadno odlišitelná od náhodného šumu při hledání
korelací v bitovém proudu.

### HEADER (3 bajty)

| Pole          | Velikost | Popis |
|---------------|----------|-------|
| `ver_flags`   | 1 B      | Bity 0–2: typ payloadu (`0` = text/UTF-8, `1` = PRBS testovací vzor, `2` = ethernetový rámec). Bity 3–7: rezerva pro budoucí FEC/whitening (dosud nevyužito, nastaveny na 0). |
| `payload_len` | 2 B, LE  | Délka PAYLOAD v bajtech, 0–256 (`am::kMaxPayload`). |
| header CRC    | 2 B      | CRC-16 (stejný polynom jako CRC payloadu, viz níže) počítané přes prvních **3 bajty** (`ver_flags` + `payload_len`). Umožní přijímači ověřit hlavičku (a tedy důvěryhodnost `payload_len`) dřív, než začne číst PAYLOAD — chybná délka by jinak mohla přijímač uvěznit v čekání na vzorky, které nikdy nedorazí. |

### PAYLOAD (0–256 bajtů)

Syrová uživatelská data. Maximální délka `kMaxPayload = 256 B` je zvolena
jako kompromis: dost dlouhá pro rozumnou zprávu nebo ethernetový rámec s
menším MTU, dost krátká, aby se drift hodin mezi vysílačem a přijímačem
během jednoho rámce neprojevil víc než zlomkem (< 0,5 %) trvání symbolu —
delší rámec by si vyžádal vlastní kompenzaci driftu uvnitř rámce, ne jen
na hranici preambule.

### CRC-16 (2 bajty, big-endian)

CRC-16/CCITT-FALSE: polynom `0x1021`, počáteční hodnota `0xFFFF`, bez
finálního XOR, výsledek se připojuje **big-endian** (horní bajt první).
Počítá se přes **PAYLOAD** (bez SYNC/HEADER — ty mají svou vlastní menší
kontrolu, viz výše). Implementace: `protocol/crc16.hpp`.

### Referenční symbol

Před SYNC se vysílá **jeden referenční symbol** (bity samé jedničky,
konstanta `kRefSymbols` v `core/config.hpp`), jednotně pro všechna
schémata; přijímač jej dekóduje a zahodí. Každé schéma z něj těží jinak:

- **DBPSK** — diferenciální detekce potřebuje fázovou referenci: první
  symbol sám o sobě nenese bit, definuje počáteční fázi, vůči které se
  měří otočení dalších symbolů.
- **OOK** — jedničkové bity = zapnutá nosná, ze které si demodulátor
  nastaví práh rozhodování.
- **FSK/MFSK** — referenci nepotřebují, jeden symbol navíc jim neškodí.

## Tabulka modulací

| Schéma   | Princip | Parametry (výchozí) | Bitů/symbol |
|----------|---------|----------------------|--------------|
| **2-FSK**  | Bit 0 → tón `f0`, bit 1 → tón `f1`. | `f0 = 1200 Hz`, `f1 = 2200 Hz` | 1 |
| **OOK**    | Bit 1 → tón `f0` přítomen, bit 0 → ticho (on–off keying). | `f0 = 1200 Hz` | 1 |
| **DBPSK**  | Bit → otočení fáze nosné o 180° vůči předchozímu symbolu (0° = bit 0, 180° = bit 1). | nosná `f0 = 1200 Hz` | 1 |
| **16-FSK** | 16 ortogonálních tónů, 4 bity/symbol, index tónu Grayovsky kódovaný (sousední tóny se liší jedním bitem — chyba na sousední tón poškodí jen 1 bit). | tóny `1000 + k·62,5 Hz`, `k = 0..15` (tedy 1000–1937,5 Hz) | 4 |

Volba `f0`/`f1`/rozestupu tónů vždy respektuje **ortogonalitu vůči
symbolové rychlosti**: rozestup tónů je celočíselný násobek `baud`
(typicky `2× baud`), aby Goertzelův filtr naladěný na jeden tón měl v
sousedním tónu nulu — sousední tóny se tak nepřelévají do sebe (žádná
mezisymbolová/mezitónová interference při obdélníkovém okně o délce
symbolu).

## Výchozí parametry a jejich zdůvodnění

| Parametr | Výchozí hodnota | Zdůvodnění |
|----------|-----------------|------------|
| Vzorkovací kmitočet | 48 000 Hz | Nativní kmitočet většiny zvukových karet a PipeWire/PulseAudio grafů — žádné převzorkování na vstupu/výstupu (zdroj zkreslení a latence navíc). |
| Symbolová rychlost (baud) | 31,25 Bd | `48000 / 31,25 = 1536` — **celé číslo** vzorků na symbol, žádná kumulující se zaokrouhlovací chyba v časování symbolů. Dost pomalé na spolehlivý přenos přes běžný reproduktor/mikrofon v místnosti s dozvukem. |
| Pásmo | 1–3 kHz | Nad rozsahem výrazného 50/60 Hz síťového brumu a jeho harmonických, pod oblastí, kde běžné malé reproduktory/mikrofony (notebooky, USB headsety) mají nejhorší frekvenční odezvu a směrovost. Zároveň je to pásmo, kde bývá nejméně akustického šumu domácnosti/kanceláře (klimatizace, ventilátory — nízké kmitočty; sykavky řeči — vysoké kmitočty). |
| Amplituda | 0,5 (plná škála 1,0) | Rezerva proti klipování při součtu více tónů (MFSK) nebo při nelinearitě zesilovače/reproduktoru blízko plného rozkmitu. |

## Bitové pořadí — shrnutí

Napříč celým projektem platí: **bity v bajtu se čtou/píšou LSB-first**.
To znamená, že bit s hodnotou `1` (tedy `0x01`) bajtu `0x01` je vyslán
jako první bit. Mnohabitová pole ve struktuře (`payload_len`) jsou
**little-endian** (méně významný bajt první) — s výjimkou závěrečného
CRC-16, které se podle konvence CRC-16/CCITT-FALSE připojuje
**big-endian** (významnější bajt první).
