/*
 * BlueALSA - hci-usb.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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

unsigned hci_usb_get_mtu(int fd, const struct ba_adapter *a);

#endif
