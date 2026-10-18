# ESP32-S3 memory map (MeowKit / esp32-s3-idf)

Everything here is measured from `rockbox.elf` and the generated linker script,
not from the datasheet. Where a number is a constant we chose, it says so and
names the header that owns it.

Re-derive any of it with the commands in **Verifying** at the bottom; several of
the bugs below were only visible because the artifact disagreed with a comment.

---

## 1. The physical picture

512 KB of internal SRAM, 8 MB of octal PSRAM, 16 MB of SPI flash. The internal
SRAM is the scarce resource and almost nothing in it is optional.

| Bus view | Range | Physical | Notes |
|---|---|---|---|
| `0x3FC88000`–`0x3FCF0000` | D/IRAM, **data** bus | shared | `.data`, `.bss` |
| `0x40378000`–`0x403D0000` | D/IRAM, **instruction** bus | **same bytes** | `.iram` |
| `0x40370000`–`0x40378000` | IRAM only | internal | vectors, early startup |
| `0x3C000000`–`0x3E000000` | Ext_RAM **data** window | flash *or* PSRAM, per page | 32 MB, bank-switched |
| `0x40000000`–`0x42000000` | Ext_RAM **instruction** window | flash *or* PSRAM, per page | aliases the above |
| `0x50000000` | RTC slow, 8 KB | internal | survives deep sleep |
| `0x600FE000` | RTC fast, 8 KB | internal | |

**The two facts that cause most of the pain:**

1. **The instruction and data buses see the same internal SRAM at addresses
   differing by a constant `0x6F0000`.** `0x40378000` *is* `0x3FC88000`. Putting
   `.iram` at one and `.data` at the other looks like two disjoint regions and is
   in fact one. This is why the generated script contains `.dram0.dummy` (58,624
   bytes) -- it advances the data-bus location counter past the bytes the
   instruction region already claimed.

2. **Flash rodata and PSRAM share one 32 MB window and one 512-entry MMU.** You
   cannot map all 8 MB of PSRAM and all of flash rodata at the same time. IDF
   papers over this with `.flash_rodata_dummy` (576 KiB) and `.ext_ram.dummy`
   (1.125 MiB), both reported at `0x3c000020`. They are *IDF's* sections, not
   ours, and the apparent overlap is two views of one reservation.

### The MMU

512 entries of 32 bits at `0x600C5000`, stride 4, one per 64 KiB of *masked*
virtual address:

```
entry  = (vaddr & 0x1FFFFFF) >> 16        NOT (vaddr - 0x3C000000) >> 16
bit 15 = target   (0 = flash, 1 = PSRAM)
bit 14 = invalid
bits 13:0 = physical page
```

On this 8 MB board the PSRAM pages are 0..127, so **entries 0..127 are the whole
available PSRAM mapping and there is nothing beyond them to map into.** A page
fault through an invalid entry returns the Ext_RAM sentinel `0x0bad0bad` on a
read; a *write* has no defined path.

---

## 2. Region budgets and current usage

Measured from the linked image.

| Region | Budget | Used | Free | Owner of the budget |
|---|---|---|---|---|
| `dram0_0_seg` (internal data) | 341,760 (`0x53700`) | **340,740** | **1,020** | IDF `memory.ld` |
| `iram0_0_seg` (internal code) | — | ~78,000 | — | IDF |
| `.flash.text` (IROM window) | — | 549,398 | — | flash |
| PSRAM window | 8,388,608 | ~7,338,000 | **~1,050,000** | see §3 |

**Internal SRAM is the constraint, and it is now down to 1,020 bytes.** That is
the direct cost of `CONFIG_ESP_PANIC_HANDLER_IRAM`, which needs ~5,464 bytes of
DRAM and only fit after `file.o` was moved to PSRAM. Before that change the
figure was 4,376 free; the handler alone would not have fitted.

Note the asymmetry: **~1 MiB of PSRAM is idle** while internal SRAM is 99.7%
full. Almost everything in DRAM is there because `rockbox_memory.lf` defaults to
`* (rb_dram)`, not because it has to be.

### DRAM is now close enough to the edge to need a guard

A 1 KiB margin means the next internal-SRAM static, or any IDF bump that grows
`.dram0.data`, fails the link with `region 'dram0_0_seg' overflowed by N bytes`.
That is a build-time failure, so it is not silent -- but it is discovered by
whoever happens to build, with no hint of which object tipped it. The inventory
commands at the bottom answer that in one step; run them before adding anything
to DRAM rather than after.

---

## 3. The external arena, low to high

Constants live in `firmware/target/xtensa/memory-xtensa.h`. `apps/plugins/plugin.lds`
derives `CODEC_ORIGIN`/`PLUGIN_ORIGIN` from the same header -- it used to restate
them, which silently desynchronised the two and made `apps/codecs.c` reject every
codec.

```
0x3C000020  +--------------------------+  entry 0
            | IDF .flash_rodata_dummy  |  576 KiB   (IDF's reservation)
            | IDF .ext_ram.dummy       |  1.125 MiB (same span, other view)
0x3C090020  | .flash.appdesc           |  256 B
0x3C090120  | .flash.rodata            |  550,640 B
0x3C120000  | .ext_ram.bss             |  4,560,124 B  (audiobuffer, framebuffer,
            |                          |                codecbuf contents, buflib arena)
0x3C5794FC  +--------------------------+  entry 87
            |                          |
            |     1,051,648 B FREE     |  <- nothing maps here today
            |                          |
0x3C680000  | codecbuf                 |  1 MiB    entry 104
0x3C780000  | pluginbuf                |  512 KiB  entry 120
0x3C800000  +--------------------------+  == end of the 8 MB window, entry 128
```

`codecbuf` and `pluginbuf` are **pinned absolute symbols**, not section contents.
`apps/codecs.c` and `apps/plugin.c` reject any image whose declared `load_addr`
is not exactly `&codecbuf` / `&pluginbuf`, and the codec/plugin images are linked
separately against the same numbers. There is no Rockbox linker script on this
target, so the constants are restated in the header and the two sides are held
together by that single definition plus a boot-time check.

### The bug this file was written to prevent

`XT_DROMORIG` is `0x3C000020` because flash-mapped segments must satisfy the
cache's page-congruence rule, `paddr % 64KB == vaddr % 64KB`. `codecbuf` and
`pluginbuf` are derived from it and **inherited the `+0x20`**, which put the top
of the arena at `0x3C800020` -- 32 bytes past the last mappable entry. The final
32 bytes of the plugin buffer were served by MMU entry 128, reported `INVALID`.

Nothing normally writes there. `fill_random_playlist_indexes()` in
`apps/tagtree.c` does, because it initialises the whole buffer:

```c
for (i = 0; i < arr_sz; i++)
    bool_array[i] = i < max_slots;
```

reached by "all tracks" from the database. Fixed by subtracting the offset in
`XT_CODECBUF_ADDR`; the arena now ends flush with `0x3C000000 + MEMORYSIZE MB`.

**A boot check in `system-xtensa.c` now reports any future overrun**, alongside
the pre-existing one that compares `rb_ext_ram_end` against `codecbuf`. The old
check only guarded the *bottom* of the arena; nothing guarded the top, which is
why this sat latent.

---

## 4. What is in internal SRAM, and whether it has to be

Largest DRAM objects in the linked image:

| Bytes | Symbol | In DRAM because | Could move? |
|---|---|---|---|
| 2,108 | `open_streams` (`file.o`) | **moved to PSRAM** | Freed deliberately: it is the donor that let `CONFIG_ESP_PANIC_HANDLER_IRAM` fit. File-descriptor table, metadata only |
| 32,768 | `cache_buffer` | default | **No** -- `sd-idf.c` passes it straight to `sdmmc_read_sectors()`, so it is a DMA destination |
| 20,480 | `tagcache_stack` | default | Thread stack; risky, PSRAM faults with cache off |
| 13,312 | `lcd_scroll` | default | Not investigated -- see the `scroll_stack` note below before assuming this is easy |
| 12,288 | `scroll_stack$0` | default | **No** -- despite the name this is a *thread stack* (`static long scroll_stack[DEFAULT_STACK_SIZE*3]` in `scroll_engine.c`). Context switches happen in assembly and a PSRAM stack would be fatal with the cache disabled |
| 12,288 | `codec_stack` | default | Thread stack |
| 12,288 | `buffering_stack` | default | Thread stack |
| 8,192 | `dma_buf` | required | **No** -- TX DMA staging |
| 8,192 | `storage_thread_stack` | default | Thread stack |
| 8,192 | `audio_stack` | default | Thread stack |
| 6,144 | `playlist_stack` | default | Thread stack |
| 5,440 | `tcb_pool` | required | **No** -- FreeRTOS TCBs, touched by the scheduler |
| 5,120 | `dircache_stack` | default | Thread stack |
| 5,056 | `voice_stack` | default | Thread stack |
| 4,096 | `rx_thread_stack` | required | **No** -- blocks in `i2s_channel_read()` |
| 4,096 | `tick_thread_stack` | required | **No** -- the kernel tick |
| 4,096 | `downmix_buf` | default | Mixer hot path |
| 4,096 | `backlight_stack` | default | Thread stack |
| 3,072 | `port_IntStack` | required | **No** -- interrupt stack |

The rule that decides it: **anything a DMA engine touches, anything the
scheduler or an ISR touches, and anything that must work while the flash cache
is disabled stays in internal SRAM.** Everything else is here by default.

`rockbox_memory.lf` grants objects individually and falls through to
`* (rb_dram)`. The scroll buffers looked like the obvious donors on size alone
and are not: `scroll_stack` is a thread stack. **Check what a symbol actually is,
not what it is called** -- `nm --print-size` on the object, then read the
definition. The safe donors are metadata tables, which is why `dir`, `fileobj_mgr`
and now `file` are the ones that have moved.

---

## 5. Known gaps and opportunities

1. **1 MiB of PSRAM is unused** between `.ext_ram.bss` and `codecbuf`. Anything
   that needs internal SRAM relief can be rebalanced against this rather than
   shrinking a real buffer.

2. **The arena top is only guarded by a printf.** A layout overrun reports
   itself at boot but does not stop the build. It should arguably be a link-time
   `ASSERT()` in the same place IDF asserts `_flash_rodata_dummy_start`.

3. **`MEMORYSIZE` drives three unrelated things at once** -- the audiobuffer
   size, the external window span, and the codec/plugin origins. Changing it is
   a three-way coupling with no compile-time check that all three stayed
   consistent.

4. **`DEFAULT_STACK_SIZE` is 0x1000 on this arch** (vs 0x400 on m68k/mips)
   because ISRs run on the interrupted thread's stack. With ~10 thread stacks in
   DRAM that is the single largest discretionary consumer, and nothing measures
   actual high-water mark. A stack-usage probe would let these be sized from
   data instead of guesswork.

5. **DRAM free space is 1,020 bytes.** It was 4,376 before
   `CONFIG_ESP_PANIC_HANDLER_IRAM` (cost ~5,464) and `file` -> PSRAM (gave back
   2,108). The handler is what makes exit-path and cache-disabled faults
   reportable at all -- without it the panic handler itself cannot execute and
   the device reboots with no register dump -- so it is worth the space. But the
   margin is now one small static deep.

---

## Verifying

```sh
cd build-meowkits3
BIN=$(ls -d ~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)

# segments the loader places, and where
$BIN/xtensa-esp32s3-elf-readelf -l rockbox.elf | grep LOAD

# section addresses and sizes
$BIN/xtensa-esp32s3-elf-readelf -S -W rockbox.elf

# the pinned arena symbols
$BIN/xtensa-esp32s3-elf-nm rockbox.elf | grep -E 'codecbuf|pluginbuf|ext_ram_end'

# largest internal-SRAM objects
$BIN/xtensa-esp32s3-elf-nm --print-size --size-sort rockbox.elf \
  | awk '{a=strtonum("0x"$1); s=strtonum("0x"$2);
          if (a>=0x3FC88000 && a<0x3FD00000 && ($3=="b"||$3=="B")) print s,$4}' | sort -n | tail -25

# every codec must link at the firmware's codecbuf or apps/codecs.c rejects it
FW=$($BIN/xtensa-esp32s3-elf-nm rockbox.elf | awk '$3=="codecbuf"{print $1}')
for c in $(find lib/rbcodec/codecs -name '*.codec'); do
  o=$($BIN/xtensa-esp32s3-elf-readelf -l "$c" | awk '/LOAD/{print $3; exit}')
  [ "$o" != "0x$FW" ] && echo "MISMATCH $c $o"
done

# the live MMU map is printed at boot by sd-idf.c (TEMPORARY probe)
```
