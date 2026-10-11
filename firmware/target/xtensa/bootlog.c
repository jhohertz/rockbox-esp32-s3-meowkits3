/***************************************************************************
 * One character per milestone, straight out of UART0.
 *
 * There is no other way to see what the target is doing: esptool can only read
 * memory by putting the chip into download mode, which resets it, so a running
 * Rockbox is invisible from the outside. The mask ROM exports
 * ets_write_char_uart, and the ROM is windowed ABI, so every call goes through
 * xt_rom_call() (see rom_cache.S).
 *
 * This is bring-up scaffolding, not a logging subsystem.
 ****************************************************************************/
#include "config.h"

#include "rom_cache.h"

/* From components/esp_rom/esp32s3/ld/esp32s3.rom.ld, base file, so it does not
 * move between ROM revisions. */
#define ROM_WRITE_CHAR 0x40043ce8u

/* A mirror of the trace in internal SRAM. The UART is only readable while the
 * chip is running, and the only way to read memory is to put the chip into
 * download mode, which resets it. Internal SRAM survives a reset, so the log is
 * still there when esptool attaches.
 *
 * .srambss is NOLOAD in internal SRAM: crt0 zeroes it before psram_init(), which
 * is also why these globals cannot live in .bss - .bss is in the PSRAM window and
 * is not reachable until mmu_init() has mapped it.
 */
__attribute__((section(".srambss"))) volatile unsigned char boot_log[4096];
__attribute__((section(".srambss"))) volatile int boot_log_len;

/* ICODE_ATTR because boot_trace() is called from the PSRAM bring-up, which runs
 * from .iram while it is reprogramming the MSPI block that feeds the flash
 * instruction cache. A marker printed from .text would be fetched from the very
 * window being reconfigured.
 */
void ICODE_ATTR boot_trace(char c)
{
    int n = boot_log_len;

    if (n < 0 || n >= (int)sizeof boot_log)
        n = 0;
    boot_log[n] = (unsigned char)c;
    boot_log_len = n + 1;

    xt_rom_call(ROM_WRITE_CHAR, (unsigned int)c, 0);
}

void ICODE_ATTR boot_trace_hex(unsigned int v)
{
    /* No digit table: .rodata lives in the flash cache window, and a stalled
     * Ext_RAM FSM makes any read through it hang. Arithmetic keeps the log
     * writable even when the cache is wedged.
     */
    boot_trace('#');
    for (int i = 7; i >= 0; i--) {
        unsigned int d = (v >> (i * 4)) & 0xfu;
        boot_trace((char)(d < 10u ? '0' + d : 'a' + (d - 10u)));
    }
}

/* Four raw bytes, no ASCII. Used when the D-cache is switched off and the
 * "digits" string in .rodata is not reliably readable.
 */
void ICODE_ATTR boot_trace_raw(unsigned int v)
{
    int n = boot_log_len;

    if (n < 0 || n > (int)sizeof boot_log - 4)
        n = 0;
    boot_log[n] = (unsigned char)(v & 0xffu);
    boot_log[n + 1] = (unsigned char)((v >> 8) & 0xffu);
    boot_log[n + 2] = (unsigned char)((v >> 16) & 0xffu);
    boot_log[n + 3] = (unsigned char)((v >> 24) & 0xffu);
    boot_log_len = n + 4;
}
