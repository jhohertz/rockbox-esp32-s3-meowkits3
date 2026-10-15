#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

INCLUDES += -I$(FIRMDIR) -I$(FIRMDIR)/export -I$(FIRMDIR)/drivers

# Rockbox carries replacements for libc headers, and on an ESP-IDF-backed target
# the C library is IDF's (newlib/picolibc), not Rockbox's. The two sets collide on:
#
#   firmware/include:      assert.h, elf.h
#   firmware/libc/include: ctype.h errno.h fcntl.h inttypes.h math.h stdlib.h
#                          string.h time.h
#
# As plain -I those directories are searched before the toolchain's system
# directories, so IDF's own headers resolve against Rockbox's replacements. That
# is not a cosmetic ordering issue: components/newlib/platform_include/assert.h is
# a shim whose whole job is `#include_next <assert.h>` to reach "newlib's own
# unmodified assert.h", and with Rockbox first the next one it finds is
# firmware/include/assert.h, which pulls in panic.h and never reaches the real
# libc. The failure then surfaces somewhere unrelated -- a missing
# ESP_STATIC_ASSERT, an unknown `FILE` -- which makes it look like a dialect or a
# macro problem when it is an include-path problem.
#
# -idirafter is the precise tool: the directory is searched *after* the standard
# system directories. IDF therefore gets the real libc, and Rockbox-only headers
# (button.h, lcd.h, the kernel headers, config.h) are unaffected because the libc
# does not provide those names.
ifeq (xtensa,$(CPU))
INCLUDES += -idirafter $(FIRMDIR)/include
INCLUDES += -I$(FIRMDIR)/kernel/include
else
INCLUDES += -I$(FIRMDIR)/include -I$(FIRMDIR)/kernel/include
endif
ifndef APP_TYPE
ifeq (xtensa,$(CPU))
INCLUDES += -idirafter $(FIRMDIR)/libc/include
else
INCLUDES += -I$(FIRMDIR)/libc/include
endif
endif

ifneq ($(filter reggen,$(TOOLSET)),)
  include $(FIRMDIR)/reggen/reggen.make
endif

FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/asm/SOURCES)
FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/SOURCES)
FIRMLIB_OBJ := $(call c2obj, $(FIRMLIB_SRC))
FIRMLIB_OBJ += $(BUILDDIR)/sysfont.o
OTHER_SRC += $(FIRMLIB_SRC)

FIRMLIB = $(BUILDDIR)/firmware/libfirmware.a

ifeq ($(SYSFONT),)
SYSFONT = 08-Schumacher-Clean
endif
SYSFONTX = $(ROOTDIR)/fonts/$(SYSFONT).bdf

CLEANOBJS += $(BUILDDIR)/sysfont.* $(BUILDDIR)/version.*

# Limits for the built-in sysfont: ASCII for bootloaders, ISO8859-1 for normal builds
ifneq (,$(findstring -DBOOTLOADER,$(EXTRA_DEFINES)))
	MAXCHAR = 127
else
	MAXCHAR = 255
endif

$(FIRMLIB): $(FIRMLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

$(BUILDDIR)/sysfont.h: $(SYSFONTX) $(TOOLS) $(BUILDDIR)/firmware/common/config.o
	$(call PRINTS,CONVBDF $(subst $(ROOTDIR)/,,$<))$(TOOLSDIR)/convbdf -l $(MAXCHAR) -h -o $@ $<

$(BUILDDIR)/sysfont.o: $(SYSFONTX) $(BUILDDIR)/sysfont.h
	$(call PRINTS,CONVBDF $(subst $(ROOTDIR)/,,$<))$(TOOLSDIR)/convbdf -l $(MAXCHAR) -c -o $(BUILDDIR)/sysfont.c $<
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$(BUILDDIR)/sysfont.c))$(CC) $(CFLAGS) -c $(BUILDDIR)/sysfont.c -o $@

ifdef USE_LTO
$(BUILDDIR)/firmware/asm/%.o: CFLAGS += -fno-lto
$(BUILDDIR)/firmware/kernel/thread.o: CFLAGS += -fno-lto
endif

# GNU make (at least) has a bug/feature that exported variable are not available
# in the shell function (but are in recipe). Thus we need to explicitely pass
# the VERSION environement variable
SVNVERSION:=$(shell VERSION='$(VERSION)' $(TOOLSDIR)/version.sh $(ROOTDIR))
OLDSVNVERSION:=$(shell grep 'RBVERSION' $(BUILDDIR)/rbversion.h 2>/dev/null|cut -d '"' -f 2 || echo "NOREVISION")

CORE_LIBS:=$(FIRMLIB)

ifneq ($(SVNVERSION),$(OLDSVNVERSION))
.PHONY: $(BUILDDIR)/rbversion.h
endif

$(BUILDDIR)/rbversion.h:
	$(call PRINTS,GEN $(@F))$(TOOLSDIR)/genversion.sh $(BUILDDIR) $(SVNVERSION)
