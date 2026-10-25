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

### The buflib arena is 4 MiB of PSRAM, and recording's ring is 2 MiB

`core_allocator_init()` maps the core context over `audiobuffer..audiobufend`, and
those two linker symbols measure exactly what they say in the linked image:
`0x3c1612e0` and `0x3c5612e0`, i.e. **4 MiB**, in the PSRAM part of the Ext_RAM
window.

`init_rec_buffers()` (`apps/recorder/pcm_record.c`) carves one maximum allocation
into `|PCMBUF|STREAMBUF|FILENAME|ENCBUF|`. The sizes for this target are not the
ones an earlier revision of this file claimed. `tools/configure` gives meowkits3
`MEMORYSIZE = 16`, so `pcm_record.c` takes the `#elif MEMORYSIZE <= 16` branch,
which defines **only** `PANIC_SECONDS` and `FLUSH_SECONDS` -- no chunk sizing --
and everything else falls to the defaults:

| | value | consequence |
|---|---|---|
| `PCM_NUM_CHUNKS` | 256 | default, not overridden |
| `PCM_CHUNK_SAMP` | 2048 | default |
| `PCM_SAMP_SIZE` | 4 | `2 * sizeof(int16_t)` |
| `PCM_CHUNK_SIZE` | 8192 | |
| `PCM_BUF_SIZE` | **2,097,152** (2 MiB) | not 512 KiB |
| `STREAM_BUF_SIZE` | 65,536 | default |
| `ENC_HDR_SIZE` | 4 | `union enc_chunk_hdr` is one word on 32-bit |
| filename block | 264 B | `CHUNK_FILE_COUNT(MAX_PATH) * 4` |
| layout minimum | 2,162,952 B | ring + stream + filename, before ENCBUF |

`struct enc_chunk_data` is `{ hdr; pcm_count; data[] }`, so the encoder's payload
starts at offset 8. That matches what `wav_enc`'s `codec_run` computes
(`a10 = a7 + 8`), so the two sides agree on the chunk layout.

`rec_buffer_size` is never assigned before the allocation. `buflib_alloc_maximum()`
(buflib_mempool.c:859) compacts and shrinks other allocations, then **overwrites**
`*size` with `buflib_allocatable()` -- so the grant is "whatever is free", not what
was asked for. `on_init_recording()` guards with `rec_buffer_size < PCM_BUF_SIZE`,
which passes when the grant is *exactly* 2 MiB. In that state `size` wraps (it is a
`size_t`) after subtracting the 64 KiB stream buffer, `enc_buflen` becomes ~357
million slots, `encbuf_get_write_ptr()`'s bounds test never fires, the ring never
wraps, and `enc_widx` grows until writes pass `audiobufend` into PSRAM pages the MMU
does not map. Nothing observed is in that state -- the fault's destination addresses
have all been inside the arena -- but the check does not exclude it.

`xt_arena_dump()` walked the allocator to answer that question and has been removed;

`xt_arena_dump()` walked the allocator to answer that question and has been removed;
the two numbers above are the answer, and the walk was only ever reachable through a
console this profile does not have.

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
| 8,192 | `dma_buf` | **moved to PSRAM** | TX DMA staging, 4 slots x 2048 B. The slot is `MIX_FRAME_SAMPLES * 4` times the mixer's rate multiplier keyed on `HW_SAMPR_MAX`, so it holds a 96 kHz frame; arm length is the frame size for the rate in effect, 1024 B at <=48 kHz. GDMA can reach PSRAM for buffers (`SOC_AHB_GDMA_SUPPORT_PSRAM`, and the 20-bit window applies only to the descriptor address register), so this one moved; `desc[]` stayed, 128 B, because descriptors cannot |
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

1. **The arena has no headroom, and it is now smaller.** 9,652 bytes of the
   window are idle (was 26,316 before the CPU-frequency seam moved
   `libfirmware.a(system.o)`'s .bss out of DRAM -- 16,664 bytes, see
   "The frequency seam is wired" below). The three largest holders are all either
   pinned or grab-everything. Any new resident consumer has to be funded by
   shrinking dircache, the theme, or the tree caches -- see §3b before assuming
   there is somewhere to put it.

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

5. **Static DRAM headroom is 184 bytes.** This is *not* the heap, and conflating
   the two is easy to do. `dram0_0_seg` is capped at `0x53700` and only bounds
   `.data`/`.bss`; IDF's internal heap is described separately in
   `components/heap/port/esp32s3/memory_layout.c`, which reserves
   `_data_start.._heap_start` (the statics) and the IRAM alias, and lets the heap
   run to `SOC_DIRAM_DRAM_HIGH` (`0x3FD00000`) -- about 149 KB free in this build,
   which is where `app_main`'s 8 KB stack and every FreeRTOS object come from.
   So running out of statics is a link error, never an allocation failure, and
   "free DRAM" in this file has always meant the former.

   The progression: 4,376 before `CONFIG_ESP_PANIC_HANDLER_IRAM` (cost ~5,464),
   4,320 after `file` -> PSRAM, 3,152 with the frequency seam, **184 with
   tickless idle and light sleep** (they cost ~2,968 of statics). The handler is
   what makes exit-path and cache-disabled faults reportable at all -- without it
   the panic handler itself cannot execute and the device reboots with no register
   dump -- so it is worth the space. Any new static has to come from a donor: the
   stack-free, DMA-free objects still in DRAM are `pcm_mixer.o` (4,121),
   `apps/settings.o` (2,316) and `dircache`'s non-stack half, and the
   `.lf` mechanism already moves objects on that rule.

---

## The mixer's rate multiplier, and the TX slot it must match

`pcm_mixer.c:mixer_set_frequency()` scales its frame by the sample rate:
`mix_frame_size = multiplier * MIX_FRAME_SAMPLES * 4`, with multiplier 1 up to
48 kHz, 2 above 48 kHz up to 96, and 4 above 96. So a 96 kHz handover is 2048
bytes and a 192 kHz one is 4096.

The transmit ring has to be capacity for the largest frame the target's
`HW_SAMPR_CAPS` can ask for, and arm length for the rate in effect. Both are
derived from the same multiplier (`XT_MIX_MULT`) in `i2s-xtensa.c`:

| `HW_SAMPR_CAPS` | `HW_SAMPR_MAX` | slot | armed |
|---|---|---|---|
| `SAMPR_CAP_ALL_48` and below | 48 kHz | 1024 B | 1024 B |
| `SAMPR_CAP_ALL_96` (this target) | 96 kHz | 2048 B | 1024 B at <=48 kHz, 2048 B above |
| above 96 kHz | 192 kHz | 4096 B | fails the `_Static_assert` — one GDMA descriptor holds 4095 |

`dma_buf` is therefore 4 x 2048 = 8 KB in PSRAM, up 4 KB from the earlier ring.
Internal DRAM is unaffected: the descriptors stay 4 x 32 B.

Before this, the slot was `MIX_FRAME_SAMPLES * 4` unconditionally and `fill()`
copied `pend_size` into it unbounded. At 96 kHz every handover wrote 1 KB past its
slot (into the next, and past the array on the last, into whatever follows in
PSRAM `.bss`), transferred only 1024 of the 2048 bytes, and reported the whole
frame consumed. High-rate local playback has therefore never worked on this
target even though `HW_SAMPR_CAPS` advertises it.

## Flashing cannot be confirmed by observation

The board has no UART bridge; the host reaches it through the SoC's own
USB-Serial-JTAG, and Rockbox moves those same pads (GPIO19/20) to the DWC OTG core
about 865 ms into `app_main`. `usb_init_device()` clears the RTC mux override at
boot so every boot starts with the pins on USJ, which is why the JTAG unit always
enumerates for the first second. It does not help after a flash: esptool's
`--after hard_reset` pulse travels over D+/D-, and the *running* image already owns
those lines, so the chip never sees it and keeps executing whatever was loaded
before. `Hash of data verified` proves the write, not that the new image ran.

That has quietly invalidated observations more than once. The workflow is: hold
BOOT to enter download mode, flash, then power-cycle. And the firmware now answers
the question directly — `app_main` stamps `esp_app_get_description()->version` into
`.noinit`, the panic handler stores the build id of the image that faulted into RTC
slow, and the boot report prints both on the `rb build` line. A backtrace resolved
against the wrong build is worse than no backtrace, which is the reason the line
exists.

## A stamp written on a crashing path is lost with the D-cache

Eight crashes produced byte-identical instrument output, including two stamp slots
whose value should have tracked the recording arena -- and the arena base visibly
alternated between boots (`pcm buf 3c37dd00` vs `3c37dcc0`). The stamps did not
change. That is not the encoder being silent; it is the cache.

`.noinit` is ordinary cached DRAM (`0x3fc......`). A store to it lands in a D-cache
line and stays there until something evicts that line. A panic resets the chip with
the cache invalidated and no write-back, so a store made moments before the fault
never reaches memory, and the next boot reads whatever stale DRAM was there -- which
survives warm resets, so it looks identical every time. The same record was read
successfully for `xtensa_pcm_slots` because those words are written in
`init_rec_buffers()`, seconds earlier, and normal operation evicted and wrote them
back in between.

The rule this establishes: **a value measured on a path that can fault is not durable
just because it is in `.noinit`.** It has to be read by the panic handler, which runs
while the caches are still coherent, and copied into RTC slow (`0x5000......`), which
is not cached. The boot report then reads RTC. This is also why the encoder stamp got
a fifth slot holding a call count: a zero count means the function was never entered,
where a stale value means the store was lost -- the two look the same otherwise, and
confusing them cost a build.

## The backtrace walk can show the *previous* boot's frames

`enc_encbuf_get_buffer()` and `enc_pcmbuf_read()` stamp their buffer and copy size,
and the panic handler reads those slots from `.noinit` while the caches are still
coherent. With the slots poisoned to `0xC0DEC0DE` at `init_rec_buffers()`, a fault
reported `encb c0dec0de/… encr c0dec0de/… n0`: the encoder callbacks were never
entered. Yet the same capture's backtrace contained `0x42600046`, which is exactly the
return address of `codec_run`'s `callx8` on the `enc_read` slot, and the slots above it
resolved to `codec_run_proc` and `run_codec`.

Both are true because DRAM survives a warm reset. The codec thread's stack still holds
the previous boot's frames when the new boot faults at the same place, and the walk
reads `sp-16`/`sp-12` from whatever is there -- so it can reconstruct a call chain that
this boot never executed. A backtrace taken this way is evidence about the *last* crash,
not this one, and it is only trustworthy where it is corroborated by something that
cannot be stale: the register slots, the cache latch, and stamps with a sentinel.

The practical consequence: the fault is earlier than the encoder path. `codec_run` calls
`get_command` (offset 60) before `get_buf` (144) and `enc_read` (136); nothing has yet
reached 144 or 136, so the fault is on the first call or in the recorder's own entry.

## A codec is a separate executable that calls the API by struct offset

`struct codec_api` is not a name table -- codec images load a pointer to the
firmware's `ci` and index it numerically. `lib/rbcodec/codecs/codecs.h` grows the
struct conditionally, so anything in `config.h` that changes those conditionals moves
every field after it. For this target, measured from the firmware's DWARF:

| offset | field |
|---|---|
| 60 | `get_command` |
| 128 | `logf` (present because `ROCKBOX_HAS_LOGF` is set) |
| 132 | `qsort` |
| 136 | `enc_pcmbuf_read` |
| 140 | `enc_pcmbuf_advance` |
| 144 | `enc_encbuf_get_buffer` |
| 148 | `enc_encbuf_finish_buffer` |
| | `sizeof(struct codec_api)` = 172 |

`firmware/export/config/meowkits3.h:667` gained `#define ROCKBOX_HAS_LOGF 1` in
`f475d50293` (the unconditional USB console), which inserted the `logf` word at 128 and
shifted the whole encoder block by one. The codecs packaged in `rockbox.zip` were from
before that commit -- all 43 of them differed from the build directory -- so
`wav_enc.codec` on the card called:

| it intends | offset it uses | what the firmware has there |
|---|---|---|
| `enc_encbuf_get_buffer` | 140 | `enc_pcmbuf_advance` |
| `enc_pcmbuf_read` | 132 | **`qsort`** |
| `enc_pcmbuf_advance` | 136 | `enc_pcmbuf_read` |

That is the recording crash. Playback was unaffected because its slots (`36`, `40`,
`60`) sit before `logf`, which is why the same card played files fine and died only on
record -- and why the symptoms looked like a firmware bug rather than a packaging one.

Two things that made it hard to see:

- **The loader only rejects a too-large API struct.** `apps/codecs.c:217` compares
  `c_hdr->api_size > sizeof(struct codec_api)`, so a codec missing a field has a
  *smaller* `api_size` and loads happily, calling `qsort` as its PCM read. A one-sided
  check cannot detect a shift.
- **`make` does not rebuild the zip.** The card is populated only from `rockbox.zip`,
  which is a separate goal (`make zip`). A stale zip therefore survives any number of
  firmware rebuilds, and the codec on the card can disagree with the running firmware.
  After any change that touches `struct codec_api` -- or any `config.h` define it
  depends on -- run `make zip` and re-extract, or the mismatch is on the card, not in
  the tree.

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

## USB DAC numbers, measured rather than derived (2026-10-23)

Device FIFO RAM is 200 words (GHWCFG3[31:16]), not the 256 the header implies, and there are
**4** dedicated IN FIFOs (GHWCFG4), not the 6 that `USB_NUM_ENDPOINTS` suggests. The split in
`usb-xtensa.c` is NPTX 16, PTX 16 x 4, RX 120 words = 480 bytes.

PTX has to be at least 16 words: `usb_dw_handle_dtxfifo()` pushes whole packets, computing
`words = (fifospace/maxpktwords)*maxpktwords`, so a 32-byte FIFO and a 64-byte full-speed
bulk packet give `words = 0`, the handler breaks, and the IN transfer never completes.
Reducing PTX to 8 to buy RX for audio broke mass storage exactly that way — enumeration was
fine because EP0 uses NPTX. RX is now computed from the hardware FIFO count rather than
`USB_NUM_ENDPOINTS - 1`, which is what freed the words without touching PTX.

An isochronous packet has to fit inside the RX budget plus EP0's and the CDC OUT packet, so
`USB_AUDIO_BUFFER_SIZE` is 320 and rates whose per-frame payload exceeds that (88.2 kHz at
353 B, 96 kHz at 384) are not advertised, via usb_audio.c's `USB_AUDIO_MAX_PACKET_BYTES`.

The route to 96 kHz is **not** a mode-dependent FIFO split. `usb.c:246` enables the
mass-storage driver on every insertion regardless of `usb_mode`, so a 64-byte bulk IN packet
is always in the composition and its FIFO must always hold a whole one; there is no mode in
which PTX can be traded for RX words.

**A 384-byte packet was tried on 2026-10-24 and killed the DAC.** Only
`USB_AUDIO_BUFFER_SIZE` (320 -> 384) and the guard's control allowance (two reserved control
packets -> one, so 480 bytes of RX held one packet plus one instead of plus two) changed. The
host enumerated an audio interface and streamed into it; nothing came out, and the USB DAC
debug page showed no sign of life. The mechanism was not identified: the descriptor's flexible
`tSamFreq[][3]` array is sized for `HW_FREQ_44+1` = 5 entries and the filtered count is 5, so
it did not overflow; `REAL_BUF_SIZE` stays a multiple of 32, so both ping-pong halves are
cache-line aligned; the target hook is asked for 768 bytes and has 768; and an OUT endpoint
needs no dedicated TX FIFO, so allocation could not have failed. Reverted rather than left
half-understood. Before 96 kHz is attempted again, something has to demonstrate that the
endpoint receives packets at all.

Audio ring: NR_BUFFERS 48 slots at REAL_DSP_BUF_SIZE = 1280 = 61,440 bytes from the 4 MB
audiobuffer arena. The ping-pong receive span is 2 * REAL_BUF_SIZE = 640 and lives in a
dedicated `xt_audio_rx_buf` at `0x3fcd2080`; mass storage keeps its own 8 KiB `_transfer_buffer`
at `0x3fcd2760`. These used to be the same memory, on the assumption that the USB profiles are
mutually exclusive — they are not (see AGENTS.md), and sharing meant the isochronous DMA
overwrote the head of every 4 KiB sector chunk while the DAC ran. Verified on hardware
(2026-10-24): 152 MB written to the card and read back during continuous playback, sha256
matching on readback; recording unaffected.

Internal SRAM has far more room than it looked like: the static region ends at `_heap_start`
(`0x3fcda5c0` after this change, `0x3fcda360` before) and the data view runs to `0x3fcf0000`,
so about 90 KiB sits above the firmware's `.bss`, unused by Rockbox because its own allocator
is in PSRAM. The earlier note that internal SRAM had "roughly a kilobyte of margin" was
wrong, and it is what caused the sharing. The restart gate / controller setpoint is MINIMUM_BUFFERS_QUEUED 22, because the
queue measures 22.5-23.5 with a gate of 24 -- see USB-AUDIO-RESEARCH.md section 9.

Interrupt levels on esp32s3, read from the config that this app actually builds with rather
than from the IDF source line that looked authoritative:

| source | level | where it is decided |
|---|---|---|
| audio GDMA OUT channel 0 | 1 | `ESP_INTR_FLAG_LEVEL1` in `gdma_init()`, i2s-xtensa.c; runs the mixer refill |
| FreeRTOS tick | 1 | sdkconfig sets `CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL1=y`; the `ESP_INTR_FLAG_LEVEL3` line at `components/freertos/port_systick.c:64` is inside `#ifdef CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL3`, which is not set |
| esp_timer | 1 | `CONFIG_ESP_TIMER_INTERRUPT_LEVEL=1`; its Kconfig gives `range 1 1` when the target is not esp32 |
| SD host | 1 | `sdmmc_host.c:539` allocates with flags 0, and `intr_alloc.c` turns a zero level mask into level 1 |
| I2C master | 1 | IDF allocates `ESP_INTR_FLAG_LOWMED`, which accepts level 1 or 2 and takes the lowest free one |
| cross-core | 1 | `crosscore_int.c` passes flags 0 |
| USB OTG | 2 | ours, `ESP_INTR_FLAG_LEVEL2` -- and therefore the top of the tree |

An earlier version of this table said the tick was level 3. It is not, and the mistake is the
interesting part: `port_systick.c` does contain `const unsigned level = ESP_INTR_FLAG_LEVEL3`,
but inside an `#ifdef` for a choice symbol this app does not select. Reading the source without
reading the built config produced a whole theory about tick occupancy delaying the transport.

What follows from the corrected table: nothing runs above level 2, so raising USB to 3 preempts
nothing, and `CONFIG_FREERTOS_HZ` cannot delay the transport at all -- the tick chains with the
audio GDMA pass at level 1, and IDF chains same-level handlers instead of nesting them. The only
thing that can hold a USB completion out is a region with interrupts disabled, and
`disable_irq_save()` here is `set_irq_level(DISABLE_INTERRUPTS)`: `PS.INTLEVEL = 15`, which
denies even a level-2 handler. That is where the ~1.8% frame loss lived -- `i2c_async_queue()`
ran this target's polling `__i2c_async_submit()` inside that mask, so each touch poll switched
USB off for ~1.6 ms. `I2C_ASYNC_SUBMIT_UNMASKED` defers the submit until the mask is released
and the loss is gone (`Filled` at the gate, nothing audible). `esp_cpu_enter_critical()` is no
longer a suspect: it was never reached on the audio path.

The display is the only console the audio profile has. With the cable attached, OTG passes to
our firmware about a second after boot, so the USB-Serial-JTAG console that carries the
breadcrumb dump is gone before a reader can attach; with the cable unplugged there is no
console at all, since GPIO 43/44 are the PMU interrupt and not UART0. Anything measured inside
the USB interrupt therefore has to reach `dbg_hw_info()`, not the RTC ring.

What survives the cleanup, and is the whole set — do not add to it without a question that
needs it:

| what | reads it | answers |
|---|---|---|
| the USB Audio debug page | the screen | `Filled`, `Min`/`Max`, `Frames dropped`, samples used per frame |
| `dbg_hw_info()` | the screen | core id, cache line, MMU pages mapped, PSRAM responding, DWC2 clock state, **the retained panic text and reset reason** |
| the panic screen itself | the screen | held for 20 s or until a key is pressed — see `system_exception_wait()` |
| `xtensa_paniclog_dump()` | boot console, first thing `app_main` does | previous boot's SRAM trail, reset reason, RTC trail, panic text |
| the `rb bt` lines in the boot report | CDC, next boot | up to 16 raw frame slots stepped by IDF's `esp_backtrace_get_next_frame`, 15 printed. Entry 0 is the real PC; later entries are windowed-ABI return addresses (bits 10-11 carry the window increment, and they point one instruction past the call), so fold them out offline rather than trusting them as printed. The interrupted SP is on the `ps` line, and comparing the two is what separates a hijacked return address from a stray pointer |
| the `rb build` line | CDC, next boot | which image is reporting and which one crashed — see §Flashing |
| `xt_usb_irqs` -> the `usbIRQ` trail entry | same dump | whether the OTG core interrupts at all |
| `dscr_streak` | not printed; it drives `tx_recover()` | whether a descriptor error is a transient or a storm |

`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` means IDF's own handler resets within
milliseconds of a panic, and its backtrace goes to UART0 — which this board exposes
only while the USB pads are still the console's, i.e. the first second of a boot with
no cable. That combination made a Rockbox panic unreadable: `panic()` draws the text
and `system_exception_wait()` reset before it could be read. The hook now polls the
GPIO keys (interrupts are masked on that path, so the tick and `button_get_*` cannot
work) for up to 20 s, prints `key = restart` on the last row, and resets. The record is
also on the debug page afterwards, which is the only way to read a panic you missed.

Everything else the bring-up measured is now recorded here or in USB-SPRINT.md instead of
being re-derived on the device each boot.

The laptop-mic capture of the speaker is not an instrument: two runs of one build gave
0.9 and 16.4 discrete >=2 ms slips per second, an 18x spread, because the marker envelope
rides on the speaker's 3-4 kHz resonance. Practical rig notes: start playback before
recording or PipeWire kills the capture stream, and `pw-record` has no `--format` flag.


```sh
# every codec must link at the firmware's codecbuf or apps/codecs.c rejects it
FW=$($BIN/xtensa-esp32s3-elf-nm rockbox.elf | awk '$3=="codecbuf"{print $1}')
for c in $(find lib/rbcodec/codecs -name '*.codec'); do
  o=$($BIN/xtensa-esp32s3-elf-readelf -l "$c" | awk '/LOAD/{print $3; exit}')
  [ "$o" != "0x$FW" ] && echo "MISMATCH $c $o"
done
```

## Power handling: the ack is what arms the cut (2026-10-24)

The AXP173 owns every rail, so power-off is a PMU conversation, not a SoC one. The
measured facts, in the order they matter:

| Fact | Evidence |
|---|---|
| PEK long-press latches in REG **0x46 bit 0** (IRQ23); REG 0x44 bit 0 is reserved | datasheet IRQ table + REG 46H bit list; the vendor driver for this board reads 0x44 and so never sees a press |
| Writing 1 to that bit **starts** the OFFLEVEL countdown | two builds differing only in that write: with it, a 2.5 s press ended in a cut ~10 s later; without it, the same press produced nothing and a continuous 10 s hold was needed |
| The countdown **cannot** be recalled once started | asserted first, then treated as fact; nothing found that aborts it |
| REG **0x36 bit 3** is the auto-off *function* enable (reset 1) | §REG 36H: 按键时长大于关机时长时自动关机功能设置, 0 关闭 / 1 打开 |
| REG **0x32 bit 7** is a separate mechanism | measured to drop every rail except LDO1, on battery and on cable, with bit 3 clear too |

So a press that Rockbox intends to refuse must be vetoed before it is acked: clear bit 3,
then clear the latch. `firmware/target/xtensa/button-xtensa.c` does exactly that while
`usb_host_session()` is true.

`usb_host_session()` is a new accessor in `firmware/usb.c` (`usb_state == USB_INSERTED`).
`usb_inserted()` is too broad for this purpose: it is also true for `USB_POWERED`, where the
cable only charges, the normal UI is running, and a shutdown is honoured - and being able to
power off while charging is worth having. The refusal itself is not MSC-specific:
`handle_usb_events()` returns only on `SYS_USB_DISCONNECTED`, so every enumerated class
refuses.

The poll reads REG 0x46 once per 100 ms (the read costs ~1.6 ms of polled I2C, this runs at
HZ in the tick thread) and stops once it has requested a shutdown, because `power_off()` has
to own the bus for its final write. `cancel_shutdown()` publishes no state, so the request
tick is remembered and the poll re-arms after 30 s of continued running - that is the
tagcache-busy refusal path.

Core files touched by all of this: `apps/misc.c` only, and only the
`#ifdef CLEAN_SHUTDOWN_MESSAGE_HOLD` seam, which defaults to no delay. `usb.c` gains the
accessor. Everything else is target and driver.

## The transmit interface runs on silence from boot (2026-10-24)

The cold-start garble was neither buffer placement nor rate bookkeeping. It was
bring-up order: MCLK is asserted by the clock generator, but **BCLK and WS only
leave the pads when `tx_start` is set**, and `tx_start` lived in `start_tx()` --
the first audio frame. The codec was reset, powered and given its clock registers
against a bus that did not exist, and acquired it at the exact moment real audio
arrived, so its lock transient landed on the first thing the listener heard. It
settled by itself a moment later at any rate, which is what made it look like a
rate-independent "first second" effect.

The MeowKit reference driver never lets the DAC see anything but zeros when there
is no audio: `tx_desc_auto_clear = true` ("Auto-clear DMA on underflow (silence)")
and `i2s_zero_dma_buffer()` on stop. Our I2S has no auto-clear, so an underrun
replays whatever the FIFO holds -- a gap is audible junk, not silence.

`idle_tx()` is that behaviour: flush, arm the whole ring with silence, start the
walk, never touch the PCM core. It runs from `sink_init()`, from `stop_tx()` and
from `i2s_set_frequency()`.

**Contract for anyone else that calls `tx_flush()`**: follow it with `idle_tx()`.
`tx_flush()` stops the GDMA channel and resets the I2S port; if nothing restarts
the walk, the ring is dead and nothing re-arms it. Related: the DONE branch re-arms
the slot it retires while `playing` is false **and advances `done_slot` exactly as
the playing path does**. An earlier version of that branch re-armed without
advancing, so every idle completion re-armed a slot the engine had already passed
and left the genuinely-retired one CPU-owned; measured on the board as ~50 errors
and ~16 `tx_recover()` calls per session, against `err 21 / rec 5` before the idle
ring existed, and each recovery discards a lap of audio (~23 ms) -- which is what a
clunk is. A silent free-running ring otherwise survives one lap -- four slots,
~23 ms at 44.1 kHz -- then walks onto a CPU-owned descriptor, raises
`out_dscr_err`, and the error branch declines to restart since nothing is playing.
`sink_stop()` no longer hands the descriptors back for the same reason; an emptied
ring is what the old teardown bursts were anchored on.

`stat_silence` counts only silence the *core could not supply* -- `playing` is set
before `start_tx()`, so preroll silence still counts, and the idle ring's ~86
re-arms per second do not. Without that guard the debug row climbs forever and
says nothing about starvation.

### Two codec registers that do not read back as written (unexplained)

Written and read over the same working I2C master, with `f=0` (no failed writes):

| REG | written | reads back |
|---|---|---|
| 0x08 (LRCK divider low half) | 255, i.e. 256-1 for 256*MCLK | 0x00 |
| 0x0D (analog power) | 0x01 | 0x02 |

In slave mode the chip takes LRCK from the pin rather than dividing MCLK, so 0x08
may be don't-care, and 0x0D may report status rather than intent. That is an
assumption, not a measurement. `es8311_stat()` is now a `Debug -> View HW info`
row so this stays observable without a console; it replaced the uncalled
`es8311_dump()`.

### Rejected: moving `dma_buf` into internal DRAM

`dram0_0_seg` overflows by 3,872 bytes with a 4 x 1024 TX ring there. Donors that
are neither thread stacks nor DMA destinations, measured from the link map:
`splash_buf` 2,121 (`apps/gui/splash.o`), `kbd_param` 2,084
(`apps/recorder/keyboard.o`), `simplelist_buffer` 1,024 (`apps/gui/list.o`) --
5,229 bytes against a 3,872 shortfall, so it is fundable. Rejected because a
placement change cannot explain a fault that heals on a one-second timer, and
because the D/IRAM region is the D-cache address room anyway, so it would not
remove the cache maintenance, only move it. Recorded here rather than in a stash.

## USB DAC sample-rate ceiling: 48 kHz, and it is the FIFO (2026-10-25)

96 kHz over USB device mode on ESP32-S3 is not reachable, and the reason is arithmetic
rather than folklore. The wire is not the limit: full-speed isochronous allows up to
1023 bytes per 1 ms frame, and a 96 kHz stereo S16 frame is 384. Neither is the
device's audio path -- 96 kHz plays locally on this board, exercising the same mixer,
TX ring, I2S clocks and codec.

The limit is the controller's single shared data FIFO. `GHWCFG3[31:16]` reads **200
words** on this silicon (`hwcfg3=0x00c804b5`, measured at boot). `GHWCFG4[3:0]` reads
0, so TinyUSB's `+ EP_LOC_CNT` correction adds nothing. The split in use is NPTX 16 +
4 x 16 dedicated IN + RX 120 words = 480 bytes. Synopsys' allocation rule, quoted in
TinyUSB's `dcd_dwc2.c:208`, is:

```
GRXFSIZ = 13 + 1 + 2 * (largest_ep_size/4 + 1) + 2 * ep_out_count
```

i.e. the shared RX FIFO must hold **two** packets of the largest endpoint size. Two
consecutive frames plus one 64-byte control packet:

| ISO packet | two frames + control | in 120 words? |
|---|---|---|
| 192 B (48 kHz) | 96 + 16 = 112 | yes, 8 spare |
| 256 B (64 kHz) | 128 + 16 = 144 | no |
| 384 B (96 kHz) | 192 + 16 = 208 | no -- more than the entire 200-word RAM |

Adding OUT endpoints makes it worse at 2 words each, so no endpoint count rescues it.
The only lever with any value is per-endpoint TX sizing -- only mass storage needs a
16-word IN FIFO, while HID, CDC notification and the audio feedback IN need 8, 16 and
4 -- which frees about 24 words and reaches 576 bytes: enough for two 64 kHz frames,
still nowhere near 96 kHz.

**Two attempts, both measured silent.** `USB_AUDIO_BUFFER_SIZE` 384 with the menu held
at 44.1/48 kHz, then with 88.2 and 96 kHz offered. Both produced the same signature:
USB Audio page shows `Playing`, `iface: 3 alt: 1`, `out ep: 0x2 in ep: 0x84`, and every
frame and sample counter at zero. Allocation was excluded (`ALLOC FAILED!` absent, and
the strong `usb_audio_target_rx_buffer()` returns the internal-SRAM static at
`3fcd20e0`, so the DMA destination was never in PSRAM). Local playback confirmed fine
in the same build, so the transmit chain was healthy. That is an RX overflow signature:
full-speed isochronous OUT is never re-sent, so bytes that do not fit are dropped
without an error, without a completion, and without a counter.

### Host-side readings that are worth keeping

`/proc/asound/card2/stream0` is the cheapest independent witness on Linux:

```
Playback: Status: Running, Interface 4, Altset 1
          Packet Size = 288            <-- ALSA's reserved ISO packet for the endpoint
          Momentary freq = 48000 Hz    <-- feedback value the host is honouring
          Rates: 96000, 88200, 48000, 44100
```

Two lessons from reading it:

- **`hw_ptr` advancing proves nothing.** Full-speed isochronous OUT has no handshake, so
  the host's URBs complete whether or not the device accepts the bytes. A stream can sit
  in `RUNNING` forever into a device that is silently discarding everything.
- **The offered rate list changes the host's reservation.** With two rates advertised
  ALSA reserved 192-byte packets; with four it reserved 288 at the same negotiated
  48 kHz. So a menu is not inert -- it changes what goes on the wire.

Also: a device that is reflashed while the host keeps its stream open produces exactly
this silence, because the host never re-sends `SET_CUR` after re-enumeration and the
device's rate stays at its `HW_FREQ_DEFAULT` -- which the debug page shows as a
plausible-looking `48000`. Replug before concluding anything about a fresh build.

## Power management: the kernel choice decides whether it exists (2026-10-25)

IDF gates power management on the FreeRTOS flavour, and the gate is structural
rather than a preference:

```
components/esp_pm/Kconfig:14-17
    # SMP FreeRTOS currently does not support power management IDF-4997
    depends on (!FREERTOS_SMP && SOC_PM_SUPPORTED) || __DOXYGEN__
components/freertos/Kconfig:319-324
    config FREERTOS_USE_TICKLESS_IDLE   # "Currently not supported in SMP ... yet"
    depends on PM_ENABLE
```

The mechanism is one port macro. The idle task's tickless branch calls
`portSUPPRESS_TICKS_AND_SLEEP(xExpectedIdleTime)` when the next wake is far enough
away; the xtensa port of the *vanilla* kernel defines it as `vApplicationSleep()`
(`FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h:510`), which
`esp_pm/pm_impl.c:855` implements as `esp_light_sleep_start()` followed by
`vTaskStepTick()` to repair the paused tick counter. The SMP port's `portmacro.h`
does not define the macro at all, so it falls back to the empty definition in
`FreeRTOS.h:866-867` — the tickless branch compiles and does nothing.

Light sleep is a whole-SoC state (CPU power-down, RTC peripheral power-down, cache
tagmem, and a *single* global tick counter), while "idle" under SMP is a per-core
predicate: one core can be ready to sleep while the other holds a spinlock on a
clock about to be stopped. `vApplicationSleep()` does take a `core_id` and consults
`other_core_should_skip_light_sleep()` (`pm_impl.c:848`, called at `:906`), so the intent
existed; the SMP port simply never reaches it.

**Decision: stay on the non-SMP kernel.** Verified for this build:
`# CONFIG_FREERTOS_SMP is not set` in `idf-app/sdkconfig`, and
`idf-app/build/esp-idf/freertos/libfreertos.a` is built from
`components/freertos/FreeRTOS-Kernel` (`strings` shows the source paths; `Kernel-SMP`
appears nowhere in it). Rockbox's kernel here is `NUM_CORES 1` and every thread is
pinned to core 0, so SMP buys nothing — and it would delete the power option
*silently*, because the Kconfig symbol simply stops being selectable. The one-line
re-check that this is still true:

```sh
strings idf-app/build/esp-idf/freertos/libfreertos.a | grep -c Kernel-SMP   # want 0
```

What is switched off today, each measured rather than assumed:

| | current | consequence |
|---|---|---|
| `CONFIG_PM_ENABLE` | not set | no DFS, no automatic light sleep |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 240 | the idle task spins at full clock |
| `HAVE_ADJUSTABLE_CPU_FREQ` | undefined | `cpu_boost()` and `set_cpu_frequency()` compile to nothing |
| `FREERTOS_USE_TICKLESS_IDLE` | not set | `vTaskStepTick` is absent from the kernel archive |
| `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0` | n | correct already — a blocked Rockbox core is normal |

The Rockbox side of the seam is already wired and already running; only the target
implementation is missing. `HAVE_SCHEDULER_BOOSTCTRL` is defined for every native
platform (`config.h:1051-1053`), so the boost state machine executes today:
`trigger_cpu_boost()`/`cancel_cpu_boost()` are called from `apps/codec_thread.c:403,469,520`
and `apps/playback.c:2470` (released at `:597,244,3195,3716`), from
`firmware/backlight.c:340,363,577`, from `dircache.c`, and `cpu_idle_mode(true/false)`
from `usb.c:374,380` on USB mode entry/exit. All of those reach `cpu_boost()`, which is
the empty macro. Defining `HAVE_ADJUSTABLE_CPU_FREQ` for this target makes them live
without touching a core file, and `CPUFREQ_MAX/NORMAL/DEFAULT` (240/160/80 MHz) are
already declared in `system-target.h`.

Two consequences of DFS that are already accounted for:

- All three ladder points are BBPLL taps, so AHB/APB — GDMA's clock — stays at 80 MHz
  and I2S, I2C and the timer groups stay on XTAL (`system-target.h:30-38`). Scaling the
  core moves nothing else.
- `udelay()` counts core cycles and `system_init()` caches `cpu_frequency`
  (`system-xtensa.c:129-135`). With DFS that cache is wrong, and the file already says
  what to do: read `esp_clk_cpu_freq()` and stop caching. This is a prerequisite, not
  a refinement.

The tick-rate coupling is the one that inverted a plan. `configEXPECTED_IDLE_TIME_BEFORE_SLEEP`
may not be below 2 (`FreeRTOS.h:874-876`) and IDF defaults it to 8 when
`FREERTOS_HZ > 200` (`freertos/Kconfig:340`). The shortest period Rockbox needs is the
10 ms tick, so 1000 Hz gives a floor of 8 ms against 10 ms of expected idle — sleepable,
with 2 ms of margin — while 100 Hz gives a floor of 80 ms against 10 ms, and light sleep
would never be attempted. Lowering the tick rate to save 900 interrupts a second is
therefore not a saving at all. `PM_SLP_IRAM_OPT` drops the default to 3 ticks (3 ms),
which widens the margin at the cost of ~180 B of RTOS idle code and 2.1 KB of PM code in
IRAM; IRAM headroom has to be read from the map before that is chosen, because DRAM free
is 1,020 bytes (§5 item 5) and the region guard will not forgive a guess.

`CONFIG_PM_SLP_SPIRAM_HALFSLEEP_ENABLED` is already `y`, which is the mechanism that lets
XIP-from-PSRAM survive light sleep (0.5x refresh plus PSRAM halfsleep). Unverified and
needs a hardware test before S4 is trusted: whether the **MSPI MMU table**
(`DR_REG_MMU_TABLE`, 512 entries) survives light sleep, since `SOC_PM_SUPPORT_RTC_PERIPH_PD`
is enabled and `rtc_init.c:114` clears `EXTMEM_CACHE_MMU_MEM_FORCE_ON`. If the table
loses power, `mmu_init()`'s audiobuffer and framebuffer mappings are gone on wake and
the first PSRAM access reads the Ext_RAM sentinel. `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` is
unset, so flash keeps power — keep it that way.

Measurement needs no new instrumentation. `CONFIG_BATTERY_MEASURE` includes
`CURRENT_MEASURE`, `axp173_discharge_current()` feeds `_battery_current()`, and
**Debug → View battery** prints `Battery current: N mA` (`apps/debug_menu.c:1244`,
`view_battery()` case 3) off the same moving average `powermgmt.c` uses for the runtime
estimate. Enabling the CPU-frequency seam also adds **Debug → CPU frequency**
(`debug_menu.c:2955`), which is the on-device witness for DFS. Note the AXP173's current
registers use five low bits, not the four the voltage registers use.

### S1 result: gating the PEK poll changed nothing measurable (2026-10-25)

Baseline on battery, `8b2ef8a821` tree: **78-80 mA** idle at the Debug -> View battery
screen (87 %, est. 950 min), **57 mA** after ~3 min of dark idle. With the PEK read
gated on the IRQ pin (`3a9d11e90d`) both readings were the same within noise, and a
1 s power-key hold still shuts the player down cleanly.

So the 1.6 ms poll was not a current cost: the AXP173's discharge reading is dominated
by the backlight and the awake 240 MHz core, and a 1.6 % duty cycle of blocking I2C
sits under that resolution. The change is kept for the reason it was actually sized
for -- it widens the idle window that light sleep has to fit into, from ~8.4 ms to
~9.7 ms against an 8 ms floor -- and it is the kind of saving that only becomes visible
once S4 makes sleep happen. Recording the null result matters: if S4 does *not* reduce
the dark-idle number, this is one of the things to re-check rather than assume.

Also measured, worth keeping as the scale of the problem: dark idle is 57 mA into a
1400 mAh cell. That is the number DFS and light sleep are aimed at, and it says the
frequency term (S3) and the sleep term (S4) together are worth far more than every I2C
and touch-poll trim combined.

One more thing worth knowing before anything network-ish is ever enabled: `esp_wifi` and
`bt` are `y` in sdkconfig but unreferenced, so their text is not linked (`esp_wifi_init`
has no entry in `rockbox.map`). If they were ever pulled in, their tasks run at priority
18 while every Rockbox task sits at `tskIDLE_PRIORITY + 1` — they would preempt the whole
player.

## The frequency seam is wired (2026-10-25)

`HAVE_ADJUSTABLE_CPU_FREQ` is now defined for meowkits3 and
`set_cpu_frequency()` is implemented in `system-xtensa.c` as `esp_pm_configure()`
with **max == min**. Rockbox's ladder (240 / 160 / 80 MHz, `system-target.h`) is
therefore applied by asking IDF for the frequency rather than writing CPUPERIOD_SEL
and the LDO slaves -- dbias travels with the core clock through REGI2C and the ROM's
PVT calibration, which only IDF can do, and the hand-written version of that is the
thing this file used to refuse. Keeping DFS inert (max == min) is deliberate: the
clock is exactly what the boost counter asked for, `udelay()`'s cached
`cpu_frequency` cannot drift out of agreement with the hardware, and the PM locks
IDF's own drivers take -- `i2s_common.c:1235` acquires one around every transfer --
cannot move the clock underneath us.

The core side needs nothing. `HAVE_SCHEDULER_BOOSTCTRL` is set for every native
platform (`config.h:1051-1053`) and `firmware/system.c` already owns the bookkeeping:
`cpu_boost_()` keeps `boost_counter`, calls `set_cpu_frequency(CPUFREQ_MAX)` on the
first boost, `CPUFREQ_NORMAL` when it drops to zero, and `CPUFREQ_DEFAULT` when
`cpu_idle_mode()` has marked the machine idle. So `trigger_cpu_boost()` from
`codec_thread.c:403,469,520` and `playback.c:2470` -- which were already executing,
against an empty macro -- now actually move the clock. Defining the seam changed no
core file.

**What it cost, and where that came from.** The seam's price is not IDF, it is a
diagnostic: `system.h:43-46` turns on `CPU_BOOST_LOGGING` when
`HAVE_ADJUSTABLE_CPU_FREQ` + `ROCKBOX_HAS_LOGF` + `NUM_CORES == 1` are all true, and
that allocates `cpu_boost_calls[64][MAX_PATH]` = 16,640 bytes (`MAX_PATH` is 260,
`fs_defines.h:32`). With `dram0_0_seg` 4,320 bytes free, enabling the seam overflowed
the region by 13,520. It is handled by granting `system (rb_psram)` in
`idf-app/main/rockbox_memory.lf`, the same mechanism already used for `file.o` and
`fileobj_mgr.o`; net result is DRAM free 4,320 -> 3,152 and the arena's idle slack
26,316 -> 9,652, flash +8,416.

**The fact that made that legal.** `.ext_ram.bss` is `NOLOAD` and IDF's `init_bss()`
clears only `_bss_start.._bss_end`, which are dram0 (`cpu_start.c:449`) -- so the PSRAM
range is *not* covered by it. It is cleared separately by `esp_psram_bss_init()`
(`esp_psram.c:767`, a plain `memset(&_ext_ram_bss_start, 0, size)`, called from
`cpu_start.c:771` and present in this image at `0x420061b4`). That matters whenever an
object with zero-initialised *bookkeeping* -- not just scratch buffers -- moves to
PSRAM: without it `boost_counter` would start random and `++boost_counter == 1` would
never fire, so the clock would never be set at all. Note the scheme only maps `bss`
and `common`; `cpu_frequency` is `.data` and stays in internal DRAM, so `udelay()` did
not start reading a clock word out of PSRAM on every call.

**Open, and the first thing to check on the device.** Nothing boosts during USB DAC
streaming: `usb_audio.c:1780` is `.needs_cpu_boost = false` (`usb_storage.c:1610` is
true), and `usb.c:374` sets `cpu_idle_mode(true)` on entering USB mode -- which now
means the core runs at 80 MHz while isochronous frames arrive. Whether that costs
anything is a measurement, not an argument: watch `Debug -> View HW info`'s TX row
(`sess / err / rec / sil`) during USB playback. If it degrades, the fix is target-side
-- hold a boost across `usb_dw_target_enable_clocks()`/`disable_clocks()` -- rather
than editing the core driver's flag.

**Measured** (protocol: on battery, Debug -> View battery view 3, ~60 s on screen then
~3 min dark). Baseline before the seam was 78-80 mA at the UI screen and **57 mA dark**;
with the seam it is **48 mA dark**, so the idle clock alone is worth 9 mA, about 16% of
the dark-idle draw, and `Debug -> CPU frequency` confirms the ladder: 80 MHz idle,
240 MHz during playback, 160 MHz while browsing.

The USB DAC question resolved in favour of leaving it alone: a minute of streaming at
80 MHz left the TX row (`Debug -> View HW info`) solid and error-free, so
`.needs_cpu_boost = false` in `usb_audio.c:1780` is not a problem to work around and no
target-side boost is added. Full-speed isochronous OUT is 192 bytes per 1 ms frame, and
servicing that is nowhere near the core's limit at 80 MHz. Worth re-checking only if the
endpoint geometry changes.

Scale for what is left: dark idle is 48 mA into a 1400 mAh cell. DFS moved the *awake*
floor from 240 to 80 MHz; the remaining draw is the chip staying awake at all, which is
light sleep's territory, not frequency's.

## Light sleep is on, and the setting that makes it safe is not the obvious one (2026-10-25)

`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` (needs `PM_ENABLE`, needs the non-SMP kernel) puts
`portSUPPRESS_TICKS_AND_SLEEP()` in the idle task, which on this port is
`vApplicationSleep()` -> `esp_light_sleep_start()` -> `vTaskStepTick()`.
`esp_pm_configure()` now passes `light_sleep_enable = true`, with a retry at `false` so
the frequency still gets programmed if tickless idle is ever switched off again
(`esp_pm_configure()` returns `ESP_ERR_NOT_SUPPORTED` for a sleep request without it --
that failure would otherwise silently stop `set_cpu_frequency()` doing anything at all).

**The load-bearing setting is `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`, left off.**
It is IDF's default, but nothing in this project would notice if it were on and plenty
would break: Rockbox's I2S, GDMA, GPSPI2, LEDC, I2C and USB drivers write registers
directly rather than through IDF's driver model, so they have no enter/exit sleep
callbacks, and `PMU_SLEEP_PD_TOP` would drop the whole digital domain and lose the
display's GPIO-matrix routing, the LEDC channel behind the backlight, the GDMA link
address and the I2S dividers. With it off, light sleep powers the CPU and cache only:
registers retain, and the audio ring keeps being walked by hardware while the core is
asleep. `esp_sleep_pd_config()` has no `ESP_PD_DOMAIN_DIG`-style knob for this on
esp32s3 -- it is the Kconfig symbol or nothing -- so it is written explicitly in
`sdkconfig.defaults` rather than inherited silently.

**One lock, and it is the USB one.** `xt_light_sleep_block(bool)`
(`system-xtensa.c`, declared in `system-target.h`) holds `ESP_PM_NO_LIGHT_SLEEP`, created
on first use, and `usb-xtensa.c` acquires it in `usb_dw_target_enable_clocks()` and
releases it in `usb_dw_target_disable_clocks()`. The reason is that a peripheral
interrupt is not a light-sleep wake source: with the core asleep, a DesignWare frame
completion would sit pending until the next tick, and with two frames of RX FIFO
headroom that is a drop. SD and I2C need nothing of the kind -- their transfers are
driven by a thread that is running, and with the peripheral domain powered the hardware
keeps going even if a sleep slips in during a wait. Recording that reasoning matters
because "hold a lock for every busy peripheral" looks like the safe general answer and
is actually the one that prevents sleeping altogether: the idle TX ring is *always*
busy, so a lock around the audio path would have meant no sleep ever.

Why sleeping does not starve the audio ring: the DONE interrupt cannot wake the core, so
slots retire without being re-armed while asleep -- but sleep windows are bounded by the
10 ms tick, the ring is four slots (~92 ms of silence), and the handler re-arms on the
next wake. Margins, not luck: 10 ms of sleep against 92 ms of ring.

Budgets: static DRAM headroom 3,152 -> 184 (see §5 item 5), flash +10,976, IRAM unchanged
and irrelevant here -- `PM_SLP_IRAM_OPT` would want ~2.1 KB and `iram0_0_seg` has 140
bytes, so the sleep path executes from flash XIP; that is fine because flash keeps power
in light sleep and the leakage workarounds hold CS0/CS1. `vTaskStepTick` landed in IRAM on
its own (`0x4037f6b4`). The sleep threshold is IDF's computed default of 8 ticks = 8 ms at
1000 Hz, against the 10 ms tick period -- see the note in `sdkconfig.defaults`, which also
explains why `FREERTOS_HZ` cannot drop to 100.

Not yet measured on the device: dark-idle current with sleeping enabled (baseline after
the frequency seam was 48 mA), whether the TX row stays clean across sleep/wake
boundaries, and whether the USB-JTAG console survives a sleep while idle (it is the boot
log's only output, and `CONFIG_ESP_SLEEP_*` handles pad restore for chips that do not
support USJ in light sleep).
