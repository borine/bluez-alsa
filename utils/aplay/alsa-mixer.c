/*
 * BlueALSA - alsa-mixer.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "alsa-mixer.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "shared/log.h"

static int alsa_mixer_elem_callback(snd_mixer_elem_t *elem, unsigned int mask) {
	struct alsa_mixer *mixer = snd_mixer_elem_get_callback_private(elem);
	if (mask & SND_CTL_EVENT_MASK_REMOVE) {
		/* The element has been removed and cannot now be used - we must close
		 * the mixer. */
		 return -1;
	}
	if (mask & SND_CTL_EVENT_MASK_VALUE)
		mixer->event_handler(mixer->event_data);

	return 0;
}

void alsa_mixer_init(struct alsa_mixer *mixer,
		alsa_mixer_event_handler event_handler, void *event_data) {
	memset(mixer, 0, sizeof(*mixer));
	mixer->volume_db_max_value = 0;
	mixer->event_handler = event_handler;
	mixer->event_data = event_data;
}

int alsa_mixer_open(struct alsa_mixer *mixer, const char *dev_name,
		const char *elem_name, unsigned int elem_idx, char **msg) {

	assert (mixer->handle == NULL);

	char buf[256];
	long vmin_db;
	int err;

	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, elem_name);
	snd_mixer_selem_id_set_index(id, elem_idx);

	if ((err = snd_mixer_open(&mixer->handle, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Open mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_attach(mixer->handle, dev_name)) != 0) {
		snprintf(buf, sizeof(buf), "Attach mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_selem_register(mixer->handle, NULL, NULL)) != 0) {
		snprintf(buf, sizeof(buf), "Register mixer class: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_load(mixer->handle)) != 0) {
		snprintf(buf, sizeof(buf), "Load mixer elements: %s", snd_strerror(err));
		goto fail;
	}
	if ((mixer->elem = snd_mixer_find_selem(mixer->handle, id)) == NULL) {
		snprintf(buf, sizeof(buf), "Mixer element not found");
		err = -1;
		goto fail;
	}

	mixer->has_mute_switch = snd_mixer_selem_has_playback_switch(mixer->elem);
	if ((err = snd_mixer_selem_get_playback_dB_range(mixer->elem,
					&vmin_db, &mixer->volume_db_max_value)) != 0)
		warn("Couldn't get ALSA mixer playback dB range: %s", snd_strerror(err));

	snd_mixer_elem_set_callback(mixer->elem, alsa_mixer_elem_callback);
	snd_mixer_elem_set_callback_private(mixer->elem, mixer);

	return 0;

fail:
	if (mixer->handle != NULL)
		snd_mixer_close(mixer->handle);
	mixer->handle = NULL;
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

int alsa_mixer_get_single_volume(const struct alsa_mixer *mixer,
		unsigned int vmax, int *loudness, bool *muted) {

	snd_mixer_elem_t *elem = mixer->elem;
	long long volume_db_sum = 0;
	bool alsa_muted = true;

	snd_mixer_selem_channel_id_t ch;
	for (ch = 0; snd_mixer_selem_has_playback_channel(elem, ch) == 1; ch++) {

		long ch_volume_db;
		int ch_switch = 1;
		int err;

		if ((err = snd_mixer_selem_get_playback_dB(elem, ch, &ch_volume_db)) != 0) {
			error("Couldn't get ALSA mixer playback dB level: %s", snd_strerror(err));
			return -1;
		}

		/* Mute switch is an optional feature for a mixer element. */
		if (mixer->has_mute_switch &&
				(err = snd_mixer_selem_get_playback_switch(elem, ch, &ch_switch)) != 0) {
			error("Couldn't get ALSA mixer playback switch: %s", snd_strerror(err));
			return -1;
		}

		volume_db_sum += ch_volume_db;
		/* Normalize volume level so it will not exceed 0.00 dB. */
		volume_db_sum -= mixer->volume_db_max_value;

		if (ch_switch == 1)
			alsa_muted = false;

	}

	/* Safety check for undefined behavior from
	 * out-of-bounds dB conversion. */
	assert(volume_db_sum <= 0LL);

	/* Convert dB to loudness using decibel formula and
	 * round to the nearest integer. */
	*loudness = lround(pow(2, (0.01 * volume_db_sum / ch) / 10) * vmax);

	/* If mixer element does not support playback switch,
	 * use our global muted state. */
	if (mixer->has_mute_switch)
		*muted = alsa_muted;

	return 0;
}

int alsa_mixer_set_single_volume(struct alsa_mixer *mixer, long dB, bool muted) {

	dB += mixer->volume_db_max_value;

	int err;
	if ((err = snd_mixer_selem_set_playback_dB_all(mixer->elem, dB, 0)) != 0) {
		error("Couldn't set ALSA mixer playback dB level: %s", snd_strerror(err));
		return -1;
	}

	/* mute switch is an optional feature for a mixer element */
	if (mixer->has_mute_switch &&
			(err = snd_mixer_selem_set_playback_switch_all(mixer->elem, !muted)) != 0) {
		error("Couldn't set ALSA mixer playback mute switch: %s", snd_strerror(err));
		return -1;
	}

	return 0;
}

void alsa_mixer_close(struct alsa_mixer *mixer) {
	if (mixer->handle != NULL)
		snd_mixer_close(mixer->handle);
	mixer->handle = NULL;
	mixer->elem = NULL;
}
