/***************************************************************************
 * ESP32-S3 USB target glue.
 *
 * The SoC has a Synopsys DWC2 OTG core at 0x60080000 (see the hardware map in
 * firmware/include/xtensa.h): full speed only, internal FS PHY, six endpoints of
 * which five can be IN, and 256 words of device FIFO RAM. Rockbox's
 * firmware/drivers/usb-designware.c implements the usb_drv.h contract for that
 * register layout, and this file supplies the five hooks it declares plus the
 * device configuration.
 *
 * STATUS: the stack is compiled; the hardware is not driven. usb_enable() is a
 * no-op in both directions, so usb_core_init() and therefore usb_drv_init() never
 * run. That is deliberate: usb_dw_check_hw() writes GUSBCFG's PHY field and then
 * compares the whole register back, and on this core GUSBCFG's fields are phyif
 * (bit 3), ulpiutmisel (4), fsintf (5) and physel (6), so the comparison fails and
 * the driver panics with "PHY type not supported" before anything else happens.
 * Making this core enumerable is the first bring-up task; until then nothing here
 * should be reachable.
 *
 * The pins are the other half of the story. The DWC does not own them: a USB wrap
 * muxes the internal FSLS PHY between the DWC and the USB-Serial-JTAG block, and
 * the mux only answers to software when the RTC's usb_conf says so. Both have to be
 * set -- usb_dw_target_enable_clocks() does it. Because the console and esptool both
 * live on USB-Serial-JTAG, that handover silences the trace and the flash port, so
 * the pads move only while USB mode is active and return to USJ on reset.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "power.h"
#include "axp173.h"
#include "xtensa.h"

#include <esp_intr_alloc.h>
#include <soc/interrupts.h>

/* The USB headers are inside the guard on purpose: firmware/SOURCES lists this
 * file unconditionally, so it has to compile when the stack is off, and
 * usb_drv.h sizes its endpoint arrays from USB_NUM_ENDPOINTS, which only exists
 * once CONFIG_USBOTG has been chosen. */
#ifdef HAVE_USBSTACK
#include "usb.h"
#include "usb_drv.h"
#include "usb-designware.h"

/* Device FIFO RAM is 256 words in total (OTG_DFIFO_DEPTH). Full speed means a 64
 * byte maximum packet, so an IN endpoint needs 64/4 = 16 words as its floor; 40
 * gives margin for the status plus a packet. NPTX serves EP0 only, whose packet is
 * 64 bytes. These are the numbers the earlier bring-up work used on this core. */
#define XT_FIFO_RAMSZ   256
#define NPTX_FIFOSZ     16
/* Each dedicated IN FIFO has to hold at least max_packet/4 words. Full speed caps a
 * bulk or interrupt packet at 64 bytes, so 16 words is the floor; anything larger
 * steals from the RX side, which has to hold two packets plus status. */
#define PTX_FIFOSZ      16
#define RX_FIFOSZ       (XT_FIFO_RAMSZ - NPTX_FIFOSZ - \
                         PTX_FIFOSZ * (USB_NUM_ENDPOINTS - 1))

_Static_assert(PTX_FIFOSZ >= 64 / 4, "a dedicated IN FIFO cannot hold one packet");
_Static_assert(RX_FIFOSZ >= 14 + 2 * (64 / 4 + 1) + 2 * USB_NUM_ENDPOINTS,
               "RX FIFO is too small for two packets plus status");
_Static_assert(NPTX_FIFOSZ + PTX_FIFOSZ * (USB_NUM_ENDPOINTS - 1) + RX_FIFOSZ
               <= XT_FIFO_RAMSZ,
               "device FIFO split exceeds the core's 256-word FIFO RAM");

const struct usb_dw_config usb_dw_config = {
    /* The internal FS PHY is an 8-bit UTMI interface. This is the value
     * usb_dw_check_hw() writes into GUSBCFG and reads back -- the reason the
     * bring-up is still gated. */
    .phytype       = DWC_PHYTYPE_UTMI_8,
    .rx_fifosz     = RX_FIFOSZ,
    .nptx_fifosz   = NPTX_FIFOSZ,
    .ptx_fifosz    = PTX_FIFOSZ,
    /* No burst on the AHB side: the DMA is fed from the FIFO. */
    .ahb_burst_len = HBSTLEN_SINGLE,
    .ahb_threshold = 0,
};

static inline uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

static inline void wr(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)reg = val;
}

void INT_USB_FUNC(void);      /* usb-designware.c: usb_dw_irq() */

/* The interrupt is IDF-owned now. The bare-metal bring-up routed it by hand to
 * XT_SLOT_USB through the vector table; under ESP-IDF the allocator does that, so
 * the enable/disable hooks below are a handle, not an INTENABLE bit. */
static intr_handle_t usb_intr = NULL;

static void usb_dwc_isr(void *arg)
{
    (void)arg;
    /* Nothing may print here: printf takes the newlib stdout lock, which aborts
     * from interrupt context. See the note on gdma_tx_isr(). */
    INT_USB_FUNC();
}

void usb_dw_target_enable_clocks(void)
{
    /* The mux needs both halves: the RTC bit that lets software override it, and
     * the wrap bit that routes the FSLS PHY to it. phy_sel selects an *external*
     * PHY, so XT_USB_PHY_SEL_EXT stays clear. */
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) |
       (XT_SW_HW_USB_PHY_SEL | XT_SW_USB_PHY_SEL));
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) | XT_USB_PAD_ENABLE);
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) | XT_USB_CLK_EN);
}

void usb_dw_target_disable_clocks(void)
{
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) & ~XT_USB_CLK_EN);

    /* Give the pins back. USB-Serial-JTAG is the console and the flash port, so
     * leaving the mux pointed at the DWC would silence both after disconnect. */
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) & ~XT_USB_PAD_ENABLE);
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) & ~XT_SW_USB_PHY_SEL);
}

void usb_dw_target_enable_irq(void)
{
    if (usb_intr == NULL)
    {
        if (esp_intr_alloc(ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1,
                           usb_dwc_isr, NULL, &usb_intr) != 0)
        {
            usb_intr = NULL;
            return;
        }
    }
    else
    {
        esp_intr_enable(usb_intr);
    }
}

void usb_dw_target_disable_irq(void)
{
    if (usb_intr != NULL)
        esp_intr_disable(usb_intr);
}

void usb_dw_target_clear_irq(void)
{
    /* The core's own interrupt status is cleared by the driver. */
}

/*
 * VBUS is not a GPIO and not a SoC input on this board: it is reported by the
 * AXP173 over I2C. That read takes ~1.6 ms on this command-list peripheral, far
 * too long for the tick, so it is sampled on demand rather than continuously.
 *
 * There used to be a monitor thread here, and it cost DEFAULT_STACK_SIZE of
 * internal DRAM (4 KB) in a region that has ~1 KB free. It bought nothing while
 * the stack is inert: CONFIG_USBOTG == USBOTG_DESIGNWARE sets USB_STATUS_BY_EVENT,
 * so Rockbox never polls usb_detect(), and a thread that only records a value
 * nobody reads is still a thread. When the core is armed, presence should come
 * from the OTG interrupt (the core reports its own connection state) rather than
 * from a poll of the PMU; the PMU read stays here for the charging display.
 */
static int vbus_sample(void)
{
    return (axp173_input_status() & POWER_INPUT_USB) ? USB_INSERTED
                                                     : USB_EXTRACTED;
}

void usb_init_device(void)
{
    /* Nothing to start. The DWC core is armed by usb_enable() once
     * usb_dw_check_hw() can pass on this PHY; until then creating a thread would
     * only spend DRAM on a value nobody consumes. */
}

/*
 * Hard gate, not a status read. VBUS is present whenever the board is powered from
 * the host, which is most of the time, and Rockbox treats an insertion as a request
 * to take over the disk. Until the core can actually enumerate, reporting an
 * insertion would put the device into USB slave mode with no host to talk to: the
 * card unmounted and nothing served. The PMU state stays available in vbus_sample()
 * for the charging display; this function reports the transport, not the cable.
 */
int usb_detect(void)
{
    return USB_EXTRACTED;
}

/*
 * Named __wrap_usb_enable, not usb_enable: ESP-IDF's esp32s3.rom.ld assigns the
 * absolute symbol usb_enable to a mask ROM routine at 0x4000297c, and a linker
 * script's absolute assignment wins over a definition in an object file without a
 * word of warning. Rockbox's usb_init() therefore called the ROM's function, which
 * is part of the download/console path and not the USB transport API -- the crash
 * was a LoadProhibited at EXCVADDR 0x0c on the first boot with HAVE_USBSTACK.
 * idf-app/CMakeLists.txt adds -Wl,--wrap=usb_enable, which is what makes this the
 * function that actually runs. Do not rename it back.
 */
void __wrap_usb_enable(bool on)
{
    /* Deliberately inert while usb_dw_check_hw() cannot pass on this core. When it
     * is fixed, this becomes the usual shape: usb_core_init() on, usb_core_exit()
     * off, with cpu_boost() around it if the FS transfer needs it. */
    (void)on;
}

#endif /* HAVE_USBSTACK */
