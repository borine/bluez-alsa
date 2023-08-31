/*
 * BlueALSA - bluealsa-pcm-client.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "ba-config.h"
#include "ba-transport-pcm.h"
#include "bluealsa-iface.h"
#include "bluealsa-mix-buffer.h"
#include "bluealsa-pcm-client.h"
#include "bluealsa-pcm-multi.h"
#include "shared/log.h"

/* How long to wait for drain to complete, in nanoseconds */
#define BLUEALSA_PCM_CLIENT_DRAIN_NS 300000000
#define BLUEALSA_CLIENT_BUFFER_PERIODS (BLUEALSA_MULTI_CLIENT_THRESHOLD + 1)

static bool bluealsa_pcm_client_is_playback(struct bluealsa_pcm_client *client) {
	return client->multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
}

static bool bluealsa_pcm_client_is_capture(struct bluealsa_pcm_client *client) {
	return client->multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

static size_t bluealsa_pcm_client_playback_init_offset(const struct bluealsa_pcm_client *client) {
	const struct bluealsa_mix_buffer *buffer = &client->multi->playback_buffer;
	return (BLUEALSA_MULTI_MIX_THRESHOLD * buffer->period) - (client->in_offset * buffer->channels / buffer->frame_size);
}

/**
 * Perform side-effects associated with a state change. */
static void bluealsa_pcm_client_set_state(struct bluealsa_pcm_client *client, enum bluealsa_pcm_client_state new_state) {
	if (new_state == client->state)
		return;

	switch (new_state) {
		case BLUEALSA_PCM_CLIENT_STATE_IDLE:
			client->drain_avail = (size_t) -1;
			/* fallthrough */
		case BLUEALSA_PCM_CLIENT_STATE_FINISHED:
			if (client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING || client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING1)
				client->multi->active_count--;
			break;
		case BLUEALSA_PCM_CLIENT_STATE_PAUSED:
			if (client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING && bluealsa_pcm_client_is_capture(client))
				client->multi->active_count--;
			break;
		case BLUEALSA_PCM_CLIENT_STATE_RUNNING:
			if (bluealsa_pcm_client_is_capture(client)) {
				if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE || client->state == BLUEALSA_PCM_CLIENT_STATE_INIT || client->state == BLUEALSA_PCM_CLIENT_STATE_PAUSED)
					client->multi->active_count++;
			}
			else {
				if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE) {
					client->out_offset = -bluealsa_pcm_client_playback_init_offset(client);
					client->multi->active_count++;
				}
				else if (client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING1)
					return;
			}
			break;
		case BLUEALSA_PCM_CLIENT_STATE_DRAINING1:
			break;
		case BLUEALSA_PCM_CLIENT_STATE_DRAINING2:
			if (client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING1)
				client->multi->active_count--;
			break;
		default:
			/* not reached */
			break;
	}
	client->state = new_state;
}

/**
 * Clean up resources associated with a client PCM connection. */
static void bluealsa_pcm_client_close_pcm(struct bluealsa_pcm_client *client) {
	if (client->pcm_fd != -1) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
		client->watch = false;
		close(client->pcm_fd);
		client->pcm_fd = -1;
	}
}

/**
 * Clean up resources associated with a client control connection. */
static void bluealsa_pcm_client_close_control(
                                          struct bluealsa_pcm_client *client) {
	if (client->control_fd != -1) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->control_fd, NULL);
		close(client->control_fd);
		client->control_fd = -1;
	}
}

/**
 * Start/stop watching for PCM i/o events. */
static void bluealsa_pcm_client_watch_pcm(
                            struct bluealsa_pcm_client *client, bool enabled) {
	if (client->watch == enabled)
		return;

	const uint32_t type = bluealsa_pcm_client_is_playback(client) ? EPOLLIN : EPOLLOUT;
	struct epoll_event event = {
		.events = enabled ? type : 0,
		.data.ptr = &client->pcm_event,
	};
	epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_MOD, client->pcm_fd, &event);
	client->watch = enabled;
}

/**
 * Start/stop watching for drain timer expiry event. */
static void bluealsa_pcm_client_watch_drain(struct bluealsa_pcm_client *client, bool enabled) {
	struct itimerspec timeout = {
		.it_interval = { 0 },
		.it_value = {
			.tv_sec = 0,
			.tv_nsec = enabled ? BLUEALSA_PCM_CLIENT_DRAIN_NS : 0,
		},
	};
	timerfd_settime(client->drain_timer_fd, 0, &timeout, NULL);
}

/**
 * Read bytes from FIFO.
 * @return number of bytes read, or -1 if client closed pipe */
static ssize_t bluealsa_pcm_client_read(struct bluealsa_pcm_client *client) {

	const size_t space = client->buffer_size - client->in_offset;
	if (space == 0)
		return 0;

	uint8_t *buf = client->buffer + client->in_offset;

	ssize_t bytes;
	while ((bytes = read(client->pcm_fd, buf, space)) == -1 && errno == EINTR)
		continue;

	/* pipe closed by remote end */
	if (bytes == 0)
		return -1;

	/* FIFO may be empty but client still open. */
	if (bytes == -1 && errno == EAGAIN)
		bytes = 0;

	if (bytes > 0)
		client->in_offset += bytes;

	return bytes;
}

/**
 * Write samples to the client fifo
 */
void bluealsa_pcm_client_write(struct bluealsa_pcm_client *client, const void *buffer, size_t samples) {
	const int fd = client->pcm_fd;
	const uint8_t *buffer_ = buffer;
	size_t len = samples * BA_TRANSPORT_PCM_FORMAT_BYTES(client->multi->pcm->format);
	ssize_t ret;

	do {

		if ((ret = write(fd, buffer_, len)) == -1)
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				/* If the client is so slow that the FIFO fills up, then it
				 * is inevitable that audio frames will be eventually be
				 * dropped in the bluetooth controller if we block here.
				 * It is better that we discard frames here so that the
				 * decoder is not interrupted. */
				warn("Dropping PCM frames: %s", "PCM overrun");
				ret = len;
				break;
			default:
				/* The client has closed the pipe, or an unrecoverable error
				 * has occurred. */
				bluealsa_pcm_client_close_pcm(client);
				bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
				return;
			}

		buffer_ += ret;
		len -= ret;

	} while (len != 0);

}

/**
 * Deliver samples to transport mix. */
void bluealsa_pcm_client_deliver(struct bluealsa_pcm_client *client) {
	struct bluealsa_pcm_multi *multi = client->multi;

	if (client->state != BLUEALSA_PCM_CLIENT_STATE_RUNNING &&
                           client->state != BLUEALSA_PCM_CLIENT_STATE_DRAINING1)
		return;

	if (client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING1) {
		ssize_t bytes = bluealsa_pcm_client_read(client);
		if (bytes < 0) {
			/* client has closed pcm connection */
			bluealsa_pcm_client_close_pcm(client);
			bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
			return;
		}
		if (client->in_offset == 0 && bytes == 0 && errno == EAGAIN) {
			size_t mix_avail = bluealsa_mix_buffer_calc_avail(&multi->playback_buffer, multi->playback_buffer.mix_offset, client->out_offset);
			if (mix_avail == 0 || mix_avail > client->drain_avail) {
				/* The mix buffer has completely drained all frames from
				 * this client. We now wait some time for the bluetooth system
				 * to play out all sent frames*/
				bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_DRAINING2);
				bluealsa_pcm_client_watch_drain(client, true);
				return;
			}
			else
				client->drain_avail = mix_avail;
		}
	}

	if (client->in_offset > 0) {
		ssize_t delivered = bluealsa_mix_buffer_add(&multi->playback_buffer, &client->out_offset, client->buffer, client->in_offset);
		if (delivered > 0) {
			memmove(client->buffer, client->buffer + delivered, client->in_offset - delivered);
			client->in_offset -= delivered;

			/* If the input buffer was full, we now have room for more. */
			bluealsa_pcm_client_watch_pcm(client, true);
		}
	}
}

/**
 * Action taken when event occurs on client PCM playback connection. */
static void bluealsa_pcm_client_handle_playback_pcm(struct bluealsa_pcm_client *client) {

	ssize_t bytes = bluealsa_pcm_client_read(client);
	if (bytes < 0) {
		/* client has closed pcm connection */
		bluealsa_pcm_client_close_pcm(client);
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	/* If buffer is full, stop reading from FIFO */
	if (bytes == 0)
		bluealsa_pcm_client_watch_pcm(client, false);

	/* Begin adding to mix when sufficient periods are buffered. */
	if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE) {
		if (client->in_offset > BLUEALSA_MULTI_CLIENT_THRESHOLD * client->multi->period_bytes)
			bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_RUNNING);
	}
}

/**
 * Action client Drain request.
 *
 * Starts drain timer. */
static void bluealsa_pcm_client_begin_drain(
                                          struct bluealsa_pcm_client *client) {
	debug("DRAIN: client %zu", client->id);
	if (bluealsa_pcm_client_is_playback(client) && client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING) {
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_DRAINING1);
		bluealsa_pcm_client_watch_pcm(client, false);
	}
	else {
		if (write(client->control_fd, "OK", 2) != 2)
			error("client control response failed");
	}
}

/**
 * Action client Drop request. */
static void bluealsa_pcm_client_drop(struct bluealsa_pcm_client *client) {
	debug("DROP: client %zu", client->id);
	if (bluealsa_pcm_client_is_playback(client)) {
		bluealsa_pcm_client_watch_drain(client, false);
		splice(client->pcm_fd, NULL, config.null_fd, NULL, 1024 * 32, SPLICE_F_NONBLOCK);
		client->in_offset = 0;
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_IDLE);
		client->drop = true;
	}
}

/**
 * Action client Pause request. */
static void bluealsa_pcm_client_pause(struct bluealsa_pcm_client *client) {
	debug("PAUSE: client %zu", client->id);
	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_PAUSED);
	bluealsa_pcm_client_watch_pcm(client, false);
	if (bluealsa_pcm_client_is_playback(client)) {
		struct bluealsa_mix_buffer *buffer = &client->multi->playback_buffer;
		client->out_offset = -bluealsa_mix_buffer_delay(buffer, client->out_offset);
	}
}

/**
 * Action client Resume request. */
static void bluealsa_pcm_client_resume(struct bluealsa_pcm_client *client) {
	debug("RESUME: client %zu", client->id);
	if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE) {
		if (bluealsa_pcm_client_is_playback(client)) {
			bluealsa_pcm_client_watch_pcm(client, true);
			client->drop = false;
		}
		else
			bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_RUNNING);
	}
	if (client->state == BLUEALSA_PCM_CLIENT_STATE_PAUSED) {
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_RUNNING);
		if (bluealsa_pcm_client_is_playback(client))
			bluealsa_pcm_client_watch_pcm(client, true);
	}
}

/**
 * Action taken when drain timer expires. */
static void bluealsa_pcm_client_handle_drain(struct bluealsa_pcm_client *client) {
	debug("DRAIN COMPLETE: client %zu", client->id);
	if (client->state != BLUEALSA_PCM_CLIENT_STATE_DRAINING2)
		return;

	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_IDLE);
	bluealsa_pcm_client_watch_drain(client, false);
	bluealsa_pcm_client_watch_pcm(client, true);
	client->in_offset = 0;
	if (write(client->control_fd, "OK", 2) != 2)
		error("client control response failed");
}

/**
 * Action taken when event occurs on client control connection. */
static void bluealsa_pcm_client_handle_control(struct bluealsa_pcm_client *client) {
	char command[6];
	ssize_t len;
	do {
		len = read(client->control_fd, command, sizeof(command));
	} while (len == -1 && errno == EINTR);

	if (len == -1) {
		if (errno == EAGAIN)
			return;
	}

	if (len <= 0) {
		bluealsa_pcm_client_close_control(client);
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	if (client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING1 ||
			client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING2) {
	/* Should not happen - a well-behaved client will block during drain.
	 * However, not all clients are well behaved. So we invoke the
	 * drain complete handler before processing this request.*/
		bluealsa_pcm_client_handle_drain(client);
	}

	if (strncmp(command, BLUEALSA_PCM_CTRL_DRAIN, len) == 0) {
		bluealsa_pcm_client_begin_drain(client);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_DROP, len) == 0) {
		bluealsa_pcm_client_drop(client);
		len = write(client->control_fd, "OK", 2);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_PAUSE, len) == 0) {
		bluealsa_pcm_client_pause(client);
		len = write(client->control_fd, "OK", 2);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_RESUME, len) == 0) {
		bluealsa_pcm_client_resume(client);
		len = write(client->control_fd, "OK", 2);
	}
	else {
		warn("Invalid PCM control command: %*s", (int)len, command);
		len = write(client->control_fd, "Invalid", 7);
	}
}

/**
 * Marshall client events.
 * Invokes appropriate action. */
void bluealsa_pcm_client_handle_event(struct bluealsa_pcm_client_event *event) {
	struct bluealsa_pcm_client *client = event->client;
	switch(event->type) {
		case BLUEALSA_EVENT_TYPE_PCM:
			if (bluealsa_pcm_client_is_playback(client))
				bluealsa_pcm_client_handle_playback_pcm(client);
			break;
		case BLUEALSA_EVENT_TYPE_CONTROL:
			bluealsa_pcm_client_handle_control(client);
			break;
		case BLUEALSA_EVENT_TYPE_DRAIN:
			bluealsa_pcm_client_handle_drain(client);
			break;
	}
}

void bluealsa_pcm_client_handle_close_event(
                                     struct bluealsa_pcm_client_event *event) {
	struct bluealsa_pcm_client *client = event->client;
	switch (event->type) {
		case BLUEALSA_EVENT_TYPE_PCM:
			bluealsa_pcm_client_close_pcm(client);
			break;
		case BLUEALSA_EVENT_TYPE_CONTROL:
			bluealsa_pcm_client_close_control(client);
			break;
		default:
			g_assert_not_reached();
	}
	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
}

/**
 * Allocate a buffer suitable for transport transfer size, and set initial
 * state. */
bool bluealsa_pcm_client_init(struct bluealsa_pcm_client *client) {
	struct bluealsa_pcm_multi *multi = client->multi;

	if (bluealsa_pcm_client_is_playback(client)) {
		client->buffer_size = BLUEALSA_CLIENT_BUFFER_PERIODS * multi->period_bytes;

		client->buffer = calloc(client->buffer_size, sizeof(uint8_t));
		if (client->buffer == NULL) {
			error("Unable to allocate client buffer: %s", strerror(errno));
			return false;
		}

		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_IDLE);
		bluealsa_pcm_client_watch_pcm(client, true);
	}
	else {
		/* Capture clients are active immediately. */
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_RUNNING);
	}

	return true;
}

/**
 * Allocate a new client instance. */
struct bluealsa_pcm_client *bluealsa_pcm_client_new(struct bluealsa_pcm_multi *multi, int pcm_fd, int control_fd) {
	struct bluealsa_pcm_client *client = calloc(1, sizeof(struct bluealsa_pcm_client));
	if (!client) {
		error("Unable to create new client: %s", strerror(errno));
		return NULL;
	}

	client->multi = multi;
	client->pcm_fd = pcm_fd;
	client->control_fd = control_fd;
	client->drain_timer_fd = -1;
	client->pcm_event.type = BLUEALSA_EVENT_TYPE_PCM;
	client->pcm_event.client = client;

	client->control_event.type = BLUEALSA_EVENT_TYPE_CONTROL;
	client->control_event.client = client;

	struct epoll_event ep_event = {
		.data.ptr = &client->pcm_event,
	 };

	if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->pcm_fd, &ep_event) == -1) {
		error("Unable to init client, epoll_ctl: %s\n", strerror(errno));
		bluealsa_pcm_client_free(client);
		return NULL;
	}

	ep_event.data.ptr = &client->control_event;
	ep_event.events = EPOLLIN;
	if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->control_fd, &ep_event) == -1) {
		error("Unable to init client, epoll_ctl: %s\n", strerror(errno));
		epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
		bluealsa_pcm_client_free(client);
		return NULL;
	}

	if (bluealsa_pcm_client_is_playback(client)) {
		client->drain_timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
		client->drain_event.type = BLUEALSA_EVENT_TYPE_DRAIN;
		client->drain_event.client = client;

		ep_event.data.ptr = &client->drain_event;
		ep_event.events = EPOLLIN;
		if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->drain_timer_fd, &ep_event) == -1) {
			error("Unable to init client, epoll_ctl: %s", strerror(errno));
			epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
			epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->control_fd, NULL);
			bluealsa_pcm_client_free(client);
			return NULL;
		}

	}

	client->watch = false;
	client->state = BLUEALSA_PCM_CLIENT_STATE_INIT;

	return client;
}

/**
 * Free the resources used by a client. */
void bluealsa_pcm_client_free(struct bluealsa_pcm_client *client) {
	if (bluealsa_pcm_client_is_playback(client)) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->drain_timer_fd, NULL);
		if (client->drain_timer_fd >= 0)
			close(client->drain_timer_fd);
		free(client->buffer);
	}
	bluealsa_pcm_client_close_pcm(client);
	bluealsa_pcm_client_close_control(client);
	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
	free(client);
}
