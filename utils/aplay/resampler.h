/*
 * BlueALSA - resampler.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_RESAMPLER_H_
#define BLUEALSA_APLAY_RESAMPLER_H_

#include <stdbool.h>

#include <alsa/asoundlib.h>
#include <samplerate.h>

#include "shared/ffb.h"

enum resampler_converter_type {
	RESAMPLER_CONV_NONE                   = -1,
	RESAMPLER_CONV_SINC_BEST_QUALITY      = SRC_SINC_BEST_QUALITY,
	RESAMPLER_CONV_SINC_MEDIUM_QUALITY    = SRC_SINC_MEDIUM_QUALITY,
	RESAMPLER_CONV_SINC_FASTEST           = SRC_SINC_FASTEST,
	RESAMPLER_CONV_ZERO_ORDER_HOLD        = SRC_ZERO_ORDER_HOLD,
	RESAMPLER_CONV_LINEAR                 = SRC_LINEAR,
};

struct resampler;

bool resampler_is_input_format_supported(snd_pcm_format_t format);
bool resampler_is_output_format_supported(snd_pcm_format_t format);

struct resampler *resampler_new(
		enum resampler_converter_type type,
		unsigned int channels,
		snd_pcm_format_t in_format,
		unsigned int in_rate,
		snd_pcm_format_t out_format,
		unsigned int out_rate,
		snd_pcm_uframes_t min_target,
		snd_pcm_uframes_t max_target,
		size_t buffer_size);

void resampler_destroy(
		struct resampler *resampler);

int resampler_process(
		struct resampler *resampler,
		ffb_t *in,
		ffb_t *out);

void resampler_reset(
		struct resampler *resampler);

double resampler_current_rate_ratio(
		struct resampler *resampler);

bool resampler_update_rate_ratio(
		struct resampler *resampler,
		snd_pcm_uframes_t frames_read,
		snd_pcm_uframes_t delay);

void resampler_convert_to_native_endian_format(
		void *buffer, size_t len, snd_pcm_format_t format);

snd_pcm_format_t resampler_native_endian_format(snd_pcm_format_t format);
snd_pcm_format_t resampler_preferred_output_format(void);

#endif
