/*
 * panic_cdc.c -- report a hardware exception through Rockbox's logf().
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
 * esp_panic_handler() is the earliest point at which IDF has re-enabled the flash
 * cache, so code linked here can execute, and it is handed a filled panic_info_t
 * whose ->frame is the XtExcFrame -- the whole register set, not just info->addr.
 * Hooking xt_unhandled_exception() instead would run with the cache still off and
 * fault on the first instruction.
 *
 * IDF defines this function strongly in esp_system/panic.c, and that object is
 * pulled into the link anyway for the watchdog helpers it also defines, so this
 * definition is a duplicate. It wins because it is an object file of the
 * executable, which ld reads before any archive, together with
 * -Wl,--allow-multiple-definition in idf-app/CMakeLists.txt. That is checked, not
 * assumed: the linked image must contain RB_PANIC_TAG and esp_panic_handler must
 * resolve into .flash.text, not to IDF's IRAM copy at 0x4037c1xx.
 *
 * The transport is logf(), not a hand-rolled CDC pump. logf() is a Rockbox
 * facility that already routes to the USB CDC console under USB_ENABLE_SERIAL
 * (firmware/logf.c flushes its ring through usb_serial_send()), and it flushes once
 * per message: usb_serial_send() queues at most TRANSIT_BUFFER_SIZE (32) bytes and
 * lets the driver's completion handler drain the rest. The previous version of this
 * file wrote 64-byte chunks in a loop straight into usb_serial_send(); with a 512
 * byte ring that cannot advance while a transfer is outstanding, it hammered the
 * endpoint. Interrupts are still enabled in IDF's panic path -- there is no
 * disable_interrupts in port/panic_handler.c or port/arch/xtensa/panic_arch.c -- so
 * the level-2 USB interrupt runs while this executes and moves the bytes.
 */

#include <stdio.h>
#include <string.h>

#include "esp_private/panic_internal.h"
#include "xtensa_context.h"
#include "panic_funcs.h"
#include "esp_rom_sys.h"

/* Rockbox, declared here rather than included: this file is compiled by IDF, which
 * does not carry Rockbox's include paths. This is the same seam rockbox_app.c uses.
 * _logf is what the logf() macro expands to when ROCKBOX_HAS_LOGF is set, which this
 * target does; the signature matches firmware/export/logf.h. */
extern void xtensa_panic_log(const char *msg);
extern int xtensa_breadcrumb_back(int back, char *buf, int bufsize);
extern int button_hw_any_pressed(void);
extern void esp_panic_handler_feed_wdts(void);
extern void _logf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Distinctive enough to grep for in the linked image, so a silent loss to the
 * archive copy of the symbol is visible before the board is flashed. */
#define RB_PANIC_TAG "*rb-panic*"

/* IDF arms the RTC watchdog for ten seconds before calling the handler. Feeding it
 * once a second lets the hold outlast that without the chip being cut off mid-read.
 * The hold exists so the host has time to drain its tty: nothing here can tell
 * whether anyone has the port open. */
#define RB_PANIC_HOLD_MS 8000
#define RB_PANIC_STEP_MS 100

/* Three, not twelve. The CDC ring is 512 bytes and every message is queued into it
 * whether or not the host is reading; the full ring is reported at boot by
 * xtensa_crash_report_logf() and shown on the debug page. */
#define RB_PANIC_TRAIL 3

void esp_panic_handler(panic_info_t *info)
{
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    char line[80];
    int back;
    int waited = 0;
    unsigned long pc = (unsigned long)(info->addr ? info->addr : 0);
    unsigned long excv = f ? (unsigned long)f->excvaddr : 0UL;
    unsigned long cause = f ? (unsigned long)f->exccause : 0UL;
    const char *reason = info->reason ? info->reason : "unknown";

    esp_panic_handler_feed_wdts();

    /* Short form into the surviving record: bounded by the record's 96-character
     * message field, and read back after the reset by xtensa_paniclog_text(). */
    snprintf(line, sizeof line, "%s %s pc=%08lx excv=%08lx cause=%lu",
             RB_PANIC_TAG, reason, pc, excv, cause);
    xtensa_panic_log(line);

    /* One message per line, each well inside the ring. The PC alone is enough to
     * resolve the fault against rockbox.elf with addr2line. */
    _logf("%s core %d %s pc=%08lx excv=%08lx cause=%lu",
          RB_PANIC_TAG, info->core, reason, pc, excv, cause);
    if (f)
    {
        _logf("  ps=%08lx sar=%08lx a0=%08lx sp=%08lx",
              (unsigned long)f->ps, (unsigned long)f->sar,
              (unsigned long)f->a0, (unsigned long)f->a1);
        _logf("  a12=%08lx a13=%08lx a14=%08lx a15=%08lx",
              (unsigned long)f->a12, (unsigned long)f->a13,
              (unsigned long)f->a14, (unsigned long)f->a15);
    }

    for (back = 0; back < RB_PANIC_TRAIL; back++)
    {
        if (xtensa_breadcrumb_back(back, line, sizeof line) == 0)
            break;
        _logf("  trail %s", line);
    }

    /* Hold so the host has time to drain, and so the screen keeps the last frame
     * instead of vanishing into a reboot. Any key ends it early. */
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
