#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

PUZZLES_SRCDIR = $(APPSDIR)/plugins/puzzles
PUZZLES_OBJDIR = $(BUILDDIR)/apps/plugins/puzzles

PUZZLES_SHARED_SRC = $(call preprocess, $(PUZZLES_SRCDIR)/SOURCES)
PUZZLES_SHARED_OBJ = $(call c2obj, $(PUZZLES_SHARED_SRC))

PUZZLES_GAMES_SRC = $(call preprocess, $(PUZZLES_SRCDIR)/SOURCES.games)
PUZZLES_GAMES_OBJ = $(call c2obj, $(PUZZLES_GAMES_SRC))

PUZZLES_HELP_SRC = $(wildcard $(PUZZLES_SRCDIR)/help/*)
PUZZLES_HELP_OBJ = $(call c2obj, $(PUZZLES_HELP_OBJ))

PUZZLES_SRC = $(PUZZLES_GAMES_SRC) $(PUZZLES_SHARED_SRC) $(PUZZLES_HELP_SRC)
PUZZLES_OBJ = $(call c2obj, $(PUZZLES_SRC))

PUZZLES_ROCKS = $(addprefix $(PUZZLES_OBJDIR)/sgt-, $(notdir $(PUZZLES_GAMES_SRC:.c=.rock)))

OTHER_SRC += $(PUZZLES_SRC)
OTHER_INC += -I$(PUZZLES_SRCDIR)/src -I $(PUZZLES_SRCDIR)

ROCKS += $(PUZZLES_ROCKS)

PUZZLES_OPTIMIZE = -O2

ifeq ($(MODELNAME), sansac200v2)
PUZZLES_OPTIMIZE = -Os # tiny plugin buffer
endif

# we suppress all warnings with -w
PUZZLESFLAGS = -I$(PUZZLES_SRCDIR)/dummy $(filter-out			\
		-O%,$(PLUGINFLAGS)) $(PUZZLES_OPTIMIZE)			\
		-Wno-unused-parameter -Wno-sign-compare			\
		-Wno-strict-aliasing -DFOR_REAL				\
		-I$(PUZZLES_SRCDIR)/src -I$(PUZZLES_SRCDIR) -include	\
		$(PUZZLES_SRCDIR)/rbcompat.h -ffunction-sections	\
		-fdata-sections -w

# Each .rock needs an EXPLICIT rule, not a pattern rule -- and the reason is a
# make behaviour that makes the pattern form fail invisibly.
#
# During pattern-rule selection make checks a candidate's explicitly listed
# prerequisites for EXISTENCE only; it does not try to build them. pattern_search()
# calls check_dep() and `make -d` shows it plainly:
#
#     Trying rule prerequisite '.../puzzles/rbwrappers.o'.
#     Not found '.../puzzles/rbwrappers.o'.
#
# Any absent prerequisite rejects the whole candidate. make then falls through to
# plugins.make's `$(BUILDDIR)/%.rock:`, which has no prerequisites at all, so it
# always matches -- and links nothing but plugin_crt0.o and the plugin libraries.
# The failure surfaces as `undefined reference to plugin_start`, which points
# nowhere near the cause.
#
# The shared objects cannot exist before the first link, so the pattern form could
# never fire on a clean tree: the rule was rejected precisely because the objects
# it would have caused to be compiled were the ones it was checking for.
# mikmod.make does not hit this because its .rock rule is explicit. This does the
# same thing, generated once per game, with the recipe unchanged.
#
# Two source directories, matching the two pattern rules this replaces: games live
# in src/, and a few in src/unfinished/. c2obj reproduces the object path either
# way, so no special-casing is needed.
define PUZZLES_ROCK_RULE
$(PUZZLES_OBJDIR)/sgt-$(1).rock: $(2) $(PUZZLES_OBJDIR)/help/$(1).o \
        $(PUZZLES_SHARED_OBJ) $(TLSFLIB)
	$$(call PRINTS,LD $$(@F))$$(CC) $$(PLUGINFLAGS) -o $(PUZZLES_OBJDIR)/$(1).elf \
		$$(filter %.o, $$^) \
		$$(filter %.a, $$+) \
		-lgcc $$(filter-out -Wl%.map, $$(PLUGINLDFLAGS)) -Wl,$$(LDMAP_OPT),$(PUZZLES_OBJDIR)/src/$(1).map
	$$(SILENT)$$(call objcopy_plugin,$(PUZZLES_OBJDIR)/$(1).elf,$$@)

endef

$(foreach src,$(PUZZLES_GAMES_SRC), \
    $(eval $(call PUZZLES_ROCK_RULE,$(basename $(notdir $(src))),$(call c2obj,$(src)))))

$(PUZZLES_SRCDIR)/rbcompat.h:	$(APPSDIR)/plugin.h			\
				$(APPSDIR)/plugins/lib/pluginlib_exit.h	\
				$(BUILDDIR)/sysfont.h			\
				$(PUZZLES_SRCDIR)/rbassert.h		\
				$(TLSFLIB_DIR)/src/tlsf.h \
				$(BUILDDIR)/lang_enum.h

# special pattern rule for compiling puzzles with extra flags
$(PUZZLES_OBJDIR)/%.o: $(PUZZLES_SRCDIR)/%.c $(PUZZLES_SRCDIR)/puzzles.make $(PUZZLES_SRCDIR)/rbcompat.h
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -I$(dir $<) $(PUZZLESFLAGS) -c $< -o $@
