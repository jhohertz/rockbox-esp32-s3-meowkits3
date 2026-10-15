/***************************************************************************
 * ESP32-S3 USB.
 *
 * The SoC has an OTG block, but it is not the classic DWC2 that
 * firmware/drivers/usb-designware.c is written against. In this core's GUSBCFG
 * the PHY fields are phyif (bit 3), ulpiutmisel (4), fsintf (5) and physel (6),
 * and ESP-IDF's own driver has to set phyif = 1 for it to work. Rockbox's
 * usb_dw_check_hw() writes the register with only the PHY field and then
 * compares the whole value back, so the bring-up panics with
 * "PHY type not supported" before anything else gets a chance to run.
 *
 * The USB that is actually usable on this board is the USB-Serial-JTAG block,
 * which enumerates as CDC with no firmware involvement at all - it is what the
 * host uses to flash the image and to read the bring-up trace. Charging is
 * monitored through the AXP173 and storage is the SD card, so nothing on this
 * target needs the OTG core.
 *
 * firmware/usb.c still calls usb_init_device(), usb_enable() and usb_detect()
 * when HAVE_USBSTACK is not defined, so those hooks are provided here as a
 * no-op plus a VBUS read from the PMU.
 *
 * The target now defines USB_NONE, which makes firmware/usb.c supply its own
 * dummy usb_detect()/usb_start_monitoring(). Defining them here as well would
 * collide at link time, so the whole file is guarded.
 ****************************************************************************/
#include "config.h"

#ifndef USB_NONE

#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "power.h"
#include "usb.h"
#include "axp173.h"

/*
 * VBUS is the only insertion signal available on this board, and it is on the
 * PMU rather than the SoC. usb.c polls usb_detect() from the tick; a PMU read
 * takes about 1.6 ms on this I2C peripheral, which is too long for the tick, so
 * the result is cached here and refreshed by a thread.
 */
static long vbus_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static int vbus_status = USB_EXTRACTED;

static void usb_vbus_monitor(void)
{
    while (1)
    {
        vbus_status = (axp173_input_status() & POWER_INPUT_USB) ? USB_INSERTED
                                                                : USB_EXTRACTED;
        sleep(HZ / 5);
    }
}

void usb_init_device(void)
{

    create_thread(usb_vbus_monitor, vbus_stack, sizeof(vbus_stack), 0,
                  "usb vbus" IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));
}

void usb_enable(bool on)
{
    (void)on;   /* no OTG stack on this target */
}

int usb_detect(void)
{
    return vbus_status;
}

#endif /* !USB_NONE */
