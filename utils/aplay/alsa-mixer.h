/*
 * BlueALSA - alsa-mixer.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_ALSAMIXER_H_
#define BLUEALSA_APLAY_ALSAMIXER_H_

#include <alsa/asoundlib.h>
#include <stdbool.h>

typedef void (*alsa_mixer_event_handler)(void *data);

struct alsa_mixer {
	snd_mixer_t *handle;
	snd_mixer_elem_t *elem;
	long volume_db_max_value;
	bool has_mute_switch;
	alsa_mixer_event_handler event_handler;
	void *event_data;
};

void alsa_mixer_init(struct alsa_mixer *mixer,
		alsa_mixer_event_handler event_handler, void *event_data);

int alsa_mixer_open(struct alsa_mixer *mixer, const char *dev_name,
		const char *elem_name, unsigned int elem_idx, char **msg);

inline static bool alsa_mixer_is_open(const struct alsa_mixer *mixer) {
	return mixer->handle != NULL && mixer->elem != NULL;
}

int alsa_mixer_get_single_volume(const struct alsa_mixer *mixer,
		unsigned int vmax, int *loudness, bool *muted);

int alsa_mixer_set_single_volume(struct alsa_mixer *mixer, long dB, bool muted);

inline static int alsa_mixer_poll_descriptors_count(struct alsa_mixer *mixer) {
	return snd_mixer_poll_descriptors_count(mixer->handle);
}
inline static int alsa_mixer_poll_descriptors(struct alsa_mixer *mixer, struct pollfd* pfds,
		unsigned int space) {
	return snd_mixer_poll_descriptors(mixer->handle, pfds, space);
}

inline static int alsa_mixer_handle_events(struct alsa_mixer *mixer) {
	return snd_mixer_handle_events(mixer->handle);
}

void alsa_mixer_close(struct alsa_mixer *mixer);

#endif
