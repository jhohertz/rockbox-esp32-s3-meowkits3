# USB sprint — ESP32-S3 MeowKit

Investigation notes before any code. Everything marked **verified** was read out of
the tree or out of ESP-IDF; anything marked **hypothesis** has not been measured.

## The verdict up front

**Use Rockbox's own stack.** ESP-IDF 5.5 ships no device-side USB stack at all:
`components/usb/` is host-only (`hcd_dwc.c`, `usb_host.c`, `usbh.c`, `hub.c`), and
TinyUSB is not present locally (only the Arduino wrapper
`cores/esp32/esp32-hal-tinyusb.c` exists; the library itself is not in the package).
Rockbox already has a DWC2 device driver — `firmware/drivers/usb-designware.c` —
which implements the `firmware/export/usb_drv.h` contract (`usb_drv_init`,
`usb_drv_ep_allocate`, `usb_drv_send*`, `usb_drv_recv*`). So the work is adapting
seams, not writing a stack.

**The register layouts agree.** Rockbox's `usb-designware.h` computes:

| Rockbox macro | offset | ESP32-S3 (`usb_struct.h`) |
|---|---|---|
| `DWC_DIEPCTL(x)` | `OTGBASE + 0x900 + 0x20*x` | `in_ep_reg[7]` at `0x0900..0x09e0` |
| `DWC_DOEPCTL(x)` | `OTGBASE + 0xb00 + 0x20*x` | `out_ep_reg[7]` at `0x0b00..0x0be0` |
| `DWC_DIEPTXF(x)` | `OTGBASE + 0x104 + 4*x` | `dieptxf[4]` at `0x0104..0x0114` |
| `DWC_DIEPMSK` / `DOEPMSK` | `0x810` / `0x814` | same |

The differences are in bring-up, not in the endpoint arithmetic.

**The silicon is proven on this board.** The stock MeowKit firmware builds a device
stack on the same pads: `platformio.ini` sets `-DARDUINO_USB_MODE=1` (OTG, i.e. the
DWC core) in one environment and `-DARDUINO_USB_MODE=0` with
`-DARDUINO_USB_CDC_ON_BOOT=1` (USB-Serial-JTAG) in the other, and it ships
`src/system/usb_msc.cpp` and `src/system/usb_hid.{h,cpp}`. Pins are native and not
remappable: `HAL_PIN_USB_DP 20`, `HAL_PIN_USB_DN 19`.

## Endpoint budget — both requested profiles fit, exactly

Measured from the core itself at boot, not from headers:

| What | IDF's cfg header | The silicon says |
|---|---|---|
| device endpoints | `OTG_NUM_EPS 6` | **7** (`hwcfg2=0x224dd930` → numdeveps 7; `usb_struct.h` also declares `in_ep_reg[7]`/`out_ep_reg[7]`) |
| dedicated TX FIFOs | `OTG_NUM_IN_EPS 5` | **4** (`hwcfg4=0xd3f0a030` → GHWCFG4[29:26] = 4) |
| FIFO RAM | `OTG_DFIFO_DEPTH 256` | **200 words** (`hwcfg3=0x00c804b5` → GHWCFG3[31:16] = 200) |
| FS PHY interface | `OTG_FSPHY_INTERFACE 1` | **1** ✓ agrees |
| HS | — | **0** ✓ full-speed only |

The TX-FIFO count is the real ceiling: `usb_dw_check_hw()` clamps `n_ptxfifos` to
4 whatever the endpoint count is, and EP0's IN FIFO is the non-periodic one, so
**four IN endpoints beyond EP0** is the limit, not seven.

Rockbox class demand, read from each driver's `ep_allocs`:

| class | endpoints |
|---|---|
| serial (CDC ACM) | 3 — bulk IN, bulk OUT, interrupt notification |
| storage (MSC) | 2 — bulk IN, bulk OUT |
| hid | 1 — interrupt IN |
| audio | 2 — isochronous OUT, isochronous feedback IN |

- `CDC + HID + MSC` = 3 + 1 + 2 = **6** endpoints, of which **4 are IN** ✓ fits
  both the 7-endpoint pool and the 4 dedicated TX FIFOs, exactly
- `CDC + HID + Audio` = 3 + 1 + 2 = **6** endpoints, of which **4 are IN** ✓ same
- `CDC + HID + MSC + Audio` = 8 ✗ — does not fit on either count

So the two profiles must be mutually exclusive, and there is no room for a fourth
class. The IN-endpoint ceiling means a profile cannot be built by trading OUT
endpoints for IN ones either. `usb_core.c` handles overflow by marking the driver `error` and dropping it
from the descriptor rather than failing the build, so an over-subscribed profile
degrades silently — the settings must not offer it (see the caveat below).

## Profiles are already expressible without touching the core

`firmware/usb.c::usb_configure_drivers()` composes the configuration from settings:

- `USB_INSERTED`: MSC **forced true**, HID = `usb_hid`, SERIAL = `usb_serial`,
  AUDIO = `usb_audio == 1 || 3` ("always" or "only in mass-storage mode")
- `USB_POWERED`: MSC false, HID **true unless `USB_ENABLE_CHARGING_ONLY` is
  defined**, SERIAL = `usb_serial`, AUDIO = `usb_audio == 1 || 2`
  ("always" or "only in charge-only mode")

Mapping the requested profiles onto that:

| profile | settings | resulting state |
|---|---|---|
| CDC+HID+MSC | `usb_mode = MASS_STORAGE`, `usb_serial` on, `usb_hid` on, `usb_audio = 2` | `USB_INSERTED`, audio off |
| CDC+HID+Audio | `usb_mode = CHARGE`, `usb_serial` on, `usb_audio = 2` | `USB_POWERED`, MSC off, HID on |

The second one depends on **not** defining `USB_ENABLE_CHARGING_ONLY`, which
`config.h` only does when the controller lacks interrupt support. DWC2 gives
`USB_HAS_INTERRUPT`, so we get `USB_ENABLE_HID` and not charging-only — the profile
works as-is.

**Caveat to decide in the sprint:** `usb_audio == 1` ("always") in `USB_INSERTED`
requests MSC *and* audio, which is 8 endpoints. Either restrict the audio setting
values for this target or accept the silent drop. Do not let the settings offer a
profile the hardware cannot build.

## What `meowkits3.h` must declare

- `HAVE_USBSTACK` and a `CONFIG_USBOTG` value. `config.h` keys both the class-driver
  list (1399-1424) and the usb_core variant (1057-1076) off `CONFIG_USBOTG`.
  `USBOTG_DESIGNWARE` yields exactly `USB_HAS_BULK` + `USB_HAS_INTERRUPT` +
  `USB_HAS_ISOCHRONOUS`. Decide whether to reuse it or add a distinct value so we do
  not inherit DesignWare-specific SoC assumptions — check every `#elif` that tests
  `USBOTG_DESIGNWARE` before choosing.
- `USB_NUM_ENDPOINTS 7`. The driver also reads the core's own count at runtime
  (`hw_numeps` from `DWC_GSNPSID`) and logs it, so verify the compile-time cap
  against what the silicon reports before trusting either.
- `#define USB_ENABLE_SERIAL` explicitly. **No target in the tree enables it** —
  every occurrence in `config/*.h` is commented out — so `usbstack/usb_serial.c` has
  very likely bit-rotted. Compiling it is step 0 and may itself be work.
- `HAVE_USB_AUDIO` plus the settings exposure (`usb_set_audio()` already exists in
  `firmware/usb.c`).
- Drop `USB_NONE`, and un-guard `usb-xtensa.c`.

## The seams the target has to bridge

1. **Clock, base and pads — done, and the header it was written against is wrong
   for this chip.** `SOC_DPORT_USB_BASE` is *referenced* by IDF's esp32s3
   `usb_reg.h` but never defined there; that file is legacy. The base is
   `0x60080000` and it is mapped (reads do not fault). What was missing is that
   **there are two clock gates**: `usb_clk_en` (PERIP_CLK_EN0 bit 23) is the wrap
   and PHY domain, `usb_device_clk_en` (PERIP_CLK_EN1 bit 10) is the DWC core
   itself — and at boot **both blocks are also held in reset**
   (`PERIP_RST_EN0=0x9eff1f3c`, `PERIP_RST_EN1=0x000003ce`). Enable both gates and
   clear both resets, or every OTG register reads `0x00000000` and nothing says
   why. `usb_wrap` carries `CLK_EN` (31), `PHY_CLK_FORCE_ON` (20),
   `AHB_CLK_FORCE_ON` (19), `DFIFO_FORCE_PU` (22) and `DFIFO_FORCE_PD` (3): the
   force bits are pad-safe, the two mux bits (`USB_PAD_ENABLE` 18, `PHY_SEL` 2)
   are not.
2. **PHY bring-up — resolved, and the premise was wrong.** This core has no
   classic PHY-type field: `GUSBCFG` carries `toutcal`(2:0), `phyif`(3),
   `ulpiutmisel`(4), `fsintf`(5), `physel`(6), confirmed against IDF's generated
   `usb_dwc_struct.h`. The FS PHY is selected by **`fsintf`**, and the core reports
   it in `GHWCFG2.fsphytype` (measured 1). `usb_dw_check_hw()` compared the whole
   register against a classic phytype constant, and `GUSBCFG`'s reset value is not
   zero (measured `0x1440`: `physel` and `usbtrdtim` are preset), so that comparison
   could only ever fail — it reported "PHY type not supported" on a PHY that is
   present and selectable. The xtensa branch now checks the capability and the one
   bit, and the probe verified the bit is writable (`gusbcfg 0x1440 → 0x1460`,
   restored afterwards). Nothing else in the driver needed touching: its offsets and
   its `TRDT`/`FDMOD`/`FSINTF` bit positions already agree with IDF's layout exactly.
3. **Interrupt.** Rockbox expects `usb_drv_int()` called from the controller's
   handler. Wire `esp_intr_alloc` at LEVEL1 to it. Two lessons from the audio path
   apply unchanged: nothing may print in that handler, and the handler must be
   IRAM-resident or it stalls behind PSRAM traffic. IRAM has room (the TX ISR
   experiment measured 1107 B against a 358 KB region) but IRAM text **costs heap**,
   of which roughly 64 KB remains.
4. **`usb_detect()`.** The VBUS monitor thread is gone — it cost
   `DEFAULT_STACK_SIZE` of internal DRAM (4 KB) to maintain a value nothing polls
   while `USB_STATUS_BY_EVENT` is set. `vbus_sample()` reads the AXP173 on demand
   for the charging display, and `usb_detect()` stays a hard gate returning
   `USB_EXTRACTED` until the core can enumerate, because an insertion is Rockbox's
   request to unmount the card. Once the core is armed, presence should come from
   the OTG interrupt rather than from polling the PMU.
5. **`cpu_idle_mode()`.** `usb_slave_mode()` calls it on entry. Check what xtensa
   does today and decide whether `waiti` is acceptable with the tick still armed.
6. **Storage handoff — the riskiest non-USB piece.** `firmware/usb.c` does
   `disk_unmount_all(); storage_soft_reset(); storage_init(); storage_enable(false);`
   on entry and `usb_enable(false); storage_init(); disk_mount_all();` on exit. Our
   SD path is IDF `sdmmc_host`, with `sd_enable()` deliberately a no-op and
   `is_removable = false`. Whether the host can be de-initialised and re-mounted
   cleanly, and what dircache does across that (its limit is load-bearing), is
   unverified and is where MSC will actually cost time.

## The console conflict — decide this before touching the pads

Our trace **and** flashing both run on USB-Serial-JTAG
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y`). USJ
and OTG share GPIO19/20 through `USB_WRAP`. Routing the pads to OTG means:

- the console trace stops appearing on `/dev/ttyACM0`
- `esptool` cannot see the port while USB mode is active
- there is no fallback UART — GPIO1/2 on this board are I2C

Plan: switch the pads only while in USB mode and restore USJ on exit, so a reset
always returns the console; and consider routing Rockbox's debug output over the CDC
endpoint while in USB mode so observability survives the switch. Accept and document
that a hang *inside* USB mode needs a power cycle (or the AXP173 long-press).

## Order of work — each step verifiable without the next

0. **Compile only — done.** `usb_serial.c` compiles for the first time in the tree
   and the stack links inert. What broke was not bit-rot in the driver but a name
   collision: ESP-IDF's `esp32s3.rom.ld` assigns the absolute symbol `usb_enable` to
   a mask ROM routine, which silently beats Rockbox's target hook, so `usb_init()`
   called the ROM's download/console function and died with `LoadProhibited`.
   `-Wl,--wrap=usb_enable` is the fix; the collision list is in `AGENTS.md`.
1. **Probe only — done, without the pads.** Enabling the wrap gate alone made every
   register read zero; both gates and both resets are required. The core is present
   (`id=0x4f54400a`, rev 10) and reports **7 device endpoints, 4 dedicated IN FIFOs,
   200 words of FIFO RAM, FS PHY available and writable**. The probe never touches
   `USB_PAD_ENABLE` or `PHY_SEL`, so the console survives and the run repeats; it
   restores everything it read or forced.
2. **Enumerate one simple class** (HID alone). First host-visible device; validates
   PHY, attach and EP0 traffic. **This is the first step that needs the pads**, and
   therefore needs someone at the board: routing GPIO19/20 to the OTG core silences
   the console *and* `esptool`, and a hang inside USB mode costs a reset or a power
   cycle. Not an unattended step.
3. **CDC alone.** Host sees a serial port; move the trace onto it and confirm the
   pad handover round-trips back to USJ on exit.
4. **MSC.** This is where the storage handoff risk surfaces. Validate against the
   card, and validate the re-mount on exit.
5. **Composite CDC+HID+MSC**, then **CDC+HID+Audio**. Isochronous transfer on this
   core is untested territory — Rockbox's audio driver needs
   `USB_HAS_ISOCHRONOUS` and the DWC2 isochronous FIFO setup; expect to verify the
   FIFO budget (16 KB total) against what MSC and audio each want.
6. **Settings and profiles**, including the decision on the `usb_audio = 1` case
   that cannot fit.

## Discarded premise

`usb-xtensa.c` currently says "nothing on this target needs the OTG core" and
defines the whole file out under `USB_NONE`. That was true while the port could not
boot; it is the thing being replaced. The VBUS monitor inside it is worth keeping.

## The CDC/serial state, in detail

This is the part that changes how the sprint is run, because it is both the conflict
and the remedy.

**Rockbox has its own CDC ACM driver.** `firmware/usbstack/usb_serial.c`, 449 lines,
© 2007 Christian Gmeiner and **updated in 2021 by Tomasz Moń** -- so it is not the
ancient code the rest of this port inherited. It implements the CDC header / ACM /
union / call-management descriptors, `SET_LINE_CODING`, `GET_LINE_CODING`,
`SET_CONTROL_LINE_STATE` and the `SERIAL_STATE` notification on the interrupt
endpoint, plus a TX ring buffer and `usb_serial_send(data, length)`.

**Its designed consumer is `logf` -- Rockbox's own trace channel.** `firmware/logf.c`
flushes the log ring to the host:

```c
#ifdef USB_ENABLE_SERIAL
    ...
    usb_serial_send(logfbuffer + old_logfindex, logfindex - old_logfindex - 1);
    usb_serial_send("\r\n", 2);
#endif
```

and `apps/debug_menu.c` has a live entry, **"USB Serial driver (logf)"**
(`toggle_usb_serial()` → `usb.c:928` → `usb_core_enable_driver(USB_DRIVER_SERIAL, ...)`)
which toggles the driver at runtime. So "the console is a CDC stream" is an existing
Rockbox concept with existing UI, not something to invent.

**RX is primed but unconsumed.** `usb_serial_init_connection()` calls
`usb_drv_recv_nonblocking(EP_OUT, receive_buffer, RECV_BUFFER_SIZE)` and re-primes on
every completion -- and then drops the bytes. Nothing reads `receive_buffer`, and there
is no queue or callback exposed. So *serial-controlled Rockbox* needs one new seam: a
consumer hook in `usb_serial_transfer_complete()`'s OUT branch. It is small, and it
belongs in the driver rather than the target, because the driver is what owns the
buffer. Note Rockbox also has a UART notion (`firmware/export/serial.h`,
`serial_tx()`/`serial_tx_raw()`, used by `logf` under `HAVE_SERIAL` + `LOGF_SERIAL`),
which is a different thing and is not available on this board -- GPIO1/2 are the I2C
bus, and the USB-C connector carries no UART.

**Nothing builds `usb_serial.c` today.** `USB_ENABLE_SERIAL` is commented out in every
target config in the tree, and `firmware/SOURCES` gates the file on it. It is therefore
not compiled in any build farm target, which means its current state is unknown. Step 0
of the plan is exactly this: define the flag and compile it.

**The conflict is the pads, not the protocol.** Our console and our flashing are IDF's
USB-Serial-JTAG console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`,
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y`), and USJ shares GPIO19/20 with the OTG
core through `USB_WRAP`. Routing the pads to OTG silences the IDF console. Rockbox's
CDC driver is the replacement for exactly that, and it costs 3 of the 6 usable
endpoints -- which is why CDC-on constrains the rest of the profile.

**IDF's own CDC console is not a route.** `CONFIG_ESP_CONSOLE_USB_CDC` exists in
`esp_system/Kconfig:283`, but that implementation needs the TinyUSB gadget component,
which is not in IDF 5.5 and not present locally. `ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`
is a secondary-output option, not a way to reach the OTG core.

### The console plan for the sprint

1. Keep IDF's USJ console as the default, and switch pads to OTG **only** while USB
   mode is active. On reset the pads return to USJ, so `esptool`'s auto-reset keeps
   working without pressing BOOT -- the invariant worth protecting.
2. While in USB mode, route diagnostics through `logf` (which already flows to CDC)
   rather than adding a second printf path. That means enabling `LOGF_ENABLE` for this
   target, which is currently off; the trace then appears on the host's serial port.
3. Accept and document that a hang *inside* USB mode costs a reset or a power cycle,
   because there is no spare UART. Worth deciding whether Rockbox's CDC should also
   carry the bring-up trace from boot, which would trade console availability for
   always-on observability -- probably not, since flashing depends on USJ.
4. If serial control is wanted, add the OUT-side consumer hook in `usb_serial.c` and
   give it a small, honest contract: a byte stream, delivered to whatever the target
   registers. Do not invent a protocol in the driver.

### Already built, worth reusing

`firmware/target/xtensa/debug-xtensa.c` has an `INFO_USB` debug-menu row that reads
`XT_PERIP_CLK_EN0 & XT_USB_CLK_EN` (bit 23) to say whether the DWC core is clock-gated,
and otherwise reads the Synopsys signature at `OTGBASE + 0x40` (`GSNPSID`), noting that
`0x31304040` is the DWC2 family. `XT_USB_DWC_BASE` is defined as `OTGBASE` in
`firmware/include/xtensa.h`. That is step 1 of this plan already written -- it reports
on the display rather than the console, so the value should also reach `logf` when the
USB work starts, and the `hw_numeps` read from `HWCONF` is the number the endpoint
budget depends on.

## Static audit done against IDF's generated core layout (no board needed)

The register arithmetic was checked rather than assumed, because a wrong offset on
this core does not fault -- it writes into reserved space or reads zero.

- **Offsets.** All 48 fixed-offset `DWC_*` macros were matched against
  `usb_dwc_struct.h`'s generated layout. Eight of them name registers this core
  config does not implement (`GI2CCTL` 0x30, `GCCFG` 0x38, `CID` 0x3c, `GLPMCFG`
  0x54, `DTKNQR1/2` 0x820/0x824, `DEACHINT`/`DEACHINTMSK` 0x838/0x83c -- IDF
  marks those offsets reserved). The driver touches none of them except
  `DTKNQR1`, and that read lives inside `usb_dw_get_epmis()`, which is
  `#ifdef USB_DW_SHARED_FIFO` and compiled out here.
- **Bit positions.** 83 bit macros across 48 registers were checked against the
  field positions IDF's bitfield structs imply. Every one lands on a field the
  core has, except the two dead registers above. `TRDT` → bits 13:10, `FDMOD` →
  bit 30, `FSINTF` → bit 5, `PHYIF16` → bit 3 all agree.
- **Endpoint arithmetic.** `DIEPCTL` 0x900 / `DIEPINT` 0x908 / `DIEPTSIZ` 0x910 /
  `DIEPDMA` 0x914 / `DTXFSTS` 0x918 and `DOEPCTL` 0xb00 / `DOEPINT` 0xb08 /
  `DOEPTSIZ` 0xb10 / `DOEPDMA` 0xb14, stride 0x20 -- matches IDF exactly. Note
  `DOEPDMA` at `+0x14`, not the classic `+0x18`.
- **Paths that are compiled out, verified at object level.** `usb_dw_handle_rxfifo`,
  `usb_dw_try_push` and `usb_dw_get_epmis` are absent from
  `usb-designware.o`: all DFIFO access is under `USB_DW_ARCH_SLAVE`, and this core
  is architecture 2 (internal DMA, measured `(ghwcfg2>>3)&3 == 2`), so the FIFO
  window and its stride never matter. `DWC_DFIFO(x)`'s `0x1000` stride would be
  wrong on a 200-word FIFO RAM, but it is dead code here.
- **The TX-FIFO ceiling fails cleanly.** Allocation walks `fnum = 1..n_ptxfifos`
  and returns `false` when all four are taken, so a fifth IN endpoint gets no FIFO
  rather than silently sharing one. `usb_serial.c`'s notification IN is marked
  `.optional = true`, so CDC survives without line-state notifications if it is the
  one that cannot be allocated.
- **The endpoint pool excludes EP0.** `usb_core.c` assigns from `epnum = 1`, so
  `USB_NUM_ENDPOINTS 7` means six class endpoints -- exactly what both profiles
  need: CDC 2 IN + 1 OUT, HID 1 IN, MSC 1 IN + 1 OUT (6 endpoints, 4 IN) or
  CDC 2 IN + 1 OUT, HID 1 IN, audio 1 IN + 1 OUT (6 endpoints, 4 IN). Zero slack.
- **Enabling the serial driver cannot stall the console.** `usb_serial_send()`
  returns early while the class is inactive, so `logf`'s flush does not block on a
  target with no host attached.
- **The Audio profile links.** `dsp_configure` and the `usb_audio` entry points are
  in the image; the driver owns its own DSP pipeline and allocates its buffers from
  the core arena (PSRAM), so it needs nothing from the target's `audiohw` path.

## Risks still open before the first attach

1. **The DWC ISR runs from PSRAM.** `ICODE_ATTR` maps to nothing on this target
   (no `.icode` in the script), so the handler fetches through the instruction
   cache window while the mixer and codec traffic are hammering the same MSPI bus.
   Same class of problem as the audio ISR; the fix, if it shows up in measurement,
   is routing the ISR call tree into IRAM -- which costs heap, not free space.
2. **The pad handover is the irreversible step.** Taking GPIO19/20 removes the
   console *and* `esptool`; a hang inside USB mode costs a power cycle, and the
   only power control is the AXP173's PEK. Do it with someone at the board.
3. **Storage handoff is statically clean; the runtime is still unproven.**
   `usb_slave_mode(true)` does `disk_unmount_all()`, `storage_soft_reset()`,
   `storage_init()`, `storage_enable(false)`; the exit path does `usb_enable(false)`,
   `storage_init()`, `disk_mount_all()` and panics if the remount finds no partition.
   Every step was traced against this target:
   - `storage_soft_reset()` expands to `(void)0` for an SD-only target.
   - `storage_init()` for us is `sd_init()` plus `storage_thread_init()`. `sd_init()`
     returns early once `sd_ready` is set, and `storage_thread_init()` returns early
     once `storage_thread_id` is set, so re-init creates no second thread and costs
     no second stack. `HAVE_SDMMC_HOST` is *not* defined here, so
     `sdmmc_host_target_init()` -- which would re-initialise the host -- is not in
     the path at all.
   - `storage_enable(false)` reaches `sd_enable()`, which is a no-op. That is the
     right answer for MSC: the host stays armed so the mass-storage driver can keep
     serving sectors while Rockbox's filesystem is unmounted.
   - `storage_read_sectors()` dispatches to `sd_read_sectors()`, whose only
     precondition is `sd_ready`. There is no "volume must be online" gate, so
     unmounting does not block MSC.
   What remains unproven is the concurrent case: MSC serving sectors from the USB
   thread while the tick thread polls the touchscreen over I2C, and whether a host
   that resets the bulk endpoints mid-transfer leaves `sd_ready` consistent.
4. **`usb_mode` needs `HAVE_USB_POWER` to exist as a setting**, and that also
   pulls the charging indicator into the status bar -- a screen change that can
   only be judged on the board.

## Interrupt level: the USB ISR and the audio ISR are peers, not nested

Both are allocated `ESP_INTR_FLAG_LEVEL1` (`i2s-xtensa.c:807` for GDMA OUT channel 0,
`usb-xtensa.c:149` for the OTG core). On Xtensa, same-level interrupts do not nest,
so while the GDMA handler runs the USB interrupt is held pending, and the reverse.
That is acceptable rather than ideal, for a reason that is about *when* each is live:

- In the mass-storage profile Rockbox stops playback on entering USB mode (the
  volumes are unmounted), so GDMA is idle and the only contention is during
  enumeration, where control transfers have host-side timeouts measured in seconds.
- In the USB-DAC profile the codec output is USB, not the I2S sink, so GDMA again
  does nothing.
- The case that would actually hurt is isochronous USB audio (1 ms service interval)
  while I2S TX is also running. That combination is not reachable through either
  profile, because the DAC profile does not drive the I2S sink.

If measurement ever shows the GDMA handler's worst-case duration hurting USB (the
`isr ... worst=` field in `i2s_dma_line()` is the number to read), moving USB to
`ESP_INTR_FLAG_LEVEL2` is a one-line change: IDF's dispatcher supports levels 1-3,
and nothing in Rockbox's own vector handling is involved on this branch. Note that
neither handler carries `ESP_INTR_FLAG_IRAM` -- the GDMA one deliberately does not,
because it reaches code in the instruction-cache window -- so a higher level buys
latency, not safety.

## Measured configuration, resolved through the target preprocessor

Reading `autoconf.h` is not enough: it contains no USB defines at all, because
`tools/configure` puts the USB configuration in `firmware/export/config/meowkits3.h`
and `config.h` selects that header on `-DMEOWKIT_ESP32S3`, which is a command-line
define, not a macro in a generated file. The values below came from running the real
target compiler over a probe that `#include`s `config.h` with that define and the
harvested IDF include path.

| Macro | Value | Where it comes from |
|---|---|---|
| `HAVE_USBSTACK` | yes | `meowkits3.h` |
| `CONFIG_USBOTG` | 6401 = `USBOTG_DESIGNWARE` | `meowkits3.h` |
| `USB_NUM_ENDPOINTS` | 7 | `meowkits3.h`, justified by GHWCFG2 |
| `USB_ENABLE_SERIAL` | yes | `meowkits3.h` -- the only target in the tree that sets it |
| `USB_ENABLE_STORAGE` | yes | derived: `USB_HAS_BULK`, which DESIGNWARE sets |
| `USB_ENABLE_HID` | yes | derived: `USB_HAS_HID` |
| `USB_ENABLE_AUDIO` | yes | derived: `USB_HAS_ISOCHRONOUS` |
| `USB_ENABLE_CHARGING_ONLY` | no | not applicable to this stack |
| `USB_STATUS_BY_EVENT` | yes | implied by DESIGNWARE -- the target must push `usb_status_event()` |
| `HAVE_USB_POWER` | no | deliberately not yet: it pulls the charging display |
| `HAVE_HOTSWAP` | no | the card is not removable on this board |
| `USBMODE_DEFAULT` | defined, but unused | `meowkits3.h` sets charge mode; `usb.c` only reads it under `HAVE_USB_POWER` |

All four class drivers are compiled into the image. That is not the same as all four
being composed: composition is `usb_core_enable_driver()` at runtime, and the
defaults today are HID on, serial on, audio off (`usb_audio = 0`), storage off (no
mass-storage mode). So the configuration the core would present the moment
`USB_ALLOW_ATTACH` is turned on is **CDC + HID alone -- three endpoints, two of them
IN**, which is well inside the four dedicated transmit FIFOs.

That is the staging to use at the board, and it is better than starting from a full
profile:

1. **CDC + HID** (already the default composition): proves clocks, pads, PHY,
   enumeration, one control-transfer path and one interrupt-IN path. CDC also gives
   the console replacement, so the trace comes back over USB before any other
   feature does.
2. **add MSC**: one more IN, one more OUT. The only new risk is the storage handoff,
   which is traced above.
3. **or add Audio instead**: one isochronous OUT plus one feedback IN. Mutually
   exclusive with MSC, because both together need six IN endpoints and the core has
   four.
