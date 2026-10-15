/*
 * scan_trace.c -- a breadcrumb ring that outlives the crash it is there to
 * explain.
 *
 * The fault under investigation arrives as a double exception. ESP-IDF's
 * _DoubleExceptionVector is `break 1, 4` followed by _xt_panic, so the cause of
 * the *first* exception is gone by the time anything is printed, and this core
 * exposes no DEPC/DEXCCAUSE to recover it from. Printing from the path is not
 * an option either: the thread switch runs with interrupts masked and IDF's
 * printf takes a lock.
 *
 * So the trace is written, not printed. Four words per step into a ring in
 * .noinit, which IDF's sections.ld declares (NOLOAD) -- it is neither in the
 * image nor cleared by startup. A panic reboots through RTC_SW_CPU_RST, which
 * leaves internal SRAM alone, so the next boot can read what the previous one
 * was doing when it died.
 *
 * Temporary bring-up instrumentation. Remove along with ROCKBOX_SCAN_TRACE.
 */

#include <stdint.h>

#define SCAN_TRACE_MAGIC 0x42534e41u   /* "BSNA" */
#define SCAN_TRACE_N     96             /* records, four words each */

struct scan_ring
{
    uint32_t magic;
    uint32_t seq;
    uint32_t head;
    uint32_t rec[SCAN_TRACE_N][4];
};

/* .noinit keeps this out of the image and out of the startup zeroing, and lands
 * it in dram0_bss -- internal SRAM, which is what survives the reset. */
static struct scan_ring ring __attribute__((section(".noinit"), aligned(4)));

void scan_trace(int tag, unsigned long a, unsigned long b, unsigned long c)
{
    if (ring.magic != SCAN_TRACE_MAGIC)
    {
        ring.magic = SCAN_TRACE_MAGIC;
        ring.head  = 0;
        ring.seq   = 0;
    }

    uint32_t *r = ring.rec[ring.head];
    ring.head = (ring.head + 1) % SCAN_TRACE_N;
    r[0] = ++ring.seq;
    r[1] = (uint32_t)tag;
    r[2] = (uint32_t)a;
    r[3] = (uint32_t)b ^ (uint32_t)c; /* two arguments beat one; the tag says which */
}

/* Called from app_main() before anything Rockbox owns exists, so what it reads
 * is exactly what the previous boot left behind. */
void scan_trace_early_dump(void)
{
    extern int esp_rom_printf(const char *, ...);

    if (ring.magic != SCAN_TRACE_MAGIC)
        return;

    esp_rom_printf("TRACE from previous boot: seq=%u head=%u\n",
                   (unsigned)ring.seq, (unsigned)ring.head);

    /* Oldest slot first, so the tail of the output is the last thing the dead
     * boot recorded before it faulted. */
    for (unsigned n = 0; n < SCAN_TRACE_N; n++)
    {
        unsigned i = (ring.head + n) % SCAN_TRACE_N;
        const uint32_t *r = ring.rec[i];

        if (r[1] == 0)
            continue;
        esp_rom_printf("  %5u  t%02d  %08lx %08lx\n", (unsigned)r[0],
                       (int)r[1], (unsigned long)r[2], (unsigned long)r[3]);
    }

    /* One dump per crash, so a clean boot does not keep repeating it. The
     * slots are wiped too: .noinit is never cleared, so without this the print
     * mixes the current boot's records with an older boot's leftovers. */
    ring.magic = 0;
    ring.seq   = 0;
    ring.head  = 0;
    for (unsigned i = 0; i < SCAN_TRACE_N; i++)
        ring.rec[i][0] = ring.rec[i][1] = ring.rec[i][2] = ring.rec[i][3] = 0;
}
