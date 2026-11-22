# Optional: live-applying KWin patches

**You do not need these.** The supported setup writes configuration that
KWin and libinput already read, and works on an unpatched system.

These patches exist only for people who specifically want a settings page
that applies changes without logging out. They are documented so the work
is not lost, not because they are recommended.

## Why they are not the default

`pacman -Syu` replaces KWin and plasma-desktop with the distro's builds. Any
patch compiled into them disappears, and the GUI silently loses its extra
properties while the underlying feature keeps working through the quirks
file. That split — the feature working but the GUI claiming it cannot — is
worse than not having the GUI at all.

Carrying them properly means a `kwin` PKGBUILD and a `plasma-desktop`
PKGBUILD in your own repo, rebuilt on every Plasma point release. That is a
real maintenance burden for one convenience.

## What they would add

`0001` on KWin: `org.kde.KWin.InputDevice` properties —
`supportsScrollCircular`, `scrollCircular`, `circularScrollingTrigger`,
`circularScrollingDistance`, `supportsPointerMomentum`, `pointerMomentum`,
`pointerMomentumStrength`, `pointerMomentumMinVelocity` — with
`kcminputrc` persistence and a CMake probe so KWin still builds against
stock libinput.

`0002` on plasma-desktop: a third "Scroll by:" option in the stock Touchpad
KCM, the trigger and distance rows, and the momentum checkbox with strength
and minimum velocity.

The patches were written and verified (they applied cleanly to fresh
Plasma/6.8 checkouts, and the plasma `kwinwaylandtouchpad.cpp` compiled with
moc-generated code), but they were removed from this repository because
shipping them implied a durability that compiled patches do not have.

If you want them, they are recoverable from the git history of this
repository before the commit titled "Rework onto configuration files".
