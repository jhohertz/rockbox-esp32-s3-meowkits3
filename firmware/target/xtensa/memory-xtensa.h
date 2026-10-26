#ifndef MEMORY_XTENSA_H
#define MEMORY_XTENSA_H

/*
 * Sizes for the buffers Rockbox expects its linker script to place.
 *
 * Only memory-xtensa-syms.S includes this today, but the sizes are also what
 * any C side has to agree with: rockbox_app.c in the IDF project creates the
 * FreeRTOS task on the buffer named stackbegin here, and its size is stated
 * separately because that file cannot include Rockbox headers.
 *
 * Everything here depends on config.h having been included first.
 */

/* Half of PSRAM, minus a reservation. On every other Rockbox target the
 * audiobuffer is whatever is left of one contiguous memory region after the
 * firmware; here IDF owns that region's other half as its general heap, so the
 * split has to be stated. Making this larger takes memory straight out of IDF's
 * PSRAM heap.
 *
 * The deduction is not slack for its own sake -- it is the clearance the
 * section needs above itself. XT_AUDIOBUF_SIZE is only what memory-xtensa-syms.S
 * puts in .ext_ram.bss; IDF's own PSRAM objects and every object the ld fragment
 * maps to extern_ram (unicode, language, the skin parser, dir/file/fileobj_mgr,
 * system.o's boost log and, since the feature flags, gui/line.o's layout state) are
 * placed in the same section, so the section ends well past audiobufend. Above it
 * sit the codec and plugin text reservations, and .ext_ram.bss is memset to zero at
 * boot by esp_psram_bss_init() -- so a section that reaches into plugin_text
 * silently zeroes the head of the region every plugin is loaded into. That is
 * exactly what happened: moving system.o's 16,664 bytes of .bss to PSRAM put the
 * section end at 0x3c58290c, 10,508 bytes past XT_PLUGIN_TEXT_ADDR, and every
 * plugin crashed on arrival while codecs (at XT_CODEC_TEXT_ADDR, further up) kept
 * working. The guard in system_init() now watches that boundary; keep this
 * deduction large enough that it never fires.
 *
 * It has to be a whole page, not merely "a bit more", and that is the part worth
 * remembering. .ext_ram.bss does not start at a fixed address: IDF's .ext_ram.dummy
 * advances the location counter past the flash rodata and then aligns to the 64 KB
 * MMU page, so the section's start moves in 64 KB steps whenever code or rodata
 * growth crosses a boundary. Enabling HAVE_ALBUMART, HAVE_JPEG, HAVE_QUICKSCREEN
 * and AB_REPEAT_ENABLE grew the image by ~24 KB, which pushed _ext_ram_bss_start
 * from 0x3c120000 to 0x3c130000 -- a full page for a 24 KB change -- and a 64 KB
 * deduction was therefore consumed entirely, putting the end at 0x3c582e8c and
 * reproducing the original crash figure almost exactly. 128 KB covers one page of
 * growth plus the donated objects. If the image grows another page this moves
 * again, so read the MEMORY: line printed at boot rather than trusting this
 * number. The deduction is now 128 KB below half of PSRAM plus the 256 KB the
 * codec text reservation gave up (see XT_CODEC_TEXT_SIZE): the arena is sized by
 * this constant alone -- core_alloc.c does buflib_init(start, audiobufend - start)
 * and audiobufend is the label right after the .space below -- so shrinking the
 * text reservations without raising this buys nothing but unused slack. Growing it
 * takes the space straight out of IDF's PSRAM heap, and the boot guard keeps watch
 * on the boundary either way. */
#define XT_AUDIOBUF_SIZE    ((MEMORYSIZE * 0x80000) + 0x20000)

/*
 * Codec and plugin load buffers, pinned to the top of the PSRAM part of the
 * external data-cache window.
 *
 * These are not free choices. apps/codecs.c and apps/plugin.c both reject a
 * loaded image whose declared load address differs from &codecbuf /
 * &pluginbuf, and the codec and plugin images are linked against the fixed
 * CODEC_ORIGIN / PLUGIN_ORIGIN that apps/plugins/plugin.lds computes from
 * MEMORYSIZE, CODEC_SIZE and PLUGIN_BUFFER_SIZE. The two sides have to agree,
 * and on every other native target they do because the same linker script hands
 * out both -- firmware/rom.lds and the arm imx233 / s5l8700 / as3525 / s3c2440
 * / tcc780x scripts all assign codecbuf from the region layout. This target has
 * no Rockbox linker script, so the constants are restated here, identically:
 *
 *   plugin.lds:  DRAMORIG 0x3C000020
 *                DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
 *                CODEC_ORIGIN   (DRAMORIG + DRAMSIZE)
 *                PLUGIN_ORIGIN  (CODEC_ORIGIN + CODEC_SIZE)
 *
 * Which puts codecbuf and pluginbuf in the top 1.5 MB of the window and leaves
 * the bottom 6.5 MB for the audiobuffer and the rest of .ext_ram.bss.
 * memory-xtensa-syms.S exports rb_ext_ram_end so boot can check that the two
 * regions do not overlap, rather than letting a collision corrupt silently.
 */
#define XT_DROMORIG         0x3C000020
/* The window is MEMORYSIZE MB from 0x3C000000 and the flash MMU has exactly that
 * many 64 KB entries pointing at PSRAM, so the arena must END at
 * 0x3C000000 + MEMORYSIZE MB -- there is no entry beyond it to map, because all
 * MEMORYSIZE*16 physical pages are already claimed. XT_DROMORIG carries +0x20 so
 * flash-mapped segments satisfy the cache's page-congruence rule, and codecbuf /
 * pluginbuf are derived from it, so without subtracting that offset here the top
 * of the arena lands 32 bytes past the last mapped entry -- the defect the boot
 * check in system-xtensa.c reports. Subtracting it also page-aligns both buffers,
 * which they wanted anyway. */
#define XT_CODECBUF_ADDR    (XT_DROMORIG + (MEMORYSIZE * 0x100000) - 0x20 \
                             - PLUGIN_BUFFER_SIZE - CODEC_SIZE)
#define XT_PLUGINBUF_ADDR   (XT_CODECBUF_ADDR + CODEC_SIZE)
#define XT_LOADADDRESS_ADDR (XT_PLUGINBUF_ADDR + PLUGIN_BUFFER_SIZE)

/* The two external-RAM cache windows alias the same physical pages -- they
 * index the same MMU entries -- and differ only in what the hardware serves:
 * the instruction window takes fetches and loads but faults on a store, the
 * data window takes loads and stores but faults on a fetch. So a loaded image
 * is linked read-only-half-up and writable-half-down, and the offset between
 * the two views of one page is constant. */
#define XT_IROM_BASE        0x40000000
#define XT_IROM_OFF         0x06000000

/* Text halves carved out of the gap below codecbuf. Measured across all codecs
 * the largest .text+.literal+.rodata is 182 KB (aac.codec is 200,232 bytes as a
 * whole image), so the codec half is 256 KB: still ~74 KB of headroom, and
 * apps/plugins/plugin.lds links against THIS_TEXT_LENGTH, so a codec that outgrows
 * it fails at link time rather than at load. The plugin half stays at 512 KB
 * because the largest .rock is 482,560 bytes and shrinking it would put the head of
 * the plugin text region at risk for no gain -- what this shrink actually funds is
 * XT_AUDIOBUF_SIZE. These are the numbers plugin.lds includes rather than repeats. */
#define XT_CODEC_TEXT_SIZE   0x40000
#define XT_PLUGIN_TEXT_SIZE  0x80000

/* Data-window addresses of those halves, i.e. the addresses they are *written*
 * through. Execution uses the linked address, which is this plus XT_IROM_OFF. */
#define XT_CODEC_TEXT_ADDR   (XT_CODECBUF_ADDR - XT_CODEC_TEXT_SIZE)
#define XT_PLUGIN_TEXT_ADDR  (XT_CODEC_TEXT_ADDR - XT_PLUGIN_TEXT_SIZE)

/* Rockbox's thread 0 stack, which is the FreeRTOS task stack the whole of
 * Rockbox runs on. The bare-metal script used the same 0x8000. */
#define XT_STACK_SIZE       0x8000

#endif /* MEMORY_XTENSA_H */
