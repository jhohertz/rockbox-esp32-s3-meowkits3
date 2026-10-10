# Flashing for ESP32 targets.
#
# There is no Rockbox bootloader to write and no partition table to create: the
# mask ROM loader reads the image header at flash offset 0, copies the RAM
# segments, sets up the flash MMU windows and jumps to the entry point recorded
# in that header. So flashing is one esptool invocation on the packed image that
# tools/esp-elf2image produces.
#
#   make flash                              # esptool picks the port
#   make flash ESPPORT=/dev/ttyACM0
#   make flash ESPTOOL=/path/to/esptool
#   make espverify                          # inspect the image, no board needed

ESPTOOL ?= esptool

# The packed image is named rockbox.<chip>, so the chip falls out of the artifact
# name. Override ESPCHIP for a different module on the same arch.
ESPCHIP ?= $(lastword $(subst ., ,$(BINARY)))
ESPPORT ?=
ESPADDR ?= 0x0

ifneq (,$(ESPPORT))
  ESPPORTOPT := --port $(ESPPORT)
else
  ESPPORTOPT :=
endif

flash: $(BUILDDIR)/$(BINARY)
	$(ESPTOOL) --chip $(ESPCHIP) $(ESPPORTOPT) write_flash $(ESPADDR) $(BUILDDIR)/$(BINARY)
.PHONY: flash

# Checks the image the ROM loader will actually see: the segment table, the load
# addresses, and the trailing checksum the loader verifies before handing control
# to the entry point. Runs without a board attached, which is what makes it
# worth running before you have one.
espverify: $(BUILDDIR)/$(BINARY)
	$(ESPTOOL) --chip $(ESPCHIP) image_info $(BUILDDIR)/$(BINARY)
.PHONY: espverify
