/***************************************************************************
 * Button code definitions for the MeowKit ESP32-S3.
 *
 * Six inputs only: A, B and a 4-direction joystick.
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
    { ACTION_STD_PREV,        BUTTON_UP,                    BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,  BUTTON_UP|BUTTON_REPEAT,      BUTTON_NONE },
    { ACTION_STD_NEXT,        BUTTON_DOWN,                  BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,  BUTTON_DOWN|BUTTON_REPEAT,    BUTTON_NONE },

    { ACTION_STD_OK,          BUTTON_A|BUTTON_REL,          BUTTON_A },
    { ACTION_STD_OK,          BUTTON_RIGHT,                 BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_B,                     BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_LEFT,                  BUTTON_NONE },

    { ACTION_STD_CONTEXT,     BUTTON_A|BUTTON_REPEAT,       BUTTON_A },
    { ACTION_STD_MENU,        BUTTON_B|BUTTON_REPEAT,       BUTTON_B },
    { ACTION_STD_QUICKSCREEN, BUTTON_B|BUTTON_REL,          BUTTON_B },
    { ACTION_STD_HOTKEY,      BUTTON_UP|BUTTON_REL,         BUTTON_UP },
    LAST_ITEM_IN_LIST
}; /* button_context_standard */

static const struct button_mapping button_context_wps[]  = {
    { ACTION_WPS_PLAY,        BUTTON_A,                     BUTTON_NONE },
    { ACTION_WPS_PLAY,        BUTTON_A|BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_WPS_STOP,        BUTTON_B,                     BUTTON_NONE },
    { ACTION_WPS_SEEKBACK,    BUTTON_LEFT|BUTTON_REPEAT,    BUTTON_NONE },
    { ACTION_WPS_SEEKFWD,     BUTTON_RIGHT|BUTTON_REPEAT,   BUTTON_NONE },
    { ACTION_WPS_STOPSEEK,    BUTTON_LEFT|BUTTON_REL,       BUTTON_LEFT|BUTTON_REPEAT },
    { ACTION_WPS_STOPSEEK,    BUTTON_RIGHT|BUTTON_REL,      BUTTON_RIGHT|BUTTON_REPEAT },
    { ACTION_WPS_SKIPNEXT,    BUTTON_RIGHT,                 BUTTON_NONE },
    { ACTION_WPS_SKIPPREV,    BUTTON_LEFT,                  BUTTON_NONE },
    { ACTION_WPS_BROWSE,      BUTTON_A|BUTTON_REL,          BUTTON_A },
    { ACTION_WPS_CONTEXT,     BUTTON_A|BUTTON_REPEAT,       BUTTON_A },
    { ACTION_WPS_MENU,        BUTTON_B|BUTTON_REPEAT,       BUTTON_B },
    { ACTION_WPS_HOTKEY,      BUTTON_UP|BUTTON_REL,         BUTTON_UP },
    LAST_ITEM_IN_LIST
}; /* button_context_wps */

static const struct button_mapping button_context_yesno[] = {
    { ACTION_YESNO_ACCEPT,    BUTTON_A,                     BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_B,                     BUTTON_NONE },
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_yesno */

const struct button_mapping* get_context_mapping(int context)
{
    switch (context)
    {
        case CONTEXT_STD:
            return button_context_standard;
        case CONTEXT_WPS:
            return button_context_wps;
        case CONTEXT_YESNOSCREEN:
            return button_context_yesno;
        case CONTEXT_TREE:
        case CONTEXT_LIST:
        case CONTEXT_MAINMENU:
        case CONTEXT_SETTINGS:
        default:
            return button_context_standard;
    }

    return button_context_standard;
}
