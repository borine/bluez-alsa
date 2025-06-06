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

#include "ba-adapter.h"

unsigned hci_usb_sco_get_mtu(const struct ba_adapter *a);

#endif
