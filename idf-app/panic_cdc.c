/*
 * panic_cdc.c -- report a hardware exception over the USB CDC console.
 *
 * Rockbox's panic() draws on the LCD and then holds the glass. A hardware
 * exception never reaches it: it goes to IDF's xt_unhandled_exception(), which
 * prints to UART0 -- a port this board gives to Rockbox, so nobody sees it -- and
 * then resets, because CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT is set with
 * CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0. IDF keeps the exception frame on
 * the stack and stores nothing durable except the reset-reason hint, so the fault
 * address was simply lost. Coredump is disabled
 * (CONFIG_ESP_COREDUMP_ENABLE_TO_NONE), which confirms nothing else is retained.
 *
 * esp_panic_handler() is the earliest point at which IDF has already re-enabled the
 * flash cache, so code linked here can execute; hooking xt_unhandled_exception()
 * instead would run with the cache off and fault on the first instruction. IDF hands
 * us a filled panic_info_t whose ->frame is the XtExcFrame, so the registers are all
 * present -- info->addr alone is enough to resolve a fault against rockbox.elf.
 *
 * IDF defines this function strongly in esp_system/panic.c, and that object is pulled
 * into the link anyway for the watchdog helpers it also defines, so this definition is
 * a duplicate. It wins because it is an object file of the executable and ld reads
 * objects before any archive, together with -Wl,--allow-multiple-definition from
 * idf-app/CMakeLists.txt. That is checked, not assumed: the linked image has to
 * contain RB_PANIC_TAG below.
 *
 * Interrupts are not masked in IDF's panic path -- there is no disable_interrupts in
 * port/panic_handler.c or port/arch/xtensa/panic_arch.c -- so the level-2 USB
 * interrupt still runs while we write, and the CDC IN endpoint drains between chunks.
 * usb_serial_send() only fills the stack's transmit ring and returns; it does not
 * wait on a semaphore, which is what makes calling it from exception context safe.
 */

#include <stdio.h>
#include <string.h>

#include "esp_private/panic_internal.h"
#include "xtensa_context.h"
#include "panic_funcs.h"
#include "esp_rom_sys.h"

/* Rockbox, declared here rather than included: this file is compiled by IDF, which
 * does not carry Rockbox's include paths. This is the same seam rockbox_app.c uses.
 * Note there is no udelay() here: Rockbox's is a static inline in system-target.h, so
 * it is not a linkable symbol, and esp_rom_delay_us() is the ROM routine IDF's own
 * panic path uses -- reachable from exception context, which a ccount spin would also
 * be, but this needs no cpu_frequency. */
extern void xtensa_panic_log(const char *msg);
extern int xtensa_breadcrumb_back(int back, char *buf, int bufsize);
extern void usb_serial_send(const unsigned char *data, int length);
extern int button_hw_any_pressed(void);
extern void esp_panic_handler_feed_wdts(void);

/* Distinctive enough to grep for in the linked image, so a silent loss to the
 * archive copy of the symbol is visible before the board is flashed. */
#define RB_PANIC_TAG "*rb-panic*"

/* IDF arms the RTC watchdog for ten seconds before calling the handler. Feeding it
 * once a second lets the hold outlast that without the chip being cut off mid-read. */
#define RB_PANIC_HOLD_MS 15000
#define RB_PANIC_STEP_MS 100
#define RB_PANIC_TRAIL 12

static void cdc_write(const char *s)
{
    while (*s != '\0')
    {
        int chunk = 0;

        while (chunk < 64 && s[chunk] != '\0')
            chunk++;
        usb_serial_send((const unsigned char *)s, chunk);
        s += chunk;
        /* A chunk at a time, with the endpoint given a moment to move it. The
         * transport is full-speed bulk, so 2 ms is far more than the bytes need. */
        esp_rom_delay_us(2000);
    }
}

void esp_panic_handler(panic_info_t *info)
{
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    char line[80];
    char head[256];
    int back;
    int waited = 0;

    esp_panic_handler_feed_wdts();

    /* Short form into the surviving record: the debug page's "Prev panic" row and
     * the boot report both read this, and it is bounded by the record's message
     * field. The reason string plus the faulting address is what identifies a
     * fault once rockbox.elf is on the host. */
    snprintf(line, sizeof line, "%s %s pc=%08lx excv=%08lx cause=%lu",
             RB_PANIC_TAG,
             info->reason ? info->reason : "unknown",
             (unsigned long)(info->addr ? info->addr : 0),
             f ? (unsigned long)f->excvaddr : 0UL,
             f ? (unsigned long)f->exccause : 0UL);
    xtensa_panic_log(line);

    snprintf(head, sizeof head,
             "\r\n%s core %d %s\r\n"
             "  pc=%08lx  excvaddr=%08lx  exccause=%lu  ps=%08lx  sar=%08lx\r\n"
             "  a0=%08lx  sp=%08lx  a12=%08lx  a13=%08lx  a14=%08lx  a15=%08lx\r\n",
             RB_PANIC_TAG, info->core,
             info->reason ? info->reason : "unknown",
             (unsigned long)(info->addr ? info->addr : 0),
             f ? (unsigned long)f->excvaddr : 0UL,
             f ? (unsigned long)f->exccause : 0UL,
             f ? (unsigned long)f->ps : 0UL,
             f ? (unsigned long)f->sar : 0UL,
             f ? (unsigned long)f->a0 : 0UL,
             f ? (unsigned long)f->a1 : 0UL,
             f ? (unsigned long)f->a12 : 0UL,
             f ? (unsigned long)f->a13 : 0UL,
             f ? (unsigned long)f->a14 : 0UL,
             f ? (unsigned long)f->a15 : 0UL);
    cdc_write(head);

    cdc_write("  trail, newest first:\r\n");
    for (back = 0; back < RB_PANIC_TRAIL; back++)
    {
        if (xtensa_breadcrumb_back(back, line, sizeof line) == 0)
            break;
        cdc_write("  ");
        cdc_write(line);
        cdc_write("\r\n");
    }

    /* Hold so the host has time to drain its tty and so the screen keeps the last
     * frame instead of vanishing into a reboot. Any key ends it early. */
    while (waited < RB_PANIC_HOLD_MS)
    {
        if (button_hw_any_pressed())
            break;
        esp_rom_delay_us(RB_PANIC_STEP_MS * 1000);
        waited += RB_PANIC_STEP_MS;
        if ((waited % 1000) == 0)
            esp_panic_handler_feed_wdts();
    }

    /* IDF's own reset path. The RTC watchdog is still armed at this point. */
    panic_restart();
}
