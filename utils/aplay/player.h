/*
 * BlueALSA - player.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_PLAYER_H_
#define BLUEALSA_APLAY_PLAYER_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <dbus/dbus.h>
#include "shared/dbus-client.h"

dbus_bool_t player_init(struct ba_dbus_ctx *ctx, DBusError *err);
dbus_bool_t player_pause(const char *device_path, DBusConnection *conn);

#endif
