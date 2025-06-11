/*
 * BlueALSA - hci-usb.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_HCI_USB_H_
#define BLUEALSA_HCI_USB_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>

#include "ba-adapter.h"

struct hci_usb_sco {
	struct ba_adapter *a;
	bool busy;
	pthread_mutex_t mutex;
};

struct hci_usb_sco *hci_usb_sco_new(struct ba_adapter *a);
void hci_usb_sco_delete(struct hci_usb_sco *h);

bool hci_usb_sco_grab(struct hci_usb_sco *h);
void hci_usb_sco_release(struct hci_usb_sco *h);

unsigned hci_usb_sco_get_mtu(const struct hci_usb_sco *h);

#endif
