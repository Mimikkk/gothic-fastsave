# zFastSaveLoad

Plugin [Union](https://gitlab.com/union-framework/union-api) dla **Gothic I** (1.08k_mod / wersja Steam), którego celem jest skrócenie czasu zapisu i wczytywania gry.

> **Status: eksperymentalny, nieprzetestowany w grze.** Pierwsza wersja służy głównie do pomiarów. Używasz na własne ryzyko. Plugin nie modyfikuje plików zapisu.

## Co robi

### 1. Profiler zapisu i wczytywania
Plugin hookuje około 50 funkcji silnika ZenGin biorących udział w zapisie, wczytywaniu i zmianie poziomu:

- `oCGame::WriteSavegame`, `LoadSavegame`, `ChangeLevel`, `LoadWorldStat` / `LoadWorldDyn`, `ClearGameState`
- `zCWorld::LoadWorld` / `SaveWorld`, `LoadBspTree`, `DisposeWorld`, `zCWayNet::Unarchive`
- `zCParser::SaveGlobalVars` / `LoadGlobalVars`, `oCSavegameManager::*`
- ładowanie zasobów: tekstury, meshe, progmeshe, MDS/MAN/MDM, dźwięki, NPC (`Unarchive`, `InitModel`)
- wejście/wyjście plikowe: `zFILE_VDFS::Open/Read`, `zFILE_FILE::Open/Read/Write` (liczba wywołań i bajty)

Po każdej operacji do `System\zFastSaveLoad.log` trafia raport z:
- drzewem wywołań z czasami i chwilą startu,
- zagregowanymi statystykami gorących funkcji i 8 najwolniejszymi plikami w każdej kategorii,
- czasem pierwszych 300 klatek po wczytaniu (przycięcia przy leniwym doczytywaniu zasobów).

Przykładowy fragment raportu:
```
===== 22:51:03  LOAD slot 12  [KeepResources=ON]  TOTAL 6342.1 ms =====
    2101.4 ms  @    0.3  oCGame::LoadSavegame
     812.0 ms  @   10.1    oCGame::ClearGameState
  ...
  -- aggregated (inclusive, may overlap) --
  zCTexture::LoadResourceData   calls=   412  total=  934.2 ms
```
*(liczby ilustracyjne)*

### 2. Optymalizacja `KeepResources`
Przy wczytywaniu zapisu silnik usuwa cały świat. Tekstury, modele i animacje, z których nic już nie korzysta, są zwalniane, a chwilę później te same pliki są ponownie czytane i dekodowane z archiwów VDF/MOD.

Plugin na czas wczytywania trzyma dodatkową referencję na już załadowane zasoby: `zCTexture`, `zCProgMeshProto`, `zCModelMeshLib`, `zCSoundFX` i `zCModelPrototype`. Dzięki temu silnik znajduje je po nazwie i używa ponownie. Po wczytaniu referencje są zwalniane, więc to, czego nowy świat nie potrzebuje, zostaje normalnie usunięte.

Zabezpieczenie: jeśli silnik usunie obiekt z pominięciem `Release()`, hook na `zCClassDef::ObjectDeleted` i destruktor prototypu wykreśla go z listy, więc plugin nie zwolni go drugi raz.

## Konfiguracja

Plik `System\zFastSaveLoad.ini`, czytany przy każdym wczytaniu, więc zmiany działają bez restartu gry:

```ini
[FASTSAVELOAD]
; 0 = wyłączone, 1 = włączone, 2 = A/B (na zmianę OFF/ON, do pomiarów)
KeepResources=2
; To samo przy zmianie poziomu. Zwiększa chwilowe zużycie RAM.
KeepResourcesOnChangeLevel=0
; 1 = raport do SystemzFastSaveLoad.log, 0 = profiler wyłączony (mniejszy narzut; czytane przy starcie gry)
Profiler=1
```

## Instalacja

Skopiuj:
- `zFastSaveLoad.dll` do `Gothic\System\Autorun\`
- `zFastSaveLoad.ini` do `Gothic\System\`

Wymagany jest Union (np. z Workshopu). Plik VDF nie jest potrzebny.

`toggle_zFastSaveLoad.bat` włącza i wyłącza plugin, zmieniając nazwę DLL.

## Budowanie

Wymagania: Visual Studio 2022 (MSVC x86, CMake, Ninja).

```bat
git clone --recursive <url-repozytorium>
cd gothic-fastsave
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Wynik: `build\zFastSaveLoad.dll`. DLL jest linkowany statycznie (union-api jako lib, statyczne CRT) i zależy tylko od `KERNEL32` i `dbghelp`.

## Steam Workshop

`workshop\publish.bat` tworzy nowy element Workshopu. Uploader publikuje go jako **Ukryty**, czyli widoczny tylko dla autora. `workshop\update.bat` aktualizuje istniejący element; ID elementu jest wpisane w skrypcie.

## Plany

- [ ] Zebrać pomiary z prawdziwej gry i potwierdzić, która faza dominuje.
- [ ] Pomijanie ponownego ładowania statycznego świata (`LoadWorldStat`), gdy zapis jest z tej samej lokacji.
- [ ] Przyspieszenie zapisu (do ustalenia po pomiarach: archiwizacja świata, kopiowanie slotu, miniatura).

## Zależności

- [union-api](https://gitlab.com/union-framework/union-api): system hooków (BSD 3-Clause)
- [gothic-api](https://gitlab.com/union-framework/gothic-api): nagłówki ZenGin z adresami dla G1 1.08k_mod
