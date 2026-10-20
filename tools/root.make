#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

include $(TOOLSDIR)/functions.make

DEFINES = -DROCKBOX -DMEMORYSIZE=$(MEMORYSIZE) $(TARGET) \
	-DTARGET_ID=$(TARGET_ID) -DTARGET_NAME=\"$(MODELNAME)\" $(BUILDDATE) \
	$(EXTRA_DEFINES) # <-- -DSIMULATOR or not
INCLUDES = -I$(BUILDDIR) -I$(BUILDDIR)/lang $(TARGET_INC)

# There are two compilation domains in a Rockbox build, and on most targets they
# are the same. On an ESP-IDF target they are not, and conflating them breaks the
# second one silently.
#
#   firmware domain   firmware/, apps/, most of lib/rbcodec. These objects end up
#                     in the firmware image, so on this target they are linked
#                     against the platform C library and have to see its headers.
#
#   freestanding      lib/rbcodec/codecs/ and apps/plugins/. A codec or a plugin
#       domain        is a separate executable with its own crt0, its own linker
#                     script and no C library in its link at all -- just -lgcc.
#                     Its "C library" is a jump table: the firmware hands it a
#                     struct codec_api / struct plugin_api of function pointers,
#                     and libcodec.a defines libc-*named* functions as thunks
#                     through it (memcpy, strcpy, strcmp, strcat, codec_strlen).
#                     Everything else libc-shaped has to be resolved at compile
#                     time by Rockbox's own libc headers, which are deliberately
#                     macro-based so no call is emitted: `#define abs(x)
#                     ((int)ABS(x))`, and assert() expanding to panicf(). Get that
#                     wrong and abs()/llabs()/__assert_func become real calls to a
#                     library that is not in the link.
#
# BASE_CFLAGS is what every target has always used, and it is what the
# freestanding domain must keep. CFLAGS is the firmware domain's, and on this
# target it gains IDF's flags.
BASE_CFLAGS = $(INCLUDES) $(DEFINES) $(GCCOPTS)
CFLAGS = $(BASE_CFLAGS)
# Built from BASE_CFLAGS, not from CFLAGS. Both variables are recursively
# expanded, so deriving PPCFLAGS from CFLAGS "before" IDF is added to it does not
# capture the earlier value -- it re-expands at use time and picks the IDF flags
# up anyway. That matters because a SOURCES file is a C preprocessor program: if
# IDF's CONFIG_ defines reach it, IDF's configuration starts deciding which
# Rockbox files get compiled.
#
# LIBC_HDR_AFTER is in it because preprocessing is a firmware-domain activity and
# those directories are no longer on INCLUDES on this target: apps/rbcodecconfig.h
# includes file.h, which lives in firmware/include, and preprocessing that file
# with a path that omits it fails at parse time, before any rule runs.
PPCFLAGS = $(filter-out -g -Dmain=SDL_main,$(BASE_CFLAGS) $(LIBC_HDR_AFTER)) # cygwin sdl-config fix

# The freestanding domain: Rockbox's include order, Rockbox's defines, Rockbox's
# warning and codegen policy, and nothing belonging to any external SDK.
# LIBC_HDR_FIRST is empty except on targets that move the libc-replacement
# directories off INCLUDES; see firmware/firmware.make.
FREESTAND_CFLAGS = $(BASE_CFLAGS) $(LIBC_HDR_FIRST)

# ESP-IDF-backed targets compile their own sources with Rockbox's build, but the
# files that reach IDF drivers have to see IDF's headers and IDF's generated
# sdkconfig.h. The flags are harvested from IDF's own build by tools/idf-flags
# instead of being duplicated here: they follow sdkconfig and shift whenever a
# component is enabled or disabled, and a hand-kept list fails as a code bug
# (header found, CONFIG_ define missing).
ifeq (xtensa,$(CPU))
-include $(BUILDDIR)/idf.make
#
# Ordering is the whole problem here, and it cuts both ways:
#
#   - IDF's headers must get the real C library.
#     components/newlib/platform_include/assert.h is a shim whose entire job is
#     `#include_next <assert.h>` to reach "newlib's own unmodified assert.h". If
#     Rockbox's firmware/include is a plain -I, the next assert.h it finds is
#     Rockbox's, which pulls in panic.h and never reaches the libc. The failure
#     then surfaces as an unrelated parse error inside IDF (a missing
#     ESP_STATIC_ASSERT, an unknown `FILE`), which is why it reads like a dialect
#     problem and is not.
#   - Rockbox's headers must keep their own names. With IDF's flags first,
#     apps/main.c's `#include "rtc.h"` landed on IDF's deprecated
#     esp_hw_support/include/soc/esp32s3/rtc.h, and rtc_init() became an implicit
#     declaration.
#
# firmware/include holds both assert.h (which must lose to the libc) and rtc.h
# (which must win over IDF), so no permutation of -I flags settles it. What does
# settle it is -idirafter on the two directories that hold libc replacements, and
# only for the firmware domain: the freestanding domain has no system libc to
# reach, and needs those headers ahead of everything instead. Both are set in
# firmware/firmware.make.
#
# -Wno-undef is not cosmetic either. Rockbox builds with -Wundef, and IDF headers
# test generated sdkconfig names bare (`#if CONFIG_COMPILER_OPTIMIZATION_PERF`,
# `#if __clang__`), which produces hundreds of warnings per file. IDF's headers
# are not ours to fix.
#
# HAVE_SYSTEM_LIBC is defined here rather than in the target's config header for
# the same reason the include directories are split: it describes what the link
# contains, and only this domain's link contains a C library. The freestanding
# domain must not see it -- see the note in config/meowkits3.h. PPCFLAGS carries it
# too because that is where firmware/SOURCES and firmware/asm/SOURCES get their
# `#if`s evaluated.
CFLAGS = $(BASE_CFLAGS) $(LIBC_HDR_AFTER) $(IDF_CFLAGS) -DHAVE_SYSTEM_LIBC -Wno-undef
PPCFLAGS += -DHAVE_SYSTEM_LIBC
#
# Include directories, and only include directories, do go to PPCFLAGS. The
# invariant that matters is that IDF's CONFIG_ defines must not reach a SOURCES
# file, because that would let IDF's configuration decide which Rockbox files get
# compiled; IDF's search paths decide nothing of the kind. They are needed because
# mkdepfile scans with PPCFLAGS under -MG, which does not fail on an unresolved
# include -- it emits the name as written, and addtargetdir.pl then turns
# `esp_err.h` into $(BUILDDIR)/esp_err.h, a prerequisite with no rule, which is a
# hard make error rather than a missing dependency.
PPCFLAGS += $(filter -I%,$(IDF_CFLAGS)) -Wno-undef

# One more ordering case, and this one is a name collision rather than a
# preference. A firmware object that reaches an ESP-IDF driver can pull IDF's
# FreeRTOS headers, and FreeRTOS.h includes <queue.h> to get its own queue API.
# Rockbox has a queue.h as well, in firmware/kernel/include, and the firmware
# domain puts that directory on the path ahead of IDF's. The two files even share
# a guard name (QUEUE_H), so IDF's is skipped entirely and every FreeRTOS type
# declared after it comes out unknown -- reported as errors inside IDF's
# idf_additions.h, which reads like a broken toolchain and is really two
# projects' headers arriving at the same name. Objects in that group are compiled
# with IDF's search path leading; none of them use Rockbox's queue API.
IDF_FIRST_CFLAGS = $(IDF_CFLAGS) $(BASE_CFLAGS) $(LIBC_HDR_AFTER) \
                   -DHAVE_SYSTEM_LIBC -Wno-undef
endif
CORE_LDOPTS = $(GLOBAL_LDOPTS)  # linker ops specifically for core build

TOOLS = $(TOOLSDIR)/rdf2binary $(TOOLSDIR)/convbdf \
	$(TOOLSDIR)/codepages $(TOOLSDIR)/scramble $(TOOLSDIR)/bmp2rb \
	$(TOOLSDIR)/uclpack $(TOOLSDIR)/mkboot $(TOOLSDIR)/iaudio_bl_flash.c \
	$(TOOLSDIR)/iaudio_bl_flash.h
TOOLS += $(foreach tool,$(TOOLSET),$(TOOLSDIR)/$(tool))

ifeq (,$(PREFIX))
ifdef APP_TYPE
# for sims, set simdisk/ as default
ifeq ($(APP_TYPE),sdl-sim)
RBPREFIX = simdisk
else ifeq ($(APP_TYPE),sdl-app)
RBPREFIX = /usr/local
endif

INSTALL = --install="$(RBPREFIX)"
else
# /dev/null as magic to tell it wasn't set, error out later in buildzip.pl
INSTALL = --install=/dev/null
endif
else
RBPREFIX = $(PREFIX)
INSTALL = --install="$(RBPREFIX)"
endif

RBINFO = $(BUILDDIR)/rockbox-info.txt

# list suffixes to be understood by $*
.SUFFIXES: .rock .codec .map .elf .c .S .o .bmp .a

.PHONY: all clean tags zip tools manual bin build info langs

ifeq (,$(filter clean veryclean reconf tags voice voicetools manual manual-pdf manual-html manual-zhtml manual-txt manual-ztxt manual-zip manual-7zip help fontzip ,$(MAKECMDGOALS)))
# none of the above
DEPFILE = $(BUILDDIR)/make.dep

endif

all: $(DEPFILE) build

# Subdir makefiles. their primary purpose is to populate SRC, OTHER_SRC,
# ASMDEFS_SRC and CORE_LIBS. But they also define special dependencies and
# compile rules
include $(TOOLSDIR)/tools.make

ifeq (,$(findstring checkwps,$(APP_TYPE)))
  ifeq (,$(findstring database,$(APP_TYPE)))
    ifeq (,$(findstring warble,$(APP_TYPE)))
      include $(FIRMDIR)/firmware.make
      include $(ROOTDIR)/apps/bitmaps/bitmaps.make
      ifeq (arch_arm,$(ARCH))
          # some targets don't use the unwarminder because they have the glibc backtrace
          ifeq (,$(filter sonynwz,$(APP_TYPE)))
            include $(ROOTDIR)/lib/unwarminder/unwarminder.make
          endif
      endif
      ifeq (arch_mips,$(ARCH))
          # mips unwinder is only usable on native ports
          ifeq (,$(APP_TYPE))
            include $(ROOTDIR)/lib/mipsunwinder/mipsunwinder.make
          endif
      endif
      ifeq (,$(findstring bootloader,$(APPSDIR)))
        include $(ROOTDIR)/lib/skin_parser/skin_parser.make
        include $(ROOTDIR)/lib/tlsf/libtlsf.make
      endif
    endif
  endif
endif

#included before codecs.make and plugins.make so they see them)
ifndef APP_TYPE
  include $(ROOTDIR)/lib/libsetjmp/libsetjmp.make
  ifeq (arch_arm,$(ARCH))
    include $(ROOTDIR)/lib/arm_support/arm_support.make
  endif
endif

ifeq (,$(NO_UTF8PROC))
  include $(ROOTDIR)/lib/utf8proc/utf8proc.make
endif

ifeq (,$(findstring checkwps,$(APP_TYPE)))
  include $(ROOTDIR)/lib/fixedpoint/fixedpoint.make
endif

ifneq (,$(findstring bootloader,$(APPSDIR)))
  ifneq (,$(findstring sonynwz,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/sonynwz/sonynwz.make
  else ifneq (,$(findstring hiby_x1600,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/hiby/hiby.make
  else ifneq (,$(findstring hibyos,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/hibyos.make
  else ifneq (,$(findstring fiio,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/fiio/fiio.make
  else ifneq (,$(findstring ingenic_x1000,$(MANUFACTURER)))
    include $(ROOTDIR)/firmware/target/mips/ingenic_x1000/x1000boot.make
  else
    include $(APPSDIR)/bootloader.make
  endif
else ifneq (,$(findstring checkwps,$(APP_TYPE)))
  include $(APPSDIR)/checkwps.make
  include $(ROOTDIR)/lib/skin_parser/skin_parser.make
else ifneq (,$(findstring database,$(APP_TYPE)))
  include $(APPSDIR)/database.make
else ifneq (,$(findstring warble,$(APP_TYPE)))
  include $(ROOTDIR)/lib/rbcodec/test/warble.make
  include $(ROOTDIR)/lib/tlsf/libtlsf.make
  include $(ROOTDIR)/lib/rbcodec/rbcodec.make
else # core
  include $(APPSDIR)/lang/lang.make
  include $(APPSDIR)/apps.make
  include $(ROOTDIR)/lib/rbcodec/rbcodec.make

  ifeq ($(ENABLEDPLUGINS),yes)
    include $(APPSDIR)/plugins/bitmaps/pluginbitmaps.make
    include $(APPSDIR)/plugins/plugins.make
  endif

  ifneq (,$(findstring sdl,$(APP_TYPE)))
    include $(ROOTDIR)/uisimulator/uisimulator.make
  endif

  ifneq (,$(findstring ypr0,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/samsungypr/ypr0/ypr0.make
  endif

  ifneq (,$(findstring ypr1,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/samsungypr/ypr1/ypr1.make
  endif

  ifneq (,$(findstring sonynwz,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/sonynwz/sonynwz.make
  endif

  ifneq (,$(findstring hiby_x1600,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/hiby/hiby.make
  endif

  ifneq (,$(findstring hibyos,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/hibyos.make
  endif

  ifneq (,$(findstring fiio,$(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/fiio/fiio.make
  endif

  ifneq (,$(findstring android_ndk, $(APP_TYPE)))
    include $(ROOTDIR)/firmware/target/hosted/ibasso/android_ndk.make
  else
    ifneq (,$(findstring android, $(APP_TYPE)))
	  include $(ROOTDIR)/android/android.make
    endif
  endif

  ifneq (,$(findstring rgnano, $(MODELNAME)))
   include $(ROOTDIR)/packaging/rgnano/rgnano.make
  endif

  ifneq (,$(findstring 3ds-homebrew, $(APP_TYPE)))
    include $(ROOTDIR)/packaging/3ds/3ds.make
  endif

endif # bootloader

# Include makefile for flashing/debugging with OpenOCD & GDB
ifeq (echor1,$(MODELNAME))
  include $(ROOTDIR)/tools/echoplayer/openocd.make
endif

# ESP32 targets have no bootloader stage to write, so flashing is one esptool
# call on the packed image. The simulator shares the arch but produces a host
# executable, so the gate is on the artifact rather than on the CPU.
ifeq (xtensa,$(CPU))
  ifneq (,$(findstring esp32,$(BINARY)))
    include $(ROOTDIR)/tools/espflash.make
  endif
endif

# One or more subdir makefiles requested --gc-sections?
ifdef CORE_GCSECTIONS
  # Do not use '--gc-sections' when compiling sdl-sim
  ifneq ($(findstring sdl-sim, $(APP_TYPE)), sdl-sim)
    CORE_LDOPTS += -Wl,--gc-sections
  endif
endif # CORE_GCSECTIONS

OBJ := $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
OBJ += $(BMP:.bmp=.o)
OBJ := $(call full_path_subst,$(ROOTDIR)/%,$(BUILDDIR)/%,$(OBJ))

build: $(TOOLS) $(BUILDDIR)/$(BINARY) $(CODECS) $(ROCKS) $(RBINFO)

$(RBINFO): $(BUILDDIR)/$(BINARY)
	$(SILENT)echo Creating $(@F)
	$(SILENT)$(TOOLSDIR)/mkinfo.pl $@

$(DEPFILE) dep:
	$(call PRINTS,Generating dependencies)
	$(call mkdepfile,$(DEPFILE)_,$(SRC))
	$(call mkdepfile,$(DEPFILE)_,$(OTHER_SRC:%.lua=))
	$(call mkdepfile,$(DEPFILE)_,$(ASMDEFS_SRC))
	$(call bmpdepfile,$(DEPFILE)_,$(BMP) $(PBMP))
	@mv $(DEPFILE)_ $(DEPFILE)

bin: $(DEPFILE) $(TOOLS) $(BUILDDIR)/$(BINARY) $(RBINFO)
rocks: $(DEPFILE) $(TOOLS) $(ROCKS)
codecs: $(DEPFILE) $(TOOLS) $(CODECS)
tools: $(TOOLS)

-include $(DEPFILE)

veryclean: clean
	$(SILENT)rm -rf $(TOOLS)

clean::
	$(SILENT)echo Cleaning build directory
	$(SILENT)rm -rf rockbox.zip rockbox.7z rockbox.tar rockbox.tar.gz \
		rockbox.tar.xz rockbox-full.* rockbox-fonts.* TAGS apps \
		firmware tools comsim sim lang lib manual \
		*.pdf *.a credits.raw rockbox.ipod bitmaps \
		pluginbitmaps UI256.bmp rockbox-full.zip html txt \
		rockbox-manual*.zip sysfont.h rockbox-info.txt voicefontids \
		*.wav *.mp3 *.voice *.talk $(CLEANOBJS) \
		$(LINKRAM) $(LINKROM) rockbox.elf rockbox.map rockbox.bin \
		make.dep rombox.elf rombox.map rombox.bin romstart.txt \
		$(BINARY) $(FLASHFILE) uisimulator bootloader flash $(BOOTLINK) \
		rockbox.apk lang_enum.h rbversion.h fontbundle.h themeassets.h 3ds rockbox.3dsx \
		rockbox.bnr rockbox.cia rockbox.icn rockbox.smdh

#### linking the binaries: ####

.SECONDEXPANSION:

ifeq (,$(findstring bootloader,$(APPSDIR)))
# not bootloader

OBJ += $(LANG_O)

ifndef APP_TYPE

## target build
CONFIGFILE := $(FIRMDIR)/export/config/$(MODELNAME).h
ifeq ($(wildcard $(FIRMDIR)/target/$(CPU)/$(MANUFACTURER)/app.lds),)
RAMLDS := $(FIRMDIR)/target/$(CPU)/app.lds
else
RAMLDS := $(FIRMDIR)/target/$(CPU)/$(MANUFACTURER)/app.lds
endif
LINKRAM := $(BUILDDIR)/ram.link
ROMLDS := $(FIRMDIR)/rom.lds
LINKROM := $(BUILDDIR)/rom.link

$(LINKRAM): $(RAMLDS) $(CONFIGFILE)
	$(call PRINTS,PP $(@F))
	$(call preprocess2file,$<,$@,)

$(LINKROM): $(ROMLDS)
	$(call PRINTS,PP $(@F))
	$(call preprocess2file,$<,$@,)

ifneq (xtensa,$(CPU))
# Note: make sure -Wl,--gc-sections comes before -T in the linker options.
# Having the latter first caused crashes on (at least) mini2g.
$(BUILDDIR)/rockbox.elf : $$(OBJ) $(FIRMLIB) $(VOICESPEEXLIB) $(CORE_LIBS) $$(LINKRAM)
	$(call PRINTS,LD $(@F))$(CC) $(GCCOPTS) -nostdlib -o $@ $(OBJ) \
		-L$(BUILDDIR)/firmware \
		-L$(RBCODEC_BLD)/codecs $(call a2lnk, $(VOICESPEEXLIB)) \
		-L$(BUILDDIR)/lib $(call a2lnk, $(CORE_LIBS)) \
		-lgcc $(CORE_LDOPTS) -T$(LINKRAM) \
		-Wl,-Map,$(BUILDDIR)/rockbox.map

$(BUILDDIR)/rombox.elf : $$(OBJ) $(FIRMLIB) $(VOICESPEEXLIB) $(CORE_LIBS) $$(LINKROM)
	$(call PRINTS,LD $(@F))$(CC) $(GCCOPTS) -nostdlib -o $@ $(OBJ) \
		-L$(BUILDDIR)/firmware \
		-L$(RBCODEC_BLD)/codecs $(call a2lnk, $(VOICESPEEXLIB)) \
		-L$(BUILDDIR)/lib $(call a2lnk, $(CORE_LIBS)) \
		-lgcc $(CORE_LDOPTS) -T$(LINKROM) \
		-Wl,-Map,$(BUILDDIR)/rombox.map

$(BUILDDIR)/rockbox.bin : $(BUILDDIR)/rockbox.elf
	$(call PRINTS,OC $(@F))$(call objcopy,$<,$@)

$(BUILDDIR)/rombox.bin : $(BUILDDIR)/rombox.elf
	$(call PRINTS,OC $(@F))$(call objcopy,$<,$@)
endif # !xtensa

ifeq (xtensa,$(CPU))
#### ESP-IDF-backed link ####
#
# Rockbox compiles and archives; ESP-IDF links. IDF owns the linker script, the
# startup (call_start_cpu0 -> esp_startup_start_app -> app_main), the cache/MMU
# and PSRAM bring-up, the C library and FreeRTOS. Those are precisely the things
# the bare-metal branch had to reimplement by hand, and doing it again here would
# be the entire difficulty of the port for no benefit. The bare-metal link lives
# on the tagged branch (baremetal-pre-idf).
#
# Consequence worth stating: Rockbox's own linker script (app.lds), crt0 and
# tools/esp-elf2image are not used on this path.

IDFDIR ?= $(ROOTDIR)/idf-app
IDF_PROJECT ?= rockbox
IDFPY ?= idf.py

# Everything Rockbox compiled that is not already an archive, in one archive.
#
# $(OBJ) is only the apps/ and other non-library sources -- a few dozen objects.
# The firmware sources live in FIRMLIB_OBJ and are already archived in
# libfirmware.a, which is a member of CORE_LIBS. Building librockbox.a from
# $(OBJ) alone and then excluding libfirmware.a from the link (on a duplicate
# symbol concern that does not apply here -- the two sets are disjoint) left every
# firmware symbol unreachable and the IDF link failed on a single undefined
# reference. Both are needed.
ROCKBOX_ARCHIVE := $(BUILDDIR)/lib/librockbox.a

$(ROCKBOX_ARCHIVE): $$(OBJ)
	$(call make_archive)

# VOICESPEEXLIB is not a codec: it is the second copy of libspeex that
# apps/voice_thread.c links against, built with VOICESPEEXFLAGS (no -DCODEC, no
# -ffunction-sections, -DROCKBOX_VOICE_CODEC) so the voice path can call the
# decoder directly from the firmware. Rockbox's own link rule listed it
# explicitly (see the rockbox.elf rule above); dropping it here showed up as
# undefined reference to `speex_decode_int' rather than as anything that named
# the cause. It has to be a prerequisite of the generated cmake file as well as
# linked, because nothing else in the tree builds it.
ROCKBOX_LINK_LIBS := $(ROCKBOX_ARCHIVE) $(CORE_LIBS) $(VOICESPEEXLIB)

# Hand the archive list to CMake rather than writing it in idf-app: which archives
# exist depends on what the Rockbox configuration enables (rbcodec, skin_parser,
# utf8proc and fixedpoint are all conditional), so a list maintained in two places
# would silently link a stale set.
#
# The linker mapping and the two CMakeLists are prerequisites as well: they are
# the inputs that decide where sections go and what the link line looks like, and
# without them a change to main/rockbox_memory.lf would be picked up only by
# accident.
$(IDFDIR)/rockbox_libs.cmake: $(ROCKBOX_LINK_LIBS) $(IDFDIR)/main/rockbox_memory.lf \
                              $(IDFDIR)/CMakeLists.txt $(IDFDIR)/main/CMakeLists.txt
	$(call PRINTS,GEN $(@F))
	$(SILENT)echo '# generated by tools/root.make -- do not edit' > $@
	$(SILENT)echo 'set(ROCKBOX_LIBS' >> $@
	$(SILENT)for l in $(ROCKBOX_LINK_LIBS); do echo "    $$l" >> $@; done
	$(SILENT)echo ')' >> $@
	# Nudge CMake to re-run. IDF builds the ninja graph itself, so registering the
	# generated file via CMAKE_CONFIGURE_DEPENDS makes ninja see it as an output
	# twice and refuse to build at all. Touching a file CMake already tracks is the
	# nudge that works, and it has to happen here rather than in CMake: this file is
	# generated after IDF has already been configured.
	$(SILENT)touch $(IDFDIR)/main/CMakeLists.txt

$(BUILDDIR)/$(BINARY): $(IDFDIR)/rockbox_libs.cmake
	$(call PRINTS,IDF $(@F))
	# sdkconfig.defaults only *seeds* sdkconfig: IDF writes sdkconfig once and then
	# ignores the defaults file forever after. A change there therefore does nothing
	# at all -- the flag lands in neither sdkconfig nor the harvested idf.make, and
	# the image is unchanged. Reseed by removing sdkconfig when the defaults are
	# newer, which is the only way IDF picks them up. This discards anything edited
	# by hand in sdkconfig; sdkconfig.defaults is the tracked source of truth.
	$(SILENT)if [ -f $(IDFDIR)/sdkconfig.defaults ] && \
		   [ $(IDFDIR)/sdkconfig.defaults -nt $(IDFDIR)/sdkconfig ]; then \
		echo "  IDF: reseeding sdkconfig from sdkconfig.defaults"; \
		rm -f $(IDFDIR)/sdkconfig; \
	fi
	$(SILENT)cd $(IDFDIR) && $(IDFPY) build
	$(SILENT)cp $(IDFDIR)/build/$(IDF_PROJECT).bin $@

# The flags IDF compiles its own sources with. tools/configure harvests this once;
# making it a target here means a rebuilt IDF graph (sdkconfig or a component set
# change) refreshes it instead of leaving Rockbox compiled against an obsolete
# sdkconfig -- that failure looks like a header found but a CONFIG_ define missing,
# which reads as a code bug and is not one.
# IDF's own compile graph. After `make clean` (which fullcleans IDF) this does not
# exist, and tools/idf-flags cannot harvest from nothing, so generate it with
# `reconfigure`: that runs CMake -- which is what emits compile_commands.json --
# without compiling the components. CMake itself re-runs when sdkconfig or a
# CMakeLists changes, so this rule only ever fires when the graph is gone.
$(IDFDIR)/build/compile_commands.json: $(IDFDIR)/CMakeLists.txt $(IDFDIR)/main/CMakeLists.txt \
                                       $(IDFDIR)/main/rockbox_memory.lf
	$(call PRINTS,IDF reconfigure)
	$(SILENT)cd $(IDFDIR) && $(IDFPY) reconfigure

$(BUILDDIR)/idf.make: $(IDFDIR)/build/compile_commands.json
	$(call PRINTS,IDF harvest compile flags)
	$(SILENT)$(TOOLSDIR)/idf-flags $(IDFDIR)/build -o $@

# Rockbox generated rockbox_libs.cmake, so clean removes it. What clean does NOT
# touch is IDF's build directory, and that is deliberate rather than lazy:
# SOURCES files are run through the *target compiler* while make parses the
# makefile, and those compilations need IDF's generated sdkconfig.h. Fullcleaning
# IDF from clean therefore breaks the very next `make`, at parse time, with
# "fatal error: sdkconfig.h: No such file or directory" -- and because a failed
# preprocess silently yields an empty file list, it does so in a way that looks
# like a missing source rather than a missing header. `idf-clean` is the goal that
# resets IDF state, and it rebuilds what it removes.
clean::
	$(SILENT)rm -f $(IDFDIR)/rockbox_libs.cmake

idf-clean:
	$(SILENT)cd $(IDFDIR) && $(IDFPY) fullclean
	$(SILENT)rm -f $(IDFDIR)/rockbox_libs.cmake
	# Re-establish the generated headers immediately: sdkconfig.h and
	# compile_commands.json are both needed before the next build can parse, and
	# reconfigure emits both without compiling the components.
	$(SILENT)cd $(IDFDIR) && $(IDFPY) reconfigure
	$(SILENT)$(TOOLSDIR)/idf-flags $(IDFDIR)/build -o $(BUILDDIR)/idf.make
.PHONY: idf-clean
else
$(BUILDDIR)/$(BINARY) : $(BUILDDIR)/rockbox.bin $(FLASHFILE)
	$(call PRINTS,SCRAMBLE $(notdir $@)) $(MKFIRMWARE) $< $@
endif

$(MAXOUTFILE):
	$(call PRINTS,Creating $(@F))
	$(SILENT)echo '#include "config.h"' > $(MAXINFILE)
	$(SILENT)echo "ROM_START" >> $(MAXINFILE)
	$(call preprocess2file,$(MAXINFILE),$(MAXOUTFILE))
	$(SILENT)rm $(MAXINFILE)

# iriver
$(BUILDDIR)/rombox.iriver: $(BUILDDIR)/rombox.bin
	$(call PRINTS,Build ROM file)$(MKFIRMWARE) $< $@

endif # !APP_TYPE
endif # !bootloader


voicetools:
	$(SILENT)$(MAKE) -C $(TOOLSDIR) CC=$(HOSTCC) AR=$(HOSTAR) rbspeexenc voicefont wavtrim

tags:
	$(SILENT)rm -f TAGS
	$(SILENT)etags -o $(BUILDDIR)/TAGS $(filter-out %.o,$(SRC) $(OTHER_SRC))

zip: $(BUILDDIR)/rockbox.zip
7zip: $(BUILDDIR)/rockbox.7z
tar: $(BUILDDIR)/rockbox.tar

fontzip: BUILDZIPOPTS=-f 1
fontzip: ZIPFILESUFFIX=-fonts
fontzip: NODEPS=1
fontzip: zip

font7zip: BUILDZIPOPTS=-f 1
font7zip: ZIPFILESUFFIX=-fonts
font7zip: NODEPS=1
font7zip: 7zip

fullzip: BUILDZIPOPTS=-f 2
fullzip: ZIPFILESUFFIX=-full
fullzip: zip

full7zip: BUILDZIPOPTS=-f 2
full7zip: ZIPFILESUFFIX=-full
full7zip: 7zip

fulltar: BUILDZIPOPTS=-f 2
fulltar: ZIPFILESUFFIX=-full
fulltar: tar

ifneq ($(NODEPS),)
$(BUILDDIR)/rockbox.zip:
else
$(BUILDDIR)/rockbox.zip: build
endif
	$(call PRINTS,ZIP rockbox$(ZIPFILESUFFIX).zip)
	$(SILENT)$(TOOLSDIR)/buildzip.pl $(VERBOSEOPT) --app=$(APPLICATION) -m \"$(MODELNAME)\" -i \"$(TARGET_ID)\" -o $(BUILDDIR)/rockbox$(ZIPFILESUFFIX).zip -r "$(ROOTDIR)" --rbdir="$(RBDIR)" $(BUILDZIPOPTS) $(TARGET) $(BINARY)
ifneq ($(REPRODUCIBLE_ZIP),)
	$(SILENT)strip-nondeterminism $(BUILDDIR)/rockbox$(ZIPFILESUFFIX).zip
endif

mapzip:
	$(SILENT)find . -name "*.map" | xargs zip rockbox-maps.zip

elfzip:
	$(SILENT)find . -name "*.elf" | xargs zip rockbox-elfs.zip

ifneq ($(NODEPS),)
$(BUILDDIR)/rockbox.7z:
else
$(BUILDDIR)/rockbox.7z: build
endif
	$(call PRINTS,7Z rockbox$(ZIPFILESUFFIX).7z)
	$(SILENT)$(TOOLSDIR)/buildzip.pl $(VERBOSEOPT) --app=$(APPLICATION) -m \"$(MODELNAME)\" -i \"$(TARGET_ID)\"  -o $(BUILDDIR)/rockbox$(ZIPFILESUFFIX).7z  -z "7za a -mx=9" -r "$(ROOTDIR)" --rbdir="$(RBDIR)" $(BUILDZIPOPTS) $(TARGET) $(BINARY)

ifneq ($(NODEPS),)
$(BUILDDIR)/rockbox.tar:
else
$(BUILDDIR)/rockbox.tar: build
endif
	$(SILENT)rm -f rockbox.tar
	$(call PRINTS,TAR $(notdir $@))
	$(SILENT)$(TOOLSDIR)/buildzip.pl $(VERBOSEOPT) --app=$(APPLICATION) -m \"$(MODELNAME)\" -i \"$(TARGET_ID)\"  -o $(BUILDDIR)/rockbox$(ZIPFILESUFFIX).tar -z "tar -cf" -r "$(ROOTDIR)" --rbdir="$(RBDIR)" $(BUILDZIPOPTS) $(TARGET) $(BINARY)

fullgzip: ZIPFILESUFFIX=-full
fullgzip: fulltar gzip
fullxz: ZIPFILESUFFIX=-full
fullxz: fulltar xz

gzip: tar
	$(call PRINTS,GZIP rockbox$(ZIPFILESUFFIX).tar.gz)
	$(SILENT)gzip -f9 rockbox$(ZIPFILESUFFIX).tar

xz: tar
	$(call PRINTS,XZ rockbox$(ZIPFILESUFFIX).tar.xz)
	$(SILENT)xz -f rockbox$(ZIPFILESUFFIX).tar

manual manual-pdf:
	$(SILENT)$(MAKE) -C $(MANUALDIR) OBJDIR=$(BUILDDIR)/manual manual-pdf
manual-html:
	$(SILENT)$(MAKE) -C $(MANUALDIR) OBJDIR=$(BUILDDIR)/manual manual-html
manual-zhtml: manual-zip
manual-txt:
	$(SILENT)$(MAKE) -C $(MANUALDIR) OBJDIR=$(BUILDDIR)/manual manual-txt
manual-ztxt:
	$(SILENT)$(MAKE) -C $(MANUALDIR) OBJDIR=$(BUILDDIR)/manual manual-txt-zip
manual-zip:
	$(SILENT)$(MAKE) -C $(MANUALDIR) OBJDIR=$(BUILDDIR)/manual manual-zip
manual-7zip:
	$(SILENT)$(MAKE) -C $(MANUALDIR) OBJDIR=$(BUILDDIR)/manual manual-7zip

ifdef TTS_ENGINE

voice: voicetools $(BUILDDIR)/apps/genlang-features
	$(SILENT)if [ -z "$$POOL" ] ; then \
		export POOL="$(BUILDDIR)/voice-pool" ; \
	fi;\
	mkdir -p $${POOL} ;\
	for lang in `echo $(VOICELANGUAGE) |sed "s/,/ /g"`; do $(TOOLSDIR)/voice.pl -V -l=$$lang -t=$(MODELNAME):`cat $(BUILDDIR)/apps/genlang-features` -i=$(TARGET_ID) -e="$(ENCODER)" -E="$(ENC_OPTS)" -s=$(TTS_ENGINE) -S="$(TTS_OPTS)"; done

talkclips: voicetools
	$(SILENT)if [ -z '$(TALKDIR)' ] ; then \
		echo "Must specify TALKDIR"; \
	else \
		for lang in `echo $(VOICELANGUAGE) |sed "s/,/ /g"`; do $(TOOLSDIR)/voice.pl -C -l=$$lang -e="$(ENCODER)" -E="$(ENC_OPTS)" -s=$(TTS_ENGINE) -S="$(TTS_OPTS)" $(FORCE) "$(TALKDIR)" ; done \
	fi

talkclips-force: FORCE=-F
talkclips-force: talkclips

endif

ifeq (,$(findstring android, $(APP_TYPE)))

simext1:
	$(SILENT)mkdir -p $@

bininstall: $(BUILDDIR)/$(BINARY)
	@echo "Installing your rockbox binary in your '$(RBPREFIX)' dir"
	$(SILENT)cp $(BINARY) "$(RBPREFIX)/.rockbox/"

install: simext1
	@echo "Installing your build in your '$(RBPREFIX)' dir"
	$(SILENT)$(TOOLSDIR)/buildzip.pl $(VERBOSEOPT) --app=$(APPLICATION) -m "$(MODELNAME)" -i "$(TARGET_ID)" $(INSTALL) -z "zip -r0" -r "$(ROOTDIR)" --rbdir="$(RBDIR)" -f 0 $(TARGET) $(BINARY)

fullinstall: simext1
	@echo "Installing a full setup in your '$(RBPREFIX)' dir"
	$(SILENT)$(TOOLSDIR)/buildzip.pl $(VERBOSEOPT) --app=$(APPLICATION) -m "$(MODELNAME)" -i "$(TARGET_ID)" $(INSTALL) -z "zip -r0" -r "$(ROOTDIR)" --rbdir="$(RBDIR)" -f 2 $(TARGET) $(BINARY)

symlinkinstall: simext1
	@echo "Installing a full setup with links in your '$(RBPREFIX)' dir"
	$(SILENT)$(TOOLSDIR)/buildzip.pl $(VERBOSEOPT) --app=$(APPLICATION) -m "$(MODELNAME)" -i "$(TARGET_ID)" $(INSTALL) -z "zip -r0" -r "$(ROOTDIR)" --rbdir="$(RBDIR)" -f 2 $(TARGET) $(BINARY) -l
endif

help:
	@echo "A few helpful make targets"
	@echo ""
	@echo "all            - builds a full Rockbox (default), including tools"
	@echo "bin            - builds only the Rockbox.<target name> file"
	@echo "rocks          - builds only plugins"
	@echo "codecs         - builds only codecs"
	@echo "dep            - regenerates make dependency database"
	@echo "clean          - cleans a build directory (not tools)"
	@echo "idf-clean      - reset ESP-IDF build state and regenerate its headers (IDF-backed targets)"
	@echo "veryclean      - cleans the build and tools directories"
	@echo "manual         - builds a manual (pdf)"
	@echo "manual-html    - HTML manual"
	@echo "manual-zip     - HTML manual (zipped)"
	@echo "manual-7zip    - HTML manual (7zipped)"
	@echo "manual-txt     - txt manual"
	@echo "zip            - creates a rockbox.zip of your build (no fonts)"
	@echo "gzip           - creates a rockbox.tar.gz of your build (no fonts)"
	@echo "xz             - creates a rockbox.tar.xz of your build (no fonts)"
	@echo "7zip           - creates a rockbox.7z of your build (no fonts)"
	@echo "fullzip        - creates a rockbox-full.zip of your build (with fonts)"
	@echo "full7zip       - creates a rockbox-full.zip of your build (with fonts)"
	@echo "fullgzip       - creates a rockbox-full.tar.gz of your build (with fonts)"
	@echo "fullxz         - creates a rockbox-full.tar.xz of your build (with fonts)"
	@echo "fontzip        - creates rockbox-fonts.zip"
	@echo "font7zip       - creates rockbox-fonts.7zip"
	@echo "mapzip         - creates rockbox-maps.zip with all .map files"
	@echo "elfzip         - creates rockbox-elfs.zip with all .elf files"
	@echo "tools          - builds the tools only"
	@echo "voice          - creates the voice clips (voice builds only)"
	@echo "voicetools     - builds the voice tools only"
	@echo "talkclips      - builds talkclips for everything under \$$TALKDIR, skipping existing clips"
	@echo "talkclips-force - builds talkclips for everything under \$$TALKDIR, overwriting all existing clips"
	@echo "install        - installs your build (at \$$PREFIX, defaults to simdisk/ for simulators (no fonts))"
	@echo "fullinstall    - installs your build (like install, but with fonts)"
	@echo "symlinkinstall - like fullinstall, but with links instead of copying files. (Good for developing on simulator)"
	@echo "reconf         - rerun configure with the same selection"

### general compile rules:

# when source and object are in different locations (normal):
$(BUILDDIR)/%.o: $(ROOTDIR)/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(ROOTDIR)/%.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@

# generated definitions for use in .S files
$(BUILDDIR)/%_asmdefs.h: $(ROOTDIR)/%_asmdefs.c
	$(call PRINTS,ASMDEFS $(@F))
	$(SILENT)mkdir -p $(dir $@)
	$(call asmdefs2file,$<,$@)

# when source and object are both in BUILDDIR (generated code):
%.o: %.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@

Makefile: $(TOOLSDIR)/configure
ifneq (reconf,$(MAKECMDGOALS))
	$(SILENT)echo "*** tools/configure is newer than Makefile. You should run 'make reconf'."
endif

reconf:
	$(SILENT$)$(TOOLSDIR)/configure $(CONFIGURE_OPTIONS)
