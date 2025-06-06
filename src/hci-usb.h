/*
 * BlueALSA - hci-usb.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
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
