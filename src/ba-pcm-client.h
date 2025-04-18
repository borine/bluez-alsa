/*
 * BlueALSA - ba-pcm-client.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BAPCMCLIENT_H_
#define BLUEALSA_BAPCMCLIENT_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include "ba-pcm-multi.h"
#include "config.h"

enum ba_pcm_client_state {
	/* client is registered, but not yet initialized */
	BA_PCM_CLIENT_STATE_INIT = 0,
	/* client is initialized, but not active */
	BA_PCM_CLIENT_STATE_IDLE,
	/* client is transferring audio frames */
	BA_PCM_CLIENT_STATE_RUNNING,
	/* client has sent PAUSE command, waiting for RESUME */
	BA_PCM_CLIENT_STATE_PAUSED,
	/* client has sent DRAIN command, processing frames remaining in the pipe */
	BA_PCM_CLIENT_STATE_DRAINING1,
	/* pipe is drained, waiting on timeout before returning to IDLE */
	BA_PCM_CLIENT_STATE_DRAINING2,
	/* client has closed pipe and/or control socket */
	BA_PCM_CLIENT_STATE_FINISHED,
};

enum ba_pcm_client_event_type {
	BA_EVENT_TYPE_PCM,
	BA_EVENT_TYPE_CONTROL,
	BA_EVENT_TYPE_DRAIN,
};

struct ba_pcm_client_event {
	enum ba_pcm_client_event_type type;
	struct ba_pcm_client *client;
};

struct ba_pcm_client {
	struct ba_pcm_multi *multi;
	int pcm_fd;
	int control_fd;
	int drain_timer_fd;
	struct ba_pcm_client_event pcm_event;
	struct ba_pcm_client_event control_event;
	struct ba_pcm_client_event drain_event;
	enum ba_pcm_client_state state;
	uint8_t *buffer;
	size_t buffer_size;
	size_t in_offset;
	intmax_t out_offset;
	size_t drain_avail;
	bool drop;
	bool watch;
#if DEBUG
	size_t id;
#endif
};

struct ba_pcm_client *ba_pcm_client_new(
					struct ba_pcm_multi *multi,
					int pcm_fd, int control_fd);

bool ba_pcm_client_init(struct ba_pcm_client *client);

void ba_pcm_client_free(struct ba_pcm_client *client);

void ba_pcm_client_handle_event(struct ba_pcm_client_event *event);
void ba_pcm_client_handle_close_event(struct ba_pcm_client_event *event);
void ba_pcm_client_deliver(struct ba_pcm_client *client);
void ba_pcm_client_fetch(struct ba_pcm_client *client);
void ba_pcm_client_write(struct ba_pcm_client *client, const void *buffer, size_t samples);
void ba_pcm_client_drain(struct ba_pcm_client *client);
void ba_pcm_client_underrun(struct ba_pcm_client *client);

#endif
