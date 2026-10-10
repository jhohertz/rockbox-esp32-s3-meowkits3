/***************************************************************************
 * ESP32-S3 debug menu.
 *
 * There is no I/O port register block to dump, so dbg_ports() has nothing to
 * show. dbg_hw_info() is the bring-up report card: the display is the only
 * console this board has, so every subsystem that can be checked without
 * hardware-in-the-loop gets a line.
 *
 * The order is the order a failure is most likely to appear in. PSRAM and the
 * MMU come first because nothing else works if they do not; then the internal
 * SRAM budget, because exceeding it is a link-time failure that only shows up
 * as an ASSERT; then the I2C devices, which are the slowest thing on the bus
 * and the easiest thing to get wrong; then storage and USB.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "list.h"
#include "xtensa.h"
#include "rom_cache.h"
#include "psram-xtensa.h"
#include "i2c.h"
#include "i2c-target.h"
#include "pca9557.h"
#include "es8311.h"
#include "es7210.h"
#include "axp173.h"
#include <stdio.h>

static inline uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

/* rsr.prid: [3:0] core revision, [11:8] core variant, [15:12] architecture
 * revision. rsr.revision does not exist on this core config. */
static unsigned int core_prid(void)
{
    unsigned int v;

    asm volatile ("rsr.prid %0" : "=r"(v));
    return v;
}

/* Defined by app.lds. dramend is the top of the internal data region; the
 * ASSERT in the same script guarantees nothing is placed above it. */
extern char _datastart[], _dataend[];
extern char _bssstart[], _bssend[];
extern char _stackend[];
extern char _psrambssstart[], _psrambssend[];
extern char dramend[];

/* Set by irom-map.c: how many flash segments crt0 armed in the MMU before
 * jumping into the IROM window. */
extern int irom_map_status;

static unsigned int mmu_mapped_pages(void)
{
    /* The table has 32-bit entries; bit 14 is the "invalid" flag. */
    volatile const uint32_t *table =
        (volatile const uint32_t *)0x600C5000;
    unsigned int n = 0;

    for (int i = 0; i < 512; i++)
        if (!(table[i] & (1u << 14)))
            n++;

    return n;
}

/*
 * Every device on the bus, in the order the target brings them up. A read of
 * one register is enough to know whether the chip is there: the command-list
 * controller reports a NACK as a failed transaction rather than hanging.
 */
struct i2c_device {
    unsigned char addr;
    unsigned char reg;
    const char *name;
};

static const struct i2c_device i2c_devices[] =
{
    { PCA9557_ADDR, 0x01, "PCA9557 expander" },
    { ES8311_I2C_ADDR, 0x00, "ES8311 DAC" },
    { ES7210_I2C_ADDR, 0x00, "ES7210 ADC" },
    { AXP173_ADDR, 0x12, "AXP173 PMU" },
    { 0x51, 0x02, "PCF8563 RTC" },
    { FT6x06_ADDR, 0x02, "FT6336 touch" },
};

static bool i2c_answers(const struct i2c_device *dev)
{
    unsigned char val;

    return i2c_readmem(dev->addr, dev->reg, &val, 1) == 1;
}

enum
{
    INFO_CPU,
    INFO_CORE,
    INFO_CACHE,
    INFO_IROM,
    INFO_PSRAM,
    INFO_MMU,
    INFO_SRAM,
    INFO_PSRAMBSS,
    INFO_I2C_BASE,          /* one entry per device in i2c_devices */
    INFO_USB = INFO_I2C_BASE + ARRAYLEN(i2c_devices),
    INFO_COUNT
};

static const char *info_get_name(int item, void *data, char *buf, size_t bufsz)
{
    (void)data;

    switch (item) {
    case INFO_CPU:
        snprintf(buf, bufsz, "CPU frequency: %ld Hz", cpu_frequency);
        break;
    case INFO_CORE: {
        unsigned int prid = core_prid();
        snprintf(buf, bufsz, "Xtensa core: variant %u revision %u",
                 (prid >> 8) & 0xf, prid & 0xf);
        break;
    }
    case INFO_CACHE:
        snprintf(buf, bufsz, "D-cache line size: %u bytes",
                 cache_dcache_line_size());
        break;
    case INFO_IROM:
        /* A negative status means crt0 could not read the image header, which
         * means the IROM window is whatever the ROM loader left it as - on this
         * chip that is nothing, so the firmware would not have reached here.
         */
        if (irom_map_status < 0)
            snprintf(buf, bufsz, "IROM MMU mapping: failed (%d)",
                     irom_map_status);
        else
            snprintf(buf, bufsz, "IROM MMU mapping: %d segments armed",
                     irom_map_status);
        break;
    case INFO_PSRAM:
        snprintf(buf, bufsz, "PSRAM: %s",
                 psram_present() ? "responding" : "not responding");
        break;
    case INFO_MMU:
        snprintf(buf, bufsz, "External cache MMU: %u of 512 pages mapped",
                 mmu_mapped_pages());
        break;
    case INFO_SRAM: {
        /* .data starts at DRAMORIG and the stack is the last thing placed, so
         * the span between them is exactly what the region is carrying.
         */
        unsigned long used = (unsigned long)(_stackend - _datastart);
        unsigned long total = (unsigned long)(dramend - _datastart);

        snprintf(buf, bufsz, "Internal SRAM: %lu used, %lu free",
                 used, total - used);
        break;
    }
    case INFO_PSRAMBSS:
        snprintf(buf, bufsz, "Statics moved to PSRAM: %lu bytes reserved",
                 (unsigned long)(_psrambssend - _psrambssstart));
        break;
    case INFO_USB:
        /* The DWC is clock-gated until usb_enable() runs, and a gated
         * peripheral reads as zero, so the gate has to be reported separately
         * from the core. GSNPSID is Synopsys' signature; 0x31304040 is the
         * DWC2 family. */
        if (!(rd(XT_PERIP_CLK_EN0) & XT_USB_CLK_EN)) {
            snprintf(buf, bufsz, "DWC2 @ 0x%08x: clock gated", XT_USB_DWC_BASE);
        } else {
            snprintf(buf, bufsz, "DWC2 @ 0x%08x: core id 0x%08x",
                     XT_USB_DWC_BASE, (unsigned)rd(OTGBASE + 0x40));
        }
        break;
    default: {
        int i = item - INFO_I2C_BASE;

        if (i >= (int)ARRAYLEN(i2c_devices)) {
            buf[0] = '\0';
            break;
        }

        snprintf(buf, bufsz, "%s @ 0x%02x: %s",
                 i2c_devices[i].name, i2c_devices[i].addr,
                 i2c_answers(&i2c_devices[i]) ? "answers" : "no answer");
        break;
    }
    }

    return buf;
}

bool dbg_hw_info(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, MODEL_NAME " hardware info", INFO_COUNT, NULL);
    info.get_name = info_get_name;

    return simplelist_show_list(&info);
}

bool dbg_ports(void)
{
    return false;
}