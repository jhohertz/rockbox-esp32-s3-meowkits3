#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

# we need to build two different speex libraries
# (one for codec, one for core voice)
# so a little trickery is necessary

# disable strict aliasing optimizations for now, it gives warnings due to
# some horrid typecasting
SPEEXFLAGS = $(filter-out -fstrict-aliasing, $(CODECFLAGS)) \
		-fno-strict-aliasing -DHAVE_CONFIG_H -DSPEEX_DISABLE_ENCODER \
		-I$(RBCODECLIB_DIR)/codecs/libspeex

# The voice library is linked into the firmware image, not into a codec image.
# codecs.make appends -mno-longcalls for xtensa because a codec is filled through
# the data cache window and executed through the instruction one, so an absolute
# call literal would name the wrong bus. That reasoning does not hold here: this
# archive runs in .text alongside the rest of the firmware, and its memset/memcpy
# calls resolve to the ROM at 0x400011xx -- more than 1 MB from the code -- so a
# plain call8 fails the link with "dangerous relocation: call8: call target out of
# range". Drop the codec-image flag and let the firmware's -mlongcalls apply.
VOICESPEEXFLAGS = $(filter-out -ffunction-sections -mno-longcalls, \
				  $(filter-out -DCODEC,$(SPEEXFLAGS))) -DROCKBOX_VOICE_CODEC

# libspeex
SPEEXLIB := $(CODECDIR)/libspeex.a
SPEEXLIB_SRC := $(call preprocess, $(RBCODECLIB_DIR)/codecs/libspeex/SOURCES)
SPEEXLIB_OBJ := $(call c2obj, $(SPEEXLIB_SRC))
OTHER_SRC += $(SPEEXLIB_SRC)

$(SPEEXLIB): $(SPEEXLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

# libspeex-voice
VOICESPEEXLIB := $(CODECDIR)/libspeex-voice.a
VOICESPEEXLIB_SRC := $(call preprocess, $(RBCODECLIB_DIR)/codecs/libspeex/SOURCES)
VOICESPEEXLIB_OBJ := $(addsuffix .o,$(basename $(subst $(RBCODECLIB_DIR)/codecs/libspeex,$(RBCODEC_BLD)/codecs/libspeex-voice,$(VOICESPEEXLIB_SRC))))

$(VOICESPEEXLIB): $(VOICESPEEXLIB_OBJ)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

# pattern rules

$(CODECDIR)/libspeex-voice/%.o : $(RBCODECLIB_DIR)/codecs/libspeex/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(VOICESPEEXFLAGS) -c $< -o $@

$(CODECDIR)/libspeex-voice/%.o : $(RBCODECLIB_DIR)/codecs/libspeex/%.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(VOICESPEEXFLAGS) -c $< -o $@

$(CODECDIR)/libspeex/%.o: $(RBCODECLIB_DIR)/codecs/libspeex/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(SPEEXFLAGS) -c $< -o $@

$(CODECDIR)/libspeex/%.o: $(RBCODECLIB_DIR)/codecs/libspeex/%.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(SPEEXFLAGS) -c $< -o $@
