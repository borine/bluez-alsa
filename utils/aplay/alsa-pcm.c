/*
 * BlueALSA - alsa-pcm.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "alsa-pcm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/log.h"

static int alsa_pcm_set_hw_params(snd_pcm_t *pcm, snd_pcm_format_t format, int channels,
		int rate, unsigned int *buffer_time, unsigned int *period_time, char **msg) {

	const snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
	snd_pcm_hw_params_t *params;
	char buf[256];
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(pcm, params)) < 0) {
		snprintf(buf, sizeof(buf), "Set all possible ranges: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_access(pcm, params, access)) != 0) {
		snprintf(buf, sizeof(buf), "Set assess type: %s: %s", snd_strerror(err), snd_pcm_access_name(access));
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_format(pcm, params, format)) != 0) {
		snprintf(buf, sizeof(buf), "Set format: %s: %s", snd_strerror(err), snd_pcm_format_name(format));
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0) {
		snprintf(buf, sizeof(buf), "Set channels: %s: %d", snd_strerror(err), channels);
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Set sample rate: %s: %d", snd_strerror(err), rate);
		goto fail;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, period_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set period time: %s: %u", snd_strerror(err), *period_time);
		goto fail;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, buffer_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set buffer time: %s: %u", snd_strerror(err), *buffer_time);
		goto fail;
	}

	if ((err = snd_pcm_hw_params(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static int alsa_pcm_set_sw_params(struct alsa_pcm *pcm, snd_pcm_uframes_t buffer_size,
		snd_pcm_uframes_t period_size, char **msg) {

	snd_pcm_t *snd_pcm = pcm->handle;
	snd_pcm_sw_params_t *params;
	char buf[256];
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Get current params: %s", snd_strerror(err));
		goto fail;
	}

	/* Start the transfer when three periods have been written (or when the
	 * buffer is full if it holds less than three periods. */
	snd_pcm_uframes_t threshold = period_size * 3;
	if (threshold > buffer_size)
		threshold = buffer_size;
	if ((err = snd_pcm_sw_params_set_start_threshold(snd_pcm, params, threshold)) != 0) {
		snprintf(buf, sizeof(buf), "Set start threshold: %s: %lu", snd_strerror(err), threshold);
		goto fail;
	}

	if ((err = snd_pcm_sw_params(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	pcm->start_threshold = threshold;

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

void alsa_pcm_init(struct alsa_pcm *pcm) {
	pcm->handle = NULL;
}

int alsa_pcm_open(
		struct alsa_pcm *pcm,
		const char *name,
		snd_pcm_format_t format,
		int channels,
		unsigned int rate,
		unsigned int buffer_time,
		unsigned int period_time,
		int flags,
		char **msg) {

	char *tmp = NULL;
	char buf[256];
	int err;

	assert (pcm->handle == NULL);

	if ((err = snd_pcm_open(&pcm->handle, name, SND_PCM_STREAM_PLAYBACK, flags)) != 0) {
		snprintf(buf, sizeof(buf), "Open PCM: %s", snd_strerror(err));
		goto fail;
	}

	unsigned int actual_buffer_time = buffer_time;
	unsigned int actual_period_time = period_time;
	if ((err = alsa_pcm_set_hw_params(pcm->handle, format, channels, rate,
				&actual_buffer_time, &actual_period_time, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set HW params: %s", tmp);
		goto fail;
	}

	snd_pcm_uframes_t buffer_size, period_size;
	if ((err = snd_pcm_get_params(pcm->handle, &buffer_size, &period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Get params: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = alsa_pcm_set_sw_params(pcm, buffer_size, period_size, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set SW params: %s", tmp);
		goto fail;
	}

	if ((err = snd_pcm_prepare(pcm->handle)) != 0) {
		snprintf(buf, sizeof(buf), "Prepare: %s", snd_strerror(err));
		goto fail;
	}

	pcm->format = format;
	pcm->channels = channels;
	pcm->sample_size = snd_pcm_format_size(format, 1);
	pcm->frame_size = snd_pcm_format_size(format, channels);
	pcm->rate = rate;
	pcm->buffer_time = actual_buffer_time;
	pcm->period_time = actual_period_time;
	pcm->buffer_frames = buffer_size;
	pcm->period_frames = period_size;
	pcm->delay = 0;

	/* Maintain buffer fill level above 1 period plus 2ms to allow for
	 * scheduling delays */
	pcm->underrun_threshold = pcm->period_frames + pcm->rate * 2 / 1000;

	return 0;

fail:
	if (pcm->handle != NULL)
		snd_pcm_close(pcm->handle);
	pcm->handle = NULL;
	if (msg != NULL)
		*msg = strdup(buf);
	if (tmp != NULL)
		free(tmp);
	return err;

}

int alsa_pcm_write(struct alsa_pcm *pcm, ffb_t *buffer, bool drain, bool verbose) {
	snd_pcm_sframes_t avail = 0;
	snd_pcm_sframes_t delay = 0;
	snd_pcm_sframes_t ret;

	if ((ret = snd_pcm_avail_delay(pcm->handle, &avail, &delay)) < 0) {
		if (ret == -EPIPE) {
			debug("ALSA playback PCM underrun");
			snd_pcm_prepare(pcm->handle);
			avail = pcm->buffer_frames;
			delay = 0;
		}
		else {
			error("ALSA playback PCM error: %s", snd_strerror(ret));
			return -1;
		}
	}

	snd_pcm_sframes_t frames = ffb_len_out(buffer) / pcm->channels;
	snd_pcm_sframes_t written_frames = 0;

	/* If not draining, write only as many frames as possible without blocking.
	 * If necessary insert silemce frames to prevent underrun. */
	if (!drain) {
		if (frames > avail)
			frames = avail;
		else if (pcm->buffer_frames - avail + frames < pcm->underrun_threshold &&
					snd_pcm_state(pcm->handle) == SND_PCM_STATE_RUNNING) {
			/* Pad the buffer with enough silence to restore it to the start
			 * threshold. */
#if DEBUG
			if (verbose) {
				debug("Underrun imminent: inserting silence frames");
			}
#endif
			size_t padding = (pcm->start_threshold - frames) * pcm->channels;
			snd_pcm_format_set_silence(pcm->format, buffer->tail, padding);
			ffb_seek(buffer, padding);
			frames += padding;
		}
	}

	while (frames > 0) {
		ret = snd_pcm_writei(pcm->handle, buffer->data, frames);
		if (ret < 0)
			switch (-ret) {
			case EINTR:
				continue;
			case EPIPE:
				debug("ALSA playback PCM underrun");
				snd_pcm_prepare(pcm->handle);
				continue;
			default:
				error("ALSA playback PCM write error: %s", snd_strerror(ret));
				return -1;
			}
		else {
			written_frames += ret;
			frames -= ret;
			delay += ret;
		}
	}

	if (drain) {
		snd_pcm_drain(pcm->handle);
		ffb_rewind(buffer);
		return 0;
	}

	pcm->delay = delay + written_frames;

	/* Move leftovers to the beginning and reposition tail. */
	if (written_frames > 0)
		ffb_shift(buffer, written_frames * pcm->channels);

	return 0;
}

void alsa_pcm_dump(const struct alsa_pcm *pcm, FILE *fp) {
	snd_output_t *out;
	snd_output_stdio_attach(&out, fp, 0);
	snd_pcm_dump(pcm->handle, out);
	snd_output_close(out);
}

void alsa_pcm_close(struct alsa_pcm *pcm) {
	if (pcm->handle != NULL) {
		snd_pcm_close(pcm->handle);
		pcm->handle = NULL;
	}
}
