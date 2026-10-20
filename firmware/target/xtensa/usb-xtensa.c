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
#include "panic.h"
#include "button.h"
#include "power.h"
#include "axp173.h"
#include "xtensa.h"
#include "tick.h"

#include <esp_intr_alloc.h>
#include <soc/interrupts.h>

/* The USB headers are inside the guard on purpose: firmware/SOURCES lists this
 * file unconditionally, so it has to compile when the stack is off, and
 * usb_drv.h sizes its endpoint arrays from USB_NUM_ENDPOINTS, which only exists
 * once CONFIG_USBOTG has been chosen. */
#ifdef HAVE_USBSTACK
#include "usb.h"
#include "usb_drv.h"
#include "usb_core.h"
#include "usb-designware.h"

/* Device FIFO RAM. IDF's usb_dwc_cfg.h says OTG_DFIFO_DEPTH 256, but the core
 * itself says otherwise: GHWCFG3[31:16] reads 200 words on this silicon
 * (hwcfg3=0x00c804b5, measured at boot). The register wins -- a split sized for
 * 256 words would program DIEPTXF registers whose start addresses run past the
 * end of the RAM, which shows up as IN transfers that never complete rather than
 * as anything that names the cause. */
#define XT_FIFO_RAMSZ   200

/* EP0 IN. Full speed caps the packet at 64 bytes = 16 words. */
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
    /* The internal full-speed PHY, selected by GUSBCFG.fsintf (bit 5). This
     * core has no classic PHY-type encoding: UTMI/ULPI/PHYSEL do not exist, and
     * fsphytype is what GHWCFG2 reports. usb_dw_check_hw() now checks that bit
     * rather than comparing the whole register, whose reset value is not zero.
     */
    .phytype       = FSINTF,
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
    /* Two gates, and the second one is the easy one to miss: usb_clk_en (bit 23
     * of PERIP_CLK_EN0) is the wrap and PHY domain, usb_device_clk_en (bit 10 of
     * PERIP_CLK_EN1) is the DWC core. With only the first, every OTG register
     * reads 0x00000000 and nothing faults -- which looks like a dead block rather
     * than a missing clock. */
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) | XT_USB_CLK_EN);
    wr(XT_PERIP_CLK_EN1, rd(XT_PERIP_CLK_EN1) | XT_USB_DEVICE_CLK_EN);
    wr(XT_PERIP_RST_EN0, rd(XT_PERIP_RST_EN0) & ~XT_USB_RST);
    wr(XT_PERIP_RST_EN1, rd(XT_PERIP_RST_EN1) & ~XT_USB_DEVICE_RST);

    /* The mux needs both halves: the RTC bit that lets software override it, and
     * the wrap bit that routes the FSLS PHY to it. phy_sel selects an *external*
     * PHY, so XT_USB_PHY_SEL_EXT stays clear. */
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) |
       (XT_SW_HW_USB_PHY_SEL | XT_SW_USB_PHY_SEL));
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) | XT_USB_PAD_ENABLE |
                        XT_USB_WRAP_CLK_EN);

    /* The three force bits are not set by IDF's driver at all -- IDF's device stack
     * reaches the core through its own HCD -- but they are what made this core
     * answer on this board. The pad-free boot probe read GSNPSID = 0x00000000 with
     * both gates on and both resets cleared, and 0x4f54400a only after
     * USB_WRAP_CLK_EN, DFIFO_FORCE_PU, PHY_CLK_FORCE and AHB_CLK_FORCE were set.
     * A block whose FIFO RAM is powered down reads as all zeroes rather than
     * faulting, and a core with no PHY clock cannot drive D+ -- which is the exact
     * symptom: the host sees no device at all, not a bad one. */
    wr(XT_USB_OTG_CONF,
       (rd(XT_USB_OTG_CONF) & ~XT_USB_DFIFO_FORCE_PD) |
       XT_USB_DFIFO_FORCE_PU | XT_USB_PHY_CLK_FORCE | XT_USB_AHB_CLK_FORCE);

    /* The last thing that runs before the console disappears: from here on the
     * breadcrumb trail is the only record, until the next boot prints it. */
    xtensa_breadcrumb("usbClk", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_CLK_EN1));
}

void usb_dw_target_disable_clocks(void)
{
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) & ~XT_USB_CLK_EN);
    wr(XT_PERIP_CLK_EN1, rd(XT_PERIP_CLK_EN1) & ~XT_USB_DEVICE_CLK_EN);

    /* Release the forced clocks so the FIFO power follows the gate again and a
     * second attach starts from the state this one found. */
    wr(XT_USB_OTG_CONF,
       rd(XT_USB_OTG_CONF) & ~(XT_USB_DFIFO_FORCE_PU | XT_USB_PHY_CLK_FORCE |
                              XT_USB_AHB_CLK_FORCE | XT_USB_WRAP_CLK_EN));

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

    xtensa_breadcrumb("usbIrq", (unsigned long)(usb_intr != NULL), 0);
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

/* TEMPORARY: identity probe, deliberately pad-free.
 *
 * The endpoint budget for both USB profiles rests on what this core reports,
 * not on the array sizes in IDF's usb_struct.h, so the numbers are read from the
 * silicon and printed. Only the peripheral clock gate is touched: the pad mux
 * (XT_USB_PAD_ENABLE and the RTC override) is *not* set, because GPIO19/20 are
 * shared with USB-Serial-JTAG and taking them would silence the console and
 * esptool. Offsets and field positions come from IDF's generated
 * usb_dwc_struct.h: numdeveps at ghwcfg2[13:10], numhstchnl at [17:14],
 * dfifodepth at ghwcfg3[31:16], numdevperioeps at ghwcfg4[3:0], ineps at
 * ghwcfg4[29:26], dedfifomode at bit 25.
 */
#define USB_HW_PROBE 1
#ifdef USB_HW_PROBE
#define USB_WRAP_CLK_EN         (1u << 31)   /* OTG_CONF bit 31 */
#define USB_WRAP_DFIFO_FORCE_PU (1u << 22)   /* force the FIFO power *up* */
#define USB_WRAP_DFIFO_FORCE_PD (1u << 3)    /* force it *down* -- reads back 0 */
#define USB_WRAP_PHY_CLK_FORCE  (1u << 20)
#define USB_WRAP_AHB_CLK_FORCE  (1u << 19)
#define USB_WRAP_PAD_ENABLE     (1u << 18)   /* NOT touched: pads belong to USJ */
#define USB_WRAP_PHY_SEL        (1u << 2)    /* NOT touched: 1 = external PHY */

static void usb_hw_probe(void)
{
    uint32_t clk0_saved = rd(XT_PERIP_CLK_EN0);
    uint32_t clk1_saved = rd(XT_PERIP_CLK_EN1);
    uint32_t rst0_saved = rd(XT_PERIP_RST_EN0);
    uint32_t rst1_saved = rd(XT_PERIP_RST_EN1);
    uint32_t wrap_saved = rd(XT_USB_OTG_CONF);
    uint32_t id, c2, c3, c4;

    printf("USB probe: clk0=%08x clk1=%08x rst0=%08x rst1=%08x otg_conf=%08x rtc=%08x\n",
           (unsigned)clk0_saved, (unsigned)clk1_saved, (unsigned)rst0_saved,
           (unsigned)rst1_saved, (unsigned)wrap_saved,
           (unsigned)rd(XT_RTC_USB_CONF));

    /* Both gates, both resets cleared. Reading with only the wrap gate enabled is
     * what produced the first all-zero probe on this board. */
    wr(XT_PERIP_CLK_EN0, clk0_saved | XT_USB_CLK_EN);
    wr(XT_PERIP_CLK_EN1, clk1_saved | XT_USB_DEVICE_CLK_EN);
    wr(XT_PERIP_RST_EN0, rst0_saved & ~XT_USB_RST);
    wr(XT_PERIP_RST_EN1, rst1_saved & ~XT_USB_DEVICE_RST);

    id = DWC_GSNPSID;
    printf("USB probe: id with clocks as-found = %08x\n", (unsigned)id);

    /* Force the OTG block's clocks and DFIFO power up. Deliberately does not
     * touch USB_PAD_ENABLE or PHY_SEL: those decide who owns GPIO19/20, and the
     * console and esptool are on USB-Serial-JTAG. DFIFO_FORCE_PD is cleared
     * because a powered-down block reads back as all zeroes rather than
     * faulting, which is what the first probe saw. */
    wr(XT_USB_OTG_CONF,
       (wrap_saved & ~(USB_WRAP_DFIFO_FORCE_PD | USB_WRAP_PHY_SEL |
                       USB_WRAP_PAD_ENABLE)) |
       USB_WRAP_CLK_EN | USB_WRAP_DFIFO_FORCE_PU |
       USB_WRAP_PHY_CLK_FORCE | USB_WRAP_AHB_CLK_FORCE);

    id = DWC_GSNPSID;
    c2 = DWC_GHWCFG2;
    c3 = DWC_GHWCFG3;
    c4 = DWC_GHWCFG4;

    printf("USB probe: otg_conf=%08x id=%08x rev=%d\n",
           (unsigned)rd(XT_USB_OTG_CONF), (unsigned)id, (int)(id & 0xff));
    printf("USB core: hwcfg2=%08x eps=%d hstchnl=%d fsphy=%d hsphy=%d\n",
           (unsigned)c2, (int)(((c2 >> 10) & 0xf) + 1),
           (int)(((c2 >> 14) & 0xf) + 1), (int)((c2 >> 8) & 3),
           (int)((c2 >> 6) & 3));
    printf("USB core: fifo_words=%u hwcfg3=%08x\n",
           (unsigned)(c3 >> 16), (unsigned)c3);
    printf("USB core: hwcfg4=%08x perio_eps=%d in_eps=%d ded_fifo=%d\n",
           (unsigned)c4, (int)((c4 & 0xf) + 1), (int)((c4 >> 26) & 0xf),
           (int)((c4 >> 25) & 1));
    printf("USB core: hwcfg1=%08x gusbcfg=%08x dcfg=%08x gintsts=%08x\n",
           (unsigned)DWC_GHWCFG1, (unsigned)DWC_GUSBCFG, (unsigned)DWC_DCFG,
           (unsigned)DWC_GINTSTS);

    /* The claim usb_dw_check_hw() now makes: fsintf (bit 5) selects the internal
     * FS PHY and is writable. Verify it here rather than finding out during a
     * bring-up that also needs the pads. */
    {
        uint32_t g = DWC_GUSBCFG;
        DWC_GUSBCFG = g | FSINTF;
        printf("USB probe: gusbcfg %08x -> %08x (fsintf %s)\n",
               (unsigned)g, (unsigned)DWC_GUSBCFG,
               (DWC_GUSBCFG & FSINTF) ? "writable" : "read-only");
        DWC_GUSBCFG = g;
    }

    /* Leave the block exactly as it was found: the console and esptool are on
     * USB-Serial-JTAG, and the pad mux was never touched. */
    wr(XT_USB_OTG_CONF, wrap_saved);
    wr(XT_PERIP_CLK_EN0, clk0_saved);
    wr(XT_PERIP_CLK_EN1, clk1_saved);
    wr(XT_PERIP_RST_EN0, rst0_saved);
    wr(XT_PERIP_RST_EN1, rst1_saved);
}
#endif /* USB_HW_PROBE */

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

/*
 * Attaching the core is opt-in, and uncommenting the define below is the only
 * switch. It is left off because the step is not recoverable from the console:
 * usb_dw_target_enable_clocks() hands GPIO19/20 to the DWC core, and those pins
 * are what USB-Serial-JTAG uses for the trace *and* for esptool. A hang inside
 * USB mode therefore leaves nothing to read and nothing to flash with; the only
 * way back is a power cycle, and the only power control on this board is the
 * AXP173's PEK input. Do not turn this on away from the board.
 *
 * With it off, the stack is compiled, linked and reachable in the image, and
 * inert at runtime: usb_enable() does nothing, so usb_core_init() and
 * usb_drv_init() never run and the pads never move.
 */
#define USB_ALLOW_ATTACH
/* TEMPORARY: report the core's state on the LCD ~8 s after attaching, then halt.
 * See the comment in __wrap_usb_enable(). */
#define USB_ATTACH_REPORT 1

#ifdef USB_ALLOW_ATTACH

/* VBUS presence has to come from the PMU. The OTG core cannot report it before
 * it is armed, and the core is only armed in USB mode, which is what presence is
 * supposed to trigger -- so polling the AXP173 is the only way to break the
 * circular dependency. CONFIG_USBOTG == USBOTG_DESIGNWARE sets USB_STATUS_BY_EVENT,
 * which means Rockbox does not poll usb_detect() itself: the target has to push
 * changes through usb_status_event().
 *
 * The read costs about 1.6 ms on a command-list I2C controller shared with the
 * codec, the IO expander, the touchscreen, the RTC and the PMU's own status, so it
 * runs at roughly 4 Hz from the tick task thread rather than every tick.
 */
#define USB_VBUS_POLL_TICKS 4

/*
 * A boot window in which the pins are not handed over. This is a bring-up
 * affordance, not a behaviour difference: it exists so the host can always reach
 * the board while the attach is still being debugged.
 *
 * Taking GPIO19/20 away from USB-Serial-JTAG removes the console *and* esptool, so
 * a firmware that attaches the moment it senses a cable can leave no way to put a
 * different firmware back -- and an attach that does not enumerate does that on
 * purpose. A flash attempt also races the attach: if the pins move while esptool is
 * mid-handshake, the ROM's SLIP responses stop and esptool reports "the chip
 * stopped responding", which is exactly what happened here. current_tick advances
 * at HZ, so this is 120 seconds of guaranteed USJ at every boot.
 *
 * When the transport is proven, drop this window rather than keep it: Rockbox's
 * behaviour is to enter USB mode on insertion, and delaying that is a deviation.
 */
#define USB_ATTACH_DELAY (120 * HZ)

static bool usb_attach_allowed(void)
{
    return current_tick >= USB_ATTACH_DELAY;
}

static void usb_vbus_poll(void)
{
    static unsigned ticks;
    static int last = USB_EXTRACTED;
    int status;

    if (++ticks < USB_VBUS_POLL_TICKS)
        return;
    ticks = 0;

    if (!usb_attach_allowed())
        return;

    /* Cable state, posted whenever it changes. usb.c turns an insertion event into
     * USB mode at line 499, and that is core behaviour this target does not
     * override: what the usb_mode setting changes -- which classes get composed,
     * whether the volumes get handed to the host -- is usb.c's business, not the
     * target's. The consequence on this hardware is that a player plugged into a
     * host is in USB mode, because charging here is not optional.
     */
    status = vbus_sample();

    if (status != last)
    {
        last = status;
        usb_status_event(status);
    }
}

/*
 * Whether 5V is present -- the contract in usb.h:80, answered the way every other
 * target answers it. This board's only cable sensor is the PMU: the SoC has no VBUS
 * input and GPIO19/20 are D+/D- only, so this is an AXP173 read.
 */
int usb_detect(void)
{
    return vbus_sample();
}

#else /* !USB_ALLOW_ATTACH */

/*
 * With USB_ALLOW_ATTACH undefined the transceiver is never armed, so the target
 * reports no cable. This is deliberately a bring-up state rather than a behaviour:
 * reporting an insertion while nothing can enumerate would put the player into USB
 * mode with no transport behind it -- the USB screen with nowhere to go, and with
 * mass storage selected, an unmounted card serving nobody. Drop this branch once
 * the core enumerates and the target becomes ordinary.
 */
int usb_detect(void)
{
    return USB_EXTRACTED;
}

#endif /* USB_ALLOW_ATTACH */

void usb_init_device(void)
{
#ifdef USB_ALLOW_ATTACH
    /* Put the pins back before anything else, and do it every boot.
     *
     * The mux override lives in the RTC block (RTCCNTL.usb_conf), which is *not*
     * cleared by a chip reset -- only by a power-on reset. So once a boot hands the
     * pads to the wrap, a panic, an abort or esp_restart leaves them there: the
     * next boot has no USB-Serial-JTAG console and esptool cannot open a port, and
     * the only way back is to take the board off power. That is what happened on
     * this board's first attach attempt -- a report panic reset into a boot that
     * could not be reached at all.
     *
     * Clearing the override at boot makes the console unconditional: every boot
     * starts with the pins on USJ, so there is always a window to flash from,
     * whatever the previous boot did.
     */
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) & ~(XT_USB_PAD_ENABLE |
                                               XT_USB_WRAP_CLK_EN));
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) & ~XT_SW_USB_PHY_SEL);
#endif

#ifdef USB_HW_PROBE
    usb_hw_probe();
#endif

#ifdef USB_ALLOW_ATTACH
    tick_add_task(usb_vbus_poll);
#endif
    /* Everything else is armed by usb_enable(). */
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
 *
 * usb.h maps usb_hw_open()/usb_hw_close() onto this, and usb.c calls them when it
 * enters and leaves USB mode. Note that usb.c calls usb_enable(true) on insertion
 * whatever the usb_mode setting says -- it does so at line 483, before it computes
 * power-only state at line 492 -- so on a software stack the mode chooses the class
 * composition, not whether the transceiver is opened. On this board that means the
 * pins move on insertion, which is core behaviour; the recovery invariant is the
 * pad clear in usb_init_device(), not a refusal here.
 */
void __wrap_usb_enable(bool on)
{
#ifdef USB_ALLOW_ATTACH
    xtensa_breadcrumb("usbEn", on ? 1 : 0, 0);

    if (on)
        usb_core_init();   /* usb_drv_init(): clocks, pads, device mode, connect */
    else
        usb_core_exit();   /* usb_drv_exit(): soft-disconnect, and the pads back */

    /* Both halves of the register state, recorded before anything can change it.
     * The trail survives the reset, and app_main() replays it inside the boot
     * window, so one power cycle tells us what the previous boot did -- including
     * whether this function was called with false, which is the other explanation
     * for a report that reads all zeroes. */
    xtensa_breadcrumb(on ? "usbIn" : "usbOut",
                      DWC_GSNPSID, rd(XT_USB_OTG_CONF));
    xtensa_breadcrumb("usbClk2", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_RST_EN0));

#ifdef USB_ATTACH_REPORT
    if (!on)
        return;   /* the report is about the attach, not the teardown */
    /* TEMPORARY: once the pads are handed over the screen is the only channel
     * left. panicf() draws on the LCD (see panic.c), and the abort resets the chip,
     * which returns the pins to USB-Serial-JTAG -- so the next boot prints the
     * breadcrumb trail on a live console. Read both.
     *
     * What to look for: DCTL bit 0 (SDIS) says whether the core is still soft
     * disconnected, i.e. whether D+ has a pull-up at all. GINTSTS bit 2 (reset
     * detected) and bit 3 (enum speed) say whether a host ever touched the bus.
     * GUSBCFG bit 5 (fsintf) and bit 30 (fdmod) say the PHY and device-mode writes
     * took. OTG_CONF bit 18 (pad_enable) says the mux moved the pins.
     */
    sleep(HZ * 8);
    /* Record everything in the trail as well as on the screen. The pads are gone by
     * now, so the console cannot see this line; but the abort resets the chip, the
     * boot hook puts the pins back on USB-Serial-JTAG, and app_main() replays the
     * trail on a live console -- which is how this gets read back without anyone
     * having to transcribe the LCD.
     *
     * clk0 bit 23 and rst0 bit 23 distinguish the two failure shapes: a block that
     * is clocked but held in reset reads zero just like a block that is ungated, so
     * the gate and reset words have to be in the report, not inferred from it. */
    xtensa_breadcrumb("usbDctl", DWC_DCTL, DWC_GINTSTS);
    xtensa_breadcrumb("usbClk3", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_RST_EN0));

    panicf("USB: id=%08lx dctl=%08lx gsts=%08lx wrap=%08lx clk0=%08lx rst0=%08lx",
           (unsigned long)DWC_GSNPSID, (unsigned long)DWC_DCTL,
           (unsigned long)DWC_GINTSTS, (unsigned long)rd(XT_USB_OTG_CONF),
           (unsigned long)rd(XT_PERIP_CLK_EN0), (unsigned long)rd(XT_PERIP_RST_EN0));
#endif
#else
    (void)on;
#endif
}

#endif /* HAVE_USBSTACK */
