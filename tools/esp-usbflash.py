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


def find_rockbox_port():
    for dev, product in tty_nodes().items():
        if PRODUCT in product:
            return dev
    return None


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

    port = args.port or find_rockbox_port()
    if port is None and not args.no_trigger:
        print("no %s device on any /dev/ttyACM; use --port, or put the board "
              "in download mode and pass --no-trigger" % PRODUCT)
        return 2

    if not args.no_trigger:
        print("asking %s for download mode ('%s' over CDC)"
              % (port, TRIGGER.decode()))
        send_trigger(port)
        time.sleep(WAIT_FOR_DOWNLOAD)

    cmd = esptool_cmd(args) + [
        "--chip", args.chip,
        "-p", port,
        "-b", args.baud,
        # The OTG core has no DTR/RTS wiring; esptool must not try.
        "--before", "no_reset",
        # Default, stated because it is what ends the loop: the ROM reboots
        # into the app, OPTION1 having been consumed, and Rockbox re-attaches.
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

    deadline = time.time() + WAIT_FOR_REATTACH
    while time.time() < deadline:
        if find_rockbox_port():
            print("Rockbox is back on the bus")
            return 0
        time.sleep(2)
    print("Rockbox did not re-attach within %.0f s" % WAIT_FOR_REATTACH)
    return 1


if __name__ == "__main__":
    sys.exit(main())
