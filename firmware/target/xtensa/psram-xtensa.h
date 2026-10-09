/***************************************************************************
 * ESP32-S3 external RAM (octal PSRAM) bring-up.
 *
 * psram_init() must run before anything reads through the 0x3C000000 window:
 * the MMU can map a page to PSRAM, but the chip only answers once its mode
 * registers and SPI0's Ext_RAM phases are set up.
 ****************************************************************************/
#ifndef PSRAM_XTENSA_H
#define PSRAM_XTENSA_H

#include <stdbool.h>

bool psram_init(void);
bool psram_present(void);

#endif /* PSRAM_XTENSA_H */
