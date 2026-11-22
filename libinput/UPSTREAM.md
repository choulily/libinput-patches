# Relationship to upstream libinput

This directory is libinput 1.32.0 with two features added. Everything else
is untouched upstream code, and the fork tracks upstream so the diff stays
readable:

    git remote add upstream https://gitlab.freedesktop.org/libinput/libinput.git
    git fetch upstream
    git diff upstream/main -- src/

The patches are:

| Area | Files |
|---|---|
| Circular scrolling | `src/evdev-mt-touchpad-circular-scroll.c` |
| Pointer momentum | `src/evdev-mt-touchpad-momentum.c` |
| Touchpad wiring | `src/evdev-mt-touchpad.c`, `src/evdev-mt-touchpad.h`, `src/evdev-mt-touchpad-gestures.c` |
| Public API | `src/libinput.h`, `src/libinput.c`, `src/libinput.sym`, `src/libinput-private.h` |
| Quirk attributes | `src/quirks.c`, `src/quirks.h`, `tools/shared.c` |
| Tests | `test/test-touchpad.c` |
| Packaging and defaults | `PKGBUILD`, `quirks/50-system-lenovo.quirks` |

## If you are the libinput maintainers

This is not an attempt to push this upstream. Circular scrolling was
considered and rejected for upstream several times over the years, and the
upstream position (that it is a device-specific workaround for old
Synaptics pads) is reasonable. This fork exists for people who use those
pads and want the behaviour back.

The momentum feature is not something upstream has expressed interest in at
all.

If you do read the code and find bugs, they are ours, not yours.
