/*
 * BlueALSA - aplay.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "alsa-mixer.h"
#include "aplay-config.h"
#include "dbus.h"
#include "io-worker.h"
#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/nv.h"

/* Many devices cannot synchronize A/V with very high audio latency. To keep
 * the overall latency below 400ms we choose default ALSA parameters such that
 * the ALSA latency for A2DP is below 200ms. For SCO we choose to prioritize
 * much lower latency over audio quality. */
#define DEFAULT_PERIOD_TIME_A2DP 50000
#define DEFAULT_PERIOD_TIME_SCO 20000
#define DEFAULT_PERIODS 4

enum profile {
	PROFILE_A2DP,
	PROFILE_ASHA,
	PROFILE_SCO,
};

static bool list_bt_devices = false;
static bool list_bt_pcms = false;
static bool ba_profiles[3] = { 0 };
static bool ba_addr_any = false;
static bdaddr_t * ba_addrs = NULL;
static size_t ba_addrs_count = 0;

static char dbus_ba_service[32] = BLUEALSA_SERVICE;

static struct ba_pcm *ba_pcms = NULL;
static size_t ba_pcms_count = 0;

static void main_loop_stop(int sig) {
	eventfd_write(config.main_loop_quit_event_fd, sig);
}

static const char * profile2str(enum profile profile) {
	switch (profile) {
	case PROFILE_A2DP:
		return "A2DP";
	case PROFILE_ASHA:
		return "ASHA";
	case PROFILE_SCO:
		return "SCO";
	}
	return "UNKNOWN";
}

static int parse_bt_addresses(char * argv[], size_t count) {

	ba_addrs_count = count;
	if ((ba_addrs = malloc(sizeof(*ba_addrs) * ba_addrs_count)) == NULL)
		return -1;

	for (size_t i = 0; i < ba_addrs_count; i++) {
		if (str2ba(argv[i], &ba_addrs[i]) != 0)
			return errno = EINVAL, -1;
		if (bacmp(&ba_addrs[i], BDADDR_ANY) == 0)
			ba_addr_any = true;
	}

	return 0;
}

static const char * ba_pcm_get_profile_name(const struct ba_pcm * pcm) {
	switch (pcm->transport) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return "A2DP";
	case BA_PCM_TRANSPORT_ASHA_SOURCE:
	case BA_PCM_TRANSPORT_ASHA_SINK:
		return "ASHA";
	case BA_PCM_TRANSPORT_HFP_AG:
	case BA_PCM_TRANSPORT_HFP_HF:
	case BA_PCM_TRANSPORT_HSP_AG:
	case BA_PCM_TRANSPORT_HSP_HS:
		return "SCO";
	default:
		error("Unknown transport: %#x", pcm->transport);
		return "[...]";
	}
}

static const char * ba_pcm_get_profile_tag(const struct ba_pcm * pcm) {
	switch (pcm->transport) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return "a2dp";
	case BA_PCM_TRANSPORT_ASHA_SOURCE:
	case BA_PCM_TRANSPORT_ASHA_SINK:
		return "asha";
	case BA_PCM_TRANSPORT_HFP_AG:
	case BA_PCM_TRANSPORT_HFP_HF:
	case BA_PCM_TRANSPORT_HSP_AG:
	case BA_PCM_TRANSPORT_HSP_HS:
		return "sco";
	default:
		error("Unknown transport: %#x", pcm->transport);
		return "unknown";
	}
}

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

static void print_bt_device_list(void) {

	static const struct {
		const char * label;
		unsigned int mode;
	} section[2] = {
		{ "**** List of PLAYBACK Bluetooth Devices ****", BA_PCM_MODE_SINK },
		{ "**** List of CAPTURE Bluetooth Devices ****", BA_PCM_MODE_SOURCE },
	};

	const char * tmp;
	size_t ii;

	for (size_t i = 0; i < ARRAYSIZE(section); i++) {
		printf("%s\n", section[i].label);
		for (ii = 0, tmp = ""; ii < ba_pcms_count; ii++) {

			struct ba_pcm * pcm = &ba_pcms[ii];
			struct bluez_device dev = { 0 };

			if (!(pcm->mode == section[i].mode))
				continue;

			if (strcmp(pcm->device_path, tmp) != 0) {
				tmp = ba_pcms[ii].device_path;

				DBusError err = DBUS_ERROR_INIT;
				if (dbus_bluez_get_device(config.dbus_ctx.conn, pcm->device_path, &dev, &err) == -1) {
					warn("Couldn't get BlueZ device properties: %s", err.message);
					dbus_error_free(&err);
				}

				char bt_addr[18];
				ba2str(&dev.bt_addr, bt_addr);

				printf("%s: %s [%s], %s%s\n",
						dev.hci_name, bt_addr, dev.name,
						dev.trusted ? "trusted " : "", dev.icon);

			}

			printf("  %s (%s): %s %d channel%s %d Hz\n",
					ba_pcm_get_profile_name(pcm),
					pcm->codec.name,
					snd_pcm_format_name(bluealsa_get_snd_pcm_format(pcm)),
					pcm->channels, pcm->channels != 1 ? "s" : "",
					pcm->rate);

		}
	}

}

static void print_bt_pcm_list(void) {

	DBusError err = DBUS_ERROR_INIT;
	struct bluez_device dev = { 0 };
	const char * tmp = "";

	for (size_t i = 0; i < ba_pcms_count; i++) {
		struct ba_pcm * pcm = &ba_pcms[i];

		if (strcmp(pcm->device_path, tmp) != 0) {
			tmp = ba_pcms[i].device_path;
			if (dbus_bluez_get_device(config.dbus_ctx.conn, pcm->device_path, &dev, &err) == -1) {
				warn("Couldn't get BlueZ device properties: %s", err.message);
				dbus_error_free(&err);
			}
		}

		char bt_addr[18];
		ba2str(&dev.bt_addr, bt_addr);

		printf(
				"bluealsa:DEV=%s,PROFILE=%s,SRV=%s\n"
				"    %s, %s%s, %s\n"
				"    %s (%s): %s %d channel%s %d Hz\n",
				bt_addr,
				ba_pcm_get_profile_tag(pcm),
				dbus_ba_service,
				dev.name,
				dev.trusted ? "trusted " : "", dev.icon,
				pcm->mode == BA_PCM_MODE_SINK ? "playback" : "capture",
				ba_pcm_get_profile_name(pcm),
				pcm->codec.name,
				snd_pcm_format_name(bluealsa_get_snd_pcm_format(pcm)),
				pcm->channels, pcm->channels != 1 ? "s" : "",
				pcm->rate);

	}

}

static struct ba_pcm *ba_pcm_add(const struct ba_pcm *pcm) {
	struct ba_pcm *tmp;
	if ((tmp = realloc(ba_pcms, (ba_pcms_count + 1) * sizeof(*ba_pcms))) == NULL)
		return NULL;
	ba_pcms = tmp;
	memcpy(&ba_pcms[ba_pcms_count], pcm, sizeof(*ba_pcms));
	return &ba_pcms[ba_pcms_count++];
}

static struct ba_pcm *ba_pcm_get(const char *path) {
	for (size_t i = 0; i < ba_pcms_count; i++)
		if (strcmp(ba_pcms[i].pcm_path, path) == 0)
			return &ba_pcms[i];
	return NULL;
}

static void ba_pcm_remove(const char *path) {
	for (size_t i = 0; i < ba_pcms_count; i++)
		if (strcmp(ba_pcms[i].pcm_path, path) == 0) {
			memmove(&ba_pcms[i], &ba_pcms[i + 1],
					(ba_pcms_count - i - 1) * sizeof(*ba_pcms));
			ba_pcms_count--;
			break;
		}
}

static bool supervise_io_worker(const struct ba_pcm *ba_pcm) {

	assert(ba_pcm != NULL);

	if (ba_pcm->mode != BA_PCM_MODE_SOURCE)
		goto stop;

	if (!(ba_profiles[PROFILE_A2DP] && (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP)) &&
			!(ba_profiles[PROFILE_ASHA] && (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_ASHA)) &&
			!(ba_profiles[PROFILE_SCO] && (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO)))
		goto stop;

	/* Check whether SCO has selected codec. */
	if (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO &&
			ba_pcm->rate == 0) {
		debug("Skipping SCO with codec not selected");
		goto stop;
	}

	if (ba_addr_any)
		goto start;

	for (size_t i = 0; i < ba_addrs_count; i++)
		if (bacmp(&ba_addrs[i], &ba_pcm->addr) == 0)
			goto start;

stop:
	io_worker_stop(ba_pcm);
	return false;
start:
	io_worker_start(ba_pcm);
	return true;
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;

	if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) == 0) {

		if (strcmp(signal, "InterfacesAdded") == 0) {
			if (!dbus_message_iter_init(message, &iter))
				goto fail;
			struct ba_pcm pcm;
			DBusError err = DBUS_ERROR_INIT;
			if (!dbus_message_iter_get_ba_pcm(&iter, &err, &pcm)) {
				error("Couldn't add new BlueALSA PCM: %s", err.message);
				dbus_error_free(&err);
				goto fail;
			}
			if (pcm.transport == BA_PCM_TRANSPORT_NONE)
				goto fail;
			if (ba_pcm_add(&pcm) == NULL) {
				error("Couldn't add new BlueALSA PCM: %s", strerror(errno));
				goto fail;
			}
			supervise_io_worker(&pcm);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (strcmp(signal, "InterfacesRemoved") == 0) {
			if (!dbus_message_iter_init(message, &iter) ||
					dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
				error("Couldn't remove BlueALSA PCM: %s", "Invalid signal signature");
				goto fail;
			}
			dbus_message_iter_get_basic(&iter, &path);
			struct ba_pcm *pcm;
			if ((pcm = ba_pcm_get(path)) == NULL)
				goto fail;
			io_worker_stop(pcm);
			ba_pcm_remove(path);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

	}

	if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0) {
		struct ba_pcm *pcm;
		if ((pcm = ba_pcm_get(path)) == NULL)
			goto fail;
		if (!dbus_message_iter_init(message, &iter) ||
				dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
			error("Couldn't update BlueALSA PCM: %s", "Invalid signal signature");
			goto fail;
		}
		dbus_message_iter_get_basic(&iter, &interface);
		dbus_message_iter_next(&iter);
		if (!dbus_message_iter_get_ba_pcm_props(&iter, NULL, pcm))
			goto fail;

		supervise_io_worker(pcm);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(int argc, char *argv[]) {

	int opt;
	const char * opts = "hVSvlLB:D:M:p:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "syslog", no_argument, NULL, 'S' },
		{ "loglevel", required_argument, NULL, 9 },
		{ "verbose", no_argument, NULL, 'v' },
		{ "list-devices", no_argument, NULL, 'l' },
		{ "list-pcms", no_argument, NULL, 'L' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "pcm", required_argument, NULL, 'D' },
		{ "pcm-buffer-time", required_argument, NULL, 3 },
		{ "pcm-period-time", required_argument, NULL, 4 },
		{ "volume", required_argument, NULL, 8 },
		{ "mixer-device", required_argument, NULL, 'M' },
		{ "mixer-control", required_argument, NULL, 6 },
		{ "mixer-index", required_argument, NULL, 7 },
		{ "profile", required_argument, NULL, 'p' },
#if WITH_LIBSAMPLERATE
		{ "resampler", required_argument, NULL, 10},
#endif
		{ "single-audio", no_argument, NULL, 5 },
		{ 0, 0, 0, 0 },
	};

	static const nv_entry_t nv_log_levels[] = {
		{ "error", .v.i = LOG_ERR },
		{ "warning", .v.i = LOG_WARNING },
		{ "info", .v.i = LOG_INFO },
		{ "debug", .v.i = LOG_DEBUG },
		{ 0 },
	};

	static const nv_entry_t nv_profile_types[] = {
		{ "A2DP", .v.u = PROFILE_A2DP },
		{ "ASHA", .v.u = PROFILE_ASHA },
		{ "SCO", .v.u = PROFILE_SCO },
		{ 0 },
	};

	static const nv_entry_t nv_volume_types[] = {
		{ "auto", .v.u = VOL_TYPE_AUTO },
		{ "mixer", .v.u = VOL_TYPE_MIXER },
		{ "software", .v.u = VOL_TYPE_SOFTWARE },
		{ "none", .v.u = VOL_TYPE_NONE },
		{ 0 },
	};

#if WITH_LIBSAMPLERATE
	static const nv_entry_t nv_resampler_methods[] = {
		{ "sinc-best", .v.u = RESAMPLER_CONV_SINC_BEST_QUALITY },
		{ "sinc-medium", .v.u = RESAMPLER_CONV_SINC_MEDIUM_QUALITY },
		{ "sinc-fastest", .v.u = RESAMPLER_CONV_SINC_FASTEST },
		{ "linear", .v.u = RESAMPLER_CONV_LINEAR },
		{ "zero-order-hold", .v.u = RESAMPLER_CONV_ZERO_ORDER_HOLD },
		{ "none", .v.u = RESAMPLER_CONV_NONE },
		{ 0 },
	};
#endif

	bool syslog = false;
	bool profile_set = false;

	/* Check if syslog forwarding has been enabled. This check has to be
	 * done before anything else, so we can log early stage warnings and
	 * errors. */
	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("Usage:\n"
					"  %s [OPTION]... [BT-ADDR]...\n"
					"\nOptions:\n"
					"  -h, --help\t\t\tprint this help and exit\n"
					"  -V, --version\t\t\tprint version and exit\n"
					"  -S, --syslog\t\t\tsend logs to the system logger\n"
					"      --loglevel=LEVEL\t\tset logging level; default: %s\n"
					"  -v, --verbose\t\t\tincrease output verbosity\n"
					"  -l, --list-devices\t\tlist available BT audio devices\n"
					"  -L, --list-pcms\t\tlist available BT audio PCMs\n"
					"  -B, --dbus=NAME\t\tBlueALSA D-Bus service name suffix\n"
					"  -D, --pcm=NAME\t\tplayback PCM device to use; default: %s\n"
					"      --pcm-buffer-time=SEC\tplayback PCM buffer time in us; default: %u\n"
					"      --pcm-period-time=SEC\tplayback PCM period time in us; default: %u\n"
					"      --volume=TYPE\t\tset volume control type; default: %s\n"
					"  -M, --mixer-device=NAME\tmixer device to use; default: %s\n"
					"      --mixer-control=NAME\tmixer control name; default: %s\n"
					"      --mixer-index=NUM\t\tmixer element index; default: %u\n"
					"  -p, --profile=TYPE\t\tset profile to handle; default: A2DP\n"
#if WITH_LIBSAMPLERATE
					"      --resampler=METHOD\tresample conversion method; default: %s\n"
#endif
					"      --single-audio\t\tenable single audio mode\n"
					"%s"
					"\nNote:\n"
					"If one wants to receive audio from more than one Bluetooth device, it is\n"
					"possible to specify more than one MAC address. By specifying any/empty MAC\n"
					"address (00:00:00:00:00:00), one will allow connections from any Bluetooth\n"
					"device. Without given explicit MAC address any/empty MAC is assumed.\n",
					argv[0],
					nv_name_from_int(nv_log_levels, log_level),
					config.pcm_device,
					config.pcm_buffer_time,
					config.pcm_period_time,
					nv_name_from_uint(nv_volume_types, config.volume_type),
					config.mixer_device,
					config.mixer_elem_name,
					config.mixer_elem_index,
#if WITH_LIBSAMPLERATE
					nv_name_from_uint(nv_resampler_methods, config.resampler_method),
#endif
					"");
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'S' /* --syslog */ :
			syslog = true;
			break;

		case 'v' /* --verbose */ :
			config.verbose++;
			break;
		}

	log_open(basename(argv[0]), syslog);
	dbus_threads_init_default();

	/* parse options */
	optind = 0; opterr = 1;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
		case 'V' /* --version */ :
		case 'S' /* --syslog */ :
		case 'v' /* --verbose */ :
			break;

		case 9 /* --loglevel=LEVEL */ : {
			const nv_entry_t * entry;
			if ((entry = nv_lookup_entry(nv_log_levels, optarg)) == NULL) {
				error("Invalid loglevel {%s}: %s", nv_join_names(nv_log_levels), optarg);
				return EXIT_FAILURE;
			}
			log_level = entry->v.i;
		} break;

		case 'l' /* --list-devices */ :
			list_bt_devices = true;
			break;
		case 'L' /* --list-pcms */ :
			list_bt_pcms = true;
			break;

		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			if (!dbus_validate_bus_name(dbus_ba_service, NULL)) {
				error("Invalid BlueALSA D-Bus service name: %s", dbus_ba_service);
				return EXIT_FAILURE;
			}
			break;

		case 'D' /* --pcm=NAME */ :
			config.pcm_device = optarg;
			break;
		case 3 /* --pcm-buffer-time=SEC */ :
			config.pcm_buffer_time = atoi(optarg);
			break;
		case 4 /* --pcm-period-time=SEC */ :
			config.pcm_period_time = atoi(optarg);
			break;

		case 'p' /* --profile=TYPE */ : {
			const nv_entry_t * entry;
			if ((entry = nv_lookup_entry(nv_profile_types, optarg)) == NULL) {
				error("Invalid profile type {%s}: %s",
						nv_join_names(nv_profile_types), optarg);
				return EXIT_FAILURE;
			}
			ba_profiles[entry->v.u] = true;
			profile_set = true;
		} break;

		case 8 /* --volume */ : {
			const nv_entry_t * entry;
			if ((entry = nv_lookup_entry(nv_volume_types, optarg)) == NULL) {
				error("Invalid volume control type {%s}: %s",
						nv_join_names(nv_volume_types), optarg);
				return EXIT_FAILURE;
			}
			config.volume_type = entry->v.u;
		} break;

		case 'M' /* --mixer-device=NAME */ :
			config.mixer_device = optarg;
			break;
		case 6 /* --mixer-control=NAME */ :
			config.mixer_elem_name = optarg;
			break;
		case 7 /* --mixer-index=NUM */ :
			config.mixer_elem_index = atoi(optarg);
			break;

		case 5 /* --single-audio */ :
			config.force_single_playback = true;
			break;

#if WITH_LIBSAMPLERATE
		case 10 /* --resampler */ : {
			const nv_entry_t * entry;
			if ((entry = nv_lookup_entry(nv_resampler_methods, optarg)) == NULL) {
				error("Invalid resampler method {%s}: %s",
						nv_join_names(nv_resampler_methods), optarg);
				return EXIT_FAILURE;
			}
			config.resampler_method = entry->v.u;
		} break;
#endif

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if ((config.main_loop_quit_event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		error("Couldn't create quit event: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_connection_ctx_init(&config.dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	if (list_bt_devices || list_bt_pcms) {

		if (!ba_dbus_pcm_get_all(&config.dbus_ctx, &ba_pcms, &ba_pcms_count, &err)) {
			warn("Couldn't get BlueALSA PCM list: %s", err.message);
			return EXIT_FAILURE;
		}

		if (list_bt_pcms)
			print_bt_pcm_list();

		if (list_bt_devices)
			print_bt_device_list();

		return EXIT_SUCCESS;
	}

	if (!profile_set)
		ba_profiles[PROFILE_A2DP] = true;

	if (optind == argc)
		ba_addr_any = true;
	else if (parse_bt_addresses(&argv[optind], argc - optind) == -1) {
		error("Couldn't parse BT addresses: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	if (config.volume_type == VOL_TYPE_NONE || config.volume_type == VOL_TYPE_SOFTWARE)
		config.mixer_device = NULL;

	if (config.pcm_buffer_time == 0) {
		if (config.pcm_period_time == 0)
			config.pcm_period_time = ba_profiles[PROFILE_A2DP] ?
				DEFAULT_PERIOD_TIME_A2DP : DEFAULT_PERIOD_TIME_SCO;
		config.pcm_buffer_time = config.pcm_period_time * DEFAULT_PERIODS;
	}
	else if (config.pcm_period_time == 0) {
		config.pcm_period_time = config.pcm_buffer_time / DEFAULT_PERIODS;
	}

	if (config.verbose >= 1) {

		char * ba_profiles_str = malloc(8 * ARRAYSIZE(ba_profiles) + 1);
		char * ba_addrs_str = malloc(19 * ba_addrs_count + 1);
		char * tmp;

		tmp = ba_profiles_str;
		for (size_t i = 0; i < ARRAYSIZE(ba_profiles); i++)
			if (ba_profiles[i])
				tmp = stpcpy(stpcpy(tmp, ", "), profile2str(i));

		tmp = ba_addrs_str;
		for (size_t i = 0; i < ba_addrs_count; i++, tmp += 19)
			ba2str(&ba_addrs[i], stpcpy(tmp, ", "));

		const char *mixer_device_str = "(not used)";
		char mixer_element_str[128] = "(not used)";
		if (config.mixer_device != NULL) {
			mixer_device_str = config.mixer_device;
			snprintf(mixer_element_str, sizeof(mixer_element_str), "'%s',%u",
					config.mixer_elem_name, config.mixer_elem_index);
		}

		info("Selected configuration:\n"
				"  BlueALSA service: %s\n"
				"  ALSA PCM device: %s\n"
				"  ALSA PCM buffer time: %u us\n"
				"  ALSA PCM period time: %u us\n"
				"  ALSA mixer device: %s\n"
				"  ALSA mixer element: %s\n"
#if WITH_LIBSAMPLERATE
				"  Resampler method: %s\n"
#endif
				"  Volume control type: %s\n"
				"  Bluetooth device(s): %s\n"
				"  Profile(s): %s",
				dbus_ba_service,
				config.pcm_device,
				config.pcm_buffer_time,
				config.pcm_period_time,
				mixer_device_str,
				mixer_element_str,
#if WITH_LIBSAMPLERATE
				nv_name_from_uint(nv_resampler_methods, config.resampler_method),
#endif
				nv_name_from_uint(nv_volume_types, config.volume_type),
				ba_addr_any ? "ANY" : &ba_addrs_str[2],
				&ba_profiles_str[2]);

		free(ba_profiles_str);
		free(ba_addrs_str);

	}

	ba_dbus_connection_signal_match_add(&config.dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded",
			"path_namespace='/org/bluealsa'");
	ba_dbus_connection_signal_match_add(&config.dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved",
			"path_namespace='/org/bluealsa'");
	ba_dbus_connection_signal_match_add(&config.dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if (!dbus_connection_add_filter(config.dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add D-Bus filter: %s", err.message);
		return EXIT_FAILURE;
	}

	/* Open the ALSA mixer before opening any PCMs. */
	if (!(config.volume_type == VOL_TYPE_SOFTWARE || config.volume_type == VOL_TYPE_NONE)) {
		debug("Opening ALSA mixer");
		alsa_mixer_init(io_worker_mixer_event_callback);
		char *msg = NULL;
		if (alsa_mixer_open(&msg) != 0) {
			warn("Couldn't open ALSA mixer: %s", msg);
			free(msg);
		}
	}

	if (!ba_dbus_pcm_get_all(&config.dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
		warn("Couldn't get BlueALSA PCM list: %s", err.message);

	for (size_t i = 0; i < ba_pcms_count; i++)
		supervise_io_worker(&ba_pcms[i]);

	struct sigaction sigact = {
		.sa_handler = main_loop_stop,
		.sa_flags = SA_RESETHAND };
	/* Call to these handlers restores the default action, so on the
	 * second call the program will be forcefully terminated. */
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	debug("Starting main loop");
	for (;;) {

		struct pollfd fds[16] = {
			{ config.main_loop_quit_event_fd, POLLIN, 0 } };
		nfds_t fd_count = 1;
		nfds_t avail_fds = ARRAYSIZE(fds) - fd_count;

		nfds_t dbus_fds = avail_fds;
		if (!ba_dbus_connection_poll_fds(&config.dbus_ctx, &fds[fd_count], &dbus_fds)) {
			error("Couldn't get D-Bus connection file descriptors");
			return EXIT_FAILURE;
		}
		avail_fds -= dbus_fds;
		fd_count += dbus_fds;

		if (alsa_mixer_is_open()) {
			nfds_t alsa_fds;
			alsa_fds = alsa_mixer_poll_descriptors_count();
			if (alsa_fds > avail_fds ||
					alsa_mixer_poll_descriptors(&fds[fd_count], alsa_fds) <= 0) {
				error("Couldn't get ALSA mixer file descriptors");
				alsa_mixer_close();
			}
			if (alsa_mixer_is_open())
				fd_count += alsa_fds;
		}

		if (poll(fds, fd_count, -1) == -1 &&
				errno == EINTR)
			continue;

		if (fds[0].revents & POLLIN)
			break;

		if (ba_dbus_connection_poll_dispatch(&config.dbus_ctx, &fds[1], dbus_fds))
			while (dbus_connection_dispatch(config.dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
				continue;

		if (alsa_mixer_is_open())
			alsa_mixer_handle_events();
	}

	io_worker_cleanup();

	ba_dbus_connection_ctx_free(&config.dbus_ctx);
	return EXIT_SUCCESS;
}
