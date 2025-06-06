/*
 * BlueALSA - hci_usb.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2016-2025 borine
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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
		snprintf(ep_path, PATH_MAX, "%s/%s", interface, interface_ent->d_name);
#pragma GCC diagnostic pop
		DIR *ep_dir;
		if ((ep_dir = opendir(ep_path)) == NULL) {
			debug("Cannot open endpoint directory %s (%s)\n", ep_path, strerror(errno));
			continue;
		}

		struct dirent *ep_ent;
		while ((ep_ent = readdir(ep_dir))) {
			char file_path[PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
			snprintf(file_path, PATH_MAX, "%s/%s/type", interface, interface_ent->d_name);
#pragma GCC diagnostic pop
			FILE *file;
			if ((file = fopen(file_path, "r")) == NULL) {
				debug("Cannot open endpoint type %s (%s)\n", file_path, strerror(errno));
				continue;
			}

			char type[5] = {0};
			if (fread(type, 1, 4, file)) {
				/* Ignore the return value; either we read "Isoc" or
				 * we did not. */
			}
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
		int matched = fscanf(file, "%1u", &bAlternateSetting);
		fclose(file);
		if (matched != 1 || bAlternateSetting == 0)
			continue;

		snprintf(path, PATH_MAX, "%s/%s", dev_path, entry->d_name);
		if (hci_usb_interface_has_isoc_endpoint(path))
			break;

		bAlternateSetting = 0;
	}

	closedir(dir);
	return bAlternateSetting;
}

unsigned hci_usb_sco_get_mtu(const struct ba_adapter *a) {

	assert(a && (a->hci.type & 0x0F) == HCI_USB);

	/* The MTU depends on the USB interface descriptor "Alternate Setting".
	 * For alternate settings 1 to 5 each HCI frame is fragmented into 3 USB
	 * transfer packets, so the MTU is given by the formula
	 * (wMaxPacketSize * 3) - 3, where
	 * Alt-1 has wMaxPacketSize == 9
	 * Alt-2 has wMaxPacketSize == 17
	 * Alt-3 has wMaxPacketSize == 25
	 * Alt-4 has wMaxPacketSize == 33
	 * Alt-5 has wMaxPacketSize == 49
	 * and we must reserve 3 bytes for the HCI SCO frame header.
	 *
	 * For alternate setting 6 wMaxPacketSize == 63, and each HCI frame is sent
	 * as a single USB transfer. So the MTU is 60 bytes (which is one complete
	 * WBS or SWB frame) with 3 bytes for the HCI header.
	 *
	 * To get the active interface alternate setting, we look at the adapter
	 * device node within the Linux sysfs. Bluetooth creates a sysfs node
	 * "/sys/class/bluetooth/hciX/device" for each HCI. For USB adapters this
	 * node is a symbolic link to the control interface of the USB device node
	 * within the "/sys/devices" tree. The actual device node is the parent of
	 * that control interface node.
	 *
	 * Note that the Linux USB Bluetooth driver changes the alternate setting,
	 * on the fly, according to the number of active SCO streams as each HCI
	 * frame is processed. So this implementation only works correctly if no
	 * more than one active SCO stream per USB adapter is allowed. */
	char device_path[sizeof("/sys/class/bluetooth//device/..") + sizeof(a->hci.name)];
	sprintf(device_path, "/sys/class/bluetooth/%s/device/..", a->hci.name);

	unsigned mtu = 0;
	switch (hci_usb_get_alternate_setting(device_path)) {
	case 0:
		break;
	case 1:
		mtu = 24;
		break;
	case 2:
		mtu = 48;
		break;
	case 3:
		mtu = 72;
		break;
	case 4:
		mtu = 96;
		break;
	case 5:
		mtu = 144;
		break;
	case 6:
		mtu = 60;
		break;
	default:
		break;
	}

	return mtu;
}
