/*
 * BlueALSA - io-worker-input.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_IOWORKER_INPUT_H_
#define BLUEALSA_APLAY_IOWORKER_INPUT_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>

#include "shared/dbus-client-pcm.h"
#include "shared/ffb.h"

typedef struct {
	const struct ba_pcm *ba_pcm;
	int ba_pcm_fd;
	int ba_pcm_ctrl_fd;
	ffb_t buffer;
	size_t pcm_1s_samples;
	snd_pcm_format_t pcm_format;
	ssize_t pcm_format_size;
 } io_worker_input_t;

bool io_worker_input_init(io_worker_input_t *input,	const struct ba_pcm *ba_pcm);

void io_worker_input_close(io_worker_input_t *input);

ssize_t io_worker_input_read(io_worker_input_t *input, bool idling, bool native_endian);

#endif
