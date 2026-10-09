#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

XTENSASUPPORTLIB_DIR := $(ROOTDIR)/lib/xtensa_support
XTENSASUPPORTLIB_SRC := $(XTENSASUPPORTLIB_DIR)/xtensa_support.c \
                       $(XTENSASUPPORTLIB_DIR)/softfloat.c
XTENSASUPPORTLIB_OBJ := $(call c2obj, $(XTENSASUPPORTLIB_SRC))
XTENSASUPPORTLIB := $(BUILDDIR)/lib/libxtensa_support.a

OTHER_SRC += $(XTENSASUPPORTLIB_SRC)
# both core and plugins link this
CORE_LIBS += $(XTENSASUPPORTLIB)
PLUGIN_LIBS += $(XTENSASUPPORTLIB)

$(XTENSASUPPORTLIB): $(XTENSASUPPORTLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null
