/***************************************************************************
 * Bring-up trace, on the IDF console.
 *
 * See bootlog.h for why this exists at all. The rule it has to respect is
 * that some call sites still sit inside hardware callbacks (the I2S and SD
 * paths trace from where their completion runs), and putchar through the VFS
 * is not safe there. So an ISR-context call is parked in a small ring and the
 * next thread-context call drains it. Nothing is ever printed from an ISR.
 *
 * Losing trace bytes is acceptable; hanging the console is not.
 ****************************************************************************/

#include <stdio.h>
#include "bootlog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define RTC_NCOUNT 24
#define TRACE_RING 512

static volatile unsigned int counters[RTC_NCOUNT];
static volatile unsigned int dropped;

static char ring[TRACE_RING];
static volatile unsigned int ring_head;   /* written by ISR callers */
static volatile unsigned int ring_tail;   /* read by thread callers */

static void drain_ring(void)
{
    while (ring_tail != ring_head)
    {
        putchar(ring[ring_tail % TRACE_RING]);
        ring_tail++;
    }
}

void boot_trace_start(void)
{
    printf("\n--- rockbox bring-up trace ---\n");
}

void boot_phase(int n)
{
    drain_ring();
    printf("\n[phase %d]\n", n);
}

void boot_trace(char c)
{
    if (xPortInIsrContext())
    {
        unsigned int h = ring_head;
        if (h - ring_tail < TRACE_RING)
        {
            ring[h % TRACE_RING] = c;
            ring_head = h + 1;
        }
        else
        {
            dropped++;
        }
        return;
    }

    drain_ring();
    putchar(c);
}

void boot_trace_raw(unsigned int v)
{
    /* the bare-metal version pushed 32 bits as four raw bytes; on a text
     * console that is unreadable, so it becomes the same hex as _hex but
     * without the separating space */
    boot_trace_hex(v);
}

void boot_trace_hex(unsigned int v)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4)
        boot_trace(digits[(v >> i) & 0xf]);

    boot_trace(' ');
}

void rtc_bump(int idx)
{
    if ((unsigned int)idx < RTC_NCOUNT)
        counters[idx]++;
}

void rtc_set(int idx, unsigned int v)
{
    if ((unsigned int)idx < RTC_NCOUNT)
        counters[idx] = v;
}

void rtc_or(int idx, unsigned int v)
{
    if ((unsigned int)idx < RTC_NCOUNT)
        counters[idx] |= v;
}

unsigned int rtc_counter(int idx)
{
    if ((unsigned int)idx >= RTC_NCOUNT)
        return 0;

    return counters[idx];
}

unsigned int boot_trace_dropped(void)
{
    return dropped;
}
