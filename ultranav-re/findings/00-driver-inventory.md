# Lenovo ThinkPad T410 Synaptics UltraNav driver — analysis

## Artifact

| | |
|---|---|
| Package | `6hgx75ww.exe` (ThinkPad T410 UltraNav driver) |
| Size | 28804240 bytes |
| SHA-256 | `088aae829fd35c65be4b00b193d948a0f49d6397f8d0992883d991057bd6bbe5` |
| Source | `https://download.lenovo.com/pccbbs/mobiles/6hgx75ww.exe` |
| Last-Modified | 2013-04-25 |
| Installer | Inno Setup 5.4.2 (extracted with `innoextract 1.9`) |
| Driver version | 16.2.19.7 |

## Key contents (`extracted/app/WinWDF/x64/`)

| File | Role |
|---|---|
| `SynTPEnh.exe` | 2.96 MB. The touchpad daemon: gesture recognition, scrolling, coasting, chiral motion. Contains the compiled-in tuning defaults. |
| `SynTPRes.dll` | 11.7 MB. All UI strings (12784 unique UTF-16 strings), bitmaps, help text, videos. |
| `SynTPCpl.dll` | 1.99 MB. The Control Panel applet — the UltraNav settings UI to clone. |
| `synpd.inf` | 10303 lines. Defines the **entire control panel UI layout** (2869 control entries) plus the registry tree. |
| `TP4table.dat` | Per-application scroll routing rules (not tunings). |
| `syndata.bin` | 1 MB opaque data blob. |
| `SynTP.sys` | Kernel driver. |

## Where the settings actually live

Two different things, which is important:

1. **UI layout and strings** — in `synpd.inf` (`SynTPCpl\Controls\...`) and
   `SynTPRes.dll`. Fully reconstructable; this is how the original
   UltraNav dialog is laid out, including caption IDs, coordinates,
   tab order, enable/disable dependencies and the IOCTL each control writes.

2. **Numeric defaults** — **not** in the INF. `SynTP\Defaults` has only 19
   entries and they are all mouse-button/corner actions, not touch
   tunings. The real defaults are compiled into `SynTPEnh.exe` and read
   from the registry with fallbacks in code.

## Settings surface (from `SynTPEnh.exe`)

Registry value names recovered from `.rdata`:

Scrolling:
`SliderFriction`, `ScrollBallisticsPower`, `ScrollBallisticsMap`,
`ScrollingSmoothnessFactor`, `ScrollingDistanceCalculation`,
`CancelCoastingMotionThreshold`, `CancelScrollAfter1FLeaves`,
`RestrictTo2FVerticalScroll`, `MaxWwhile2FScroll`,
`IgnoreOtherFingersWhen2FScroll`, `Allow2FScrollingAfter1FMoving1FInButtonZone`,
`EnableScrollFor2FReportedAs1F`, `ForceWheelMouseScrollOneLine`,
`GlobalScrollingConfig`, `ScrollMethod`

ChiralMotion (Synaptics' name for what the X driver calls circular):
`ChiralCenterX`, `ChiralCenterY`, `ChiralWheelGain`, `ChiralPageGain`,
`ChiralModalIndicator`, `ChiralMouse`,
`PercentStraightMoveForChiralReset`, `ShowAnimatedChiralIcons`,
`ShowLinearScrollingCursorInChiral`

Palm / contact filtering:
`PalmOnPadMinTime`, `PalmViolationToReleaseCursor`, `PalmCheckGroupPresent`,
`CancelScrollAfter1FLeaves`

## Values decoded from code

Constants read by the scroll-output routine at `0x68df0`:

| Constant | Value | Meaning |
|---|---|---|
| `0x13b4f0` | 3.0 | exponent applied in the ballistics curve |
| `0x13b4f8` | 10.0 | divisor in the scroll-distance conversion |
| `0x13b4fc` | 0.00390625 | 1/256, fixed-point scale |
| `0x13b4d8` | 1.0 | unit gain |

Registry fallbacks set immediately after the name lookup:

| Setting | Fallback | Notes |
|---|---|---|
| `ChiralWheelGain` | 4 | default wheel gain |
| `ChiralPageGain` | 0x100 (256) | page scroll gain |
| `ScrollingSmoothnessFactor` | clamped to **1..6** | 1 = smoothest, 6 = most responsive |
| `MaxWwhile2FScroll` | 0x19 (25) | max finger width while 2F scrolling |

`ScrollingSmoothnessFactor` deserves attention: the code clamps it to
1..6 and then derives `[rsi+0x304]`/`[rsi+0x178]` from it, i.e. it is the
master smoothness knob and it is *not* a continuous value.

## Authoritative coasting model

The INF does not carry it, but the freedesktop `xf86-input-synaptics`
driver reverse-engineered this same Windows behaviour, and its
documentation is the reference definition:

- `CoastingSpeed` — scrolls/second needed to start coasting. **Default 20.**
  "which should prevent you from starting coasting unintentionally."
- `CoastingFriction` — scrolls/second² by which the coasting speed
  decreases. **Default 50.**

Implementation (`src/synaptics.c:2587`):

```c
double dtime = (millis - last_millis) / 1000.0;
double ddy   = coasting_friction * dtime;
delta += coast_speed * dtime * abs(scroll_dist);
if (fabs(coast_speed) < ddy)
        coast_speed = 0;                       /* stop */
else
        coast_speed += (coast_speed < 0 ? ddy : -ddy);
```

**This is constant deceleration, not exponential decay.** Per frame the
speed drops by a fixed amount, so the cursor stops in finite time and the
tail is linear. That is the single biggest reason the existing
momentum implementation feels wrong: it decays exponentially, so it has a
long slow tail and no definite end.

### Why the current implementation feels wrong

| Our implementation | Original |
|---|---|
| Exponential decay (`pow(0.02, t)`) | Constant deceleration |
| Threshold in arbitrary "units/s" | Threshold in **scroll events/second** |
| No minimum speed gate by default | Must exceed 20 scrolls/s to engage at all |
| Strength as a percentage | `CoastingFriction` in events/s², lower = more slippery |

An exponential curve is always moving and asymptotically approaches zero,
which reads as an unresponsive drift. Constant deceleration has a crisp
end, which is what a coasting gesture is supposed to feel like.

## Circular scrolling

Same story as before but now confirmed: Synaptics calls it
**ChiralMotion**, and it is a *two-axis* feature (`ChiralWheelGain` for
wheel-mode output, `ChiralPageGain` for page-mode output), with the
centre stored as `ChiralCenterX`/`ChiralCenterY` rather than assumed to be
the pad midpoint.

The X driver's circular code (`HandleScrolling`, `scroll_dist_circ`
default 0.1 rad) matches what is already implemented, so the geometry is
right; what is missing is the *gain* distinction (wheel vs page) and the
explicit centre.

## Next

Port the constant-deceleration coasting model and the
`ScrollingSmoothnessFactor` 1..6 quantisation, then rebuild the UI from
the INF control tree and SynTPRes strings.
