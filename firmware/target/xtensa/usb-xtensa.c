/***************************************************************************
 * ESP32-S3 USB.
 *
 * The chip has a native USB-OTG peripheral and the reference firmware uses it as
 * CDC and MSC, but Rockbox's usbstack needs a device driver behind it and none
 * exists for this core yet. These entry points exist so that firmware/usb.c,
 * which is built unconditionally, links; nothing is attached.
 *
 * usb_detect() reporting USB_EXTRACTED is what makes the charging and MSC paths
 * inert. If the peripheral is ever implemented, this file is where it starts.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "usb.h"

void usb_init_device(void)
{
    /* No peripheral is driven yet. */
}

void usb_enable(bool on)
{
    (void)on;
}

int usb_detect(void)
{
    return USB_EXTRACTED;
}
