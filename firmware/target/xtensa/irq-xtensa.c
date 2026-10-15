/***************************************************************************
 * Level-1 interrupt dispatch for the esp32s3 core.

 * Called from the handler in vectors.S with the pending bitmask over all 32 slots.
 * The handler is entered for any level-1 slot, so this is where the slot is worked
 * out.

 * This has to be in .icode.irq, placed immediately after the vector table by app.lds:
 * the handler reaches it with a range-limited call8, and the table has to be a
 * physical address the core can fetch from, so it is in internal SRAM. Calls out
 * of the dispatch are not limited because -mlongcalls makes GCC emit callx8 with an
 * absolute literal instead of a range-limited call8, so the mixer and the tick tasks
 * can live in the flash cache window 1.6 MB away.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "kernel.h"
#include "tick.h"
#include "timer.h"
#include "bootlog.h"
#include "xtensa.h"

void timer_irq(void);
void gdma_irq(void);
void sdmmc_irq(void);
void i2c_irq(void);
#ifdef HAVE_RECORDING
void gdma_rx_irq(void);
#endif
#ifdef HAVE_USBSTACK
void INT_USB_FUNC(void);
#endif

/* The level-1 vector is entered far more often than the 100 Hz tick. Counting
 * the vector entry (vectors.S) and the dispatch separately is the only way to
 * tell a spurious entry from a real one.
 */
__attribute__((section(".srambss"))) volatile unsigned int xt_dispatch_n;
__attribute__((section(".srambss"))) volatile unsigned int xt_pending_last;

void VEC_ATTR xt_irq_dispatch(unsigned int pending)
{
    xt_dispatch_n++;
    xt_pending_last = pending;
    /* Accumulate rather than overwrite: the last dispatch tells us little, the
     * union of every pending bitmask tells us which slots ever reach dispatch.
     */
    rtc_bump(13);
    rtc_or(12, pending);

    /* Bisection: dispatch does nothing but return. */
    (void)pending;

#ifdef HAVE_RECORDING
    if (pending & (1u << XT_SLOT_GDMA_IN_CH0))
        gdma_rx_irq();
#endif

#ifdef HAVE_USBSTACK
    if (pending & (1u << XT_SLOT_USB))
        INT_USB_FUNC();
#endif
}
