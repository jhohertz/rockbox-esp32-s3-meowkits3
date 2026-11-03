#ifndef PANICLOG_H
#define PANICLOG_H

#include "config.h"           /* HAVE_DBG_BREADCRUMB, so the guard below works
                               * regardless of the includer's order */
#include <sys/types.h>
#include <stdint.h>

/*
 * What firmware/target/xtensa/paniclog.c exports to the rest of the target.
 *
 * The one hook the core calls is declared where the core sees it:
 * panic_console_write() in panic.h under HAVE_PANIC_CONSOLE. Everything else
 * here is target-facing, including the crash trail writers -- no core file
 * calls them, so nothing about them belongs in a core header. Each caller
 * reaches them through the driver seams that hand over the values
 * (usb_dw_target_init_note() and usb_dw_target_flush_timeout() being the two
 * in the DWC2 driver), or calls them directly from target code.
 */

#ifdef HAVE_DBG_BREADCRUMB
/* Record a step that might be the last one. Needs no console, no lock, no
 * stack and no working panic handler, because its whole purpose is to survive
 * the hang or reset that follows it. The retained variant lives in memory that
 * outlives that reset; use it for steps that can stall. */
void dbg_breadcrumb(const char *tag, unsigned long a, unsigned long b);
void dbg_breadcrumb_retained(const char *tag, unsigned long a, unsigned long b);
#endif

/* Dump the retained record to whatever console exists. Called before the first
 * screen is drawn, so it must not allocate or take a lock. */
void paniclog_dump(void);

/* Read the retained panic record back from a context that has no console:
 * the screen. text() prefers the RTC-slow copy, which survives the reset
 * that clears internal DRAM, and returns 0 when nothing was retained.
 * reset_reason() is the reset that produced this boot; rtc_count() says
 * whether the surviving trail was ever written -- zero means the dead boot
 * never reached the steps that stamp it. */
int paniclog_text(char *buf, int bufsize);
unsigned long paniclog_reset_reason(void);
unsigned long paniclog_rtc_count(void);

/* Write the surviving crash record -- panic text, reset reason, the last few
 * trail entries -- over the USB CDC console, using logf(). Bounded on
 * purpose: four short messages, because the CDC ring is 512 bytes and a
 * burst with no reader open reboot-loops the board. Call from thread context
 * only. */
void paniclog_crash_report_logf(void);

/* Read the SRAM trail back one entry at a time, newest first, for a caller
 * that has no console: the screen. Hardware exceptions go to the platform
 * handler and reset without ever reaching Rockbox's panic(), so the trail is
 * what survives them. Returns 0 when there is nothing at that index. */
int dbg_breadcrumb_back(int back, char *buf, int bufsize);

/* Retain the raw exception frame across the reset. The frame is copied
 * verbatim because which layout it is in (XtExcFrame or XtDbFrame) is the
 * open question -- EPC1 holding the vector address means a second exception
 * overwrote it -- and decoding that at fault time would be guessing.
 * exception/pseudo come from the platform handler's panic_info_t and say
 * which layout to read the words with. */
void paniclog_frame(const unsigned long *words, int count,
                    int exception, int pseudo);
void paniclog_frame_info(unsigned long *exception, unsigned long *pseudo,
                         unsigned long *count);
/* Four retained frame words per line, line 0 first. 0 when nothing is
 * stored. */
int paniclog_frame_words(char *buf, int bufsize, int line);

/* Store ps, epc1..epc4, depc and the cache-error detail alongside the frame.
 * These are read from the registers, not the frame: on a double exception
 * the ISA shifts the original EPC1 down to EPC2, so the faulting instruction
 * is nowhere in the frame the handler sees. */
void paniclog_frame_regs(const unsigned long *regs, int count);
int paniclog_frame_regs_text(char *buf, int bufsize);

/* Window state at fault. The exception frame carries neither windowbase nor
 * windowstart, and a5 is the pointer the window handlers use as the ancestor
 * frame address, so it is the value that says whether the walk was sane. */
void paniclog_frame_window(uint32_t windowbase, uint32_t windowstart,
                           uint32_t a5);
int paniclog_frame_window_text(char *buf, int bufsize);

/* The address and width the cache refused; 0 when the latch reported
 * nothing. */
int paniclog_frame_cache(char *buf, int bufsize);

/* The register line, in pieces, for a screen that cannot hold all seven
 * fields. Truncating the single line loses epc2 -- the slot that carries the
 * real faulting address when the fault doubled, and therefore the only field
 * that names a function in the faulting image. Returns 0 when nothing was
 * stored, or when part is past the end. */
int paniclog_frame_regs_part(char *buf, int bufsize, int part);

/* The bytes resident at the faulting address, plus EXCCAUSE, captured at panic
 * time. An IllegalInstruction whose address holds a legal opcode in the image
 * is a failed fetch, and these two words are the difference between that and a
 * genuinely illegal instruction. Returns 0 when nothing was recorded. */
void paniclog_frame_fault(uint32_t addr, uint32_t w0, uint32_t w1,
                          uint32_t cause);
int paniclog_frame_fault_text(char *buf, int bufsize);

/* Which reset produced this boot, and the build the dead boot was running.
 * Both are recorded by the IDF-side entry point, which has the values and
 * cannot call into Rockbox yet. */
void paniclog_set_reset(unsigned reason);
void paniclog_crash_id(const char *build);
/* The build id the dead boot stamped; 0 when nothing was recorded. */
int paniclog_crash_build(char *buf, int bufsize);
/* The build this boot is running, printable only once it has been stamped. */
const char *paniclog_build_id_running(void);

/* Mark the boundary between the dead boot's trail and this boot's. seq is the
 * boot sequence number, so a reader can tell the two apart. */
void paniclog_stamp_boot(unsigned long seq);

#endif /* PANICLOG_H */
