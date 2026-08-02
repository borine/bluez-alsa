/*
 * BlueALSA - player.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */


#include "player.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#include "shared/dbus-client.h"
#include "shared/log.h"

struct media_player {
	/* BlueZ D-Bus device path */
	char device_path[128];
	/* Player path */
	char player_path[128];
};

static struct media_player *media_players = NULL;
static size_t media_players_count = 0;
static pthread_mutex_t players_mutex = PTHREAD_MUTEX_INITIALIZER;

static dbus_bool_t player_add(const struct media_player *player) {
	struct media_player *tmp;
	bool result = FALSE;

	pthread_mutex_lock(&players_mutex);
	if ((tmp = realloc(media_players, (media_players_count + 1) * sizeof(*media_players))) == NULL)
		goto finish;
	media_players = tmp;
	memcpy(&media_players[media_players_count++], player, sizeof(*media_players));
	debug("added player %s for device %s", player->player_path, player->device_path);
	result = TRUE;

finish:
	pthread_mutex_unlock(&players_mutex);
	return result;
}

static dbus_bool_t player_remove(const char *path) {
	dbus_bool_t found = FALSE;

	pthread_mutex_lock(&players_mutex);
	for (size_t i = 0; i < media_players_count; i++)
		if (strcmp(media_players[i].player_path, path) == 0) {
			debug("removed player %s for device %s", path, media_players[i].device_path);
			memmove(&media_players[i], &media_players[i + 1],
					(media_players_count - i - 1) * sizeof(*media_players));
			media_players_count--;
			found = TRUE;
			break;
		}
	pthread_mutex_unlock(&players_mutex);
	return found;
}

/**
 * Callback function for media player parser. */
static dbus_bool_t dbus_message_iter_get_player_props_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	struct media_player *player = (struct media_player *)userdata;

	char type;
	if ((type = dbus_message_iter_get_arg_type(value)) != DBUS_TYPE_VARIANT) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
				"Incorrect property value type: %c != %c", type, DBUS_TYPE_VARIANT);
		return FALSE;
	}

	DBusMessageIter variant;
	dbus_message_iter_recurse(value, &variant);
	type = dbus_message_iter_get_arg_type(&variant);

	char type_expected;
	const char *tmp;

	if (strcmp(key, "Device") == 0) {
		if (type != (type_expected = DBUS_TYPE_OBJECT_PATH))
			goto fail;
		dbus_message_iter_get_basic(&variant, &tmp);
		strncpy(player->device_path, tmp, sizeof(player->device_path) - 1);
	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Parse BlueALSA PCM properties. */
dbus_bool_t dbus_message_iter_get_player_props(
		DBusMessageIter *iter,
		DBusError *error,
		struct media_player *player) {
	return dbus_message_iter_dict(iter, error,
			dbus_message_iter_get_player_props_cb, player);
}

/**
 * Parse player. */
static dbus_bool_t dbus_message_iter_get_player(
		DBusMessageIter *iter,
		DBusError *error,
		struct media_player *player) {

	const char *path;
	char *signature;

	memset(player, 0, sizeof(*player));

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_OBJECT_PATH)
		goto fail;
	dbus_message_iter_get_basic(iter, &path);

	if (!dbus_message_iter_next(iter))
		goto fail;

	DBusMessageIter iter_ifaces;
	for (dbus_message_iter_recurse(iter, &iter_ifaces);
			dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_ifaces)) {

		if (dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_DICT_ENTRY)
			goto fail;

		DBusMessageIter iter_iface_entry;
		dbus_message_iter_recurse(&iter_ifaces, &iter_iface_entry);

		const char *iface_name;
		if (dbus_message_iter_get_arg_type(&iter_iface_entry) != DBUS_TYPE_STRING)
			goto fail;
		dbus_message_iter_get_basic(&iter_iface_entry, &iface_name);

		if (strcmp(iface_name, "org.bluez.MediaPlayer1") == 0) {

			strncpy(player->player_path, path, sizeof(player->player_path) - 1);

			if (!dbus_message_iter_next(&iter_iface_entry))
				goto fail;

			DBusError err = DBUS_ERROR_INIT;
			if (!dbus_message_iter_get_player_props(&iter_iface_entry, &err, player)) {
				dbus_set_error(error, err.name, "Get properties: %s", err.message);
				dbus_error_free(&err);
				return FALSE;
			}

			break;
		}

	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != oa{sa{sv}}", signature);
	dbus_free(signature);
	return FALSE;
}

static dbus_bool_t player_get_all(
		DBusConnection *conn,
		struct media_player **players,
		size_t *length,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call("org.bluez", "/",
					DBUS_INTERFACE_OBJECT_MANAGER, "GetManagedObjects")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	dbus_bool_t rv = TRUE;
	struct media_player *_players = NULL;
	size_t _length = 0;

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	DBusMessageIter iter_objects;
	for (dbus_message_iter_recurse(&iter, &iter_objects);
			dbus_message_iter_get_arg_type(&iter_objects) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_objects)) {

		if (dbus_message_iter_get_arg_type(&iter_objects) != DBUS_TYPE_DICT_ENTRY) {
			char *signature = dbus_message_iter_get_signature(&iter);
			dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
					"Incorrect signature: %s != a{oa{sa{sv}}}", signature);
			dbus_free(signature);
			goto fail;
		}

		DBusMessageIter iter_object_entry;
		dbus_message_iter_recurse(&iter_objects, &iter_object_entry);

		struct media_player player;
		DBusError err = DBUS_ERROR_INIT;
		if (!dbus_message_iter_get_player(&iter_object_entry, &err, &player)) {
			dbus_set_error(error, err.name, "Get player: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}

		if (player.player_path[0] == 0)
			continue;

		struct media_player *tmp = _players;
		if ((tmp = realloc(tmp, (_length + 1) * sizeof(*tmp))) == NULL) {
			dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}

		_players = tmp;

		memcpy(&_players[_length++], &player, sizeof(*_players));
		debug("Added player %s for device %s", player.player_path, player.device_path);
	}

	*players = _players;
	*length = _length;

	goto success;

fail:
	free(_players);
	rv = FALSE;

success:
	if (rep != NULL)
		dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

static DBusHandlerResult player_dbus_signal_handler(DBusConnection *conn,
			DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;

	if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(signal, "InterfacesAdded") == 0) {
		struct media_player	player;
		DBusError err = DBUS_ERROR_INIT;

		if (!dbus_message_iter_init(message, &iter))
			goto fail;

		if (!dbus_message_iter_get_player(&iter, &err, &player)) {
			error("Couldn't add new media player: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}
		if (player.player_path[0] == 0)
			goto fail;

		if (!player_add(&player)) {
			error("Couldn't add new BlueALSA PCM: %s", strerror(errno));
			goto fail;
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(signal, "InterfacesRemoved") == 0) {
		if (!dbus_message_iter_init(message, &iter) ||
				dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
			error("Couldn't remove player: %s", "Invalid signal signature");
			goto fail;
		}
		dbus_message_iter_get_basic(&iter, &path);
		if (!player_remove(path))
			goto fail;

		return DBUS_HANDLER_RESULT_HANDLED;
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

dbus_bool_t player_init(struct ba_dbus_ctx *ctx, DBusError *err) {
	ba_dbus_connection_signal_match_add(ctx,
			"org.bluez", NULL, DBUS_INTERFACE_OBJECT_MANAGER,
			"InterfacesAdded", NULL);

	ba_dbus_connection_signal_match_add(ctx,
			"org.bluez", NULL, DBUS_INTERFACE_OBJECT_MANAGER,
			"InterfacesRemoved", NULL);

	if (!dbus_connection_add_filter(ctx->conn,
				player_dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add D-Bus filter for media players");
		return FALSE;
	}

	if (!player_get_all(ctx->conn, &media_players, &media_players_count, err)) {
		error("Couldn't get media players");
		return FALSE;
	}

	return TRUE;
}

dbus_bool_t player_pause(const char *device_path, DBusConnection *conn) {
	char *player_path = NULL;

	pthread_mutex_lock(&players_mutex);

	for (size_t i = 0; i < media_players_count; i++)
		if (strcmp(media_players[i].device_path, device_path) == 0) {
			player_path = strdup(media_players[i].player_path);
			break;
		}

	pthread_mutex_unlock(&players_mutex);

	if (player_path == NULL)
		return FALSE;

	dbus_bool_t ret = FALSE;
	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;

	msg = dbus_message_new_method_call("org.bluez", player_path, "org.bluez.MediaPlayer1", "Pause");
	if (msg == NULL) {
		error("Cannot create Pause message");
		goto finish;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(conn, msg,
					DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		warn("Couldn't pause player: %s", err.message);
		dbus_error_free(&err);
		goto finish;
	}

	ret = TRUE;
	debug("Requested playback pause for player %s", player_path);

finish:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	free(player_path);
	return ret;
}
