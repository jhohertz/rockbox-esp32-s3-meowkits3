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

/* Half of PSRAM. On every other Rockbox target the audiobuffer is whatever is
 * left of one contiguous memory region after the firmware; here IDF owns that
 * region's other half as its general heap, so the split has to be stated.
 * Making this larger takes memory straight out of IDF's PSRAM heap. */
#define XT_AUDIOBUF_SIZE    (MEMORYSIZE * 0x80000)

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
#define XT_CODECBUF_ADDR    (XT_DROMORIG + (MEMORYSIZE * 0x100000) \
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

/* Text halves carved out of the gap below codecbuf. Measured across all 43
 * codecs the largest .text+.literal+.rodata is 182 KB and the largest
 * .data+.bss is 362 KB, so 512 KB each leaves room to grow. These are the
 * numbers apps/plugins/plugin.lds links against -- it includes this header
 * rather than repeating them. */
#define XT_CODEC_TEXT_SIZE   0x80000
#define XT_PLUGIN_TEXT_SIZE  0x80000

/* Data-window addresses of those halves, i.e. the addresses they are *written*
 * through. Execution uses the linked address, which is this plus XT_IROM_OFF. */
#define XT_CODEC_TEXT_ADDR   (XT_CODECBUF_ADDR - XT_CODEC_TEXT_SIZE)
#define XT_PLUGIN_TEXT_ADDR  (XT_CODEC_TEXT_ADDR - XT_PLUGIN_TEXT_SIZE)

/* Rockbox's thread 0 stack, which is the FreeRTOS task stack the whole of
 * Rockbox runs on. The bare-metal script used the same 0x8000. */
#define XT_STACK_SIZE       0x8000

#endif /* MEMORY_XTENSA_H */
