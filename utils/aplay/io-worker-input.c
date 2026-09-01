/*
 * BlueALSA - io-worker-input.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <dbus/dbus.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>

#include "aplay-config.h"
#include "io-worker-input.h"
#include "shared/dbus-client-pcm.h"
#include "shared/ffb.h"
#include "shared/log.h"

static snd_pcm_format_t bluealsa_get_snd_pcm_format(const struct ba_pcm *pcm) {
	switch (pcm->format) {
	case 0x0108:
		return SND_PCM_FORMAT_U8;
	case 0x8210:
		return SND_PCM_FORMAT_S16_LE;
	case 0x8318:
		return SND_PCM_FORMAT_S24_3LE;
	case 0x8418:
		return SND_PCM_FORMAT_S24_LE;
	case 0x8420:
		return SND_PCM_FORMAT_S32_LE;
	default:
		error("Unknown PCM format: %#x", pcm->format);
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

#if WITH_LIBSAMPLERATE
/**
 * Convert a buffer of PCM samples to the equivalent native-endian format. The
 * samples are modified in place. Also pad 24bit samples (packed into 32bits)
 * to convert them to valid 32-bit samples.
 *
 * @param buffer The buffer of samples to convert.
 * @param len The number of samples in the buffer.
 * @param format The original format of the samples. */
static void convert_to_native_endian_format(
		void *restrict buffer, size_t len, snd_pcm_format_t format) {
# if __BYTE_ORDER == __BIG_ENDIAN

	switch (format) {
	case SND_PCM_FORMAT_S16_LE: {
		uint16_t *data = buffer;
		for (size_t n = 0; n < len; n++)
			le16toh(data[n]);
	} break;
	case SND_PCM_FORMAT_S24_LE: {
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++) {
			le32toh(data[n]);
			/* convert to S32 */
			data[n] <<= 8;
		}
	} break;
	case SND_PCM_FORMAT_S32_LE: {
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++)
			le32toh(data[n]);
	} break;
	default:
		return;
	}

# else

	switch (format) {
	case SND_PCM_FORMAT_S24_LE: {
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++) {
			/* convert to S32 */
			data[n] <<= 8;
		}
	} break;
	default:
		return;
	}

# endif
}
#endif

bool io_worker_input_init(io_worker_input_t *input,	const struct ba_pcm *ba_pcm) {

	memset(input, 0, sizeof(*input));
	input->ba_pcm = ba_pcm;
	input->ba_pcm_fd = -1;
	input->ba_pcm_ctrl_fd = -1;
	input->pcm_format = bluealsa_get_snd_pcm_format(ba_pcm);
	input->pcm_format_size = snd_pcm_format_size(input->pcm_format, 1);
	input->pcm_1s_samples = ba_pcm->rate * ba_pcm->channels;

	/* Create a buffer big enough to hold enough PCM data for three periods.
	 * This will be later be revised if necessary to match the actual ALSA
	 * start threshold when the ALSA PCM is opened. */
	const size_t nmemb = ((size_t)config.pcm_period_time * 3 / 1000) * (input->pcm_1s_samples / 1000);
	if (ffb_init(&input->buffer, nmemb, input->pcm_format_size) == -1) {
		error("Couldn't create PCM buffer: %s", strerror(errno));
		return false;
	}

	debug("Opening BlueALSA source PCM: %s", ba_pcm->pcm_path);
	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_open(&config.dbus_ctx, ba_pcm->pcm_path,
				&input->ba_pcm_fd, &input->ba_pcm_ctrl_fd, &err)) {
		error("Couldn't open BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		return false;
	}

	return true;
}

void io_worker_input_close(io_worker_input_t *input) {
	ffb_free(&input->buffer);
	if (input->ba_pcm_fd != -1) {
		close(input->ba_pcm_fd);
		input->ba_pcm_fd = -1;
	}
	if (input->ba_pcm_ctrl_fd != -1) {
		close(input->ba_pcm_ctrl_fd);
		input->ba_pcm_ctrl_fd = -1;
	}
}

ssize_t io_worker_input_read(io_worker_input_t *input, bool idling, bool native_endian) {
#if !WITH_LIBSAMPLERATE
	(void) native_endian;
#endif

	/* If the read buffer is full then we have an overrun. We must
	 * discard audio frames in order to continue reading fresh data
	 * from the server. */
	if (ffb_blen_in(&input->buffer) == 0) {
		unsigned int buffered = 0;
		ioctl(input->ba_pcm_fd, FIONREAD, &buffered);
		const size_t discard_bytes = MIN(buffered, ffb_blen_out(&input->buffer));
		const size_t discard_samples = discard_bytes / input->pcm_format_size;
		ffb_shift(&input->buffer, discard_samples);
		if (!idling)
			warn("Dropping PCM frames: %zu", discard_samples / input->ba_pcm->channels);
	}

	ssize_t ret;
	while ((ret = read(input->ba_pcm_fd, input->buffer.tail, ffb_blen_in(&input->buffer))) == -1) {
		if (errno != EINTR) {
			error("BlueALSA source PCM read error: %s", strerror(errno));
			return ret;
		}
	}

	ssize_t read_samples = ret / input->pcm_format_size;
	if (ret % input->pcm_format_size != 0)
		warn("Invalid read from BlueALSA source PCM: %zd %% %zd != 0", ret, input->pcm_format_size);

#if WITH_LIBSAMPLERATE
	if (native_endian)
		convert_to_native_endian_format(&input->buffer.tail, read_samples, input->pcm_format);
#endif

	ffb_seek(&input->buffer, read_samples);

	return read_samples;
}
