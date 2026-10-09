/***************************************************************************
 * Simulator button map for the MeowKit ESP32-S3.
 *
 * The board has six keys: A, B and a four-way joystick. There is no power key
 * among them - the PMU's PEK input is the only way to turn the player off, and
 * the simulator has no PMU.
 *
 * The hotspot coordinates below are the positions drawn in
 * uisimulator/bitmaps/UI-meowkits3.bmp; changing the artwork means changing
 * them.
 ****************************************************************************/

#include <SDL.h>
#include "button.h"
#include "buttonmap.h"

int key_to_button(int keyboard_button)
{
    switch (keyboard_button)
    {
        case SDLK_UP:
            return BUTTON_UP;
        case SDLK_DOWN:
            return BUTTON_DOWN;
        case SDLK_LEFT:
            return BUTTON_LEFT;
        case SDLK_RIGHT:
            return BUTTON_RIGHT;
        case SDLK_a:
            return BUTTON_A;
        case SDLK_b:
            return BUTTON_B;
        default:
            return BUTTON_NONE;
    }
}

struct button_map bm[] = {
    { SDLK_UP,    120, 331, 14, "Up" },
    { SDLK_DOWN,  120, 389, 14, "Down" },
    { SDLK_LEFT,   91, 360, 14, "Left" },
    { SDLK_RIGHT, 149, 360, 14, "Right" },
    { SDLK_a,     260, 360, 20, "A" },
    { SDLK_b,     320, 360, 20, "B" },
    { 0, 0, 0, 0, "None" }
};
