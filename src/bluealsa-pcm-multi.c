/*
 * BlueALSA - bluealsa-pcm-multi.c
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
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "ba-config.h"
#include "bluealsa-pcm-multi.h"
#include "bluealsa-pcm-client.h"
#include "ba-transport-pcm.h"
#include "ba-transport.h"
#include "shared/log.h"
#include "shared/defs.h"


/* Limit number of clients to ensure sufficient resources are available. */
#define BLUEALSA_MULTI_MAX_CLIENTS 32

/* Size of epoll event array. Allow for client control, pcm, and drain timer,
 * plus the mix event fd. */
#define BLUEALSA_MULTI_MAX_EVENTS (1 + BLUEALSA_MULTI_MAX_CLIENTS * 3)

/* Determines the size of the mix buffer. */
#define BLUEALSA_MULTI_BUFFER_PERIODS 16

static void *bluealsa_pcm_mix_thread_func(struct bluealsa_pcm_multi *multi);
static void *bluealsa_pcm_snoop_thread_func(struct bluealsa_pcm_multi *multi);
static void bluealsa_pcm_multi_remove_client(struct bluealsa_pcm_multi *multi, struct bluealsa_pcm_client *client);


static bool bluealsa_pcm_multi_is_capture(const struct bluealsa_pcm_multi *multi) {
	return multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

static bool bluealsa_pcm_multi_is_playback(const struct bluealsa_pcm_multi *multi) {
	return multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
}

static bool bluealsa_pcm_multi_is_target(const struct bluealsa_pcm_multi *multi) {
	return multi->pcm->t->profile & (BA_TRANSPORT_PROFILE_A2DP_SINK | BA_TRANSPORT_PROFILE_MASK_HF);
}

static void bluealsa_pcm_multi_cleanup(struct bluealsa_pcm_multi *multi) {
	if (multi->thread != config.main_thread) {
		eventfd_write(multi->event_fd, 0xDEAD0000);
		pthread_join(multi->thread, NULL);
		multi->thread = config.main_thread;
	}
	if (bluealsa_pcm_multi_is_playback(multi) && multi->playback_buffer.size > 0)
		bluealsa_mix_buffer_release(&multi->playback_buffer);

	pthread_mutex_lock(&multi->client_mutex);
	while (multi->client_count > 0)
		bluealsa_pcm_multi_remove_client(multi, g_list_first(multi->clients)->data);
	multi->client_count = 0;
	pthread_mutex_unlock(&multi->client_mutex);
}

/**
 * Is multi-client support implemented and configured for the given transport ? */
bool bluealsa_pcm_multi_enabled(const struct ba_transport *t) {
	if (!config.multi_enabled)
		return false;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return t->a2dp.pcm.format != BA_TRANSPORT_PCM_FORMAT_S24_3LE;

	return true;
}

/**
 * Create multi-client support for the given transport pcm. */
struct bluealsa_pcm_multi *bluealsa_pcm_multi_create(struct ba_transport_pcm *pcm) {

	struct bluealsa_pcm_multi *multi = calloc(1, sizeof(struct bluealsa_pcm_multi));
	if (multi == NULL)
		return multi;

	multi->pcm = pcm;
	multi->thread = config.main_thread;

	pthread_mutex_init(&multi->client_mutex, NULL);
	pthread_mutex_init(&multi->buffer_mutex, NULL);
	pthread_cond_init(&multi->cond, NULL);

	if ((multi->epoll_fd = epoll_create(1)) == -1)
		goto fail;

	if ((multi->event_fd = eventfd(0, 0)) == -1)
		goto fail;

	pcm->multi = multi;

	return multi;

fail:
	if (multi->epoll_fd != -1)
		close(multi->epoll_fd);
	if (multi->event_fd != -1)
		close(multi->event_fd);
	free(multi);
	return NULL;
}

static void bluealsa_pcm_multi_init_clients(struct bluealsa_pcm_multi *multi) {
	pthread_mutex_lock(&multi->client_mutex);
	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;
		if (client->buffer == NULL) {
			if (!bluealsa_pcm_client_init(client))
				bluealsa_pcm_multi_remove_client(client->multi, client);
		}
	}
	pthread_mutex_unlock(&multi->client_mutex);
}

/**
 * Start the multi client thread. */
static bool bluealsa_pcm_multi_start(struct bluealsa_pcm_multi *multi) {

	if (bluealsa_pcm_multi_is_playback(multi)) {
		if (pthread_create(&multi->thread, NULL, PTHREAD_FUNC(bluealsa_pcm_mix_thread_func), multi) == -1) {
			error("Cannot create pcm multi mix thread: %s", strerror(errno));
			bluealsa_mix_buffer_release(&multi->playback_buffer);
			multi->thread = config.main_thread;
			return false;
		}
		pthread_setname_np(multi->thread, "ba-pcm-mix");
	}
	else {
		if (pthread_create(&multi->thread, NULL, PTHREAD_FUNC(bluealsa_pcm_snoop_thread_func), multi) == -1) {
			error("Cannot create pcm multi snoop thread: %s", strerror(errno));
			multi->thread = config.main_thread;
			return false;
		}
		pthread_setname_np(multi->thread, "ba-pcm-snoop");
	}

	return true;
}

/**
 * Initialize multi-client support.
 *
 * Set up the buffer parameters and enable client audio I/O.
 *
 * @param multi The multi-client instance to be initialized.
 * @param transfer_samples The largest number of samples that will be passed
 *                         between the transport I/O thread and the client
 *                         thread in a single transfer.
 * @return true if multi-client successfully initialized. */
bool bluealsa_pcm_multi_init(struct bluealsa_pcm_multi *multi, size_t transfer_samples) {

	debug("Initializing multi client support");

	multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
	multi->period_frames = transfer_samples / multi->pcm->channels;
	multi->period_bytes = multi->period_frames * multi->pcm->channels * BA_TRANSPORT_PCM_FORMAT_BYTES(multi->pcm->format);

	if (bluealsa_pcm_multi_is_playback(multi)) {
		size_t buffer_frames = BLUEALSA_MULTI_BUFFER_PERIODS * multi->period_frames;
		if (bluealsa_mix_buffer_init(&multi->playback_buffer,
				multi->pcm->format, multi->pcm->channels,
				buffer_frames, multi->period_frames) == -1)
			return false;
		multi->buffer_ready = false;
		multi->delay = multi->period_frames * (BLUEALSA_MULTI_MIX_THRESHOLD + BLUEALSA_MULTI_CLIENT_THRESHOLD) * 10000 / multi->pcm->rate;
		multi->active_count = 0;
	}

	multi->drain = false;
	multi->drop = false;
	bluealsa_pcm_multi_init_clients(multi);

	if (bluealsa_pcm_multi_is_capture(multi) && multi->client_count > 0) {
		if (multi->thread == config.main_thread && !bluealsa_pcm_multi_start(multi))
			return false;
	}
	return true;
}

/**
 * Stop the multi-client support. */
void bluealsa_pcm_multi_reset(struct bluealsa_pcm_multi *multi) {
	if (!bluealsa_pcm_multi_is_target(multi))
		bluealsa_pcm_multi_cleanup(multi);
	multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
}

/**
 * Release the resources used by a multi. */
void bluealsa_pcm_multi_free(struct bluealsa_pcm_multi *multi) {
	bluealsa_pcm_multi_cleanup(multi);
	g_list_free(multi->clients);

	close(multi->epoll_fd);
	close(multi->event_fd);

	pthread_mutex_destroy(&multi->client_mutex);
	pthread_mutex_destroy(&multi->buffer_mutex);
	pthread_cond_destroy(&multi->cond);

	free(multi);
}

/**
 * Include a new client stream.
 *
 * Starts the multi thread if not already running.
 *
 * @param multi The multi to which the client is to be added.
 * @param pcm_fd File descriptor for client audio i/o.
 * @param control_fd File descriptor for client control commands.
 * @return true if successful.
 */
bool bluealsa_pcm_multi_add_client(struct bluealsa_pcm_multi *multi, int pcm_fd, int control_fd) {
	int rv;

	if (multi->client_count == BLUEALSA_MULTI_MAX_CLIENTS)
		return false;

	if (bluealsa_pcm_multi_is_capture(multi) && multi->state == BLUEALSA_PCM_MULTI_STATE_FINISHED) {
		/* client thread has failed - clean it up before starting new one. */
		bluealsa_pcm_multi_reset(multi);
	}

	pthread_mutex_lock(&multi->pcm->mutex);
	if (multi->pcm->fd == -1)
		rv = multi->pcm->fd = eventfd(0, EFD_NONBLOCK);
	pthread_mutex_unlock(&multi->pcm->mutex);
	if (rv == -1)
		return false;

	struct bluealsa_pcm_client *client = bluealsa_pcm_client_new(multi, pcm_fd, control_fd);
	if (!client)
		goto fail;


	pthread_mutex_lock(&multi->client_mutex);

	/* Postpone initialization of client if multi itself is not yet
	 * initialized. */
	if (multi->period_bytes > 0) {
		if (!bluealsa_pcm_client_init(client)) {
			bluealsa_pcm_client_free(client);
			pthread_mutex_unlock(&multi->client_mutex);
			goto fail;
		}
	}

#if DEBUG
	client->id = ++multi->client_no;
#endif

	multi->clients = g_list_prepend(multi->clients, client);
	multi->client_count++;

	if (bluealsa_pcm_multi_is_playback(multi)) {
		if (multi->state == BLUEALSA_PCM_MULTI_STATE_FINISHED)
			multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
	}
	else {
		if (multi->state == BLUEALSA_PCM_MULTI_STATE_INIT)
			multi->state = BLUEALSA_PCM_MULTI_STATE_RUNNING;
	}

	pthread_mutex_unlock(&multi->client_mutex);

	if (multi->thread == config.main_thread && !bluealsa_pcm_multi_start(multi))
		goto fail;

	debug("new client id %zu, total clients now %zu", client->id, multi->client_count);
	return true;

fail:
	pthread_mutex_lock(&multi->pcm->mutex);
	if (multi->pcm->fd != -1) {
		close(multi->pcm->fd);
		multi->pcm->fd = -1;
	}
	pthread_mutex_unlock(&multi->pcm->mutex);
	return false;
}

/* Remove a client stream.
 * @return false if no clients remain, true otherwise. */
static void bluealsa_pcm_multi_remove_client(struct bluealsa_pcm_multi *multi, struct bluealsa_pcm_client *client) {
	client->multi->clients = g_list_remove(multi->clients, client);
	--client->multi->client_count;
	debug("removed client no %zu, total clients now %zu", client->id, multi->client_count);
	bluealsa_pcm_client_free(client);
}

/**
 * Write out decoded samples to the clients.
 *
 * Called by the transport I/O thread.
 * @param multi Pointer to the multi.
 * @param buffer Pointer to the buffer from which to obtain the samples.
 * @param samples the number of samples available in the decoder buffer.
 * @return the number of samples written. */
ssize_t bluealsa_pcm_multi_write(struct bluealsa_pcm_multi *multi, const void *buffer, size_t samples) {

	pthread_mutex_lock(&multi->client_mutex);

	if (multi->state == BLUEALSA_PCM_MULTI_STATE_FINISHED) {
		pthread_mutex_lock(&multi->pcm->mutex);
		ba_transport_pcm_release(multi->pcm);
		pthread_mutex_unlock(&multi->pcm->mutex);
		samples = 0;
		goto finish;
	}

	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;
		if (client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING)
			bluealsa_pcm_client_write(client, buffer, samples);

		if (client->state == BLUEALSA_PCM_CLIENT_STATE_FINISHED) {
			bluealsa_pcm_multi_remove_client(multi, client);
		}
	}

finish:
	pthread_mutex_unlock(&multi->client_mutex);
	return (ssize_t) samples;
}

/**
 * Read mixed samples.
 *
 * multi client replacement for io_pcm_read() */
ssize_t bluealsa_pcm_multi_read(struct bluealsa_pcm_multi *multi, void *buffer, size_t samples) {
	eventfd_t value = 0;
	ssize_t ret;
	enum bluealsa_pcm_multi_state state;

	pthread_mutex_lock(&multi->pcm->mutex);
	if (multi->pcm->fd == -1) {
		pthread_mutex_unlock(&multi->pcm->mutex);
		errno = EBADF;
		return -1;
	}

	/* Clear pcm available event */
	ret = eventfd_read(multi->pcm->fd, &value);
	pthread_mutex_unlock(&multi->pcm->mutex);
	if (ret < 0 && errno != EAGAIN)
		return ret;

	/* Trigger client thread to re-fill the mix. */
	eventfd_write(multi->event_fd, 1);

	/* Wait for mix update to complete */
	pthread_mutex_lock(&multi->buffer_mutex);
	while (((state = multi->state) == BLUEALSA_PCM_MULTI_STATE_RUNNING) && !multi->buffer_ready)
		pthread_cond_wait(&multi->cond, &multi->buffer_mutex);
	multi->buffer_ready = false;
	pthread_mutex_unlock(&multi->buffer_mutex);

	switch (state) {
	case BLUEALSA_PCM_MULTI_STATE_RUNNING:
	{
		double scale_array[multi->pcm->channels];
		if (multi->pcm->soft_volume) {
			for (unsigned i = 0; i < multi->pcm->channels; ++i)
				scale_array[i] = multi->pcm->volume[i].scale;
		}
		else {
			for (unsigned i = 0; i < multi->pcm->channels; ++i)
				if (multi->pcm->volume[i].scale == 0)
					scale_array[i] = 0;
		}
		ret = bluealsa_mix_buffer_read(&multi->playback_buffer, buffer, samples, scale_array);
		if (ret == 0) {
			errno = EAGAIN;
			ret = -1;
		}
		break;
	}
	case BLUEALSA_PCM_MULTI_STATE_FINISHED:
		pthread_mutex_lock(&multi->pcm->mutex);
		ba_transport_pcm_release(multi->pcm);
		pthread_mutex_unlock(&multi->pcm->mutex);
		ret = 0;
		break;
	case BLUEALSA_PCM_MULTI_STATE_INIT:
		errno = EAGAIN;
		ret = -1;
		break;
	default:
		errno = EIO;
		ret = -1;
		break;
	}

	return ret;
}

/**
 * Signal the transport i/o thread that mixed samples are available. */
static void bluealsa_pcm_multi_wake_transport(struct bluealsa_pcm_multi *multi) {
	pthread_mutex_lock(&multi->pcm->mutex);
	eventfd_write(multi->pcm->fd, 1);
	pthread_mutex_unlock(&multi->pcm->mutex);
}

/**
 * Add more samples from clients into the mix.
 * Caller must hold lock on multi client_mutex  */
static void bluealsa_pcm_multi_update_mix(struct bluealsa_pcm_multi *multi) {
	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;
		bluealsa_pcm_client_deliver(client);
	}
}

static void bluealsa_pcm_multi_stop_if_no_clients(struct bluealsa_pcm_multi *multi) {
	pthread_mutex_lock(&multi->pcm->mutex);
	ba_transport_pcm_release(multi->pcm);
	ba_transport_pcm_signal_send(multi->pcm, BA_TRANSPORT_PCM_SIGNAL_CLOSE);
	pthread_mutex_unlock(&multi->pcm->mutex);
	ba_transport_stop_if_no_clients(multi->pcm->t);
}

/**
 * The mix thread. */
static void *bluealsa_pcm_mix_thread_func(struct bluealsa_pcm_multi *multi) {

	struct epoll_event events[BLUEALSA_MULTI_MAX_EVENTS] = { 0 };

	struct epoll_event event = {
		.events =  EPOLLIN,
		.data.ptr = multi,
	};

	epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, multi->event_fd, &event);

	debug("Starting pcm mix loop");
	for (;;) {

		int event_count;
		do {
			event_count = epoll_wait(multi->epoll_fd, events, BLUEALSA_MULTI_MAX_EVENTS, -1);
		} while (event_count == -1 && errno == EINTR);

		if (event_count <= 0) {
			error("epoll_wait failed: %d (%s)", errno, strerror(errno));
			goto terminate;
		}

		int n;
		for (n = 0; n < event_count; n++) {

			if (events[n].data.ptr == multi) {
				/* trigger from encoder thread */
				eventfd_t value = 0;
				eventfd_read(multi->event_fd, &value);
				if (value >= 0xDEAD0000)
					goto terminate;
				pthread_mutex_lock(&multi->buffer_mutex);
				pthread_mutex_lock(&multi->client_mutex);
				bluealsa_pcm_multi_update_mix(multi);
				pthread_mutex_unlock(&multi->client_mutex);
				multi->buffer_ready = true;
				pthread_cond_signal(&multi->cond);
				pthread_mutex_unlock(&multi->buffer_mutex);
				break;
			}

			else {   /* client event */
				struct bluealsa_pcm_client_event *cevent = events[n].data.ptr;
				struct bluealsa_pcm_client *client = cevent->client;

				bluealsa_pcm_client_handle_event(cevent);

				if (client->state == BLUEALSA_PCM_CLIENT_STATE_FINISHED) {
					pthread_mutex_lock(&multi->client_mutex);
					bluealsa_pcm_multi_remove_client(multi, client);
					pthread_mutex_unlock(&multi->client_mutex);

					/* removing a client invalidates the event array, so
					 * we need to call epoll_wait() again here */
					break;
				}
			}
		}

		if (multi->client_count == 0) {
			multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
			bluealsa_mix_buffer_clear(&multi->playback_buffer);
			bluealsa_pcm_multi_stop_if_no_clients(multi);
			continue;
		}

		if (multi->client_count == 1) {
			struct bluealsa_pcm_client* client = g_list_first(multi->clients)->data;
			if (client->drop) {
				bluealsa_mix_buffer_clear(&multi->playback_buffer);
				ba_transport_pcm_drop(multi->pcm);
				client->drop = false;
			}
		}

		if (multi->state == BLUEALSA_PCM_MULTI_STATE_INIT) {
			if (multi->active_count > 0) {
				pthread_mutex_lock(&multi->client_mutex);
				bluealsa_pcm_multi_update_mix(multi);
				pthread_mutex_unlock(&multi->client_mutex);
				if (bluealsa_mix_buffer_at_threshold(&multi->playback_buffer)) {
					multi->state = BLUEALSA_PCM_MULTI_STATE_RUNNING;
					bluealsa_pcm_multi_wake_transport(multi);
				}
			}
		}
		else if (multi->state == BLUEALSA_PCM_MULTI_STATE_RUNNING) {
			if (bluealsa_mix_buffer_empty(&multi->playback_buffer))
				multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
			else
				bluealsa_pcm_multi_wake_transport(multi);
		}
	}

terminate:
	multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
	pthread_cond_signal(&multi->cond);
	bluealsa_pcm_multi_wake_transport(multi);
	debug("mix thread function terminated");
	return NULL;
}


/**
 * The snoop thread. */
static void *bluealsa_pcm_snoop_thread_func(struct bluealsa_pcm_multi *multi) {

	struct epoll_event events[BLUEALSA_MULTI_MAX_EVENTS];

	struct epoll_event event = {
		.events =  EPOLLIN,
		.data.ptr = multi,
	};

	epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, multi->event_fd, &event);

	debug("Starting pcm snoop loop");
	for (;;) {
		int ret;

		do {
			ret = epoll_wait(multi->epoll_fd, events, BLUEALSA_MULTI_MAX_EVENTS, -1);
		} while (ret == -1 && errno == EINTR);

		if (ret <= 0) {
			error("epoll_wait failed: %d (%s)", errno, strerror(errno));
			goto terminate;
		}

		int n;
		for (n = 0; n < ret; n++) {

			if (events[n].data.ptr == multi) {
				/* trigger from transport thread */

				eventfd_t value = 0;
				eventfd_read(multi->event_fd, &value);
				if (value >= 0xDEAD0000)
					goto terminate;

			}

			else {
				/* client event */
				struct bluealsa_pcm_client_event *cevent = events[n].data.ptr;
				if (events[n].events & (EPOLLHUP|EPOLLERR)) {
					bluealsa_pcm_client_handle_close_event(cevent);
					pthread_mutex_lock(&multi->client_mutex);
					bluealsa_pcm_multi_remove_client(multi, cevent->client);
					if (multi->client_count == 0) {
						multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
						bluealsa_pcm_multi_stop_if_no_clients(multi);
					}
					pthread_mutex_unlock(&multi->client_mutex);

					/* removing a client invalidates the event array, so
					 * we need to call epoll_wait() again here */
					break;
				}
				else {
					bluealsa_pcm_client_handle_event(cevent);
					if (multi->state == BLUEALSA_PCM_MULTI_STATE_PAUSED && multi->active_count > 0) {
						multi->state = BLUEALSA_PCM_MULTI_STATE_RUNNING;
						ba_transport_pcm_resume(multi->pcm);
;
					}
				}
			}
		}
	}

terminate:
	multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
	debug("snoop thread function terminated");
	return NULL;
}
