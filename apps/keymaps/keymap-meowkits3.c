/***************************************************************************
 * Button code definitions for the MeowKit ESP32-S3.
 *
 * Six inputs: A, B and a 4-direction joystick. The FT6336 panel is reported
 * as a 3x3 grid and apps/keymaps/keymap-touchscreen.c maps those cells; the
 * stick has to do the same thing, so the cardinal cells are repeated here.
 *
 * The joystick follows the scroll-wheel convention of the iriver and Sansa
 * targets: up and down are the wheel axis - move through a list, change a
 * setting, adjust volume - and left and right are the select axis.
 *
 * keymap-touchscreen.c owns get_context_mapping(); this file supplies the
 * target_get_context_mapping() that it chains to.
 ****************************************************************************/

#include "config.h"
#include "action.h"
#include "button.h"

/*
 * The format of the list is as follows
 * { Action Code,   Button code,    Prereq button code }
 * if there's no need to check the previous button's value, use BUTTON_NONE
 * Insert LAST_ITEM_IN_LIST at the end of each mapping
 */
static const struct button_mapping button_context_standard[]  = {
    /* Wheel axis: up and down move. */
    { ACTION_STD_PREV,        BUTTON_UP,                    BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,  BUTTON_UP|BUTTON_REPEAT,      BUTTON_NONE },
    { ACTION_STD_NEXT,        BUTTON_DOWN,                  BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,  BUTTON_DOWN|BUTTON_REPEAT,    BUTTON_NONE },

    /* Select axis: left backs out, right commits. */
    { ACTION_STD_OK,          BUTTON_A|BUTTON_REL,          BUTTON_A },
    { ACTION_STD_OK,          BUTTON_RIGHT,                 BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_LEFT,                  BUTTON_NONE },

    /* B is the menu key: tap it for the main menu, hold it for the quick
     * screen. That is what the panel's top-left and bottom-left cells do. */
    { ACTION_STD_MENU,        BUTTON_B|BUTTON_REL,          BUTTON_B },
    { ACTION_STD_QUICKSCREEN, BUTTON_B|BUTTON_REPEAT,       BUTTON_B },

    /* The same four directions from the touch grid. keymap-touchscreen.c
     * claims the left cell on a press and the right one on release; the top and
     * bottom cells only get here, in the list and tree contexts. */
    { ACTION_STD_PREV,        BUTTON_TOPMIDDLE,             BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,  BUTTON_TOPMIDDLE|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_NEXT,        BUTTON_BOTTOMMIDDLE,          BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,  BUTTON_BOTTOMMIDDLE|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_MIDLEFT,               BUTTON_NONE },
    { ACTION_STD_OK,          BUTTON_MIDRIGHT|BUTTON_REL,   BUTTON_MIDRIGHT },

    { ACTION_STD_CONTEXT,     BUTTON_A|BUTTON_REPEAT,       BUTTON_A },
    /* Screen lock, and the way out of it: A and B held together. While
     * locked, do_softlock() filters every other action (action.c:885-940)
     * and, under the default settings, button_enable_touch(false) switches
     * the panel off at the driver level as well, so this combo is the only
     * input that does anything. Mapping it is also what makes autolock
     * reachable at all: the settings can carry SEL_ACTION_AUTOLOCK, but
     * action_autosoftlock_init() only arms it when unlock_combo !=
     * BUTTON_NONE, and that happens solely once something produces
     * ACTION_STD_KEYLOCK. */
    { ACTION_STD_KEYLOCK,     BUTTON_A|BUTTON_B|BUTTON_REPEAT, BUTTON_A|BUTTON_B },
    LAST_ITEM_IN_LIST
}; /* button_context_standard */

static const struct button_mapping button_context_wps[]  = {
    /* Both buttons commit on release, so holding one never fires the tap
     * action first. */
    { ACTION_WPS_PLAY,        BUTTON_A|BUTTON_REL,          BUTTON_A },
    /* B is back, not stop. Leaving the now-playing screen should keep the music
     * running and drop back to the browser, which is what ACTION_WPS_MENU does:
     * gwps_leave_wps(true) then GO_TO_ROOT. It used to be ACTION_WPS_STOP here,
     * so backing out faded the track out first. */
    { ACTION_WPS_MENU,        BUTTON_B|BUTTON_REL,          BUTTON_B },
    { ACTION_WPS_CONTEXT,     BUTTON_B|BUTTON_REPEAT,       BUTTON_B },
    /* Stop moves to holding the play key, which is the convention on the other
     * few-button targets -- keymap-m3.c and keymap-shanlingq1.c both bind
     * ACTION_WPS_STOP to BUTTON_PLAY|BUTTON_REPEAT. */
    { ACTION_WPS_STOP,        BUTTON_A|BUTTON_REPEAT,       BUTTON_A },

    /* There is no volume key on this board, so the wheel axis is volume here,
     * the same as the top and bottom cells of the grid. */
    { ACTION_WPS_VOLUP,       BUTTON_UP,                    BUTTON_NONE },
    { ACTION_WPS_VOLUP,       BUTTON_UP|BUTTON_REPEAT,      BUTTON_NONE },
    { ACTION_WPS_VOLDOWN,     BUTTON_DOWN,                  BUTTON_NONE },
    { ACTION_WPS_VOLDOWN,     BUTTON_DOWN|BUTTON_REPEAT,    BUTTON_NONE },

    { ACTION_WPS_SEEKBACK,    BUTTON_LEFT|BUTTON_REPEAT,    BUTTON_NONE },
    { ACTION_WPS_SEEKFWD,     BUTTON_RIGHT|BUTTON_REPEAT,   BUTTON_NONE },
    { ACTION_WPS_STOPSEEK,    BUTTON_LEFT|BUTTON_REL,       BUTTON_LEFT|BUTTON_REPEAT },
    { ACTION_WPS_STOPSEEK,    BUTTON_RIGHT|BUTTON_REL,      BUTTON_RIGHT|BUTTON_REPEAT },
    /* Skip on release, hold to seek - the same shape as the grid's left and
     * right cells in keymap-touchscreen.c. */
    { ACTION_WPS_SKIPNEXT,    BUTTON_RIGHT|BUTTON_REL,      BUTTON_RIGHT },
    { ACTION_WPS_SKIPPREV,    BUTTON_LEFT|BUTTON_REL,       BUTTON_LEFT },
    /* A-B repeat markers. ab_repeat_buttons is enabled, so the setting is
     * documented and reachable, but without these the points themselves could
     * never be marked. A is the modifier: hold it, then press the direction.
     * This is the as3525 convention verbatim -- keymap-c200.c:66-69 and
     * keymap-e200.c:66-69 use POWER+LEFT, POWER+RIGHT and POWER+UP with
     * POWER as the prerequisite, and keymap-fuzeplus.c:93-95 does the same with
     * BACK. action.c's matcher requires an exact button_code and prefers an
     * exact prerequisite, so these are distinct events rather than ambiguous
     * ones: pressing A on its own reports BUTTON_A, and A-then-Left reports
     * BUTTON_A|BUTTON_LEFT with last->button == BUTTON_A.
     *
     * Worth knowing before using them: these two gestures are dual-purpose
     * upstream. When A-B repeat is not active they jump to the previous and
     * next directory instead of marking a point -- the *_PREVDIR and *_NEXTDIR
     * names say so -- which is why the manual lists the same gesture twice,
     * once for A-B and once for directory skipping. */
    { ACTION_WPS_ABSETA_PREVDIR, BUTTON_A|BUTTON_LEFT,       BUTTON_A },
    { ACTION_WPS_ABSETB_NEXTDIR, BUTTON_A|BUTTON_RIGHT,      BUTTON_A },
    { ACTION_WPS_ABRESET,        BUTTON_A|BUTTON_B,          BUTTON_A },
    /* Key lock. Held, not tapped: the tap is already A-B reset here, and a
     * pocket lock should not be reachable by an accidental press. Holding both
     * face buttons is the same gesture in every context, which matters because
     * the unlock gesture is remembered from whatever locked the panel
     * (action.c:887 compares cur->button against unlock_combo). */
    { ACTION_STD_KEYLOCK,        BUTTON_A|BUTTON_B|BUTTON_REPEAT, BUTTON_A|BUTTON_B },
    /* Bookmarks. Only the WPS context handles these -- apps/gui/wps.c:1053
     * lists them and :1062 creates one -- so they are mapped here and not in
     * the standard or tree contexts, where the action would go nowhere.
     * B is the modifier, following keymap-hm801.c:79-80 which uses
     * POWER+SELECT to list and POWER+DOWN to create. B alone stays menu and
     * B held stays context, because action.c:591-604 matches the button code
     * exactly and prefers an exact prerequisite. */
    { ACTION_WPS_CREATE_BOOKMARK, BUTTON_B|BUTTON_UP,        BUTTON_B },
    { ACTION_WPS_LIST_BOOKMARKS,  BUTTON_B|BUTTON_DOWN,      BUTTON_B },
    LAST_ITEM_IN_LIST
}; /* button_context_wps */

#ifdef HAVE_RECORDING
/* The recording screen. There is no REC key on this board, so the play/pause
 * key takes the role, which is what every target without a dedicated record
 * button does: clip uses SELECT, fuzeplus uses PLAYPAUSE, fiiom3k uses PLAY,
 * and A is our play/pause on the WPS. Without this context the screen falls
 * through to button_context_standard, which never produces ACTION_REC_PAUSE --
 * so the recording screen could be opened but no key could start or stop a
 * recording.
 *
 * The stick's left/right become value adjusters for whichever item is
 * highlighted (the screen handles ACTION_SETTINGS_INC/DEC for volume, balance,
 * base, treble), matching keymap-fuzeplus. Up/down still move through the list
 * by falling through to CONTEXT_STD, and B backs out -- consistent with the
 * WPS, where B leaves the screen without stopping playback.
 *
 * The explicit LEFT/RIGHT entries are listed before the fallthrough on purpose:
 * the mapping lists are searched in order, so CONTEXT_STD's LEFT=cancel and
 * RIGHT=ok would otherwise win and the values could not be changed. */
static const struct button_mapping button_context_recscreen[]  = {
    { ACTION_REC_PAUSE,       BUTTON_A|BUTTON_REL,          BUTTON_A },
    { ACTION_REC_NEWFILE,     BUTTON_B|BUTTON_REPEAT,       BUTTON_B },

    { ACTION_SETTINGS_INC,    BUTTON_RIGHT,                 BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT, BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_SETTINGS_DEC,    BUTTON_LEFT,                  BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT, BUTTON_LEFT|BUTTON_REPEAT, BUTTON_NONE },

    /* Back out of the recording screen. Deliberately not MENU: on this target B
     * means "leave this screen" everywhere else too. */
    { ACTION_STD_CANCEL,      BUTTON_B|BUTTON_REL,          BUTTON_B },

    /* Same gestures from the touch grid: right cell records, left cell
    /* leaves. */
    { ACTION_REC_PAUSE,       BUTTON_MIDRIGHT|BUTTON_REL,   BUTTON_MIDRIGHT },
    { ACTION_STD_CANCEL,      BUTTON_MIDLEFT,               BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_recscreen */
#endif /* HAVE_RECORDING */

static const struct button_mapping button_context_yesno[] = {
    { ACTION_YESNO_ACCEPT,    BUTTON_A|BUTTON_REL,          BUTTON_A },
    { ACTION_STD_CANCEL,      BUTTON_B|BUTTON_REL,          BUTTON_B },
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_yesno */

/* In the settings list the wheel axis changes the highlighted value and the
 * select axis moves between items, which is how the grid's top/bottom and
 * left/right cells are already mapped. */
static const struct button_mapping button_context_settings[] = {
    { ACTION_SETTINGS_INC,        BUTTON_UP,                BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT,  BUTTON_UP|BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_SETTINGS_DEC,        BUTTON_DOWN,              BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT,  BUTTON_DOWN|BUTTON_REPEAT,BUTTON_NONE },
    { ACTION_STD_PREV,            BUTTON_LEFT,              BUTTON_NONE },
    { ACTION_STD_NEXT,            BUTTON_RIGHT,             BUTTON_NONE },
    { ACTION_STD_OK,              BUTTON_A|BUTTON_REL,      BUTTON_A },
    { ACTION_STD_CANCEL,          BUTTON_B|BUTTON_REL,      BUTTON_B },
    LAST_ITEM_IN_LIST
}; /* button_context_settings */

/* The EQ, colour-picker and time screens take left and right as decrease and
 * increase and up and down as move through the list. keymap-touchscreen.c only
 * spells this out for the first of them, and leaves the top and bottom cells
 * unclaimed, so the stick and the missing cells are added here. Repeating a
 * cell that that table already maps is harmless: it is matched before this one
 * is reached. */
static const struct button_mapping stick_context_settings_lr_inc[] = {
    { ACTION_SETTINGS_INC,        BUTTON_RIGHT,               BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT,  BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_SETTINGS_DEC,        BUTTON_LEFT,                BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT,  BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_SETTINGS_INC,        BUTTON_MIDRIGHT,            BUTTON_NONE },
    { ACTION_SETTINGS_DEC,        BUTTON_MIDLEFT,             BUTTON_NONE },
    { ACTION_STD_PREV,            BUTTON_UP,                  BUTTON_NONE },
    { ACTION_STD_NEXT,            BUTTON_DOWN,                BUTTON_NONE },
    { ACTION_STD_PREV,            BUTTON_TOPMIDDLE,           BUTTON_NONE },
    { ACTION_STD_NEXT,            BUTTON_BOTTOMMIDDLE,        BUTTON_NONE },
    { ACTION_STD_OK,              BUTTON_A|BUTTON_REL,        BUTTON_A },
    { ACTION_STD_CANCEL,          BUTTON_B|BUTTON_REL,        BUTTON_B },
    LAST_ITEM_IN_LIST
}; /* stick_context_settings_lr_inc */

/* The line-mode keyboard. This target has no keyboard context of its own, so
 * before this the stick did nothing at all in that screen: keymap-touchscreen.c
 * maps ACTION_KBD_* onto the 3x3 grid, and those cells drive actions rather
 * than characters, so at 320x240 neither the stick nor the panel could type.
 *
 * The gestures copy the semantics of keymap-touchscreen.c's keyboard table, in
 * particular the distinction it relies on: select is a *tap* and done is a
 * *hold* of the same control (BUTTON_CENTER vs BUTTON_CENTER|BUTTON_REPEAT at
 * lines 265-266). Up/down move between rows, left/right move along a row.
 *
 * Deliberately absent: cursor gestures. keyboard.c:713-725 already rewrites
 * ACTION_KBD_LEFT/RIGHT into cursor movement whenever the keyboard is in
 * line-edit or morse mode, so LEFT and RIGHT serve both jobs on their own. A
 * separate hold-based cursor mapping would be redundant, and because the
 * dispatcher returns the first match it would also shadow the row move.
 */
static const struct button_mapping button_context_keyboard[] = {
    { ACTION_KBD_UP,           BUTTON_UP,                   BUTTON_NONE },
    { ACTION_KBD_UP,           BUTTON_UP|BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_KBD_DOWN,         BUTTON_DOWN,                 BUTTON_NONE },
    { ACTION_KBD_DOWN,         BUTTON_DOWN|BUTTON_REPEAT,   BUTTON_NONE },

    { ACTION_KBD_LEFT,         BUTTON_LEFT,                 BUTTON_NONE },
    { ACTION_KBD_LEFT,         BUTTON_LEFT|BUTTON_REPEAT,   BUTTON_NONE },
    { ACTION_KBD_RIGHT,        BUTTON_RIGHT,                BUTTON_NONE },
    { ACTION_KBD_RIGHT,        BUTTON_RIGHT|BUTTON_REPEAT,  BUTTON_NONE },

    { ACTION_KBD_SELECT,       BUTTON_A|BUTTON_REL,         BUTTON_A },
    { ACTION_KBD_DONE,         BUTTON_A|BUTTON_REPEAT,      BUTTON_A },
    { ACTION_KBD_ABORT,        BUTTON_B|BUTTON_REL,         BUTTON_B },
    { ACTION_KBD_BACKSPACE,    BUTTON_B|BUTTON_REPEAT,      BUTTON_B },
    /* Both face buttons together flips between the character sets. This is the
     * ipod convention for a two-key gesture (keymap-ipod.c:61 uses
     * SELECT|PLAY for the tree hotkey). */
    { ACTION_KBD_PAGE_FLIP,    BUTTON_A|BUTTON_B,           BUTTON_NONE },

    LAST_ITEM_IN_LIST
}; /* button_context_keyboard */

const struct button_mapping* target_get_context_mapping(int context)
{
    switch (context & ~CONTEXT_LOCKED)
    {
        case CONTEXT_STD:
            return button_context_standard;
        case CONTEXT_WPS:
            return button_context_wps;
        case CONTEXT_YESNOSCREEN:
            return button_context_yesno;
#ifdef HAVE_RECORDING
        case CONTEXT_RECSCREEN:
            return button_context_recscreen;
#endif
        case CONTEXT_SETTINGS:
        case CONTEXT_SETTINGS_TIME:
            return button_context_settings;
        case CONTEXT_CUSTOM|CONTEXT_SETTINGS:
        case CONTEXT_SETTINGS_EQ:
        case CONTEXT_SETTINGS_COLOURCHOOSER:
            return stick_context_settings_lr_inc;
        /* keymap-touchscreen.c's keyboard list chains here via
         * LAST_ITEM_IN_LIST__NEXTLIST, so this is reached for the keyboard and
         * for the morse input screen. */
        case CONTEXT_KEYBOARD:
        case CONTEXT_MORSE_INPUT:
            return button_context_keyboard;
        case CONTEXT_TREE:
        case CONTEXT_LIST:
        case CONTEXT_MAINMENU:
        default:
            return button_context_standard;
    }

    return button_context_standard;
}
