# zFastSaveLoad

Plugin [Union](https://gitlab.com/union-framework/union-api) dla **Gothic I** (1.08k_mod / wersja Steam), który przyspiesza zapis i wczytywanie gry. Zawiera też profiler (drzewo wywołań i flamegraph), dzięki któremu powstały poniższe optymalizacje.

> **Status:** przetestowany na Edition Final (Steam Workshop) z kilkunastoma innymi pluginami Union. Optymalizacje nie zmieniają formatu plików zapisu.

## Wyniki (Edition Final, ten sam komputer, pomiar A/B w jednej sesji)

| Operacja | Bez pluginu | Z pluginem |
|---|---|---|
| Wczytanie w trakcie gry (F12) | 1,66–1,85 s | **0,95–0,98 s** (−44%) |
| Zapis | 0,59–0,76 s | **0,41–0,43 s** (−43%) |
| Start gry do menu | 12,3 s | **4,6 s** (głównie `playLogoVideos=0`, patrz niżej) |

Wczytywanie i zapis są w pełni jednowątkowe i ograniczone przez CPU (główny wątek liczy przez ~100% czasu, dysk nie jest wąskim gardłem), więc zyski pochodzą z poprawek algorytmów, nie z wielowątkowości.

## Optymalizacje

Zachowanie każdej podmienionej funkcji zostało odczytane z disassemblera `GothicMod.exe` i odtworzone dokładnie (te same wyniki, te same efekty uboczne).

| Przełącznik | Funkcja silnika | Problem | Rozwiązanie |
|---|---|---|---|
| `KeepResources` | `oCGame::LoadSavegame` | Przy wczytywaniu zwalniane są wszystkie tekstury, modele i animacje, po czym te same pliki są czytane ponownie | Na czas wczytywania plugin trzyma dodatkową referencję na zasoby, potem ją zwalnia |
| `FastWaypoints` | `zCWayNet::GetWaypoint` | Wyszukiwanie binarne na **liście łączonej**, więc każdy krok przechodzi listę od początku | To samo wyszukiwanie, z tymi samymi porównaniami i efektem ubocznym, na tablicy |
| `FastAniInit` | `oCAniCtrl_Human::InitAllAnis` | Każdy NPC skleja ~600 nazw animacji i szuka ich w modelu | Cache wyniku dla listy prototypów modelu, zweryfikowany bajt po bajcie z oryginałem (3× przed użyciem, potem co 32. wywołanie; przy różnicy cache dla tego modelu zostaje wyłączony) |
| `FastWorldLists` | `oCWorld::InsertInLists` | Liniowe sprawdzanie, czy obiekt już jest na liście | Zbiór członków listy; odpowiedź „jest” dalej sprawdza oryginalne przejście po liście |
| `FastFileRead` | `zFILE_VDFS::Read` | Archiwizator tekstowy czyta po **1 bajcie** na wywołanie (17 mln wywołań dla `WORLD.ZEN`) | Adaptacyjny bufor odczytu (4→64 KB), włączany po kilku kolejnych odczytach sekwencyjnych |
| `FastStringMap` | `GETStringMap` (BinSafe) | Tablica haszująca ma na sztywno 97 kubełków, więc łańcuchy mają po kilkaset elementów | Pomocniczy indeks obok oryginalnej tablicy. Numery kubełków trafiają do pliku zapisu, więc samej tablicy **nie** powiększamy. |
| `FastFileWrite` | `zFILE_FILE::Write` | 2 mln małych zapisów `fwrite` przy zapisie gry | Bufor zapisu 64 KB (tylko podczas zapisu gry) |
| `FastListHints` *(eksperymentalne, domyślnie 0)* | `zCListSort::InsertSort` | Wstawianie posortowane na listach obiektów świata | Przeszukiwanie od ostatnio wstawionego węzła |
| `FastLineRead` *(eksperymentalne, domyślnie 0)* | `zCArchiverGeneric::RestoreStringEOL` | Tekstowy ZEN czytany jednym wirtualnym `Read` na bajt | Bajty prosto z bufora odczytu, te same reguły końca linii |
| `FastSkipRemove` *(eksperymentalne, domyślnie 0)* | `oCWorld::RemoveFromLists` | Skany 3 list dla przedmiotów, których na nich nie ma | Pominięcie, gdy zbiory członków mówią „nie ma” |

Przy profilowaniu podjąłem dwie próby, które zostały wycofane, bo prowadziły do crashy:
- przyspieszenie `HasWaypoint`;
- wstawianie z podpowiedzią na liście punktów drogi. Komparator `SortWpList` ma efekty uboczne, więc wymaga dokładnie oryginalnej sekwencji porównań.

## Konfiguracja

Ustawienia są w `System\GOTHIC.INI`, w sekcji `[ZFASTSAVELOAD]`. Klucze pojawiają się tam same po pierwszym uruchomieniu gry. Edytuj je przy **wyłączonej** grze, bo Gothic przy wyjściu nadpisuje `GOTHIC.INI`.

```ini
[ZFASTSAVELOAD]
; 0 = wyłączone, 1 = włączone, 2 = A/B (na zmianę wyłączone/włączone przy kolejnych zapisach i wczytaniach, do pomiarów)
KeepResources=1
KeepResourcesOnChangeLevel=0     ; to samo przy zmianie poziomu (więcej RAM-u w trakcie)
FastWaypoints=1
FastAniInit=1
FastWorldLists=1
FastFileRead=1
FastStringMap=1
FastFileWrite=1
FastListHints=0                  ; eksperymentalne
FastSkipRemove=0                 ; eksperymentalne
FastLineRead=0                   ; eksperymentalne: czytanie linii plików tekstowych (ZEN) prosto z bufora
; 0 = wyłączony, 1 = drzewo wywołań + profiler próbkujący (flamegraph), 3 = drzewo + hooki na każdy odczyt/zapis
Profiler=0
```

Dodatkowo (ustawienie gry, nie pluginu): `playLogoVideos=0` w sekcji `[GAME]` pomija filmy intro, co skraca start o ~8 s.

## Profiler

Przy `Profiler=1` po każdym zapisie i wczytaniu do `System\zFastSaveLoad.log` trafia drzewo wywołań z czasami i statystyki optymalizacji. Do `System\zFastSaveLoad_prof\` trafiają próbki stosu (~500–1000 na sekundę), także z całego startu gry do pierwszej klatki menu, wraz z czasem CPU wszystkich wątków i operacjami dyskowymi.

```bat
python tools\flame.py deps\gothic-api\ZenGin\Gothic_I_Classic\Names.txt out <dump.txt> [...]
```

Polecenie tworzy interaktywny flamegraph (`out\LOAD.html` itd.) i tabelę najdroższych funkcji. Symbole exe pochodzą z `Names.txt` z gothic-api, a symbole DLL z ich eksportów. `tools\dumpstack.py` czyta stos ze zrzutu crasha Windows (`%LOCALAPPDATA%\CrashDumps`), a `tools\dis.ps1` disassembluje fragment `GothicMod.exe`.

## Praca nad pluginem

- `tools\dev.ps1`: build i wgranie `System\Autorun\zFastSaveLoad_dev.dll`. Kopia dev ma pierwszeństwo: inne kopie pluginu, np. z Workshopu, same się wyłączają. `-Remove` wraca do wersji z Workshopu.
- `toolselease.ps1 "opis"`: build, wysłanie na Workshop i otwarcie strony moda (`-RemoveDev` usuwa kopię dev).
- `tools\profile.ps1`: flamegraphy z dumpów ostatniego uruchomienia gry.

## Instalacja

- Steam Workshop: element prywatny, aktualizowany przez `workshop\update.bat`.
- Ręcznie: skopiuj `zFastSaveLoad.dll` do `Gothic\System\Autorun\`. Wymagany jest Union. Plik VDF nie jest potrzebny.

## Budowanie

Wymagania: Visual Studio 2022 (MSVC x86, CMake, Ninja).

```bat
git clone --recursive <url-repozytorium>
cd gothic-fastsave
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Wynik: `build\zFastSaveLoad.dll` (statyczne union-api i CRT; zależy tylko od systemowych DLL).

## Uwagi techniczne

- Plugin **nigdy** nie wywołuje `Hook::Disable()`. union-api trzyma listę hooków wspólną dla wszystkich pluginów, a jej zmniejszanie `realloc`-iem z CRT innego modułu psuje stertę. Hooki opcjonalne są tworzone dopiero wtedy, gdy są potrzebne.
- Struktury bez blokad są używane tylko na głównym wątku. Bufor odczytu ma blokadę SRW.

## Co jeszcze można zrobić

Pozostałe koszty wczytywania po optymalizacjach:
- parsowanie nagłówków obiektów (`ReadChunkStart` → `PickWord`, ~6%);
- `zCRayCache::~zCRayCache` (liniowe wypinanie z globalnej listy, ~4%);
- sortowanie dialogów w konstruktorze `oCInfoManager` (~4%; sortowanie jest wpisane bezpośrednio w kod konstruktora);
- alokacje w pluginie zParserExtender (~6%).

## Zależności

- [union-api](https://gitlab.com/union-framework/union-api): system hooków (BSD 3-Clause)
- [gothic-api](https://gitlab.com/union-framework/gothic-api): nagłówki ZenGin z adresami dla G1 1.08k_mod
