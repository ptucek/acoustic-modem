# Protokol spolupráce dvou agentů (Linux ↔ Mac) přes Dropbox

Tento adresář slouží ke koordinaci dvou Claude agentů pracujících na stejném
projektu ze dvou strojů. Dropbox nemá zámky — konfliktům se vyhýbáme
konvencí: **každý soubor má právě jednoho pisatele**.

## Role

- **linux** (Fedora, primární): vlastní zdrojový kód. Jediný smí editovat
  `src/`, `tests/`, `CMakeLists.txt`, `docs/` a jediný používá git (commituje).
- **mac** (sekundární): staví, testuje, spouští a měří na macOS. Do zdrojáků
  NEzasahuje, pokud mu linux zprávou explicitně nepředá vlastnictví
  konkrétních souborů (a pak je zase vrátí). Výsledky zapisuje do
  `sync/results/`.

## Soubory (pisatel → čtenář)

| Soubor | Píše | Čte |
|---|---|---|
| `sync/inbox-mac.md`    | linux | mac   — zprávy PRO mac (append na konec) |
| `sync/inbox-linux.md`  | mac   | linux — zprávy PRO linux (append na konec) |
| `sync/status-linux.md` | linux | mac   — stav práce, přepisuje se celý |
| `sync/status-mac.md`   | mac   | linux — stav práce, přepisuje se celý |
| `sync/results/*-mac.md`| mac   | oba   — výsledky měření/testů z macu |

## Formát zprávy v inboxu (append-only)

```markdown
## MSG <pořadové číslo> | <ISO 8601 čas> | stav: nová
**Věc:** krátké shrnutí
Tělo zprávy…
```

**Příjemce do cizího inboxu NIKDY nezapisuje** (ani stav, ani odpověď) —
souběžný append odesílatele by editaci přepsal (Dropbox = last writer
wins; ověřeno v praxi 2026-07-02, viz MSG 5). Potvrzení se dělá zprávou
v opačném směru: `**ACK MSG <n>:** krátká odpověď…` (jako běžná zpráva
nebo úvodní řádek další zprávy). Každý soubor má tedy PRÁVĚ jednoho
pisatele, bez výjimek. Zpracované zprávy si příjemce eviduje sám (stačí
poslední ACKnuté číslo ve vlastním status souboru).

## Zásady

1. **Než začneš pracovat, přečti si svůj inbox a status protistrany.**
2. Po každém uzavřeném celku aktualizuj svůj status soubor (co děláš,
   výsledek posledního buildu/testů, čas).
3. Build adresář drž MIMO Dropbox (`cmake -B ~/builds/acoustic-modem -S <repo>`)
   — objektové soubory nesynchronizovat, mezi platformami stejně nejsou
   přenositelné.
4. Git: jen linux. Mac bere pracovní strom Dropboxu jako zdroj pravdy.
5. Vznikne-li přesto konfliktní kopie Dropboxu („conflicted copy"), řeší ji
   linux; mac na ni jen upozorní zprávou.
6. Koordinace živých rádiových testů (vzduchem): navrhovatel pošle do
   protisměrného inboxu plán (kdo vysílá, schéma, baud, čas startu), druhá
   strana potvrdí ve svém statusu a spustí `listen`/GUI.

## Bootstrap pro mac agenta

Přečti kořenový `README.md` (sekce „Build na macOS"), postav projekt do
`~/builds/acoustic-modem`, spusť `modem_tests` a výsledek zapiš do
`sync/status-mac.md`. Pak zpracuj `sync/inbox-mac.md`.
