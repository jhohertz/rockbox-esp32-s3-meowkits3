/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Christian Gmeiner
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef USB_SERIAL_H
#define USB_SERIAL_H

#include "usb_class_driver.h"

void usb_serial_send(const unsigned char *data, int length);

/* Optional target hook, called with whatever arrived on the serial class's OUT
 * endpoint. Defined weak in usb_serial.c with an empty body, so a target that has no
 * use for received bytes links the no-op and behaves exactly as it did before this
 * existed -- the core has never dispatched received data to anybody.
 *
 * Runs in interrupt context: usb_core_transfer_complete() is reached from the device
 * driver's handler. A target that wants to act on a command records it here and acts
 * from a thread or a tick task, not inside this call. */
void usb_serial_target_rx(const unsigned char *data, int length);

extern struct usb_class_driver usb_cdrv_serial;

#endif
