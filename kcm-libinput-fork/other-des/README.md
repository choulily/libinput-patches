# Other desktop environments

The two features live **inside libinput**, so the only question for each
desktop is how it lets you select a scroll method and toggle momentum.
There are three tiers, from least to most work.

---

## Tier 1: works today, no patches (sway and wlroots compositors)

sway already accepts `circular` as a scroll method. It feature-detects
`LIBINPUT_CONFIG_SCROLL_CIRCULAR` at build time and lists the option when the
header provides it, so **with the fork installed, this just works**:

```
input "2:7:SynPS/2_Synaptics_TouchPad" {
    scroll_method circular
}
```

Nothing else to do. The same applies to any wlroots compositor that passes
`LIBINPUT_CONFIG_SCROLL_CIRCULAR` through, which is most of them since they
forward the enum value directly.

For momentum and the circular-scroll parameters there is
`other-des/sway-0001-input-fork-features.patch`, which adds six commands:

```
input <identifier> {
    circular_scrolling enabled
    circular_scroll_trigger right_edge
    circular_scroll_distance 5.7
    pointer_momentum enabled
    pointer_momentum_strength 150
    pointer_momentum_min_velocity 120
}
```

The patch is guarded by `HAVE_LIBINPUT_FORK`, so sway still builds and the
commands politely report "unavailable" against stock libinput.

---

## Tier 2: quirks — works on *any* desktop, zero patches

This is the important one. Because both features are implemented in libinput
itself, they can be configured from libinput's own quirks database. Every
desktop that uses libinput then gets them, whether or not it has ever heard
of them.

Drop a file in `/etc/libinput/local-overrides.quirks`:

```
[My ThinkPad circular scrolling]
MatchName=*SynPS/2 Synaptics TouchPad
AttrCircularScrollEnabled=1
AttrCircularScrollTrigger=0
AttrCircularScrollDistance=5.7
AttrPointerMomentumEnabled=1
AttrPointerMomentumStrength=150
AttrPointerMomentumMinVelocity=120
```

Verified attributes:

| Attribute | Type | Range |
|---|---|---|
| `AttrCircularScrollEnabled` | bool | selects circular as the scroll method |
| `AttrCircularScrollTrigger` | uint | 0–8, same order as the C enum |
| `AttrCircularScrollDistance` | double | degrees per step, 0 < d ≤ 180 |
| `AttrPointerMomentumEnabled` | bool | |
| `AttrPointerMomentumStrength` | uint | 10–400 |
| `AttrPointerMomentumMinVelocity` | double | 0–2000 |

`MatchName` / `MatchDMIModalias` / `Match*` work exactly as for any other
quirk. Values are applied by libinput when the device is opened, before the
compositor sees it.

This is how the T60–T440p ThinkPads get circular scrolling by default, via
`quirks/50-system-lenovo.quirks`. It is also the recommended route for any
desktop without a dedicated port: **you do not need to patch anything.**

### Note on the quirks tool

`tools/shared.c` used to `abort()` when printing any quirk attribute that was
not bool/uint/string/double/dimensions/range, because every attribute had to
be listed in a `switch`. The new attributes are added there, so
`libinput quirks list` prints them instead of crashing. This was a latent
upstream bug that only showed up once non-boolean attributes existed.

---

## Tier 3: native settings UI

| Desktop | Status |
|---|---|
| KDE Plasma | **Settings module here.** Writes the quirks file and `kcminputrc`; no KWin patch needed. See `../README.md`. |
| GNOME | Not ported. See below. |
| XFCE / Cinnamon / MATE (libinput via Xorg) | Tier 2 quirks only. |
| wlroots (sway, river, Wayfire, labwc, Hyprland) | Tier 1 for the scroll method; Tier 2 for the rest. |

### GNOME / mutter notes

Mutter does not have a scroll-method enum. It maps two independent GSettings
booleans onto libinput method bits and **ORs them together**:

```c
method = two_finger ? LIBINPUT_CONFIG_SCROLL_2FG : LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
current = libinput_device_config_scroll_get_method(device);
current &= ~LIBINPUT_CONFIG_SCROLL_2FG;
device_set_scroll_method(device, current | method);
```

Circular scrolling is exclusive, so a naive third bit would fight with the
existing ones. A correct port needs either:

* a new `org.gnome.desktop.peripherals.touchpad scroll-method` enum
  (`default|two-finger|edge|circular`) in gsettings-desktop-schemas,
  replacing the two booleans in mutter's
  `meta_input_settings_native_set_{edge,two_finger}_scroll`, or
* a dedicated `circular-scrolling` boolean that takes precedence over both
  when enabled.

Plus `pointer-momentum`, `pointer-momentum-strength` and
`pointer-momentum-min-velocity` keys, and the corresponding calls in
`meta_input_settings_native_set_*`.

That is a three-repository change (gsettings-desktop-schemas, mutter,
gnome-control-center) with an API/ABI review, so it is deliberately left
unported rather than half-done. The Tier 2 quirk route covers GNOME fully in
the meantime.

---

## Summary

| I want… | Do this |
|---|---|
| It to work on my ThinkPad right now, any DE | install the fork; the quirk already matches T60–T440p |
| It on any other touchpad, any DE | Tier 2 quirk file |
| A GUI | KDE patches above |
| sway | `scroll_method circular`, or the sway patch for the rest |
