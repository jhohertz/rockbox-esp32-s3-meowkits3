# Flashing for ESP32 targets built through ESP-IDF.
#
# The bare-metal shape (one esptool write at flash offset 0, because the mask ROM
# reads the image header there) no longer applies. IDF's image is an *app* image:
# its second-stage bootloader occupies 0x0, the partition table sits at 0x8000 and
# the app itself is at the app partition offset, 0x10000. Writing the app at 0x0
# therefore does not boot anything -- it overwrites the bootloader with an image
# whose header the ROM will try to parse as a bootloader.
#
# So flashing is delegated to IDF, which knows the layout and the flash
# mode/frequency/size it built for:
#
#   make flash                              # idf.py picks the port
#   make flash ESPPORT=/dev/ttyACM0
#
# espverify inspects the app image without a board attached.

ESPTOOL ?= esptool

# The packed image is named rockbox.<chip>, so the chip falls out of the artifact
# name. Override ESPCHIP for a different module on the same arch.
ESPCHIP ?= $(lastword $(subst ., ,$(BINARY)))
ESPPORT ?=

ifneq (,$(ESPPORT))
  IDFPORTOPT := -p $(ESPPORT)
else
  IDFPORTOPT :=
endif

flash: $(BUILDDIR)/$(BINARY)
	cd $(IDFDIR) && $(IDFPY) $(IDFPORTOPT) flash
.PHONY: flash

# Checks the app image the loader will see: segment table, load addresses and the
# appended hash. Useful without a board, which is what makes it worth running
# before you have one. `idf.py size` in the same directory gives the per-section
# breakdown if the image is not fitting.
espverify: $(BUILDDIR)/$(BINARY)
	$(ESPTOOL) --chip $(ESPCHIP) image_info $(BUILDDIR)/$(BINARY)
.PHONY: espverify

# Flash over Rockbox's own USB serial. The firmware answers one byte on the CDC
# OUT endpoint by handing the pads over and asking the ROM for download mode, so
# this needs neither the BOOT button nor a power cycle. esptool runs with
# --before no_reset because the OTG core has no DTR/RTS wiring, and with its
# default --after hard_reset so the ROM boots Rockbox again at the end.
#
# Use `make flash` (with BOOT held) for a board that is not answering on USB.
usbflash: $(BUILDDIR)/$(BINARY)
	python3 $(TOOLSDIR)/esp-usbflash.py --build $(IDFDIR)/build \
		--chip $(ESPCHIP) $(IDFPORTOPT)
.PHONY: usbflash
