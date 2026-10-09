/***************************************************************************
 * ESP32-S3 debug menu.
 *
 * There is no I/O port register block to dump, so dbg_ports() has nothing to
 * show. dbg_hw_info() reports what is actually worth looking at during bring-up:
 * the core identity, the clock, the cache line size the ROM reports, and how many
 * of the 512 external-cache MMU entries Rockbox has mapped.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "list.h"
#include "xtensa.h"
#include "psram-xtensa.h"
#include <stdio.h>

/* rsr.prid: [3:0] core revision, [11:8] core variant, [15:12] architecture
 * revision. rsr.revision does not exist on this core config. */
static unsigned int core_prid(void)
{
    unsigned int v;

    asm volatile ("rsr.prid %0" : "=r"(v));
    return v;
}

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

enum
{
    INFO_CPU,
    INFO_CORE,
    INFO_CACHE,
    INFO_PSRAM,
    INFO_MMU,
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
    case INFO_PSRAM:
        snprintf(buf, bufsz, "PSRAM: %s",
                 psram_present() ? "responding" : "not responding");
        break;
    case INFO_MMU:
        snprintf(buf, bufsz, "External cache MMU: %u of 512 pages mapped",
                 mmu_mapped_pages());
        break;
    default:
        buf[0] = '\0';
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
