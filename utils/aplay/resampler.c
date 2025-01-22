/*
 * BlueALSA - resampler.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <alsa/asoundlib.h>
#include <samplerate.h>

#include <errno.h>
#include <stdlib.h>
#include <sys/param.h>

#include "shared/log.h"
#include "shared/ffb.h"

#include "resampler.h"

# define RESAMPLER_TOLERANCE_MS 2

struct aplay_resampler {
	SRC_STATE *src_state;
	SRC_DATA src_data;
	float *in_buffer;
	float *out_buffer;
	unsigned int channels;
	snd_pcm_format_t in_format;
	snd_pcm_format_t out_format;
	snd_pcm_uframes_t max_frames;
	double nominal_rate_ratio;
	double rate_delta;
	snd_pcm_uframes_t target_delay;
	snd_pcm_uframes_t delay_tolerance;
	snd_pcm_sframes_t delay_diff;
};

bool resampler_supports_format(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16_LE ||
		format == SND_PCM_FORMAT_S32_LE ||
		format == SND_PCM_FORMAT_FLOAT_LE;
}

void resampler_delete(struct aplay_resampler *resampler) {
	if (resampler == NULL || resampler->src_state == NULL)
		return;
	src_delete(resampler->src_state);
	if (resampler->in_format != SND_PCM_FORMAT_FLOAT_LE)
		free(resampler->in_buffer);
	if (resampler->out_format != SND_PCM_FORMAT_FLOAT_LE)
		free(resampler->out_buffer);
	free(resampler);
}

struct aplay_resampler *resampler_create(
			enum aplay_converter converter_type,
			unsigned int channels,
			snd_pcm_format_t in_format,
			unsigned int in_rate,
			snd_pcm_format_t out_format,
			unsigned int out_rate,
			snd_pcm_uframes_t max_frames) {

	/* Check formats can be resampled */
	if (!resampler_supports_format(in_format) || !resampler_supports_format(out_format)) {
		errno = EINVAL;
		return NULL;
	}

	struct aplay_resampler *resampler = calloc(1, sizeof(struct aplay_resampler));
	if (resampler == NULL) {
		debug("Unable to create resampler: %s", strerror(errno));
		return NULL;
	}

	int error;
	resampler->src_state = src_new(converter_type, channels, &error);
	if (resampler->src_state == NULL) {
		debug("Unable to create resampler: %s", src_strerror(error));
		goto fail;
	}

	if (in_format != SND_PCM_FORMAT_FLOAT_LE) {
		resampler->in_buffer = calloc(max_frames * channels, sizeof(float));
		if (resampler->in_buffer == NULL)
			goto fail;
	}

	if (out_format != SND_PCM_FORMAT_FLOAT_LE) {
		resampler->out_buffer = calloc(max_frames * channels, sizeof(float));
		if (resampler->out_buffer == NULL)
			goto fail;
	}

	resampler->channels = channels;
	resampler->in_format = in_format;
	resampler->out_format = out_format;
	resampler->max_frames = max_frames;
	resampler->rate_delta = 1.0 / ((double)in_rate * 8);
	resampler->delay_tolerance = RESAMPLER_TOLERANCE_MS * in_rate / 1000;
	resampler->nominal_rate_ratio = (double)out_rate / (double)in_rate;
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;

	return resampler;

fail:
	resampler_delete(resampler);
	return NULL;
}

int resampler_process(struct aplay_resampler *resampler, ffb_t *in, ffb_t *out) {
	int error = 0;
	SRC_DATA *src_data = &resampler->src_data;
	snd_pcm_uframes_t frames_used = 0;

	// fix input endianness ??

	/* We must ensure that we only process as many samples as will fit into
	 * the out buffer. */
	size_t out_samples = ffb_len_in(out);
	size_t max_in_samples = out_samples / resampler->src_data.src_ratio;

	size_t in_samples = MIN(ffb_len_out(in), max_in_samples);
	if (resampler->in_format == SND_PCM_FORMAT_S16_LE) {
		in_samples = MIN(in_samples, resampler->max_frames * resampler->channels);
		src_short_to_float_array(in->data, resampler->in_buffer, in_samples);
		src_data->data_in = resampler->in_buffer;
	}
	else if (resampler->in_format == SND_PCM_FORMAT_S32_LE) {
		in_samples = MIN(in_samples, resampler->max_frames * resampler->channels);
		src_int_to_float_array(in->data, resampler->in_buffer, in_samples);
		src_data->data_in = resampler->in_buffer;
	}
	else {
		src_data->data_in = in->data;
	}
	src_data->input_frames = in_samples / resampler->channels;

	if (resampler->out_format == SND_PCM_FORMAT_FLOAT_LE) {
		src_data->data_out = out->tail;
		src_data->output_frames = out_samples / resampler->channels;
	}
	else {
		src_data->data_out = resampler->out_buffer;
		src_data->output_frames = resampler->max_frames / resampler->channels;
	}

	while (true) {
		if ((error = src_process(resampler->src_state, src_data)) != 0) {
			error("Resampler failed: %s", src_strerror(error));
			return error;
		}
		if (src_data->output_frames_gen == 0)
			break;

		src_data->data_in += src_data->input_frames_used * resampler->channels;
		src_data->input_frames -= src_data->input_frames_used;
		frames_used += src_data->input_frames_used;
		src_data->output_frames -= src_data->output_frames_gen;

		if (resampler->out_format == SND_PCM_FORMAT_S16_LE)
			src_float_to_short_array(resampler->out_buffer, out->tail, src_data->output_frames_gen * resampler->channels);
		else if (resampler->out_format == SND_PCM_FORMAT_S32_LE)
			src_float_to_int_array(resampler->out_buffer, out->tail, src_data->output_frames_gen * resampler->channels);

		ffb_seek(out, src_data->output_frames_gen * resampler->channels);
	}
	ffb_shift(in, frames_used * resampler->channels);

	// fix output endianness ??

	return error;
}

/**
 * @return true if the rate ratio was changed.
 */
bool resampler_update_rate_ratio(
			struct aplay_resampler *resampler,
			snd_pcm_uframes_t delay) {

	if (resampler->target_delay == 0)
		return false;

	double old_ratio = resampler->src_data.src_ratio;
	snd_pcm_sframes_t delay_diff = delay - resampler->target_delay;
	if (labs(delay_diff) > resampler->delay_tolerance) {
		if (delay_diff > 0 && delay_diff >= resampler->delay_diff)
			resampler->src_data.src_ratio -= resampler->rate_delta;
		else if (delay_diff < 0 && delay_diff <= resampler->delay_diff)
			resampler->src_data.src_ratio += resampler->rate_delta;
	}
	else if (labs(resampler->delay_diff) > resampler->delay_tolerance) {
		if (resampler->delay_diff > 0)
			resampler->src_data.src_ratio += resampler->rate_delta;
		else
			resampler->src_data.src_ratio -= resampler->rate_delta;
	}
	resampler->delay_diff = delay_diff;
	return resampler->src_data.src_ratio != old_ratio;
}

void resampler_reset(struct aplay_resampler *resampler, snd_pcm_uframes_t target) {
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;
	resampler->target_delay = target;
}

double resampler_current_rate_ratio(struct aplay_resampler *resampler) {
	return resampler->src_data.src_ratio;
}

bool resampler_ready(struct aplay_resampler *resampler) {
	return resampler->target_delay != 0;
}
