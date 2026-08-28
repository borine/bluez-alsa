/*
 * BlueALSA - alsa-mixer.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_ALSAMIXER_H_
#define BLUEALSA_APLAY_ALSAMIXER_H_

#include <poll.h>
#include <stdbool.h>

typedef void (*alsa_mixer_event_handler)(void *userdata);

void alsa_mixer_init(alsa_mixer_event_handler handler);

int alsa_mixer_open(char **err_msg);

void alsa_mixer_close(void);

bool alsa_mixer_is_open(void);

bool alsa_mixer_has_mute_switch(void);

int alsa_mixer_poll_descriptors_count(void);

int alsa_mixer_poll_descriptors(
		struct pollfd* pfds,
		unsigned int space);

int alsa_mixer_handle_events(void);

int alsa_mixer_get_volume_scaling(
		double *vol_scaling,
		bool *muted);

int alsa_mixer_set_volume_scaling(
		double vol_scaling,
		bool muted);

#endif
