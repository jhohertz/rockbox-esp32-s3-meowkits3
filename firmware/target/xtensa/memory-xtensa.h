#ifndef MEMORY_XTENSA_H
#define MEMORY_XTENSA_H

/*
 * Sizes for the buffers Rockbox expects its linker script to place.
 *
 * Only memory-xtensa-syms.S includes this today, but the sizes are also what
 * any C side has to agree with: rockbox_app.c in the IDF project creates the
 * FreeRTOS task on the buffer named stackbegin here, and its size is stated
 * separately because that file cannot include Rockbox headers.
 *
 * Everything here depends on config.h having been included first.
 */

/* Half of PSRAM. On every other Rockbox target the audiobuffer is whatever is
 * left of one contiguous memory region after the firmware; here IDF owns that
 * region's other half as its general heap, so the split has to be stated.
 * Making this larger takes memory straight out of IDF's PSRAM heap. */
#define XT_AUDIOBUF_SIZE    (MEMORYSIZE * 0x80000)

/* Rockbox's thread 0 stack, which is the FreeRTOS task stack the whole of
 * Rockbox runs on. The bare-metal script used the same 0x8000. */
#define XT_STACK_SIZE       0x8000

#endif /* MEMORY_XTENSA_H */
