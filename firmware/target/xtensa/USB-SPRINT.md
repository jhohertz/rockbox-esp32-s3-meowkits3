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

`usb_struct.h` gives **7 IN and 7 OUT** endpoint blocks, i.e. EP0 plus six usable.
Device EP FIFO is `fifo[16][0x400]` = 16 KB total. The core is full-speed only.

Rockbox class demand, read from each driver's `ep_allocs`:

| class | endpoints |
|---|---|
| serial (CDC ACM) | 3 — bulk IN, bulk OUT, interrupt notification |
| storage (MSC) | 2 — bulk IN, bulk OUT |
| hid | 1 — interrupt IN |
| audio | 2 — isochronous OUT, isochronous feedback IN |

- `CDC + HID + MSC` = 3 + 1 + 2 = **6** ✓ fills the core exactly
- `CDC + HID + Audio` = 3 + 1 + 2 = **6** ✓ fills the core exactly
- `CDC + HID + MSC + Audio` = 8 ✗ — does not fit

So the two profiles must be mutually exclusive, and there is no room for a fourth
class. `usb_core.c` handles overflow by marking the driver `error` and dropping it
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

1. **Clock, base and pads.** `SOC_DPORT_USB_BASE` with the device block at +0xb00,
   the USB OTG bit in the peripheral clock enable, and `USB_WRAP`
   (`usb_wrap_reg.h`/`usb_wrap_struct.h` exist for esp32s3) to route GPIO19/20 to the
   OTG internal PHY instead of USB-Serial-JTAG. `usb_wrap_struct.h` has the
   internal/external PHY select bit and a "force PHY clock always on" bit — both
   likely needed.
2. **PHY bring-up — the known blocker.** The current stub records that this core's
   `GUSBCFG` fields are `phyif` (bit 3), `ulpiutmisel` (4), `fsintf` (5), `physel`
   (6), and that ESP-IDF's own driver sets `phyif = 1`. Rockbox's
   `usb_dw_check_hw()` writes `DWC_GUSBCFG = c->phytype` and then compares the
   **whole register** back, so it panics "PHY type not supported" before anything
   runs. The fix is a masked comparison or a target-specific override. This is the
   first thing to get past.
3. **Interrupt.** Rockbox expects `usb_drv_int()` called from the controller's
   handler. Wire `esp_intr_alloc` at LEVEL1 to it. Two lessons from the audio path
   apply unchanged: nothing may print in that handler, and the handler must be
   IRAM-resident or it stalls behind PSRAM traffic. IRAM has room (the TX ISR
   experiment measured 1107 B against a 358 KB region) but IRAM text **costs heap**,
   of which roughly 64 KB remains.
4. **`usb_detect()`.** Keep the existing AXP173 VBUS monitor from the stub — it is
   correct: a PMU read is ~1.6 ms on this I2C peripheral, too slow for the tick, so
   it caches the result and refreshes it in a thread. Discard the comments claiming
   the OTG core is not needed.
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

0. **Compile only.** Add the config flags, enable `usb_serial.c`, see what breaks.
   No hardware, and it answers the bit-rot question cheaply.
1. **Probe only.** Enable OTG clock and pads, read `GSNPSID`, `hw_numeps`, and the
   `GUSBCFG` reset value; print them. Do not attach. This answers whether the core is
   present and how many endpoints it really reports — the number the whole profile
   plan rests on.
2. **Enumerate one simple class** (HID alone). First host-visible device; validates
   PHY, attach, and EP0 traffic.
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
