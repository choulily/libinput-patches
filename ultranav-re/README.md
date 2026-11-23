# ThinkPad UltraNav: driver analysis and Linux port

Reverse-engineering notes and code from the **ThinkPad T410 Synaptics
UltraNav driver**, used to correct the feel of the libinput fork's circular
scrolling and momentum.

## The problem this solved

The momentum and circular scrolling in the libinput fork were implemented
from the *documentation* of `xf86-input-synaptics`. They worked, but felt
wrong: momentum drifted instead of coasting, and circular scrolling was
weaker than on Windows.

Analysing the real driver showed why. The X driver's reimplementation differs
from the original in one important way, and the original has tunings the
documentation does not describe.

## The artifact

| | |
|---|---|
| File | `6hgx75ww.exe` — ThinkPad T410 UltraNav driver |
| Source | <https://download.lenovo.com/pccbbs/mobiles/6hgx75ww.exe> |
| SHA-256 | `088aae829fd35c65be4b00b193d948a0f49d6397f8d0992883d991057bd6bbe5` |
| Version | Synaptics 16.2.19.7, dated 2013-04-25 |
| Format | Inno Setup 5.4.2; `innoextract` unpacks it |

Reproduce:

```sh
curl -O https://download.lenovo.com/pccbbs/mobiles/6hgx75ww.exe
innoextract -e -d extracted 6hgx75ww.exe
```

## Findings

- [`findings/00-driver-inventory.md`](findings/00-driver-inventory.md) —
  what is in the package, where the settings actually live, the complete
  configuration name list recovered from `SynTPEnh.exe`, the float constants
  decoded from the scroll routine, and the authoritative coasting model.
- [`findings/01-ported.md`](findings/01-ported.md) — what was ported, the
  friction scaling bug found while doing it, and what was deliberately not
  ported.

## Headline numbers

| Setting | Original value | Used for |
|---|---|---|
| `CoastingFriction` | 50 events/s² | momentum deceleration |
| `CoastingSpeed` | 20 events/s | engagement gate |
| `ScrollingSmoothnessFactor` | 1..6 (clamped) | master smoothness |
| `ChiralWheelGain` | 4 | circular, wheel mode |
| `ChiralPageGain` | 256 | circular, page mode |
| `CircScrollDelta` | 0.1 rad | angle per step |

## The UI recreation

`ui/` rebuilds the original UltraNav control panel on Linux from the
driver's own declarations rather than a hand-drawn mock-up:

- **2927 controls across 14 tabs**, with the original pixel geometry,
  control types, caption ids, tab order and enable/disable dependency
  expressions, taken from `synpd.inf`.
- **1053 English strings** from the `STRINGTABLE` resources of
  `SynTPRes.dll`; the same data exists for 17 other languages.
- **139 original bitmaps** from `SynTPRes.dll`, including the animated
  speed indicators.

```sh
cd ui
cmake -B build && cmake --build build
./build/bin/ultranav-settings --data-dir ./build/ui
./build/bin/ultranav-settings --data-dir ./build/ui --dump   # verify layout
```

## Licence note

The extracted strings, bitmaps and layout data are **Synaptics/Lenovo
copyrighted material**, reproduced here for interoperability analysis. The
code in `ui/` and the analysis text are GPL-2.0-or-later. Do not redistribute
the extracted assets as your own work.
