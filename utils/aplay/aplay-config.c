/*
 * BlueALSA - aplay-config.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include "aplay-config.h"
#include "resampler.h"
#include "shared/dbus-client.h"



struct aplay_config config = {
	.pcm_device = "default",
	.mixer_device = "default",
	.mixer_elem_name = "Master",
	.mixer_elem_index = 0,
	.pcm_buffer_time = 0,
	.pcm_period_time = 0,
	.volume_type = VOL_TYPE_AUTO,
	.force_single_playback = false,
	.main_loop_quit_event_fd = -1,
	.dbus_ctx = { 0 },
	.verbose = false,
#if WITH_LIBSAMPLERATE
	.resampler_method = RESAMPLER_CONV_NONE,
#endif
};
