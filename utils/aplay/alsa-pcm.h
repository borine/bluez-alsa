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

	/* The ALSA device handle. */
	snd_pcm_t *pcm;

	snd_pcm_format_t format;
	/* The hardware parameters actually set within the ALSA device. Note that
	 * these may differ from the values requested by the application. */
	unsigned int channels;
	unsigned int rate;
	unsigned int buffer_time;
	unsigned int period_time;
	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t period_frames;

	/* The number of frames that must be written to trigger
	 * automatic start of the ALSA device. */
	snd_pcm_uframes_t start_threshold;

	/* The number of frames below which we are going to pad
	 * the buffer with silence to prevent underrun. */
	snd_pcm_uframes_t underrun_threshold;
	/* Indicates whether the last write recovered from an underrun. */
	bool underrun;

	/* The number of bytes in 1 sample. */
	size_t sample_size;
	/* The number of bytes in 1 frame. */
	size_t frame_size;

	/* The internal delay of the ALSA device immediately
	 * after the last write. */
	size_t delay;

};

void alsa_pcm_init(
		struct alsa_pcm *pcm);

int alsa_pcm_open(
		struct alsa_pcm *pcm,
		const char *name,
		snd_pcm_format_t format,
		unsigned int channels,
		unsigned int rate,
		unsigned int buffer_time,
		unsigned int period_time,
		int flags,
		char **msg);

void alsa_pcm_close(
		struct alsa_pcm *pcm);

inline static bool alsa_pcm_is_open(
		const struct alsa_pcm *pcm) {
	return pcm->pcm != NULL;
}

inline static ssize_t alsa_pcm_frames_to_bytes(
		const struct alsa_pcm *pcm,
		snd_pcm_sframes_t frames) {
	return snd_pcm_frames_to_bytes(pcm->pcm, frames);
}

int alsa_pcm_write(
		struct alsa_pcm *pcm,
		ffb_t *buffer,
		bool drain,
		unsigned int verbose);

void alsa_pcm_dump(
		const struct alsa_pcm *pcm,
		FILE *fp);

#endif
