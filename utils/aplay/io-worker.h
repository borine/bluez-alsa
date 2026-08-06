/*
 * BlueALSA - io-worker.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_IOWORKER_H_
#define BLUEALSA_APLAY_IOWORKER_H_

#include <stdbool.h>

#include "shared/dbus-client-pcm.h"

bool io_worker_start(const struct ba_pcm *ba_pcm);
void io_worker_stop(const struct ba_pcm *ba_pcm);
void io_worker_cleanup(void);

bool io_worker_mixer_volume_sync_alsa_mixer(struct ba_pcm *ba_pcm);

#endif
