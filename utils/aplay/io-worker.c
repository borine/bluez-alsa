/*
 * BlueALSA - io-worker.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#include "io-worker.h"

#include "alsa-mixer.h"
#include "aplay-config.h"
#include "delay-report.h"
#include "io-worker-output.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

struct io_worker {
	pthread_t thread;
	bool thread_started;
	/* thread-safety for worker data access */
	pthread_mutex_t mutex;
	/* used BlueALSA PCM device */
	struct ba_pcm ba_pcm;
	/* file descriptor of PCM FIFO */
	int ba_pcm_fd;
	/* file descriptor of PCM control */
	int ba_pcm_ctrl_fd;
	/* mixer for volume control */
	struct alsa_mixer alsa_mixer;
	/* if true, playback is active */
	atomic_bool active;
	/* human-readable BT address */
	char addr[18];
};

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct io_worker *workers[16] = { NULL };
static size_t workers_size = ARRAYSIZE(workers);

static pthread_mutex_t single_playback_mutex = PTHREAD_MUTEX_INITIALIZER;

/* local PCM muted state for software mute */
static bool pcm_muted = false;

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

/**
 * Update BlueALSA PCM volume according to ALSA mixer element. */
static int io_worker_mixer_volume_sync_ba_pcm(
		struct io_worker *worker,
		struct ba_pcm *ba_pcm) {

	unsigned int volume;
	/* If mixer element does not support playback switch,
	 * use our global muted state as a default value. */
	bool muted = pcm_muted;

	const int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	if (alsa_mixer_get_volume(&worker->alsa_mixer, vmax, &volume, &muted) != 0)
		return -1;

	for (size_t i = 0; i < ba_pcm->channels; i++) {
		ba_pcm->volume[i].muted = muted;
		ba_pcm->volume[i].volume = volume;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_update(&config.dbus_ctx, ba_pcm, BLUEALSA_PCM_VOLUME, &err)) {
		error("Couldn't update BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		return -1;
	}

	return 0;
}

static struct io_worker *get_active_io_worker(void) {

	pthread_rwlock_rdlock(&workers_lock);

	struct io_worker *w = NULL;
	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL && workers[i]->active) {
			w = workers[i];
			break;
		}

	pthread_rwlock_unlock(&workers_lock);

	return w;
}

static int pause_device_player(const struct ba_pcm *ba_pcm) {

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	char path[160];
	int ret = 0;

	snprintf(path, sizeof(path), "%s/player0", ba_pcm->device_path);
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

static bool pcm_hw_params_equal(
		const struct ba_pcm *ba_pcm_1,
		const struct ba_pcm *ba_pcm_2) {
	if (ba_pcm_1->format != ba_pcm_2->format)
		return false;
	if (ba_pcm_1->channels != ba_pcm_2->channels)
		return false;
	if (ba_pcm_1->rate != ba_pcm_2->rate)
		return false;
	return true;
}

static void io_worker_mixer_event_callback(void *data) {
	struct io_worker *worker = data;
	io_worker_mixer_volume_sync_ba_pcm(worker, &worker->ba_pcm);
}

static snd_pcm_uframes_t io_worker_playback_delay(
		const struct io_worker *w, const struct io_worker_output *output,
		const ffb_t *read_buffer) {

	snd_pcm_uframes_t delay = 0;

	unsigned int ba_pcm_buffered = 0;
	/* Get the delay due to BlueALSA PCM FIFO buffering. */
	ioctl(w->ba_pcm_fd, FIONREAD, &ba_pcm_buffered);
	delay = ba_pcm_buffered / read_buffer->size / w->ba_pcm.channels;

	delay += ffb_len_out(read_buffer) / w->ba_pcm.channels;
	delay += io_worker_output_delay(output);

	return delay;
}

static void io_worker_routine_exit(struct io_worker *w) {

	pthread_mutex_lock(&w->mutex);

	if (w->ba_pcm_ctrl_fd != -1) {
		close(w->ba_pcm_ctrl_fd);
		w->ba_pcm_ctrl_fd = -1;
	}

	alsa_mixer_close(&w->alsa_mixer);

	debug("Exiting IO worker %s", w->addr);
	pthread_mutex_unlock(&w->mutex);

}

static void *io_worker_routine(struct io_worker *w) {

	const snd_pcm_format_t pcm_format = bluealsa_get_snd_pcm_format(&w->ba_pcm);
	const ssize_t pcm_format_size = snd_pcm_format_size(pcm_format, 1);
	const size_t pcm_1s_samples = w->ba_pcm.rate * w->ba_pcm.channels;
	/* Buffer for audio frames read from the BlueALSA server. */
	ffb_t read_buffer = { 0 };
	struct io_worker_output output = { 0 };

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_routine_exit), w);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &read_buffer);
	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_output_free), &output);

	/* Create a buffer big enough to hold enough PCM data for three periods.
	 * This will be later be revised if necessary to match the actual ALSA
	 * start threshold when the ALSA PCM is opened. */
	const size_t nmemb = ((size_t)config.pcm_period_time * 3 / 1000) * (pcm_1s_samples / 1000);
	if (ffb_init(&read_buffer, nmemb, pcm_format_size) == -1) {
		error("Couldn't create PCM buffer: %s", strerror(errno));
		goto fail;
	}

	DBusError err = DBUS_ERROR_INIT;

	/* initialize the PCM soft_volume setting */
	if (config.volume_type != VOL_TYPE_AUTO) {
		bool softvol = (config.volume_type == VOL_TYPE_SOFTWARE);
		debug("Setting BlueALSA source PCM volume mode: %s: %s",
				w->ba_pcm.pcm_path, softvol ? "software" : "pass-through");
		if (softvol != w->ba_pcm.soft_volume) {
			w->ba_pcm.soft_volume = softvol;
			if (!ba_dbus_pcm_update(&config.dbus_ctx, &w->ba_pcm, BLUEALSA_PCM_SOFT_VOLUME, &err)) {
				error("Couldn't set BlueALSA source PCM volume mode: %s", err.message);
				dbus_error_free(&err);
				goto fail;
			}
		}
	}

	debug("Opening BlueALSA source PCM: %s", w->ba_pcm.pcm_path);
	if (!ba_dbus_pcm_open(&config.dbus_ctx, w->ba_pcm.pcm_path,
				&w->ba_pcm_fd, &w->ba_pcm_ctrl_fd, &err)) {
		error("Couldn't open BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	if (!io_worker_output_init(&output, pcm_format, w->ba_pcm.channels,
								w->ba_pcm.rate)) {
		goto fail; // ???
	}

	/* Track the lock state of the single playback mutex within this thread. */
	bool single_playback_mutex_locked = false;

	struct delay_report dr;
	delay_report_init(&dr, &config.dbus_ctx, &w->ba_pcm);

	size_t pause_retry_pcm_samples = pcm_1s_samples;
	size_t pause_retries = 0;

	int timeout = -1;

	debug("Starting IO loop");
	for (;;) {

		if (single_playback_mutex_locked) {
			pthread_mutex_unlock(&single_playback_mutex);
			single_playback_mutex_locked = false;
		}

		struct pollfd fds[16] = {
			{ config.main_loop_quit_event_fd, POLLIN, 0 },
			{ w->ba_pcm_fd, POLLIN, 0 }};
		nfds_t nfds = 2;

		if (alsa_mixer_is_open(&w->alsa_mixer)) {
			nfds += alsa_mixer_poll_descriptors_count(&w->alsa_mixer);
			if (nfds <= ARRAYSIZE(fds))
				alsa_mixer_poll_descriptors(&w->alsa_mixer, fds + 2, nfds - 2);
			else {
				error("Poll FD array size exceeded: %zu > %zu", (size_t)nfds, ARRAYSIZE(fds));
				goto fail;
			}
		}

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(fds, nfds, timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		pthread_mutex_lock(&w->mutex);
		/* Check the PCM running status on every iteration. */
		bool ba_pcm_running = w->ba_pcm.running;
		pthread_mutex_unlock(&w->mutex);

		if (poll_rv == -1) {
			if (errno == EINTR)
				continue;
			error("IO loop poll error: %s", strerror(errno));
			goto fail;
		}

		if (poll_rv == 0 &&
				ba_pcm_running &&
				w->active &&
				ffb_blen_out(&read_buffer) == 0 &&
				!alsa_pcm_is_running(&output.pcm)) {
			/* The BT device is in the running state, but is not sending audio
			 * frames. As there is no work for the ALSA device to do we simply
			 * wait for more audio to arrive from the server. */
			timeout = -1;
			continue;
		}

		if (fds[0].revents & POLLIN)
			break;

		if (alsa_mixer_is_open(&w->alsa_mixer))
			alsa_mixer_handle_events(&w->alsa_mixer);

		size_t read_samples = 0;
		if (fds[1].revents & POLLIN) {

			/* If the read buffer is full then we have an overrun. We must
			 * discard audio frames in order to continue reading fresh data
			 * from the server. */
			if (ffb_blen_in(&read_buffer) == 0) {
				unsigned int buffered = 0;
				ioctl(w->ba_pcm_fd, FIONREAD, &buffered);
				const size_t discard_bytes = MIN(buffered, ffb_blen_out(&read_buffer));
				const size_t discard_samples = discard_bytes / pcm_format_size;
				ffb_shift(&read_buffer, discard_samples);
				if (io_worker_output_is_open(&output))
					warn("Dropping PCM frames: %zu", discard_samples / w->ba_pcm.channels);
			}

			ssize_t ret;
			if ((ret = read(w->ba_pcm_fd, read_buffer.tail, ffb_blen_in(&read_buffer))) == -1) {
				if (errno == EINTR)
					continue;
				error("BlueALSA source PCM read error: %s", strerror(errno));
				goto fail;
			}

			read_samples = ret / pcm_format_size;
			if (ret % pcm_format_size != 0)
				warn("Invalid read from BlueALSA source PCM: %zd %% %zd != 0", ret, pcm_format_size);

			io_worker_output_set_endianness(&output, read_buffer.tail, read_samples, pcm_format);
			ffb_seek(&read_buffer, read_samples);

		}
		else if (fds[1].revents & POLLHUP) {
			/* Source PCM FIFO has been terminated on the writing side. */
			debug("BlueALSA source PCM disconnected: %s", w->ba_pcm.pcm_path);
			ba_pcm_running = false;
			break;
		}
		else if (fds[1].revents) {
			error("Unexpected BlueALSA source PCM poll event: %#x", fds[1].revents);
		}

		/* If current worker is not active and the single playback mode was
		 * enabled, we have to check if there is any other active worker. */
		if (!w->active) {

			/* Before checking active worker, we need to lock the single playback
			 * mutex. It is required to lock it, because the active state is changed
			 * in the worker thread after opening the PCM device, so we have to
			 * synchronize all threads at this point. */
			pthread_mutex_lock(&single_playback_mutex);
			single_playback_mutex_locked = true;

			if (get_active_io_worker() != NULL) {
				/* In order not to flood BT connection with AVRCP packets,
				 * we are going to send pause command every 0.5 second. */
				if (pause_retries < 5 &&
						(pause_retry_pcm_samples += read_samples) > pcm_1s_samples / 2) {
					if (pause_device_player(&w->ba_pcm) == -1)
						/* pause command does not work, stop further requests */
						pause_retries = 5;
					pause_retry_pcm_samples = 0;
					pause_retries++;
					timeout = 100;
				}
				continue;
			}

		}

		if (!io_worker_output_is_open(&output)) {

			if (!io_worker_output_open_check(&output, read_samples, pcm_1s_samples))
				continue;

			if (!io_worker_output_open(&output, &read_buffer)) {
				if (errno == EBUSY)
					continue;
				goto fail;
			}

			/* Skip mixer setup in case of software volume. */
			if (config.mixer_device != NULL && !w->ba_pcm.soft_volume) {
				char *tmp = NULL;
				pthread_mutex_lock(&w->mutex);
				debug("Opening ALSA mixer: name=%s elem=%s index=%u",
						config.mixer_device, config.mixer_elem_name,
						config.mixer_elem_index);
				if (alsa_mixer_open(&w->alsa_mixer, config.mixer_device,
						config.mixer_elem_name, config.mixer_elem_index,
						&tmp) == 0)
					io_worker_mixer_volume_sync_ba_pcm(w, &w->ba_pcm);
				else {
					warn("Couldn't open ALSA mixer: %s", tmp);
					free(tmp);
				}
				pthread_mutex_unlock(&w->mutex);
			}

			/* Reset moving delay window buffer. */
			delay_report_reset(&dr);

			if (config.verbose >= 2) {
				info("Used configuration for %s:\n"
						"  BlueALSA PCM format: %s\n"
						"  BlueALSA PCM sample rate: %u Hz\n"
						"  BlueALSA PCM channels: %u\n"
						"  ALSA PCM buffer time: %u us (%zu bytes)\n"
						"  ALSA PCM period time: %u us (%zu bytes)\n"
						"  ALSA PCM format: %s\n"
						"  ALSA PCM sample rate: %u Hz\n"
						"  ALSA PCM channels: %u\n"
						"  ALSA mixer volume mapping: %s",
						w->addr,
						snd_pcm_format_name(pcm_format),
						w->ba_pcm.rate,
						w->ba_pcm.channels,
						output.pcm.buffer_time, alsa_pcm_frames_to_bytes(&output.pcm, output.pcm.buffer_frames),
						output.pcm.period_time, alsa_pcm_frames_to_bytes(&output.pcm, output.pcm.period_frames),
						snd_pcm_format_name(output.pcm.format),
						output.pcm.rate,
						output.pcm.channels,
						w->alsa_mixer.mixer ? (w->alsa_mixer.has_db_scale ? "dB scale" : "linear") : "none");
			}

			if (config.verbose >= 3)
				alsa_pcm_dump(&output.pcm, stderr);

		}

		/* Mark device as active. */
		w->active = true;

		/* Current worker was marked as active, so we can safely
		 * release the single playback mutex if it was locked. */
		if (single_playback_mutex_locked) {
			pthread_mutex_unlock(&single_playback_mutex);
			single_playback_mutex_locked = false;
		}

		const bool force_mute = (!w->alsa_mixer.has_mute_switch && pcm_muted);
		if ((timeout = io_worker_output_write(&output, &read_buffer, force_mute, !ba_pcm_running)) < 0) {
			if (alsa_pcm_is_running(&output.pcm))
				goto close_output;
		}

		if (!ba_pcm_running)
			goto device_inactive;

		const snd_pcm_uframes_t delay_frames = io_worker_playback_delay(w, &output, &read_buffer);
		if (!delay_report_update(&dr, delay_frames, &err)) {
			error("Couldn't update BlueALSA PCM client delay: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}

		io_worker_output_update_rate(&output, read_samples / w->ba_pcm.channels, dr.avg_value);

		continue;

device_inactive:
		debug("BT device marked as inactive: %s", w->addr);
		pause_retry_pcm_samples = pcm_1s_samples;
		pause_retries = 0;
		timeout = -1;

close_output:
		ffb_rewind(&read_buffer);
		io_worker_output_free(&output);
		pthread_mutex_lock(&w->mutex);
		alsa_mixer_close(&w->alsa_mixer);
		pthread_mutex_unlock(&w->mutex);

		w->active = !config.force_single_playback;
	}

fail:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static struct io_worker *io_worker_create(const char *addr) {
	struct io_worker *worker;

	if ((worker = malloc(sizeof(struct io_worker))) == NULL)
		return NULL;

	worker->thread_started = false;
	pthread_mutex_init(&worker->mutex, NULL);
	strcpy(worker->addr, addr);
	worker->ba_pcm_fd = -1;
	worker->ba_pcm_ctrl_fd = -1;

	return worker;
}

static bool io_worker_start_private(struct io_worker *worker, const struct ba_pcm *ba_pcm) {
	memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
	alsa_mixer_init(&worker->alsa_mixer, io_worker_mixer_event_callback, worker);
	worker->active = !config.force_single_playback;

	if ((errno = pthread_create(&worker->thread, NULL,
					PTHREAD_FUNC(io_worker_routine), worker)) == 0) {
		worker->thread_started = true;
		return true;
	}

	return false;
}

static void io_worker_stop_private(struct io_worker *w) {
	if (w->thread_started) {
		pthread_cancel(w->thread);
		pthread_join(w->thread, NULL);
		w->thread_started = false;
	}
}

/**
 * Stop the IO worker thread and free its resources. */
static void io_worker_destroy(struct io_worker *w) {
	io_worker_stop_private(w);
	pthread_mutex_destroy(&w->mutex);
	free(w);
}

/**
 * Update ALSA mixer element according to BlueALSA PCM volume. */
bool io_worker_mixer_volume_sync_alsa_mixer(struct ba_pcm *ba_pcm) {
	struct io_worker *worker = NULL;
	bool ret = false;

	pthread_rwlock_rdlock(&workers_lock);

	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] && strcmp(workers[i]->ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			worker = workers[i];
			break;
		}

	pthread_rwlock_unlock(&workers_lock);

	if (worker == NULL)
		return false;

	/* This function is called by the D-Bus signal handler, so we have to
	 * make sure that we will not have any interference from the IO thread
	 * trying to modify ALSA mixer at the same time. */
	pthread_mutex_lock(&worker->mutex);

	if (!alsa_mixer_is_open(&worker->alsa_mixer))
		goto final;

	/* User can connect BlueALSA PCM to mono, stereo or multi-channel output.
	 * For mono input (audio from BlueALSA PCM), the case is simple: we are
	 * changing all output channels at once. However, for stereo input it is
	 * not possible to know how to control left/right volume unless there is
	 * some kind of channel mapping. In order to simplify things, we will set
	 * all channels to the average left-right volume. */

	unsigned int volume_sum = 0, muted = 0;
	for (size_t i = 0; i < ba_pcm->channels; i++) {
		volume_sum += ba_pcm->volume[i].volume;
		muted |= ba_pcm->volume[i].muted;
	}

	/* keep local muted state up to date */
	pcm_muted = muted;

	const unsigned int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	const unsigned int volume = volume_sum / ba_pcm->channels;
	ret = (alsa_mixer_set_volume(&worker->alsa_mixer, vmax, volume, muted) == 0);

final:
	pthread_mutex_unlock(&worker->mutex);
	return ret;
}

bool io_worker_start(const struct ba_pcm *ba_pcm) {

	struct io_worker *worker = NULL;
	char addr[sizeof(worker->addr)];
	ssize_t worker_slot = -1;

	for (size_t i = 0; i < workers_size; i++) {
		if (workers[i] == NULL) {
			if (worker_slot == -1)
				worker_slot = i;
		}
		else if (strcmp(workers[i]->ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			/* If the codec has changed after the device connected, then the
			 * audio format may have changed. If it has, the worker thread
			 * needs to be restarted. Otherwise, update the running state. */
			if (!pcm_hw_params_equal(&workers[i]->ba_pcm, ba_pcm)) {
				io_worker_stop_private(workers[i]);
				worker = workers[i];
				worker_slot = i;
				break;
			}
			else {
				pthread_mutex_lock(&workers[i]->mutex);
				workers[i]->ba_pcm.running = ba_pcm->running;
				pthread_mutex_unlock(&workers[i]->mutex);
				return true;
			}
		}
	}

	if (worker == NULL) {

		/* Human-readable BT address for early error reporting. */
		ba2str(&ba_pcm->addr, addr);

		if (worker_slot == -1) {
			error("Couldn't start IO worker %s: %s", addr, "No empty slots");
			return false;
		}

		if ((worker = io_worker_create(addr)) == NULL) {
			error("Couldn't start IO worker %s: %s", addr, strerror(errno));
			return false;
		}

	}

	/* Synchronize access to the worker data, which we are about
	 * to modify, with other IO worker threads. */
	pthread_rwlock_wrlock(&workers_lock);

	debug("Starting IO worker %s", worker->addr);
	if (io_worker_start_private(worker, ba_pcm))
		workers[worker_slot] = worker;
	else {
		error("Couldn't start IO worker %s: %s", worker->addr, strerror(errno));
		io_worker_destroy(worker);
		worker = NULL;
	}

	pthread_rwlock_unlock(&workers_lock);

	return worker != NULL;
}

void io_worker_stop(const struct ba_pcm *ba_pcm) {

	pthread_rwlock_wrlock(&workers_lock);

	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] && strcmp(workers[i]->ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			io_worker_destroy(workers[i]);
			workers[i] = NULL;
			break;
		}

	pthread_rwlock_unlock(&workers_lock);
}

void io_worker_cleanup(void) {
	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL)
			io_worker_stop_private(workers[i]);
	/* When all workers are stopped, we can safely free the resources
	 * in a lockless manner without risking any race conditions. */
	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL)
			io_worker_destroy(workers[i]);
}
