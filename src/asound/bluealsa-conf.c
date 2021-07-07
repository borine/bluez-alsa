/*
 * bluealsa-conf.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <alsa/asoundlib.h>
#include <alsa/conf.h>
#include <alsa/pcm.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "../utils/aplay/dbus.h"

/**
 * a tag to easily identify alsa namehint entries created by this module */
#define BLUEALSA_CONF_PREFIX "__bluealsa"

/**
 * create a NULL-terminated array of active D-Bus bluealsa service names */
static int bluealsa_conf_get_services(struct DBusConnection *dbus_conn, char ***services, DBusError *err) {

	DBusMessage *msg = NULL, *rep = NULL;
	int ret = -1;

	if ((msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
					DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames")) == NULL) {
		dbus_set_error(err, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, err)) == NULL) {
		goto fail;
	}

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(err, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	/* It is unlikely that there will be more than 1 or 2 bluealsa services
	 * running, so we initially allocate space for just 2 names, and increase
	 * the allocation in increments of 2 if necessary. */
	char **name_list = malloc(3 * sizeof(char*));
	int array_size = 2;
	int name_count = 0;
	DBusMessageIter iter_names;
	for (dbus_message_iter_recurse(&iter, &iter_names);
			dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_names)) {

		if (dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_STRING) {
			char *signature = dbus_message_iter_get_signature(&iter);
			dbus_set_error(err, DBUS_ERROR_INVALID_SIGNATURE,
					"Incorrect signature: %s != as", signature);
			dbus_free(signature);
			goto fail;
		}

		const char *name;
		char **tmp;
		dbus_message_iter_get_basic(&iter_names, &name);
		if (strncmp(name, BLUEALSA_SERVICE, sizeof(BLUEALSA_SERVICE) - 1) == 0) {
			if (name_count >= array_size) {
				if ((tmp = realloc(name_list, array_size + 2)) == NULL) {
					dbus_set_error(err, DBUS_ERROR_NO_MEMORY, NULL);
					free(name_list);
					goto fail;
				}
				name_list = tmp;
				array_size += 2;
			}
			if ((name_list[name_count++] = strdup(name)) == NULL) {
				dbus_set_error(err, DBUS_ERROR_NO_MEMORY, NULL);
				free(name_list);
				goto fail;
			}
		}
	}
	name_list[name_count] = NULL;
	*services = name_list;
	ret = 0;

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return ret;
}

/**
 * Find the alsa config node for namehint pcm entries */
static int bluealsa_conf_get_pcm_namehints(snd_config_t *root, snd_config_t **hint_pcm_node) {
	snd_config_t *hint_root, *node;
	int ret;

	if ((ret = snd_config_search(root, "namehint", &hint_root)) >= 0) {
		if (snd_config_get_type(hint_root) != SND_CONFIG_TYPE_COMPOUND) {
			SNDERR("*** Invalid root namehint node");
			return -EINVAL;
		}
	}
	else {
		if ((ret = snd_config_make_compound(&hint_root, "namehint", 0)) < 0)
			return ret;
		if ((ret = snd_config_add(root, hint_root)) < 0) {
			snd_config_delete(hint_root);
			return ret;
		}
	}

	if ((ret = snd_config_search(hint_root, "pcm", &node)) < 0) {
		if ((ret = snd_config_make_compound(&node, "pcm", 0)) < 0)
			return ret;
		if ((ret = snd_config_add(hint_root, node)) < 0) {
			snd_config_delete(node);
			return ret;
		}
	}

	*hint_pcm_node = node;
	return 0;
}

/* Create a new ALSA configuration namehint node for the given pcm */
static int bluealsa_conf_add_pcm(snd_config_t *hint_pcm_node, struct ba_pcm *pcm, const char *name) {
	snd_config_t *pcm_node;
	snd_config_t *node, *n;
	int ret = 0;

	char bt_addr[18];
	ba2str(&pcm->addr, bt_addr);


	char buffer[256];
	snprintf(buffer, sizeof(buffer),
			"bluealsa:DEV=%s,PROFILE=%s,SRV=%s|DESC%s, %s (%s)\n"
			"Bluetooth Audio %s|IOID%s",
		bt_addr,
		pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "a2dp" : "sco",
		BLUEALSA_SERVICE,
		name,
		pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "A2DP" :
				pcm->transport & BA_PCM_TRANSPORT_MASK_HFP ? "HFP" :
				pcm->transport & BA_PCM_TRANSPORT_MASK_HSP ? "HSP" :
				"",
		pcm->codec,
		pcm->mode == BA_PCM_MODE_SINK ? "Output" : "Input",
		pcm->mode == BA_PCM_MODE_SINK ? "Output" : "Input");

	char id[sizeof(BLUEALSA_CONF_PREFIX) + 31];
	snprintf(id, sizeof(id), BLUEALSA_CONF_PREFIX "%s_%s_%s",
		bt_addr,
		pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "a2dp" : "sco",
		pcm->mode == BA_PCM_MODE_SINK ? "Playback" : "Capture");

	if (snd_config_search(hint_pcm_node, id, &n) == 0) {
		snd_config_delete(n);
	}

	if ((ret = snd_config_imake_string(&node, id, buffer)) < 0)
		return ret;

	if ((ret = snd_config_add(hint_pcm_node, node)) < 0) {
		snd_config_delete(node);
		return ret;
	}

	return ret;
}

/**
 * Fetch a list of all active pms from the given bluealsa service, and create a
 * new ALSA namehint entry for each. */
static int bluealsa_conf_get_pcms(struct ba_dbus_ctx *dbus_ctx, const char *service, snd_config_t *hint_pcm_node, DBusError *err) {
	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;
	int i, ret = 0;

	/* We first remove all existing bluealsa namehints */
	snd_config_iterator_t pos, next;
	snd_config_for_each(pos, next, hint_pcm_node) {
		const char *id;
		snd_config_t *child = snd_config_iterator_entry(pos);
		snd_config_get_id(child, &id);
		if (strncmp(id, BLUEALSA_CONF_PREFIX, sizeof(BLUEALSA_CONF_PREFIX) -1) == 0) {
			snd_config_delete(child);
		}
	}

	strncpy(dbus_ctx->ba_service, service, sizeof(dbus_ctx->ba_service) - 1);
	if (!bluealsa_dbus_get_pcms(dbus_ctx, &pcms, &pcms_count, err)) {
		ret = -EIO;
		goto fail;
	}

	struct bluez_device dev = { 0 };
	const char *path = "";
	for (i = 0; i < pcms_count; i++) {
		struct ba_pcm *pcm = &pcms[i];
		if (strcmp(pcm->device_path, path) != 0) {
			path = pcms[i].device_path;
			if (dbus_bluez_get_device(dbus_ctx->conn, pcm->device_path, &dev, err) == -1) {
				dbus_error_free(err);
			}
		}
		if ((ret = bluealsa_conf_add_pcm(hint_pcm_node, pcm, dev.name)) < 0) {
			goto fail;
		}
	}

fail:
	free(pcms);
	return ret;
}

/**
 * Update the ALSA user namehint nodes, in-memory, to include all active
 * BlueALSA pcms.
 * @param root Handle to the root source node
 * @param config Handle to the configuration node for this hook
 * @param dst The function always places NULL at this address.
 * @param private_data Unused.
 * @return Zero if successful, otherwise a negative error code.
 */
int
bluealsa_conf_hook_load(snd_config_t * root, snd_config_t * config,
				snd_config_t ** dst,
				snd_config_t * private_data) {
	(void) private_data;
	snd_config_t *hint_pcm_node;
	snd_config_t *n;
	snd_config_iterator_t i, next;
	int ret = 0, errors = 1;

	assert(root && dst);

	/* do not replace the original root ! */
	*dst = NULL;

	if ((ret = snd_config_search(config, "errors", &n)) >= 0) {
		char *tmp;
		ret = snd_config_get_ascii(n, &tmp);
		if (ret < 0)
			return ret;
		errors = snd_config_get_bool_ascii(tmp);
		free(tmp);
		if (errors < 0) {
			SNDERR("Invalid bool value in field errors");
			return errors;
		}
	}

	if ((ret = bluealsa_conf_get_pcm_namehints(root, &hint_pcm_node)) < 0)
		return ret;

	struct ba_dbus_ctx dbus_ctx;
	DBusError err = DBUS_ERROR_INIT;
	dbus_threads_init_default();
	if ((ret = bluealsa_dbus_connection_ctx_init(&dbus_ctx, BLUEALSA_SERVICE, &err)) < 0)
		goto fail;

	char **services;
	if ((ret = bluealsa_conf_get_services(dbus_ctx.conn, &services, &err))) {
		goto fail;
	}

	char **service_ptr;
	for (service_ptr = services; *service_ptr != NULL; service_ptr++) {
		if ((ret = bluealsa_conf_get_pcms(&dbus_ctx, *service_ptr, hint_pcm_node, &err)) < 0)
			goto fail;

	}

fail:
	if (errors && dbus_error_is_set(&err))
		SNDERR("%s", err.message);

	if (!errors)
		ret = 0;

	bluealsa_dbus_connection_ctx_free(&dbus_ctx);
	dbus_error_free(&err);

	return ret;
}

SND_DLSYM_BUILD_VERSION(bluealsa_conf_hook_load, SND_CONFIG_DLSYM_VERSION_HOOK);
