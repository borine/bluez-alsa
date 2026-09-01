/*
 * BlueALSA - single-playback.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "aplay-config.h"
#include "io-worker.h"
#include "shared/log.h"
#include "single-playback.h"

/* Mutex to control entry to critical section where active worker is chosen
 * for single playback modes. */
static pthread_mutex_t single_playback_mutex = PTHREAD_MUTEX_INITIALIZER;

static int pause_device_player(const char *device_path) {

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	char path[160];
	int ret = 0;

	snprintf(path, sizeof(path), "%s/player0", device_path);
	msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.MediaPlayer1", "Pause");

	if ((rep = dbus_connection_send_with_reply_and_block(config.dbus_ctx.conn, msg,
					DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		warn("Couldn't pause player: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	debug("Requested playback pause");
	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return ret;
}

void single_playback_init(single_playback_t *sp, const char *device, size_t interval_samples) {
	sp->device = device;
	sp->locked = false;
	sp->pause_retry_interval = interval_samples;
	sp->pause_retry_pcm_samples = sp->pause_retry_interval;
}

void single_playback_lock(single_playback_t *sp) {
	pthread_mutex_lock(&single_playback_mutex);
	sp->locked = true;
}

void single_playback_unlock(single_playback_t *sp) {
	if (sp->locked)
		pthread_mutex_unlock(&single_playback_mutex);
	sp->locked = false;
}

void single_playback_pause(single_playback_t *sp, size_t input_samples) {
	if (sp->pause_retries < 5 &&
			(sp->pause_retry_pcm_samples += input_samples) > sp->pause_retry_interval) {
		if (!pause_device_player(sp->device))
			/* pause command does not work, stop further requests */
			sp->pause_retries = 5;
		sp->pause_retry_pcm_samples = 0;
		sp->pause_retries++;
	}
}

void single_playback_reset(single_playback_t *sp) {
	sp->pause_retry_pcm_samples = sp->pause_retry_interval;
	if (sp->pause_retries < 5)
		sp->pause_retries = 0;
	single_playback_unlock(sp);
}

