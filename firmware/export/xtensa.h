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
/* The wrap's own PHY pad loads, from IDF 4.4's usb_phy_ll.h for this chip
 * (usb_phy_ll_int_load_conf(), which is what usb_enable(USB_MODE_DEVICE) called).
 * A full-speed device needs D+ pulled up *and* D- pulled down, with
 * pad_pull_override set so these values actually drive the pads. */
#define XT_USB_PAD_PULL_OVERRIDE  (1u << 12)
#define XT_USB_DP_PULLUP          (1u << 13)
#define XT_USB_DP_PULLDOWN        (1u << 14)
#define XT_USB_DM_PULLUP          (1u << 15)
#define XT_USB_DM_PULLDOWN        (1u << 16)
#define XT_USB_PULLUP_VALUE       (1u << 17)
#define XT_USB_PAD_ENABLE       (1u << 18)
#define XT_USB_SESSEND_OVERRIDE   (1u << 0)      /* 1 = software drives session end */
#define XT_USB_SESSEND_VALUE      (1u << 1)      /* 1 = VBus < 0.2V, 0 = VBus > 0.8V */
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
 * A dedicated vector section used to live here (VEC_ATTR, ".icode.irq"), because the
 * bare-metal branch wrote its own table and the `j` in it has only an 18-bit signed
 * range, forcing the handler to sit next to the table. ESP-IDF owns the vectors on this
 * path and Rockbox installs handlers through esp_intr_alloc(), so there is nothing for
 * such a section to place and the macro is gone rather than unused.
 */

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

/*
 * Memory layout. Like every other CPU header here (x1000.h, as3525.h,
 * imx233.h), this is the single place the region numbers live: cpu.h includes
 * it after config.h, so apps/plugins/plugin.lds and the firmware see the same
 * constants rather than restating them.
 *
 * The buffers themselves are defined by target/xtensa/memory-xtensa-syms.S,
 * because on this path the platform linker script owns placement rather than a
 * Rockbox .lds. rockbox_app.c in the IDF project creates the FreeRTOS task on
 * the buffer named stackbegin here and states its size separately, since that
 * file cannot include Rockbox headers.
 */

/* Half of PSRAM, minus a reservation. On every other Rockbox target the
 * audiobuffer is whatever is left of one contiguous memory region after the
 * firmware; here IDF owns that region's other half as its general heap, so the
 * split has to be stated. Making this larger takes memory straight out of IDF's
 * PSRAM heap.
 *
 * The deduction is not slack for its own sake -- it is the clearance the
 * section needs above itself. XT_AUDIOBUF_SIZE is only what memory-xtensa-syms.S
 * puts in .ext_ram.bss; IDF's own PSRAM objects and every object the ld fragment
 * maps to extern_ram (unicode, language, the skin parser, dir/file/fileobj_mgr,
 * system.o's boost log and, since the feature flags, gui/line.o's layout state) are
 * placed in the same section, so the section ends well past audiobufend. Above it
 * sit the codec and plugin text reservations, and .ext_ram.bss is memset to zero at
 * boot by esp_psram_bss_init() -- so a section that reaches into plugin_text
 * silently zeroes the head of the region every plugin is loaded into. That is
 * exactly what happened: moving system.o's 16,664 bytes of .bss to PSRAM put the
 * section end at 0x3c58290c, 10,508 bytes past XT_PLUGIN_TEXT_ADDR, and every
 * plugin crashed on arrival while codecs (at XT_CODEC_TEXT_ADDR, further up) kept
 * working. The guard in system_init() now watches that boundary; keep this
 * deduction large enough that it never fires.
 *
 * It has to be a whole page, not merely "a bit more", and that is the part worth
 * remembering. .ext_ram.bss does not start at a fixed address: IDF's .ext_ram.dummy
 * advances the location counter past the flash rodata and then aligns to the 64 KB
 * MMU page, so the section's start moves in 64 KB steps whenever code or rodata
 * growth crosses a boundary. Enabling HAVE_ALBUMART, HAVE_JPEG, HAVE_QUICKSCREEN
 * and AB_REPEAT_ENABLE grew the image by ~24 KB, which pushed _ext_ram_bss_start
 * from 0x3c120000 to 0x3c130000 -- a full page for a 24 KB change -- and a 64 KB
 * deduction was therefore consumed entirely, putting the end at 0x3c582e8c and
 * reproducing the original crash figure almost exactly. 128 KB covers one page of
 * growth plus the donated objects. If the image grows another page this moves
 * again, so read the MEMORY: line printed at boot rather than trusting this
 * number. The deduction is now 128 KB below half of PSRAM plus the 256 KB the
 * codec text reservation gave up (see XT_CODEC_TEXT_SIZE): the arena is sized by
 * this constant alone -- core_alloc.c does buflib_init(start, audiobufend - start)
 * and audiobufend is the label right after the .space below -- so shrinking the
 * text reservations without raising this buys nothing but unused slack. Growing it
 * takes the space straight out of IDF's PSRAM heap, and the boot guard keeps watch
 * on the boundary either way. */
#define XT_AUDIOBUF_SIZE    ((MEMORYSIZE * 0x80000) + 0x20000)

/*
 * Codec and plugin load buffers, pinned to the top of the PSRAM part of the
 * external data-cache window.
 *
 * These are not free choices. apps/codecs.c and apps/plugin.c both reject a
 * loaded image whose declared load address differs from &codecbuf /
 * &pluginbuf, and the codec and plugin images are linked against the fixed
 * CODEC_ORIGIN / PLUGIN_ORIGIN that apps/plugins/plugin.lds computes from
 * MEMORYSIZE, CODEC_SIZE and PLUGIN_BUFFER_SIZE. The two sides have to agree,
 * and on every other native target they do because the same linker script hands
 * out both -- firmware/rom.lds and the arm imx233 / s5l8700 / as3525 / s3c2440
 * / tcc780x scripts all assign codecbuf from the region layout. This target has
 * no Rockbox linker script, so the constants are restated here, identically:
 *
 *   plugin.lds:  DRAMORIG 0x3C000020
 *                DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
 *                CODEC_ORIGIN   (DRAMORIG + DRAMSIZE)
 *                PLUGIN_ORIGIN  (CODEC_ORIGIN + CODEC_SIZE)
 *
 * Which puts codecbuf and pluginbuf in the top 1.5 MB of the window and leaves
 * the bottom 6.5 MB for the audiobuffer and the rest of .ext_ram.bss.
 * memory-xtensa-syms.S exports rb_ext_ram_end so boot can check that the two
 * regions do not overlap, rather than letting a collision corrupt silently.
 */
#define XT_DROMORIG         0x3C000020
/* The window is MEMORYSIZE MB from 0x3C000000 and the flash MMU has exactly that
 * many 64 KB entries pointing at PSRAM, so the arena must END at
 * 0x3C000000 + MEMORYSIZE MB -- there is no entry beyond it to map, because all
 * MEMORYSIZE*16 physical pages are already claimed. XT_DROMORIG carries +0x20 so
 * flash-mapped segments satisfy the cache's page-congruence rule, and codecbuf /
 * pluginbuf are derived from it, so without subtracting that offset here the top
 * of the arena lands 32 bytes past the last mapped entry -- the defect the boot
 * check in system-xtensa.c reports. Subtracting it also page-aligns both buffers,
 * which they wanted anyway. */
#define XT_CODECBUF_ADDR    (XT_DROMORIG + (MEMORYSIZE * 0x100000) - 0x20 \
                             - PLUGIN_BUFFER_SIZE - CODEC_SIZE)
#define XT_PLUGINBUF_ADDR   (XT_CODECBUF_ADDR + CODEC_SIZE)
#define XT_LOADADDRESS_ADDR (XT_PLUGINBUF_ADDR + PLUGIN_BUFFER_SIZE)

/* The two external-RAM cache windows alias the same physical pages -- they
 * index the same MMU entries -- and differ only in what the hardware serves:
 * the instruction window takes fetches and loads but faults on a store, the
 * data window takes loads and stores but faults on a fetch. So a loaded image
 * is linked read-only-half-up and writable-half-down, and the offset between
 * the two views of one page is constant. */
#define XT_IROM_BASE        0x40000000
#define XT_IROM_OFF         0x06000000

/* Text halves carved out of the gap below codecbuf. Measured across all codecs
 * the largest .text+.literal+.rodata is 182 KB (aac.codec is 200,232 bytes as a
 * whole image), so the codec half is 256 KB: still ~74 KB of headroom, and
 * apps/plugins/plugin.lds links against THIS_TEXT_LENGTH, so a codec that outgrows
 * it fails at link time rather than at load. The plugin half stays at 512 KB
 * because the largest .rock is 482,560 bytes and shrinking it would put the head of
 * the plugin text region at risk for no gain -- what this shrink actually funds is
 * XT_AUDIOBUF_SIZE. These are the numbers plugin.lds includes rather than repeats. */
#define XT_CODEC_TEXT_SIZE   0x40000
#define XT_PLUGIN_TEXT_SIZE  0x80000

/* Data-window addresses of those halves, i.e. the addresses they are *written*
 * through. Execution uses the linked address, which is this plus XT_IROM_OFF. */
#define XT_CODEC_TEXT_ADDR   (XT_CODECBUF_ADDR - XT_CODEC_TEXT_SIZE)
#define XT_PLUGIN_TEXT_ADDR  (XT_CODEC_TEXT_ADDR - XT_PLUGIN_TEXT_SIZE)

/* Rockbox's thread 0 stack, which is the FreeRTOS task stack the whole of
 * Rockbox runs on. The bare-metal script used the same 0x8000. */
#define XT_STACK_SIZE       0x8000

#endif /* XTENSA_H */
