/*
 * BlueALSA - single-playback.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_SINGLE_PLAYBACK_H_
#define BLUEALSA_APLAY_SINGLE_PLAYBACK_H_

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	/* D-Bus object path to device */
	const char *device;
	/* Track the lock state of the single playback mutex within this thread. */
	bool locked;
	/* Time interval between repeat pause requests, expressed as a count of
	 * samples received from source */
	size_t pause_retry_interval;
	/* Count of samples received since last pause request. */
	size_t pause_retry_pcm_samples;
	/* Count of Pause command repeats */
	size_t pause_retries;
} single_playback_t;

void single_playback_init(single_playback_t *sp, const char *device, size_t interval_samples);
void single_playback_lock(single_playback_t *sp);
void single_playback_unlock(single_playback_t *sp);
void single_playback_pause(single_playback_t *sp, size_t input_samples);
void single_playback_reset(single_playback_t *sp);

#endif
