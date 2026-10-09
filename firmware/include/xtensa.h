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
#define XT_SRC_SDIO_HOST        30   /* base + 0x078 */

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
