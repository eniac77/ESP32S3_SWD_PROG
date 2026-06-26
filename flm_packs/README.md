# flm_packs/ — vendored CMSIS `.FLM` flash-algoritmusok

Ide kerülnek a céleszközök **`.FLM`** (CMSIS flash-loader) fájljai, amelyekből a
build-time `tools/flm_extract.py` C tömböket generál a `components/flm_blobs/`
számára.

## Mi az a `.FLM`?

A `.FLM` egy **ARM ELF** állomány, amely tartalmazza:

- a flash-műveletek kódját (`PrgCode`, esetleg `PrgData` szekció),
- a CMSIS `FlashDevice` leírót (`DevDsc` szekció: név, flash bázis, méret,
  page-méret, törölt érték, timeoutok, szektor-tömb),
- a belépési pontok szimbólumait (`Init`, `UnInit`, `EraseChip`,
  `EraseSector`, `ProgramPage`, `Verify`).

A loader a célt RAM-jában futtatja ezt a kódot (lásd a terv 8. szekcióját).

## Honnan szerezd be?

ST eszközökhöz a hivatalos **Device Family Pack (DFP)** vagy a
**STM32CubeProgrammer** tartalmazza a `.FLM`-eket:

- Keil/ARM CMSIS-Pack: a `Keil.STM32F4xx_DFP.*.pack` (ZIP) `Flash/` mappájában
  vannak a `*.FLM` fájlok.
- A pack ZIP-ként kibontható; abból másold ide a kívánt `.FLM`-eket.

Család-buildhez a **legkisebb RAM-ú taghoz méretezett** változatot válaszd
(FLM + stack + buffer beférjen a kis RAM-ú variánsokba is — lásd terv 19.).

## Hogyan használja a build?

A CMake build (tervezett bekötés a `components/flm_blobs/CMakeLists.txt`-ben,
egyelőre kommentben) így dolgozza fel:

```cmake
file(GLOB FLM_SOURCES "${CMAKE_SOURCE_DIR}/flm_packs/*.FLM")
add_custom_command(... COMMAND python tools/flm_extract.py ${FLM_SOURCES}
                   -o flm_generated.c --header flm_generated.h ...)
```

Kézi futtatás (teszteléshez):

```bash
python tools/flm_extract.py flm_packs/STM32F4xx_1024.FLM \
    -o components/flm_blobs/flm_generated.c \
    --header components/flm_blobs/flm_generated.h
```

(Függőség: `pip install pyelftools`.)

## Állapot

Jelenleg **nincs vendored `.FLM`** ebben a mappában, ezért a
`components/flm_blobs/flm_blobs.c` ÜRES táblát ad (count = 0) — a build így zöld
marad. Amint ide kerül `.FLM`, aktiváld a `flm_blobs/CMakeLists.txt`-ben a
generátor bekötését.

## Licenc / jogi figyelmeztetés

A `.FLM` fájlok az ST (vagy más gyártó) szerzői joga alá esnek. A repóba
vendorolás előtt ellenőrizd a DFP licencfeltételeit. A `flm_packs/` tartalma
ezért alapból **nincs** verziókövetésbe kommitolva (lásd a projekt `.gitignore`
beállítását, ha van).
