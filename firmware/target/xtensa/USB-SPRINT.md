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

## GUSBCFG: what the driver writes, and the one bit nobody can settle offline

Measured reset value at boot is `0x00001440`, which decodes against IDF's
`usb_dwc_gusbcfg_reg_t` as `physel = 1` (bit 6) and `usbtrdtim = 5` (bits 13:10),
with `phyif` (3) and `fsintf` (5) clear. The boot probe confirms both the reset
value and that `fsintf` is writable (`0x1440 -> 0x1460`).

`usb_dw_init()` builds the value from scratch rather than modifying it in place:

```c
gusbcfg = c->phytype | TRDT(USB_DW_TURNAROUND) | USB_DW_TOUTCAL | USB_DW_FORCED_MODE;
```

Two consequences:

- **Turnaround.** Without an override, `USB_DW_TURNAROUND` is derived from the PHY
  encoding -- 9 for anything that is not `DWC_PHYTYPE_UTMI_16` -- which is a
  statement about an 8-bit UTMI PHY at 60 MHz and has nothing to do with this core.
  `meowkits3.h` now sets it to 5, the value the silicon chose for itself. Eight other
  targets do the same, `firmware/export/stm32h743.h` among them, and that target's
  internal FS PHY is the closest analogue here. Verified through the target
  preprocessor that the macro expands to 5, which is what actually determines
  whether the driver's `#ifndef` branch is taken.
- **`physel` gets cleared.** The full-register write drops the reset's `physel = 1`.
  Whether that matters is a hardware question with no offline answer: `physel`
  selects between UTMI and ULPI on cores that have both, and this core has neither,
  so the plausible reading is that the bit is vestigial. The board session should
  enumerate once with the plain write and, if it fails, once with
  `DWC_GUSBCFG |= BIT(6)` restored, before concluding anything about the PHY. Note
  the driver's own comment on this register -- capability bits that are not
  supported read back unchanged -- so a readback cannot tell us which reading is
  right.

## Composite descriptor size against the 256-byte control buffer

`usb_core.c` assembles the configuration descriptor into
`usb_control_data[256]` (`firmware/usbstack/usb_core.c:492`) with

```c
size += drivers[i]->get_config_descriptor(reqdata + size, max_packet_size);
```

and **no bound check** -- the second argument is the endpoint max packet size, not a
capacity. Every enabled class driver therefore writes past its neighbour's bytes if
the total ever exceeds 256, and the corruption is silent. Measured the actual
composition rather than estimating: struct sizes came from compiling a probe with the
target compiler (the feature-unit type is generated by `DEFINE_USB_AC_FEATURE_UNIT`,
so it has to be invoked to be sized), and `HW_FREQ_44` turned out to be **4** for
this target, not 0 -- `HW_SAMPR_CAPS` enables 192/96/64/48/44, so the USB format
descriptor advertises five discrete rates and is `8 + 5*3 = 23` bytes.

| Piece | Bytes |
|---|---|
| configuration header | 9 |
| CDC: IAD 8 + 2 interfaces 18 + header 5 + call mgmt 5 + ACM 4 + union 5 + 3 endpoints 21 | 58 |
| HID: interface 9 + HID class descriptor 9 + endpoint 7 | 25 |
| MSC: interface 9 + 2 endpoints 14 | 23 |
| Audio: AC interface 9 + header 9 + input terminal 12 + feature unit 10 + output terminal 9 + 2 AS interfaces 18 + AS class 7 + format 23 + control-data 7 + ISO out 9 + sync 9 | 94 |

- **CDC + HID + MSC: 115 bytes.**
- **CDC + HID + Audio: 186 bytes.**
- All four at once would be 209 -- still inside the buffer, and still impossible for
  a different reason (six IN endpoints against four transmit FIFOs).

So both requested profiles fit with room to spare and the missing bound check is not
a live hazard here. It is worth knowing for later: an added class would land within
47 bytes of the limit, and the failure mode is memory corruption rather than a
message.

Interfaces: CDC claims 2, HID 1, MSC 1 -- four for the storage profile; audio's AC
plus AS (two alt-settings on one interface) makes five for the DAC profile.

## Step 2 result: attach is now a deliberate act, and the board is safe by default

What the first attach attempts actually established, from the breadcrumb trail on a
live console rather than from inference:

- `usb_enable(true)` fires on the first insertion event once the boot window
  expires, and Rockbox's charge-mode default does **not** prevent it -- see the
  ordering note above. So the pins moved, the console vanished, and the report
  panicked into a reset that could not be reached at all. Only a power cycle
  recovered it, because the mux is in the RTC domain.
- Fixing that took two things: clearing the override at every boot (so the console
  is unconditional), and deciding *in the target* whether the transport is worth
  opening, using the same rule usb.c applies but evaluated before the pins move.
- Verified state now: `usbSkip a=00000001 b=00000000` -- mode 1 is
  `USB_MODE_CHARGE`, no button held, transport refused -- and `usbOut` with
  `clk0=6100e0c3 rst0=9eff1f3c`, the as-found gate and reset words. Pins stayed on
  USB-Serial-JTAG the whole time; SD mounted, `root_menu` reached, a codec loaded
  (`seg`/`placed`).

To attempt enumeration, either pick Settings -> General -> USB mode: Mass storage
and replug (a mode change alone does nothing -- `usb_set_mode()` only stores the
value), or hold any button while plugging in, which is Rockbox's documented
inversion. The attach then runs `usb_core_init()`, the pads go to the wrap, and 8
seconds later the report halts with the register set drawn on the LCD *and* written
to the trail, which the following boot prints on the console.

## The detection question, settled

Charging on this board is inevitable and outside firmware control: the AXP173 charges
from VBUS whatever Rockbox does. That makes `USB_MODE_CHARGE` a misnomer here -- it is
not a charging mode, it is a "no data transport" mode, and it is only honest because
`HAVE_USB_CHARGING_ENABLE` is *not* defined, so the firmware never claims to limit the
charge current.

Detection has exactly one hardware source. The SoC has no VBUS input; GPIO19/20 are
D+/D-. The PMU's REG 0x00 bit 5 is the only cable signal, read over the shared I2C bus
at ~1.6 ms per transaction. The chip's VBUS IRQ bits are left off deliberately: the
GPIO43 line goes low for any enabled event, and the PEK check depends on that line
meaning "PEK happened".

What was wrong about using it: Rockbox reads an insertion event as "enter USB mode"
(`usb.c:499`), unconditionally. Reporting the cable therefore put the player on the
USB screen for as long as it was plugged in -- most of the time -- while offering no
transport. Verified fixed: with the cable in and charge mode selected the device stays
on the root menu, and the trail records no insertion attempt.

Two rules follow, both implemented in the target:

1. **The poll posts insertion only when a data profile is wanted** -- the same
   mode-XOR-button predicate that decides the pins -- and extraction when that stops
   being true. The charging display is untouched: it reads `power_input_status()`,
   the honest source for the cable.
2. **`usb_detect()` answers the transport, not the cable.** `apps/playlist.c:1127`
   uses it to suppress write-error reports on the theory that USB owns the disk; with
   a cable-always answer, every genuine disk error while charging would be silently
   ignored. The two diverge deliberately in charge mode, which is the point:
   `usb_inserted()` reports the state for the status bar and USB screen, while
   `usb_detect()` says there is nothing to wait for.

`usb_init()` calling `usb_enable(false)` under `USB_FULL_INIT` (usb.c:769) is why a
`usbOut` entry appears in the trail at every boot. That is Rockbox making sure the
transceiver starts disabled, not a teardown of anything.

The reference firmware points at the end state -- always-attached CDC composite, MSC as
a `mediaPresent` toggle, an explicit Eject key, a 1 Hz transfer readout. Rockbox has
the eject machinery already. Adopting that shape means giving up the USJ console and
esptool for good, so it is a decision for after bring-up, not during.

## Step 2 state: connected, silent
## Step 2 measured: the interrupt never arrives

A full attach cycle, read from a 40-slot trail:

| Register | Value | Meaning |
|---|---|---|
| `GINTSTS` | `0x54808c28` / `0x5480bc38` | SRQINT, CIDSCHG, PTXFE, EOPF, **ENUMDNE**, **USBRST**, USBSUSP, ESUSP, NPTXFE, RXFLVL |
| `GINTMSK` | `0x000c3000` | OEPINT, IEPINT, ENUMDNE, USBRST -- masked correctly |
| `GAHBCFG` | `0x00000021` | GINT and DMAEN set |
| `PCGCCTL` | `0` | core not clock-gated |
| `DCTL` | `0` | connected, not soft-disconnected |
| `DSTS` | `0x00400003`..`0x0040ee02` | EmNum = full speed, frame number advancing between samples |
| `OTG_CONF` | `0x805c0001` | pad_enable, clk_en, ahb/phy force, dfifo_force_pu, sesend override |
| `INTENABLE` | `0x1106332e` | the allocated slot enabled |
| `rsr.interrupt` | `0x00018040` | **that slot not pending**, sampled at the instant `GINTSTS & GINTMSK` was non-zero (`usbLin`) |
| `esp_intr_dump` | slot 13 level 1 `Used: USB` on CPU 0 | source 38 mapped correctly; `ETS_USB_INTR_SOURCE` is what IDF's own `esp32s3/usb_dwc_periph.c` declares |

So the core has reset, enumerated and is receiving frames, and its INT output does not
reach the interrupt matrix. GDMA and I2C interrupts on the same source mechanism work,
so IDF's dispatch is fine -- this is specific to the OTG core's line.

**A bug found underneath it.** `usb-designware.c` reads the remaining transfer count
with a 19-bit mask. This core's EP0 registers are not 19 bits: IDF's own SVD
(`soc/esp32s3/include/soc/usb_dwc_struct.h`) declares `dieptsiz0`/`doeptsiz0` as
`xfersize:7` with `pktcnt` at bit 19, while the generic per-endpoint registers are
`xfersize:19` and `pktcnt:10`. `DOEPTSIZ(0)` is programmed as
`STUPCNT(1)|PKTCNT(1)|64 = 0x20080040`, and `0x20080040 & 0x7ffff` is 524352, not 64 --
so every control stage computes a nonsense byte count. That is what turns the host's
failure from a timeout into `error -71`: the device answers, with a malformed
transaction. The fix is `EPTSIZ_XFERSZ(epnum)` in the driver, 7-bit for EP0 on
`CPU_XTENSA`, unchanged everywhere else.

**A trap the instrumentation itself created.** Allocating the USB interrupt at boot took
a slot from CPU 0's 22-slot general-use pool. Audio allocates its GDMA TX interrupt
lazily, so the attach consumed the last free slot and the next thing seen was
`intr_alloc: No free interrupt inputs for DMA_OUT_CH0` then
`i2s: cannot allocate the GDMA TX interrupt` -- a panic, a reboot, pads back on
USB-Serial-JTAG, and a transport that appeared to drop two seconds after attaching. The
bring-up build now takes no slot at all (`USB_BRINGUP_NO_IRQ`) and drives the driver
from a tick task (`USB_BRINGUP_POLLED_IRQ`, 100 Hz: far too slow to stream, adequate to
enumerate, because the host retransmits a SETUP it got no answer to). `xt_usb_polled`
counts those separately from `xt_usb_irqs`, which is incremented only in the handler --
an earlier version incremented it in the shared path and made "the interrupt arrived"
true by construction, which also silently disabled the attach judge.

**Why the panic reason was invisible.** A panic during an attach has no console (the
pads are gone), so it reboots silently; the next boot prints the stored record about a
second after starting, before the serial node exists, and that print *consumed* the
record. While `XT_BRINGUP_TRAIL_REPEAT` is set the record is kept and re-printed every
10 s, so a reason is actually readable.

**Open.** Why the pads return roughly two seconds after an attach on some runs while the
trail shows the attach lasting the full 90 seconds on others. `usbEnT` now records the
teardown relative to the attach in ticks, and the wider ring keeps `usbEn(1)`, `usbIn`,
`usbFifo`, `usbConn` and `usbPost`, which the 16-slot ring used to evict.


D+ is asserted and the host sees a full-speed connection. It then gets nothing:
`device descriptor read/64, error -110` twice, then `device not accepting address,
error -62`. That is "the pull-up is there, no SETUP response".

**What fixed the connection.** The OTG core's `iddig`, `bvalid`, `vbusvalid` and
`avalid` are GPIO-matrix *inputs*, not pins. IDF ties them in
`components/esp_hw_support/port/esp32s3/usb_phy.c` (device mode: iddig/bvalid/
vbusvalid to `GPIO_MATRIX_CONST_ONE_INPUT` 0x38, avalid to
`GPIO_MATRIX_CONST_ZERO_INPUT` 0x3C). Rockbox never did that, so the core sat in
IDLE/`fetsusp` and never drove D+. Signals: IDDIG 58, AVALID 59, BVALID 60,
VBUSVALID 61. Doing it from C via `esp_rom_gpio_connect_in_signal()` is one line
per signal and needs no IDF-only symbols.

**What that also refutes.** `DSTS bit 22 fetsusp` was never evidence of missing
VBUS: bit 22 is set in `usbDsts a=00400000`, which came from a probe where the OTG
core was not armed at all. It is the reset default. VBUS genuinely has no chip pin
on this SoC; the AXP173 is the only source, which is what the poll reads.

**Hypotheses, and what discriminates them.** The instrumented poll records
`usbIRQ <count> <GINTSTS>` and `usbDsts <DSTS> <DAINT>` every 4 s while armed, and
`usbIrqE <err>` if `esp_intr_alloc` failed.

| Reading | Meaning | Next action |
|---|---|---|
| `usbIrqE <err>` present | allocation failed | check source 38 routing, level |
| `usbIRQ a=0`, `GINTSTS` has `usbrst`/`enumdone` | status set, mask not delivered | `GINTMSK`, `GAHBCFG.GINT`, `PCGCCTL` |
| `usbIRQ` growing, `DSTS enum speed` 0 | we interrupt but the core never enumerates | PHY clock: `usbClk2`/`usbClk3`, `USB_PLL`, force bits |
| `usbIRQ` growing, enum speed = full, `DAINT` ep0 | core enumerated, driver dropped EP0 | `usb_dw_ep0_recv`, `DOEPDMA` offset, setup decode |

Cache coherence is *not* a candidate: the `CPU_XTENSA` branch of
`usb-designware.c` defines `NO_UNCACHED_ADDR` and `POST_DMA_FLUSH`, and
`cache-xtensa.h` implements `commit_dcache_range` / `discard_dcache_range` through
`cache_hal_writeback_addr` / `cache_hal_invalidate_addr`, so EP0 and the transfer
buffers are maintained in both directions. There is no non-cacheable alias for
internal SRAM on this SoC, which is why it has to be explicit.

**How the data reaches a console.** With the pads handed to the OTG core there is
no console and no esptool. `app_main()` prints the trail about a second after boot,
which is before the host finishes enumerating USB-Serial-JTAG and creates the node,
so a single print is uncatchable -- and the trail is never consumed, because
`xtensa_paniclog_dump()` deliberately leaves `bc_next` alone. Two bring-up affordances
close that gap:

- `paniclog.c` re-prints the trail from the tick task thread every 10 s
  (`XT_BRINGUP_TRAIL_REPEAT`).
- `usb-xtensa.c` judges an attach after 90 s: if `xt_usb_irqs == 0` it records
  `usbDead` and calls `esp_restart()`. A chip reset keeps `.noinit`, and the boot hook
  restores the USJ pads, so the board returns to a readable console and repeats while
  the transport is dead. If interrupts were seen it records `usbAlive` and leaves the
  device running -- restarting a working transport would break audio for nothing. The
  judgement runs from the poll, not from a `sleep()`, because a sleep in the tick task
  thread stops button scanning and the backlight timeout for its duration.

Reaching the board without touching the hardware: unplug the cable. The poll posts
extraction, `usb_core_exit()` runs, `disable_clocks()` releases pads 19/20 and
USB-Serial-JTAG returns.

**Tooling facts, learned the hard way.** Opening the node with pyserial asserts
DTR/RTS and the ROM prints `rst:0x15 (USB_UART_CHIP_RESET)`; read it with a raw
`os.open` instead. Holding the node open makes esptool fail with "device reports
readiness to read but returned no data" *and* makes the kernel hand the next
enumeration a new name (`ttyACM0` -> `ttyACM1`), so both tools must discover the
node dynamically and the capture must close before the flash starts.

## Decision point: the OTG core versus USB-Serial-JTAG

The stock MeowKit firmware's USB CDC runs on the **USB-Serial-JTAG** peripheral, not on
the DWC OTG core -- that is what `303a:1001 "USB JTAG/serial debug unit"` on the host has
been all along, and it is why the console works with no firmware of ours involved. ESP-IDF
deprecated USB-OTG device mode on esp32s2/esp32s3, and nothing on this board exercises the
OTG core except our bring-up.

Checked rather than assumed: arduino-esp32's TinyUSB enable path for this chip touches
only `USB_SERIAL_JTAG` -- `esp32-hal-tinyusb.c:424` calls
`usb_phy_ll_int_jtag_enable(&USB_SERIAL_JTAG)` and nothing in that file initialises the
Wrap or the OTG core -- so the stock firmware's CDC *and* MSC both run on USJ, and the
OTG core is exercised by nothing on this board except our bring-up. TinyUSB appearing in
`usb_msc.cpp` is therefore not evidence that the OTG core works here.

That matters for the sprint target. USJ gives a proven device-side path with a hard
endpoint ceiling: EP0 plus two more, so CDC+HID or CDC+MSC, never CDC+HID+MSC and never
Audio. The OTG core gives the two full profiles the target asks for but needs the
interrupt line to work, which it currently does not: the core asserts nothing on the
matrix even with GINTSTS pending, GINTMSK masked, GAHBCFG.GINT set and IDF's mapping
present.

So the choice is: settle for a reduced profile on the peripheral that works, or keep
pressing on the OTG interrupt with no IDF reference to compare against. It is a hardware
and product question, not something to decide silently in code.

## Delivery is dead below the allocator

Re-run with the interrupt allocated and enabled (and `CONFIG_ESP_INT_WDT` off, which
frees the CPU 0 slot the allocation needs so audio's GDMA allocation does not fail): 94
`usbIRQ` samples across a full attach, all zero. `esp_intr_dump()` shows the source
mapped, INTENABLE has the slot, and `rsr.interrupt` does not. Everything below the
allocator is ruled out by measurement, not inference:

| Ruled out | Evidence |
|---|---|
| Allocator / source number | alloc returns ESP_OK, no `usbIrqE`; `ETS_USB_INTR_SOURCE` is what IDF's `esp32s3/usb_dwc_periph.c` declares as `.irq`; dump shows slot 13 level 1 `Used: USB` on CPU 0 |
| Core masks and enables | `GINTMSK = OEPINT|IEPINT|ENUMDNE|USBRST`, `GAHBCFG = GINT|DMAEN`, `PCGCCTL = 0`, `DCTL = 0` |
| Core is asleep | `DSTS` frame number advances between samples, EmNum = full speed |
| Pad mux points at USJ | IDF's own `usb_wrap_ll.h` says `sw_usb_phy_sel = 1` maps the internal FSLS PHY to the **Wrap** and `0` to the USJ; we set `sw_hw_usb_phy_sel = 1`, `sw_usb_phy_sel = 1`, `otg_conf.phy_sel = 0` -- measured `RTC = 0x00180000`, `OTG_CONF = 0x805c0001` |
| USJ still owning the pads | the USJ only reappears after the judge's restart, which restores the RTC override; the boot hook clears it every boot |

Calling the ROM was tried and refuted. `--wrap=usb_enable` means `__real_usb_enable`
resolves to the ROM symbol at `0x4000297c`; calling it paniced immediately with
`LoadProhibited` at `PC 0x400533d1` in a boot loop. The ROM's `usb_enable` belongs to a
whole device stack in ROM (`usb_transfer`, `usb_set_config`, `usb_dc_ep_set_callback`,
`g_usb_print` in `.data.interface`), not to a separable PHY-and-clock bring-up. The
lower-level `chip_usb_dw_init` at `0x400026f4` is the piece worth trying next.

A hint in the same ROM symbol list: it exports `usb_dc_check_poll_for_interrupts`. The
ROM's own OTG device path (it can put a console over OTG UART) appears to poll rather
than take an interrupt. That is not proof the S3's OTG interrupt is broken, but it is the
kind of thing worth knowing before assuming the silicon is fine and our wiring is wrong.

**Why polling cannot substitute.** The polled dispatch answers the first SETUPs -- which
is what moved the host from `-110` to `-71` -- but the *state transitions* matter as much
as the packets: a bus reset has to be serviced before the host's first SETUP, and at
100 Hz that window is missed. The host retransmits a SETUP, so enumeration looks possible
over polling, but a reset followed immediately by a SETUP is not a retransmission the
device gets a second chance at. So delivery has to be fixed; the tick-task dispatch is a
diagnostic, not a transport.

**Still open after the EP0 fix.** The `-71` remains, and with the 7-bit EP0 mask in place
the next suspect is exactly the ordering above rather than the size arithmetic:
`DOEPTSIZ(0)` is programmed correctly (`0x20080040` = supcnt 1, pktcnt 1, 64 bytes) and
`DIEPINT` shows bit 7 with `xfercompl` clear, i.e. no IN transfer ever completed.

## Step 2 done: the host enumerates Rockbox

```
usb 3-1.1: New USB device found, idVendor=303a, idProduct=4002
usb 3-1.1: Product: Rockbox media player
usb 3-1.1: Manufacturer: Rockbox.org
cdc_acm 3-1.1:1.0: ttyACM0: USB ACM device
usbhid 3-1.1:1.2: can't add hid device: -71
```

Interrupts are delivered (`usbIRQ a=00000024`), the device descriptor and configuration
descriptor were read, and `cdc_acm` bound and created a tty. The HID interface still
fails with `-71`, and the host then dropped the device -- which releases the pads through
`usb_core_exit()`, so this is a genuine failure of the composite descriptor path rather
than the bring-up judge interfering.

Two things fixed it, and one of them is a general lesson about this driver:

1. **`usb_dw_target_clear_irq()` actually clears.** Rockbox has a target hook for exactly
   this and the xtensa implementation was an empty function with a comment saying the
   driver handled it. It does not. TinyUSB's `dwc2_core_init()` writes `0xFFFFFFFF` to
   `gintsts` and `gotgint` *before* enabling any mask, and that ordering matters: status
   bits that were already set when `GINTMSK` was programmed never produce a fresh
   assertion of a level interrupt. After clearing them, the core asserts and the handler
   runs.
2. **The wrap's `pad_pull_override` must stay clear for a device.** IDF 4.4's
   `usb_phy_ll_int_load_conf()` set it with D+ pull-up and D- pull-down for device mode,
   and copying that made things strictly worse: with the override on, `GINTSTS` lost
   `usbrst` and the host made no enumeration attempt at all. IDF 5.5's `usb_new_phy()` --
   the path TinyUSB's device examples actually use -- only calls
   `usb_wrap_ll_phy_enable_pull_override()` in the host branch. The core drives its own
   pull-up on connect.

Two claims I made earlier here were wrong and are corrected in the code comments:

- PHYSEL is not a bit this core can be talked out of, nor into. `GHWCFG2` reports no
  high-speed PHY, and `GUSBCFG` reads `0x40001460` with bit 6 set even when the driver
  assigns a value that omits it. Setting `.phytype = FSINTF | PHYSEL` changed nothing;
  the run where the connection vanished was the one that added the pull override.
- `#define USB_BRINGUP_POLLED_IRQ 0` does not disable anything: `#ifdef` is satisfied by
  a definition of 0. The polled dispatch was still running (`usbPoll` non-zero) while I
  believed it was off. The guard is `#if USB_BRINGUP_POLLED_IRQ` now.

Still open: the `-71` on the HID interface, and after that MSC and the audio profile. The
bring-up scaffolding (the 120 s attach window, the register report, the restart judge) is
still in and should come out once the composite descriptor is fully accepted.

## Step 3 done: interrupt-driven, no scaffolding, self-triggered flashing

Delivery was fixed at its source. The polled dispatch is gone, as is the bring-up
judge, the periodic register dump and the 120-second attach window -- `usb_detect()`
answers the cable from the first tick, which is what every other target does and what
`usb.h` requires. What earned its place: the interrupt allocated enabled with
`ESP_INTR_FLAG_LOWMED` (CherryUSB's dwc2 port for this controller does exactly that, and
Rockbox's `ESP_INTR_FLAG_INTRDISABLED` meant the first pending event never drew the
line), and the handler clearing `GINTSTS`/`GOTGINT` when it fires before the driver is
armed -- the latter has never actually been observed running, so it is insurance rather
than a measured fix.

`make usbflash` now flashes the board over Rockbox's own CDC: `'f'` on the OUT endpoint
-> release the pads -> `RTC_CNTL_OPTION1.FORCE_DOWNLOAD_BOOT` -> reset -> esptool with
`--before no_reset` -> default `--after hard_reset` puts Rockbox back. Measured in ~13
s with no BOOT button and no power cycle. Details, including the clock bit the USJ
shares with the DWC core and the fact that a software-driven reset lands the ROM on the
OTG core rather than the USJ, are in `AGENTS.md`.

Remaining for the driver: the two profiles (`CDC+HID+MSC` and `CDC+HID+Audio`), and
suspend/resume -- the host runtime-suspends an idle composite device today and the
driver's mask covers only `USBRST|ENUMDNE|IEPINT|OEPINT`.
