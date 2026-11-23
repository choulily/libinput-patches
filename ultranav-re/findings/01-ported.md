# What was ported from the T410 driver

## Momentum / coasting — model replaced

| | Before | Now |
|---|---|---|
| Physics | Exponential decay | **Constant deceleration** |
| Parameter | `strength` as a percentage of an arbitrary curve | `CoastingFriction`, scroll events/s², default 50 |
| Engagement | any release above 120 units/s | `CoastingSpeed`, 20 scroll events/s |
| Stop | asymptotic, never crisp | finite, linear tail |

The engagement gate matters as much as the curve. The original documents
20 scrolls/second as "which should prevent you from starting coasting
unintentionally". Ours started at any brisk movement, so ordinary
deliberate pointing flung the cursor — that is the "awkwardly strange"
feeling.

### Friction scaling bug found while porting

The first implementation of constant deceleration produced a **15.8 second**
coast, because `CoastingFriction` is in scroll *events*/s² but the animation
runs in normalized pixel units. Converting by the distance one scroll event
covers (≈19.7 units) gives the original's intended feel:

| strength | friction | coast from a hard 4000 u/s flick |
|---|---|---|
| 50 % | 1968 u/s² | 2.0 s |
| 100 % (default) | 984 u/s² | 4.1 s |
| 150 % | 656 u/s² | 6.1 s |

## Smoothness — new

`ScrollingSmoothnessFactor`, clamped to **1..6** by the original's own code,
now applies a low-pass filter to scroll deltas (α from 0.35 at 1 to 0.95 at
6). This is the master "how does scrolling feel" knob and was missing
entirely; per-method multipliers cannot substitute for it.

## Circular scrolling — gains added

The original calls it **ChiralMotion** and scales output by a gain that
differs by scroll mode: `ChiralWheelGain` = 4, `ChiralPageGain` = 256.
Ours emitted a fixed step distance, which is why it felt weak compared with
the driver. Both gains are now configurable and the ratio is preserved.

## UltraNav UI — reproduced

`ui/` is a Qt app that rebuilds the original control panel from the driver's
own control tree:

- `ui_tree.json` — all 2927 controls across 14 tabs, with the original
  pixel geometry, control types, caption ids, tab order, dependency
  expressions and IOCTLs.
- `strings_en.json` — the English STRINGTABLE (1053 strings) extracted from
  `SynTPRes.dll`; 17 other languages are present in the DLL and could be
  extracted the same way.
- `assets/bitmaps/` — 139 original bitmaps (DIB) taken from `SynTPRes.dll`,
  including the animated speed indicators `SCROLLING_SPEED_1..9` and
  `EDGE_MOTION_SPEED_1..10`.

Verified by dumping the built widget tree: the resulting captions and
geometry match the driver's table, e.g.

```
QCheckBox 261,92 300x19 "&ChiralMotion scrolling"
QCheckBox 261,40 300x19 "Enable &vertical scrolling"
QGroupBox 209,20 375x170 "PalmCheck"
```

## Not ported

- The kernel driver's raw packet decoding: libinput already handles the
  hardware correctly.
- `TP4table.dat` per-application scroll routing: that is a Windows
  window-manager feature with no Linux equivalent at this layer.
- Illumination/LED controls: the T410 has none.
- The pressure-sensitive EdgeMotion implementation: libinput's edge
  scrolling is a different (and deliberate) design.
