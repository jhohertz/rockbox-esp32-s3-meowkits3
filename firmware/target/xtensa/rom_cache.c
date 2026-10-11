/***************************************************************************
 * Calling the ESP32-S3 ROM by absolute address.
 *
 * Rockbox links -nostdlib, so the ROM functions are reached through a function
 * pointer rather than through ESP-IDF. Under the windowed ABI that needs no
 * special handling: GCC emits l32r + callx8 for an out-of-range call, which is
 * what ESP-IDF's own callers use, and the argument/return-value shuffling between
 * the caller's window and the callee's is the ABI's normal behaviour.
 *
 * The trampoline that used to live here existed only because Rockbox built with
 * -mabi=call0; a call0 caller has to move its arguments into a10/a11 by hand to
 * look like a windowed caller, and read the result back out of a10.
 *
 * .icode because every ROM call in bring-up happens while the MSPI block is being
 * reprogrammed, so the caller must not be fetched from the flash instruction
 * cache window it is calling into the ROM from.
 ****************************************************************************/
#include "config.h"
#include "rom_cache.h"

ICODE_ATTR unsigned int xt_rom_call(unsigned int fn,
                                    unsigned int arg0,
                                    unsigned int arg1)
{
    return ((unsigned int (*)(unsigned int, unsigned int))fn)(arg0, arg1);
}
