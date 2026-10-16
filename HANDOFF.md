# Session handoff — Rockbox ESP32-S3 port

**Branch:** `esp32-s3-ports`
**Repo:** `/home/jhohertz/co/rockbox`
**HEAD:** `6bbab13605` ("xtensa: get the SD card mounted"), 124 commits ahead of
upstream. Working tree is dirty — see "Uncommitted changes" below.
HANDOFF.md is a session artifact and is deliberately untracked.

**Read `AGENTS.md` first.** It is the accumulated hardware reference and every
register claim in it has been checked against ESP-IDF headers or the SVD. Do not
re-derive anything from memory.

---

## 0. PLAN — stop reimplementing what ESP-IDF already provides

This is the strategic decision taken after the PSRAM-write stall, and it governs
every subsequent step. Do not lose it.

### Findings that led here

- **No Rockbox target uses FreeRTOS or any RTOS.** Rockbox *is* the RTOS:
  `firmware/kernel/thread.c` owns the ready list, timeouts, `sleep()`,
  `wake_thread()`, semaphores, mutexes, tick tasks. A target only supplies
  `tick_start()`, `thread_store_context()`/`thread_load_context()`, and
  `core_sleep()`/`wait_for_interrupt()`. Every bare-metal arch implements those
  in assembly.
- **But the layering already exists in the tree.** `firmware/asm/thread-unix.c`
  (setjmp/longjmp + a signal trampoline), `thread-win32.c` and `thread-sdl.c`
  run Rockbox's cooperative threads *inside one OS thread*, and
  `firmware/target/hosted/kernel-unix.c` supplies `tick_start` from a timer
  thread. "Rockbox on top of someone else's scheduler" is a supported shape.
- **ESP-IDF's Xtensa interrupt/exception code is not FreeRTOS-specific.** It is
  Cadence's generic "Xtensa RTOS support sources" with a glue header
  (`xtensa_rtos.h`) that maps exactly three callouts — `XT_RTOS_INT_ENTER()`,
  `XT_RTOS_INT_EXIT()`, `XT_RTOS_TIMER_INT()`. There is a **bare-metal port**
  where those are no-ops. Everything else — `_xt_context_save`,
  `_xt_context_restore`, `_xt_user_exit`, the window overflow/underflow
  handlers, the vector layout, alloca and coprocessor exception paths — is
  generic assembly we can take verbatim.
- **The actual blocker is peripheral bring-up, not the scheduler.** The current
  hang is a bulk write to the PSRAM framebuffer through SPI0's Ext_RAM cache.
  One word per MMU entry across 121 pages works; a multi-kilobyte write stalls.
  AGENTS.md already records the smoking gun: "at 20 MHz MR0 reads back 0x0d28
  … and after going to 80 MHz every subsequent read returns the previous value",
  and `psram-xtensa.c` says outright that we copied ESP-IDF's *fallback*
  config because "Rockbox has no equivalent" of `mspi_timing_psram_tuning()`.

### The plan, in order of payoff

1. **Port ESP-IDF's PSRAM bring-up and MSPI timing tuning verbatim.**
   Sources (all RTOS-free, register level + ROM calls):
   - `components/esp_hal_mspi/esp32s3/include/hal/psram_ctrlr_ll.h`
   - `components/esp_hal_mspi/esp32s3/include/hal/mspi_ll.h`
   - `components/esp_mspi/mspi_timing_tuning/{mspi_timing_tuning.c,port/esp32s3/*}`
   - `components/esp_hal_cache/esp32s3/include/hal/{cache_ll.h,mmu_ll.h}`
   - `components/esp_psram/esp32s3/esp_psram_impl_octal.c`
   Replace the hand-rolled sequence in `firmware/target/xtensa/psram-xtensa.c`
   with ESP-IDF's, **including the DIN_MODE/DIN_NUM/TIMING_CALI sweep**.
   Success test: the 16 KB bulk framebuffer write completes and reads back.

2. **Replace the hand-written level-1 handler with ESP-IDF's `_xt_lowint1`,
   `_xt_context_save` and `_xt_user_exit`, stubbing `XT_RTOS_*`.**
   `_xt_context_save` is written for the windowed ABI: it saves a2-a11, a14, a15,
   SAR, LBEG, LEND, LCOUNT, then runs `SPILL_ALL_WINDOWS` with EXCM temporarily
   cleared and INTLEVEL raised to `XCHAL_EXCM_LEVEL` (= 3 on this core). It is
   the same machinery FreeRTOS-on-Xtensa uses for task switching, so it is the
   right thing to be compatible with.
   Note: `_xt_context_save` **must be entered via `call0`**.

3. **Keep Rockbox's scheduler.** Do not swap in FreeRTOS. If Bluetooth becomes a
   goal, the compatible end state is:
   ```
   FreeRTOS (ESP-IDF)
     |-- task: ESP-IDF BT / WiFi / drivers
     |-- task: Rockbox (its own cooperative threads inside this one task)
   ```
   Adopting (1) and (2) now makes that transition cheap, because the context
   handling is already the code FreeRTOS uses.

### What is *not* in the plan

- Reimplementing soft float or 64-bit divide (libgcc is fine now that we are on
  the windowed ABI).
- Replacing `firmware/kernel/thread.c`.
- Hand-tuning MSPI dummy/delay values by guesswork — take ESP-IDF's sweep.

---

## 1. Where the port is right now

| Subsystem | Status |
|---|---|
| Boot through the mask ROM | works; crt0 brings up PSRAM, MMU, IROM mapping |
| CPU frequency | 240 MHz (was stuck at 20 MHz) |
| Scheduler + tick thread | runs; 930 level-1 interrupts handled with zero window drift |
| SD card | **mounts** — `disk_mount_all()` returns 7 (sometimes 1 or 0 after a hard reset) |
| Dircache scan | completes within 300 s |
| LCD (ST7789 over GPSPI2) | **works** — user saw the Rockbox logo and a panic screen |
| Backlight | on |
| I²C (PCA9557, AXP173, FT6336, ES8311) | working transaction-level path |
| USB | stub only (OTG core not built; VBUS comes from the AXP173) |
| Audio | **not started** |
| Stability | boot reaches the UI, then hangs/resets. Root cause not yet pinned. |

The reset is **not** a watchdog: RTC `WDTCONFIG0 = 0x00000000` (WDT_EN clear),
brownout disabled. `RESET_STATE` reset_cause_procpu reads 21 =
`USB_UART_CHIP_RESET`, i.e. esptool's own reset, when the probe is what stopped
it.

---

## 2. The decisive finding of this session: `rfe` semantics

The whole windowed-ABI question turns on what `rfe` (= `RESTOREPS; RETW`) does at
the end of a level-1 handler. It was unresolved because the Cadence ISA PDF is
not reachable (403/404 from every mirror tried, including web.archive).

**Resolved by experiment on the board.** A one-shot probe in
`xt_level1_handler` set `a0` to `xt_marker_a0` and `epc1` to `xt_marker_epc1`,
both spin markers that stamp a distinct 32-bit value into `xt_probe_out`, then
executed `rfe`.

Result:

```
xt_probe_done   = 1
wb before rfe   = 0000000d
ws before rfe   = 00002aa0
ps before rfe   = 00060000
MARKER          = 45454545      -> 'EEEE'
```

So:

* **`rfe` resumes at EPC1, not at a0.** Setting `a0` to a decoy had no effect.
* **`rfe` does not rotate the window.** `xt_isr_hist` recorded
  `wb@entry == wb@exit` and `delta == 0` across 930 interrupts.
* The exception-entry PS on this core is `0x60010`: `INTLEVEL=0`, `EXCM=1`,
  `UM=0`, `OWB=0`, `CALLINC=8`, `WOE=1`. Note `INTLEVEL` is **0**, not 1 — this
  core does not raise INTLEVEL for a level-1 exception taken at level 0.

The handler's current exit (`PS = entryPS & 0xFFFF0000`, then `wsr epc1`, restore
a0..a15 and a1, `rfe`) is therefore correct. The comment in `vectors.S` claiming
the mask clears `CALLINC` is wrong — `0xFFFF0000` clears bits 0-15 and leaves
bits 16-17 (`CALLINC`) and 18 (`WOE`) set — but it does not matter, because RETW
does not use CALLINC in exception mode.

**The probe is still in `vectors.S`** behind `#define XT_RET_PROBE 1` at the top
of the file. Set it to 0 for production builds.

---

## 3. The ROM ships a complete XOS exception layer — worth knowing, not usable

`/tmp/romelf/esp32s3_rev0_rom.elf` (from `esp-rom-elfs`) contains a full Xtensa
RTOS exception layer at the default VECBASE (0x40000000):

| Symbol | Address | Notes |
|---|---|---|
| `_WindowOverflow4/8/12`, `_WindowUnderflow4/8/12` | 0x40000000–0x40000170 | byte-identical to ESP-IDF's, `s32e`/`l32e` + `rfwo`/`rfwu` |
| `_xtos_alloca_handler` | 0x40000010 | alloca (EXCCAUSE 5) |
| `_UserExceptionVector` | 0x40000340 | builds a 256-byte pseudo frame, dispatches via `xtos_exc_handler_table` at 0x3fcedf68 |
| `_xtos_l1int_handler` | 0x40034eb8 | full level-1 dispatch: `entry a1,0x100`, `rsil a15,1` (clears EXCM, sets INTLEVEL), `callx12` to the C handler |
| `_xtos_return_from_exc` | 0x400559b4 | restores PS verbatim, sets EPC1, `rfe` |
| `xthal_window_spill` | 0x40053e74 | ROM window spill (**not** in the published `esp32s3.rom.ld`) |
| `xthal_window_spill_nw` | 0x40053d60 | the actual spill loop |

Only these are in the published ROM interface (`esp32s3.rom.ld`):
`_xtos_ints_off/on`, `_xtos_restore_intlevel`, `_xtos_set_exception_handler`,
`_xtos_set_interrupt_handler(_arg)`, `_xtos_set_intlevel`, `_xtos_set_vpri`,
`xthal_get_interrupt`, `xthal_set_intclear`, `xthal_bcopy`, `xthal_memcpy`,
`xthal_get_ccompare`, `xthal_set_ccompare`, `xthal_get_ccount`.

**Why we cannot use it directly:** the ROM's `_KernelExceptionVector` is
`break 1,0; j .` — supervisor-mode (UM=0) exceptions are unhandled. Rockbox runs
in supervisor mode, so the ROM's vectors would trap. ESP-IDF overrides both
vectors for the same reason.

`xthal_window_spill` is callable at a fixed address but is not part of the
published interface, so it is revision-fragile. `__xtensa_libgcc_window_spill()`
(at `0x429038f0` in the current image) is the safer equivalent and is what
`store_context()` and `lib/libsetjmp/xtensa/setjmp.S` already use.

ESP-IDF does **not** call the ROM spill; it uses the `SPILL_ALL_WINDOWS` macro
from `components/xtensa/include/xt_asm_utils.h` (also in `/tmp/xt_asm_utils.h`).

---

## 4. Window spill / context save — the reference implementation

ESP-IDF's `_xt_lowint1` / `_xt_context_save` / `_xt_user_exit`
(`/tmp/vectors_S`, `/tmp/xtensa_context.S`, extracted with
`git show HEAD:components/xtensa/xtensa_vectors.S`) is the proven model for this
exact core with the windowed ABI. The sequence:

1. Vector: `wsr a0, EXCSAVE_1` then `call0 _xt_user_exc` (`call0` used as a jump).
2. `_xt_user_exc`: `rsr exccause`; if `EXCCAUSE_LEVEL1INTERRUPT` → `j _xt_lowint1`.
3. `_xt_lowint1`: allocate `XT_STK_FRMSZ`, save pre-interrupt SP, PS, EPC1,
   EXCSAVE_1 (a0), and the exit point.
4. `call0 _xt_context_save` — saves a2-a11 (+a14,a15 for windowed ABI), SAR,
   LBEG/LEND/LCOUNT (and *zeroes* LCOUNT so the dispatch's loops do not rewind
   the interruptee's).
5. Inside `_xt_context_save`, for the windowed ABI:
   - `INTLEVEL = max(INTLEVEL, XCHAL_EXCM_LEVEL)` (= 3 on esp32s3)
   - clear `EXCM`, set `WOE`
   - save EPC1 (the overflow exceptions overwrite it)
   - `addi sp, sp, XT_STK_FRMSZ` then `SPILL_ALL_WINDOWS` then subtract again
   - restore PS and EPC1
6. `wsr PS_INTLEVEL(1) | PS_UM | PS_WOE` → C code can now run.
7. `dispatch_c_isr` — uses `call4`/`call8`, i.e. real window rotation.
8. `_xt_user_exit`: restore PS **verbatim** from the frame, set EPC1, restore a0
   and sp, `rsync`, `rfe`.

`SPILL_ALL_WINDOWS` for `XCHAL_NUM_AREGS == 64`:
```
and a12, a12, a12 ; rotw 3
and a12, a12, a12 ; rotw 3
and a12, a12, a12 ; rotw 3
and a12, a12, a12 ; rotw 3
and a12, a12, a12 ; rotw 4
```
Net rotation zero; each `and` raises a window overflow if a12 holds a live
ancestor register, and the overflow handler spills it into that ancestor's frame.
Requires `EXCM=0` and `WOE=1`. It is *faster* than the HAL `xthal_window_spill`
(145 vs 279 cycles on LX6 per the header comment).

**The pseudo base-save area is mandatory.** ESP-IDF's vectors.S header comment
says it outright: the exception dispatcher uses the same window as the
pre-exception code and only uses `call0`, so to make the overflow handlers able
to walk the chain you must store a copy of the pre-exception base-save area
(a0..a4) below the dispatcher's SP. Our handler does this with
`s32e a0/a10/a2/a3` at `a1-16/-12/-8/-4`. Without it the overflow handler reads
uninitialised stack as the ancestor sp and stores through garbage — that was the
`StoreProhibited` at address 0 we saw in `xt_double_trap`.

`XT_ISR_SPILL` in `vectors.S` is currently **0**. The dispatch works without an
explicit spill because the pseudo base-save area makes the overflow chain valid.
Setting it to 1 previously produced a bad overflow (`a1 = 0xcccccccd`) and was
reverted. If a deeper dispatch chain ever breaks, revisit it with the ESP-IDF
ordering (spill *before* the dispatch, PS restored afterwards).

---

## 5. Context switch (`firmware/asm/xtensa/thread.c`)

`struct regs` (`firmware/asm/xtensa/thread.h`):
```
r[16]  a0, a2..a15          (offsets 0,8,12,...,60)
sp     64
ps     68
windowbase 72
windowstart 76
sar      80
start    84   (thread entry point, cleared to 0 once running)
```

`store_context()` calls `__xtensa_libgcc_window_spill()` first, then saves the
current window plus WB/WS/PS/SAR. PS is masked with `-16` (0xFFFFFFF0) which
clears `INTLEVEL` (bits 0-3) but **leaves `EXCM` (bit 4) alone**. If a context is
ever saved while `EXCM=1`, the resumed thread runs in exception mode and its next
`retw` raises `EXCCAUSE_ILLEGAL_INSTRUCTION`. **This is the most likely cause of
the boot loop and should be changed to `-17` (0xFFFFFFEF).**

`load_context()` restores WB first, then WS, PS, SAR, then the registers. It
cannot use a plain `l32r sym` because the context lives in `.bss` (PSRAM window)
while the code is in the flash window — outside l32r's 16-bit PC-relative range —
so the address is parked as a literal in the same section and jumped over.

There is a self-test (`XTENSA_THREAD_SELFTEST`) that alternates two threads, each
recursing 6 deep through a function pointer, switching at the bottom, and
checking that `xt_depth(6)` returns 21. It is off in the production build.

---

## 6. Tooling

| Tool | Purpose |
|---|---|
| `/tmp/rbwin` | build dir. `make -j8` → `rockbox.elf` + `rockbox.esp32s3` |
| `/tmp/rbtest/prefix/xtensa-esp-elf/bin/xtensa-esp-elf-gcc` | esp-14.2.0_20260121 |
| `/tmp/rbtest/prefix/bin/xtensa-esp32s3-elf-nm`, `.../xtensa-esp-elf-objdump` | symbol/disassembly |
| `/tmp/probe8.py` | reads `xt_probe_out`, `xt_isr_hist`, boot log tail. Run with `/tmp/esptoolvenv/bin/python probe8.py` |
| `/tmp/probe7.py` | reads boot_log tail, `xt_exc_dump` (128 words), `xt_isr_hist`, `xt_ctx_hist` |
| `/tmp/rbconsole.py` | CDC console (RTS-only reset) |
| `/tmp/rawread.py` | live CDC capture |
| `/tmp/rbwatch.py` | CDC without reset — only captures the 28 bytes of ROM loader output, because `boot_trace` writes to UART0 FIFO, not USB-Serial-JTAG CDC |
| `/tmp/romelf/esp32s3_rev0_rom.elf` | ROM ELF for XOS/ISA archaeology |
| `/tmp/espidf` | full ESP-IDF git clone (sparse). `git show HEAD:<path>` extracts any file |
| `/tmp/vectors_S`, `/tmp/xtensa_context.S`, `/tmp/xt_asm_utils.h` | ESP-IDF xtensa port, extracted |

**esptool `dump_mem` hard-resets the chip** before attaching, so peripheral
registers read as reset defaults. For runtime state use the boot-log ring or
`xt_exc_dump`.

`xt_exc_dump` is in `.iramdata` (instruction bus, 0x4037aec0 currently); probes
must subtract 0x6F0000 to read it through the data-bus alias. Symbols in
`.srambss` (0x3fc...) do not need the alias.

Layout of `xt_exc_dump` (96 words):
```
0 exccause   1 epc1   2 excvaddr   3 epc2
4 a0  5 a1  6 a2  7 a3  8 a4(marker 0)  9 a5 ... 19 a15
20 ps  21 windowbase  22 windowstart  23 excsave1
24 intenable  25 interrupt  26 boot_log_len_at_trap
56 double_trap_count  57 exc_trap_count
32..60 nested (double) exception
```

`xt_isr_hist` (8 words, `.srambss`): `wb@entry, ws@entry, ps@entry, wb@exit,
ws@exit, count, delta, spare`.

---

## 6b. Booting through ESP-IDF's second-stage bootloader

**Does the MeowKit use ESP-IDF stage 2? Yes.** `meowkit-s3-v1.0.2.bin` is a
standard ESP image: ESP-IDF second-stage bootloader at 0x0 (entry `0x403c98d8`,
three segments, DROM at `0x3fce3808`), partition table at 0x8000, app at 0x10000.
It is arduino-esp32 on ESP-IDF 4.4.3. Flashing that image back makes the player
work, so the board definitely runs a stage-2 loader.

`tools/esp-stage2` writes the same layout from a prebuilt ESP-IDF bootloader:

```sh
make flash-stage2 ESPBOOT=~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/bin/bootloader_qio_80m.elf ESPPORT=/dev/ttyACM0
```

Two things learned the hard way:

1. **The partition table has to say APP.** The first version of `esp-stage2`
   wrote `app0` as type 2 (`BOOTLOADER`) instead of type 0 (`APP`), so the loader
   never found an application. `PARTITIONS` in the script is now
   `(0, 0, 0x10000, 0x330000, "app0")`.

2. **The PlatformIO prebuilt bootloader does not run on this board.** With it at
   0x0 the mask ROM prints, on every attempt:

   ```
   ESP-ROM:esp32s3-20210327
   rst:0x7 (TG0WDT_SYS_RST), boot:0xb (SPI_FAST_FLASH_BOOT)
   SPIWP:0xee
   mode:QIO, clock div:2
   load:0x3fce3808,len:0x4bc
   ets_loader.c 78
   ```

   and then the watchdog resets. A normal ESP32-S3 boot prints one `load:` line
   per segment followed by `entry 0x...`; here only the first segment is loaded.
   The stock bootloader extracted from `meowkit-s3-v1.0.2.bin` (entry
   `0x403c98d8`, DROM len `0x44c`) is the known-good one and is the one to use.

**How to tell whether crt0 actually ran.** RTC slow memory survives a reset, so
crt0 stamps `0xC0FFEE01` at `0x50000D60` immediately after `boot_trace_start()`. If
a probe reads that value, this build's crt0 executed. Under the PlatformIO
bootloader it never did.

**RTC slow memory is not uniformly writable.** A whole-8KB stamp of `0x5A5A5A5A`
landed on zero words. `0x50000D80` and `0x50000E40` silently drop stores and read
back whatever an older run left. The slots `bootlog.c` uses — `0x0000` log, `0x0C00`
live, `0x0C80` hist, `0x0D40` diag, `0x0E80` exception, `0x1004` log length — are
verified to stick. Do not place new markers anywhere else.

**USB access caveat.** Once the stock firmware is running it claims the
USB-Serial-JTAG, and esptool can no longer connect (`No serial data received`).
Toggling DTR/RTS from a Python script made the device disappear from USB
entirely. To flash again the board has to be put in download mode by hand
(hold BOOT while tapping RESET) or the stock firmware has to be replaced.

---

## 7. Open problems, in priority order

1. **No way to flash right now.** The board is running the stock firmware, which
   claims the USB-Serial-JTAG; esptool cannot connect. Get the board into
   download mode (BOOT + RESET) before the next flash.
2. **Boot reaches the UI then hangs/resets.** Exception seen at a `retw.n` with
   `exccause = 0` (ILLEGAL_INSTRUCTION). The epc1 value shifts between runs
   (0x42947ce8, 0x42947cf2, 0x42947d36) — non-deterministic, which points at a
   window/PS state problem rather than a fixed code bug.
   - Prime suspect: `store_context()` masking PS with `-16` instead of `-17`,
     leaving `EXCM` set in a saved context.
   - Second suspect: the interruptee's `OWB` is not preserved by the handler exit
     (`0xFFFF0000` zeroes bits 8-11, so OWB becomes 0). ESP-IDF restores PS
     verbatim. Consider doing the same.
3. **SD mount is flaky after a chip reset** (7 / 1 / 0 across runs). The card
   stays powered; CMD0 may not be recovering it from a data-transfer state.
4. **Audio (I2S + GDMA) not started.** GDMA trigger ids: I2S0 = 3, I2S1 = 4,
   SPI2 = 0, SPI3 = 1. Descriptors must be in internal SRAM (20-bit register
   based at 0x3FC00000); buffers may be in PSRAM with explicit cache
   maintenance.
5. **Bring-up scaffolding still present**: `test_pattern()`, `probe_control_lines()`,
   `boot_trace` calls in filetype/dircache paths, the RETW probe.
6. **SPI is at 20 MHz polled**; the reference firmware uses 80 MHz with DMA.

---

## 8. Uncommitted changes

`git status` at HEAD `6bbab13605` shows modifications to:

```
AGENTS.md
apps/filetypes.c                 (boot markers '1'..'5' around filetype_init)
apps/main.c                      (boot markers, tick_task_thread)
firmware/asm/xtensa/thread.c     (store/load_context, xt_ctx_hist instrumentation)
firmware/common/dircache.c       (yield markers)
firmware/export/config/meowkits3.h (USB OTG defines removed)
firmware/target/xtensa/backlight-xtensa.c
firmware/target/xtensa/bootlog.c
firmware/target/xtensa/gpio-xtensa.h
firmware/target/xtensa/i2c-xtensa.c
firmware/target/xtensa/i2s-xtensa.c
firmware/target/xtensa/lcd-xtensa.c
firmware/target/xtensa/pca9557.{c,h}
firmware/target/xtensa/sdmmc-xtensa.c
firmware/target/xtensa/spi-xtensa.{c,h}
firmware/target/xtensa/st7789.c
firmware/target/xtensa/system-xtensa.c
firmware/target/xtensa/usb-xtensa.c   (stub)
firmware/target/xtensa/vectors.S      (RETW probe + window instrumentation)
```

Display fixes already landed and worth remembering:
- `MS_DLEN` must be the **exact** bit count (`n*8 - 1` for the actual byte count),
  not rounded up to 32-bit words — padding corrupts CASET/RASET argument parsing.
- `RAMWR` (0x2C) must be sent before pixel data; without it the panel discards
  everything (this was the "random noise" symptom).
- `MADCTL` = 0xB0, not 0xB8 (LovyanGFX rotation entry 3 = MAD_MV|MAD_MY|MAD_ML;
  `rgb_order=false` contributes no BGR bit).
- LCD CS is asserted once in `st7789_init()` and never toggled (the reference
  firmware has `pin_cs = -1`; CS goes through the PCA9557 expander and toggling it
  costs ~480 I²C transactions per frame).

---

## 9. Immediate next actions

**Follow section 0 (the plan).** Concretely, right now:

1. Extract ESP-IDF's esp32s3 MSPI/PSRAM HAL and the timing-tuning table, and
   rewrite `firmware/target/xtensa/psram-xtensa.c` to follow it, sweep included.
   Test with the bulk-write probe already in `lcd_init` (markers `E`..`F`).
2. Only after PSRAM bulk writes are reliable, move the level-1 handler to
   ESP-IDF's `_xt_lowint1` / `_xt_context_save` / `_xt_user_exit`.
3. Then audio.

Legacy items still worth doing:

1. Set `#define XT_RET_PROBE 0` in `vectors.S` and remove the marker code.
2. Change `store_context`'s PS mask from `-16` to `-17` (clear EXCM too).
3. Consider restoring PS verbatim at the handler exit like ESP-IDF, instead of
   masking OWB/CALLINC.
4. Re-flash, run `/tmp/probe7.py`, and check whether boot gets past
   `read_viewers_config_init` (markers `dom12L` in the boot log).
5. Then: SD robustness, then audio.

---

## 10. Session update: boot unblocked, one ABI question left

### What was wrong with the previous conclusion

The claim in §6b that "the ESP-IDF stage-2 bootloader never reaches crt0" was
**an artefact of a bad probe address**. The marker was `0x50000D60`, which
`boot_trace_start()` deliberately zeroes. Re-probing at `0x50000D64` (the stage
letter) shows crt0 runs to completion under the stock bootloader.

The real reason the board was boot-looping was self-inflicted:
`ESP_STAGE2_BOOTLOADER` was `1` in `meowkits3.h` while the image was flashed at
`0x0` under the mask ROM. The mask ROM does not arm the IROM MMU entries, so the
first call into `.text` faults, the bring-up trap loops, and the watchdog resets.
`ESP_STAGE2_BOOTLOADER` is now `0` and the mask-ROM path is the one in use.

### Fixed this session

1. **crt0's window pin was a regression and has been removed.** It wrote
   `WINDOWSTART=0`, `WINDOWBASE=0`, `PS=0x40000`. With no window marked live, no
   overflow ever fires, no base-save area is ever written, and `retw` reads an
   uninitialised frame. Both loaders leave a consistent state; leaving it alone
   is correct. See AGENTS.md §11.
2. **crt0 now masks interrupts** (`wsr intenable, 0`) before installing the bring-up
   vectors. ESP-IDF's stage-2 loader leaves `INTENABLE` set, and a stray level-1
   interrupt landing in `diag_trap` looked exactly like an IllegalInstruction.
3. **The tick slot was never unmasked.** `system_init()` zeroes `INTENABLE`; only
   the GDMA slots were re-enabled. `tick_start()` now sets bit `XT_SLOT_TIMERG1`.
4. **The asm tick handler incremented the wrong global** (`xt_exc_cause` via a
   stale literal) instead of `current_tick`. It now bumps `current_tick` and
   mirrors it into the RTC live block.

With those, on the mask-ROM path: crt0 reaches stage `G`, PSRAM and the MMU come
up, the SD card initialises (`CMD41` OCR `0x00ff8000`), the scheduler runs
(`switch_thread` iterating, threads sleeping/waking) and a full 320x240 frame is
pushed over SPI (4842 chunks). No exception. The panel is lit but shows black —
not yet investigated.

### The one open question

Enabling the tick makes things *worse*: the run dies early with a corrupted
window. The mechanism is that the tick handler's `rfe` sets `PS.EXCM=1`, and on
this core config `ENTRY` then faults.

Measured in isolation (minimal image, `callx8` into a callee that does
`entry a1,48` … `retw.n`):

- `PS = 0x60520` (EXCM=0, what the loader leaves) → callee runs, returns.
- `PS = 0x60530` (EXCM=1) → callee never executes. `WSR.PS` really does set the
  bit (read back `0x60530`), so this is not a silently ignored write.
- Fails at every recursion depth including 1, with `WINDOWSTART` forced to
  `0x1 / 0xf / 0xff / 0xffff`, and with the window handler bodies placed inline
  at the vector offsets exactly as ESP-IDF's `xtensa_vectors.S` does rather than
  behind a `j` trampoline.

So the windowed ABI's prerequisite (EXCM=1) appears unreachable with the window
state the loader hands us — yet earlier builds of Rockbox ran for tens of
thousands of ticks, which suggests EXCM=1 *is* reachable in the real firmware and
something else about the minimal test is wrong. That contradiction is the thing
to resolve next.

### Suggested next step

Stop hand-rolling the interrupt and context-switch paths and port ESP-IDF's
verbatim, as §4 already outlines:

- `_xt_lowint1` (frame alloc + `_xt_context_save` + dispatch),
- `_xt_context_save` / `_xt_context_restore` with `SPILL_ALL_WINDOWS`,
- `_xt_user_exit` as the single resume path, so the switch ends in `rfe` and
  `PS.EXCM` is established by hardware rather than by `wsr ps`.

Rockbox's `switch_thread()` currently calls `thread_store_context()` and
`thread_load_context()` as ordinary C functions and returns through the
compiler's `retw`, which is why the saved window state can never be made
consistent. FreeRTOS enters the scheduler through a syscall exception instead,
so the hardware captures `a0/a1/PS/PC` as a coherent frame — that is the shape to
move to.

---

# Design note: the ESP32-S3 codec/plugin load window

Status: **proposed, not implemented.** Written up because it is the only
remaining blocker for audio, and because it is the first change in this port
that would touch shared Rockbox core files.

## The problem

Rockbox's loadable-image model assumes one address serves both roles: a loaded
image's `load_addr` is where you *write* it, where you *read* its header, and
where you *execute* it. `apps/codecs.c` and `apps/plugin.c` enforce that by
rejecting any image whose `load_addr` is not exactly `codecbuf` / `pluginbuf`.

ESP32-S3 is the one target where that assumption fails. Its external-RAM cache
window is addressed through two ranges that share MMU entries and differ by
exactly `0x06000000`:

| window | base | legal operations |
|---|---|---|
| data cache (DROM) | `0x3C000020` | load / store |
| instruction cache (IROM) | `0x42000020` | fetch |

Each cache block only decodes its own range. Both were measured on this board,
not inferred:

- image linked and copied at `0x3c680020`, entered there →
  **`InstructionFetchError`**, `PC == EXCVADDR`, the first instruction never fetched
- buffers moved to `0x42680020` → **`LoadStoreError`** at `pluginbuf + 0x1c`
  during boot, i.e. the first ordinary data access

So no single base address can work, and no Kconfig, linker expression or MMU
programming changes what the silicon decodes. `CONFIG_SPIRAM_XIP_FROM_PSRAM` is
already enabled and does work for the firmware's own `.text`, but IDF only
re-maps ranges that existed at link time, so it cannot help a runtime-loaded
image.

## Why this cannot be solved entirely outside core files

The translation has to happen where a load address is used *as a pointer*. That
is `lc-rock.c` (bounds check, the `read()` target, and `lc_get_header()`), plus
the two `load_addr != buf` comparisons. A target-side shim cannot intercept
those without duplicating the loader.

## The change, and why it is inert elsewhere

One macro, defined once:

```c
#if defined(CPU_XTENSA)
/* Images are linked and executed through the instruction window but filled and
 * read through the data window: same pages, 0x06000000 apart. */
#define LC_DATA_ADDR(a)  ((unsigned char *)((uintptr_t)(a) - 0x06000000))
#else
#define LC_DATA_ADDR(a)  (a)
#endif
```

Applied at roughly eight sites in three files:

| file | sites | what |
|---|---|---|
| `firmware/lc-rock.c` | 3 | bounds check, `read()` target, `lc_get_header()` |
| `apps/codecs.c` | 2 | `load_addr != codecbuf`, `end_addr > codecbuf + CODEC_SIZE` |
| `apps/plugin.c` | 2 | the same two for `pluginbuf` |

`codecbuf` / `pluginbuf` stay **data-window** symbols, so every existing use as a
buffer — the codec heap at `&codecbuf[codec_size]`, `bufread()`,
`lc_open_from_mem()`, `plugin_get_buf()` — is untouched. Only the image's own
addresses are translated, and `lc_open` still returns `hdr.load_addr`, the
fetchable address, which is what the entry point needs.

On every non-xtensa target `LC_DATA_ADDR` is the identity, so the preprocessor
output and the generated code are **byte-identical to today**. That is the
guarantee worth having: not "this should be safe on other targets" but "there is
nothing there to be unsafe".

The link side needs no core edit at all: `apps/plugins/plugin.lds` already
provides `#ifndef CODEC_ORIGIN /* targets can specify another origin */`, which
is the sanctioned hook for exactly this.

## Architectural rationale, in one paragraph

The invariant "`load_addr` is simultaneously a writable buffer and an executable
address" is not a Rockbox design choice so much as an assumption that held
because every target Rockbox has shipped on had a single flat, read-write-
executable RAM or flash window for loadable images. ESP32-S3's split cache
windows are the first counterexample. Encoding the difference as a target-
conditional address translation states the new property where it belongs, keeps
the invariant intact everywhere it is still true, and leaves the shared loader
logic — bounds checking, header validation, entry-point dispatch — completely
untouched.

## Risks to check when implementing

1. **Does writing through `0x3c…` while executing from `0x42…` stay coherent?**
   Same physical pages, but two cache blocks. `lc_open` already calls
   `commit_discard_idcache()` after the copy; that may need to become a
   write-back on the data side plus an invalidate on the instruction side.
2. **Codec `.data` / `.bss`** live in the same image, so the codec reads and
   writes its own data through `0x42…` addresses baked in at link time. If the
   D-cache cannot reach that range, the codec faults on its first global access
   even after the entry point is reached. **This is the most likely reason the
   plan fails**, and it would mean the real answer is splitting the image into an
   executable segment and a data segment — a linker-script change in
   `plugin.lds`, which is target-gated already.
3. `CODEC_SIZE`/`PLUGIN_BUFFER_SIZE` regions must stay clear of `.ext_ram.bss`;
   the `rb_ext_ram_end` check covers that, but it must compare against the
   **data-window** alias or it proves nothing.

Recommendation: implement, and treat risk 2 as the likely outcome. If it bites,
the segment split is the follow-on, and it is still target-only work.

## Update: risk 2 resolved, and it splits the problem in two

The boot-time `LoadStoreError` at `pluginbuf + 0x1c` was resolved against a
matching ELF:

```
skin_get_gwps -> skin_load -> skin_data_load -> skin_parse
  -> skin_parse_viewport -> skin_alloc_element      <-- faulted
```

So `pluginbuf` is not only a plugin image slot. `plugin_get_buf()` hands it out
as the **allocation arena for the WPS and skin parser**, which means core UI
code reads and writes it as data constantly. It can never live behind the
instruction window.

That is good news in disguise, because the two buffers do not have to share a
base:

| buffer | used as data by | can move to `0x42…`? |
|---|---|---|
| `codecbuf` | `apps/codecs.c` only (a handful of sites) | **yes** |
| `pluginbuf` | `apps/plugin.c` **and** the skin/WPS parser via `plugin_get_buf()` | **no** |

So **audio can be fixed without touching plugin support at all**, and plugins
remain a separate, later problem that genuinely needs the segment split.

The macro should therefore be range-conditional rather than unconditional, so
that `pluginbuf` and every skin allocation through it are provably untouched:

```c
#if defined(CPU_XTENSA)
/* Only addresses in the instruction window need translating; data-window
 * addresses -- which is what pluginbuf and all skin arenas are -- pass
 * through unchanged. */
#define LC_DATA_ADDR(a) \
    ((uintptr_t)(a) >= 0x40000000u \
        ? (unsigned char *)((uintptr_t)(a) - 0x06000000) \
        : (unsigned char *)(a))
#else
#define LC_DATA_ADDR(a)  (a)
#endif
```

Revised scope: `firmware/lc-rock.c` (bounds check, `read()` target,
`lc_get_header()`) and `apps/codecs.c` (the two `codecbuf` comparisons).
`apps/plugin.c` needs **no change**.

## Update: there is a non-core route after all — a target-supplied loader

`firmware/SOURCES` selects the loader by `CONFIG_BINFMT`:

```
#if CONFIG_BINFMT == BINFMT_ROCK
lc-rock.c
```

and hosted targets already replace it wholesale -- `target/hosted/lc-unix.c`,
`target/hosted/game_console/3ds/lc-3ds.c`. So a target supplying its own
`lc_open()` is existing Rockbox practice, not a new mechanism invented for
ESP32-S3.

That is a better shape than the `LC_DATA_ADDR` macro, because the window split
is a property of *how this target loads images*, and it belongs in a target file
rather than threaded through shared code. It also turns out to be necessary
rather than merely tidy: a single contiguous `read(fd, hdr.load_addr, copy_size)`
cannot fill two disjoint windows at all, so the loader genuinely has to become
segment-aware on this target. A translation macro would not have been enough.

Plan:

- `firmware/target/xtensa/lc-xtensa.c` -- a loader that copies the executable
  segment to the instruction window and the data/bss segments to the data
  window, and returns a handle whose header is read through the data window.
- `apps/plugins/plugin.lds`, xtensa branch only -- place `.text` in the
  instruction region and `.data`/`.bss` in the data region. It already carries
  per-arch `#if` branches and an `IRAM` region for exactly this kind of split.
- `firmware/SOURCES` -- exclude `lc-rock.c` for xtensa and add the target file.
  SOURCES is preprocessed per target and is already full of `#if` on CPU and
  CONFIG, so this is idiomatic for that file.
- `memory-xtensa-syms.S` -- `codecbuf` becomes the instruction-window base, with
  a data-window companion for the segments that need it.

`apps/codecs.c`, `apps/plugin.c` and `firmware/lc-rock.c` are **untouched**, so
no other target can be affected even in principle.

Open question this raises, worth answering before writing code: `pluginbuf` is
the skin arena and must stay in the data window, but plugins also execute. So
the same segment split has to apply to the plugin region too, or plugins keep
their current broken state. Splitting the two problems is still the right order
-- codecs first, plugins as a follow-on -- but the linker work is common to
both, so it is worth designing the `plugin.lds` change once.

## Settled: the segment split is required, and here is the proof

The `-mno-longcalls` step was real progress and is worth keeping -- it took the
codec from "faults on the first internal call" to "runs `CODEC_LOAD` to
completion and 16 bytes into `run_proc`". But it cannot finish the job, and the
disassembly of `run_proc`'s prologue says why:

```
3c6800b6:  call8  3c684a18 <setjmp>          PC-relative, aliased correctly
3c6800b9:  bnez   a10, ...                   <- A0 = 0x826800b9, our return addr
```

`setjmp` itself then does:

```
l32r  a4, __xtensa_libgcc_window_spill       -> 0x3c6849b0  (absolute)
callx8 a4                                    -> fetch from the DATA window
```

`-mno-longcalls` removes longcall *stubs*. It does nothing about `l32r`+`callx8`,
because an indirect call has to materialise an absolute address somewhere. Every
such address in the image is the address it was linked at, and that is the data
window.

It is not only `setjmp`. The literal pool in the same prologue holds four more
absolute **code** addresses:

| literal | target |
|---|---|
| `0x3c680094` | `read_handler` |
| `0x3c680098` | `initial_seek_handler` |
| `0x3c68009c` | `tell_handler` |
| `0x3c6800a0` | `close_handler` |

These are the codec's callbacks, handed to the firmware through the codec API.
So even with `setjmp` hand-patched, the first `(*api->read_data)()` from firmware
would fault identically. **There is no pointer-level fix that survives this**,
because the addresses are inside the image and there are arbitrarily many of them.

Therefore the only correct shape is the one the design note predicted: link the
codec's **code into the instruction window** and its **data into the data
window**, so that absolute code literals are fetchable and absolute data
literals are loadable, each in the window that actually serves them. That is a
segment split, it changes the file shape, and it means the codecs get rebuilt
and re-copied.

Two things this rules out, so nobody retries them:

- aliasing individual pointers (done -- works for the two header fields, cannot
  reach into the image)
- `-mno-longcalls` as a complete fix (helps, is necessary, is not sufficient)

## Three different things I nearly merged into one

The filesystem hang turned out to be three separate behaviours, and conflating
them is why the diagnosis took so long. Recording them separately:

**1. dircache holding the writer lock is correct behaviour, not a bug.**
`dircache_thread()` takes `dircache_lock()` at firmware/common/dircache.c:2056
and holds it across cache builds -- the in-source comment says *"calls made
within the loop reopen the lock"*. Readers legitimately queue behind it. On a
card with a great many files a build takes a long time and the UI stalls for
that duration. That is slow, not deadlocked, and it was observed to recover on
its own. A reading of `owner=dircache waiting=[main(R)]` is therefore expected
under load and is not evidence of anything.

**2. `abandon ship! dircache` is a real bug.** It is
`thread_panicf()` at firmware/kernel/thread.c:1410, reached from `thread_exit()`
when the exiting thread's priority-distribution mask still has entries -- the
kernel's own comment: *"otherwise threads are waiting on an abandoned object"*.
Note that `dircache_unlock()` at dircache.c:2107 *is* on the suspend path, so
this is about `pdist`, not about the mrsw count. dircache terminates while the
kernel still believes it owns something.

**3. A free lock with permanent waiters is a real bug, and a different one.**
Measured directly:

    FSLOCK: count=0 owner=(none) waiters=2
    main=blk@0x3fcb6bbc
    power=blk@0x3fcb6bbc

`0x3fcb6bbc` = `file_internal_mrsw` (0x3fcb6bb0) + 0xc = `&mrsw.splay.blocker`,
resolved from the symbol table. The lock reports itself free and unowned while
two threads remain queued on its blocker forever. That cannot be a long build --
the lock is not held by anyone. It is a missed handoff, and it belongs to the
same family as the `corep->running` fix: Rockbox's scheduler bookkeeping and the
wakeups the IDF task model actually performs disagreeing.

Distinguishing test between (1) and (3): does it recover if left alone? (1)
does; (3) never will.

## Instrumentation lessons

- `who :`/`hist:` were gated on `count != 0`, which suppresses output for
  precisely state (3) above -- a free lock with live waiters. Fixed to
  `count != 0 || waiters != 0`. Two rounds of "the ring is silent" were this
  gating error, not absent data.
- `mrsw_debug_report()` now labels each waiter `W` or `R` from `retval`, which
  `mrsw_read_acquire()`/`mrsw_write_acquire()` set to 1 and 0 respectively just
  before blocking. That separates "a writer was never handed the lock" from
  "everyone is merely behind one".
- `thread_debug_dump()` walks `__threads[]` and prints `name=run` or
  `name=blk@<blocker>`. Two threads naming the same blocker address are queued
  on the same object, which is what identified (3). Cross-referencing it against
  the lock's own counters is what produced the contradiction -- neither readout
  alone was sufficient.
- Cosmetic defect still open: the `playlist` thread prints without its status
  suffix. Harmless for reading states but it should be fixed before this output
  is trusted for anything subtle.

## Root cause of the play hang: a woken thread is not a reachable thread

Three readouts, taken in the same instant while the UI sat at "Loading...":

    FSLOCK: count=-1 owner=buffering waiters=2
    who : owner=buffering/running waiting=[main(W), playlist cachectrl(W)]
    THREADS: main=blk@0x3fcb6bbc ... codec=run audio=blk@0x3fca5684 buffering=run

`buffering` holds the filesystem writer lock and reports `/running` -- its
`blocker` is NULL, so Rockbox believes it should be executing. It never resumes.
Both waiters are `(W)`, i.e. queued as writers, so nothing can slip past.

The reason is structural, and it is in `firmware/kernel/thread.c:422`:

```c
static NO_INLINE void core_schedule_wakeup(struct thread_entry *thread)
{
    core_rtr_add(corep, thread);
#if NUM_CORES > 1
    if (core != CURRENT_CORE)
        core_wake(core);
#endif
}
```

On a single-core build, waking a thread adds it to the runnable list and
notifies *nobody*. `core_wake()` is declared in kernel/include/thread.h:136 but
is compiled out unless NUM_CORES > 1, and this target does not define it.

Meanwhile the whole port contains exactly one `xSemaphoreGive`:
target/xtensa/thread-idf.c:163, inside the voluntary handoff. So a gate is given
only when a currently-running thread happens to call `switch_thread()` *and* the
kernel then selects that particular thread.

For upstream Rockbox that is correct: the arch switch is a context swap the
kernel performs, so "eligible at the next voluntary switch" and "will run" are
the same statement. Under the gated-task model they are not. A woken thread's
FreeRTOS task is in the Blocked state pending on its gate, and no amount of
Rockbox bookkeeping can dispatch it. The chain is then self-sustaining: the
unreachable thread holds the writer lock, and every thread that could have
reached a `switch_thread()` capable of handing it the gate is queued behind it.

This is the same family as the `corep->running` bug fixed earlier -- both are
places where the port assumed FreeRTOS and Rockbox agreed about who is running
when in fact only one of them can dispatch.

## The fix, and the trap in it

The port needs an *eager* wake: when a thread is added to the RTR list, its gate
must be given. There is no single-core hook for that today, so it needs a small
gated call in `core_schedule_wakeup()` to a target-supplied function; the target
can map a `thread_entry` to its gate because `rb_thread_ref` already carries a
`thread` pointer (added with the `corep->running` fix).

**The trap:** the gate cannot become a "you are now running" signal, because
eager gives and the existing handoff gives would race, and two Rockbox threads
would believe they are the running one simultaneously -- which breaks the single
invariant the whole cooperative scheduler rests on. The gate has to be a *hint
to re-check*, with the task verifying against Rockbox's own state before
proceeding and returning to pending if it is not the selected thread. A binary
semaphore makes surplus gives harmless (it saturates at 1), which is what makes
that shape viable.

Worth noting before anyone implements it: this is the first change in the port
that needs a new hook in shared kernel code rather than a target file. The
rationale is above; it must be gated so no other target compiles a different
instruction, and the honest framing is that the port changed the dispatch
contract and therefore owes the kernel a notification the old contract did not
need.

## Dead scaffolding to remove

`switch_thread()` begins with `#ifdef RB_DIAG_NO_SWITCH ... return; #endif` -- a
bisect leftover that is not defined anywhere, but is a `return` at the top of the
scheduler's entry point. It should go before this tree is shown to anyone.
