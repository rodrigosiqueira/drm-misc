// SPDX-License-Identifier: GPL-2.0
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vkms_drv.h"
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

static enum hrtimer_restart vkms_vblank_simulate(struct hrtimer *timer)
{
	ktime_t new_period, current_timestamp, diff_times, current_offset;
	struct vkms_output *output = container_of(timer, struct vkms_output,
						  vblank_hrtimer);
	struct drm_crtc *crtc = &output->crtc;
	unsigned long flags;
	int ret_overrun;
	bool ret;

	current_timestamp = ktime_get();
	current_offset = current_timestamp % output->period_ns;
	diff_times = ktime_sub(current_offset, output->base_offset);
	output->expires = ktime_sub(current_timestamp, diff_times);

	ret = drm_crtc_handle_vblank(crtc);
	if (!ret)
		DRM_ERROR("vkms failure on handling vblank");

	if (output->event) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, output->event);
		output->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		drm_crtc_vblank_put(crtc);
	}

	current_timestamp = ktime_get();
	current_offset = current_timestamp % output->period_ns;
	diff_times = ktime_sub(output->base_offset, current_offset);
	new_period = ktime_add(output->period_ns, diff_times);
	output->expires = ktime_add(current_timestamp, new_period);
	ret_overrun = hrtimer_forward_now(&output->vblank_hrtimer, new_period);

	return HRTIMER_RESTART;
}

static int vkms_enable_vblank(struct drm_crtc *crtc)
{
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);
	unsigned long period = 1000 * crtc->mode.htotal * crtc->mode.vtotal /
			       crtc->mode.clock;
	ktime_t current_timestamp;

	hrtimer_init(&out->vblank_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	out->vblank_hrtimer.function = &vkms_vblank_simulate;
	out->period_ns = ktime_set(0, period * US_TO_NS);
	current_timestamp = ktime_get();
	out->base_offset = current_timestamp % out->period_ns;
	out->expires = ktime_add(current_timestamp, out->period_ns);
	hrtimer_start(&out->vblank_hrtimer, out->period_ns, HRTIMER_MODE_REL);

	return 0;
}

static void vkms_disable_vblank(struct drm_crtc *crtc)
{
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);

	hrtimer_cancel(&out->vblank_hrtimer);
}

bool vkms_get_vblank_timestamp(struct drm_device *dev, unsigned int pipe,
			       int *max_error, ktime_t *vblank_time,
			       bool in_vblank_irq)
{
	struct vkms_device *vkmsdev = drm_device_to_vkms_device(dev);
	struct vkms_output *output = &vkmsdev->output;

	*vblank_time = output->expires;

	return true;
}

static const struct drm_crtc_funcs vkms_crtc_funcs = {
	.set_config             = drm_atomic_helper_set_config,
	.destroy                = drm_crtc_cleanup,
	.page_flip              = drm_atomic_helper_page_flip,
	.reset                  = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= vkms_enable_vblank,
	.disable_vblank		= vkms_disable_vblank,
};

static int vkms_crtc_atomic_check(struct drm_crtc *crtc,
				  struct drm_crtc_state *state)
{
	return 0;
}

static void vkms_crtc_atomic_enable(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_state)
{
	drm_crtc_vblank_on(crtc);
}

static void vkms_crtc_atomic_begin(struct drm_crtc *crtc,
				   struct drm_crtc_state *old_s)
{
	struct vkms_output *vkms_output = drm_crtc_to_vkms_output(crtc);

	if (crtc->state->event) {
		crtc->state->event->pipe = drm_crtc_index(crtc);
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		vkms_output->event = crtc->state->event;
		crtc->state->event = NULL;
	}
}

static const struct drm_crtc_helper_funcs vkms_crtc_helper_funcs = {
	.atomic_check	= vkms_crtc_atomic_check,
	.atomic_enable	= vkms_crtc_atomic_enable,
	.atomic_begin	= vkms_crtc_atomic_begin,
};

int vkms_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
		   struct drm_plane *primary, struct drm_plane *cursor)
{
	int ret;

	ret = drm_crtc_init_with_planes(dev, crtc, primary, cursor,
					&vkms_crtc_funcs, NULL);
	if (ret) {
		DRM_ERROR("Failed to init CRTC\n");
		return ret;
	}

	drm_crtc_helper_add(crtc, &vkms_crtc_helper_funcs);

	return ret;
}
