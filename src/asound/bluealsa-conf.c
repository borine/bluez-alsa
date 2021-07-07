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
 * default description template.
 *
 * possible substitution keys are:
 *	%a	bluetooth address
 *	%c	codec
 *	%n	device name (alias)
 *	%p	profile
 *	%s	stream direction ("Input" | "Output")
 *	%%	literal '%'
 */
#define BLUEALSA_CONF_TEMPLATE "%n %p (%c)\nBluetooth Audio %s"


struct bluealsa_config {
	snd_config_t *root;
	snd_config_t *namehint;
	char *pattern;
};

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
 * Find or create a base sub-tree by id. */
static int bluealsa_conf_get_sub_tree(snd_config_t *root, const char *id, snd_config_t **subtree) {
	snd_config_t *node;
	int ret;

	if ((ret = snd_config_search(root, id, &node)) >= 0) {
		if (snd_config_get_type(node) != SND_CONFIG_TYPE_COMPOUND) {
			SNDERR("Invalid root %s node", id);
			return -EINVAL;
		}
	}
	else {
		if ((ret = snd_config_make_compound(&node, id, 0)) < 0)
			return ret;
		if ((ret = snd_config_add(root, node)) < 0) {
			snd_config_delete(node);
			return ret;
		}
	}

	*subtree = node;
	return 0;
}

/**
 * Find or create a compound child node under the given parent node. */
static int bluealsa_conf_get_child(snd_config_t *parent, const char *id, snd_config_t **child) {
	snd_config_t *node;
	int ret;

	if ((ret = snd_config_search(parent, id, &node)) < 0) {
		if ((ret = snd_config_make_compound(&node, id, 0)) < 0)
			return ret;
		if ((ret = snd_config_add(parent, node)) < 0) {
			snd_config_delete(node);
			return ret;
		}
	}

	*child = node;
	return 0;
}

/**
 * Find the alsa config node for namehint pcm entries */
static int bluealsa_conf_get_pcm_namehints(snd_config_t *root, snd_config_t **hint_pcm_node) {
	snd_config_t *hint_root, *node;
	int ret;

	if ((ret = bluealsa_conf_get_sub_tree(root, "namehint", &hint_root)) < 0)
		return ret;

	if ((ret = bluealsa_conf_get_child(hint_root, "pcm", &node)) < 0)
		return ret;

	*hint_pcm_node = node;
	return 0;
}

/* Create a new ALSA configuration namehint node for the given pcm */
static int bluealsa_conf_add_namehint(struct bluealsa_config *config, struct ba_pcm *pcm, const char *name, const char *service) {
	snd_config_t *node, *n;
	int ret = 0;

	char bt_addr[18];
	ba2str(&pcm->addr, bt_addr);

	char buffer[256];
	const char *end = buffer + sizeof(buffer);
	int offset;
	char *pos;

	offset = snprintf(buffer, sizeof(buffer),
#if SND_LIB_VERSION >= 0x010204
			"bluealsa:DEV=%s,PROFILE=%s,SRV=%s|",
#elif SND_LIB_VERSION == 0x010203
	/* alsa-lib changed the parser for namehints in a bug-fix update to release
	 * v1.2.3 (v1.2.3.2) - but that final version component is not visible as a
	 * pre-processor macro; so we are forced to do a compile-time string
	 * comparison to know which syntax to use :( */
			(strcmp(SND_LIB_VERSION_STR, "1.2.3.2") ?
					"bluealsa:DEV=%s,PROFILE=%s,SRV=%s|DESC" :
					"bluealsa:DEV=%s,PROFILE=%s,SRV=%s|"),
#else
			"bluealsa:DEV=%s,PROFILE=%s,SRV=%s|DESC",
#endif
			bt_addr,
			pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "a2dp" : "sco",
			service);

	if (offset < 0 || (size_t)offset > sizeof(buffer))
		return -ENOMEM;

	pos = buffer + offset;

	char *p;
	for (p = config->pattern; *p != 0 && pos < end; p++) {
		if (*p == '%') {
			switch (*(++p)) {
			case 'a': /* address */ {
				size_t len = strlen(bt_addr);
				if ((pos + len) >= end)
					return -ENOMEM;
				strcpy(pos, bt_addr);
				pos += len;
				break;
			}
			case 'n': /* name (alias) */ {
				size_t len = strlen(name);
				if ((pos + len) >= end)
					return -ENOMEM;
				strcpy(pos, name);
				pos += len;
				break;
			}
			case 'p': /* profile */ {
				size_t len;
				const char *profile =
					pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "A2DP" :
					pcm->transport & BA_PCM_TRANSPORT_MASK_HFP ? "HFP" :
					pcm->transport & BA_PCM_TRANSPORT_MASK_HSP ? "HSP" :
					NULL;
				if (profile == NULL)
					return -EINVAL;
				len = strlen(profile);
				if ((pos + len) >=end)
					return -ENOMEM;
				strcpy(pos, profile);
				pos += len;
				break;
			}
			case 'c': /* codec */ {
				size_t len = strlen(pcm->codec);
				if ((pos + len) >= end)
					return -ENOMEM;
				strcpy(pos, pcm->codec);
				pos += len;
				break;
			}
			case 's': /* stream direction */ {
				size_t len;
				const char *stream =
						pcm->mode == BA_PCM_MODE_SINK ? "Output" : "Input";
				len = strlen(stream);
				if ((pos + len) >=end)
					return -ENOMEM;
				strcpy(pos, stream);
				pos += len;
				break;
			}
			case '%': /* literal percent */
				*pos++ = '%';
				break;
			default:
				*pos++ = *p;
			}
		}
		else
			*pos++ = *p;
	}

	if ((end - pos) > 12) {
		strcpy(pos, "|IOID");
		pos += 5;
		if (pcm->mode == BA_PCM_MODE_SINK)
			strcpy(pos, "Output");
		else
			strcpy(pos, "Input");
	}
	else
		*pos = '\0';

	char id[sizeof(BLUEALSA_CONF_PREFIX) + 31];
	snprintf(id, sizeof(id), BLUEALSA_CONF_PREFIX "%s_%s_%s",
		bt_addr,
		pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "a2dp" : "sco",
		pcm->mode == BA_PCM_MODE_SINK ? "Playback" : "Capture");

	if (snd_config_search(config->namehint, id, &n) == 0) {
		snd_config_delete(n);
	}

	if ((ret = snd_config_imake_string(&node, id, buffer)) < 0)
		return ret;

	if ((ret = snd_config_add(config->namehint, node)) < 0) {
		snd_config_delete(node);
		return ret;
	}

	return ret;
}

/**
 * Create ALSA config entries for each active pcm of the given bluealsa service. */
static int bluealsa_conf_add_service_pcms(struct ba_dbus_ctx *dbus_ctx, const char *service, struct bluealsa_config *config, DBusError *err) {
	struct ba_pcm *pcms = NULL;
	size_t i, pcms_count = 0;
	int ret = 0;

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

		if ((ret = bluealsa_conf_add_namehint(config, pcm, dev.name, service)) < 0)
			goto fail;
	}

fail:
	free(pcms);
	return ret;
}

/**
 * Update the ALSA config, in-memory, to include all active
 * BlueALSA pcms.
 * @param root Handle to the root node
 * @param hook_node Handle to the configuration node for this hook
 * @param dst Handle to updated root node.
 * @param private_data Unused.
 * @return Zero if successful, otherwise a negative error code.
 */
int bluealsa_conf_hook_namehints(snd_config_t *root, snd_config_t *hook_node,
				snd_config_t **dst,
				snd_config_t *private_data) {
	(void) private_data;
	(void) hook_node;
	struct bluealsa_config config = { 0 };
	snd_config_t *node;
	snd_config_iterator_t i, next;
	char **services = NULL;
	DBusError err = DBUS_ERROR_INIT;
	int ret = 0;

	assert(root && dst);
	*dst = NULL;

	/* Work on a copy of the root */
	if ((ret = snd_config_copy(&config.root, root)) < 0)
		return ret;

	/* perform namehint creation only if user config has enabled it. */
	if (snd_config_search(root, "defaults.bluealsa.namehint", &node) >= 0 &&
			snd_config_get_bool(node) > 0) {
		if ((ret = bluealsa_conf_get_pcm_namehints(config.root, &config.namehint)) < 0)
			goto fail_init;
	}

	if (config.namehint == NULL)
		goto fail_init;

	/* Get the description template. */
	if (snd_config_search(root, "defaults.bluealsa.description", &node) >= 0) {
		/* interpret the value as a custom pattern */
		const char *pattern;
		snd_config_get_string(node, &pattern);
		config.pattern = strdup(pattern);
	}
	if (config.pattern == NULL)
		config.pattern = strdup(BLUEALSA_CONF_TEMPLATE);

	/* First remove all existing bluealsa dynamic namehints */
	if (config.namehint) {
		snd_config_for_each(i, next, config.namehint) {
			const char *id;
			node = snd_config_iterator_entry(i);
			snd_config_get_id(node, &id);
			if (strncmp(id, BLUEALSA_CONF_PREFIX, sizeof(BLUEALSA_CONF_PREFIX) -1) == 0) {
				snd_config_delete(node);
			}
		}
	}

	/* Establish connection to D-Bus */
	struct ba_dbus_ctx dbus_ctx;
	dbus_threads_init_default();
	if ((ret = bluealsa_dbus_connection_ctx_init(&dbus_ctx, BLUEALSA_SERVICE, &err)) < 0)
		goto fail_init;

	/* Find all running BlueALSA services */
	if ((ret = bluealsa_conf_get_services(dbus_ctx.conn, &services, &err))) {
		goto fail;
	}

	/* Create namehints */
	char **service_ptr;
	for (service_ptr = services; *service_ptr != NULL; service_ptr++) {
		if ((ret = bluealsa_conf_add_service_pcms(&dbus_ctx, *service_ptr, &config, &err)) < 0)
			goto fail;
	}

	*dst = config.root;
	ret = 0;

fail:
	free(services);
	bluealsa_dbus_connection_ctx_free(&dbus_ctx);

fail_init:
	free(config.pattern);
	dbus_error_free(&err);
	if (config.root != *dst)
		snd_config_delete(config.root);

	return ret;
 }

SND_DLSYM_BUILD_VERSION(bluealsa_conf_hook_namehints, SND_CONFIG_DLSYM_VERSION_HOOK);
