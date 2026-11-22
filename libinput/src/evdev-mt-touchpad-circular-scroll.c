/*
 * Copyright © 2026 Mikio Braun
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
 * Synaptics-style circular scrolling.
 *
 * A touch that starts inside the configured trigger region (an edge, a
 * corner, or the ring outside the inner disc on a round pad) is latched
 * into circular scrolling until the finger is lifted. Every subsequent motion is converted into an
 * angular delta around the pad centre and accumulated. Once the accumulated
 * angle passes the configured "angle per scroll step" (the equivalent of the
 * Synaptics driver's CircScrollDelta, 0.1rad by default) a scroll event is
 * generated.
 *
 * Because the delta is an angle, not a distance, a small tight circle
 * scrolls faster than a large lazy one - the behaviour Synaptics users
 * expect. Only the vertical axis is generated, which matches the original
 * driver; natural scrolling is applied downstream by
 * evdev_notify_axis_finger() like for any other scroll source.
 */

/* Trigger zone width as a fraction of the pad dimension on each axis,
 * matching the Synaptics driver's default edge width. */
#define CIRCULAR_SCROLL_EDGE_FRACTION 0.125

/* Minimum absolute trigger strip, so that a very large pad still has a
 * reachable trigger. */
#define CIRCULAR_SCROLL_EDGE_MIN_MM 4.0

/* Near the pad centre the polar angle is meaningless: atan2 swings by
 * pi across a one-pixel crossing of the origin and small noise produces
 * huge angle deltas. Inside this radius we suspend scroll generation
 * and re-baseline when the finger leaves, so dragging across the middle
 * cannot fabricate a scroll event. */
#define CIRCULAR_SCROLL_DEAD_ZONE_MM 3.0

/* Clamp for the angle per step. */
#define CIRCULAR_SCROLL_MIN_STEP_RAD (M_PI / 180.0) /* 1 degree */
#define CIRCULAR_SCROLL_MAX_STEP_RAD M_PI            /* 180 degrees */

#define CIRCULAR_SCROLL_DEFAULT_STEP_RAD 0.1 /* CircScrollDelta default */

/* Scroll is expressed in normalized (1000dpi) units. One full step
 * produces this many units, which gives roughly a line of scrolling
 * for the default step and matches what 2fg scroll produces for a
 * comparable amount of finger travel. */
#define CIRCULAR_SCROLL_STEP_DISTANCE (TP_MM_TO_DPI_NORMALIZED(0.5))

static inline const char *
circular_state_to_str(enum tp_circular_scroll_touch_state state)
{
	switch (state) {
	CASE_RETURN_STRING(CIRCULAR_SCROLL_TOUCH_STATE_NONE);
	CASE_RETURN_STRING(CIRCULAR_SCROLL_TOUCH_STATE_INNER);
	CASE_RETURN_STRING(CIRCULAR_SCROLL_TOUCH_STATE_RING);
	}
	return NULL;
}

/* Signed angular delta in (-π, π]. Handles wraparound across ±π. */
static double
circular_scroll_angle_delta(double prev, double curr)
{
	double d = curr - prev;
	if (d > M_PI)
		d -= 2.0 * M_PI;
	else if (d < -M_PI)
		d += 2.0 * M_PI;
	return d;
}

/* Angle of the point around the pad centre, in radians. Device
 * coordinates have Y growing downward, so with plain atan2(dy, dx) the
 * angle increases clockwise on screen (right edge → bottom edge). That
 * is exactly the mapping we want: clockwise is "scroll down", matching
 * the Synaptics driver and a physical wheel turned towards the user. */
static double
tp_circular_scroll_angle(const struct tp_dispatch *tp,
			 const struct device_coords *point)
{
	double dx = point->x - tp->scroll.circular.center.x;
	double dy = point->y - tp->scroll.circular.center.y;
	return atan2(dy, dx);
}

static void
tp_circular_scroll_reset_touch(struct tp_touch *t)
{
	t->scroll.circular.state = CIRCULAR_SCROLL_TOUCH_STATE_NONE;
	/* NAN signals "no valid baseline angle"; the next event in RING
	 * state will set a fresh baseline without emitting. */
	t->scroll.circular.prev_angle = NAN;
	t->scroll.circular.angle_accum = 0.0;
	t->scroll.circular.direction = -1;
}

/*
 * Map the touch position onto the Synaptics edge bitmask. The bits are
 * physical (top/bottom/left/right) so they can be compared against the
 * trigger enum directly.
 */
enum circular_scroll_edge {
	CIRCULAR_EDGE_NONE = 0,
	CIRCULAR_EDGE_TOP = (1 << 0),
	CIRCULAR_EDGE_RIGHT = (1 << 1),
	CIRCULAR_EDGE_BOTTOM = (1 << 2),
	CIRCULAR_EDGE_LEFT = (1 << 3),
};

static bool
tp_circular_scroll_trigger_matches(uint32_t edge,
				   enum libinput_config_circular_scroll_trigger trigger)
{
	uint32_t want = 0;

	switch (trigger) {
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_ANY_EDGE:
		return edge != CIRCULAR_EDGE_NONE;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_TOP_EDGE:
		want = CIRCULAR_EDGE_TOP;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_TOP_RIGHT_CORNER:
		want = CIRCULAR_EDGE_TOP | CIRCULAR_EDGE_RIGHT;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_RIGHT_EDGE:
		want = CIRCULAR_EDGE_RIGHT;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_BOTTOM_RIGHT_CORNER:
		want = CIRCULAR_EDGE_BOTTOM | CIRCULAR_EDGE_RIGHT;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_BOTTOM_EDGE:
		want = CIRCULAR_EDGE_BOTTOM;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_BOTTOM_LEFT_CORNER:
		want = CIRCULAR_EDGE_BOTTOM | CIRCULAR_EDGE_LEFT;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_LEFT_EDGE:
		want = CIRCULAR_EDGE_LEFT;
		break;
	case LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_TOP_LEFT_CORNER:
		want = CIRCULAR_EDGE_TOP | CIRCULAR_EDGE_LEFT;
		break;
	}

	return (edge & want) == want;
}

/*
 * Edge detection, matching the Synaptics driver.
 *
 * Two modes, as in the original driver:
 *
 * - Rectangular pads (all ThinkPads this targets, T60..T440p) use four
 *   straight edge strips. The strip is a fraction of the pad dimension
 *   on that axis, so it scales with the pad.
 *
 * - Round pads (CircularPad=1 in Synaptics) are treated as an ellipse:
 *   anything outside the inscribed ellipse is an edge, with the
 *   quadrant deciding which edge. That is circular_edge_detection() from
 *   xf86-input-synaptics.
 */
static uint32_t
tp_circular_scroll_get_edge(const struct tp_dispatch *tp,
			    const struct device_coords *point)
{
	const struct evdev_device *device = tp->device;
	const struct input_absinfo *absinfo_x = device->abs.absinfo_x;
	const struct input_absinfo *absinfo_y = device->abs.absinfo_y;
	double x_center = (absinfo_x->minimum + absinfo_x->maximum) / 2.0;
	double y_center = (absinfo_y->minimum + absinfo_y->maximum) / 2.0;
	double half_w = (absinfo_x->maximum - absinfo_x->minimum) / 2.0;
	double half_h = (absinfo_y->maximum - absinfo_y->minimum) / 2.0;
	uint32_t edge = CIRCULAR_EDGE_NONE;

	if (half_w <= 0.0 || half_h <= 0.0)
		return CIRCULAR_EDGE_NONE;

	if (tp->scroll.circular.round_pad) {
		double rel_x = (point->x - x_center) / half_w;
		double rel_y = (point->y - y_center) / half_h;

		if (rel_x * rel_x + rel_y * rel_y > 1.0) {
			if (rel_x > M_SQRT1_2)
				edge |= CIRCULAR_EDGE_RIGHT;
			else if (rel_x < -M_SQRT1_2)
				edge |= CIRCULAR_EDGE_LEFT;

			if (rel_y < -M_SQRT1_2)
				edge |= CIRCULAR_EDGE_TOP;
			else if (rel_y > M_SQRT1_2)
				edge |= CIRCULAR_EDGE_BOTTOM;
		}

		return edge;
	}

	/* Rectangular pad: an eighth of the pad on each axis, with a
	 * minimum absolute width so tiny pads still have a usable strip. */
	double zone_w = max(half_w * 2.0 * CIRCULAR_SCROLL_EDGE_FRACTION,
			    (double)tp->scroll.circular.edge_min_x);
	double zone_h = max(half_h * 2.0 * CIRCULAR_SCROLL_EDGE_FRACTION,
			    (double)tp->scroll.circular.edge_min_y);

	if (point->x <= absinfo_x->minimum + zone_w)
		edge |= CIRCULAR_EDGE_LEFT;
	else if (point->x >= absinfo_x->maximum - zone_w)
		edge |= CIRCULAR_EDGE_RIGHT;

	if (point->y <= absinfo_y->minimum + zone_h)
		edge |= CIRCULAR_EDGE_TOP;
	else if (point->y >= absinfo_y->maximum - zone_h)
		edge |= CIRCULAR_EDGE_BOTTOM;

	return edge;
}

static enum libinput_config_status
tp_circular_scroll_set_trigger(struct libinput_device *device,
			       enum libinput_config_circular_scroll_trigger trigger)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	tp->scroll.circular.trigger = trigger;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_circular_scroll_trigger
tp_circular_scroll_get_trigger(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->scroll.circular.trigger;
}

static enum libinput_config_circular_scroll_trigger
tp_circular_scroll_get_default_trigger(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_ANY_EDGE;
}

static enum libinput_config_status
tp_circular_scroll_set_distance(struct libinput_device *device, double degrees)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;
	double rad = degrees * M_PI / 180.0;

	tp->scroll.circular.angle_step =
		max(CIRCULAR_SCROLL_MIN_STEP_RAD,
		    min(CIRCULAR_SCROLL_MAX_STEP_RAD, rad));
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
tp_circular_scroll_get_distance(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);
	struct tp_dispatch *tp = (struct tp_dispatch *)evdev->dispatch;

	return tp->scroll.circular.angle_step * 180.0 / M_PI;
}

static double
tp_circular_scroll_get_default_distance(struct libinput_device *device)
{
	return CIRCULAR_SCROLL_DEFAULT_STEP_RAD * 180.0 / M_PI;
}

void
tp_circular_scroll_config_init(struct tp_dispatch *tp)
{
	struct libinput_device_config_scroll_method *config =
		&tp->scroll.config_method;

	config->circular.set_trigger = tp_circular_scroll_set_trigger;
	config->circular.get_trigger = tp_circular_scroll_get_trigger;
	config->circular.get_default_trigger =
		tp_circular_scroll_get_default_trigger;
	config->circular.set_distance = tp_circular_scroll_set_distance;
	config->circular.get_distance = tp_circular_scroll_get_distance;
	config->circular.get_default_distance =
		tp_circular_scroll_get_default_distance;

	tp->scroll.circular.trigger =
		LIBINPUT_CONFIG_CIRCULAR_SCROLL_TRIGGER_ANY_EDGE;
}

static bool
tp_circular_scroll_in_trigger(const struct tp_dispatch *tp,
			      const struct device_coords *point)
{
	uint32_t edge = tp_circular_scroll_get_edge(tp, point);

	/* On a round pad the trigger region is the whole outer ring: the
	 * inscribed ellipse is the inner disc for normal pointer motion
	 * and gestures, everything outside it scrolls. */
	if (tp->scroll.circular.round_pad)
		return edge != CIRCULAR_EDGE_NONE;

	return tp_circular_scroll_trigger_matches(edge,
						 tp->scroll.circular.trigger);
}

/* Is the point too close to the pad centre for the polar angle to be
 * meaningful? Uses squared distance to avoid a sqrt per event. */
static bool
tp_circular_scroll_in_dead_zone(const struct tp_dispatch *tp,
				const struct device_coords *point)
{
	int32_t dx = point->x - tp->scroll.circular.center.x;
	int32_t dy = point->y - tp->scroll.circular.center.y;

	return dx * dx + dy * dy < tp->scroll.circular.dead_zone_radius_sq;
}

void
tp_circular_scroll_init(struct tp_dispatch *tp, struct evdev_device *device)
{
	struct tp_touch *t;
	struct phys_coords min_mm = {
		.x = CIRCULAR_SCROLL_EDGE_MIN_MM,
		.y = CIRCULAR_SCROLL_EDGE_MIN_MM,
	};
	struct phys_coords dead_mm = {
		.x = CIRCULAR_SCROLL_DEAD_ZONE_MM,
		.y = CIRCULAR_SCROLL_DEAD_ZONE_MM,
	};
	int32_t dead_units;

	/* Circular scrolling is a single-finger gesture, so unlike
	 * two-finger scrolling it works on every touchpad, including the
	 * semi-MT and single-touch Synaptics pads in older ThinkPads
	 * (T60 through T440p and the contemporary R/X/Z series). */

	/* Center in device coordinates (the usable area midpoint). */
	tp->scroll.circular.center.x =
		(device->abs.absinfo_x->maximum + device->abs.absinfo_x->minimum) / 2;
	tp->scroll.circular.center.y =
		(device->abs.absinfo_y->maximum + device->abs.absinfo_y->minimum) / 2;

	/* Note: evdev_device_mm_to_units() returns absolute coordinates
	 * (it adds the axis minimum), so it must not be used for a
	 * radius. Convert with the resolution instead. */
	tp->scroll.circular.edge_min_x =
		max(1, (int32_t)(min_mm.x * device->abs.absinfo_x->resolution));
	tp->scroll.circular.edge_min_y =
		max(1, (int32_t)(min_mm.y * device->abs.absinfo_y->resolution));

	dead_units =
		(int32_t)(dead_mm.x * device->abs.absinfo_x->resolution);
	if (dead_units <= 0)
		dead_units = 1;
	tp->scroll.circular.dead_zone_radius_sq = dead_units * dead_units;

	tp->scroll.circular.angle_step = CIRCULAR_SCROLL_DEFAULT_STEP_RAD;
	tp->scroll.circular.round_pad =
		evdev_device_has_model_quirk(device, QUIRK_MODEL_CIRCULAR_TOUCHPAD);

	tp_for_each_touch(tp, t)
		tp_circular_scroll_reset_touch(t);

	tp_circular_scroll_config_init(tp);
}

void
tp_remove_circular_scroll(struct tp_dispatch *tp)
{
	/* Nothing to tear down — no timers, no allocations. */
}

void
tp_circular_scroll_handle_state(struct tp_dispatch *tp, usec_t time)
{
	struct tp_touch *t;

	if (!(tp->scroll.method & LIBINPUT_CONFIG_SCROLL_CIRCULAR))
		return;

	tp_for_each_touch(tp, t) {
		enum tp_circular_scroll_touch_state prev_state;

		if (!t->dirty)
			continue;

		prev_state = t->scroll.circular.state;

		switch (t->state) {
		case TOUCH_NONE:
		case TOUCH_HOVERING:
			break;
		case TOUCH_BEGIN:
			/* Circular scrolling is single-finger only. With
			 * more than one finger down the gesture handlers
			 * take over so that scrolling, swipes and pinches
			 * keep working. */
			if (tp->nfingers_down != 1) {
				t->scroll.circular.state =
					CIRCULAR_SCROLL_TOUCH_STATE_INNER;
				break;
			}

			/* Intent lock: whichever zone the touch starts in
			 * is where it stays for its entire lifetime. This
			 * is the "locks in until the finger is raised"
			 * behaviour of the Synaptics driver. */
			if (tp_circular_scroll_in_trigger(tp, &t->point)) {
				t->scroll.circular.state =
					CIRCULAR_SCROLL_TOUCH_STATE_RING;
				t->scroll.circular.prev_angle =
					tp_circular_scroll_angle(tp, &t->point);
			} else {
				t->scroll.circular.state =
					CIRCULAR_SCROLL_TOUCH_STATE_INNER;
			}
			break;
		case TOUCH_UPDATE:
			/* State is locked from TOUCH_BEGIN. Do nothing. */
			break;
		case TOUCH_MAYBE_END:
		case TOUCH_END:
			tp_circular_scroll_reset_touch(t);
			break;
		}

		if (prev_state != t->scroll.circular.state)
			evdev_log_debug(
				tp->device,
				"circular-scroll: touch %d %s → %s\n",
				t->index,
				circular_state_to_str(prev_state),
				circular_state_to_str(t->scroll.circular.state));
	}
}

int
tp_circular_scroll_post_events(struct tp_dispatch *tp, usec_t time)
{
	struct tp_touch *t;

	if (!(tp->scroll.method & LIBINPUT_CONFIG_SCROLL_CIRCULAR))
		return 0;

	if (tp->nfingers_down != 1)
		return 0;

	tp_for_each_touch(tp, t) {
		double angle, delta, steps;
		struct normalized_coords normalized = { 0.0, 0.0 };

		if (!t->dirty)
			continue;

		if (t->scroll.circular.state != CIRCULAR_SCROLL_TOUCH_STATE_RING)
			continue;

		if (t->palm.state != PALM_NONE || tp_thumb_ignored(tp, t))
			continue;

		/* Inside the dead zone the angle is not meaningful. Pause
		 * and re-baseline on the way out. */
		if (tp_circular_scroll_in_dead_zone(tp, &t->point)) {
			t->scroll.circular.prev_angle = NAN;
			continue;
		}

		angle = tp_circular_scroll_angle(tp, &t->point);

		/* Re-baseline on the first event after touch-down. */
		if (isnan(t->scroll.circular.prev_angle)) {
			t->scroll.circular.prev_angle = angle;
			continue;
		}

		delta = circular_scroll_angle_delta(t->scroll.circular.prev_angle,
						    angle);
		t->scroll.circular.prev_angle = angle;

		if (delta == 0.0)
			continue;

		t->scroll.circular.angle_accum += delta;

		steps = t->scroll.circular.angle_accum /
			tp->scroll.circular.angle_step;
		if (steps == 0.0)
			continue;

		/* Only emit whole steps, the remainder carries over to
		 * the next event. This gives smooth scrolling for slow
		 * rotation and doesn't lose fractions. */
		if (fabs(steps) < 1.0)
			continue;

		t->scroll.circular.angle_accum =
			fmod(t->scroll.circular.angle_accum,
			     tp->scroll.circular.angle_step);

		/* One scroll step is a fixed distance in normalized units,
		 * independent of how far from the centre the finger is.
		 * That is what makes small circles scroll faster: the
		 * same finger distance covers more angle. */
		normalized.y = steps * CIRCULAR_SCROLL_STEP_DISTANCE *
			       tp->scroll.speed.circular;

		evdev_notify_axis_finger(
			tp->device,
			time,
			bit(LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL),
			&normalized);
		t->scroll.circular.direction =
			LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
	}

	/* Return 0: we don't own the whole frame. Pointer motion for
	 * non-ring touches is suppressed upstream via our
	 * tp_circular_scroll_touch_active() hook — same mechanism
	 * edge-scroll uses. */
	return 0;
}

void
tp_circular_scroll_stop_events(struct tp_dispatch *tp, usec_t time)
{
	struct tp_touch *t;
	const struct normalized_coords zero = { 0.0, 0.0 };

	tp_for_each_touch(tp, t) {
		if (t->scroll.circular.direction != -1) {
			evdev_notify_axis_finger(
				tp->device,
				time,
				bit(t->scroll.circular.direction),
				&zero);
		}
		tp_circular_scroll_reset_touch(t);
	}
}

bool
tp_circular_scroll_is_active(const struct tp_dispatch *tp)
{
	struct tp_touch *t;

	tp_for_each_touch(tp, t) {
		switch (t->scroll.circular.state) {
		case CIRCULAR_SCROLL_TOUCH_STATE_RING:
			return true;
		case CIRCULAR_SCROLL_TOUCH_STATE_NONE:
		case CIRCULAR_SCROLL_TOUCH_STATE_INNER:
			break;
		}
	}

	return false;
}

bool
tp_circular_scroll_touch_active(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	if (!(tp->scroll.method & LIBINPUT_CONFIG_SCROLL_CIRCULAR))
		return true; /* touch is eligible for normal pointer motion */

	/* Only single-finger touches can be owned by circular scroll.
	 * With more than one finger down the gesture handlers need all
	 * of them. */
	if (tp->nfingers_down != 1)
		return true;

	/* Touches in the trigger region are owned by scroll; inner touches
	 * are free to drive pointer motion. */
	switch (t->scroll.circular.state) {
	case CIRCULAR_SCROLL_TOUCH_STATE_RING:
		return false;
	case CIRCULAR_SCROLL_TOUCH_STATE_INNER:
	case CIRCULAR_SCROLL_TOUCH_STATE_NONE:
		return true;
	}
	return true;
}
