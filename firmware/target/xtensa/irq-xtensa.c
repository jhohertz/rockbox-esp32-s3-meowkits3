/***************************************************************************
 * Level-1 interrupt dispatch for the esp32s3 core.

 * Called from the handler in vectors.S with the pending bitmask over all 32 slots.
 * The handler is entered for any level-1 slot, so this is where the slot is worked
 * out.

 * This has to be in .icode.irq, placed immediately after the vector table by app.lds:
 * the call0 from the handler is range-limited to 18 bits signed, and the table has to
 * be a physical address the core can fetch from, so it is in internal SRAM. Calls out
 * of the dispatch are not limited because -mlongcalls makes GCC emit callx0 with an
 * absolute literal instead of a range-limited call8, so the mixer and the tick tasks
 * can live in the flash cache window 1.6 MB away.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "kernel.h"
#include "tick.h"
#include "timer.h"
#include "xtensa.h"

void timer_irq(void);
void gdma_irq(void);

void VEC_ATTR xt_irq_dispatch(unsigned int pending)
{
    if (pending & (1u << XT_SLOT_TIMERG1))
        timer_irq();

    if (pending & (1u << XT_SLOT_GDMA_OUT_CH0))
        gdma_irq();
}
