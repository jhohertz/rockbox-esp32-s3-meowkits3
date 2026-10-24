/*
 * panic_cdc.c -- retain a hardware exception's state across the reset.
 *
 * Rockbox's panic() draws on the LCD and then holds the glass. A hardware
 * exception never reaches it: it goes to IDF's xt_unhandled_exception(), which
 * prints to UART0 -- a port this board gives to Rockbox, so nobody sees it -- and
 * then resets, because CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT is set with
 * CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0. IDF keeps the exception frame on
 * the stack and stores nothing durable except the reset-reason hint
 * (CONFIG_ESP_COREDUMP_ENABLE_TO_NONE confirms nothing else is retained), so the
 * fault address was simply lost.
 *
 * esp_panic_handler() is the earliest point at which IDF has re-enabled the flash
 * cache, so code linked here can execute; hooking xt_unhandled_exception() instead
 * would run with the cache off and fault on its first instruction. IDF hands us a
 * filled panic_info_t whose ->frame is the exception frame.
 *
 * IDF defines this function strongly in esp_system/panic.c, in an object the link
 * pulls anyway for the watchdog helpers it also defines, so this is a duplicate
 * definition. It wins because it is an object file of the executable, which ld
 * reads before any archive, together with -Wl,--allow-multiple-definition from
 * idf-app/CMakeLists.txt. Checked, not assumed: esp_panic_handler must resolve into
 * .flash.text, not to IDF's IRAM helpers at 0x4037c1xx.
 *
 * Nothing is written to the console here. The recorder's retained frame carries
 * PS.INTLEVEL = 4, which is debug exception level: _DoubleExceptionVector does
 * `break 1,4` (xtensa_vectors.S:586-593) and IDF then reaches this handler from
 * that context. At level 4 every interrupt is masked, including the level-2 USB
 * interrupt, so a CDC write cannot drain no matter how it is queued -- an earlier
 * version of this file wrote logf lines here and none of them ever arrived. The
 * only channel that works from this context is RTC slow, so everything goes there
 * and is printed by xtensa_crash_report_logf() at the next boot. Dropping the
 * console writes also removes a suspect: a fault inside our own handler would
 * itself promote to a double exception.
 *
 * The hold is two seconds rather than eight. In this context it is not a courtesy
 * to the host -- there is nothing the host could receive -- it is two seconds of
 * interrupts masked on a board that was mid-audio.
 */

#include <stdio.h>
#include <string.h>

#include "esp_private/panic_internal.h"
#include "esp_private/cache_err_int.h"
#include "xtensa_context.h"
#include "panic_funcs.h"
#include "esp_rom_sys.h"

/* Rockbox, declared here rather than included: this file is compiled by IDF, which
 * does not carry Rockbox's include paths. Same seam rockbox_app.c uses. */
extern void xtensa_panic_log(const char *msg);
extern void xtensa_paniclog_frame(const unsigned long *words, int count,
                                  int exception, int pseudo);
extern void xtensa_paniclog_frame_regs(const unsigned long *regs, int count);
extern int button_hw_any_pressed(void);
extern void esp_panic_handler_feed_wdts(void);

/* Distinctive enough to grep for in the linked image, so a silent loss to the
 * archive copy of the symbol is visible before the board is flashed. */
#define RB_PANIC_TAG "*rb-panic*"

#define RB_PANIC_HOLD_MS 2000
#define RB_PANIC_STEP_MS 100

/* Order of the register array handed to xtensa_paniclog_frame_regs():
 * ps, epc1, epc2, epc3, epc4, depc, cache_vaddr, cache_size. */
#define RB_REGS 8

/* EPC and DEPC are only readable at their own exception levels: reading EPC1 at
 * level 1 is a DISASTER exception, which would manufacture a third fault and
 * destroy the state being read. So PS is read first -- that is legal at any level
 * -- and the rest is only touched at level 4, where the retained frame says we
 * are. If a future crash reaches this handler at level 1, the register line is
 * simply absent, and its absence is itself the answer. */
static unsigned long xt_psr(int level)
{
    unsigned long v = 0;

    switch (level)
    {
        case 1: __asm__ __volatile__ ("rsr.epc1 %0" : "=r"(v)); break;
        case 2: __asm__ __volatile__ ("rsr.epc2 %0" : "=r"(v)); break;
        case 3: __asm__ __volatile__ ("rsr.epc3 %0" : "=r"(v)); break;
        case 4: __asm__ __volatile__ ("rsr.epc4 %0" : "=r"(v)); break;
        default: break;
    }
    return v;
}

void esp_panic_handler(panic_info_t *info)
{
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    char line[96];
    unsigned long regs[RB_REGS];
    int waited = 0;
    int i;
    /* info->addr is frame->pc, but IDF leaves it NULL on the cache-error path,
     * which is why the first reports printed pc=00000000. The frame is the
     * reliable source: XtExcFrame carries pc at offset 4. */
    unsigned long pc = f ? (unsigned long)f->pc
                         : (unsigned long)(info->addr ? info->addr : 0);
    unsigned long excv = f ? (unsigned long)f->excvaddr : 0UL;
    unsigned long cause = f ? (unsigned long)f->exccause : 0UL;
    const char *reason = info->reason ? info->reason : "unknown";
    unsigned long ps;

    esp_panic_handler_feed_wdts();

    /* The frame verbatim, before anything else. Its layout is the open question,
     * so no attempt is made to interpret it here; the host has both layouts. */
    if (f != NULL)
    {
        const unsigned long *raw = (const unsigned long *)f;
        unsigned long fw[32];

        for (i = 0; i < 32; i++)
            fw[i] = raw[i];
        xtensa_paniclog_frame(fw, 32, (int)info->exception,
                              info->pseudo_excause ? 1 : 0);
    }

    __asm__ __volatile__ ("rsr.ps %0" : "=r"(ps));
    regs[0] = ps;
    if ((ps & 0xfu) >= 4u)
    {
        regs[1] = xt_psr(1);
        regs[2] = xt_psr(2);
        regs[3] = xt_psr(3);
        regs[4] = xt_psr(4);
        /* DEPC holds the address that doubled, and on a double exception the ISA
         * has already shifted the original EPC1 down to EPC2 -- which is the
         * faulting instruction this handler's own frame cannot show. */
        __asm__ __volatile__ ("rsr.depc %0" : "=r"(regs[5]));
    }
    else
    {
        for (i = 1; i < 6; i++)
            regs[i] = 0;
    }

    /* The cache-error latch, which is what "Cache error" as a reason means: IDF
     * overrides the reason in panic_soc_fill_info() and the refused address and
     * size live here instead. There is no esp_cache_err_has_active_err() on
     * esp32s3 -- riscv and c-series only -- so the guard is the returned info:
     * esp_cache_err_get_panic_info() leaves err_str NULL when neither the
     * illegal-access status nor the ACS latch reports anything (esp32s3/
     * cache_err_int.c:131-146). err_str is not stored: it points into ROM, and
     * the string is meaningless after the reset. */
    {
        esp_cache_err_info_t ci;

        ci.err_str = NULL;
        ci.vaddr = 0;
        ci.size = 0;
        esp_cache_err_get_panic_info(&ci);
        regs[6] = ci.err_str != NULL ? (unsigned long)ci.vaddr : 0UL;
        regs[7] = ci.err_str != NULL ? (unsigned long)ci.size : 0UL;
    }
    xtensa_paniclog_frame_regs(regs, RB_REGS);

    /* Short form in the text record, bounded by its 96-character field, so the
     * fault is identifiable from the debug page as well as the serial report. */
    snprintf(line, sizeof line, "%s %s pc=%08lx excv=%08lx cause=%lu",
             RB_PANIC_TAG, reason, pc, excv, cause);
    xtensa_panic_log(line);

    /* Two seconds, feeding the RTC watchdog IDF armed for ten. */
    while (waited < RB_PANIC_HOLD_MS)
    {
        if (button_hw_any_pressed())
            break;
        esp_rom_delay_us(RB_PANIC_STEP_MS * 1000);
        waited += RB_PANIC_STEP_MS;
        if ((waited % 1000) == 0)
            esp_panic_handler_feed_wdts();
    }

    panic_restart();
}
