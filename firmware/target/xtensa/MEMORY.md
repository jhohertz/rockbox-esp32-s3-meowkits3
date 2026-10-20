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
| `dram0_0_seg` (internal data) | 341,760 (`0x53700`) | **339,296** | **2,464** | IDF `memory.ld` |
| `iram0_0_seg` (internal code) | — | 76,819 | — | IDF |
| `.flash.text` (IROM window) | — | 572,438 | — | flash |
| PSRAM window | 8,388,608 | 8,361,972 | **26,636** | see §3 |

Measured from the linked image with the USB stack compiled in and the attach path
compiled out: `.dram0.data` 26,840 B at `0x3fc97100`, `.noinit` 2,332 B at
`0x3fc9d9d8`, `.dram0.bss` 248,416 B at `0x3fc9e300`, high water `0x3fcdad60`,
link region end `0x3fcdb700`. The device agrees: boot reports
`heap_init: At 3FCDAD60 len 0000E9B0 (58 KiB): RAM`, so the high water and the heap
start are the same number -- which is the check to run after changing anything in
DRAM, because it proves the linker script and IDF see the same edge. IDF continues
the heap past the link region (`At 3FCE9710 len 00005724 (21 KiB)`) to `0x3FCF0000`,
the end of the data-bus view; the link region ending at `0x3fcdb700` caps *statics*,
not the heap.

**Internal SRAM is the constraint and the margin is single-digit KB.** It is not
enlargeable: `DRAM0_0_SEG_LEN` is capped at `I_D_SRAM_SIZE`, which this build
already sits on, and the cache size configs feed IRAM rather than DRAM. Most of
what is left is thread stacks (a PSRAM stack faults) and DMA destinations (the
DWC and SDHOST masters cannot reach the PSRAM window).

**There is no spare PSRAM.** An earlier revision of this file claimed ~1 MiB of
PSRAM was idle, and decisions were made against that claim. It was wrong: it
added up `.ext_ram.bss`, `codecbuf` and `pluginbuf` and missed the two `_text`
reservations below `codecbuf`. The real idle figure at that point was 25,284 bytes;
the current build measures **26,636**, and §3 shows every byte of where the rest
went.

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
            | IDF .flash_rodata_dummy  |  sized SIZEOF(.flash.text)
            | IDF .ext_ram.dummy       |  1,179,616 B (same span, other view)
0x3C090120  | .flash.rodata            |  568,704 B
0x3C120000  | .ext_ram.bss             |  4,560,884 B  entry 18
0x3C15D2E0  |   audiobuffer            |    4,194,304 B (the buflib arena) entry 21
0x3C55D2E0  |   ...framebuffers etc.   |    ~368,000 B of PSRAM_BSS_ATTR objects
0x3C5797F4  +--------------------------+  end of .ext_ram.bss, entry 85
            |     26,636 B free        |  <- the only idle bytes in the window
0x3C580000  | plugin_text              |  524,288 B  entry 88
0x3C600000  | codec_text               |  524,288 B  entry 96
0x3C680000  | codecbuf                 |  1 MiB      entry 104
0x3C780000  | pluginbuf                |  512 KiB    entry 120
0x3C800000  +--------------------------+  == end of the 8 MB window, entry 128
```

Every byte is accounted for: 1.125 MB firmware mapping + 4.36 MB `.ext_ram.bss`
+ 1 MB `_text` reservations + 1.5 MB codec/plugin buffers + 26 KB idle = 8 MB.

**The 1.125 MB firmware reservation is not reclaimable.** It looks like the cost
of IDF's PSRAM-XIP options (`CONFIG_SPIRAM_RODATA`, `SPIRAM_FETCH_INSTRUCTIONS`,
`SPIRAM_XIP_FROM_PSRAM`), so those were turned off and the image relinked. The
layout came out **byte-identical**: `.ext_ram.dummy` is sized
`_rodata_reserved_end - _flash_rodata_dummy_start` and `flash_rodata_dummy` is
sized `SIZEOF(.flash.text)` regardless of those configs. The reservation exists
because `extern_ram_seg` and `drom0_0_seg` share one bus and one linear range,
so the firmware's own text and rodata must hold address space whether or not
they are also copied to PSRAM. The configs were reverted.

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

### That second check has its own blind spot (open)

`memory-xtensa-syms.S` describes `rb_ext_ram_end` as the end of `.ext_ram.bss`
and the boot check trusts it. It is not:

```
rb_ext_ram_end    0x3C55D2C0   <- what the check compares against codecbuf
.ext_ram.bss end  0x3C579D3C   <- the real end, 119 KB further
```

Other objects contribute to `.ext_ram.bss` through `PSRAM_BSS_ATTR` and are
placed *after* the arena, so the guard that exists specifically to catch
`.ext_ram.bss` growing into the pinned codec buffer cannot see 119 KB of it. IDF
already exports `_ext_ram_bss_end` for exactly this purpose; the check should use
it. Not yet fixed.

---

## 3b. Who actually holds the arena

Measured by walking buflib at three points (boot, first playback frame, after a
recording session closes). The full arena is 4,095 KiB and reads
`used=4095 KiB, free=0 KiB in 0 runs` in steady state.

| Bytes | Owner (resolved from the `ops` pointer) | Can it yield? |
|---|---|---|
| 1,980,744 | `apps/playback.c:1069` — playback file buffer | **yes** — registers `shrink_callback` |
| 1,980,824 | `buflib_ops_locked` — recording ring | **no** — cannot move or shrink |
| 1,179,668 | `dircache_runinfo.ops` — dircache | **no** — `move_callback` only |
| 200,016 + 100,016 | `apps/tree.c:1197` — recent dir/file caches | movable, no shrink |
| 153,616 x2 | `apps/gui/skin_engine/skin_backdrops.c:81` | movable, no shrink |
| 153,040 | `apps/tagtree.c:290` | movable, no shrink |
| 120,016 / 16,200 | `core_alloc()` with NULL ops | movable, no shrink |
| 40,016 | `apps/playlist.c:1940` | movable, no shrink |
| 20,496 | `apps/gui/icon.c:169` | movable, no shrink |
| 20,256 / 15,656 / 9,016 | `apps/gui/skin_engine/skin_parser.c:2050` x3 | movable, no shrink |
| 11,216 | `firmware/font.c:159` | movable, no shrink |

Three things this settles:

1. **There is no leak.** The allocation count goes 22 -> 21 across a recording
   session, so `core_free(pcmrec_handle)` runs. `free=0` is not exhaustion by
   attrition, it is the design: playback and recording each take everything left
   with `core_alloc_maximum()`, and they are mutually exclusive so only one is
   ever resident.

2. **`DIRCACHE_LIMIT` is load-bearing, not a workaround.** Dircache registers
   `move_callback` only, so buflib can relocate it but never take bytes back.
   `dircache_set()` has no callers outside `dircache.c`, and there is no
   `DC_OP_SET_SIZE_LIMIT`. The only brake is the compile-time cap, and
   `prepare_build()` takes the *whole* arena when the cache is stuffed:

   ```c
   int syncbuild = size > 0 && !stuffed ? 0 : 1;
   if (syncbuild)
       size = audio_buffer_available() + dircache_runinfo.bufsize;  /* grab */
   else
       size = MAX(size, DIRCACHE_RESERVE) + DIRCACHE_RESERVE*2;     /* +192 KB */
   ```

   A card large enough to stuff the cache therefore hits the grab path on every
   rebuild. The upstream default `DIRCACHE_LIMIT` is 6 MB, bigger than this whole
   window, so it must be lowered here. It is set to 1 MB (`DIRCACHE_MIN`).

3. **That is what fixed the EOM panic.** With the cap at 2 MB, dircache held
   2,228,228 B and playback's file buffer came back at 932,184 B -- less than
   crossfade's `MIN_BUFFER_SIZE + (fade_delay + fade_duration) * BYTERATE`
   demand of ~1.15 MB, hence `audio_reset_buffer_noalloc(): EOM (1149612 >
   932248)`. At a 1 MB cap dircache holds 1,179,668 B and the file buffer is
   1,980,744 B, which clears the demand with ~800 KB of margin.

**`CODEC_SIZE` was left alone deliberately.** Measured codec images peak at
182 KB text and 362 KB data, so 1 MB looks generous, but 78 of 87 targets use
`0x100000`; the `0x80000` cases are three and `0x48000` are IRAM-starved legacy
targets. There is no precedent for shrinking it on a target of this size, and
plugin support is the next feature, which needs the room.

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
| 17,024 | `usb_stack` (`usb.c`) | thread stack | **No** -- `DEFAULT_STACK_SIZE*4` plus `DUMP_BMP_LINESIZE`; the USB thread runs the GUI |
| 8,192 | `_transfer_buffer` (`usb_storage.c`) | DWC DMA | **No** -- the core's AHB master cannot reach PSRAM, hence `USB_STATIC_ALLOC` for ESP32S3 in `usb_storage.c:450`. 2 x 4 KB; the default 64 KB per direction overflowed the region by 152,968 B |
| 1,024 | `send_buffer` / `receive_buffer` (`usb_serial.c`) | DWC DMA | **No** -- same master |
| 4,096 | `cache_buffer` | SDHOST IDMAC destination | **No**, and it was halved: `fs_defines.h` sizes it from `MEMORYSIZE` (16 = flash size, not DMA-capable RAM), so 64 entries became 32. esp32s3 declares no `SOC_SDMMC_PSRAM_DMA_CAPABLE` |
| 4,096 | `dma_buf` | **moved to PSRAM** | TX DMA staging, 4 slots x 1024 B. GDMA can reach PSRAM for buffers (`SOC_AHB_GDMA_SUPPORT_PSRAM`, and the 20-bit window applies only to the descriptor address register), so this one moved; `desc[]` stayed, 128 B, because descriptors cannot |
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

1. **The arena has no headroom.** 26,316 bytes of the window are idle and the
   three largest holders are all either pinned or grab-everything. Any new
   resident consumer has to be funded by shrinking dircache, the theme, or the
   tree caches -- see §3b before assuming there is somewhere to put it.

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

# Name every arena allocation from an xt_arena_dump() log: buflib stores no
# owner names, but it does store the ops pointer, and every caller passes its own
# file-static struct buflib_callbacks, which carries a symbol. dircache is the
# exception -- it passes &dircache_runinfo.ops, a field, so resolve it to the
# nearest symbol *below* the pointer.
for p in $(grep -ao 'ops=0x[0-9a-f]*' LOG | sed 's/ops=//' | sort -u); do
  printf "%s " "$p"; $BIN/xtensa-esp32s3-elf-addr2line -e rockbox.elf -f $p | tail -1
done
```
