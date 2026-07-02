# Akustický modem — závěrečná zpráva

*Školní projekt, uzavřeno 2. července 2026.*

## 1. Cíl a výsledek jednou větou

Cílem bylo postavit od nuly kompletní digitální komunikační systém, který
přenáší data zvukem mezi dvěma běžnými notebooky (reproduktor → vzduch →
mikrofon), a na reálném kanálu ho změřit. Výsledek: **všechny čtyři
implementované modulace přenášejí data obousměrně s nulovou naměřenou
bitovou chybovostí a nad systémem funguje i IP vrstva — obousměrný ping
vzduchem s RTT ~13 s.**

## 2. Co bylo postaveno

Celý řetězec komunikačního systému v malém (detailně
[`architecture.md`](architecture.md), parametry [`protocol.md`](protocol.md)):

- **DSP jádro:** Goertzelův detektor, FIR filtry, chirp korelátor pro
  synchronizaci, simulátor kanálu (šum, drift hodin, echo) pro
  deterministické offline testy.
- **Čtyři modulace** za společným rozhraním: 2-FSK, OOK, DBPSK (1 bit/symbol,
  ≈31 bit/s) a 16-FSK (4 bity/symbol, ≈125 bit/s), Grayovo kódování.
- **Rámcový protokol:** preambule (warmup tón + chirp), SYNC slovo,
  hlavička s CRC-16, payload do 256 B s CRC-16.
- **Reálné audio I/O** (miniaudio), CLI (`modem_cli`: tx/rx/chansim/
  send/listen/devices, PRBS měřicí režim) a GUI (`modem_gui`: waterfall,
  konstelace, metriky kvality).
- **Síťová vrstva (`modem_tap`):** TUN/TAP rozhraní, CSMA MAC
  s carrier-sense a backoffem — IP pakety jedou zvukem jako běžnou linkou.
- **Testy:** 41 testovacích sad, 1116 assertions, včetně regresních testů
  vzniklých z reálných nálezů (obnova po falešném chirp locku, dozvukový
  OOK scénář).

Vývoj probíhal paralelně na dvou strojích (Fedora + MacBook) dvěma
koordinovanými AI agenty; protokol spolupráce přes sdílený adresář
je popsán v [`dva-agenti.md`](dva-agenti.md) — mj. code review druhým
agentem odhalilo před měřením 17 reálných chyb.

## 3. Měření na reálném kanálu

Metodika (PRBS-15 rámce, BER XORem proti známé sekvenci, surové záznamy
pro offline analýzu, handshake oken přes sdílený stav) a kompletní data
jsou v [`measurements.md`](measurements.md). Podmínky: dva notebooky ve
stejné místnosti, vestavěné reproduktory a mikrofony.

**Souhrnná obousměrná matice (FER / BER / typické SNR):**

| Schéma | Linux → Mac | Mac → Linux |
|---|---|---|
| 2-FSK | 0/3 · 0/3072 · 40–41 dB | 0/3 · 0/3072 · 30–32 dB |
| OOK | 0/3 · 0/3072 · 22 dB | 0/6 · 0/6144 · ~16 dB¹ |
| DBPSK | 0/3 · 0/3072 · 18,5–20 dB | 0/3 · 0/3072 · **12 dB** |
| 16-FSK | 0/8 · 0/8192 · 23–25 dB | 0/3 · 0/3072 · 21,5 dB |

¹ offline z raw záznamů po opravě demodulátoru (viz §4.3).

**Ani jeden vadný bit z 33 792 přenesených.** Nejrobustnější je DBPSK
(čisté i při 12 dB SNR), 16-FSK potřebuje ~18+ dB, ale nese 4× víc dat.

**IP přes zvuk:** obousměrný ping, Linux → Mac 3/4 paketů (RTT
13,15–13,47 s), Mac → Linux 1/1 (14,06 s). RTT odpovídá teorii
(84 B ICMP ≈ 6–7 s airtime na směr při 125 bit/s). Jedna ztráta =
kolize/backoff; linka je záměrně best-effort, ztráty patří vyšším vrstvám.

## 4. Nejcennější část: co se pokazilo a proč

Každé selhání při měření mělo příčinu **mimo modem** a každé vedlo
k opravě kódu, nebo metodickému ponaučení (forenzní detaily
v [`measurements.md`](measurements.md)):

### 4.1 Xrun na vysílači

2-FSK rámec s BER 0,43 při SNR 23 dB: bloková analýza ukázala 6 vložených
symbolů uprostřed rámce — na vysílači běžel souběžně build a audio vlákno
nestíhalo. Pevné symbolové hodiny (zámek jen na preambuli) díru neumí
překlenout; přesně proto reálné systémy dělají průběžný timing tracking.
*Ponaučení: při vysílání žádná zátěž na stroji; TX přetečení bufferu je
od té doby tvrdá chyba, ne varování.*

### 4.2 „Chytrý" mikrofon

Směr Mac → Linux zprvu nedekódoval vůbec nic, přestože záznam signál
obsahoval: výchozím vstupem byla webkamera s hlasovým DSP (AGC, potlačení
šumu). Transientní preambule prošla, stacionární FSK tóny potlačovač
průběžně „odečítal". *Ponaučení: capture cestu vždy ověřit lokálním
loopbackem; konferenční zařízení jsou pro datové přenosy nepoužitelná.
Oprava: `--device` podle jména + watchdog v `listen`.*

### 4.3 Dozvuk místnosti vs. OOK

OOK selhalo jen ve směru s citlivějším mikrofonem — a paradoxně tím víc,
čím silnější byl signál. Nosná po ON symbolu v místnosti doznívá
(~20 dB/60 ms) a izolované nuly po jedničkách měly energii nad rozhodovacím
prahem; slabší signál na macu měl ocas utopený v šumu, silný signál na
Fedoře ne. Dozvuk je forma mezisymbolové interference a OOK jako jediné
schéma nemá vlastní referenci (FSK druhý tón, DBPSK předchozí symbol).
*Oprava demodulátoru (energie z druhé poloviny symbolu, práh vážený k ON)
laděná offline proti archivovaným záznamům — po ní obě nahrávky dekódují
3/3 rámců čistě.*

### 4.4 Drobnější, ale poučné

- **Falešný chirp lock** (nález z code review, reprodukovaný testem):
  obnova po přerušené preambuli uměla zahodit následující pravý rámec.
- **macOS stealth mode** tiše zahazoval ICMP — paket prokazatelně dorazil
  do kernelu, odpověď nevznikla.
- **Výchozí timeout pingu (~10 s) je kratší než RTT linky (~13 s)** —
  „ztracené" odpovědi reálně doletěly po timeoutu.
- **Latence koordinace přes sdílený adresář** je proměnná (sekundy až
  minuty) — naslouchací okna musí krýt nejhorší případ.
- **Směrovost mikrofonního pole:** natočení notebooku ke zdroji zvedlo
  SNR o jednotky dB (kvalitativní pozorování).

## 5. Omezení a možná pokračování

- Žádná FEC ani ARQ — jeden vadný symbol zahodí celý rámec; konvoluční
  kód nebo aspoň opakování hlavičky by výrazně zvedlo spolehlivost na
  hraně dosahu.
- Pevné symbolové hodiny po chirpu — per-symbol timing tracking by řešil
  xrun scénář i delší rámce.
- Ekvalizace z chirpu: chirp přejíždí celé pásmo, takže z něj lze odhadnout
  frekvenční odezvu kanálu a vážit energie tónů 16-FSK (nápad vznikl při
  měření frekvenčně selektivního útlumu).
- Vyšší rychlosti: vyšší baud, více tónů, případně OFDM.
- TCP přes zvuk (`nc` chat) a delší CSMA testy.

## 6. Závěr

Projekt splnil zadání v plném rozsahu: funkční, změřený a zdokumentovaný
akustický modem se čtyřmi modulacemi, vlastním rámcovým protokolem, GUI
a IP vrstvou. Nulová naměřená chybovost na reálném kanálu je hezké číslo,
ale největší hodnota je v cestě k ní — všechna reálná selhání (xrun,
DSP webkamery, dozvuk, firewall) byla nalezena forenzní analýzou
surových záznamů, reprodukována a opravena, často s regresním testem.
Systém se tak choval přesně jako skutečné komunikační systémy: modem je
jen část příběhu, zbytek je kanál, hardware a operační systém okolo.
