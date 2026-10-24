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
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_app_desc.h"
#include "panic_funcs.h"
#include "esp_rom_sys.h"

/* Rockbox, declared here rather than included: this file is compiled by IDF, which
 * does not carry Rockbox's include paths. Same seam rockbox_app.c uses. */
extern void xtensa_panic_log(const char *msg);
extern void xtensa_paniclog_frame(const unsigned long *words, int count,
                                  int exception, int pseudo);
extern void xtensa_paniclog_frame_regs(const unsigned long *regs, int count);
extern void xtensa_paniclog_frame_window(uint32_t windowbase,
                                         uint32_t windowstart, uint32_t a5);
extern void xtensa_paniclog_backtrace(const unsigned long *pcs,
                                      const unsigned long *sps, int n,
                                      const char *build);
extern void xtensa_paniclog_api_handover(const unsigned long *words, int n);
extern void xtensa_paniclog_pcm_slots(const unsigned long *words, int n);
extern uint32_t xtensa_pcm_slots[4];
extern int button_hw_any_pressed(void);
extern void esp_panic_handler_feed_wdts(void);

/* Distinctive enough to grep for in the linked image, so a silent loss to the
 * archive copy of the symbol is visible before the board is flashed. */
#define RB_PANIC_TAG "*rb-panic*"

#define RB_PANIC_HOLD_MS 2000
#define RB_PANIC_STEP_MS 100

/* Order of the register array handed to xtensa_paniclog_frame_regs():
 * ps, epc1, epc2, epc3, epc4, depc, cache_vaddr, cache_size, windowbase,
 * windowstart, a5. */
#define RB_REGS 11

/* Frames of caller chain to retain. IDF walks until the chain goes insensible;
 * eight is what fits in the report and enough to name the caller whose return
 * address was hijacked. */
/* Frames of caller chain to retain. The exception handler runs on the faulting
 * thread's own stack, so its frames occupy the first several slots; anything
 * shorter never reaches the thread that actually faulted. */
#define RB_BT_FRAMES 16

/* The loaded image's API handover, read at fault time.
 *
 * Three samples of the firmware's ci -- at codec load, after the image's
 * CODEC_LOAD entry returns, and immediately before run_proc() -- all showed the
 * encoder slots as real code. The codec then called offset 136 and executed a
 * stack address, so the change happens inside its own run. Reading the handover
 * here, at the moment of the fault, separates the two remaining cases: the
 * struct was written through, or the image's pointer slot was.
 *
 * The header layout is fixed by lib/rbcodec/codecs/codecs.h: struct lc_header is
 * five words (magic, target_id, version, start_addr, end_addr), then entry_point,
 * run_proc, then api -- so the api field is at byte 28 of the image, which is
 * codecbuf. Its value is the address of the image's own `codec` pointer, and the
 * word there is the firmware's ci the image dereferences.
 *
 * PSRAM is mapped when esp_panic_handler runs (IDF re-enables the flash cache
 * before calling it), so these loads are safe. The pointer is still range-checked
 * before it is dereferenced: a corrupted slot must not turn this read into a
 * second fault, and at debug exception level there is no handler left to catch
 * one.
 */
/* Where the image keeps its codec pointer, as recorded by codec_load_ram().
 *
 * Hard-coding the header offset here was a mistake: struct lc_header is four
 * words in this build, so the header's `api` field is at byte 24 of the image,
 * and reading codecbuf+28 reported 0x000000a8 -- the tail of api_size, not a
 * pointer. The boot side has the typed struct and knows the address, so it hands
 * it over and this reads what it is told. .noinit survives the reset, and it is
 * written on every codec load, so it names whichever image was loaded last. */
extern uint32_t xtensa_codec_api_slot;

static int img_word_is_readable(uint32_t v)
{
    return esp_ptr_internal((const void *)v) || esp_ptr_external_ram((const void *)v);
}

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

    /* The window state, which is not in the frame. The fault's PC equals the
     * interrupted SP, which is what a window spill or underflow produces when it
     * walks a register-window chain that does not exist: the handler reads a stack
     * address as an ancestor's base-save area, stores a0 there, and the eventual
     * retw executes it. windowstart says how many windows the core thinks are live,
     * windowbase which one is current, and a5 is the frame pointer the spill and
     * underflow handlers walk -- if a5 is not a sane stack address, the chain is
     * broken and the handler's stores go wherever a5 points.
     *
     * Caveat, stated so the numbers are read correctly: the exception vectors save
     * the context without rotating the window, but the C handler is entered with
     * call8, so the windowbase read here is the faulted context's plus the rotations
     * since the fault. windowstart however is a bitmask of live windows and is only
     * changed by entry/spill, so it is comparable as read. */
    {
        uint32_t wb = 0, ws = 0;

        __asm__ volatile ("rsr.windowbase %0" : "=r"(wb));
        __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
        xtensa_paniclog_frame_window(wb, ws,
                                    f != NULL ? (uint32_t)f->a5 : 0);
    }

    /* The caller chain, stepped with IDF's esp_backtrace_get_next_frame: it
     * reads frame(i-1)'s base-save area from the four words below SP (return PC
     * at sp-16, caller SP at sp-12), which the exception vectors fill for every
     * frame because XT_DEBUG_BACKTRACE is unconditional in xtensa_vectors.S.
     *
     * Two deliberate departures from how IDF uses it.
     *
     * Its return value is ignored. It reports false when the frame it just read
     * has an insensible SP or a non-executable PC, which is right for a
     * human-readable trace and wrong here: the first such frame is the
     * interesting one, and stopping at it hides it. Termination is therefore only
     * about safety -- each SP is validated with esp_stack_ptr_is_sane before the
     * step that reads below it, because a read through a bad SP faults and a fault
     * inside this handler is a third exception with no handler left, which would
     * take the whole record with it.
     *
     * PCs are stored unprocessed. esp_cpu_process_stack_pc folds the windowed
     * ABI's window-increment bits and steps off the return address, which turns a
     * slot holding a literal-pool address into something that looks like code.
     * The raw value is what lets the host tell a genuine return address from a
     * data slot -- the first version of this walk printed processed PCs and
     * showed 0x42600043, which resolved inside a codec's literal pool and looked
     * like a frame until the raw slot (0x42600046) said otherwise.
     *
     * Each frame is committed before the next is attempted, and the frame and
     * register slots are stored first, so a walk that dies part-way keeps what it
     * already had.
     */
    {
        unsigned long pcs[RB_BT_FRAMES];
        unsigned long sps[RB_BT_FRAMES];
        esp_backtrace_frame_t bf;
        unsigned long prev_sp = 0;
        int n = 0;

        if (f != NULL)
        {
            bf.pc       = (uint32_t)f->pc;
            bf.sp       = (uint32_t)f->a1;
            bf.next_pc  = (uint32_t)f->a0;
            bf.exc_frame = f;

            pcs[n] = bf.pc;
            sps[n] = bf.sp;
            n++;

            while (n < RB_BT_FRAMES && bf.next_pc != 0)
            {
                /* A repeated SP means the base-save area points back at the same
                   frame; without this the walk spins to the cap. */
                if (!esp_stack_ptr_is_sane(bf.sp) || bf.sp == prev_sp)
                    break;
                prev_sp = bf.sp;

                esp_backtrace_get_next_frame(&bf);

                pcs[n] = bf.pc;
                sps[n] = bf.sp;
                n++;
            }
        }

        /* The build id of the image that faulted, not of the one that will print
         * this: a flash cannot confirm itself because the post-flash reset pulse
         * never reaches a chip whose pads the old image already gave to the DWC
         * core. */
        if (n > 0)
            xtensa_paniclog_backtrace(pcs, sps, n,
                                      esp_app_get_description()->version);
    }

    /* What the loaded image held at the instant of the fault: the address of its
     * codec pointer, that pointer, and four words of the API struct at offsets
     * 136..148 -- enc_pcmbuf_read, enc_pcmbuf_advance, enc_encbuf_get_buffer,
     * enc_encbuf_finish_buffer. Those four are the slots the image calls. An
     * earlier version read offsets 120..133 as well (memcmp, memchr, logf, qsort),
     * which were always healthy and said nothing about the fault, and printing
     * eight words instead of four is what pushed the boot report past the CDC
     * ring. Both pointers are range-checked before dereference: at debug
     * exception level a fault here has no handler left, and losing the record
     * would be worse than not reading it. */
    {
        unsigned long handover[6] = { 0 };
        uint32_t slot_addr = xtensa_codec_api_slot;
        int i;

        handover[0] = slot_addr;
        if (slot_addr != 0 && img_word_is_readable(slot_addr))
        {
            uint32_t api = *(volatile uint32_t *)slot_addr;

            handover[1] = api;
            if (api != 0 && img_word_is_readable(api))
            {
                for (i = 0; i < 4; i++)
                    handover[2 + i] = *(volatile uint32_t *)(api + 136 + i * 4);
            }
        }
        xtensa_paniclog_api_handover(handover, 6);
    }

    /* The pcm_record statics, read live through the addresses the recorder
     * recorded. pcm_copyfn is the point of this: it is a function pointer in
     * .bss, and if it holds a stack address then the "fetch refused at a DRAM
     * address" fault is the copy routine being called, not a corrupted return.
     * Slots the recorder has not filled yet read as zero and print as absent. */
    {
        unsigned long pcmw[4] = { 0, 0, 0, 0 };
        int i;

        for (i = 0; i < 4; i++)
        {
            uint32_t addr = xtensa_pcm_slots[i];

            if (addr != 0 && esp_ptr_internal((const void *)addr))
                pcmw[i] = *(volatile uint32_t *)addr;
        }
        xtensa_paniclog_pcm_slots(pcmw, 4);
    }

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
