/*
 * BlueALSA - hci_usb.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include "hci-usb.h"
#include "shared/log.h"

/* IWYU pragma: no_include "config.h" */

static bool hci_usb_interface_has_isoc_endpoint(const char *interface) {

	bool has_isoc_ep = false;

	DIR *interface_dir;
	if ((interface_dir = opendir(interface)) == NULL) {
		debug("Cannot open interface directory %s (%s)\n", interface, strerror(errno));
		return has_isoc_ep;
	}

	struct dirent *interface_ent;
	while ((interface_ent = readdir(interface_dir))) {
		/* endpoint directory names all begin "ep_" */
		if (strncmp(interface_ent->d_name, "ep_", 3))
			continue;

		char ep_path[PATH_MAX];
		snprintf(ep_path, PATH_MAX, "%s/%s", interface, interface_ent->d_name);
		DIR *ep_dir;
		if ((ep_dir = opendir(ep_path)) == NULL) {
			debug("Cannot open endpoint directory %s (%s)\n", ep_path, strerror(errno));
			continue;
		}

		struct dirent *ep_ent;
		while ((ep_ent = readdir(ep_dir))) {
			char file_path[PATH_MAX];
			snprintf(file_path, PATH_MAX, "%s/%s/%s", interface, interface_ent->d_name, "type");
			FILE *file;
			if ((file = fopen(file_path, "r")) == NULL) {
				debug("Cannot open endpoint type %s (%s)\n", file_path, strerror(errno));
				continue;
			}

			char type[5] = {0};
			fread(type, 1, 4, file);
			fclose(file);
			type[4] = 0;
			if (strcmp(type, "Isoc") == 0) {
				has_isoc_ep = true;
				break;
			}
		}

		closedir(ep_dir);
		if (has_isoc_ep)
			break;
	}

	closedir(interface_dir);
	return has_isoc_ep;
}

static unsigned hci_usb_get_alternate_setting(const char *dev_path) {

	unsigned bAlternateSetting = 0;

	/* The device id is the final component of the device path */
	char *devid = strrchr(dev_path, '/');
	if (devid == NULL || strlen(devid) < 1) {
		warn("Invalid USB device path [%s]\n", dev_path);
		return bAlternateSetting;
	}
	++devid;

	/* The device node is a directory containing a sub-directory for each
	 * interface and also many other items. btusb configures only one interface
	 * with isochronous endpoints, so we iterate through the device  node
	 * looking for that one interface, and if found return its
	 * bAlternateSetting value. */

	DIR *dir;
	if ((dir = opendir(dev_path)) == NULL) {
		debug("Cannot open device directory %s (%s)\n", dev_path, strerror(errno));
		return bAlternateSetting;
	}

	char path[PATH_MAX];
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		/* Interface directory names are distinguished by having a colon ':' */
		if (strchr(entry->d_name, ':') == NULL)
			continue;

		/* Check the alternate setting before searching for isochronous
		 * endpoints. If the setting is zero then this interface is not
		 * configured */
		snprintf(path, PATH_MAX, "%s/%s/%s", dev_path, entry->d_name, "bAlternateSetting");
		FILE *file;
		if ((file = fopen(path, "r")) == NULL) {
			debug("Cannot read bAlternateSetting %s (%s)\n", path, strerror(errno));
			continue;
		}
		fscanf(file, "%1u", &bAlternateSetting);
		fclose(file);
		if (bAlternateSetting == 0)
			continue;

		snprintf(path, PATH_MAX, "%s/%s/", dev_path, entry->d_name);
		if (hci_usb_interface_has_isoc_endpoint(path))
			break;

		bAlternateSetting = 0;
	}

	closedir(dir);
	return bAlternateSetting;
}

unsigned hci_usb_get_mtu(int fd, const struct ba_adapter *a) {

	assert(a && (a->hci.type & 0x0F) == HCI_USB);

	struct bt_voice voice = { 0 };
	socklen_t len = sizeof(voice);
	if (getsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, &len) == -1)
		warn("Couldn't get SCO voice options: %s", strerror(errno));

	/* btusb always requires HCI SCO message size of 48 bytes for CVSD */
	if (voice.setting != BT_VOICE_TRANSPARENT)
		return 48;

	/* Use 24 bytes as the default mtu for mSBC, since that value is known to
	 * work with the majority of adapters. */
	unsigned mtu = 24;

	/* For USB adapters, the SCO socket mtu depends on the selected interface
	 * alternate setting. The USB interface configuration alternate setting is
	 * not chosen until the first incoming USB packet is ready, so we need to
	 * wait for that to happen. We set a timeout of 100ms in case the BT
	 * controller fails to deliver any incoming data. */
	struct pollfd pfd = { fd, POLLIN, 0 };
	if (poll(&pfd, 1, 100) <= 0)
		warn("Couldn't wait for SCO connection: %s", strerror(errno));

	/* To get the interface alternate setting, we look at the adapter device
	 * node within the Linux sysfs. Bluetooth creates a sysfs node
	 * "/sys/class/bluetooth/hciX/device" for each HCI. For USB adapters this
	 * node is a symbolic link to the control interface of the USB device node
	 * within the "/sys/devices" tree. The actual device node is the parent of
	 * that interface node. */
	char device_path[strlen("/sys/class/bluetooth/hci000/device/..") + 1];
	sprintf(device_path, "/sys/class/bluetooth/%s/device/..", a->hci.name);

	/* btusb uses only three possible interface configuration alternate
	 * settings for mSBC: 1, 3, or 6. For alternate settings 1 and 3, the mtu
	 * is given by the formula (wMaxPacketSize * 3) - 3
	 * Alt-1 has wMaxPacketSize == 9
	 * Alt-3 has wMaxPacketSize == 25
	 * We must reserve 3 bytes for the HCI SCO frame header.
	 *
	 * For alternate setting 6 wMaxPacketSize == 63, so we use an mtu of 60
	 * bytes which is one complete mSBC frame leaving 3 bytes for the HCI
	 * header. */
	switch (hci_usb_get_alternate_setting(device_path)) {
	default:
		/* If an unexpected alternate setting value is returned then we fall
		 * back to the value that is most commonly used. */
		debug("No valid Isochronous endpoint found, assuming Alt-1\n");
		/* fallthrough */
	case 1:
		mtu = 24;
		break;
	case 3:
		mtu = 72;
		break;
	case 6:
		mtu = 60;
		break;
	}

	return mtu;
}
