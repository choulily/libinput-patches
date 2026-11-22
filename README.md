# libinput patches: circular scrolling + cursor momentum

Patches that add Synaptics-style **circular scrolling** and **pointer
momentum** to libinput, plus a KDE System Settings module and packaging that
survives upgrades.

---

> ## ⚠️ AIGC warning
>
> **Most of this code was written by an AI (Claude, via OpenCode).**
>
> It is not upstream libinput code and has not been reviewed by the libinput
> maintainers. It is not endorsed by freedesktop.org, KDE, or anyone else.
>
> What that means in practice:
>
> - The C is written to libinput's own conventions and style, and the test
>   suite passes — but passing tests is not the same as being correct.
> - Behaviour that is hard to unit-test (long sessions, suspend/resume,
>   unusual hardware, palm rejection interaction, multi-touch edge cases)
>   has had far less scrutiny than upstream code gets.
> - The input stack is **privileged**: libinput runs inside your compositor
>   and reads directly from `/dev/input`. A bug here can hang your session.
>   Test it before you rely on it, and keep a way to get back to stock
>   `libinput` (`sudo pacman -S libinput libinput-udev`).
> - The packaging claims are verified on one machine, an Arch install on a
>   ThinkPad. Your system may differ.
>
> If you are not comfortable with that, do not install this. Upstream
> libinput also has a circular scrolling mode (a ring on round touchpads);
> if that is enough for you, use it instead.

---

## What is here

```
libinput/          the fork: libinput 1.32.0 + the two features
  PKGBUILD         Arch package that replaces stock libinput
  quirks/          50-system-lenovo.quirks: T60..T440p defaults
  examples/        copy-paste quirk file for other machines
kcm-libinput-fork/ KDE System Settings module
  scripts/         pkexec helper that writes the config files
  other-des/       sway patch, notes on GNOME and the quirks route
  optional-patches/ the KWin patches, documented but not used
```

## The features

**Circular scrolling** — draw circles on the touchpad edge to scroll. This
follows `xf86-input-synaptics` rather than inventing new behaviour:

- A touch starting in the trigger region is latched until the finger lifts.
- Scroll distance comes from the *angle* travelled, so tight circles scroll
  faster than large ones.
- `CircScrollDelta` equivalent defaults to 5.7° (0.1 rad).
- Works on semi-MT and single-touch pads, because it never needs more than
  one finger. That matters: the ThinkPads this targets are not multi-touch.

**Cursor momentum** — a fast flick keeps the cursor gliding and decays to a
stop. Only starts from pointer motion, never from a scroll or gesture
release, and stops on any new touch, button or suspend.

Both are opt-in, except on the ThinkPad generation that shipped circular
scrolling in its Windows driver (T60 through T440p, via a quirk).

## Design in one paragraph

Both features live inside libinput, so the only question for any desktop is
how it lets you turn them on. Circular scrolling is a scroll method, so it
is selected through `ScrollMethod` in `kcminputrc` — which **unpatched**
KWin already honours, because it casts the value and only checks it against
a supported-method bitmask that the fork satisfies. The parameters live in
libinput's own quirks file, which libinput reads itself. That means the
settings are configuration files, not patched binaries, and therefore
survive `pacman -Syu`. See `kcm-libinput-fork/README.md`.

## Install

```sh
git clone git@github.com:choulily/libinput-patches.git
cd libinput-patches/libinput
makepkg -si
```

That replaces `libinput`, `libinput-udev` and `libinput-docs`. Then, for the
GUI:

```sh
cd ../kcm-libinput-fork
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build && sudo cmake --install build
```

**Settings survive upgrades. The binary does not** — when libinput itself is
updated by a future release, rebuild this package. That is unavoidable for
any fork; the point of the packaging is that `pacman` will not silently
revert you to stock libinput in the meantime.

## Settings without a GUI, on any desktop

Because the features are inside libinput, a quirk file configures them
everywhere — GNOME, sway, XFCE, anything:

```
[My touchpad]
MatchName=*SynPS/2 Synaptics TouchPad
AttrCircularScrollEnabled=1
AttrCircularScrollTrigger=0
AttrCircularScrollDistance=5.7
AttrPointerMomentumEnabled=1
AttrPointerMomentumStrength=150
AttrPointerMomentumMinVelocity=120
```

Put it in `/etc/libinput/local-overrides.quirks`. See
`examples/fork-quirks-example.quirks` and `libinput/README.fork.md`.

sway additionally accepts `scroll_method circular` today, with no patch.

## Testing

The libinput litest suite covers both features across every touchpad model it
knows: direction, trigger regions, the centre dead zone, intent locking,
distance scaling, momentum start/stop, and that momentum never starts from a
scroll release. Suites for `circular`, `momentum`, `touchpad`, `gestures` and
`quirks` pass with zero failures.

Several tests are scoped to rectangular pads, because the harness expresses
touch positions as percentages of the pad and a round Pad's geometry does not
map onto those coordinates. That is a limitation of the tests, not of the
code, and it is noted where it applies.

## Licence

The libinput changes are MIT, matching upstream (see `libinput/COPYING`).
The KCM is GPL-2.0-or-later, matching KDE.
