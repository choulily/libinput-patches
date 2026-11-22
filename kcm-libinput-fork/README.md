# Touchpad settings for the libinput fork

A System Settings module for circular scrolling and cursor momentum.

It is a single page, and everything on it writes real configuration. There
is no second page of instructions, and no dependency on a patched KWin.

![settings page](docs/settings.png)

## How it works

The module writes the two files that KWin and libinput already read:

| File | Read by | Holds |
|---|---|---|
| `/etc/libinput/local-overrides.quirks` | libinput, when a device opens | trigger region, scroll distance, momentum on/off, strength, minimum velocity |
| `~/.config/kcminputrc` | KWin, when a device is added | `ScrollMethod` — how circular scrolling is selected |

**No KWin patch is required.** Unpatched KWin already applies whatever
`ScrollMethod` it finds in `kcminputrc`; it casts the value and only checks
it against the supported-method bitmask, which the fork satisfies. And
libinput reads the quirks file itself.

That is also why this survives upgrades: both files are configuration.
`pacman -Syu` replaces binaries, not `/etc` or `~/.config`.

### Why the settings take effect at login

libinput reads quirks when a device is opened, and KWin reads
`ScrollMethod` when a device is added. Neither watches for changes, so the
page says so plainly after saving: log out and back in, or unplug and
replug an external touchpad. There is no way to apply it live without a
patched compositor, and pretending otherwise would be the same empty
promise as a "Live apply" button that does nothing.

## Build

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Requires Qt 6.6+, KF6 6.0+ (CoreAddons, Config, ConfigWidgets, I18n,
KCMUtils) and extra-cmake-modules.

It appears as **Input Devices → Touchpad Gestures (Fork)**.

## The privileged helper

Writing `/etc/libinput/local-overrides.quirks` needs root, so the module
calls `libinput-fork-apply` through `pkexec` — one password prompt, and the
file is replaced atomically so a crash cannot leave a half-written quirk
database behind (libinput refuses to load *any* quirks if one file is
broken).

The helper validates the new section with libinput's own parser before
installing it, and **refuses** if the running libinput is not the fork,
because the stock parser rejects these attributes as unknown keys and would
break the whole database.

It also merges rather than overwrites: sections it owns are prefixed
`libinput-fork:` and regenerated; anything else in the file is preserved.

```sh
libinput-fork-apply --write-quirks /path/to/section.quirks
libinput-fork-apply --set-scroll-method 8 --device-match "SynPS/2 Synaptics TouchPad"
libinput-fork-apply --show
```

## What this replaced

An earlier revision shipped two KWin patches (D-Bus properties and a
patched stock KCM) plus a second settings page that explained how to apply
them. That was wrong for two reasons:

1. The patches are erased by the next `pacman -Syu`, so the settings page
   would silently regress to read-only after every update.
2. The extra page had no content of its own — it only described work the
   user had to do.

Both patches are gone. `optional-patches/kwin-patches.md` keeps them
explained for anyone who specifically wants a live-applying GUI, but they
are not part of the supported setup and nothing here depends on them.

## Requirements

The settings are only meaningful with the libinput fork installed. The
module detects this and says so instead of writing settings that would do
nothing. See `../libinput/PKGBUILD`: it replaces the stock `libinput`,
`libinput-udev` and `libinput-docs` packages and provides their files, so it
stays installed across upgrades.
