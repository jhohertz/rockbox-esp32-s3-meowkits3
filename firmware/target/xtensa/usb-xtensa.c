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
 * STATUS: operational. Rockbox's firmware/drivers/usb-designware.c drives the DWC
 * core through the hooks in this file, and the CDC console, mass storage and the DAC
 * class all enumerate on this board. The hardware check that used to abort bring-up
 * (usb_dw_check_hw() comparing GUSBCFG back after writing the PHY field, failing as
 * "PHY type not supported") no longer fires -- the PHYSEL override that made it pass
 * was later deleted as dead code, not worked around. IDF's USB-Serial-JTAG keeps the
 * pads until USB mode takes them over, which is why the console and esptool go quiet
 * while a transport is attached; nothing in Rockbox's USB path goes through IDF.
 * See __wrap_usb_enable() below for why the ROM's usb_enable() cannot be borrowed
 * instead of this bring-up.
 *
 * The pins are the other half of the story. The DWC does not own them: a USB wrap
 * muxes the internal FSLS PHY between the DWC and the USB-Serial-JTAG block, and
 * the mux only answers to software when the RTC's usb_conf says so. Both have to be
 * set -- usb_dw_target_enable_clocks() does it. Because the console and esptool both
 * live on USB-Serial-JTAG, that handover silences the trace and the flash port, so
 * the pads move only while USB mode is active and return to USJ on reset.
 ****************************************************************************/
#include "config.h"
#include "debug.h"           /* dbg_breadcrumb() */
#include "system.h"
#ifdef USB_ENABLE_SERIAL
#include <stdio.h>          /* snprintf */
#include "usbstack/usb_serial.h"
#include "usb.h"               /* usb_drv_stall(), to recover the control endpoint */
#endif
#ifdef USB_ENABLE_AUDIO
#include "usbstack/usb_audio.h"   /* state getters, for the trace line */
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
#include "cache-xtensa.h"

/* Device FIFO RAM. IDF's usb_dwc_cfg.h says OTG_DFIFO_DEPTH 256, but the core
 * itself says otherwise: GHWCFG3[31:16] reads 200 words on this silicon
 * (hwcfg3=0x00c804b5, measured at boot). The register wins -- a split sized for
 * 256 words would program DIEPTXF registers whose start addresses run past the
 * end of the RAM, which shows up as IN transfers that never complete rather
 * than as anything that names the cause.
 *
 * TinyUSB ships the same wrong number: its dwc2_esp32.h controller table
 * hardcodes .otg_dfifo_depth = 256 for ESP32-S2/S3, so a UAC device built on
 * it over-commits the FIFO by 56 words. Note also that its dfifo accounting
 * adds EP_LOC_CNT to the register field; GHWCFG4[3:0] reads 0 on this core,
 * so that correction adds nothing here and 200 is the whole SPRAM. */
#define XT_FIFO_RAMSZ   200

/* EP0 IN. Full speed caps the packet at 64 bytes = 16 words. */
#define NPTX_FIFOSZ     16
/* How many dedicated transmit FIFOs actually exist. GHWCFG4[29:26] reads 4
 * on this core (hwcfg4=0xd3f0a030, measured at boot), and usb_dw_check_hw()
 * clamps n_ptxfifos to that number however many endpoints are declared, so
 * reserving room for USB_NUM_ENDPOINTS-1 of them budgets 32 words for FIFOs
 * that will never be programmed. Both profiles need exactly four IN
 * endpoints -- CDC bulk IN, CDC notification IN, HID IN, and then either the
 * storage bulk IN or the audio feedback IN -- so 4 is also what is used, not
 * just what is available. */
#define XT_PTX_FIFOS    4
/* Each dedicated IN FIFO must hold one whole packet of the endpoint that owns
 * it. This is not a nicety: usb_dw_handle_dtxfifo() (the path compiled here,
 * since USB_DW_SHARED_FIFO is unset) pushes whole packets only --
 *
 *     maxpktwords = max_packet_size >> 2;              // 64/4 = 16
 *     words = (fifospace / maxpktwords) * maxpktwords; // (8/16)*16 = 0
 *     if (!words) break;
 *
 * With a 32-byte FIFO and a 64-byte bulk packet that computes zero words and
 * breaks, forever. Mass storage therefore enumerated (EP0 runs on the NPTX
 * FIFO, 16 words) and then never answered INQUIRY, READ_CAPACITY or a CSW, so
 * the host showed a device with no volume. That was the regression 1df51c1468
 * introduced when this value went from 16 to 8 to buy RX room; the guard that
 * used to reject it, `PTX_FIFOSZ >= 64 / 4`, was rewritten as `>= 16 / 4` --
 * the CDC notification payload -- so the build said nothing.
 *
 * Going back to 16 costs audio nothing, because the RX side no longer pays
 * for six FIFOs: 200 - 16 - 4*16 = 120 words = 480 bytes, against the 88
 * words = 352 bytes that the old six-FIFO reservation left when this value
 * was 16. The audio bug 1df51c1468 was fixing -- an isochronous OUT packet
 * with nowhere to land -- stays fixed at 480 bytes: one 320-byte isochronous
 * packet is 80 words, plus two 64-byte control packets at 16 each, which
 * commits 112 of the 120. That is the whole budget, which is why the
 * isochronous packet cannot grow: two consecutive frames plus one control
 * packet is what Synopsys' allocation rule asks for, and a 384-byte frame
 * needs more than the entire 200-word RAM. See the note on
 * USB_AUDIO_BUFFER_SIZE in config/meowkits3.h. */
#define PTX_FIFOSZ      16
#define RX_FIFOSZ       (XT_FIFO_RAMSZ - NPTX_FIFOSZ - \
                         PTX_FIFOSZ * XT_PTX_FIFOS)

_Static_assert(PTX_FIFOSZ >= 64 / 4,
               "a dedicated IN FIFO cannot hold one full-speed packet");
_Static_assert(RX_FIFOSZ >= (USB_AUDIO_BUFFER_SIZE / 4) + 2 * (64 / 4),
               "RX FIFO cannot hold an isochronous packet plus the two control packets");
_Static_assert(NPTX_FIFOSZ + PTX_FIFOSZ * XT_PTX_FIFOS + RX_FIFOSZ
               <= XT_FIFO_RAMSZ,
               "device FIFO split exceeds the core's measured FIFO RAM");

const struct usb_dw_config usb_dw_config = {
    /* The internal full-speed PHY. Two bits, and we were only setting one:
     *
     *   fsintf (bit 5) -- the full-speed *interface*
     *   physel (bit 6) -- selects the embedded FS/LS PHY over the embedded HS PHY
     *
     * PHYSEL (bit 6) is not something this core can be talked out of: GHWCFG2
     * reports no high-speed PHY, so there is nothing for the bit to choose
     * between, and it reads back set no matter what the driver assigns.
     * Measured: with phytype left at FSINTF -- i.e. the driver's GUSBCFG write
     * not asking for PHYSEL at all -- the register still reads 0x40001460, bit
     * 6 included. Adding PHYSEL to phytype therefore changed nothing in that
     * register, and the run in which the connection disappeared was the one
     * that also turned on the wrap's pad_pull_override; that, not PHYSEL, is
     * what took the connection away (see usb_dw_target_enable_clocks).
     *
     * The 0x40001460 reading above was taken with the enumeration-era attach
     * trail, which has since been removed; GUSBCFG is no longer mirrored
     * anywhere on the device.
     */
    .phytype       = FSINTF,
    .rx_fifosz     = RX_FIFOSZ,
    .nptx_fifosz   = NPTX_FIFOSZ,
    .ptx_fifosz    = PTX_FIFOSZ,
    /* AHB burst length. Every other DWC2 target in the tree programs a burst --
     * s5l8700 INCR4, as3525 and s5l8702 INCR8, stm32h7 INCR8, x1000 INCR16 -- and
     * TinyUSB's dwc2 common init, which is what actually drives this core in production
     * ESP32 code, sets GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_2 (dwc2_common.c:238), where
     * HBSTLEN_2 is field value 3, the same encoding as HBSTLEN_INCR4 here. So INCR4 is
     * both the Rockbox convention and the value the vendor's own USB stack uses on this
     * silicon.
     *
     * ESP-IDF's bare HAL leaves it at Single (usb_dwc_hal.c:92) and only raises
     * it to INCR for esp32-s2 below revision 1.00, to dodge an AHB arbiter
     * errata that could corrupt other peripherals' DMA bursts; that errata is
     * not applied to esp32s3, so a burst here is not working around anything
     * the chip requires.
     *
     * The mirror of that note: a burst holds the arbiter, so peripherals
     * sharing the AHB with this core (audio GDMA, I2C, the flash cache path)
     * see less arbitration. If a GDMA or I2C regression ever appears on this
     * board, this field is the first thing to put back to HBSTLEN_SINGLE.
     *
     * It is raised to INCR16 to test the mass-storage/DAC case: with MSC
     * flooding bulk reads, the isochronous OUT packet waits on ISR service, and
     * the RX FIFO is 120 words = 480 bytes, only about 1.5 packets of tolerance
     * at 48 kHz. A longer burst drains each FIFO read in fewer AHB
     * transactions, which is the quantity that decides whether a packet
     * overflows while storage is being serviced. INCR16 is 16 beats of 4 bytes
     * = one full-speed packet, so it is also the largest burst that means
     * anything here. Judge it by the debug menu's "Frames dropped" count
     * (apps/debug_menu.c:2582, from usb_audio_get_frames_dropped()) with MSC
     * flooded, before and after -- not by the arithmetic. If it does not help,
     * the value to fall back to is INCR4, which is what the vendor's own stack
     * programs; if it helps but disturbs audio GDMA or I2C, INCR8 is the middle
     * step that as3525, s5l8702 and stm32h7 use.
     */
    .ahb_burst_len = HBSTLEN_INCR16,
    /* Rx FIFO thresholding stays off, as on x1000 and stm32h7: Synopsys
     * recommends disabling it because it misbehaves under certain error
     * conditions. */
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

/* The interrupt is IDF-owned now. The bare-metal bring-up routed it by hand
 * to XT_SLOT_USB through the vector table; under ESP-IDF the allocator does
 * that, so the enable/disable hooks below are a handle, not an INTENABLE
 * bit. */
static intr_handle_t usb_intr = NULL;

/* Counting core interrupts, and nothing else. A host that sees a connection
 * but gets no reply to GET_DESCRIPTOR (kernel: "device descriptor read/64,
 * error -110") is either a core that never interrupts or a driver that
 * mishandles the interrupt; this counter separates the two, and the poll
 * below carries it to the breadcrumb trail because the console is gone by
 * then. */
static volatile uint32_t xt_usb_irqs;
/* Counted separately from xt_usb_irqs, which is incremented only inside
 * usb_dwc_isr() and therefore means "the interrupt was delivered" and
 * nothing else. The polled stand-in calls the driver directly so it cannot
 * inflate that claim. */

/* How many times the post-init one-shot service fired, i.e. how much status
 * the core had already latched by the time the interrupt was enabled. */

/* enable/disable churn from the driver, counted rather than trailed: see the
 * note in usb_dw_target_enable_irq(). */
static volatile uint32_t xt_irq_enable_n;
static volatile uint32_t xt_irq_disable_n;

/* Set between usb_enable(true) and usb_enable(false), purely so the poll can
 * report what the armed core is doing. Deliberately not used by
 * usb_detect(), which answers the cable as usb.h:80 requires. Declared here
 * because the handler below needs it: the interrupt is allocated before
 * usb_core_init() has configured the core (at boot, for the routing dump),
 * so the handler has to know which phase it is in. */
static bool xt_armed;

#ifdef USB_ENABLE_AUDIO
/* Sampling rate for the interrupt-side audio breadcrumb, in endpoint events. */

#endif

static void usb_dwc_isr(void *arg)
{
    (void)arg;
    /* Nothing may print here: printf takes the newlib stdout lock, which aborts
     * from interrupt context. See the note on gdma_tx_isr(). */
    xt_usb_irqs++;
    if (!xt_armed)
    {
        /* The interrupt is live from allocation, so this can run while the
         * core is still being configured, before the driver has masks anyone
         * cares about. Clear the status rather than return: on a level
         * source a bit left set keeps the line drawn and re-enters the
         * handler with nothing to do -- and clearing pending bits is the
         * only thing that has ever made this core assert again for a later
         * event. */
        DWC_GINTSTS = 0xFFFFFFFF;
        DWC_GOTGINT = 0xFFFFFFFF;
        return;
    }
    INT_USB_FUNC();

#ifdef USB_ENABLE_AUDIO
#endif
}

void usb_dw_target_enable_clocks(void)
{
    /* Two gates, and the second one is the easy one to miss: usb_clk_en (bit
     * 23 of PERIP_CLK_EN0) is the wrap and PHY domain, usb_device_clk_en
     * (bit 10 of PERIP_CLK_EN1) is the DWC core. With only the first, every
     * OTG register reads 0x00000000 and nothing faults -- which looks like a
     * dead block rather than a missing clock. */
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) | XT_USB_CLK_EN);
    wr(XT_PERIP_CLK_EN1, rd(XT_PERIP_CLK_EN1) | XT_USB_DEVICE_CLK_EN);
    wr(XT_PERIP_RST_EN0, rd(XT_PERIP_RST_EN0) & ~XT_USB_RST);
    wr(XT_PERIP_RST_EN1, rd(XT_PERIP_RST_EN1) & ~XT_USB_DEVICE_RST);

    /* The mux needs both halves: the RTC bit that lets software override it,
     * and the wrap bit that routes the FSLS PHY to it. phy_sel selects an
     * *external* PHY, so XT_USB_PHY_SEL_EXT stays clear. */
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) |
       (XT_SW_HW_USB_PHY_SEL | XT_SW_USB_PHY_SEL));
    /* No pull override here, deliberately. IDF 4.4's
     * usb_phy_ll_int_load_conf() did set pad_pull_override with D+ pull-up
     * and D- pull-down for device mode, and adding it measured worse: with
     * the override on, the host stopped seeing a connection during an attach
     * entirely (no descriptor attempts at all, where before it had at least
     * attempted some), and GINTSTS lost its usbrst bit. IDF's current path
     * -- usb_new_phy(), which is what the TinyUSB device examples use --
     * only calls usb_wrap_ll_phy_enable_pull_override() in the *host*
     * branch, so on 5.5 a device leaves the override alone and the core
     * drives the pull-up through its own connect. That is what this does
     * too, and the measured difference agrees: with the override on, GINTSTS
     * lost usbrst and the host made no enumeration attempt at all; with it
     * off, the host reads the descriptors, binds cdc_acm and creates a tty. */
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) | XT_USB_PAD_ENABLE |
                        XT_USB_WRAP_CLK_EN);

    /* The three force bits are not set by IDF's driver at all -- IDF's
     * device stack reaches the core through its own HCD -- but they are what
     * made this core answer on this board. The pad-free boot probe read
     * GSNPSID = 0x00000000 with both gates on and both resets cleared, and
     * 0x4f54400a only after USB_WRAP_CLK_EN, DFIFO_FORCE_PU, PHY_CLK_FORCE
     * and AHB_CLK_FORCE were set. A block whose FIFO RAM is powered down
     * reads as all zeroes rather than faulting, and a core with no PHY clock
     * cannot drive D+ -- which is the exact symptom: the host sees no device
     * at all, not a bad one. */
    wr(XT_USB_OTG_CONF,
       (rd(XT_USB_OTG_CONF) & ~XT_USB_DFIFO_FORCE_PD) |
       XT_USB_DFIFO_FORCE_PU | XT_USB_PHY_CLK_FORCE | XT_USB_AHB_CLK_FORCE);

    /* Force the session-end signal to "VBUS present".
     *
     * This board routes VBUS to the AXP173 only -- the SoC has no VBUS input,
     * and GPIO19/20 are D+/D-. With OTG_CONF.srp_sessend_override clear, IDF's
     * own usb_wrap_ll says the signal is "controlled by the chip input", and
     * there is no chip input, so the DWC core sees a floating session-end and
     * behaves as if VBus were below 0.2 V. Measured on hardware: after a full
     * attach (both gates on, both resets cleared, pads handed over, DCTL = 0 so
     * soft-disconnect is released) GINTSTS read 0x14008020 -- bit 22 fetsusp,
     * forced device suspend -- with bit 12 usbrst, bit 13 enumdone and bit 23
     * resetdet all clear. The host never drove a reset because the core never
     * asserted the D+ pull-up.
     *
     * srp_sessend_value = 0 means "VBus > 0.8V", which is the truth whenever
     * the board is powered from a host -- and on this board that is whenever
     * it is plugged in at all. */
    wr(XT_USB_OTG_CONF,
       (rd(XT_USB_OTG_CONF) & ~XT_USB_SESSEND_VALUE) | XT_USB_SESSEND_OVERRIDE);

    /* Tie the internal FS PHY's OTG status inputs, through the GPIO matrix.
     *
     * This is the piece that was missing, and it is why a fully armed core
     * still drew no reset from the host. The FSLS PHY's OTG state machine is
     * fed four status signals -- iddig, bvalid, vbusvalid, avalid -- as *inputs
     * to the GPIO matrix*, and nothing on this board drives them: VBUS goes to
     * the AXP173 and the USB-C connector's ID pin is not wired to the SoC at
     * all. Floating, the PHY sees no valid session and never lets the core
     * assert the D+ pull-up.
     *
     * IDF's own usb_phy.c does exactly this for device mode (components/usb/
     * usb_phy.c:183-188): iddig = 1 (mini-B side), bvalid = 1 (force device
     * mode), vbusvalid = 1 (valid VBUS present), avalid = 0 (not an
     * A-device/host). Its host-mode branch is the mirror image, plus pull-downs
     * on D+ and D-. Note that avalid must be 0 for device: leaving these
     * unconnected is not "neutral", it is undefined.
     */
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_OTG_IDDIG_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_SRP_BVALID_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_OTG_VBUSVALID_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT,
                                   USB_OTG_AVALID_IN_IDX, false);

    /* The device is now armed: from here the host can drive frames at us, and
     * its interrupt cannot wake the core out of light sleep. Hold the lock
     * until the clocks come back down. See xt_light_sleep_block() for the
     * arithmetic.
     */
    xt_light_sleep_block(true);

    /* The last thing that runs before the console disappears: from here on the
     * breadcrumb trail is the only record, until the next boot prints it. */
    dbg_breadcrumb("usbClk", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_CLK_EN1));
}

void usb_dw_target_disable_clocks(void)
{
    /*
     * Order matters here, and the previous order is what the crash markers exposed.
     * Every access has to happen while the block it targets is still clocked:
     *
     *   1. core registers, while the OTG clock is on;
     *   2. the pad mux and PHY loads, while the USB wrap clock is on;
     *   3. only then the forced-clock bits and the wrap gate.
     *
     * The old sequence dropped the wrap gate first, then wrote the pad
     * registers, which is the measured "the pad write landed in an unclocked
     * block, D+ never got its pull-up and the host saw no device" failure --
     * and it read DWC_DAINT/GINTSTS last, on a block that had already been
     * ungated, which is a bus stall of the same family as the PCGCCTL hang.
     *
     * The final marker reads RTCCNTL and the SoC clock-enable register only:
     * both are always clocked, so they are safe to take after the wrap gate has
     * gone.
     */
    dbg_breadcrumb_retained("usbRel", DWC_DAINT, DWC_GINTSTS);

    /* Give the pins back, and release the PHY loads, while the wrap clock
     * still reaches the pad mux. USB-Serial-JTAG is the console and the
     * flash port, so leaving the mux pointed at the DWC would silence both
     * after disconnect, and leaving D+ pulled up would fight the USJ
     * peripheral for the bus. */
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) & ~(XT_USB_PAD_ENABLE |
       XT_USB_PAD_PULL_OVERRIDE | XT_USB_DP_PULLUP | XT_USB_DM_PULLDOWN));
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) & ~XT_SW_USB_PHY_SEL);

    /* Release the forced clocks so the FIFO power follows the gate again and
     * a second attach starts from the state this one found. */
    wr(XT_USB_OTG_CONF,
       rd(XT_USB_OTG_CONF) & ~(XT_USB_DFIFO_FORCE_PU | XT_USB_PHY_CLK_FORCE |
                              XT_USB_AHB_CLK_FORCE | XT_USB_WRAP_CLK_EN |
                              XT_USB_SESSEND_OVERRIDE));

    /* Last: the wrap/PHY gate. XT_USB_DEVICE_CLK_EN (bit 10 of
     * PERIP_CLK_EN1) stays set on purpose -- usb_serial_jtag_ll.h:326 gates
     * USB-Serial-JTAG with that same bit, so clearing it clocks out the
     * console and the flash port along with the core. */
    wr(XT_PERIP_CLK_EN0, rd(XT_PERIP_CLK_EN0) & ~XT_USB_CLK_EN);

    /* Only RTCCNTL and the SoC clock-enable register are readable here: the
     * wrap block has just been ungated, and reading it would stall the bus
     * the way PCGCCTL did. */
    /* USB is down and the PHY is powered back to its reset state, so the
     * idle task may sleep again. Paired with the acquire in enable_clocks(). */
    xt_light_sleep_block(false);

    dbg_breadcrumb_retained("usbEnd", rd(XT_RTC_USB_CONF), rd(XT_PERIP_CLK_EN0));
}

void usb_dw_target_enable_irq(void)
{
    esp_err_t err = ESP_OK;

    err = ESP_OK;

#ifdef USB_BRINGUP_NO_IRQ
    /* Bring-up only: take no CPU interrupt slot. The slot is not useful while
     * the core's INT output is silent -- usbLin sampled rsr.interrupt at the
     * moment GINTSTS & GINTMSK was non-zero and the mapped slot was not pending
     * -- and taking one starves the audio path. The tick-task dispatch drives
     * the driver instead.
     */
    dbg_breadcrumb("usbNoIrq", 1, 0);
    return;
#endif

    if (usb_intr == NULL)
    {
        /* LOWMED, and enabled straight away. CherryUSB's dwc2 port for this
         * same controller (port/dwc2/usb_glue_esp.c:163) is the
         * interrupt-driven reference that enumerates on this silicon, and it
         * passes ESP_INTR_FLAG_LOWMED alone -- no INTRDISABLED, no later
         * esp_intr_enable(). Rockbox's driver sequence here is clear_irq,
         * enable_irq, 3 ms, soft reconnect, so the slot is enabled either
         * way before the host's first reset arrives; what differs is that
         * the vendor's handler has been live since before the core was
         * configured and has therefore already run once against status
         * raised during configuration. With INTRDISABLED the very first
         * pending event is the one that never draws the line, which is the
         * failure measured on the board. */
        /* Level 2, not the allocator's default level 1. The audio GDMA
         * interrupt is allocated at ESP_INTR_FLAG_LEVEL1 (gdma_init() in
         * i2s-xtensa.c) and runs the mixer pass at that level, so with USB also
         * at level 1 the two are serialised by IDF's dispatcher and a packet
         * completion cannot preempt a pass. Measured on the board with the log
         * path enabled: ~530 accepted 196-byte packets per second against ~1000
         * on the wire, ~100 kB/s arriving against a proven 192 kB/s consumed. A
         * full-speed isochronous packet carries exactly one frame and has no
         * retry, so every millisecond the handler waits is a frame of audio
         * silently gone -- the core discards it and reports nothing. Level 2
         * gives the transport priority over the mixer.
         *
         * Level 2 is already the top of the tree on this build, which is worth
         * stating because it is easy to assume otherwise. Everything else sits
         * at level 1: IDF's FreeRTOS tick, because sdkconfig selects
         * CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL1 and the ESP_INTR_FLAG_LEVEL3
         * line at components/freertos/port_systick.c:64 is inside #ifdef
         * CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL3, which is not set here; I had
         * this wrong and read that line as unconditional. esp_timer is pinned
         * to level 1 on this chip (its Kconfig gives range 1 1 when the target
         * is not esp32). The SD host is allocated with flags 0 at
         * sdmmc_host.c:539, and intr_alloc.c turns a zero level mask into level
         * 1. IDF's I2C master passes ESP_INTR_FLAG_LOWMED, which accepts level
         * 1 or 2 and takes the lowest free one -- level 1 has free slots, so it
         * lands there.
         *
         * Two consequences. Raising USB to level 3 would preempt nothing, since
         * nothing runs above 2, and IDF chains handlers at the same level
         * rather than nesting them. And CONFIG_FREERTOS_HZ cannot be what
         * delays this handler: halving the tick only halves level-1 traffic,
         * which never blocks a level-2 entry -- the tick chains with the audio
         * GDMA pass, not with the transport. The only thing that can hold a
         * packet completion out is a region with interrupts disabled:
         * esp_cpu_enter_critical(), Rockbox's own disable_irq_save(), or IDF's
         * flash/cache-disable window.
         *
         * The handler is flash-resident and runs with the cache enabled,
         * which is the only requirement IDF places on a non-IRAM handler
         * above level 1. */
        err = esp_intr_alloc(ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LEVEL2,
                             usb_dwc_isr, NULL, &usb_intr);
        if (err != ESP_OK)
        {
            /* Silent before: a failed allocation means the core can
             * interrupt forever and nobody ever runs the handler, which is
             * indistinguishable from a dead bus from the outside. Record the
             * code -- ESP_ERR_INVALID_STATE means something else already
             * owns this source. */
            usb_intr = NULL;
            dbg_breadcrumb("usbIrqE", (unsigned long)err, 0);
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
        /* Once, at allocation. The driver calls usb_dw_target_enable_irq()
         * on every endpoint operation -- dozens of times during enumeration,
         * thousands during mass storage -- and as a ring entry that pushed
         * everything else out, so the trail read as sixteen identical usbIrq
         * lines. The churn is a counter in the CDC trace now, which is where
         * a count belongs. */
        dbg_breadcrumb("usbIrq", (unsigned long)(usb_intr != NULL), 0);
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
     * This hook exists for exactly that, and leaving it empty was the
     * difference between our bring-up and the one that works on this chip.
     * TinyUSB's dwc2_core_init() -- what IDF's usb/device examples run on
     * esp32s2 and esp32s3 -- writes 0xFFFFFFFF to gintsts and gotgint before
     * enabling any mask. Status bits that were already set when GINTMSK was
     * programmed never produce a fresh assertion, and that is precisely what we
     * measured: GINTSTS carrying usbrst and enumdone, GINTMSK masking both,
     * GAHBCFG.GINT set, the CPU slot enabled, and rsr.interrupt never showing
     * that slot -- sampled at the instant the masked status was non-zero.
     */
    DWC_GINTSTS = 0xFFFFFFFF;
    DWC_GOTGINT = 0xFFFFFFFF;
}

/*
 * VBUS is not a GPIO and not a SoC input on this board: it is reported by the
 * AXP173 over I2C. That read takes ~1.6 ms on this command-list peripheral, far
 * too long for the tick, so it is sampled on demand rather than continuously.
 *
 * There used to be a monitor thread here, and it cost DEFAULT_STACK_SIZE of
 * internal DRAM (4 KB) in a region that has ~1 KB free. It bought nothing while
 * the stack is inert: CONFIG_USBOTG == USBOTG_DESIGNWARE sets
 * USB_STATUS_BY_EVENT, so Rockbox never polls usb_detect(), and a thread that
 * only records a value nobody reads is still a thread. When the core is armed,
 * presence should come from the OTG interrupt (the core reports its own
 * connection state) rather than from a poll of the PMU; the PMU read stays here
 * for the charging display.
 */
/* VBUS presence, error-tolerant and debounced.
 *
 * axp173_input_status() returns POWER_INPUT_NONE when the read itself fails,
 * which is indistinguishable from an unplugged cable -- and the poll used to
 * act on it. The first I2C failure after an attach therefore released the pads
 * about two seconds later, on every run, which is why the polled dispatch only
 * ever recorded two or three services and the host never got past its first
 * descriptor read. The PMU and the touchscreen share this bus, and the attach
 * makes the bus busier.
 *
 * A failed read now keeps the last known state, and a removal needs four
 * consecutive absent readings before it is reported -- the same shape as
 * Rockbox's own 200 ms USB debounce, scaled to this poll's period.
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

/* When the current attach began, in ticks. Kept because the trail's usbEnT
 * entry records the gap between attach and teardown, which is the only way
 * to tell a host that dropped the device from a teardown the target
 * initiated. */
static uint32_t xt_attach_tick;

#ifdef USB_ALLOW_ATTACH

/* VBUS presence has to come from the PMU. The OTG core cannot report it before
 * it is armed, and the core is only armed in USB mode, which is what presence
 * is supposed to trigger -- so polling the AXP173 is the only way to break the
 * circular dependency. CONFIG_USBOTG == USBOTG_DESIGNWARE sets
 * USB_STATUS_BY_EVENT, which means Rockbox does not poll usb_detect() itself:
 * the target has to push changes through usb_status_event().
 *
 * The read costs about 1.6 ms on a command-list I2C controller shared with the
 * codec, the IO expander, the touchscreen, the RTC and the PMU's own status, so
 * it runs at roughly 4 Hz from the tick task thread rather than every tick.
 */
#define USB_VBUS_POLL_TICKS 4

/* Nothing here talks on the CDC endpoint unless asked, and nothing here resets
 * the chip.
 *
 * Removed, because none of it is porting Rockbox to this device:
 *
 * - the 'f' flash handover (usb_serial_target_rx ->
 * xt_usb_enter_download_mode), which soft-disconnected, handed the pads to
 * USB-Serial-JTAG, set FORCE_DOWNLOAD_BOOT and esp_restart()ed. The CDC OUT
 * endpoint re-delivers packets it already consumed, so a byte received minutes
 * ago could be honoured as a fresh request -- seen on the console as rst:0xc
 * (RTC_SW_CPU_RST),boot:0x3 (DOWNLOAD(USB/UART0)) with nothing typed. A device
 * that reboots itself into the bootloader during audio playback makes every
 * measurement of audio playback worthless. Flashing is a host-side act on the
 * ROM, not something the firmware should be asked to do to itself. - the 't'
 * trail dump over CDC, and the periodic register and usb-audio lines. They were
 * a load on a transport whose host may not be reading, and the crash trail is
 * still available: dbg_breadcrumb() keeps the ring and the boot console prints
 * it.
 *
 * What is left is the transport, the audio endpoints and the cable sense. */

#ifdef USB_ENABLE_AUDIO
/* Strong override of the weak default in usbstack/usb_audio.c.
 *
 * The isochronous OUT buffer is written by the core's DMA. On this SoC that DMA
 * is an internal AHB master and cannot reach the Ext_RAM window -- ESP-IDF says
 * so outright, gating USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM on
 * IDF_TARGET_ESP32P4 -- and core_alloc()'s pool *is* the PSRAM audiobuffer
 * here. So the host's PCM would be transferred into memory the master cannot
 * write, which shows up as a device that enumerates, streams, and says nothing.
 *
 * Internal SRAM has room for a dedicated static: the firmware's .data/.bss
 * region ends where the IDF heap begins, and dram0_0_seg currently reports
 * 2,464 bytes unused, so the 768 bytes this needs (two 384-byte halves) costs
 * nothing. Raising USB_AUDIO_BUFFER_SIZE moves this size with it, since the
 * span is 2 * arm.
 *
 * It has to be dedicated rather than borrowed. The mass-storage class uses its
 * transfer buffer for the head of every SCSI read and write, and usb.c enables
 * the audio class alongside storage whenever usb_audio is 1 or 3 -- so sharing
 * one region meant the isochronous DMA overwrote the first 640 bytes of every 4
 * KiB sector chunk while the DAC ran, corrupting files written to the card, the
 * data read back from it, and the PCM in the other direction.
 */
#ifdef USB_AUDIO_RX_ARM_SIZE
#define XT_AUDIO_RX_ARM USB_AUDIO_RX_ARM_SIZE
#else
#define XT_AUDIO_RX_ARM USB_AUDIO_BUFFER_SIZE
#endif
#ifdef USB_AUDIO_RX_PINGPONG
#define XT_AUDIO_RX_SPAN (2 * XT_AUDIO_RX_ARM)
#else
#define XT_AUDIO_RX_SPAN XT_AUDIO_RX_ARM
#endif

/* The buffer the isochronous OUT endpoint DMAs into. The class asks for one
 * span, or two when it ping-pongs them, and splits that region itself; this
 * covers both halves. USB_DEVBSS_ATTR puts it in the DMA-reachable static
 * region, cache-line aligned so the discard below rounds to whole lines.
 */
static unsigned char xt_audio_rx_buf[XT_AUDIO_RX_SPAN]
    USB_DEVBSS_ATTR __attribute__((aligned(32)));

/* Remembered so the cache-maintenance hook knows which buffer is the audio
/* one. */
static void *xt_audio_rx_addr;

unsigned char *usb_audio_target_rx_buffer(size_t size)
{
    if (size > sizeof(xt_audio_rx_buf))
        return NULL;

    xt_audio_rx_addr = xt_audio_rx_buf;
    return xt_audio_rx_buf;
}

#endif /* USB_ENABLE_AUDIO */

/* Strong implementation of the driver's weak DMA hook. The esp32s3 D-cache
 * covers internal SRAM -- where every DMA buffer has to live -- and the DWC
 * core writes those bytes without going through it, so a receiver that reads
 * the cached address sees whatever was there before the transfer.
 * cache_hal_invalidate_addr() takes no lock and is safe from the interrupt
 * context this is called in.
 *
 * No counters here. This hook is a convenient place to sample DOEPTSIZ and
 * DOEPCTL at receive completion, and did briefly carry four such counters; they
 * were removed because the display is the only console on this board and the
 * RTC breadcrumb ring cannot be read in the audio profile -- the cable gives
 * OTG to our firmware about a second after boot, so the Serial-JTAG console
 * that would carry the dump is gone before anything can listen.
 */
void usb_dw_target_dma_discard(void *buf, unsigned int len)
{
    discard_dcache_range(buf, len);
}

/* No usb_dw_target_frame_number() override: the driver's generic full-speed
 * reading, (DWC_DSTS >> 8) & 0x3fff, is the correct one for this core, and the
 * driver's own comment says why -- "SOFFN is a 14-bit microframe number for
 * high-speed hosts and a plain frame number for full-speed hosts."
 *
 * This file overrode it with (dsts >> 11) & 0x7ff, i.e. the high-speed shift,
 * on the assumption that SOFFN carries a microframe offset in its low three
 * bits at full speed too. It does not. Measured through the CDC log path, the
 * value this override returns advanced only ~125 times per second on a bus
 * whose frames advance a thousand times per second -- exactly 1000/8, which is
 * what shifting a plain frame number by three further bits does.
 *
 * That is not merely a wrong number on the debug page. usb_dw_epstart() picks
 * the expected data PID for an isochronous receive endpoint from
 * usb_drv_get_frame_number(), and a full-speed stream alternates DATA0/DATA1
 * every frame. A counter whose parity flips every 8 ms therefore matches the
 * host's PID for roughly one frame in eight -- and a rejected isochronous
 * packet raises no completion, no error and no counter, so half the audio
 * disappears while every gauge still reads healthy. That is the ~530 accepted
 * 196-byte packets per second against ~1000 on the wire, which no change to
 * interrupt level, armed packet count or feedback control altered.
 *
 * The two overrides before this one were wrong in the same shape: (dsts >> 4)
 * returned the frame multiplied by 16, and the measurement that appeared to
 * "prove" full speed always uses DATA0 was taken while the parity test was
 * reading a frame number whose low bits were not the parity at all. Falling
 * back to the generic reading also puts this target back on the same rule the
 * high-speed DWC2 targets already rely on.
 */

#ifdef USB_ENABLE_AUDIO
/* Hand over the next slot that actually carries audio.
 *
 * The generic path hands back one slot per call, whatever dsp_buf_size[] says
 * it holds -- including zero. That is not a no-op on the other side:
 * pcm_mixer.c tests
 *
 *     if (!(chan->start && chan->size))
 *         channel_stopped(chan);
 *
 * -- the comment there reads "Channel is stopping" -- so an empty slot is read
 * as the end of the stream. The channel is torn down, playback mutes, and it
 * does not come back until MINIMUM_BUFFERS_QUEUED buffers have re-accumulated
 * -- 16 ms of silence, repeatedly, while the ring around it is full of
 * perfectly good data.
 *
 * Empty slots are not a curiosity. Rockbox documents the behaviour at the head
 * of dsp_process(): "samples consumed and samples produced do not necessarily
 * have a direct correlation. Samples may be consumed without producing any
 * output and samples may be produced without consuming any input." The class
 * sets
 *
 *     dsp_buf_size[rx_usb_idx] = dst.remcount * 2 * sizeof(*dsp_buf);
 *
 * straight from that output count, so any packet an internal stage absorbs
 * lands in the ring with size 0. That also explains two things measured on the
 * board: gaps at a rate that barely changed between 44.1 kHz and 48 kHz, and
 * two very different feedback setpoints producing byte-identical queue traces.
 * Neither was a rate error; the channel was being stopped by a zero, not by an
 * empty ring.
 *
 * What this hook must NOT do is coalesce several slots into one block by
 * treating the ring as contiguous: slots are REAL_DSP_BUF_SIZE (1280) apart
 * while a packet writes about 192 bytes, so the mixer would read the gap
 * between them as audio. That turned pulsed music into plain noise, and made
 * the noise keep playing after the host stopped. Measured, not reasoned: the
 * character changed at exactly that commit and reverted at this one. */
int usb_audio_target_get_more(int play_idx, int filled, int nr_buffers,
                              const int *slot_sizes, const int16_t *buf, size_t slot_stride,
                              const void **start, size_t *size)
{
    /* The common case: this slot has data, let the generic hand-off deal
    /* with it. */
    if (slot_sizes[play_idx] > 0)
        return 0;

    for (int n = 1; n < filled; n++)
    {
        int idx = play_idx + n;

        if (idx >= nr_buffers)
            idx -= nr_buffers;

        if (slot_sizes[idx] > 0)
        {
            /* Same address arithmetic the class uses for the generic path. */
            *start = buf + (idx * (slot_stride / sizeof(*buf)));
            *size = slot_sizes[idx];
            return n + 1; /* class advances rx_play_idx past the empty slots */
        }
    }

    /* Every queued slot is empty, or none are: report it the way the generic
     * path does so a genuine underflow still sets its flag and prebuffers
     * before resuming. */
    return 0;
}

/* usb_audio_target_fb_due() and usb_audio_target_fb_latch() both stay generic.
 *
 * The latch used to be overridden here -- `frame == (startframe + 64) %
 * (USB_FRAME_MAX + 1)` is the only place send_fb becomes true, and it is tested
 * in the else leg of the cadence check, so the override argued that arming
 * feedback depended on catching one exact frame once, ever, and a missed window
 * would leave the host pacing open loop forever.
 *
 * That argument rested on a measurement taken while the frame number was wrong:
 * the readings quoted at the time -- "Samples used per frame" 48.000000 on one
 * build and 0.000000 on the next -- were made before
 * usb_dw_target_frame_number() was deleted, when DWC_DSTS was being shifted by
 * 11 and the frame advanced ~125 times a second instead of 1000. With an honest
 * frame the equality is a normal period test, and it is the same one every
 * high-speed target depends on. The override is gone on that basis; the check
 * is that Samples used per Frame settles near 48.87 rather than reading
 * 0.000000, which is what an unarmed latch looks like.
 */

/* Feedback control is deliberately NOT overridden here, and that is a decision
 * rather than an omission.
 *
 * Both overrides below started from a claim that proved false: that the generic
 * accumulator "is only fed by completions from endpoints other than the
 * isochronous OUT one", so it stayed 0 and feedback sat at nominal. It is not.
 * usb_audio.c adds (usb_audio_get_prebuffering() - MINIMUM_BUFFERS_QUEUED) to
 * that accumulator on every frame between feedback updates, and
 * usb_audio_get_prebuffering() is the true ring occupancy, (rx_usb_idx -
 * rx_play_idx + NR_BUFFERS) % NR_BUFFERS. It is signed, so it moves feedback
 * both ways. The flat reading that motivated the override was the fb_latch race
 * just above, which is fixed on its own terms; once send_fb latched there was
 * nothing wrong with the generic term.
 *
 * What the override then did was actively harmful, and the debug page hid it
 * perfectly. The seam's return value is signed in the arithmetic (samples_fb =
 * samples_base - buffers_filled/4) but a negative return is reserved for "the
 * target measured nothing", so an under-full queue cannot be reported through
 * it: nominal is the ceiling. Clamped to stay non-negative, the hook said
 * "queue at target" -- buffers_filled = 0, samples_fb = samples_base -- for
 * every occupancy from 0 buffers up to the 36-slot setpoint. That is the entire
 * range this stream ever occupies. Which is why "Samples used per frame" read a
 * serene 48.000000 while Min sat at 1 and UNDERFLOW blinked continuously: a
 * one-way valve wired to the half of the queue that never happens, reporting
 * health because it could not report emptiness.
 *
 * The generic path needs no separate setpoint -- its reference is
 * MINIMUM_BUFFERS_QUEUED, the same quantity the restart logic uses, so
 * controller and gate cannot disagree. The band stays at the weak default of
 * +/- 1 sample per frame: on a full-speed bus that is +/- 2.08%, which is
 * not a tight tolerance but the whole authority this endpoint has, since
 * UAC1 feedback at full speed counts whole samples per 1 ms frame and the
 * host may clamp to that band regardless. Widening it, as this file used to,
 * only commands rates the host is entitled to ignore. */
#endif /* USB_ENABLE_AUDIO */

static void usb_vbus_poll(void)
{
    static unsigned ticks;
    static unsigned report;
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
    /* 10 s, in ticks: HZ is 100 (kernel/include/tick.h), so this block runs
     * ten times a second at most. It used to read "4 * USB_VBUS_POLL_TICKS /
     * 4", which is just USB_VBUS_POLL_TICKS -- 40 ms -- and turned the
     * report into a 23 line/s flood on the CDC endpoint. */
    if (xt_armed && ++report >= 10 * HZ)
    {
        report = 0;
        dbg_breadcrumb("usbIRQ", xt_usb_irqs, DWC_GINTSTS);
        dbg_breadcrumb("usbDsts", DWC_DSTS, DWC_DAINT);
    }

    if (++ticks < USB_VBUS_POLL_TICKS)
        return;
    ticks = 0;

    /* Cable state, posted whenever it changes. usb.c turns an insertion event
     * into USB mode, and that is core behaviour this target does not override:
     * what the usb_mode setting changes -- which classes get composed, whether
     * the volumes get handed to the host -- is usb.c's business, not the
     * target's. The consequence on this hardware is that a player plugged into
     * a host is in USB mode, because charging here is not optional.
     */
    /* The state where the cable is known but the transport never came up is the
     * one state CDC cannot report, because CDC is what is missing: the OTG pads
     * stay on USB-Serial-JTAG, so this console is the only channel that works.
     * Deliberately reads nothing from the USB wrap block -- its clock gate is
     * down in this state and a read there stalls the bus, which is the detach
     * bug all over again. RTCCNTL and the peripheral clock register are always
     * clocked.
     *
     * Gated on the anomaly (cable present while unarmed) or on I2C failures, so
     * a player sitting on a desk with no cable stays quiet.
     */
    /* Two cadences: the anomaly every 5 s, and a slow heartbeat while
     * unarmed at all. The heartbeat is what makes silence meaningful --
     * without it, a stuck board that prints nothing could mean "the PMU says
     * no cable" or "this console is dead", and those are different faults
     * with different fixes. */
    /* Counted in poll calls, not kernel ticks: this block runs once every
     * USB_VBUS_POLL_TICKS (4 ticks = 40 ms), so 25 calls make a second.
     * Getting this wrong turns a 5 s report into a 20 s one. */
    static unsigned xt_heartbeat;
    static unsigned xt_idle;
    const unsigned idle_int = 5 * HZ / USB_VBUS_POLL_TICKS;
    const unsigned beat_int = 30 * HZ / USB_VBUS_POLL_TICKS;

    if (!xt_armed && (++xt_heartbeat >= beat_int ||
                      xt_vbus_present == 1 || xt_vbus_err != 0))
    {
        if (xt_heartbeat >= beat_int)
            xt_heartbeat = 0;

        if (xt_vbus_present == 1 || xt_vbus_err != 0)
            xt_idle++;
        else
            xt_idle = idle_int; /* unarmed and no cable: heartbeat only */

        if (xt_idle >= idle_int)
        {
            xt_idle = 0;
            printf("usb idle: cable=%d miss=%d i2c_err=%lu rtc_usb=%08lx clk0=%08lx tick=%lu\n",
                   xt_vbus_present, xt_vbus_miss, xt_vbus_err,
                   (unsigned long)rd(XT_RTC_USB_CONF), (unsigned long)rd(XT_PERIP_CLK_EN0),
                   current_tick);
        }
    }

    status = vbus_sample();

    if (status != last)
    {
        /* The unplug goes through here and the crash happens just after it,
         * so the transition belongs in the trail: whether the cable event
         * was seen at all, and when, is half of the question. */
        last = status;
        dbg_breadcrumb_retained("usbEv", (unsigned long)status, current_tick);
        usb_status_event(status);
    }
}

/*
 * Whether 5V is present -- the contract in usb.h:80, answered the way every
 * other target answers it. This board's only cable sensor is the PMU: the SoC
 * has no VBUS input and GPIO19/20 are D+/D- only, so this is an AXP173 read.
 */
int usb_detect(void)
{
    /* The cable, answered as soon as the boot hook has put the pads back. This
     * used to be gated behind a 120-second window so that a flash attempt could
     * not race the attach; the window is gone because the host can now ask for
     * the handover over USB instead, which is both faster and something the
     * player does on its own.
     */
    return vbus_sample();
}

#else /* !USB_ALLOW_ATTACH */

/*
 * With USB_ALLOW_ATTACH undefined the transceiver is never armed, so the target
 * reports no cable. This is deliberately a bring-up state rather than a
 * behaviour: reporting an insertion while nothing can enumerate would put the
 * player into USB mode with no transport behind it -- the USB screen with
 * nowhere to go, and with mass storage selected, an unmounted card serving
 * nobody. Drop this branch once the core enumerates and the target becomes
 * ordinary.
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
     * The mux override lives in the RTC block (RTCCNTL.usb_conf), which is
     * *not* cleared by a chip reset -- only by a power-on reset. So once a boot
     * hands the pads to the wrap, a panic, an abort or esp_restart leaves them
     * there: the next boot has no USB-Serial-JTAG console and esptool cannot
     * open a port, and the only way back is to take the board off power. That
     * is what happened on this board's first attach attempt -- a report panic
     * reset into a boot that could not be reached at all.
     *
     * Clearing the override at boot makes the console unconditional: every boot
     * starts with the pins on USJ, so there is always a window to flash from,
     * whatever the previous boot did.
     */
    wr(XT_USB_OTG_CONF, rd(XT_USB_OTG_CONF) & ~(XT_USB_PAD_ENABLE |
                                               XT_USB_WRAP_CLK_EN));
    wr(XT_RTC_USB_CONF, rd(XT_RTC_USB_CONF) & ~XT_SW_USB_PHY_SEL);
#endif

#ifdef USB_ALLOW_ATTACH
    tick_add_task(usb_vbus_poll);
#endif
    /* Everything else is armed by usb_enable(). */
}

/*
 * Named __wrap_usb_enable, not usb_enable: ESP-IDF's esp32s3.rom.ld assigns the
 * absolute symbol usb_enable to a mask ROM routine at 0x4000297c, and a linker
 * script's absolute assignment wins over a definition in an object file without
 * a word of warning. Rockbox's usb_init() therefore called the ROM's function,
 * which is part of the download/console path and not the USB transport API --
 * the crash was a LoadProhibited at EXCVADDR 0x0c on the first boot with
 * HAVE_USBSTACK. idf-app/CMakeLists.txt adds -Wl,--wrap=usb_enable, which is
 * what makes this the function that actually runs. Do not rename it back.
 *
 * usb.h maps usb_hw_open()/usb_hw_close() onto this, and usb.c calls them when
 * it enters and leaves USB mode. Note that usb.c calls usb_enable(true) on
 * insertion whatever the usb_mode setting says -- it does so at line 483,
 * before it computes power-only state at line 492 -- so on a software stack the
 * mode chooses the class composition, not whether the transceiver is opened. On
 * this board that means the pins move on insertion, which is core behaviour;
 * the recovery invariant is the pad clear in usb_init_device(), not a refusal
 * here.
 */
/* Tried and refuted: the ROM's usb_enable() is not a bring-up helper you can
 * borrow. --wrap=usb_enable means __real_usb_enable resolves to the ROM
 * symbol at 0x4000297c, and calling it panicked immediately with
 * LoadProhibited at PC 0x400533d1 in a boot loop. The ROM's usb_enable
 * belongs to a complete device stack in ROM (usb_transfer, usb_set_config,
 * usb_dc_ep_set_callback, g_usb_print in .data.interface), not to a
 * separable PHY-and-clock setup, so it has to be entered the way the mask
 * ROM enters it. The lower-level chip_usb_dw_init (0x400026f4) is the piece
 * worth trying later; the bring-up here stays hand-rolled meanwhile. */

void __wrap_usb_enable(bool on)
{
#ifdef USB_ALLOW_ATTACH
    dbg_breadcrumb("usbEn", on ? 1 : 0, current_tick);

    /* Sample the register state BEFORE the teardown. usb_core_exit() ends with
     * usb_dw_target_disable_clocks(), which clears the USB-wrap gate, and
     * reading XT_USB_OTG_CONF afterwards stalls the bus. That read is what has
     * been hanging the detach ever since this breadcrumb existed: the surviving
     * trail stops at dRet and uOut/uClk/wRet never appear because the read that
     * would record them never returns. The same trap is why the pad release now
     * runs before the gate is dropped.
     */
    uint32_t pre_gsnpsid;
    uint32_t pre_otgconf;

    if (!on)
    {
        /* Tearing down: usb_core_exit() ends with
         * usb_dw_target_disable_clocks(), which clears the USB-wrap gate, so
         * the sample has to be taken while it is still clocked. Reading the
         * block afterwards stalls the bus -- that read hung every detach since
         * this breadcrumb existed, which is why the trail stopped at dRet.
         */
        pre_gsnpsid = DWC_GSNPSID;
        pre_otgconf = rd(XT_USB_OTG_CONF);
    }

    if (!on && xt_attach_tick != 0)
    {
        /* When a teardown happens relative to the attach, in ticks. The
         * kernel shows the pads coming back a couple of seconds after an
         * attach on some runs and the full 90 seconds on others, and the
         * trail cannot say which without a clock: 200 ticks is Rockbox
         * releasing the transport on its own, 9000 is the bring-up judge. */
        dbg_breadcrumb("usbEnT", current_tick - xt_attach_tick, xt_attach_tick);
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

    if (on)
    {
        /* Coming up: on a re-attach the wrap gate is still off, because the
         * previous detach dropped it, so sampling before usb_core_init()
         * hangs the attach instead -- the mirror of the bug above. It is
         * clocked only after the driver has brought the core up. */
        pre_gsnpsid = DWC_GSNPSID;
        pre_otgconf = rd(XT_USB_OTG_CONF);
    }

    /* Both halves of the register state, recorded before anything can change
     * it. The trail survives the reset, and app_main() replays it inside the
     * boot window, so one power cycle tells us what the previous boot did --
     * including whether this function was called with false, which is the
     * other explanation for a report that reads all zeroes. */
    dbg_breadcrumb(on ? "usbIn" : "usbOut", pre_gsnpsid, pre_otgconf);
    dbg_breadcrumb("usbClk2", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_RST_EN0));
    /* The same two steps in the ring that survives a reset. They are the
     * only markers between usb_core_exit()'s dRet and whatever runs next,
     * and while they lived only in the .noinit ring a watchdog reset erased
     * them -- which is why every detach trace so far appeared to stop
     * exactly at dRet with no explanation. These are register reads on SoC
     * blocks plus GSNPSID, taken while the OTG block is still clocked. */
    dbg_breadcrumb_retained(on ? "uIn" : "uOut", pre_gsnpsid, pre_otgconf);
    dbg_breadcrumb_retained("uClk", rd(XT_PERIP_CLK_EN0), rd(XT_PERIP_RST_EN0));

    /* The register state after the attach, recorded rather than panicked. The
     * earlier version of this slept 8 s and called panicf() so the values would
     * reach the LCD -- but PRINT_REBOOT turned that into the 10-second reboot
     * loop the kernel log shows, and the trail carries the same numbers to a
     * live console on the next boot anyway. Nothing here should stop the
     * device.
     *
     * Reading the trail: DCTL bit 0 is soft-disconnect (clear means the core is
     * connected); GINTSTS bit 12 usbrst, bit 13 enumdone and bit 23 resetdet
     * are the only evidence a host ever touched the bus; GUSBCFG bit 5 fsintf
     * and bit 30 fdmod say the PHY and device-mode writes took; OTG_CONF bit 18
     * pad_enable says the mux moved the pins, and bit 0 the session-end
     * override.
     */
    if (on)
    {
        dbg_breadcrumb("usbPost", DWC_DCTL, DWC_GINTSTS);
    }

    /* Bring-up only: collect ~90 s of interrupt and bus state, then restart the
     * chip exactly once per power-on.
     *
     * Why a restart rather than a report on screen: the breadcrumb trail lives
     * in .noinit, which survives a chip reset and is destroyed by a power-on
     * reset. With the pins handed to the OTG core there is no console to read,
     * so the only way to get this data out is for the firmware to reset itself
     * -- the boot hook puts the pins back on USB-Serial-JTAG, and app_main()
     * replays the trail on a live console. The once-per-power-on flag is what
     * kept the earlier version from looping: a panic every 10 seconds is how
     * the board ended up popping the amp over and over.
     */
    if (on)
        xt_attach_tick = current_tick;
#else
    (void)on;
#endif

    /* Last marker in the target's USB enable wrapper. If this appears in the
     * surviving ring and nothing after it does, the boot died in code that
     * runs once the transport is down: charging bookkeeping, the threads the
     * detach broadcast woke, or whatever the player does on its way back to
     * its loop. */
    dbg_breadcrumb_retained("wRet", on ? 1 : 0, current_tick);
}

#endif /* HAVE_USBSTACK */
