/*
 * BlueALSA - alsa-pcm.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_ALSAPCM_H_
#define BLUEALSA_APLAY_ALSAPCM_H_

#include <stdbool.h>
#include <stdio.h>

#include <alsa/asoundlib.h>

#include "shared/ffb.h"

struct alsa_pcm {
	snd_pcm_format_t format;
	int channels;
	unsigned int rate;
	unsigned int buffer_time;
	unsigned int period_time;
	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t period_frames;
	snd_pcm_uframes_t start_threshold;
	snd_pcm_uframes_t underrun_threshold;
	size_t sample_size;
	size_t frame_size;
	size_t delay;
	bool underrun;
	snd_pcm_t *handle;
};

void alsa_pcm_init(struct alsa_pcm *pcm);

int alsa_pcm_open(
		struct alsa_pcm *pcm, const char *name,
		snd_pcm_format_t format_1, snd_pcm_format_t format_2,
		int channels, unsigned int rate,
		unsigned int buffer_time, unsigned int period_time,
		int flags, char **msg);

inline static bool alsa_pcm_is_open(const struct alsa_pcm *pcm) {
	return pcm->handle != NULL;
}

inline static bool alsa_pcm_is_running(const struct alsa_pcm *pcm) {
	return snd_pcm_state(pcm->handle) == SND_PCM_STATE_RUNNING;
}

inline static ssize_t alsa_pcm_frames_to_bytes(const struct alsa_pcm *pcm,
				snd_pcm_sframes_t frames) {
	return snd_pcm_frames_to_bytes(pcm->handle, frames);
}

int alsa_pcm_write(struct alsa_pcm *pcm, ffb_t *buffer, bool drain, bool verbose);
void alsa_pcm_dump(const struct alsa_pcm *pcm, FILE *fp);
void alsa_pcm_close(struct alsa_pcm *pcm);


#endif
