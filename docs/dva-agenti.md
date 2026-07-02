# Dva agenti: spolupráce Linux ↔ Mac přes Dropbox

Nezvyklá, ale velmi praktická součást projektu: kód i měření vznikaly za
spolupráce dvou samostatných Claude agentů, každý na jiném fyzickém stroji
(Fedora a MacBook Pro), synchronizovaných přes sdílenou Dropbox složku.
Akustický modem potřebuje ke smysluplnému testování *dvě* zařízení s
reproduktorem a mikrofonem — jeden agent nemůže otestovat přenos vzduchem
sám se sebou. Tento dokument je narativní shrnutí pro zprávu; normativní
pravidla protokolu jsou v `sync/README.md`.

## Role

- **linux** (Fedora, primární) — vlastní zdrojový kód. Jediný edituje
  `src/`, `tests/`, `CMakeLists.txt`, `docs/` a jediný používá git
  (commituje).
- **mac** (sekundární) — staví, testuje, spouští a měří na macOS. Do
  zdrojáků nezasahuje, pokud mu linux výslovně nepředá vlastnictví
  konkrétních souborů. Výsledky měření zapisuje do `sync/results/`.

Rozdělení odpovídá reálné situaci: úpravy kódu by při editaci ze dvou stran
současně kolidovaly (Dropbox nemá zamykání souborů), zatímco build a měření
jsou přirozeně vázané na konkrétní fyzický stroj.

## Jeden pisatel na soubor

Dropbox synchronizuje soubory jako celek a při souběžné úpravě týž soubor
na obou stranách jednoduše **přepíše tím, kdo zapsal později** (last writer
wins) — bez sloučení, bez varování. Aby ke kolizi nemohlo dojít, platí
pravidlo: **každý soubor má právě jednoho pisatele.**

| Soubor | Píše | Čte |
|---|---|---|
| `sync/inbox-mac.md` | linux | mac (zprávy PRO mac, jen append) |
| `sync/inbox-linux.md` | mac | linux (zprávy PRO linux, jen append) |
| `sync/status-linux.md` | linux | mac (aktuální stav, přepisuje se celý) |
| `sync/status-mac.md` | mac | linux (aktuální stav, přepisuje se celý) |
| `sync/results/*-mac.md` | mac | oba (výsledky měření z macu) |

## ACK zprávy místo zápisu do cizího inboxu

Zprávy v inboxech jsou append-only bloky (`## MSG <n> | <čas> | stav: ...`).
Přirozeně by se nabízelo, aby příjemce po zpracování zprávy přepsal její
stavový řádek na „hotovo" přímo v cizím inboxu — jenže to je přesně zápis
do souboru, který píše ten druhý. Řešení: **potvrzení jde jako nová zpráva
ve vlastním směru** (`**ACK MSG <n>:** ...`), ne jako úprava zprávy
protistrany. Kdo zprávu zpracoval, si poznamená poslední ACKnuté číslo ve
vlastním status souboru — stav zpracování je tedy vždy jen v souboru, který
daná strana sama vlastní.

## Handshake pro rádiové (akustické) testy

Živý test přenosu vzduchem vyžaduje, aby přijímací strana poslouchala
přesně v okamžiku, kdy druhá strana vysílá — a Dropbox synchronizace má
proměnnou latenci v řádu sekund, takže domluva na pevný čas by byla
nespolehlivá. Použitý handshake:

1. Strana, která bude naslouchat, spustí `modem_cli listen` a
   *bezprostředně poté* zapíše do svého status souboru řádek typu
   `Naslouchám: od <čas> po 90 s (test #1, 2-FSK)`.
2. Druhá strana svůj status pravidelně kontroluje; jakmile zachytí změnu,
   do ~10 s zahájí vysílání odpovídajícím schématem.
3. Výsledek (CRC, SNR, dekódovaný text nebo BER) zapíše naslouchající
   strana do `sync/results/`.

Tenhle mechanismus fungoval spolehlivě i pro vícekrokové scénáře (např.
přepnutí schématu uprostřed série testů) — klíčové bylo vždy napsat status
*před* spuštěním posluchače, ne po něm, jinak by druhá strana mohla začít
vysílat do prázdna.

## Poučení z last-writer-wins konfliktu

V praxi došlo přesně k tomu, čemu měl protokol předejít: mac agent ve své
odpovědi omylem přepsal stavový řádek zprávy přímo v `inbox-linux.md`
(tedy v souboru, který vlastní linux) — souběžně s tím, jak linux do téhož
souboru appendoval novou zprávu. Dropbox konflikt tiše vyřešil podle
času zápisu a část editace linuxu zmizela. Bezprostředně po odhalení byl
protokol zpřísněn na aktuální podobu (žádná strana needituje cizí inbox,
potvrzení jen jako nová zpráva ve vlastním souboru) a incident je od té
doby explicitně zaznamenaný v `sync/README.md` jako zdůvodnění pravidla —
ne jen jako abstraktní doporučení, ale jako zdokumentovaná příhoda, proč to
selhalo napoprvé a jak se to opravilo.

## Proč to stálo za to zdokumentovat

Tenhle vedlejší produkt projektu — protokol pro dva nezávislé agenty
koordinující se přes obyčejné sdílené úložiště bez zámků — je zajímavý sám
o sobě: řeší stejnou třídu problémů jako distribuované systémy (souběžný
zápis, jeden vlastník dat, potvrzování zpráv, race podmínky při
handshaku), jen v extrémně jednoduchém prostředí (soubory na disku
synchronizované Dropboxem) a s LLM agenty jako aktéry na obou stranách.
