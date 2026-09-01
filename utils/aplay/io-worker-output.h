/*
 * BlueALSA - io-worker-output.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_IOWORKER_OUTPUT_H_
#define BLUEALSA_APLAY_IOWORKER_OUTPUT_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <alsa/asoundlib.h>
#include <stddef.h>

#include "alsa-pcm.h"
#include "io-worker.h"
#include "shared/ffb.h"
#if WITH_LIBSAMPLERATE
# include "resampler.h"
#endif

typedef struct {
	size_t pcm_open_retry_pcm_samples;
	size_t pcm_open_retries;
	/* Buffer from which audio frames are written to the ALSA PCM. If the
	 * resampler is used then this is the resampler output buffer. Otherwise
	 * it is the same as the read buffer. */
	ffb_t *write_buffer;
	struct alsa_pcm	pcm;
	/* Preferred format for the ALSA PCM. If not using the resampler then this
	 * is the format of the incoming BlueALSA stream. */
	snd_pcm_format_t format_1;
	/* Alternative format that can be generated internally by the resampler.
	 * This is only used if the resampler is enabled. */
	snd_pcm_format_t format_2;
	unsigned int channels;
	unsigned int in_rate;
	int pcm_flags;
#if WITH_LIBSAMPLERATE
	struct resampler resampler;
	snd_pcm_format_t resampler_format;
	ffb_t resampled_buffer;
	/* For detecting when the ALSA device has auto-started after reaching its
	 * start threshold. */
	bool alsa_pcm_started;
	bool use_resampler;
#endif
} io_worker_output_t;

bool io_worker_output_init(
					io_worker_output_t *output,
					snd_pcm_format_t input_format,
					unsigned int channels,
					unsigned int input_rate);

void io_worker_output_free(io_worker_output_t *output);

bool io_worker_output_open(
					io_worker_output_t *output,
					ffb_t *read_buffer, size_t read_samples);

void io_worker_output_close(io_worker_output_t *output);

int io_worker_output_write(io_worker_output_t *output,
					ffb_t *input_buffer,
					bool mute,
					bool drain);

snd_pcm_uframes_t io_worker_output_delay(
					const io_worker_output_t *output);

void io_worker_output_update_rate(io_worker_output_t *output,
					size_t frames_read, size_t delay);

static inline void io_worker_output_fix_endianness(
					io_worker_output_t *output,
					void *buffer,
					size_t len,
					snd_pcm_format_t format) {
#if WITH_LIBSAMPLERATE
	if (output->use_resampler)
		resampler_convert_to_native_endian_format(buffer, len, format);
#else
	(void) output;
	(void) buffer;
	(void) len;
	(void) format;
#endif
}

static inline bool io_worker_output_is_open(const io_worker_output_t *output) {
	return alsa_pcm_is_open(&output->pcm);
}

static inline bool io_worker_output_is_running(const io_worker_output_t *output) {
	return alsa_pcm_is_running(&output->pcm);
}

#endif
