#!/usr/bin/env python3
#
# Flash a MeowKit ESP32-S3 over Rockbox's own USB serial, without the BOOT
# button and without a power cycle.
#
# The firmware answers a single 'f' on the CDC OUT endpoint by releasing the
# USB pads, writing RTC_CNTL_OPTION1.FORCE_DOWNLOAD_BOOT and resetting. The ROM
# then comes up in download mode on the DWC core -- measured, with USJ CONF0
# usb_pad_enable clear and OPTION1 consumed -- so the host keeps the same
# /dev/ttyACM node it already had, and esptool talks to the ROM with
# NOTE, corrected against the mask ROM (esp32s3_rev0_rom.elf): the boot-mode decision is
# detect_uart_usb_spi_boot_mode at 0x4004391c, which calls UartConnCheck(0), (1), (3), (4)
# for UART0, UART1, USB-Serial-JTAG and USB-OTG. UartConnCheck at 0x40047b00 is the ROM's
# "wait 0x3e8 ms for esptool's sync magic" loop (movi a10, 0x3e8; then RcvMsg). The OPTION1
# register (RTC_CNTL_FORCE_DOWNLOAD_BOOT) is referenced exactly once in the whole ROM, inside
# usb_uart_otg_rx_one_char_block, and never by the boot decision -- so on esp32s3 an app
# cannot force download mode by writing a register. The host has to send sync while the ROM
# is still probing, or drive the reset the ROM enables for the USJ path
# (uart_usb_otg_enable_reset_on_rts). That is what --before default_reset does below, and why
# the handover is deterministic now rather than a race against a one-second window.
# --before no_reset. esptool's default --after hard_reset then puts Rockbox
# back, which is what makes the loop close: no button, no unplug.
#
# Why 'no_reset' rather than the usual default_reset: esptool's reset handshake
# drives DTR/RTS, and on this path the active USB peripheral is the OTG core,
# which has no such wiring. Asserting DTR before the chip is in download mode
# does nothing, and asserting it afterwards is how a board ends up parked in the
# ROM with the host holding a stale device entry.
#
# The trigger byte is written through a raw file descriptor rather than a
# pyserial handle on purpose: opening a serial port asserts DTR/RTS, and on the
# USB-Serial-JTAG node that is a chip reset. The device being asked is not the
# USJ one, but the same habit avoids a class of surprises.
#
# Usage, from a configured build directory:
#
#   make usbflash
#   make usbflash ESPPORT=/dev/ttyACM0
#
# Or directly:
#
#   tools/esp-usbflash.py --build idf-app [--port /dev/ttyACM0] [--chip esp32s3]

import argparse
import glob
import os
import select
import subprocess
import sys
import time

PRODUCT = "Rockbox media player"
TRIGGER = b"f"
WAIT_FOR_DOWNLOAD = 3.0
WAIT_FOR_REATTACH = 45.0


def tty_nodes():
    """Every /dev/ttyACMn with the product string its USB device reports."""
    found = {}
    for path in sorted(glob.glob("/sys/class/tty/ttyACM*/device/../product")):
        name = path.split("/sys/class/tty/")[1].split("/")[0]
        try:
            with open(path) as f:
                found["/dev/" + name] = f.read().strip()
        except OSError:
            continue
    return found


def app_streaming(port, seconds=16):
    """True only when the device sends its trace line.

    A sysfs product string is not evidence of a live app: it survives on a device that
    stopped answering, which is exactly how a board parked in download mode reads as a
    working Rockbox. That false positive is why success is decided by bytes arriving from
    the device rather than by what the host still has bound.
    """
    fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
    buf = b""
    try:
        deadline = time.time() + seconds
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 1.0)
            if r:
                try:
                    buf += os.read(fd, 4096)
                except BlockingIOError:
                    pass
            if b"rockbox:" in buf:
                return True
    finally:
        os.close(fd)
    return False


def find_rockbox_ports():
    """Every ACM node currently bound to the Rockbox device.

    There is usually one, but the host can hold a stale node for the same physical
    device while it re-enumerates: after a crash reboot the CDC port moved from
    ttyACM0 to ttyACM1, the trigger was written to ttyACM0, and the device never
    received it -- which looked like a broken handover, not a wrong address. Write
    to everything that matches and re-resolve between attempts.
    """
    return [dev for dev, product in tty_nodes().items() if PRODUCT in product]


def send_trigger(port):
    """One byte, no line-state change: see the note above about DTR."""
    fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
    try:
        os.write(fd, TRIGGER)
    finally:
        os.close(fd)


def read_flash_args(build_dir):
    """IDF's own address map, so this tool cannot drift from what idf.py does."""
    args_file = os.path.join(build_dir, "flash_args")
    with open(args_file) as f:
        return f.read().split()


def esptool_cmd(args):
    if args.esptool:
        return [args.esptool]
    idf = os.environ.get("IDF_PATH")
    if idf:
        tool = os.path.join(idf, "components", "esptool_py", "esptool",
                            "esptool.py")
        if os.path.exists(tool):
            return [sys.executable, tool]
    return ["esptool"]


def main():
    ap = argparse.ArgumentParser(
        description="flash over Rockbox's own USB serial, no BOOT button")
    ap.add_argument("--build", default="idf-app",
                    help="IDF build dir holding flash_args (default idf-app)")
    ap.add_argument("--port", help="serial port (default: find Rockbox)")
    ap.add_argument("--chip", default="esp32s3")
    ap.add_argument("--baud", default="921600")
    ap.add_argument("--esptool", help="esptool executable or script")
    ap.add_argument("--no-trigger", action="store_true",
                    help="assume the chip is already in download mode")
    args = ap.parse_args()

    port = args.port
    answered = None

    if not args.no_trigger:
        if port:
            ports = [port]
        else:
            ports = find_rockbox_ports()
        if not ports:
            print("no %s device on any /dev/ttyACM; use --port, or put the board "
                  "in download mode and pass --no-trigger" % PRODUCT)
            return 2

        print("asking %s for download mode ('%s' over CDC)"
              % (", ".join(ports), TRIGGER.decode()))
        for dev in ports:
            try:
                send_trigger(dev)
            except OSError as exc:
                print("  %s: %s" % (dev, exc))
        time.sleep(WAIT_FOR_DOWNLOAD)
        # Whatever node carried the trigger is gone by then: releasing the pads takes the
        # Rockbox device off the bus, and the USJ node that replaces it can arrive under a
        # different ttyACM index. Polling the trigger's own port therefore found nothing,
        # which is what made a working handover look broken.
        #
        # Each attempt probes with read_mac, which is itself the sync magic the ROM is
        # listening for (see the note at the top), and asks for default_reset so the ROM is
        # handed a reset it can act on. Every ttyACM node is a candidate.
        for attempt in range(5):
            ports = [args.port] if args.port else list(tty_nodes())
            if not ports:
                print("  no ttyACM node at all (attempt %d)" % (attempt + 1))
                time.sleep(1.0)
                continue
            for dev in ports:
                probe = esptool_cmd(args) + ["--chip", args.chip, "-p", dev,
                                             "-b", "115200",
                                             "--before", "default_reset",
                                             "--after", "no_reset", "read_mac"]
                try:
                    out = subprocess.run(probe, cwd=args.build, timeout=25,
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT)
                except subprocess.TimeoutExpired:
                    continue
                if b"MAC" in out.stdout:
                    answered = dev
                    break
            if answered:
                break
            print("  ROM not answering (attempt %d), ports: %s"
                  % (attempt + 1, ", ".join(ports)))
            time.sleep(1.0)
        if not answered:
            print("the chip never answered the ROM, so the trigger did not take. If the "
                  "board is wedged with the pads on the OTG core, hold BOOT and use "
                  "make flash.")
            return 1
        port = answered

    cmd = esptool_cmd(args) + [
        "--chip", args.chip,
        "-p", port,
        "-b", args.baud,
        # default_reset, not no_reset: the ROM decides by listening for sync after a reset
        # it was given, and does not read OPTION1. Over USJ this drives exactly the reset the
        # ROM enables for that interface, so the flash lands deterministically.
        "--before", "default_reset",
        # Ends the loop: the ROM reboots into the app and Rockbox re-attaches.
        "--after", "hard_reset",
        "write_flash",
    ] + read_flash_args(args.build)

    print(" ".join(cmd))
    # flash_args holds relative paths (bootloader/bootloader.bin and friends), so
    # esptool has to run from the IDF build directory or every file misses.
    rc = subprocess.call(cmd, cwd=args.build)
    if rc != 0:
        print("esptool returned %d" % rc)
        return rc

    if app_streaming(port, seconds=WAIT_FOR_REATTACH):
        print("Rockbox is running and streaming over CDC")
        return 0

    # esptool's --after hard_reset does not always stick: the chip can still be sitting
    # in the ROM waiting for a host. Asking the ROM to reset is the cheap second try,
    # and it only works while download mode is up -- which is the state we are in.
    print("the app did not come back; asking the ROM to reset again")
    cmd = esptool_cmd(args) + ["--chip", args.chip, "-p", port, "-b", "115200",
                               "--before", "no_reset", "--after", "hard_reset",
                               "read_mac"]
    subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if app_streaming(port, seconds=WAIT_FOR_REATTACH):
        print("Rockbox is running and streaming over CDC")
        return 0
    print("Rockbox did not come back; power-cycle the board")
    return 1


if __name__ == "__main__":
    sys.exit(main())
