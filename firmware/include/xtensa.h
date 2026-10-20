/***************************************************************************
 * Xtensa LX7 (ESP32-S3) architecture header.
 *
 * Register bases and bit positions here are taken from the chip's SVD
 * (components/soc/esp32s3/register/soc/) and cross-checked against the
 * bare-metal reference, not guessed.
 ****************************************************************************/
#ifndef XTENSA_H
#define XTENSA_H

/* Peripherals. Only the ones Rockbox uses are listed. */
#define XT_SYSTEM_BASE          0x600C0000      /* peripheral clock gating */
#define XT_INTERRUPT_CORE0_BASE 0x600C2000      /* interrupt matrix, core 0 */
#define XT_RTCCNTL_BASE         0x60008000      /* RTC, incl. the RTC watchdog */
#define XT_TIMERGROUP0_BASE     0x6001F000
#define XT_TIMERGROUP1_BASE     0x60020000
#define XT_SDMMC_BASE           0x60028000      /* SDHOST (DesignWare MMC) */
#define XT_LEDC_BASE            0x60019000      /* LEDC PWM (8 low-speed channels) */

/*
 * Interrupt matrix: one 5-bit register per peripheral interrupt source,
 * at base + 4 * source_id. The value is the core interrupt slot the source
 * is routed to.
 *
 * The source ids are the ETS_*_INTR_SOURCE enum values, and they are not the
 * obvious ones: TIMERG1 timer 0 is 53, not 48 or 49 (48/49 are the legacy
 * systimer slots). Cross-checked against INTERRUPT_CORE0_TG1_T0_INT_MAP_REG at
 * base + 0x0D4, which is 4 * 53.
 */
#define XT_SRC_TIMERG0_T0       50
#define XT_SRC_TIMERG0_T1       51
#define XT_SRC_TIMERG1_T0       53
#define XT_SRC_TIMERG1_T1       54
#define XT_SRC_GDMA_OUT_CH0     71   /* base + 0x11C */
#define XT_SRC_GDMA_IN_CH0      66   /* base + 0x108 */
#define XT_SRC_SDIO_HOST        30   /* base + 0x078 */
#define XT_SRC_I2C0             42   /* base + 0x0A8; ETS_I2C_EXT0_INTR_SOURCE */

/*
 * Levels cannot be programmed on this core config: there are no level or
 * priority registers in INTERRUPT_CORE0, and rsr.intlevel / wsr.intlevel are
 * not valid opcodes. The level of each slot is fixed by the core config, so
 * a handler can only be entered for a slot that is already level 1.
 *
 * Level 1 slots on the esp32s3 core config: 0-10, 12, 13, 17, 18.
 * Slot 0 is reserved for the WiFi MAC and slot 4 for the WiFi baseband,
 * so 1 and 2 are the lowest ones free for Rockbox.
 */
#define XT_SLOT_TIMERG1         1
#define XT_SLOT_GDMA_OUT_CH0    2
#define XT_SLOT_SDHOST          3
#define XT_SLOT_GDMA_IN_CH0     5
#define XT_SLOT_I2C0            8
#define XT_SLOT_USB             9

/*
 * USB.
 *
 * The SoC has a Synopsys DWC2 OTG core at 0x60080000. Its configuration is not a
 * guess: ESP-IDF ships it generated for this core in
 * components/soc/esp32s3/include/soc/usb_dwc_cfg.h -- OTG_ARCHITECTURE 2 (the
 * core has its own internal DMA, so a transfer is programmed with an address
 * rather than pushed through the FIFOs by hand), OTG_FSPHY_INTERFACE 1,
 * OTG_NUM_EPS 6, OTG_NUM_IN_EPS 5, OTG_DFIFO_DEPTH 256 words. Register offsets
 * and field positions likewise come from IDF's generated usb_dwc_struct.h, and
 * they agree with firmware/export/usb-designware.h exactly: GUSBCFG at 0x0c
 * rather than the classic 0x28, DIEPDMA and DOEPDMA at +0x14 inside an endpoint's
 * 0x20 stride, and the FS PHY selected by fsintf (bit 5) instead of the classic
 * UTMI/ULPI/PHYSEL encoding.
 *
 * The DWC does not own the pins. A "USB wrap" muxes the internal FSLS PHY between
 * the DWC and the USB serial jtag block, and that mux only answers to software
 * when the RTC's usb_conf says so. Both have to be set before the DWC can talk to
 * a host.
 *
 * There are two clock gates, and enabling only one is the mistake that makes every
 * OTG register read 0x00000000 without faulting: usb_clk_en (bit 23 of
 * PERIP_CLK_EN0) belongs to the wrap and PHY domain, while usb_device_clk_en (bit
 * 10 of PERIP_CLK_EN1) belongs to the DWC core itself. Each has a matching reset
 * bit in PERIP_RST_EN0/EN1, where 0 means not in reset and both default to 0.
 */
#define OTGBASE                 0x60080000     /* for usb-designware.h */
#define XT_USB_DWC_BASE         OTGBASE
#define XT_USB_WRAP_BASE        0x60039000
#define XT_USB_OTG_CONF         (XT_USB_WRAP_BASE + 0x00)
#define XT_USB_PHY_SEL_EXT      (1u << 2)      /* 1 = external PHY */
#define XT_USB_PAD_ENABLE       (1u << 18)
#define XT_USB_WRAP_CLK_EN      (1u << 31)     /* the wrap block's own gate */
#define XT_USB_PHY_CLK_FORCE    (1u << 20)
#define XT_USB_AHB_CLK_FORCE    (1u << 19)
#define XT_USB_DFIFO_FORCE_PU   (1u << 22)
#define XT_USB_DFIFO_FORCE_PD   (1u << 3)

#define XT_RTC_USB_CONF         (XT_RTCCNTL_BASE + 0x120)
#define XT_SW_HW_USB_PHY_SEL    (1u << 20)     /* software overrides the mux */
#define XT_SW_USB_PHY_SEL       (1u << 19)     /* 1 = FSLS PHY to the USB wrap */

#define XT_PERIP_CLK_EN0        (XT_SYSTEM_BASE + 0x18)
#define XT_USB_CLK_EN           (1u << 23)     /* wrap / PHY domain */
#define XT_PERIP_CLK_EN1        (XT_SYSTEM_BASE + 0x1c)
#define XT_USB_DEVICE_CLK_EN    (1u << 10)     /* the DWC core itself */
#define XT_PERIP_RST_EN0        (XT_SYSTEM_BASE + 0x20)
#define XT_USB_RST              (1u << 23)
#define XT_PERIP_RST_EN1        (XT_SYSTEM_BASE + 0x24)
#define XT_USB_DEVICE_RST       (1u << 10)

#define XT_SRC_USB              38             /* ETS_USB_INTR_SOURCE, +0x098 */

/*
 * The L1 data cache works in 32-byte lines. DMA buffers have to be aligned to a
 * line so that committing or invalidating a range never touches a line belonging
 * to something else.
 */
#define CACHEALIGN_BITS         5
#define CACHEALIGN_SIZE         (1 << CACHEALIGN_BITS)

/*
 * The vector table has to be a physical address the core can fetch from, so it goes
 * in internal RAM on the instruction-bus view. The j in the table has an 18-bit
 * signed range, so the handler and the dispatch have to sit right after it - hence a
 * dedicated section placed by app.lds, not the general .icode.
 */
#define VEC_ATTR __attribute__((section(".icode.irq")))

/*
 * Timer group registers, per timer n: block at base + 0x24 * n.
 */
#define XT_TN_CONFIG(n)         (0x00 + 0x24 * (n))
#define XT_TN_LO(n)             (0x04 + 0x24 * (n))
#define XT_TN_HI(n)             (0x08 + 0x24 * (n))
#define XT_TN_UPDATE(n)         (0x0C + 0x24 * (n))
#define XT_TN_ALARM_LO(n)       (0x10 + 0x24 * (n))
#define XT_TN_ALARM_HI(n)       (0x14 + 0x24 * (n))
#define XT_TN_LOAD_LO(n)        (0x18 + 0x24 * (n))
#define XT_TN_LOAD_HI(n)        (0x1C + 0x24 * (n))
#define XT_TN_LOAD(n)           (0x20 + 0x24 * (n))

#define XT_TN_INT_ENA           0x70
#define XT_TN_INT_RAW           0x74
#define XT_TN_INT_ST            0x78
#define XT_TN_INT_CLR           0x7C

/* tnconfig bit positions */
#define XT_TN_USE_XTAL          (1 << 9)
#define XT_TN_ALARM_EN          (1 << 10)     /* self-clearing when the alarm fires */
#define XT_TN_DIVCNT_RST        (1 << 12)
#define XT_TN_DIVIDER_SHIFT     13            /* 16 bits */
#define XT_TN_AUTORELOAD        (1 << 29)
#define XT_TN_INCREASE          (1 << 30)
#define XT_TN_EN                (1 << 31)

/*
 * CPU clock tree.
 *
 * CPU_CLK is muxed from XTAL (40 MHz), the BBPLL (480 or 320 MHz) or RC_FAST
 * (20 MHz). PRE_DIV_CNT divides the XTAL and RC_FAST paths only; on the PLL
 * path the frequency is CPUPERIOD_SEL, which with a 480 MHz BBPLL gives
 *   0 -> 80 MHz, 1 -> 160 MHz, 2 -> 240 MHz.
 *
 * Register bases and bit positions are from
 * components/soc/esp32s3/include/soc/system_reg.h, cross-checked against
 * components/hal/esp32s3/include/hal/clk_tree_ll.h.
 */
#define XT_SYS_CLK_CONF         (XT_SYSTEM_BASE + 0x60)
#define XT_SYS_PRE_DIV_MASK     0x3FF           /* bits [9:0], stores divider - 1 */
#define XT_SYS_CLK_SEL_SHIFT    10              /* bits [11:10] */
#define XT_SYS_CLK_SEL_XTAL     0
#define XT_SYS_CLK_SEL_PLL      1
#define XT_SYS_CLK_SEL_RC_FAST  2

#define XT_SYS_CPU_PER_CONF     (XT_SYSTEM_BASE + 0x10)
#define XT_CPU_PERIOD_MASK      0x3             /* bits [1:0] */
#define XT_CPU_PERIOD_80M       0
#define XT_CPU_PERIOD_160M      1
#define XT_CPU_PERIOD_240M      2
#define XT_PLL_FREQ_SEL         (1u << 2)       /* 1 = 480 MHz, 0 = 320 MHz */

/*
 * RTC_CNTL_DATE_REG bits [18:13] are the LDO slaves powered down; the rest of the
 * register is the RTC date and has to be preserved. ESP-IDF writes
 * DEFAULT_LDO_SLAVE (0x7) >> (frequency / 80), i.e. 0 slaves off at 240 MHz, 1 at
 * 160 MHz and 3 at 80 MHz.
 */
#define XT_RTC_DATE_REG         (XT_RTCCNTL_BASE + 0x1FC)
#define XT_LDO_SLAVE_MASK       0x3F
#define XT_LDO_SLAVE_SHIFT      13

/*
 * The timer group counter is 54 bits on ESP32-S3, not 64: tnhi is 22 bits.
 *
 * The counter can be driven from APB_CLK or from XTAL_CLK. APB_CLK depends on
 * the CPU frequency, so it changes when the CPU is clocked down; XTAL is always
 * the 40 MHz crystal. Rockbox uses XTAL so TIMER_FREQ does not move.
 */
#define XT_XTAL_FREQ            40000000
#define XT_TIMER_PRESCALER      40
#define TIMER_FREQ              (XT_XTAL_FREQ / XT_TIMER_PRESCALER)  /* 1 MHz */

#endif /* XTENSA_H */
