# libinput fork: circular scrolling + cursor momentum

A fork of [libinput](https://gitlab.freedesktop.org/libinput/libinput) that adds
two features taken from the old `xf86-input-synaptics` driver and macOS-style
trackpad behaviour.

Everything in upstream libinput still works unchanged. Both features are off
until explicitly enabled, except on the ThinkPad generation that shipped
circular scrolling in its Windows driver.

## Circular scrolling

Synaptics-style circular scrolling: draw circles on the touchpad edge and the
pad emits scroll events instead of moving the cursor.

The behaviour deliberately matches `xf86-input-synaptics`:

* A touch **starting** inside the trigger region is latched into circular
  scrolling until the finger is lifted (the driver's "circular scroll locks in
  until finger is raised").
* A touch starting outside the region moves the pointer normally, even if it
  later crosses into the trigger region.
* Scroll distance is derived from the **angle** the finger travels, not the
  distance. Small, tight circles therefore scroll faster than large, lazy ones.
* Only the vertical axis is generated, and natural scrolling inverts it like
  any other scroll source.

It is implemented as a scroll method, so it is selected the same way as
two-finger or edge scrolling:

```c
libinput_device_config_scroll_set_method(device, LIBINPUT_CONFIG_SCROLL_CIRCULAR);
```

### Trigger region

The trigger is an edge, a corner, or the union of all edges. Regions are
computed the way the Synaptics driver did it: an eighth of the pad dimension on
each axis, with a 4 mm minimum so large pads still have a usable strip. Round
pads (the `ModelCircularTouchpad` quirk) instead use the inscribed ellipse, so
the inner disc stays available for pointer motion and gestures.

```c
libinput_device_config_circular_scroll_set_trigger(device,
        LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_RIGHT_EDGE);
```

### Scroll distance

`CircScrollDelta` in Synaptics terms: the angle a finger must travel to produce
one scroll step. Exposed in degrees for readability; the default is 5.7°
(0.1 rad), the same as the driver.

```c
libinput_device_config_circular_scroll_set_distance(device, 5.7); /* degrees */
```

## Cursor momentum

When the last finger is lifted during pointer motion, a fast flick keeps the
cursor gliding and decays exponentially to a stop.

* Only starts from **pointer motion**, never from a scroll, swipe or pinch, so a
  two-finger scroll release cannot fling the cursor.
* A new touch, a button event, changing the scroll method or suspending the
  device stops it immediately.
* Below a configurable minimum velocity no momentum starts, which keeps precise
  positioning usable.

```c
libinput_device_config_momentum_set_enabled(device, 1);
libinput_device_config_momentum_set_strength(device, 150);     /* percent */
libinput_device_config_momentum_set_min_velocity(device, 120.0);
```

Momentum is available on touchpads, trackpoints and pointing sticks. It is off
by default.

## ThinkPad defaults

The T60 through T440p generation (and the contemporary R/X/Z/W/L series) shipped
the same family of Synaptics pad whose Windows driver supported circular
scrolling. Those machines are marked with a `ModelSynapticsCircularPad` quirk in
`quirks/50-system-lenovo.quirks`, which makes circular scrolling their default
scroll method, matching what `xf86-input-synaptics` did with
`CircularScrolling`.

Every other touchpad keeps upstream defaults (two-finger scrolling) and can opt
in at runtime.

Circular scrolling is single-finger, so unlike two-finger scrolling it works on
the semi-MT and single-touch pads of that era.

## Building

```sh
meson setup build -Ddocumentation=false -Dtests=true
ninja -C build
```

The test suite needs root and a writable `/dev/uinput`:

```sh
sudo ./build/libinput-test-suite --filter-test='*circular*'
sudo ./build/libinput-test-suite --filter-test='*momentum*'
```

## New API

Circular scrolling:

* `libinput_device_config_circular_scroll_is_available()`
* `libinput_device_config_circular_scroll_set_trigger()`
* `libinput_device_config_circular_scroll_get_trigger()`
* `libinput_device_config_circular_scroll_get_default_trigger()`
* `libinput_device_config_circular_scroll_set_distance()`
* `libinput_device_config_circular_scroll_get_distance()`
* `libinput_device_config_circular_scroll_get_default_distance()`
* `enum libinput_config_circular_scroll_trigger`
* `LIBINPUT_CONFIG_SCROLL_CIRCULAR`

Cursor momentum:

* `libinput_device_config_momentum_is_available()`
* `libinput_device_config_momentum_set_enabled()`
* `libinput_device_config_momentum_get_enabled()`
* `libinput_device_config_momentum_set_strength()`
* `libinput_device_config_momentum_get_strength()`
* `libinput_device_config_momentum_set_min_velocity()`
* `libinput_device_config_momentum_get_min_velocity()`

All symbols are versioned `LIBINPUT_1.32`.

## Configuring without any desktop support

Because both features are implemented inside libinput, they can be enabled
from libinput's own quirks database. Any desktop that uses libinput then gets
them, whether or not it knows they exist.

Copy a block from `examples/fork-quirks-example.quirks` into
`/etc/libinput/local-overrides.quirks`:

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

| Attribute | Type | Range |
|---|---|---|
| `AttrCircularScrollEnabled` | bool | selects circular as the scroll method |
| `AttrCircularScrollTrigger` | uint | 0–8 |
| `AttrCircularScrollDistance` | double | degrees per step, 0 < d ≤ 180 |
| `AttrPointerMomentumEnabled` | bool | |
| `AttrPointerMomentumStrength` | uint | 10–400 |
| `AttrPointerMomentumMinVelocity` | double | 0–2000 |

Verify with:

```sh
libinput quirks list /dev/input/eventN
```

This also fixes a latent upstream bug: `libinput quirks list` used to abort on
any attribute that was not bool/uint/string/double/dimensions/range, because
every attribute had to be listed in a switch statement.

## Desktop integration

See `../kcm-libinput-fork/`: a System Settings module for both features, plus a
helper that reconciles the settings with the running compositor. KWin 6.8 does
not expose these two features as D-Bus properties, so the module persists its
settings, and a small KWin patch (not included here) is needed for live
application without a session restart.
