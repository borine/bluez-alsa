/*
 * BlueALSA - delay-reporting.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_DELAYREPORTING_H_
#define BLUEALSA_APLAY_DELAYREPORTING_H_

#include <alsa/asoundlib.h>
#include <dbus/dbus.h>
#include <stdbool.h>
#include <sys/time.h>

#include "shared/ffb.h"


struct delay_report {
	struct ba_dbus_ctx *dbus_ctx;
	struct ba_pcm *ba_pcm;
	int ba_pcm_fd;
	/* The time-stamp for delay update rate limiting. */
	struct timespec update_ts;
	/* Window buffer for calculating delay moving average. */
	snd_pcm_sframes_t values[64];
	snd_pcm_sframes_t avg_value;
	size_t values_i;
	size_t frame_size;
};

void delay_report_init(struct delay_report *dr,
				struct ba_dbus_ctx *dbus_ctx,
				struct ba_pcm *ba_pcm,
				int ba_pcm_fd,
				size_t frame_size);

void delay_report_reset(struct delay_report *dr);

bool delay_report_update(struct delay_report *dr,
			ffb_t *buffer, size_t alsa_delay,
			DBusError *err);
#endif
