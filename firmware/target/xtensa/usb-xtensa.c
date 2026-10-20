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
#ifdef USB_ENABLE_SERIAL
#include <stdio.h>          /* snprintf */
#include "usbstack/usb_serial.h"
#endif
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
#include <soc/soc.h>          /* REG_WRITE */
#include <soc/rtc_cntl_reg.h>  /* RTC_CNTL_OPTION1_REG, FORCE_DOWNLOAD_BOOT */
#include <soc/usb_serial_jtag_reg.h> /* USJ CONF0, for the handover to USJ */
#include <esp_rom_gpio.h>
#include <esp_system.h>
#include <stdlib.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_pins.h>

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
    /* The internal full-speed PHY. Two bits, and we were only setting one:
     *
     *   fsintf (bit 5) -- the full-speed *interface*
     *   physel (bit 6) -- selects the embedded FS/LS PHY over the embedded HS PHY
     *
     * PHYSEL (bit 6) is not something this core can be talked out of: GHWCFG2 reports
     * no high-speed PHY, so there is nothing for the bit to choose between, and it
     * reads back set no matter what the driver assigns. Measured: with phytype left at
     * FSINTF -- i.e. the driver's GUSBCFG write not asking for PHYSEL at all -- the
     * register still reads 0x40001460, bit 6 included. Adding PHYSEL to phytype
     * therefore changed nothing in that register, and the run in which the connection
     * disappeared was the one that also turned on the wrap's pad_pull_override; that,
     * not PHYSEL, is what took the connection away (see usb_dw_target_enable_clocks).
     *
     * The register is recorded in the attach trail (usbGusb) so this stays checkable.
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

/* Counting core interrupts, and nothing else. A host that sees a connection but
 * gets no reply to GET_DESCRIPTOR (kernel: "device descriptor read/64, error -110")
 * is either a core that never interrupts or a driver that mishandles the interrupt;
 * this counter separates the two, and the poll below carries it to the breadcrumb
 * trail because the console is gone by then. */
static volatile uint32_t xt_usb_irqs;
/* Counted separately from xt_usb_irqs, which is incremented only inside
 * usb_dwc_isr() and therefore means "the interrupt was delivered" and nothing else.
 * The polled stand-in calls the driver directly so it cannot inflate that claim. */

/* How many times the post-init one-shot service fired, i.e. how much status the core
 * had already latched by the time the interrupt was enabled. */


/* Which masked source is firing, and on which endpoint. Measured: 14,500 interrupts
 * per second while only ~23 CDC lines per second are being sent, and GINTSTS & GINTMSK
 * reads as 0 at report time -- the conditions are set and cleared between reports, so
 * the attribution has to happen inside the handler. DAINT is read before the driver
 * runs because the driver is what clears it, and the masks are sticky so a slow report
 * cadence still shows which endpoints were involved at all. */
static volatile uint32_t xt_irq_rst;
static volatile uint32_t xt_irq_enum;
static volatile uint32_t xt_irq_iep;
static volatile uint32_t xt_irq_oep;
static volatile uint32_t xt_irq_none;
/* Handler runs that happened before the driver was armed, i.e. while the core was still
 * being configured. Kept separate from xt_irq_none, which means "the line was drawn with
 * no masked source set" -- conflating the two would hide a real delivery fault. */
static volatile uint32_t xt_irq_early;
/* enable/disable churn from the driver, counted rather than trailed: see the note in
 * usb_dw_target_enable_irq(). */
static volatile uint32_t xt_irq_enable_n;
static volatile uint32_t xt_irq_disable_n;
static volatile uint32_t xt_irq_daint_in;
static volatile uint32_t xt_irq_daint_out;

/* INTENABLE and INTERRUPT are readable on this core config (rsr.intlevel is not,
 * the level lives in PS). Together with GINTMSK and GAHBCFG these say which half
 * of the path is broken: a set bit in GINTSTS whose source is unmasked is a core
 * problem, a masked source with the CPU slot enabled is a routing problem, and
 * neither is an allocator problem. */
static uint32_t xt_read_intenable(void)
{
    uint32_t v;
    __asm__ __volatile__("rsr.intenable %0" : "=r"(v));
    return v;
}

static uint32_t xt_read_interrupt(void)
{
    uint32_t v;
    __asm__ __volatile__("rsr.interrupt %0" : "=r"(v));
    return v;
}

/* Set between usb_enable(true) and usb_enable(false), purely so the poll can report
 * what the armed core is doing. Deliberately not used by usb_detect(), which answers
 * the cable as usb.h:80 requires. Declared here because the handler below needs it:
 * the interrupt is allocated before usb_core_init() has configured the core (at boot,
 * for the routing dump), so the handler has to know which phase it is in. */
static bool xt_armed;

static void usb_dwc_isr(void *arg)
{
    (void)arg;
    /* Nothing may print here: printf takes the newlib stdout lock, which aborts
     * from interrupt context. See the note on gdma_tx_isr(). */
    xt_usb_irqs++;
    if (!xt_armed)
    {
        /* The interrupt is live from allocation, so this can run while the core is
         * still being configured, before the driver has masks anyone cares about.
         * Clear the status rather than return: on a level source a bit left set keeps
         * the line drawn and re-enters the handler with nothing to do -- and clearing
         * pending bits is the only thing that has ever made this core assert again for
         * a later event. */
        DWC_GINTSTS = 0xFFFFFFFF;
        DWC_GOTGINT = 0xFFFFFFFF;
        xt_irq_early++;
        return;
    }
    {
        uint32_t pend = DWC_GINTSTS & DWC_GINTMSK;
        uint32_t daint = DWC_DAINT;

        if (pend & USBRST)
            xt_irq_rst++;
        if (pend & ENUMDNE)
            xt_irq_enum++;
        if (pend & IEPINT)
        {
            xt_irq_iep++;
            xt_irq_daint_in |= daint & 0xffff;
        }
        if (pend & OEPINT)
        {
            xt_irq_oep++;
            xt_irq_daint_out |= (daint >> 16) & 0xffff;
        }
        /* An interrupt with no masked source set is its own answer: the line was
         * drawn by something the driver will not look at. */
        if (!pend)
            xt_irq_none++;
    }
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
    /* No pull override here, deliberately. IDF 4.4's usb_phy_ll_int_load_conf() did
     * set pad_pull_override with D+ pull-up and D- pull-down for device mode, and
     * adding it measured worse: with the override on, the host stopped seeing a
     * connection during an attach entirely (no descriptor attempts at all, where
     * before it had at least attempted some), and GINTSTS lost its usbrst bit. IDF's
     * current path -- usb_new_phy(), which is what the TinyUSB device examples use --
     * only calls usb_wrap_ll_phy_enable_pull_override() in the *host* branch, so on
     * 5.5 a device leaves the override alone and the core drives the pull-up through
     * its own connect. That is what this does too, and the measured difference agrees:
     * with the override on, GINTSTS lost usbrst and the host made no enumeration
     * attempt at all; with it off, the host reads the descriptors, binds cdc_acm and
     * creates a tty. */
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

    /* Force the session-end signal to "VBUS present".
     *
     * This board routes VBUS to the AXP173 only -- the SoC has no VBUS input, and
     * GPIO19/20 are D+/D-. With OTG_CONF.srp_sessend_override clear, IDF's own
     * usb_wrap_ll says the signal is "controlled by the chip input", and there is no
     * chip input, so the DWC core sees a floating session-end and behaves as if
     * VBus were below 0.2 V. Measured on hardware: after a full attach (both gates
     * on, both resets cleared, pads handed over, DCTL = 0 so soft-disconnect is
     * released) GINTSTS read 0x14008020 -- bit 22 fetsusp, forced device suspend --
     * with bit 12 usbrst, bit 13 enumdone and bit 23 resetdet all clear. The host
     * never drove a reset because the core never asserted the D+ pull-up.
     *
     * srp_sessend_value = 0 means "VBus > 0.8V", which is the truth whenever the
     * board is powered from a host -- and on this board that is whenever it is
     * plugged in at all. */
    wr(XT_USB_OTG_CONF,
       (rd(XT_USB_OTG_CONF) & ~XT_USB_SESSEND_VALUE) | XT_USB_SESSEND_OVERRIDE);

    /* Tie the internal FS PHY's OTG status inputs, through the GPIO matrix.
     *
     * This is the piece that was missing, and it is why a fully armed core still
     * drew no reset from the host. The FSLS PHY's OTG state machine is fed four
     * status signals -- iddig, bvalid, vbusvalid, avalid -- as *inputs to the GPIO
     * matrix*, and nothing on this board drives them: VBUS goes to the AXP173 and
     * the USB-C connector's ID pin is not wired to the SoC at all. Floating, the PHY
     * sees no valid session and never lets the core assert the D+ pull-up.
     *
     * IDF's own usb_phy.c does exactly this for device mode (components/usb/
     * usb_phy.c:183-188): iddig = 1 (mini-B side), bvalid = 1 (force device mode),
     * vbusvalid = 1 (valid VBUS present), avalid = 0 (not an A-device/host). Its
     * host-mode branch is the mirror image, plus pull-downs on D+ and D-. Note that
     * avalid must be 0 for device: leaving these unconnected is not "neutral", it is
     * undefined.
     */
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_OTG_IDDIG_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_SRP_BVALID_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_OTG_VBUSVALID_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT,
                                   USB_OTG_AVALID_IN_IDX, false);

    /* The last thing that runs before the console disappears: from here on the
     * breadcrumb trail is the only record, until the next boot prints it. */
    xtensa_breadcrumb("usbClk", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_CLK_EN1));
}

void usb_dw_target_disable_clocks(void)
{
    /* Only the wrap/PHY gate is ours to drop. XT_USB_DEVICE_CLK_EN (bit 10 of
     * PERIP_CLK_EN1) is shared: usb_serial_jtag_ll.h:326 gates USB-Serial-JTAG with the
     * same bit, SYSTEM.perip_clk_en1.usb_device_clk_en. Clearing it on release clocks
     * the console and the flash port out along with the DWC core -- measured, after a
     * release the USJ pad write landed in an unclocked block, D+ never got its pull-up,
     * and the host saw no device at all. Leaving the core clocked while idle costs a
     * little power and buys both paths back. */
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) & ~XT_USB_CLK_EN);

    /* Release the forced clocks so the FIFO power follows the gate again and a
     * second attach starts from the state this one found. */
    wr(XT_USB_OTG_CONF,
       rd(XT_USB_OTG_CONF) & ~(XT_USB_DFIFO_FORCE_PU | XT_USB_PHY_CLK_FORCE |
                              XT_USB_AHB_CLK_FORCE | XT_USB_WRAP_CLK_EN |
                              XT_USB_SESSEND_OVERRIDE));

    /* Give the pins back. USB-Serial-JTAG is the console and the flash port, so
     * leaving the mux pointed at the DWC would silence both after disconnect. */
    /* Release the PHY loads as well as the pads, so the USB-Serial-JTAG console is
     * not left fighting a D+ pull-up and a D- pull-down it does not own. */
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) & ~(XT_USB_PAD_ENABLE |
       XT_USB_PAD_PULL_OVERRIDE | XT_USB_DP_PULLUP | XT_USB_DM_PULLDOWN));
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) & ~XT_SW_USB_PHY_SEL);

    /* The last thing the transport does before the core is torn down. If a crash
     * follows an unplug and this entry is absent, the fault is upstream of the pad
     * release rather than in it. */
    xtensa_breadcrumb("usbRel", DWC_DAINT, DWC_GINTSTS);
}

void usb_dw_target_enable_irq(void)
{
    esp_err_t err = ESP_OK;

    err = ESP_OK;

#ifdef USB_BRINGUP_NO_IRQ
    /* Bring-up only: take no CPU interrupt slot. The slot is not useful while the
     * core's INT output is silent -- usbLin sampled rsr.interrupt at the moment
     * GINTSTS & GINTMSK was non-zero and the mapped slot was not pending -- and taking
     * one starves the audio path. The tick-task dispatch drives the driver instead.
     */
    xtensa_breadcrumb("usbNoIrq", 1, 0);
    return;
#endif

    if (usb_intr == NULL)
    {
        /* LOWMED, and enabled straight away. CherryUSB's dwc2 port for this same
         * controller (port/dwc2/usb_glue_esp.c:163) is the interrupt-driven reference
         * that enumerates on this silicon, and it passes ESP_INTR_FLAG_LOWMED alone --
         * no INTRDISABLED, no later esp_intr_enable(). Rockbox's driver sequence here is
         * clear_irq, enable_irq, 3 ms, soft reconnect, so the slot is enabled either way
         * before the host's first reset arrives; what differs is that the vendor's
         * handler has been live since before the core was configured and has therefore
         * already run once against status raised during configuration. With INTRDISABLED
         * the very first pending event is the one that never draws the line, which is
         * the failure measured on the board. */
        err = esp_intr_alloc(ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LOWMED,
                             usb_dwc_isr, NULL, &usb_intr);
        if (err != ESP_OK)
        {
            /* Silent before: a failed allocation means the core can interrupt
             * forever and nobody ever runs the handler, which is indistinguishable
             * from a dead bus from the outside. Record the code -- ESP_ERR_INVALID_STATE
             * means something else already owns this source. */
            usb_intr = NULL;
            xtensa_breadcrumb("usbIrqE", (unsigned long)err, 0);
            return;
        }
    }
    else
    {
        esp_intr_enable(usb_intr);
        xt_irq_enable_n++;
    }

    if (usb_intr != NULL && xt_irq_enable_n == 0)
    {
        /* Once, at allocation. The driver calls usb_dw_target_enable_irq() on every
         * endpoint operation -- dozens of times during enumeration, thousands during
         * mass storage -- and as a ring entry that pushed everything else out, so the
         * trail read as sixteen identical usbIrq lines. The churn is a counter in the
         * CDC trace now, which is where a count belongs. */
        xtensa_breadcrumb("usbIrq", (unsigned long)(usb_intr != NULL), 0);
    }
}

void usb_dw_target_disable_irq(void)
{
    if (usb_intr != NULL)
        esp_intr_disable(usb_intr);
    xt_irq_disable_n++;
}

void usb_dw_target_clear_irq(void)
{
    /* Clear every pending status bit before the masks are enabled.
     *
     * This hook exists for exactly that, and leaving it empty was the difference
     * between our bring-up and the one that works on this chip. TinyUSB's
     * dwc2_core_init() -- what IDF's usb/device examples run on esp32s2 and esp32s3 --
     * writes 0xFFFFFFFF to gintsts and gotgint before enabling any mask. Status bits
     * that were already set when GINTMSK was programmed never produce a fresh
     * assertion, and that is precisely what we measured: GINTSTS carrying usbrst and
     * enumdone, GINTMSK masking both, GAHBCFG.GINT set, the CPU slot enabled, and
     * rsr.interrupt never showing that slot -- sampled at the instant the masked
     * status was non-zero.
     */
    DWC_GINTSTS = 0xFFFFFFFF;
    DWC_GOTGINT = 0xFFFFFFFF;
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
/* VBUS presence, error-tolerant and debounced.
 *
 * axp173_input_status() returns POWER_INPUT_NONE when the read itself fails, which is
 * indistinguishable from an unplugged cable -- and the poll used to act on it. The
 * first I2C failure after an attach therefore released the pads about two seconds
 * later, on every run, which is why the polled dispatch only ever recorded two or
 * three services and the host never got past its first descriptor read. The PMU and
 * the touchscreen share this bus, and the attach makes the bus busier.
 *
 * A failed read now keeps the last known state, and a removal needs four consecutive
 * absent readings before it is reported -- the same shape as Rockbox's own 200 ms USB
 * debounce, scaled to this poll's period.
 */
static int xt_vbus_present = -1;   /* -1 unknown, 0 absent, 1 present */
static int xt_vbus_miss;
static unsigned long xt_vbus_err;

static int vbus_sample(void)
{
    int last = xt_vbus_present < 0 ? USB_EXTRACTED
                                   : (xt_vbus_present ? USB_INSERTED : USB_EXTRACTED);
    int r = axp173_read_reg(AXP_REG_POWERSTATUS);

    if (r < 0)
    {
        /* Nothing is known about the cable from a failed transaction. */
        xt_vbus_err++;
        return last;
    }

    if (r & AXP_VBUS_PRESENT)
    {
        xt_vbus_present = 1;
        xt_vbus_miss = 0;
        return USB_INSERTED;
    }

    if (++xt_vbus_miss < 4)
        return last;

    xt_vbus_present = 0;
    xt_vbus_miss = 0;
    return USB_EXTRACTED;
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

/* When the current attach began, in ticks. Kept because the trail's usbEnT entry
 * records the gap between attach and teardown, which is the only way to tell a host
 * that dropped the device from a teardown the target initiated. */
static uint32_t xt_attach_tick;

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

#ifdef USB_ENABLE_SERIAL
/*
 * Flash handover, requested by the host over our own CDC.
 *
 * The request is recorded as a flag rather than acted on where it arrives:
 * usb_serial_target_rx() runs in interrupt context, and the sequence below disconnects
 * the device, writes the RTC block and resets the chip. None of that belongs in a
 * handler, so the tick task picks it up.
 */
static volatile bool xt_flash_request;
/* Asked for the retained breadcrumb trail over CDC. The boot console is USJ and the pads
 * are the OTG core's while USB mode is up, so after an unplug that ended in a reboot the
 * trail is only reachable through the transport that survived it. */
static volatile bool xt_trail_request;

void usb_serial_target_rx(const unsigned char *data, int length)
{
    int i;

    for (i = 0; i < length; i++)
    {
        if (data[i] == 'f')
        {
            xt_flash_request = true;
            xtensa_breadcrumb("usbRx", (unsigned long)length, data[i]);
        }
        else if (data[i] == 't')
        {
            xt_trail_request = true;
        }
    }
}

/* USJ CONF0 usb_pad_enable. Computed from the bitfield layout in
 * usb_serial_jtag_struct.h rather than guessed: phy_sel is bit 0, exchg_pins_override
 * 1, exchg_pins 2, vrefh 3:4, vrefl 5:6, vref_override 7, pad_pull_override 8,
 * dp_pullup 9, dp_pulldown 10, dm_pullup 11, dm_pulldown 12, pullup_value 13,
 * usb_pad_enable 14. */
#define XT_USJ_CONF0_PAD_ENABLE   (1u << 14)

/* Hand the port back, then ask the ROM to come up in download mode.
 *
 * The trigger arrives over our CDC, but the ESP32-S3 ROM's USB download mode is the
 * USB-Serial-JTAG device, so this has to give the pads back rather than persist the
 * OTG core. The mux override lives in the RTC block and survives esp_restart(), so
 * without an explicit release the ROM would find D+/D- still routed to a core it does
 * not speak to in download mode. usb_dw_target_disable_clocks() is the same release a
 * detach performs: it clears the wrap's pad_enable and the RTC override together.
 *
 * Then the USJ has to be connected *before* the reset. The ROM chooses between UART and
 * USB download at the very start of boot, and it can only choose USB if it sees a host
 * on these pins -- measured without this, the board entered download mode and
 * enumerated nothing on USB at all, which is the UART path. phy_sel (bit 0) stays clear
 * so the internal PHY is the one used, and no pull override is set: like the OTG core,
 * the USJ drives its own pull-up once its pad is enabled. Its bus clock is the same bit
 * the OTG core uses (SYSTEM.perip_clk_en1.usb_device_clk_en, usb_serial_jtag_ll.h:326),
 * so usb_dw_target_disable_clocks() has to leave that one set -- see the note there.
 *
 * Persist flags are deliberately not set. chip_usb_set_persist_flags(USBDC_PERSIST_ENA)
 * is the ESP32-S2 shape of this, where the ROM's download mode does run on the OTG
 * core; on the S3 it would leave the pads claimed by something the ROM ignores.
 */
static void xt_usb_enter_download_mode(void)
{
    xtensa_breadcrumb("usbFlsh", DWC_DCTL, rd(XT_USB_OTG_CONF));

    DWC_DCTL = SDIS;                /* stop answering the host before the pads move */
    usb_dw_target_disable_clocks(); /* wrap releases the pads */
    REG_WRITE(USB_SERIAL_JTAG_CONF0_REG,
              REG_READ(USB_SERIAL_JTAG_CONF0_REG) | XT_USJ_CONF0_PAD_ENABLE);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    /* Read the request back before the reset, because after it nothing can be read.
     * Measured: an ordinary boot honours FORCE_DOWNLOAD_BOOT and the ROM comes up in
     * USB download mode; a boot that follows a panic reboot resets the chip but the ROM
     * boots the app normally. Whether the bit is actually set at this point, and what
     * the pad state looks like, is the only way to tell "the write did not take" from
     * "the ROM ignored it". */
    xtensa_breadcrumb("usbDl",
                      REG_READ(RTC_CNTL_OPTION1_REG),
                      REG_READ(USB_SERIAL_JTAG_CONF0_REG));
    esp_restart();
}
#endif /* USB_ENABLE_SERIAL */

static void usb_vbus_poll(void)
{
    static unsigned ticks;
    static unsigned report;
    static unsigned slow;
    static int last = USB_EXTRACTED;
    int status;

    /* While the core is armed, carry its state to the trail every ~4 s. Two
     * entries, oldest-first on the next boot:
     *   usbIRQ  a = interrupt count, b = GINTSTS
     *   usbDsts a = DSTS (bit 0 suspend, bits 1:2 enum speed), b = DAINT
     * DSTS enum speed is the answer to "did the host finish enumerating us at
     * full speed", and DAINT says which endpoints the core thinks are active.
     * A count stuck at 0 with DSTS unchanging means the interrupt never arrives;
     * a growing count means the core sees the bus and the driver is the problem.
     */
    /* 10 s, in ticks: HZ is 100 (kernel/include/tick.h), so this block runs ten times
     * a second at most. It used to read "4 * USB_VBUS_POLL_TICKS / 4", which is just
     * USB_VBUS_POLL_TICKS -- 40 ms -- and turned the report into a 23 line/s flood on
     * the CDC endpoint. */
    if (xt_armed && ++report >= 10 * HZ)
    {
        report = 0;
        xtensa_breadcrumb("usbIRQ", xt_usb_irqs, DWC_GINTSTS);
        xtensa_breadcrumb("usbDsts", DWC_DSTS, DWC_DAINT);
        /* Source attribution from the handler, not from this snapshot. */
        xtensa_breadcrumb("usbSrc", xt_irq_iep, xt_irq_oep);
        xtensa_breadcrumb("usbRstE", xt_irq_rst, xt_irq_enum);
        xtensa_breadcrumb("usbDain", xt_irq_daint_in, xt_irq_daint_out);
        xtensa_breadcrumb("usbNone", xt_irq_none, 0);

#ifdef USB_ENABLE_SERIAL
        /* Same numbers out of the CDC IN endpoint. The breadcrumb ring only reaches
         * the console at the *next* boot, and during an attach the USJ console is gone
         * because the OTG core owns the pads, so a live attach is otherwise invisible.
         *
         * logf would be the natural route and does push to usb_serial_send, but that
         * push is gated on HAVE_SERIAL && LOGF_SERIAL (firmware/logf.c:244), which are
         * iPod-only defines; HAVE_SERIAL would also pull drivers/serial.c into the
         * build, which expects target serial hardware we do not have. So this writes
         * the class directly. usb_serial_send() returns without effect when the class
         * is not active, so this is harmless before enumeration. Tick-task context, as
         * everything here is: the send path takes an endpoint, not a lock. */
        {
            /* The sticky DAINT masks say which endpoints the OEPINT/IEPINT counts
             * belong to. oep runs at 2.4/s against iep at 0.3/s, and nothing in this
             * profile should be receiving that much: knowing whether it is EP0 or the
             * CDC OUT endpoint is the difference between a chattering host and a driver
             * that re-primes an endpoint it should not. */
            static char cdc_line[192];
            int n = snprintf(cdc_line, sizeof(cdc_line),
                             "rockbox: irq=%lu iep=%lu oep=%lu rst=%lu "
                             "enum=%lu none=%lu early=%lu en=%lu/%lu "
                             "dain=%04lx,%04lx gintsts=%08lx dsts=%08lx\r\n",
                             xt_usb_irqs, xt_irq_iep, xt_irq_oep, xt_irq_rst,
                             xt_irq_enum, xt_irq_none, xt_irq_early,
                             xt_irq_enable_n, xt_irq_disable_n,
                             xt_irq_daint_in, xt_irq_daint_out,
                             DWC_GINTSTS, DWC_DSTS);
            if (n > 0)
                usb_serial_send((unsigned char *)cdc_line, n);
        }
#endif

        /* Slower cadence on purpose: the ring is 16 deep, and usbIRQ/usbDsts are
         * the entries whose *history* matters. These two are a snapshot. */
        if (++slow >= 3)
        {
            slow = 0;
            xtensa_breadcrumb("usbMask", DWC_GINTMSK, DWC_GAHBCFG);
            xtensa_breadcrumb("usbCpu", xt_read_intenable(), xt_read_interrupt());
            /* Who owns the pads, and whether the core is connected at all.
             * OTG_CONF bit 18 is pad_enable and bit 2 is phy_sel; DCTL bit 0 is
             * soft-disconnect, which would explain a device that holds D+ high and
             * never answers if something left it set. */
            xtensa_breadcrumb("usbWrap", rd(XT_USB_OTG_CONF), rd(XT_RTC_USB_CONF));
            xtensa_breadcrumb("usbDctl", DWC_DCTL, DWC_PCGCCTL);
            /* Failed PMU reads during the attach: this counter says whether the bus
             * is the problem or the core is. */
            xtensa_breadcrumb("usbVbus", xt_vbus_err, (unsigned long)xt_vbus_present);
            /* Which transceiver the core was told to use. The whole story of the
             * last two runs hinges on bit 6 here. */
            xtensa_breadcrumb("usbGusb", DWC_GUSBCFG, DWC_DCFG);
            /* EP0 in detail. DOEPINT bit 13 is setup-packet-complete (STUP) and bit
             * 0 is transfer-complete (XFRC); the EPTSIZ low 7 bits of a completed
             * OUT stage say how many bytes actually arrived. A host that gets a
             * protocol error (-71) rather than a timeout is being answered, and
             * these say whether the answer was the wrong size or the wrong thing. */
            xtensa_breadcrumb("usbEp0", DWC_DOEPINT(0), DWC_DIEPINT(0));
            /* DWC_EPTSIZ's direction argument is usb-designware.c's private enum
             * (IN = 0 -> DIEPTSIZ at 0x910, OUT = 1 -> DOEPTSIZ at 0xB10), which the
             * target cannot name, so the values are spelled out here. */
            xtensa_breadcrumb("usbSz", DWC_EPTSIZ(0, 1), DWC_EPTSIZ(0, 0));
        }
    }

#ifdef USB_ENABLE_SERIAL
    /* Picked up here rather than in the hook: this runs on the tick task thread. */
    if (xt_trail_request)
    {
        /* All 40 retained slots, in chunks. Reading the trail over CDC exists to see
         * the sequence that led to a crash, and 16 slots turned out to be all noise and
         * none of the story. A small buffer reused per chunk, because the static region
         * cannot hold forty formatted lines. */
        static char trail[352];
        int start;

        xt_trail_request = false;
        for (start = 0; start < 40; start += 10)
        {
            int n = xtensa_breadcrumb_window(trail, sizeof trail, start, 10);
            if (n <= 0)
                break;
            usb_serial_send((unsigned char *)trail, n);
        }
        usb_serial_send((unsigned char *)"trail end\r\n", 10);
    }

    if (xt_flash_request)
    {
        xt_flash_request = false;
        xt_usb_enter_download_mode();
    }
#endif

    if (++ticks < USB_VBUS_POLL_TICKS)
        return;
    ticks = 0;

    /* Cable state, posted whenever it changes. usb.c turns an insertion event into
     * USB mode, and that is core behaviour this target does not override: what the
     * usb_mode setting changes -- which classes get composed, whether the volumes get
     * handed to the host -- is usb.c's business, not the target's. The consequence on
     * this hardware is that a player plugged into a host is in USB mode, because
     * charging here is not optional.
     */
    status = vbus_sample();

    if (status != last)
    {
        /* The unplug goes through here and the crash happens just after it, so the
         * transition belongs in the trail: whether the cable event was seen at all,
         * and when, is half of the question. */
        last = status;
        xtensa_breadcrumb("usbEv", (unsigned long)status, current_tick);
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
    /* The cable, answered as soon as the boot hook has put the pads back. This used
     * to be gated behind a 120-second window so that a flash attempt could not race
     * the attach; the window is gone because the host can now ask for the handover
     * over USB instead, which is both faster and something the player does on its own.
     */
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
/* Tried and refuted: the ROM's usb_enable() is not a bring-up helper you can borrow.
 * --wrap=usb_enable means __real_usb_enable resolves to the ROM symbol at 0x4000297c,
 * and calling it panicked immediately with LoadProhibited at PC 0x400533d1 in a boot
 * loop. The ROM's usb_enable belongs to a complete device stack in ROM (usb_transfer,
 * usb_set_config, usb_dc_ep_set_callback, g_usb_print in .data.interface), not to a
 * separable PHY-and-clock setup, so it has to be entered the way the mask ROM enters it.
 * The lower-level chip_usb_dw_init (0x400026f4) is the piece worth trying later; the
 * bring-up here stays hand-rolled meanwhile. */

void __wrap_usb_enable(bool on)
{
#ifdef USB_ALLOW_ATTACH
    xtensa_breadcrumb("usbEn", on ? 1 : 0, current_tick);

    if (!on && xt_attach_tick != 0)
    {
        /* When a teardown happens relative to the attach, in ticks. The kernel shows
         * the pads coming back a couple of seconds after an attach on some runs and
         * the full 90 seconds on others, and the trail cannot say which without a
         * clock: 200 ticks is Rockbox releasing the transport on its own, 9000 is the
         * bring-up judge. */
        xtensa_breadcrumb("usbEnT", current_tick - xt_attach_tick, xt_attach_tick);
    }

    if (on)
    {
        xt_usb_irqs = 0;
        xt_armed = true;
        usb_core_init();   /* usb_drv_init(): clocks, pads, device mode, connect */


    }
    else
    {
        xt_armed = false;
        usb_core_exit();   /* usb_drv_exit(): soft-disconnect, and the pads back */
    }

    /* Both halves of the register state, recorded before anything can change it.
     * The trail survives the reset, and app_main() replays it inside the boot
     * window, so one power cycle tells us what the previous boot did -- including
     * whether this function was called with false, which is the other explanation
     * for a report that reads all zeroes. */
    xtensa_breadcrumb(on ? "usbIn" : "usbOut",
                      DWC_GSNPSID, rd(XT_USB_OTG_CONF));
    xtensa_breadcrumb("usbClk2", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_RST_EN0));

    /* The register state after the attach, recorded rather than panicked. The
     * earlier version of this slept 8 s and called panicf() so the values would
     * reach the LCD -- but PRINT_REBOOT turned that into the 10-second reboot loop
     * the kernel log shows, and the trail carries the same numbers to a live console
     * on the next boot anyway. Nothing here should stop the device.
     *
     * Reading the trail: DCTL bit 0 is soft-disconnect (clear means the core is
     * connected); GINTSTS bit 12 usbrst, bit 13 enumdone and bit 23 resetdet are the
     * only evidence a host ever touched the bus; GUSBCFG bit 5 fsintf and bit 30
     * fdmod say the PHY and device-mode writes took; OTG_CONF bit 18 pad_enable says
     * the mux moved the pins, and bit 0 the session-end override.
     */
    if (on)
    {
        xtensa_breadcrumb("usbPost", DWC_DCTL, DWC_GINTSTS);
    }

    /* Bring-up only: collect ~90 s of interrupt and bus state, then restart the
     * chip exactly once per power-on.
     *
     * Why a restart rather than a report on screen: the breadcrumb trail lives in
     * .noinit, which survives a chip reset and is destroyed by a power-on reset.
     * With the pins handed to the OTG core there is no console to read, so the only
     * way to get this data out is for the firmware to reset itself -- the boot hook
     * puts the pins back on USB-Serial-JTAG, and app_main() replays the trail on a
     * live console. The once-per-power-on flag is what kept the earlier version from
     * looping: a panic every 10 seconds is how the board ended up popping the amp
     * over and over.
     */
    if (on)
        xt_attach_tick = current_tick;
#else
    (void)on;
#endif
}

#endif /* HAVE_USBSTACK */
