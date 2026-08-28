/*
 * BlueALSA - alsa-mixer.c
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "alsa-mixer.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "aplay-config.h"
#include "shared/log.h"

static struct {

	/* The ALSA mixer handle. */
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;

	bool has_db_scale;
	bool has_mute_switch;
	long volume_min_value;
	long volume_max_value;

	alsa_mixer_event_handler event_handler;

	pthread_mutex_t mutex;

} mixer = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
};

static int alsa_mixer_elem_callback(snd_mixer_elem_t *elem, unsigned int mask) {
	(void) elem;
	if (mask == SND_CTL_EVENT_MASK_REMOVE)
		/* The element has been removed and cannot
		 * now be used - we must close the mixer. */
		 return -1;
	if (mask & SND_CTL_EVENT_MASK_VALUE)
		mixer.event_handler(NULL);
	return 0;
}

void alsa_mixer_init(alsa_mixer_event_handler handler) {
	mixer.event_handler = handler;
}

int alsa_mixer_open(char **err_msg) {
	char buf[256];
	int err = 0;

	pthread_mutex_lock(&mixer.mutex);

	if (mixer.mixer != NULL && mixer.elem != NULL) {
		pthread_mutex_unlock(&mixer.mutex);
		return 0;
	}

	debug("Opening ALSA mixer: name=%s elem=%s index=%u",
			config.mixer_device, config.mixer_elem_name,
			config.mixer_elem_index);

	if (mixer.mixer == NULL) {
		if ((err = snd_mixer_open(&mixer.mixer, 0)) != 0) {
			snprintf(buf, sizeof(buf), "Open mixer: %s", snd_strerror(err));
			goto fail;
		}
	}
	if ((err = snd_mixer_attach(mixer.mixer, config.mixer_device)) != 0) {
		snprintf(buf, sizeof(buf), "Attach mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_selem_register(mixer.mixer, NULL, NULL)) != 0) {
		snprintf(buf, sizeof(buf), "Register mixer class: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_load(mixer.mixer)) != 0) {
		snprintf(buf, sizeof(buf), "Load mixer elements: %s", snd_strerror(err));
		goto fail;
	}

	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, config.mixer_elem_name);
	snd_mixer_selem_id_set_index(id, config.mixer_elem_index);
	if ((mixer.elem = snd_mixer_find_selem(mixer.mixer, id)) == NULL) {
		snprintf(buf, sizeof(buf), "Mixer element '%s',%u not found", config.mixer_elem_name, config.mixer_elem_index);
		err = -1;
		goto fail;
	}

	mixer.has_mute_switch = snd_mixer_selem_has_playback_switch(mixer.elem);

	/* To determine whether a control has a dB scale defined, we fetch the
	 * dB scale limits and check that they are valid. */
	mixer.has_db_scale = (snd_mixer_selem_get_playback_dB_range(mixer.elem,
					&mixer.volume_min_value, &mixer.volume_max_value) == 0 &&
					mixer.volume_min_value < mixer.volume_max_value);

	/* For controls that lack a dB scale, we assume a simple linear scale
	 * provided that we can obtain valid scale limits. */
	if (!mixer.has_db_scale &&
			(((err = snd_mixer_selem_get_playback_volume_range(mixer.elem,
					&mixer.volume_min_value, &mixer.volume_max_value)) != 0) ||
					mixer.volume_min_value >= mixer.volume_max_value)) {
		snprintf(buf, sizeof(buf), "Couldn't get playback volume range: %s", snd_strerror(err));
		if (err == 0)
			err = EIO;
		goto fail;
	}

	snd_mixer_elem_set_callback(mixer.elem, alsa_mixer_elem_callback);
	snd_mixer_elem_set_callback_private(mixer.elem, NULL);

	pthread_mutex_unlock(&mixer.mutex);
	return 0;

fail:
	if (mixer.mixer != NULL)
		snd_mixer_close(mixer.mixer);
	mixer.mixer = NULL;
	mixer.elem = NULL;
	pthread_mutex_unlock(&mixer.mutex);

	if (err_msg != NULL)
		*err_msg = strdup(buf);
	return err;
}

void alsa_mixer_close(void) {
	pthread_mutex_lock(&mixer.mutex);
	if (mixer.mixer != NULL)
		snd_mixer_close(mixer.mixer);
	mixer.mixer = NULL;
	mixer.elem = NULL;
	pthread_mutex_unlock(&mixer.mutex);
}

bool alsa_mixer_is_open(void) {
	bool ret;
	pthread_mutex_lock(&mixer.mutex);
	ret = mixer.mixer != NULL && mixer.elem != NULL;
	pthread_mutex_unlock(&mixer.mutex);
	return ret;
}

bool alsa_mixer_has_mute_switch() {
	bool ret;
	pthread_mutex_lock(&mixer.mutex);
	ret = mixer.has_mute_switch;
	pthread_mutex_unlock(&mixer.mutex);
	return ret;
}

int alsa_mixer_poll_descriptors_count(void) {
	return snd_mixer_poll_descriptors_count(mixer.mixer);
}

int alsa_mixer_poll_descriptors(
		struct pollfd* pfds,
		unsigned int space) {
	return snd_mixer_poll_descriptors(mixer.mixer, pfds, space);
}

int alsa_mixer_handle_events(void) {
	return snd_mixer_handle_events(mixer.mixer);
}

int alsa_mixer_get_volume_scaling(
		double *vol_scaling,
		bool *muted) {

	pthread_mutex_lock(&mixer.mutex);
	snd_mixer_elem_t *elem = mixer.elem;
	long long volume_sum = 0;
	bool alsa_muted = true;
	int err = 0;

	snd_mixer_selem_channel_id_t ch;
	for (ch = 0; snd_mixer_selem_has_playback_channel(elem, ch) == 1; ch++) {

		long ch_volume;
		int ch_switch = 1;

		if (mixer.has_db_scale) {
			if ((err = snd_mixer_selem_get_playback_dB(elem, ch, &ch_volume)) != 0) {
				error("Couldn't get ALSA mixer playback dB level: %s", snd_strerror(err));
				goto finish;
			}
		}
		else {
			if ((err = snd_mixer_selem_get_playback_volume(elem, ch, &ch_volume)) != 0) {
				error("Couldn't get ALSA mixer playback volume level: %s", snd_strerror(err));
				goto finish;
			}
		}

		/* Mute switch is an optional feature for a mixer element. */
		if (mixer.has_mute_switch &&
				(err = snd_mixer_selem_get_playback_switch(elem, ch, &ch_switch)) != 0) {
			error("Couldn't get ALSA mixer playback switch: %s", snd_strerror(err));
			goto finish;
		}

		volume_sum += ch_volume;

		if (ch_switch == 1)
			alsa_muted = false;

	}

	if (mixer.has_db_scale) {
		/* Normalize volume level so it will not exceed 0.00 dB. */
		volume_sum -= ch * mixer.volume_max_value;
		/* Safety check for undefined behavior from
		 * out-of-bounds dB conversion. */
		assert(volume_sum <= 0LL);
		/* Convert dB to loudness using decibel formula and
		 * round to the nearest integer. */
		*vol_scaling = pow(2, (0.01 * volume_sum / ch) / 10);
	}
	else {
		/* For raw linear scale use average value of channels. */
		volume_sum /= ch;
		/* Ensure returned volume is within valid range. */
		if (volume_sum < mixer.volume_min_value)
			volume_sum = mixer.volume_min_value;
		if (volume_sum > mixer.volume_max_value)
			volume_sum = mixer.volume_max_value;
		const long range = mixer.volume_max_value - mixer.volume_min_value;
		*vol_scaling = (double)(volume_sum - mixer.volume_min_value) / range;
	}

	/* If mixer element supports playback switch,
	 * return the actual mute state to the caller. */
	if (mixer.has_mute_switch)
		*muted = alsa_muted;

finish:
	pthread_mutex_unlock(&mixer.mutex);
	return err;
}

int alsa_mixer_set_volume_scaling(
		double vol_scaling,
		bool muted) {

	int err = 0;

	pthread_mutex_unlock(&mixer.mutex);

	if (mixer.has_db_scale) {
		/* Convert loudness to dB using decibel formula. */
		long db = 10 * log2(vol_scaling) * 100;
		/* Shift dB level so it will match hardware range. */
		db += mixer.volume_max_value;
		if ((err = snd_mixer_selem_set_playback_dB_all(mixer.elem, db, 0)) != 0) {
			error("Couldn't set ALSA mixer playback dB level: %s", snd_strerror(err));
			goto finish;
		}
	}
	else {
		const long range = mixer.volume_max_value - mixer.volume_min_value;
		const long value = mixer.volume_min_value + (range * vol_scaling);
		if ((err = snd_mixer_selem_set_playback_volume_all(mixer.elem, value)) != 0) {
			error("Couldn't set ALSA mixer playback volume level: %s", snd_strerror(err));
			goto finish;
		}
	}

	/* Mute switch is an optional feature for a mixer element. */
	if (mixer.has_mute_switch &&
			(err = snd_mixer_selem_set_playback_switch_all(mixer.elem, !muted)) != 0) {
		error("Couldn't set ALSA mixer playback mute switch: %s", snd_strerror(err));
			goto finish;
	}

finish:
	return err;
}
