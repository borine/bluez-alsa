/*
 * BlueALSA - aplay-config.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_CONFIG_H_
#define BLUEALSA_APLAY_CONFIG_H_

#include <stdbool.h>

#include "resampler.h"
#include "shared/dbus-client.h"

enum volume_type {
	VOL_TYPE_AUTO,
	VOL_TYPE_MIXER,
	VOL_TYPE_SOFTWARE,
	VOL_TYPE_NONE,
};

struct aplay_config {
	const char *pcm_device;
	const char *mixer_device;
	const char *mixer_elem_name;
	unsigned int mixer_elem_index;
	unsigned int pcm_buffer_time;
	unsigned int pcm_period_time;
	enum volume_type volume_type;
	bool force_single_playback;
	int main_loop_quit_event_fd;
	struct ba_dbus_ctx dbus_ctx;
	unsigned int verbose;
#if WITH_LIBSAMPLERATE
	enum resampler_converter_type resampler_method;
#endif
};

extern struct aplay_config config;

#endif
