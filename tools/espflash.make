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

flash: $(BUILDDIR)/$(BINARY) esplayer
	cd $(IDFDIR) && $(IDFPY) $(IDFPORTOPT) flash
.PHONY: flash

# Checks the app image the loader will see: segment table, load addresses and the
# appended hash. Useful without a board, which is what makes it worth running
# before you have one. `idf.py size` in the same directory gives the per-section
# breakdown if the image is not fitting.
espverify: $(BUILDDIR)/$(BINARY) esplayer
	$(ESPTOOL) --chip $(ESPCHIP) image_info $(BUILDDIR)/$(BINARY)
.PHONY: espverify

# The external arena has no link-time guard, because ESP-IDF owns the linker script
# and the ldfragment we feed it only routes archives to memory regions -- an ASSERT()
# has nowhere to live. tools/esp-layout-check reads the addresses out of the map and
# compares them against the reservations memory-xtensa.h defines, so an arena that has
# grown into the plugin/codec text fails the build instead of appearing as a printf on
# a device after the image shipped. flash and espverify both run it: the point is that
# you cannot reach a board without having passed it.
# Spelled from $(BUILDDIR) rather than $(IDFDIR)/$(ROOTDIR): this file is included
# by root.make before those are set, so a ?= here would expand to an empty path.
RBROOT := $(patsubst %/,%,$(dir $(BUILDDIR)))
ESPLELFMAP ?= $(RBROOT)/idf-app/build/rockbox.map

esplayer: $(ESPLELFMAP)
	$(SILENT)$(RBROOT)/tools/esp-layout-check $(ESPLELFMAP) \
		$(RBROOT)/firmware/export/config/$(MODELNAME).h \
		$(RBROOT)/firmware/target/xtensa/memory-xtensa.h $(BUILDDIR)/Makefile
.PHONY: esplayer
