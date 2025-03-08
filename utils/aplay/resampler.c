/*
 * BlueALSA - resampler.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <alsa/asoundlib.h>
#include <samplerate.h>

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/param.h>

#include "shared/log.h"
#include "shared/ffb.h"
#include "shared/rt.h"

#include "resampler.h"

/* How many milliseconds to allow the delay to change before adjusting the
 * resampling rate. This value must allow the delay to vary due to timer
 * jitter without triggering a rate change. */
# define RESAMPLER_TOLERANCE_MS 3

/* How many milliseconds to wait for the delay value to stabilize after a
 * reset. */
#define RESAMPLER_STABILIZE_MS 5000

/* Step size of rate adjustment */
#define RESAMPLER_STEP_SIZE 0.000004

/* Limit how many increment steps can be made when adjusting rate ratio. */
#define RESAMPLER_MAX_STEPS 100

/* Ignore rapid changes in delay since such changes can only result from
 * stream discontinuities, not timer drift. */
#define RESAMPLER_MAX_CHANGE_MS 10

/* Minimum time in milliseconds between rate ratio adjustments */
#define RESAMPLER_PERIOD_MS 100

struct aplay_resampler {
	/* libasound state and configuration data */
	SRC_STATE *src_state;
	SRC_DATA src_data;
	/* buffers for converting from integer to float sample formats */
	float *in_buffer;
	float *out_buffer;
	/* size (in samples) of the above conversion buffers */
	size_t buffer_size;
	/* number of channels of the stream */
	unsigned int channels;
	/* input sample format */
	snd_pcm_format_t in_format;
	/* output sample format */
	snd_pcm_format_t out_format;
	/* lower bound on the selected target delay */
	snd_pcm_uframes_t min_target;
	/* upper bound on the selected target delay */
	snd_pcm_uframes_t max_target;
	/* conversion ratio assuming zero timer drift */
	double nominal_rate_ratio;
	/* how many steps above or below nominal rate ratio for next processing iteration */
	int rate_ratio_step_count;
	/* current best estimate of step count to give steady delay value */
	int steady_rate_ratio_step_count;
	/* delay value that conversion tries to achieve */
	snd_pcm_uframes_t target_delay;
	/* variation in delay tolerated without changing number of steps */
	snd_pcm_uframes_t delay_tolerance;
	/* difference between delay and target delay at last processing iteration */
	snd_pcm_sframes_t delay_diff;
	/* upper bound on absolute delay difference before automatic reset */
	snd_pcm_sframes_t max_delay_diff;
	/* total number of input frames processed */
	uintmax_t input_frames;
	/* total number of input frames at time of last rate ratio update */
	uintmax_t last_input_frames;
	/* minimum number of input frames between rate ration updates */
	snd_pcm_uframes_t period;
	/* timestamp of last resampler reset */
	struct timespec reset_ts;
#if DEBUG
	unsigned int in_rate;
#endif
};

static const struct timespec ts_stabilize = {
	.tv_sec = RESAMPLER_STABILIZE_MS / 1000,
	.tv_nsec = (RESAMPLER_STABILIZE_MS % 1000) * 1000000,
};

static bool timestamp_is_zero(const struct timespec *ts) {
	return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

/**
 * The ALSA audio formats supported as output by the resampler */
static bool resampler_supports_output_format(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16 ||
		format == SND_PCM_FORMAT_S32 ||
		format == SND_PCM_FORMAT_FLOAT;
}

/**
 * The Bluetooth audio formats supported as input by the resampler */
bool resampler_supports_input_format(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16_LE ||
		format == SND_PCM_FORMAT_S32_LE ||
		format == SND_PCM_FORMAT_S24_LE;
}

/**
 * Free an allocated resampler structure. */
void resampler_delete(struct aplay_resampler *resampler) {
	if (resampler == NULL)
		return;
	if (resampler->src_state != NULL)
		src_delete(resampler->src_state);
	free(resampler->in_buffer);
	free(resampler->out_buffer);
	free(resampler);
}

/**
 * Allocate and initialize a resampler
 * @param converter_type The libsamplerate converter to use.
 * @param channels The number of channels in the stream.
 * @param in_format The sample format of the incoming stream.
 * @param in_rate The nominal sample rate of the incoming stream.
 * @param out_format The required output sample format.
 * @param out_rate The nominal sample rate of the output stream.
 * @param min_target The minimum target delay.
 * @param max_target The maximum target delay.
 * @param buffer_size The number of samples in the resampler buffer.
 * @return The allocated resampler structure, or NULL on error. */
struct aplay_resampler *resampler_create(
			enum aplay_converter converter_type,
			unsigned int channels,
			snd_pcm_format_t in_format,
			unsigned int in_rate,
			snd_pcm_format_t out_format,
			unsigned int out_rate,
			snd_pcm_uframes_t min_target,
			snd_pcm_uframes_t max_target,
			size_t buffer_size) {

	/* Check formats can be resampled */
	if (!resampler_supports_input_format(in_format) || !resampler_supports_output_format(out_format)) {
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

	if (in_format != SND_PCM_FORMAT_FLOAT) {
		resampler->in_buffer = calloc(buffer_size, sizeof(float));
		if (resampler->in_buffer == NULL)
			goto fail;
	}

	if (out_format != SND_PCM_FORMAT_FLOAT) {
		resampler->out_buffer = calloc(buffer_size, sizeof(float));
		if (resampler->out_buffer == NULL)
			goto fail;
	}

	resampler->channels = channels;
	resampler->in_format = in_format;
	resampler->out_format = out_format;
	resampler->min_target = min_target;
	resampler->max_target = max_target;
	resampler->buffer_size = buffer_size;
	resampler->max_delay_diff = RESAMPLER_MAX_CHANGE_MS * in_rate / 1000;
	resampler->rate_ratio_step_count = 0;
	resampler->delay_tolerance = RESAMPLER_TOLERANCE_MS * in_rate / 1000;
	resampler->nominal_rate_ratio = (double)out_rate / (double)in_rate;
	resampler->steady_rate_ratio_step_count = 0;
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;
	resampler->input_frames = 0;
	resampler->last_input_frames = 0;
	resampler->period = RESAMPLER_PERIOD_MS * in_rate / 1000;
#if DEBUG
	resampler->in_rate = in_rate;
#endif
	return resampler;

fail:
	resampler_delete(resampler);
	return NULL;
}

/**
 * Resample as many frames as possible from the buffer in to the buffer out. */
int resampler_process(struct aplay_resampler *resampler, ffb_t *in, ffb_t *out) {
	int err = 0;
	SRC_DATA *src_data = &resampler->src_data;
	snd_pcm_uframes_t frames_used = 0;

	/* We must ensure that we only process as many samples as will fit into
	 * the out buffer. */
	size_t out_samples = ffb_len_in(out);
	size_t max_in_samples = out_samples / resampler->src_data.src_ratio;

	size_t in_samples = MIN(ffb_len_out(in), max_in_samples);

	/* Convert input samples to FLOAT format */
	if (resampler->in_format == SND_PCM_FORMAT_S16) {
		in_samples = MIN(in_samples, resampler->buffer_size);
		src_short_to_float_array(in->data, resampler->in_buffer, in_samples);
		src_data->data_in = resampler->in_buffer;
	}
	else if (resampler->in_format == SND_PCM_FORMAT_S32) {
		in_samples = MIN(in_samples, resampler->buffer_size);
		src_int_to_float_array(in->data, resampler->in_buffer, in_samples);
		src_data->data_in = resampler->in_buffer;
	}
	else {
		src_data->data_in = in->data;
	}
	src_data->input_frames = in_samples / resampler->channels;

	if (resampler->out_format == SND_PCM_FORMAT_FLOAT)
		src_data->data_out = out->tail;
	else
		src_data->data_out = resampler->out_buffer;
	src_data->output_frames = out_samples / resampler->channels;

	while (true) {
		if ((err = src_process(resampler->src_state, src_data)) != 0) {
			error("Resampler failed: %s", src_strerror(err));
			return err;
		}
		if (src_data->output_frames_gen == 0)
			break;

		src_data->data_in += src_data->input_frames_used * resampler->channels;
		src_data->input_frames -= src_data->input_frames_used;
		frames_used += src_data->input_frames_used;
		src_data->output_frames -= src_data->output_frames_gen;

		/* Convert output samples to integer format, if necessary */
		if (resampler->out_format == SND_PCM_FORMAT_S16)
			src_float_to_short_array(resampler->out_buffer, out->tail, src_data->output_frames_gen * resampler->channels);
		else if (resampler->out_format == SND_PCM_FORMAT_S32)
			src_float_to_int_array(resampler->out_buffer, out->tail, src_data->output_frames_gen * resampler->channels);

		ffb_seek(out, src_data->output_frames_gen * resampler->channels);
	}
	ffb_shift(in, frames_used * resampler->channels);

	return err;
}

/**
 * Reset the resampling ratio to its nominal rate after any discontinuity in
 * the stream. */
void resampler_reset(struct aplay_resampler *resampler) {
	debug("Resetting resampler");
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;
	resampler->rate_ratio_step_count = 0;
	resampler->steady_rate_ratio_step_count = 0;
	/* disable adaptive resampling until the delay has had time to settle */
	resampler->target_delay = 0;
	gettimestamp(&resampler->reset_ts);
}

/**
 * Change the rate ratio applied by the resampler to adjust for the given new
 * delay value, always trying to move the delay back towards the target value
 * @return true if the rate ratio was changed. */
bool resampler_update_rate_ratio(
			struct aplay_resampler *resampler,
			snd_pcm_uframes_t frames_read,
			snd_pcm_uframes_t delay) {

	/* Update the rate_ratio only if at least one period has passed since the
	 * last update. */
	if (frames_read > 0) {
		resampler->input_frames += frames_read;
		/* Prevent the possibility of integer overflow */
		resampler->input_frames %= INTMAX_MAX;
		if (resampler->input_frames - resampler->last_input_frames < resampler->period)
			return false;
		resampler->last_input_frames = resampler->input_frames;
	}

	snd_pcm_sframes_t delay_diff = delay - resampler->target_delay;

	/* Reset the resampler whenever the delay exceeds the limit. */
	if (labs(delay_diff) > resampler->max_delay_diff) {
		/* Reset the resampler if not already done */
		if (timestamp_is_zero(&resampler->reset_ts)) {
			resampler_reset(resampler);
			return true;
		}
	}

	bool ret = false;

	if (resampler->target_delay == 0) {
		/* Do not restart adaptive resampling until the delay has had time to
		 * settle to a new value */
		struct timespec ts_wait;
		struct timespec ts_now;
		gettimestamp(&ts_now);
		timespecadd(&resampler->reset_ts, &ts_stabilize, &ts_wait);
		if (difftimespec(&ts_now, &ts_wait, &ts_wait) < 0) {
			/* Do not allow the target to be outside the configured range. If
			 * the actual delay is outside that range then try to move it back
			 * as quickly as possible. */
			if (delay > resampler->max_target) {
				resampler->target_delay = resampler->max_target;
				resampler->src_data.src_ratio = resampler->nominal_rate_ratio - RESAMPLER_STEP_SIZE * RESAMPLER_MAX_STEPS;
				resampler->rate_ratio_step_count = RESAMPLER_MAX_STEPS;
				ret = true;
			}
			else if (delay < resampler->min_target) {
				resampler->target_delay = resampler->min_target;
				resampler->src_data.src_ratio = resampler->nominal_rate_ratio + RESAMPLER_STEP_SIZE * RESAMPLER_MAX_STEPS;
				resampler->rate_ratio_step_count = RESAMPLER_MAX_STEPS;
				ret = true;
			}
			else {
				resampler->target_delay = delay;
				resampler->delay_diff = delay - resampler->target_delay;
			}
			resampler->reset_ts.tv_sec = 0;
			resampler->reset_ts.tv_nsec = 0;
#if DEBUG
			debug("Adaptive resampling enabled: target delay = %.1fms",
					1000 * (float)resampler->target_delay / resampler->in_rate);
#endif
		}
		return ret;
	}

	if (labs(delay_diff) > resampler->delay_tolerance) {
		/* When the delay is not already moving back towards tolerance,
		 * step the size of the rate adjustment in the appropriate
		 * direction */
		if (delay_diff > 0 && delay_diff > resampler->delay_diff) {
			if (resampler->rate_ratio_step_count > -RESAMPLER_MAX_STEPS) {
				resampler->src_data.src_ratio -= RESAMPLER_STEP_SIZE;
				resampler->rate_ratio_step_count--;
				ret = true;
			}
		}
		else if (delay_diff < 0 && delay_diff < resampler->delay_diff) {
			if (resampler->rate_ratio_step_count < RESAMPLER_MAX_STEPS) {
				resampler->src_data.src_ratio += RESAMPLER_STEP_SIZE;
				resampler->rate_ratio_step_count++;
				ret = true;
			}
		}
	}
	else if (labs(resampler->delay_diff) > resampler->delay_tolerance) {
		/* When the delay has returned to tolerance, step the size of the steady
		 * rate ratio in the appropriate direction and set the rate ratio to
		 * the revised steady rate ratio. */
		if (resampler->delay_diff > 0) {
			if (resampler->steady_rate_ratio_step_count > -RESAMPLER_MAX_STEPS) {
				resampler->steady_rate_ratio_step_count--;
				ret = true;
			}
		}
		else {
			if (resampler->steady_rate_ratio_step_count > RESAMPLER_MAX_STEPS) {
				resampler->steady_rate_ratio_step_count++;
				ret = true;
			}
		}
		if (ret) {
			resampler->rate_ratio_step_count = resampler->steady_rate_ratio_step_count;
			resampler->src_data.src_ratio = resampler->nominal_rate_ratio + RESAMPLER_STEP_SIZE * resampler->rate_ratio_step_count;
		}
	}

	resampler->delay_diff = delay_diff;

	return ret;
}

double resampler_current_rate_ratio(struct aplay_resampler *resampler) {
	return resampler->src_data.src_ratio;
}

#if __BYTE_ORDER == __LITTLE_ENDIAN

/**
 * Convert a buffer of little-endian samples to the equivalent native-endian
 * format. The samples are modified in place. Also pad 24bit samples (packed
 * into 32bits) to convert them to valid 32-bit samples.
 * On little-endian hosts this function only modifies 24-bit samples.
 * @param len the number of **samples** in the buffer
 * @param format the original format of the samples. */
void resampler_format_le_to_native(void *buffer, size_t len, snd_pcm_format_t format) {
	if (format == SND_PCM_FORMAT_S24_LE) {
		/* Convert to S32 */
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++) {
			if (data[n] & 0x00800000)
				data[n] |= 0xff000000;
			else
				data[n] &= 0x00ffffff;
		}
	}
}

#elif __BYTE_ORDER == __BIG_ENDIAN
#include <byteswap.h>

/**
 * Convert a buffer of little-endian samples to the equivalent native-endian
 * format. The samples are modified in place. Also pad 24bit samples (packed
 * into 32bits) to convert them to valid 32-bit samples.
 * @param len the number of **samples** in the buffer
 * @param format the original format of the samples. */
void resampler_format_le_to_native(void *buffer, size_t len, snd_pcm_format_t format) {
	size_t n;

	switch (format) {
	case SND_PCM_FORMAT_S16_LE:
		uint16_t *data = buffer;
		for (n = 0; n < len; n++)
			bswap_16(data[n]);
		break;
	case SND_PCM_FORMAT_S24_LE:
		uint32_t *data = buffer;
		/* Convert to S32 */
		for (n = 0; n < len; n++) {
			if (data[n] & 0x00008000)
				data[n] |= 0x000000ff;
			else
				data[n] &= 0xffffff00;
			bswap_32(data[n]);
		}
		break;
	case SND_PCM_FORMAT_S32_LE:
		uint32_t *data = buffer;
		for (n = 0; n < len; n++)
			bswap_32(data[n]);
		break;
	default:
		return;
	}
}

/**
 * Return the equivalent supported native-endian format for the given source
 * format. For unsupported formats the given source format value is returned. */
snd_pcm_format_t resampler_native_format(snd_pcm_format_t source_format) {
	switch (source_format) {
	case SND_PCM_FORMAT_S16_LE:
		return SND_PCM_FORMAT_S16;
	case SND_PCM_FORMAT_S24_LE:
		/* 24bit samples must be converted to 32 bit before passing to
		 * the resampler */
		return SND_PCM_FORMAT_S32;
	case SND_PCM_FORMAT_S32_LE:
		return SND_PCM_FORMAT_S32;
	default:
		return source_format;
	}
}

#else
# error "Unknown byte order"
#endif
