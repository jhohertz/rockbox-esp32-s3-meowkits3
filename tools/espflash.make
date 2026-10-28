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

# esptool's name depends on where it came from. A distribution package installs
# `esptool`, but ESP-IDF's own Python environment installs it as `esptool.py`
# (4.12.0 in v5.5.5) with the `esptool` module behind it, and puts only that name
# on PATH. Resolving it here rather than hard-coding one spelling is what makes
# `make espverify` work both on a host with esptool installed and inside the IDF
# environment; ESPTOOL= still overrides either way.
ESPTOOL ?= $(shell command -v esptool 2>/dev/null || command -v esptool.py 2>/dev/null)
ifeq (,$(ESPTOOL))
$(warning espverify: neither esptool nor esptool.py is on PATH -- set ESPTOOL=)
endif

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
#
# The ELF and the linker map are copied into $(BUILDDIR) by the image rule in
# tools/root.make, because that is where every other port leaves them and where the
# goals that read them look. spelling from $(ROOTDIR) into IDF's own build tree was
# only needed while the map had no home in the build directory, and it broke on an
# out-of-tree build.
ESPLELFMAP ?= $(BUILDDIR)/rockbox.map

# The map is a build product with no rule of its own, so depending on it directly
# makes `make esplayer` in a clean tree fail with "No rule to make target" instead
# of building first. Depending on the packed image gives the ordering we want: the
# guard runs after there is something to guard.
esplayer: $(BUILDDIR)/$(BINARY)
	$(SILENT)$(ROOTDIR)/tools/esp-layout-check $(ESPLELFMAP) \
		$(ROOTDIR)/firmware/export/config/$(MODELNAME).h \
		$(ROOTDIR)/firmware/target/xtensa/memory-xtensa.h $(BUILDDIR)/Makefile
.PHONY: esplayer

# The all-in-one image: bootloader, partition table and app merged into one file
# that starts with the bootloader's image header, so there is one file and one
# offset. This is what a web flasher wants, and what a vendor release image looks
# like -- `esptool write_flash 0x0 rockbox-full.bin`, or the same file at offset
# 0x0 in a browser. esptool's own merger does the assembling, so the three pieces
# are byte-identical to what `make flash` hands to idf.py; the gaps between them
# are filled with 0xFF, which is what erased flash already holds.
#
# It depends on esplayer for the same reason flash and espverify do: an image whose
# external arena has grown into the plugin/codec reservation must not reach a board.
ESPFULL ?= $(BUILDDIR)/rockbox-flash.bin
ESPARTIFACTS ?= $(BUILDDIR)/esp-artifacts

espfull: $(BUILDDIR)/$(BINARY) esplayer
	$(ESPTOOL) --chip $(ESPCHIP) merge_bin --output $(ESPFULL) --target-offset 0x0 \
		0x0 $(IDFDIR)/build/bootloader/bootloader.bin \
		0x8000 $(IDFDIR)/build/partition_table/partition-table.bin \
		0x10000 $(BUILDDIR)/$(BINARY)
.PHONY: espfull

# Everything that constitutes a release of an ESP target, gathered by a Rockbox goal
# rather than by somebody knowing IDF's tree: the app image, the two pieces that make
# it bootable at 0x0 with the offsets IDF itself uses, the merged one-file image, the
# loader's view of that image, and the map and ELF so the arena reservations and the
# symbols can be checked by a receiver with no toolchain and no ESP-IDF.
espartifacts: $(BUILDDIR)/$(BINARY) esplayer espfull
	$(call PRINTS,ESPARTIFACTS $(ESPARTIFACTS))
	$(SILENT)rm -rf $(ESPARTIFACTS)
	$(SILENT)mkdir -p $(ESPARTIFACTS)
	$(SILENT)cp $(IDFDIR)/build/bootloader/bootloader.bin \
		   $(IDFDIR)/build/partition_table/partition-table.bin \
		   $(IDFDIR)/build/flash_args \
		   $(BUILDDIR)/$(BINARY) $(ESPFULL) \
		   $(BUILDDIR)/rockbox.map $(BUILDDIR)/rockbox.elf $(ESPARTIFACTS)/
	$(SILENT)$(ESPTOOL) --chip $(ESPCHIP) image_info $(BUILDDIR)/$(BINARY) \
		   > $(ESPARTIFACTS)/espverify.txt 2>&1
.PHONY: espartifacts

# clean:: in tools/root.make removes $(BINARY) but not the merged image or the
# artifact directory, so a stale release image could sit beside a fresh one and be
# the file someone ships.
clean::
	$(SILENT)rm -f $(ESPFULL)
	$(SILENT)rm -rf $(ESPARTIFACTS)
