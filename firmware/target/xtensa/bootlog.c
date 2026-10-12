/***************************************************************************
 * One character per milestone, straight out of UART0.
 *
 * There is no other way to see what the target is doing: esptool can only read
 * memory by putting the chip into download mode, which resets it, so a running
 * Rockbox is invisible from the outside. The mask ROM exports
 * ets_write_char_uart, but we do not use it: uart_tx_one_char dispatches through
 * ets_get_printf_channel, which reads g_usb_print / g_uart_print out of the ROM's
 * own data section at 0x3fceffb8 - a page in the Ext_RAM cache window that nobody
 * arms before the application runs, so the ROM's printf faults on a
 * LoadProhibited. Writing the UART FIFO directly is shorter and safe.
 *
 * This is bring-up scaffolding, not a logging subsystem.
 ****************************************************************************/
#include "config.h"

/* DR_REG_UART_BASE for esp32s3; UART_FIFO_REG(0) is the first register. */
#define UART0_FIFO 0x6000B000u

/* DR_REG_USB_SERIAL_JTAG_BASE for esp32s3. The ESP32-S3 has a hardware USB
 * device that enumerates as a CDC serial port (/dev/ttyACM0) with no firmware
 * involvement at all - the mask ROM uses it for download mode, and ESP-IDF uses
 * it for its console. Writing a byte to EP1_REG and then setting WR_DONE in
 * EP1_CONF pushes it into the 64-byte Tx FIFO, which the host drains.
 *
 * EP1_CONF bit1 (SERIAL_IN_EP_DATA_FREE) is set while the Tx FIFO has room;
 * it clears after WR_DONE until the host reads the byte.
 */
#define USB_JTAG_BASE   0x60038000u
#define USB_JTAG_EP1    0x60038000u
#define USB_JTAG_EP1_CONF 0x60038004u
#define USB_JTAG_MISC_CONF 0x60038044u
#define USB_JTAG_DATA_FREE 0x2u
#define USB_JTAG_WR_DONE   0x1u

/* The host may not be reading. A 64-byte FIFO fills in 64 characters and then
 * DATA_FREE stays clear, so an unbounded wait would hang the bring-up. Bail out
 * instead: the SRAM mirror is still written, so the log is recoverable via
 * esptool even when nobody is watching the serial port.
 */
/* .srambss rather than .bss: usb_cdc_init() runs from crt0, before mmu_init()
 * has mapped the PSRAM window that .bss lives in. A store to an MMU entry the
 * ROM loader armed to flash has no write path, so a plain static here is not
 * reliably set before the first character is emitted.
 */
__attribute__((section(".srambss"))) static int usb_cdc_ok;

static void usb_cdc_putc(char c)
{
    volatile unsigned int *conf = (volatile unsigned int *)USB_JTAG_EP1_CONF;
    int guard = 200000;

    if (!usb_cdc_ok)
        return;

    while (!(conf[0] & USB_JTAG_DATA_FREE) && guard > 0)
        guard--;

    if (guard <= 0) {
        usb_cdc_ok = 0;   /* host is not draining; stop trying */
        return;
    }

    *(volatile unsigned int *)USB_JTAG_EP1 = (unsigned int)(unsigned char)c;
    conf[0] |= USB_JTAG_WR_DONE;
}

void ICODE_ATTR usb_cdc_init(void)
{
    /* Force the peripheral clock on; it is gated by default. */
    *(volatile unsigned int *)USB_JTAG_MISC_CONF |= 0x1u;
    usb_cdc_ok = 1;
}

/* A mirror of the trace in internal SRAM. The UART is only readable while the
 * chip is running, and the only way to read memory is to put the chip into
 * download mode, which resets it. Internal SRAM survives a reset, so the log is
 * still there when esptool attaches.
 *
 * .srambss is NOLOAD in internal SRAM: crt0 zeroes it before psram_init(), which
 * is also why these globals cannot live in .bss - .bss is in the PSRAM window and
 * is not reachable until mmu_init() has mapped it.
 */
__attribute__((section(".srambss"))) volatile unsigned char boot_log[8192];
__attribute__((section(".srambss"))) volatile int boot_log_len;

/* Exception scratch. This must not be a raw address: the previous location,
 * 0x3FC90100, is the data-bus alias of 0x40378100, which is inside .iram - the
 * handler was overwriting its own code, and a memory dump of it looked like a
 * fault record when it was nothing of the kind.
 */
__attribute__((section(".srambss"))) volatile unsigned int diag_record[2][12];

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

    *(volatile unsigned int *)UART0_FIFO = (unsigned int)(unsigned char)c;
    usb_cdc_putc(c);
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

/* Bring-up probe for the Xtensa frame convention: crt0 sets a1 to _stackend and
 * then calls this, so the difference between the printed a1 and _stackend says
 * whether ENTRY adds or subtracts the frame size, and where locals sit relative
 * to a1. The level-1 handler's manual frame depends on the answer.
 */
void ICODE_ATTR frame_probe(void)
{
    unsigned int sp;
    int local = 0x5a5a5a5a;

    asm volatile ("mov %0, a1" : "=r"(sp));
    boot_trace('F');
    boot_trace_hex(sp);
    boot_trace_hex((unsigned int)&local);
}
