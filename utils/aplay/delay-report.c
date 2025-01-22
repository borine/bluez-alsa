/*
 * BlueALSA - delay-reporting.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "delay-report.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <dbus/dbus.h>

#include "alsa-pcm.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"


void delay_report_init(struct delay_report *dr,
				struct ba_dbus_ctx *dbus_ctx,
				struct ba_pcm *ba_pcm,
				int ba_pcm_fd,
				size_t frame_size) {
	dr->dbus_ctx = dbus_ctx;
	dr->ba_pcm = ba_pcm;
	dr->ba_pcm_fd = ba_pcm_fd;
	memset(&dr->update_ts, 0, sizeof(dr->update_ts));
	memset(dr->values, 0, sizeof(dr->values));
	dr->avg_value = 0;
	dr->values_i = 0;
	dr->frame_size = frame_size;
}

void delay_report_reset(struct delay_report *dr) {
	memset(dr->values, 0, sizeof(dr->values));
	dr->values_i = 0;
}

bool delay_report_update(struct delay_report *dr,
			ffb_t *buffer, size_t alsa_delay,
			DBusError *err) {

	const size_t num_values = ARRAYSIZE(dr->values);
	unsigned int buffered = 0;
	snd_pcm_uframes_t delay_frames = alsa_delay;

	ioctl(dr->ba_pcm_fd, FIONREAD, &buffered);
	buffered += ffb_blen_out(buffer);
	delay_frames += buffered / dr->frame_size;

	dr->values[dr->values_i % num_values] = delay_frames;
	dr->values_i++;

	struct timespec ts_now;
	/* Rate limit delay updates to 1 update per second. */
	struct timespec ts_delay = { .tv_sec = 1 };

	gettimestamp(&ts_now);
	timespecadd(&dr->update_ts, &ts_delay, &ts_delay);

	snd_pcm_sframes_t delay_frames_avg = 0;
	for (size_t i = 0; i < num_values; i++)
		delay_frames_avg += dr->values[i];
	if (dr->values_i < num_values)
		delay_frames_avg /= dr->values_i;
	else
		delay_frames_avg /= num_values;
	dr->avg_value = delay_frames_avg;

	const int delay = delay_frames_avg * 10000 / dr->ba_pcm->rate;
	if (difftimespec(&ts_now, &ts_delay, &ts_delay) < 0 &&
			abs(delay - dr->ba_pcm->client_delay) >= 100 /* 10ms */) {

		dr->update_ts = ts_now;

		dr->ba_pcm->client_delay = delay;
		return ba_dbus_pcm_update(dr->dbus_ctx, dr->ba_pcm, BLUEALSA_PCM_CLIENT_DELAY, err);
	}

	return true;
}
