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

/* A mirror of the trace in RTC slow memory.
 *
 * The UART is only readable while the chip is running, and the only way to read
 * memory is to put the chip into download mode, which resets it. Internal SRAM is
 * wiped by crt0, so a .srambss log always shows a fresh boot - never the state the
 * previous run hung in.
 *
 * RTC slow memory (8 KB at 0x50000000) lives in the RTC domain and is NOT cleared
 * by a hard reset; verified by writing a word, resetting, and reading it back.
 * So the ring goes here, and crt0 stamps a '\n' at the start of each run. The
 * probe reads the whole 8 KB and takes the segment after the last '\n', which is
 * the trace of the run that just hung.
 */
#define RTC_SLOW_BASE 0x50000000u
#define RTC_LOG_SIZE  3072u   /* must stop before RTC_LIVE_ADDR */
#define RTC_LIVE_ADDR     (RTC_SLOW_BASE + 3072u)   /* 24 liveness counters */
#define RTC_HIST_ADDR     (RTC_SLOW_BASE + 3200u)   /* handler cause histogram */
#define RTC_DIAG_ADDR     (RTC_SLOW_BASE + 3328u)   /* handler diagnostics */
#define RTC_SWITCH_ADDR   (RTC_SLOW_BASE + 3456u)   /* 32 (corep,thread) pairs */
#define RTC_LOOP_ADDR     (RTC_SLOW_BASE + 3584u)   /* switch_thread loop state */
#define RTC_EXC_ADDR      (RTC_SLOW_BASE + 3712u)   /* 96-word exception dump */
#define RTC_LOG_LEN_ADDR  (RTC_SLOW_BASE + 4100u)
#define RTC_IRQ_ADDR      (RTC_SLOW_BASE + 4104u)
#define RTC_TICK_ADDR     (RTC_SLOW_BASE + 4108u)
#define RTC_PREV_TICK     (RTC_SLOW_BASE + 4112u)
#define RTC_PREV_IRQ      (RTC_SLOW_BASE + 4116u)
static volatile unsigned char *const rtc_log =
    (volatile unsigned char *)RTC_SLOW_BASE;
static volatile unsigned int *const rtc_log_len =
    (volatile unsigned int *)RTC_LOG_LEN_ADDR;

/* Kept as a symbol so probes can still find it by name; the authoritative copy is
 * the RTC one. */
__attribute__((section(".srambss"))) volatile unsigned char boot_log[8192];
__attribute__((section(".srambss"))) volatile int boot_log_len;

/* Exception scratch. This must not be a raw address: the previous location,
 * 0x3FC90100, is the data-bus alias of 0x40378100, which is inside .iram - the
 * handler was overwriting its own code, and a memory dump of it looked like a
 * fault record when it was nothing of the kind.
 */
__attribute__((section(".srambss"))) volatile unsigned int diag_record[2][12];
__attribute__((section(".srambss"))) volatile unsigned int xt_irq_level_hist[64];
__attribute__((section(".srambss"))) volatile unsigned int xt_irq_caller[64];
__attribute__((section(".srambss"))) volatile unsigned int xt_irq_level_pos;

/*
 * Window bookkeeping across the level-1 handler and the context switch.
 * xt_isr_hist[0] = WINDOWBASE at handler entry, [1] = WINDOWBASE just before
 * rfe, [2] = how many times the handler ran, [3] = how many of those left
 * WINDOWBASE different from what it found.
 *
 * xt_ctx_hist records what load_context installs: a0, a1, PS, WINDOWBASE,
 * WINDOWSTART, and the number of switches.
 */
__attribute__((section(".srambss"))) volatile unsigned int xt_isr_hist[16];
__attribute__((section(".srambss"))) volatile unsigned int xt_ctx_hist[8];

/* Histogram of EXCCAUSE for every level-1 vector entry. The level-1 vector is
 * shared by interrupts and exceptions, so this is the only way to tell a tick
 * from a fault that the trap path never reached. */
__attribute__((section(".srambss"))) volatile unsigned int xt_exc_cause[40];

/* First few switch_thread() calls: [2i] = corep, [2i+1] = thread. */
__attribute__((section(".srambss"))) volatile unsigned int xt_switch_trace[16];
__attribute__((section(".srambss"))) volatile int xt_switch_trace_n;

/* ICODE_ATTR because boot_trace() is called from the PSRAM bring-up, which runs
 * from .iram while it is reprogramming the MSPI block that feeds the flash
 * instruction cache. A marker printed from .text would be fetched from the very
 * window being reconfigured.
 */
/* Start a fresh run in the RTC ring. The ring is not cleared by a reset, so
 * without this consecutive boots concatenate and the probe cannot tell where one
 * run ends and the next begins.
 */
/* Liveness counters, separate from the character ring. A hang is easier to
 * localise by asking "which of these stopped moving?" than by reading a trace:
 *
 *   [0] switch_thread entries   [1] its inner loop iterations
 *   [2] core_sleep entries      [3] current_tick
 *   [4] sdmmc commands issued   [5] sdmmc event waits
 *   [6] sdmmc wait timeouts     [7] sleep() calls
 *   [8] tick-thread iterations  [9] timeout scans
 *
 * .srambss so crt0 zeroes them on every reset, which makes the deltas readable
 * from one probe to the next.
 */
__attribute__((section(".srambss"))) volatile unsigned int xt_live[32];

void ICODE_ATTR rtc_bump(int idx)
{
    xt_live[idx]++;
    /* RTC slow memory is not cached, so it is the only copy esptool can see:
     * a dump_mem of internal SRAM goes through the D-cache and shows stale
     * values for anything the running image wrote. */
    ((volatile unsigned int *)RTC_LIVE_ADDR)[idx] = xt_live[idx];
}

void ICODE_ATTR rtc_set(int idx, unsigned int v)
{
    xt_live[idx] = v;
    ((volatile unsigned int *)RTC_LIVE_ADDR)[idx] = v;
}

/* Accumulate a bitmask, so a probe sees every bit that ever appeared rather
 * than only the last value.
 */
void ICODE_ATTR rtc_or(int idx, unsigned int v)
{
    xt_live[idx] |= v;
    ((volatile unsigned int *)RTC_LIVE_ADDR)[idx] = xt_live[idx];
}

void ICODE_ATTR boot_trace_start(void)
{
    int n = (int)*rtc_log_len;

    /* Snapshot the previous run's liveness counters before starting a new ring:
     * esptool can only read memory by resetting the chip, so the only way to see
     * where the *hung* run stopped is to stash its state for the next one.
     */
    *(volatile unsigned int *)RTC_PREV_TICK = *(volatile unsigned int *)RTC_TICK_ADDR;
    *(volatile unsigned int *)RTC_PREV_IRQ  = *(volatile unsigned int *)RTC_IRQ_ADDR;

    /* Start each run at offset 0 so raw[0..len-1] is always the trace in order;
     * when it overflows it wraps and keeps the tail, which is the part that
     * matters for a hang. */
    n = 0;
    for (int i = 0; i < (int)RTC_LOG_SIZE; i++)
        rtc_log[i] = 0;
    rtc_log[n] = '\n';
    *rtc_log_len = (unsigned int)(n + 1);

    /* Fresh-run marker: if 0x42414345 ('FACE') is not in the live block, the
     * image being read is not the one that just ran.
     */
    {
        volatile unsigned int *live = (volatile unsigned int *)RTC_LIVE_ADDR;
        for (int i = 0; i < 32; i++)
            live[i] = 0;
        live[15] = 0x42414345u;
    }

    /* Clear the exception record too. RTC slow memory survives the reset, so
     * without this a trap from a previous run looks like the current one.
     */
    {
        volatile unsigned int *exc = (volatile unsigned int *)RTC_EXC_ADDR;
        for (int i = 0; i < 96; i++)
            exc[i] = 0;
    }

    /* The handler histogram and window counters live in RTC too, so they must be
     * zeroed as well; otherwise a probe reads the previous run's numbers. */
    {
        volatile unsigned int *h = (volatile unsigned int *)RTC_HIST_ADDR;
        for (int i = 0; i < 32; i++)
            h[i] = 0;
        /* window counters, non-tick exception record and heartbeat. The diag
         * record lives at 0x50001900, clear of the phase ring which ends at
         * 0x50001500. */
        for (int i = 0; i < 400; i++)
            *(volatile unsigned int *)(0x50001400u + 4u * (unsigned)i) = 0;
        *(volatile unsigned int *)0x50000D60u = 0;
    }
}

void ICODE_ATTR boot_trace(char c)
{
    int n = (int)*rtc_log_len;

    if (n < 0 || n >= (int)RTC_LOG_SIZE)
        n = 0;
    rtc_log[n] = (unsigned char)c;
    *rtc_log_len = (unsigned int)(n + 1);

    boot_log[n < (int)sizeof boot_log ? n : 0] = (unsigned char)c;
    boot_log_len = n + 1;

    *(volatile unsigned int *)UART0_FIFO = (unsigned int)(unsigned char)c;
    usb_cdc_putc(c);
}

/* Marks the start of a run in the RTC ring so the probe can tell one boot from
 * the next. */


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

/* Compact phase stamp: '>' followed by two hex digits. main()'s init sequence
 * has more steps than the alphabet, so the milestones are numbered.
 */
/* The character ring is 3 KB and the SPI/LCD traces fill it in seconds, so the
 * phase sequence gets its own ring in the otherwise unused upper half of RTC
 * slow memory. 1 KB is 128 phase stamps, far more than boot needs.
 */
#define RTC_PHASE_ADDR  0x50001100u
#define RTC_PHASE_LEN   1024u
#define RTC_PHASE_MAX   (RTC_SLOW_BASE + 0x1500u)

void ICODE_ATTR boot_phase(int n)
{
    static unsigned int pos;   /* .srambss-backed static is fine: crt0 zeroes it */

    *(volatile unsigned int *)RTC_PHASE_MAX = (unsigned)n;

    if (pos >= RTC_PHASE_LEN)
        pos = 0;
    *(volatile unsigned char *)(RTC_PHASE_ADDR + pos) = (unsigned char)n;
    pos++;
}

/* Four raw bytes, no ASCII. Used when the D-cache is switched off and the
 * "digits" string in .rodata is not reliably readable.
 */
/* Dump the timer-group-1 state: the thing that decides whether the scheduler
 * ever sees a tick. */
void ICODE_ATTR boot_trace_timer(void)
{
    volatile unsigned int *p = (volatile unsigned int *)0x60020000u;
    boot_trace_hex(p[0]);              /* TN_CONFIG(0)  */
    boot_trace_hex(p[1]);              /* TN_LO(0)      */
    boot_trace_hex(p[4]);              /* TN_ALARM(0)   */
    boot_trace_hex(p[28]);             /* TN_INT_ENA    */
    boot_trace_hex(p[29]);             /* TN_INT_RAW    */
    boot_trace_hex(p[30]);             /* TN_INT_ST     */
    boot_trace_hex(*(volatile unsigned int *)0x600C2004u); /* map reg, not INTENABLE */
    /* Second sample, taken later, to see whether the counter is moving. */
    boot_trace_hex(p[1]);
    boot_trace_hex(p[2]);
}

/* State of SPI0's external-RAM cache: the path every PSRAM access goes through.
 * A stalled Ext_RAM FSM is the signature of a write the hardware could not
 * complete, and the reject registers name the address it refused. */
void ICODE_ATTR boot_trace_extmem(void)
{
    boot_trace_hex(*(volatile unsigned int *)0x600C4130u); /* EXTMEM_CACHE_STATE      */
    boot_trace_hex(*(volatile unsigned int *)0x600C4100u); /* EXTMEM_DBUS_REJECT_ST   */
    boot_trace_hex(*(volatile unsigned int *)0x600C4104u); /* EXTMEM_DBUS_REJECT_VADDR*/
    boot_trace_hex(*(volatile unsigned int *)0x600C4120u); /* EXTMEM_CACHE_MMU_FAULT  */
    boot_trace_hex(*(volatile unsigned int *)0x600C4000u); /* EXTMEM_DCACHE_CTRL      */
    boot_trace_hex(*(volatile unsigned int *)0x60003054u); /* SPI0 FSM state          */
    boot_trace_hex(*(volatile unsigned int *)0x600C40B4u); /* IBUS_TO_FLASH_START     */
    boot_trace_hex(*(volatile unsigned int *)0x600C40B8u); /* IBUS_TO_FLASH_END       */
    boot_trace_hex(*(volatile unsigned int *)0x600C40BCu); /* DBUS_TO_FLASH_START     */
    boot_trace_hex(*(volatile unsigned int *)0x600C40C0u); /* DBUS_TO_FLASH_END       */
    boot_trace_hex(*(volatile unsigned int *)0x600C40E4u); /* CACHE_ILG_INT_ST        */
    boot_trace_hex(*(volatile unsigned int *)0x600C4124u); /* CACHE_MMU_FAULT_VADDR   */
    boot_trace_hex(*(volatile unsigned int *)0x600C40D4u); /* DBUS SPIRAM miss count  */
    boot_trace_hex(*(volatile unsigned int *)0x600C4004u); /* EXTMEM_ICACHE_CTRL      */
    /* MMU entries covering .text (IROM window) */
    for (int e = 144; e < 153; e++)
        boot_trace_hex(*(volatile unsigned int *)(0x600C5000u + 4u * (unsigned)e));
}

/* Print the MMU entries covering a given address range, plus the PSRAM flag. */
void ICODE_ATTR boot_trace_mmu(unsigned int vaddr, int pages)
{
    volatile unsigned int *table = (volatile unsigned int *)0x600C5000u;
    int first = (int)((vaddr & 0x1FFFFFFu) >> 16);
    for (int i = first; i < first + pages; i++)
        boot_trace_hex(table[i]);
}

void ICODE_ATTR boot_trace_raw(unsigned int v)
{
    int n = (int)*rtc_log_len;

    if (n < 0 || n > (int)RTC_LOG_SIZE - 4)
        n = 0;
    rtc_log[n] = (unsigned char)(v & 0xffu);
    rtc_log[n + 1] = (unsigned char)((v >> 8) & 0xffu);
    rtc_log[n + 2] = (unsigned char)((v >> 16) & 0xffu);
    rtc_log[n + 3] = (unsigned char)((v >> 24) & 0xffu);
    *rtc_log_len = (unsigned int)(n + 4);
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
