/***************************************************************************
 * ESP32-S3 USB-OTG.
 *
 * The SoC has a Synopsys DWC2 core, and firmware/drivers/usb-designware.c
 * already implements the protocol machinery for it. What is missing is the set
 * of hooks the driver expects from a target and the FIFO layout, which is what
 * this file supplies.
 *
 * Two things here are specific to this chip rather than to DWC2 in general:
 *
 *  1. The DWC does not own the USB pins. The internal full-speed PHY is muxed
 *     between the DWC and the USB serial jtag block, and that mux only answers
 *     to software when the RTC's usb_conf says so. Until both are set the DWC
 *     sees nothing on the bus.
 *  2. There is no VBUS sense line. Neither the DWC nor the GPIO block can tell
 *     whether a cable is plugged in. The AXP173 can, so insertion is found by
 *     polling the PMU over I2C.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "power.h"
#include "usb.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "usb-designware.h"
#include "axp173.h"
#include "xtensa.h"
#include "bootlog.h"

/*
 * FIFO sizing. The core has 256 words total (OTG_DFIFO_DEPTH), and the driver
 * panics if rx + nptx + ptx exceeds it.
 *
 * The RX FIFO needs at least 16 + 2*(max packet / 4) + 2*endpoints words; for a
 * full-speed core with 64-byte packets and six endpoints that is exactly 64.
 * The non-periodic TX FIFO only ever carries EP0, whose packet is 64 bytes, so
 * 16 words is enough.
 *
 * The periodic TX FIFOs are the interesting constraint. GHWCFG4 reports five
 * (OTG_NUM_IN_EPS), but only four DIEPTXF registers exist; the driver writes one
 * per FIFO it counts. 40 words each gives four FIFOs and 240 words used, so the
 * phantom fifth is never allocated.
 */
#define RX_FIFOSZ   64
#define NPTX_FIFOSZ 16
#define PTX_FIFOSZ  40

_Static_assert(RX_FIFOSZ + NPTX_FIFOSZ + PTX_FIFOSZ <= 256,
               "FIFO allocation exceeds the esp32s3 DFIFO");

const struct usb_dw_config usb_dw_config = {
    /* The internal FS PHY is an 8-bit UTMI interface; the driver verifies this
     * by writing GUSBCFG back and reading it. */
    .phytype       = DWC_PHYTYPE_UTMI_8,
    .rx_fifosz     = RX_FIFOSZ,
    .nptx_fifosz   = NPTX_FIFOSZ,
    .ptx_fifosz    = PTX_FIFOSZ,
    /* No burst on the AHB side: the DMA is already fed from the FIFO. */
    .ahb_burst_len = HBSTLEN_SINGLE,
    .ahb_threshold = 0,
};

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)reg = val;
}

void usb_dw_target_enable_clocks(void)
{
    /* Put the internal FSLS PHY on the USB wrap rather than on the serial jtag
     * block, and enable the pads. phy_sel selects an *external* PHY, so it
     * stays clear; sw_usb_phy_sel is the bit that picks the wrap. */
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) |
                        XT_SW_HW_USB_PHY_SEL | XT_SW_USB_PHY_SEL);
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) | XT_USB_PAD_ENABLE);

    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) | XT_USB_CLK_EN);
}

void usb_dw_target_disable_clocks(void)
{
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) & ~XT_USB_CLK_EN);
}

void usb_dw_target_enable_irq(void)
{
    set_intenable(get_intenable() | (1u << XT_SLOT_USB));
}

void usb_dw_target_disable_irq(void)
{
    set_intenable(get_intenable() & ~(1u << XT_SLOT_USB));
}

void usb_dw_target_clear_irq(void)
{
    /* The core's own GINTSTS is cleared as each event is serviced, and the
     * slot is level-triggered, so there is nothing chip-specific to flush. */
}

/*
 * VBUS is the only insertion signal available on this board, and it is on the
 * PMU, not on the SoC. A read of the PMU status register takes about 1.6 ms on
 * this I2C peripheral, far too long to do from the tick, so it gets its own
 * thread. 200 ms matches the debounce interval usb.c already applies.
 */
static long vbus_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static bool vbus_last;

static void usb_vbus_monitor(void)
{
    boot_trace('v');
    while (1)
    {
        bool inserted =
            (axp173_input_status() & POWER_INPUT_USB) != 0;

        if (inserted != vbus_last)
        {
            vbus_last = inserted;
            usb_status_event(inserted ? USB_INSERTED : USB_EXTRACTED);
        }

        sleep(HZ / 5);
    }
}

void usb_init_device(void)
{
    /* Route the source to its slot once. The enable/disable hooks above only
     * touch INTENABLE, so the mapping does not have to be rewritten on every
     * transfer. Slot 9 is the lowest level-1 slot left: 1 is the tick, 2 the
     * audio GDMA, 3 SDHOST, 5 the recording GDMA and 8 the I2C bus. */
    wr(XT_INTERRUPT_CORE0_BASE + 4 * XT_SRC_USB, XT_SLOT_USB);

    create_thread(usb_vbus_monitor, vbus_stack, sizeof(vbus_stack), 0,
                  "usb vbus" IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));

    boot_trace('U');                /* USB device side started */
}

void usb_enable(bool on)
{
    if (on)
        usb_core_init();
    else
        usb_core_exit();
}

int usb_detect(void)
{
    return (axp173_input_status() & POWER_INPUT_USB) ? USB_INSERTED
                                                     : USB_EXTRACTED;
}
