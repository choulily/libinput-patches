/*
 * Copyright © 2026 Lily Chou
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <math.h>

#include "evdev-mt-touchpad.h"

/*
 * Pointer momentum ("inertial" / "coasting" cursor), the trackpad
 * equivalent of lifting a finger and having the cursor keep sliding.
 *
 * When the last finger is lifted we look at the recent motion history to
 * estimate the finger velocity. If it exceeds the configured minimum we
 * start a timer-driven animation that posts pointer motion events with an
 * exponentially decaying velocity. The decay is scaled by the user's
 * strength setting.
 *
 * Momentum is cancelled immediately by any new touch, button event or by
 * disabling the feature. It only ever generates motion, never scroll or
 * gesture events, so it cannot interfere with scrolling or window
 * manipulation.
 */

/* Interval between momentum motion events. 8ms is close to a 120Hz frame
 * and keeps timer overhead low. */
#define MOMENTUM_INTERVAL usec_from_millis(8)

/*
 * The original Synaptics Windows driver, and xf86-input-synaptics which
 * reimplemented it, do not use exponential decay. Coasting is a constant
 * deceleration: every second the speed drops by a fixed amount, so the
 * cursor stops after a finite, predictable time and the tail is linear.
 *
 * That difference is not cosmetic. Exponential decay is asymptotic: it is
 * always still moving, so it reads as an unresponsive drift rather than a
 * coast. Constant deceleration has a crisp end.
 *
 * CoastingFriction: scroll events/second^2 of deceleration. The original
 * default is 50. Lower means the cursor slides further.
 */
#define MOMENTUM_DEFAULT_FRICTION 50.0

/*
 * CoastingSpeed: the scroll rate, in scroll events/second, the finger must
 * reach before coasting starts. The original default is 20, documented as
 * "which should prevent you from starting coasting unintentionally".
 *
 * We work in normalized units, so the gate is expressed as a velocity:
 * one scroll event corresponds to CIRCULAR/2fg step distance. Keeping the
 * original number visible here makes the two tunings comparable.
 */
#define MOMENTUM_DEFAULT_MIN_SCROLL_RATE 20.0

/* Clamp the initial velocity so a pathological event stream cannot fling
 * the cursor across the screen. Normalized units (pixels at 1000dpi) per
 * second. */
#define MOMENTUM_MAX_VELOCITY 4000.0

/* Below this speed the animation has visually stopped. */
#define MOMENTUM_STOP_VELOCITY 8.0

/* One scroll event, in normalized units. Matches the step distance used
 * for finger scrolling so the rate gate is comparable. */
#define MOMENTUM_SCROLL_STEP (TP_MM_TO_DPI_NORMALIZED(0.5))

/* The velocity estimate uses the most recent motion samples. This is
 * the number of samples we look back over when estimating. */
#define MOMENTUM_VELOCITY_SAMPLES 3

/*
 * Map the public "strength" percentage onto CoastingFriction.
 *
 * 100% is the original default of 50 events/s^2. Higher strength means a
 * more slippery pad, i.e. less friction, so the mapping is inverted.
 * Clamped so a pathological setting cannot make coasting last forever or
 * stop instantly.
 */
static double
momentum_friction(int strength)
{
	const double scale = 100.0 / max(10, min(400, strength));

	/*
	 * CoastingFriction is expressed in scroll events/s^2, but the
	 * animation works in normalized units. Scaling by the distance one
	 * scroll event covers converts it, and is what makes the original
	 * default of 50 produce a coast of roughly a second rather than
	 * sixteen: 50 events/s^2 * ~19.7 units/event is almost 1000
	 * units/s^2, so a 4000 unit/s flick stops in about 4 seconds.
	 */
	const double friction =
		max(5.0, min(500.0, MOMENTUM_DEFAULT_FRICTION * scale));

	return friction * MOMENTUM_SCROLL_STEP;
}

/* The scroll-rate gate from the public minimum velocity, in the same
 * normalized units. */
static double
momentum_min_velocity(int strength, double min_velocity)
{
	/* Honours an explicit minimum velocity when set, otherwise derives
	 * it from the original 20-scrolls/second default. */
	if (min_velocity > 0.0)
		return min_velocity;

	return MOMENTUM_DEFAULT_MIN_SCROLL_RATE * MOMENTUM_SCROLL_STEP;
}

/* Estimate pointer velocity in normalized units/second from the touch
 * motion history. Returns false if there is not enough data. */
static bool
tp_momentum_get_velocity(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 struct normalized_coords *velocity)
{
	const struct tp_history_point *newest, *oldest;
	struct device_coords delta;
	struct device_float_coords fdelta;
	usec_t tdelta;
	double seconds;
	int nsamples;

	if (t->history.count < 2)
		return false;

	nsamples = min(t->history.count - 1, MOMENTUM_VELOCITY_SAMPLES);

	newest = tp_motion_history_offset(t, 0);
	oldest = tp_motion_history_offset(t, nsamples);

	tdelta = usec_delta(newest->time, oldest->time);
	if (usec_is_zero(tdelta))
		return false;

	delta.x = newest->point.x - oldest->point.x;
	delta.y = newest->point.y - oldest->point.y;
	fdelta.x = delta.x;
	fdelta.y = delta.y;

	/* Scale to the x-axis resolution and to 1000dpi normalized units,
	 * the same conversion the pointer motion path uses. Motion events
	 * are reported in these units so the decay is consistent across
	 * devices with different resolutions. */
	struct normalized_coords normalized = tp_normalize_delta(
		tp,
		tp_scale_to_xaxis(tp, fdelta));

	seconds = usec_as_uint64_t(tdelta) / 1000000.0;
	if (seconds <= 0.0)
		return false;

	velocity->x = normalized.x / seconds;
	velocity->y = normalized.y / seconds;

	return true;
}

static void
tp_momentum_handle_timeout(usec_t now, void *data)
{
	struct tp_dispatch *tp = data;
	struct normalized_coords delta;
	const struct device_float_coords raw = { 0.0, 0.0 };
	double elapsed, speed, friction, decel, new_speed;
	usec_t tdelta;

	if (!tp->momentum.anim.active)
		return;

	if (tp->nfingers_down > 0 || tp->device->is_suspended) {
		tp_momentum_stop(tp, now);
		return;
	}

	tdelta = usec_delta(now, tp->momentum.anim.last_time);
	tp->momentum.anim.last_time = now;

	elapsed = usec_as_uint64_t(tdelta) / 1000000.0;
	if (elapsed <= 0.0)
		elapsed = usec_as_uint64_t(MOMENTUM_INTERVAL) / 1000000.0;

	/*
	 * Constant deceleration, matching CoastingFriction in the original:
	 * the speed drops by a fixed number of units per second, per second.
	 * A frame's worth of that is friction * elapsed.
	 *
	 * Using the magnitude and restoring the sign afterwards keeps this
	 * correct for both directions and for diagonal motion, where the
	 * deceleration applies along the velocity vector rather than per
	 * axis (per-axis deceleration would bend the path).
	 */
	speed = hypot(tp->momentum.anim.velocity.x,
		      tp->momentum.anim.velocity.y);
	if (speed <= 0.0) {
		tp_momentum_stop(tp, now);
		return;
	}

	friction = momentum_friction(tp->momentum.strength);
	decel = friction * elapsed;
	new_speed = speed - decel;

	if (new_speed < MOMENTUM_STOP_VELOCITY) {
		tp_momentum_stop(tp, now);
		return;
	}

	/* Scale the direction vector to the new magnitude. */
	const double scale = new_speed / speed;
	tp->momentum.anim.velocity.x *= scale;
	tp->momentum.anim.velocity.y *= scale;

	delta.x = tp->momentum.anim.velocity.x * elapsed;
	delta.y = tp->momentum.anim.velocity.y * elapsed;

	if (!normalized_is_zero(delta))
		pointer_notify_motion(&tp->device->base, now, &delta, &raw);

	libinput_timer_set(&tp->momentum.anim.timer,
			   usec_add(now, MOMENTUM_INTERVAL));
}

static void
tp_momentum_start(struct tp_dispatch *tp, struct tp_touch *t, usec_t time)
{
	struct normalized_coords velocity;
	double speed;

	if (!tp->momentum.enabled)
		return;

	velocity = t->momentum.velocity;

	speed = hypot(velocity.x, velocity.y);
	if (speed < momentum_min_velocity(tp->momentum.strength,
					  tp->momentum.min_velocity))
		return;

	if (speed > MOMENTUM_MAX_VELOCITY) {
		double scale = MOMENTUM_MAX_VELOCITY / speed;
		velocity.x *= scale;
		velocity.y *= scale;
	}

	tp->momentum.anim.velocity = velocity;
	tp->momentum.anim.active = true;
	tp->momentum.anim.last_time = time;

	libinput_timer_set(&tp->momentum.anim.timer,
			   usec_add(time, MOMENTUM_INTERVAL));
}

void
tp_momentum_stop(struct tp_dispatch *tp, usec_t time)
{
	if (!tp->momentum.anim.active)
		return;

	tp->momentum.anim.active = false;
	tp->momentum.anim.velocity.x = 0.0;
	tp->momentum.anim.velocity.y = 0.0;
	libinput_timer_cancel(&tp->momentum.anim.timer);
}

/*
 * Called for every motion frame while a finger is down. Keeps a rolling
 * velocity estimate on the touch so that the release velocity is known
 * even though the motion history is reset when a touch ends.
 */
void
tp_momentum_update_velocity(struct tp_dispatch *tp, struct tp_touch *t)
{
	struct normalized_coords velocity;

	if (!tp->momentum.enabled)
		return;

	if (!tp_momentum_get_velocity(tp, t, &velocity))
		return;

	t->momentum.velocity = velocity;
}

/*
 * Called once the last finger has been lifted. Uses the velocity the
 * touch had just before it ended.
 */
void
tp_momentum_handle_release(struct tp_dispatch *tp, struct tp_touch *t, usec_t time)
{
	if (!tp->momentum.enabled)
		return;

	if (tp->device->is_suspended)
		return;

	/* Only start momentum from a clean release. A palm or thumb
	 * release should not fling the cursor. */
	if (t->palm.state != PALM_NONE)
		return;

	tp_momentum_start(tp, t, time);
}

void
tp_momentum_post_events(struct tp_dispatch *tp, usec_t time)
{
	/* The animation runs entirely from the timer. There is nothing
	 * to do per frame, but a touch in progress always cancels it. */
	if (tp->momentum.anim.active && tp->nfingers_down > 0)
		tp_momentum_stop(tp, time);
}

void
tp_momentum_init(struct tp_dispatch *tp)
{
	char timer_name[64];

	snprintf(timer_name,
		 sizeof(timer_name),
		 "%s (momentum)",
		 evdev_device_get_sysname(tp->device));
	libinput_timer_init(&tp->momentum.anim.timer,
			    tp_libinput_context(tp),
			    timer_name,
			    tp_momentum_handle_timeout,
			    tp);

	tp->momentum.enabled = false;
	tp->momentum.strength = 100;
	/* The original gate is 20 scroll events/second, which on a normal
	 * pad is about 394 normalized units/s. Using it as the default is
	 * what stops slow, deliberate movements from flinging the cursor. */
	tp->momentum.min_velocity =
		MOMENTUM_DEFAULT_MIN_SCROLL_RATE * MOMENTUM_SCROLL_STEP;
	tp->momentum.decay_exponent = 1.0;
	tp->momentum.anim.active = false;

	tp_momentum_config_init(tp);
}

void
tp_remove_momentum(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->momentum.anim.timer);
	libinput_timer_destroy(&tp->momentum.anim.timer);
}

static enum libinput_config_status
tp_momentum_config_set_enabled(struct libinput_device *device, int enable)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	if (tp->momentum.enabled == !!enable)
		return LIBINPUT_CONFIG_STATUS_SUCCESS;

	tp->momentum.enabled = !!enable;
	if (!tp->momentum.enabled)
		tp_momentum_stop(tp, libinput_now(tp_libinput_context(tp)));

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static int
tp_momentum_config_get_enabled(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->momentum.enabled;
}

static enum libinput_config_status
tp_momentum_config_set_strength(struct libinput_device *device, int strength)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	tp->momentum.strength = max(10, min(400, strength));
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static int
tp_momentum_config_get_strength(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->momentum.strength;
}

static enum libinput_config_status
tp_momentum_config_set_min_velocity(struct libinput_device *device,
				    double velocity)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	tp->momentum.min_velocity = max(0.0, min(2000.0, velocity));
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
tp_momentum_config_get_min_velocity(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->momentum.min_velocity;
}

static enum libinput_config_status
tp_momentum_config_set_decay_curve(struct libinput_device *device, double exponent)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	/*
	 * Kept for API compatibility. The original Synaptics model uses
	 * constant deceleration, which has no exponent, so this now maps
	 * onto friction: a larger exponent means less friction and a
	 * longer coast, preserving the direction of the old effect.
	 */
	const double clamped = max(0.25, min(4.0, exponent));
	int strength = (int)(100.0 / clamped);
	strength = max(10, min(400, strength));
	tp->momentum.strength = strength;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
tp_momentum_config_get_decay_curve(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return max(0.25, min(4.0, 100.0 / tp->momentum.strength));
}

void
tp_momentum_config_init(struct tp_dispatch *tp)
{
	struct libinput_device_config_scroll_method *config =
		&tp->scroll.config_method;

	config->momentum.set_enabled = tp_momentum_config_set_enabled;
	config->momentum.get_enabled = tp_momentum_config_get_enabled;
	config->momentum.set_strength = tp_momentum_config_set_strength;
	config->momentum.get_strength = tp_momentum_config_get_strength;
	config->momentum.set_min_velocity = tp_momentum_config_set_min_velocity;
	config->momentum.get_min_velocity = tp_momentum_config_get_min_velocity;
	config->momentum.set_decay_curve = tp_momentum_config_set_decay_curve;
	config->momentum.get_decay_curve = tp_momentum_config_get_decay_curve;
}

/* -------------------------------------------------------------------------
 * Per-method scroll speed
 * ------------------------------------------------------------------------- */

#define TP_SCROLL_SPEED_MIN 0.1
#define TP_SCROLL_SPEED_MAX 5.0

static double
clamp_scroll_speed(double multiplier)
{
	return max(TP_SCROLL_SPEED_MIN, min(TP_SCROLL_SPEED_MAX, multiplier));
}

static enum libinput_config_status
tp_scroll_speed_set_two_finger(struct libinput_device *device, double multiplier)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	tp->scroll.speed.two_finger = clamp_scroll_speed(multiplier);
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
tp_scroll_speed_get_two_finger(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->scroll.speed.two_finger;
}

static enum libinput_config_status
tp_scroll_speed_set_edge(struct libinput_device *device, double multiplier)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	tp->scroll.speed.edge = clamp_scroll_speed(multiplier);
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
tp_scroll_speed_get_edge(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->scroll.speed.edge;
}

static enum libinput_config_status
tp_scroll_speed_set_circular(struct libinput_device *device, double multiplier)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	tp->scroll.speed.circular = clamp_scroll_speed(multiplier);
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
tp_scroll_speed_get_circular(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->scroll.speed.circular;
}

void
tp_scroll_speed_config_init(struct tp_dispatch *tp)
{
	struct libinput_device_config_scroll_method *config =
		&tp->scroll.config_method;

	config->scroll_speed.set_two_finger_speed = tp_scroll_speed_set_two_finger;
	config->scroll_speed.get_two_finger_speed = tp_scroll_speed_get_two_finger;
	config->scroll_speed.set_edge_speed = tp_scroll_speed_set_edge;
	config->scroll_speed.get_edge_speed = tp_scroll_speed_get_edge;
	config->scroll_speed.set_circular_speed = tp_scroll_speed_set_circular;
	config->scroll_speed.get_circular_speed = tp_scroll_speed_get_circular;
}
