/*
 * The buffers Rockbox's core expects its linker script to place.
 *
 * Rockbox's kernel, allocator, codec loader and LCD drivers reference these as
 * *symbols* -- bare addresses, not variables -- because on every native target
 * a linker script hands them out:
 *
 *     audiobuffer, audiobufend    core_alloc.c carves the audio buffer from here
 *     codecbuf, pluginbuf         apps/codecs.c and apps/plugins reject a loaded
 *                                 image whose load address is not codecbuf
 *     loadaddress                 rolo.c
 *     stackbegin, stackend        kernel/thread.c: thread 0's stack bounds
 *
 * On this path there is no Rockbox linker script -- ESP-IDF owns it -- so they
 * are defined here instead.
 *
 * They have to be defined here rather than in C with the end symbols computed
 * alongside: `.set audiobufend, audiobuffer + N' in one object referring to an
 * array defined in another emits nothing at all. GNU as will only build such a
 * symbol from a base it can see, so the base and the derived address live in
 * the same file. (The failure is quiet: the object assembles, and the link
 * reports audiobufend as an undefined reference with nothing pointing at the
 * cause.)
 *
 * Placement is a real decision, not a formality:
 *
 *   - .ext_ram.bss is the section IDF's linker script places in PSRAM, and
 *     IDF's startup zeroes it -- but only when
 *     CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set in the IDF project.
 *     Without it the section is not claimed and these land in internal DRAM,
 *     where the 337 KB region overflows loudly at link time. That is the right
 *     way for that mistake to fail, and it is why the config option is called
 *     out rather than assumed.
 *   - The stack is deliberately *not* in PSRAM. FreeRTOS switches to it in
 *     assembly, and a stack in cache-backed memory presents as corruption
 *     rather than as a failure.
 *
 * The framebuffer is not here. lcd-color-common.c already defines one, and the
 * display driver reads that one through lcd_framebuffer_default; a second
 * buffer defined here is how a black screen with no error anywhere happens.
 */

#include "config.h"
#include "memory-xtensa.h"

/*
 * PSRAM. IDF's sections.ld matches *(.ext_ram.bss*) and zeroes the region when
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is on.
 */
    .section .ext_ram.bss, "aw", @nobits
    .align 4

    /* core_alloc.c: the audio buffer, audiobuffer..audiobufend. */
    .globl audiobuffer
audiobuffer:
    .space XT_AUDIOBUF_SIZE
    .globl audiobufend
audiobufend:

    /* End of everything this file puts in PSRAM, and of the rest of
     * .ext_ram.bss that IDF places here. Checked against codecbuf at boot: the
     * codec and plugin buffers are pinned to a fixed address to agree with
     * plugin.lds, so nothing can be left to grow into them unnoticed. */
    .globl rb_ext_ram_end
rb_ext_ram_end:

    /*
     * Codec and plugin load buffers.
     *
     * Pinned, not placed. See XT_CODECBUF_ADDR in memory-xtensa.h for the
     * derivation and for why this side and apps/plugins/plugin.lds have to
     * produce the same number: apps/codecs.c and apps/plugin.c reject a loaded
     * image whose declared load address is not these symbols, and the images
     * are linked against plugin.lds's CODEC_ORIGIN / PLUGIN_ORIGIN.
     *
     * They are absolute symbols rather than .space in .ext_ram.bss, which is
     * what the bare-metal script effectively did. The cost of that is that no
     * section reserves the range, so the overlap check against rb_ext_ram_end
     * at boot is what stands between a correct layout and silent corruption of
     * the codec image. The gain is that the address no longer depends on how
     * large IDF decides to make everything before it.
     */
    .globl codecbuf
    .set codecbuf, XT_CODECBUF_ADDR
    .globl pluginbuf
    .set pluginbuf, XT_PLUGINBUF_ADDR
    .globl loadaddress
    .set loadaddress, XT_LOADADDRESS_ADDR

/*
 * Thread 0's stack, and therefore the stack every Rockbox thread runs on: the
 * cooperative scheduler switches contexts inside one FreeRTOS task, and that
 * task is created from this buffer (see idf-app/main/rockbox_app.c). Plain
 * .bss, which is internal DRAM under IDF's script. 16-byte aligned for the
 * Xtensa ABI.
 */
    .section .bss, "aw", @nobits
    .align 4

    .globl rockbox_stack
    .align 16
rockbox_stack:
    /* FreeRTOS's stack-overflow check (configCHECK_FOR_STACK_OVERFLOW == 2,
     * which is how IDF configures it) reads the last 16 bytes of the buffer it
     * is handed and expects them to still hold its 0xa5 fill. Rockbox's
     * scheduler reads the first word of *its* thread-0 stack and expects
     * DEADBEEF there. Both regions are the bottom of the same allocation, so
     * one of them has to move: the buffer starts with a strip reserved for
     * FreeRTOS, and stackbegin -- what Rockbox's kernel sees -- starts above
     * it. Without this the two checks trip each other, and the symptom is
     * FreeRTOS reporting a stack overflow in a task that has used almost
     * nothing. */
    .space 32

    .globl stackbegin
stackbegin:
    .space XT_STACK_SIZE - 32

    .globl stackend
stackend:
