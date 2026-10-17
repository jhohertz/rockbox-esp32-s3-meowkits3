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
    LAST_ITEM_IN_LIST
}; /* button_context_wps */

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
        case CONTEXT_SETTINGS:
        case CONTEXT_SETTINGS_TIME:
            return button_context_settings;
        case CONTEXT_CUSTOM|CONTEXT_SETTINGS:
        case CONTEXT_SETTINGS_EQ:
        case CONTEXT_SETTINGS_COLOURCHOOSER:
            return stick_context_settings_lr_inc;
        case CONTEXT_TREE:
        case CONTEXT_LIST:
        case CONTEXT_MAINMENU:
        default:
            return button_context_standard;
    }

    return button_context_standard;
}
