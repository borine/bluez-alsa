/*
 * BlueALSA - io-worker.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#include "io-worker.h"

#include "alsa-mixer.h"
#include "aplay-config.h"
#include "delay-report.h"
#include "io-worker-input.h"
#include "io-worker-output.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "single-playback.h"

typedef struct {
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
	/* if true, playback is active */
	atomic_bool active;
	/* human-readable BT address */
	char addr[18];
} io_worker_t;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static io_worker_t *workers[16] = { NULL };
static size_t workers_size = ARRAYSIZE(workers);

/* local PCM muted state for software mute */
static bool pcm_muted = false;

static io_worker_t *get_active_io_worker(void) {

	pthread_rwlock_rdlock(&workers_lock);

	io_worker_t *w = NULL;
	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL && workers[i]->active) {
			w = workers[i];
			break;
		}

	pthread_rwlock_unlock(&workers_lock);

	return w;
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

static snd_pcm_uframes_t io_worker_playback_delay(
		const io_worker_t *w, const io_worker_output_t *output,
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

/**
 * Update BlueALSA PCM volume according to given volume scaling and mute state. */
static void io_worker_mixer_ba_pcm_set_volume(
		struct ba_pcm *ba_pcm,
		double vol_scaling,
		bool muted) {

	const int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	long volume = lround(vmax * vol_scaling);
	assert(volume < 0x80);

	for (size_t i = 0; i < ba_pcm->channels; i++) {
		ba_pcm->volume[i].muted = muted;
		ba_pcm->volume[i].volume = volume;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_update(&config.dbus_ctx, ba_pcm, BLUEALSA_PCM_VOLUME, &err)) {
		error("Couldn't update BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
	}
}

/**
 * Update one BlueALSA PCM volume according to ALSA mixer element. */
static void io_worker_mixer_volume_sync_ba_pcm(struct ba_pcm *ba_pcm) {
	double vol_scaling;
	bool muted;

	if (alsa_mixer_is_open()) {
		if (alsa_mixer_get_volume_scaling(&vol_scaling, &muted) < 0) {
			error("Couldn't get ALSA mixer volume");
			return;
		}
		io_worker_mixer_ba_pcm_set_volume(ba_pcm, vol_scaling, muted);
	}
}

/**
 * Update ALSA mixer element according to BlueALSA PCM volume. */
static bool io_worker_mixer_volume_sync_alsa_mixer(const struct ba_pcm *ba_pcm) {
	bool ret = false;

	/* Skip update in case of software volume. */
	if (ba_pcm->soft_volume)
		return 0;

	if (!alsa_mixer_is_open())
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
	const double vol_scaling = (double)volume_sum / (vmax * ba_pcm->channels);
	ret = (alsa_mixer_set_volume_scaling(vol_scaling, muted) == 0);

final:
	return ret;
}

/**
 * Update all active BlueALSA PCMs volume according to ALSA mixer element. */
void io_worker_mixer_event_callback(void *data) {
	(void) data;

	double vol_scaling;
	bool muted;

	if (alsa_mixer_get_volume_scaling(&vol_scaling, &muted) < 0) {
		warn("Couldn't get ALSA mixer volume");
		return;
	}

	for (size_t i = 0; i < workers_size; i++) {
		if (workers[i] && workers[i]->active) {
			pthread_mutex_lock(&workers[i]->mutex);
			io_worker_mixer_ba_pcm_set_volume(&workers[i]->ba_pcm, vol_scaling, muted);
			pthread_mutex_unlock(&workers[i]->mutex);
		}
	}
}

void io_worker_print_config(
			const io_worker_t *w,
			const io_worker_input_t *input,
			const io_worker_output_t *output) {

	info("Used configuration for %s:\n"
		"  BlueALSA PCM format: %s\n"
		"  BlueALSA PCM sample rate: %u Hz\n"
		"  BlueALSA PCM channels: %u\n"
		"  ALSA PCM buffer time: %u us (%zu bytes)\n"
		"  ALSA PCM period time: %u us (%zu bytes)\n"
		"  ALSA PCM format: %s\n"
		"  ALSA PCM sample rate: %u Hz\n"
		"  ALSA PCM channels: %u\n",
		w->addr,
		snd_pcm_format_name(input->pcm_format),
		input->ba_pcm->rate,
		input->ba_pcm->channels,
		output->pcm.buffer_time, alsa_pcm_frames_to_bytes(&output->pcm, output->pcm.buffer_frames),
		output->pcm.period_time, alsa_pcm_frames_to_bytes(&output->pcm, output->pcm.period_frames),
		snd_pcm_format_name(output->pcm.format),
		output->pcm.rate,
		output->pcm.channels);
	if (config.verbose >= 3)
		alsa_pcm_dump(&output->pcm, stderr);
}

/**
 * @return > 0 try again after timeout
 *           0 ok to write output
 *          -1 unable to access output
 *                 - EAGAIN try again with infinite timeout
 *                     - pausing and pause count exceeded
 *                     - opening and open retry threshold not reached
 *                     - opening and open attempt failed
 *                 - any other errno terminate worker thread.
 *                     - unable to allocate resampler
 */
static int io_worker_active_check(io_worker_t *w,
						io_worker_output_t *output,
						single_playback_t *sp,
						ffb_t *read_buffer,
						size_t input_samples) {

	errno = 0;
	if (!w->active) {

		/* Before checking active worker, we need to lock the single playback
		 * mutex. It is required to lock it, because the active state is changed
		 * in the worker thread after opening the PCM device, so we
		 * have to synchronize all threads at this point. */
		single_playback_lock(sp);

		if ((get_active_io_worker()) != NULL) {
			single_playback_unlock(sp);
			single_playback_pause(sp, input_samples);
			return 100;
		}
	}

	int timeout = 0;

	if (!io_worker_output_is_open(output)) {

		if (!io_worker_output_open(output, read_buffer, input_samples)) {
			if (errno == EBUSY)
				errno = EAGAIN;
			timeout = -1;
			goto finish;
		}

		/* Mark device as active. */
		w->active = true;
		single_playback_reset(sp);
		debug("BT device marked as active: %s", w->addr);

		/* Set device initial volume only if not using soft-volume */
		if (!w->ba_pcm.soft_volume)
			io_worker_mixer_volume_sync_ba_pcm(&w->ba_pcm);
	}

finish:
	single_playback_unlock(sp);
	return timeout;
}

static int io_worker_do_output(
				io_worker_t *w,
				io_worker_input_t *input,
				io_worker_output_t *output,
				struct delay_report *dr,
				size_t read_samples,
				bool drain) {

	const bool force_mute = (!alsa_mixer_has_mute_switch() && pcm_muted);
	int timeout;

	errno = 0;
	if ((timeout = io_worker_output_write(output, &input->buffer, force_mute, drain)) < 0) {
		if (errno != EAGAIN) {
			/* Failure to write to a running PCM is not recoverable, so we
			 * close the output. */
			io_worker_output_close(output);
			/* Reset moving delay window buffer. */
			delay_report_reset(dr);
		}
		return -1;
	}

	if (drain) {
		/* No need to update the delay report or resampler when the input is
		* finished. */
		return -1;
	}

	DBusError err = DBUS_ERROR_INIT;
	const snd_pcm_uframes_t delay_frames = io_worker_playback_delay(w, output, &input->buffer);
	if (!delay_report_update(dr, delay_frames, &err)) {
		if (config.verbose >= 3)
			warn("Couldn't update BlueALSA PCM client delay: %s", err.message);
		dbus_error_free(&err);
	}

	io_worker_output_update_rate(output, read_samples / w->ba_pcm.channels, dr->avg_value);

	return timeout;
}

static void io_worker_event_loop(
				io_worker_t *w,
				io_worker_input_t *input,
				io_worker_output_t *output,
				single_playback_t *sp,
				struct delay_report *dr) {

	struct pollfd fds[16] = {
		{ config.main_loop_quit_event_fd, POLLIN, 0 },
		{ input->ba_pcm_fd, POLLIN, 0 }};
	const nfds_t nfds = 2;
	int timeout = -1;
	bool config_printed = false;

	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(fds, nfds, timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		if (poll_rv == -1) {
			if (errno == EINTR)
				continue;
			/* The worker thread cannot recover from a poll() failure */
			error("IO loop poll error: %s", strerror(errno));
			break;
		}

		if (fds[0].revents & POLLIN)
			/* Process terminated by user */
			break;

		pthread_mutex_lock(&w->mutex);
		/* Check the PCM running status on every iteration. */
		bool ba_pcm_running = w->ba_pcm.running;
		pthread_mutex_unlock(&w->mutex);

		if (poll_rv == 0) {
			/* Timeout. If the ALSA PCM is not already stopped, then allow it
			 * to process any remaining samples in the buffers. If the source
			 * has stopped running then block until ALSA has drained all
			 * remaining samples from the buffers. */
			if (io_worker_output_is_running(output)) {
				timeout = io_worker_do_output(w, input, output, dr, 0, !ba_pcm_running);
				if (timeout > 0)
					continue;
			}

			timeout = -1;

			if (w->active && ba_pcm_running) {
				/* The BT device is in the running state, but is not sending
				 * audio frames. As there is no work for the ALSA device to do
				 * we simply wait for more audio to arrive from the server. */
				continue;
			}

			if (!ba_pcm_running)
				debug("BT device marked as inactive: %s", w->addr);

			io_worker_output_close(output);
			single_playback_reset(sp);
			delay_report_reset(dr);
			w->active = !config.force_single_playback;
			continue;
		}

		ssize_t read_samples = 0;
		if (fds[1].revents & POLLIN) {
			if ((read_samples = io_worker_input_read(input, !w->active, output->use_resampler)) == -1) {
				error("Error reading from FIFO (%s)", strerror(errno));
				break;
			}

			if (read_samples == 0 && !io_worker_output_is_running(output)) {
				debug("BlueALSA source PCM has disconnected");
				break;
			}
		}
		else {
			error("Unexpected poll event on FIFO: %d", fds[1].revents);
			break;
		}

		if ((timeout = io_worker_active_check(w, output, sp, &input->buffer, read_samples)) == -1 && errno != EAGAIN)
			break;

		if (timeout == 0) {
			/* On the first iteration after opening the output print the setup. */
			if (config.verbose >= 2 && !config_printed) {
				single_playback_lock(sp);
				io_worker_print_config(w, input, output);
				single_playback_unlock(sp);
				config_printed = true;
			}

			/* Write samples to the output */
			timeout = io_worker_do_output(w, input, output, dr, 0, !ba_pcm_running);
		}
	}
}

static void io_worker_routine_exit(io_worker_t *w) {
#if !DEBUG
	(void) w;
#else
	debug("Exiting IO worker %s", w->addr);
#endif
}

static void *io_worker_routine(io_worker_t *w) {

	const size_t pcm_1s_samples = w->ba_pcm.rate * w->ba_pcm.channels;

	io_worker_input_t input = { 0 };
	io_worker_output_t output = { 0 };
	single_playback_t sp;
	struct delay_report dr;

	/* Cancellation should be possible only in the carefully selected place
	* in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_routine_exit), w);
	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_input_close), &input);
	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_output_close), &output);

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

	if (!io_worker_input_init(&input, &w->ba_pcm))
		goto fail;

	if (!io_worker_output_init(&output, input.pcm_format, w->ba_pcm.channels, w->ba_pcm.rate))
		goto fail;

	/* In order not to flood BT connection with AVRCP packets, we are limit
	 * sending of pause command in single playback mode to every 0.5 second. */
	single_playback_init(&sp, w->ba_pcm.device_path, pcm_1s_samples / 2);

	delay_report_init(&dr, &config.dbus_ctx, &w->ba_pcm);

	debug("Starting IO loop");
	io_worker_event_loop(w, &input, &output, &sp, &dr);

fail:
	pthread_cleanup_pop(1);  /* io_worker_output_close() */
	pthread_cleanup_pop(1);  /* io_worker_input_close()  */
	pthread_cleanup_pop(1);  /* io_worker_routine_exit() */
	return NULL;
}

static io_worker_t *io_worker_create(const char *addr) {
	io_worker_t *worker;

	if ((worker = malloc(sizeof(io_worker_t))) == NULL)
		return NULL;

	worker->thread_started = false;
	pthread_mutex_init(&worker->mutex, NULL);
	strcpy(worker->addr, addr);
	worker->ba_pcm_fd = -1;
	worker->ba_pcm_ctrl_fd = -1;

	return worker;
}

static bool io_worker_start_private(io_worker_t *worker, const struct ba_pcm *ba_pcm) {
	memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
	worker->active = !config.force_single_playback;

	if ((errno = pthread_create(&worker->thread, NULL,
					PTHREAD_FUNC(io_worker_routine), worker)) == 0) {
		worker->thread_started = true;
		io_worker_mixer_volume_sync_ba_pcm(&worker->ba_pcm);
		return true;
	}

	return false;
}

static void io_worker_stop_private(io_worker_t *w) {
	if (w->thread_started) {
		pthread_cancel(w->thread);
		pthread_join(w->thread, NULL);
		w->thread_started = false;
	}
}

/**
 * Stop the IO worker thread and free its resources. */
static void io_worker_destroy(io_worker_t *w) {
	io_worker_stop_private(w);
	pthread_mutex_destroy(&w->mutex);
	free(w);
}

bool io_worker_start(const struct ba_pcm *ba_pcm) {

	io_worker_t *worker = NULL;
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
				/* Skip volume update in case of software volume. */
				if (!ba_pcm->soft_volume && workers[i]->active && ba_pcm->running)
					io_worker_mixer_volume_sync_alsa_mixer(ba_pcm);
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
