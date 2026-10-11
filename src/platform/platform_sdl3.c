/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "platform/platform.h"
#include "input/input_manager.h"

#include <SDL3/SDL.h>
#ifdef RMMZ_RENDER_GPU
#include "rendering/gpu_backend.h"
#include "rendering/renderer.h"
#else
#include "rendering/gl_loader.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Platform {
    SDL_Window   *window;
    SDL_GLContext  gl_ctx;

    SDL_Gamepad    *gamepads[INPUT_MAX_GAMEPADS];
    SDL_JoystickID  gamepad_ids[INPUT_MAX_GAMEPADS];

    /* Set when SDL_EVENT_WINDOW_RESIZED fires; consumed by platform_check_resize. */
    bool  resize_pending;
    int   resize_w;
    int   resize_h;

#ifdef RMMZ_PS5
    /* Pad buttons that press keys (the launcher's settings) */
    bool  kb_down[12];
    /* The DualSense touchpad as a mouse */
    int   tp_handle, tp_tries;
    bool  tp_active, tp_moved, tp_press, tp_press_used;
    int   tp_prev_x, tp_prev_y, tp_start_x, tp_start_y, tp_maxfingers, tp_button;
    Uint64 tp_start_ms, cursor_until;
    float cur_x, cur_y;
    uint32_t cursor_tex;
    int   cursor_scale;
#endif
};

/* SDL keycode -> DOM keyCode */

static int sdl_to_dom_keycode(SDL_Keycode key)
{
    /* Letters (DOM uses the uppercase ASCII code; SDL3 keycodes are lowercase). */
    if (key >= SDLK_A && key <= SDLK_Z)
        return (key - SDLK_A) + 65;

    if (key >= SDLK_0 && key <= SDLK_9)
        return key;

    /* F1..F12 -> 112..123 */
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return 112 + (int)(key - SDLK_F1);

    /* Numpad 0..9 -> 96..105 */
    if (key >= SDLK_KP_0 && key <= SDLK_KP_9)
        return 96 + (int)(key - SDLK_KP_0);

    switch (key) {
        case SDLK_RETURN:       return 13;
        case SDLK_KP_ENTER:     return 13;
        case SDLK_ESCAPE:       return 27;
        case SDLK_BACKSPACE:    return 8;
        case SDLK_TAB:          return 9;
        case SDLK_SPACE:        return 32;
        case SDLK_LEFT:         return 37;
        case SDLK_UP:           return 38;
        case SDLK_RIGHT:        return 39;
        case SDLK_DOWN:         return 40;
        case SDLK_PAGEUP:       return 33;
        case SDLK_PAGEDOWN:     return 34;
        case SDLK_HOME:         return 36;
        case SDLK_END:          return 35;
        case SDLK_INSERT:       return 45;
        case SDLK_DELETE:       return 46;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:       return 16;
        case SDLK_LCTRL:
        case SDLK_RCTRL:        return 17;
        case SDLK_LALT:
        case SDLK_RALT:         return 18;
        case SDLK_LGUI:
        case SDLK_RGUI:         return 91;
        case SDLK_CAPSLOCK:     return 20;
        case SDLK_NUMLOCKCLEAR: return 144;
        case SDLK_SCROLLLOCK:   return 145;
        case SDLK_PAUSE:        return 19;
        case SDLK_PRINTSCREEN:  return 44;
        case SDLK_KP_MULTIPLY:  return 106;
        case SDLK_KP_PLUS:      return 107;
        case SDLK_KP_MINUS:     return 109;
        case SDLK_KP_DECIMAL:   return 110;
        case SDLK_KP_DIVIDE:    return 111;
        case SDLK_SEMICOLON:    return 186;
        case SDLK_EQUALS:       return 187;
        case SDLK_COMMA:        return 188;
        case SDLK_MINUS:        return 189;
        case SDLK_PERIOD:       return 190;
        case SDLK_SLASH:        return 191;
        case SDLK_GRAVE:        return 192;
        case SDLK_LEFTBRACKET:  return 219;
        case SDLK_BACKSLASH:    return 220;
        case SDLK_RIGHTBRACKET: return 221;
        case SDLK_APOSTROPHE:   return 222;
        default:                return 0;
    }
}

/* SDL keycode -> DOM "key" string */

static const char *sdl_to_dom_key(SDL_Keycode key, SDL_Keymod mod)
{
    static char buf[8];

    if (key >= SDLK_A && key <= SDLK_Z) {
        buf[0] = (mod & SDL_KMOD_SHIFT) ? (char)('A' + (key - SDLK_A))
                                        : (char)('a' + (key - SDLK_A));
        buf[1] = '\0';
        return buf;
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        buf[0] = (char)key;
        buf[1] = '\0';
        return buf;
    }

    switch (key) {
        case SDLK_RETURN:       return "Enter";
        case SDLK_KP_ENTER:     return "Enter";
        case SDLK_ESCAPE:       return "Escape";
        case SDLK_BACKSPACE:    return "Backspace";
        case SDLK_TAB:          return "Tab";
        case SDLK_SPACE:        return " ";
        case SDLK_LEFT:         return "ArrowLeft";
        case SDLK_UP:           return "ArrowUp";
        case SDLK_RIGHT:        return "ArrowRight";
        case SDLK_DOWN:         return "ArrowDown";
        case SDLK_PAGEUP:       return "PageUp";
        case SDLK_PAGEDOWN:     return "PageDown";
        case SDLK_HOME:         return "Home";
        case SDLK_END:          return "End";
        case SDLK_INSERT:       return "Insert";
        case SDLK_DELETE:       return "Delete";
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:       return "Shift";
        case SDLK_LCTRL:
        case SDLK_RCTRL:        return "Control";
        case SDLK_LALT:
        case SDLK_RALT:         return "Alt";
        case SDLK_LGUI:
        case SDLK_RGUI:         return "Meta";
        case SDLK_CAPSLOCK:     return "CapsLock";
        case SDLK_F1:           return "F1";
        case SDLK_F2:           return "F2";
        case SDLK_F3:           return "F3";
        case SDLK_F4:           return "F4";
        case SDLK_F5:           return "F5";
        case SDLK_F6:           return "F6";
        case SDLK_F7:           return "F7";
        case SDLK_F8:           return "F8";
        case SDLK_F9:           return "F9";
        case SDLK_F10:          return "F10";
        case SDLK_F11:          return "F11";
        case SDLK_F12:          return "F12";
        default:                break;
    }

    if (key >= SDLK_KP_0 && key <= SDLK_KP_9) {
        buf[0] = (char)('0' + (key - SDLK_KP_0));
        buf[1] = '\0';
        return buf;
    }

    return "Unidentified";
}

/* SDL scancode -> DOM "code" string */

static const char *sdl_to_dom_code(SDL_Scancode sc)
{
    static char buf[16];

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        snprintf(buf, sizeof(buf), "Key%c", 'A' + (sc - SDL_SCANCODE_A));
        return buf;
    }

    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        snprintf(buf, sizeof(buf), "Digit%c", '1' + (sc - SDL_SCANCODE_1));
        return buf;
    }
    if (sc == SDL_SCANCODE_0) return "Digit0";

    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
        snprintf(buf, sizeof(buf), "Numpad%c", '1' + (sc - SDL_SCANCODE_KP_1));
        return buf;
    }
    if (sc == SDL_SCANCODE_KP_0) return "Numpad0";

    switch (sc) {
        case SDL_SCANCODE_RETURN:       return "Enter";
        case SDL_SCANCODE_KP_ENTER:     return "NumpadEnter";
        case SDL_SCANCODE_ESCAPE:       return "Escape";
        case SDL_SCANCODE_BACKSPACE:    return "Backspace";
        case SDL_SCANCODE_TAB:          return "Tab";
        case SDL_SCANCODE_SPACE:        return "Space";
        case SDL_SCANCODE_LEFT:         return "ArrowLeft";
        case SDL_SCANCODE_UP:           return "ArrowUp";
        case SDL_SCANCODE_RIGHT:        return "ArrowRight";
        case SDL_SCANCODE_DOWN:         return "ArrowDown";
        case SDL_SCANCODE_PAGEUP:       return "PageUp";
        case SDL_SCANCODE_PAGEDOWN:     return "PageDown";
        case SDL_SCANCODE_HOME:         return "Home";
        case SDL_SCANCODE_END:          return "End";
        case SDL_SCANCODE_INSERT:       return "Insert";
        case SDL_SCANCODE_DELETE:       return "Delete";
        case SDL_SCANCODE_LSHIFT:       return "ShiftLeft";
        case SDL_SCANCODE_RSHIFT:       return "ShiftRight";
        case SDL_SCANCODE_LCTRL:        return "ControlLeft";
        case SDL_SCANCODE_RCTRL:        return "ControlRight";
        case SDL_SCANCODE_LALT:         return "AltLeft";
        case SDL_SCANCODE_RALT:         return "AltRight";
        case SDL_SCANCODE_LGUI:         return "MetaLeft";
        case SDL_SCANCODE_RGUI:         return "MetaRight";
        case SDL_SCANCODE_CAPSLOCK:     return "CapsLock";
        case SDL_SCANCODE_F1:           return "F1";
        case SDL_SCANCODE_F2:           return "F2";
        case SDL_SCANCODE_F3:           return "F3";
        case SDL_SCANCODE_F4:           return "F4";
        case SDL_SCANCODE_F5:           return "F5";
        case SDL_SCANCODE_F6:           return "F6";
        case SDL_SCANCODE_F7:           return "F7";
        case SDL_SCANCODE_F8:           return "F8";
        case SDL_SCANCODE_F9:           return "F9";
        case SDL_SCANCODE_F10:          return "F10";
        case SDL_SCANCODE_F11:          return "F11";
        case SDL_SCANCODE_F12:          return "F12";
        case SDL_SCANCODE_MINUS:        return "Minus";
        case SDL_SCANCODE_EQUALS:       return "Equal";
        case SDL_SCANCODE_LEFTBRACKET:  return "BracketLeft";
        case SDL_SCANCODE_RIGHTBRACKET: return "BracketRight";
        case SDL_SCANCODE_BACKSLASH:    return "Backslash";
        case SDL_SCANCODE_SEMICOLON:    return "Semicolon";
        case SDL_SCANCODE_APOSTROPHE:   return "Quote";
        case SDL_SCANCODE_GRAVE:        return "Backquote";
        case SDL_SCANCODE_COMMA:        return "Comma";
        case SDL_SCANCODE_PERIOD:       return "Period";
        case SDL_SCANCODE_SLASH:        return "Slash";
        default:                        return "Unidentified";
    }
}

/* SDL mouse button -> DOM button index */

static int sdl_to_dom_button(uint8_t sdl_button)
{
    switch (sdl_button) {
        case SDL_BUTTON_LEFT:   return 0;
        case SDL_BUTTON_MIDDLE: return 1;
        case SDL_BUTTON_RIGHT:  return 2;
        default:                return (int)sdl_button - 1;
    }
}

/* Event translation */

static void process_key_event(const SDL_KeyboardEvent *kev, InputEventType type)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = type;
    ev.keyCode = sdl_to_dom_keycode(kev->key);
    ev.repeat = kev->repeat;
    ev.shiftKey = (kev->mod & SDL_KMOD_SHIFT) != 0;
    ev.ctrlKey  = (kev->mod & SDL_KMOD_CTRL)  != 0;
    ev.altKey   = (kev->mod & SDL_KMOD_ALT)   != 0;
    ev.metaKey  = (kev->mod & SDL_KMOD_GUI)   != 0;

    const char *key_str = sdl_to_dom_key(kev->key, kev->mod);
    strncpy(ev.key, key_str, sizeof(ev.key) - 1);

    const char *code_str = sdl_to_dom_code(kev->scancode);
    strncpy(ev.code, code_str, sizeof(ev.code) - 1);

    input_manager_push_event(&ev);
}

static void process_text_input(const SDL_TextInputEvent *tev)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TEXT;
    if (tev->text) strncpy(ev.key, tev->text, sizeof(ev.key) - 1);
    input_manager_push_event(&ev);
}

static void process_mouse_button(const SDL_MouseButtonEvent *mev, InputEventType type)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = type;
    ev.mouseX = (int)mev->x;
    ev.mouseY = (int)mev->y;
    ev.button = sdl_to_dom_button(mev->button);

    input_manager_push_event(&ev);
}

static void process_mouse_motion(const SDL_MouseMotionEvent *mev)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = INPUT_MOUSE_MOVE;
    ev.mouseX = (int)mev->x;
    ev.mouseY = (int)mev->y;

    input_manager_push_event(&ev);
}

static void process_mouse_wheel(const SDL_MouseWheelEvent *wev)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = INPUT_WHEEL;

    double dx = (double)wev->x;
    double dy = (double)wev->y;
    if (wev->direction == SDL_MOUSEWHEEL_FLIPPED) {
        dx = -dx;
        dy = -dy;
    }
    /* DOM: positive deltaY = scroll down; SDL: positive y = scroll up. */
    ev.wheelDeltaX = dx;
    ev.wheelDeltaY = -dy;

    /* Wheel events carry no position; attach the current pointer. */
    float mx = 0.0f, my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    ev.mouseX = (int)mx;
    ev.mouseY = (int)my;

    input_manager_push_event(&ev);
}

#ifdef RMMZ_PS5
/* ---- PlayStation 5 (Vulkan build) -------------------------------------------------------
   As in the SDL2 port (platform_sdl2.c): the launcher's Settings screen writes $HOME/settings.ini (or the file
   RMMZ_SETTINGS names) with the pad buttons' mapping, keys that pad buttons press, the pointer speed and the
   scaling; the DualSense touchpad drives a mouse pointer. */

#define SETTINGS_PAD_BUTTONS 12
/* order as in the launcher: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad */
static int settings_map[SETTINGS_PAD_BUTTONS] = { 0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 9, 8 };
static const SDL_GamepadButton settings_buttons[SETTINGS_PAD_BUTTONS] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID,   /* L2 and R2 are analog */
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_TOUCHPAD,
};
static const char *const settings_keys[SETTINGS_PAD_BUTTONS] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "touchpad",
};
static float settings_pointer_gain = 1.6f;
static int settings_key[SETTINGS_PAD_BUTTONS] = { 90, 88 };   /* DOM key code a pad button also presses, 0 = none; by default Cross presses Z and Circle presses X */

static void load_settings(void)
{
    const char *path = getenv("RMMZ_SETTINGS");
    char buf[512];
    if (!path || !*path) {
        const char *home = getenv("HOME");
        snprintf(buf, sizeof(buf), "%s/settings.ini", home ? home : ".");
        path = buf;
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        int value = atoi(eq + 1);
        if (strncmp(line, "map.", 4) == 0) {
            for (int i = 0; i < SETTINGS_PAD_BUTTONS; i++)
                if (strcmp(line + 4, settings_keys[i]) == 0 && value >= -1 && value < 12) settings_map[i] = value;
        } else if (strncmp(line, "key.", 4) == 0) {
            for (int i = 0; i < SETTINGS_PAD_BUTTONS; i++)
                if (strcmp(line + 4, settings_keys[i]) == 0 && value >= 0 && value < 256) settings_key[i] = value;
        } else if (strcmp(line, "pointer") == 0) {
            static const float gains[3] = { 1.0f, 1.6f, 2.4f };
            if (value >= 0 && value <= 2) settings_pointer_gain = gains[value];
        }
        /* "scale" (sharp or smooth upscaling) is the OpenGL builds' own: here the game is drawn at the size of the
           screen */
    }
    fclose(f);
}

static bool ps5_pad_down(SDL_Gamepad *gp, int i, double lt, double rt, double *value)
{
    *value = 1.0;
    if (i == 6) { *value = lt; return lt > 0.5; }
    if (i == 7) { *value = rt; return rt > 0.5; }
    return SDL_GetGamepadButton(gp, settings_buttons[i]);
}

/* The W3C state of a pad, with the face, shoulder, trigger, stick and Options buttons assigned by the settings
   (settings_map[i]: the virtual button pad button i triggers, -1 for none). The D-pad and Guide keep their
   places. */
static void ps5_fill_gamepad_buttons(SDL_Gamepad *gp, GamepadState *state)
{
    static const SDL_GamepadButton fixed[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_GUIDE,
    };
    for (int b = 12; b < INPUT_GAMEPAD_BUTTONS && b - 12 < (int)SDL_arraysize(fixed); b++) {
        bool pressed = SDL_GetGamepadButton(gp, fixed[b - 12]);
        state->buttons[b].pressed = pressed;
        state->buttons[b].value = pressed ? 1.0 : 0.0;
    }
    double lt = (double)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0;
    double rt = (double)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0;
    for (int i = 0; i < SETTINGS_PAD_BUTTONS; i++) {
        int target = settings_map[i];
        double value;
        if (target < 0 || !ps5_pad_down(gp, i, lt, rt, &value)) continue;
        state->buttons[target].pressed = true;
        if (value > state->buttons[target].value) state->buttons[target].value = value;
    }
}

/* Pad buttons that press keyboard keys: some PC games read the keyboard directly ("press E", Shift, F-keys). */
static bool dom_key_names(int code, const char **key, const char **dom_code)
{
    static char key_buf[8], code_buf[16];
    if (code >= 65 && code <= 90) {
        snprintf(key_buf, sizeof(key_buf), "%c", 'a' + (code - 65));
        snprintf(code_buf, sizeof(code_buf), "Key%c", code);
        *key = key_buf; *dom_code = code_buf;
        return true;
    }
    if (code >= 48 && code <= 57) {
        snprintf(key_buf, sizeof(key_buf), "%c", code);
        snprintf(code_buf, sizeof(code_buf), "Digit%c", code);
        *key = key_buf; *dom_code = code_buf;
        return true;
    }
    if (code >= 112 && code <= 123) {
        snprintf(key_buf, sizeof(key_buf), "F%d", code - 111);
        snprintf(code_buf, sizeof(code_buf), "F%d", code - 111);
        *key = key_buf; *dom_code = code_buf;
        return true;
    }
    switch (code) {
        case 8:  *key = "Backspace";  *dom_code = "Backspace";    return true;
        case 9:  *key = "Tab";        *dom_code = "Tab";          return true;
        case 13: *key = "Enter";      *dom_code = "Enter";        return true;
        case 16: *key = "Shift";      *dom_code = "ShiftLeft";    return true;
        case 17: *key = "Control";    *dom_code = "ControlLeft";  return true;
        case 18: *key = "Alt";        *dom_code = "AltLeft";      return true;
        case 27: *key = "Escape";     *dom_code = "Escape";       return true;
        case 32: *key = " ";          *dom_code = "Space";        return true;
        case 33: *key = "PageUp";     *dom_code = "PageUp";       return true;
        case 34: *key = "PageDown";   *dom_code = "PageDown";     return true;
        case 35: *key = "End";        *dom_code = "End";          return true;
        case 36: *key = "Home";       *dom_code = "Home";         return true;
        case 37: *key = "ArrowLeft";  *dom_code = "ArrowLeft";    return true;
        case 38: *key = "ArrowUp";    *dom_code = "ArrowUp";      return true;
        case 39: *key = "ArrowRight"; *dom_code = "ArrowRight";   return true;
        case 40: *key = "ArrowDown";  *dom_code = "ArrowDown";    return true;
        case 45: *key = "Insert";     *dom_code = "Insert";       return true;
        case 46: *key = "Delete";     *dom_code = "Delete";       return true;
        default: return false;
    }
}

static void push_key(int code, bool down)
{
    const char *key, *dom_code;
    if (!dom_key_names(code, &key, &dom_code)) return;
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? INPUT_KEY_DOWN : INPUT_KEY_UP;
    ev.keyCode = code;
    strncpy(ev.key, key, sizeof(ev.key) - 1);
    strncpy(ev.code, dom_code, sizeof(ev.code) - 1);
    if (code == 16) ev.shiftKey = down;
    if (code == 17) ev.ctrlKey = down;
    if (code == 18) ev.altKey = down;
    input_manager_push_event(&ev);
}

static void update_key_bindings(Platform *p)
{
    SDL_Gamepad *gp = NULL;
    for (int i = 0; i < INPUT_MAX_GAMEPADS && !gp; i++) gp = p->gamepads[i];
    if (!gp) return;
    double lt = (double)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0;
    double rt = (double)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0;
    for (int i = 0; i < SETTINGS_PAD_BUTTONS; i++) {
        double value;
        if (settings_key[i] == 0) continue;
        bool down = ps5_pad_down(gp, i, lt, rt, &value);
        if (down != p->kb_down[i]) {
            p->kb_down[i] = down;
            push_key(settings_key[i], down);
        }
    }
}

/* The touchpad: one finger moves a pointer (relative, like a laptop touchpad), a tap or a press of the touchpad
   clicks, and the same with two fingers is a right click (cancel in RPG Maker). The pad API reports the fingers;
   SDL's joystick driver opened the pad, and scePadGetHandle finds its handle. */
typedef struct { uint16_t x, y; uint8_t finger, pad[3]; } PadTouch;
typedef struct { uint8_t fingers, pad1[3]; uint32_t pad2; PadTouch touch[2]; } PadTouchData;
typedef struct {
    uint32_t buttons;
    struct { uint8_t x, y; } leftStick, rightStick;
    struct { uint8_t l2, r2; } analogButtons;
    uint16_t padding;
    struct { float x, y, z, w; } quat;
    struct { float x, y, z; } vel, acell;
    PadTouchData touch;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t unknown[15];
} PadData;

extern int scePadGetHandle(int user_id, int type, int index);
extern int scePadReadState(int handle, PadData *data);
extern int sceUserServiceGetLoginUserIdList(int user_ids[4]);

#define TP_BUTTON_PRESS 0x100000u

/* Mouse events carry window pixels, as SDL's do; the game maps them into its letterboxed picture */
static void push_mouse(InputEventType type, int x, int y, int button)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.mouseX = x;
    ev.mouseY = y;
    ev.button = button;
    input_manager_push_event(&ev);
}

static void touchpad_update(Platform *p)
{
    if (p->tp_handle < 0) {
        if ((p->tp_tries++ % 120) != 0) return;       /* look for the pad about twice a second */
        int ids[4] = { -1, -1, -1, -1 };
        if (sceUserServiceGetLoginUserIdList(ids) != 0) return;
        for (int i = 0; i < 4 && p->tp_handle < 0; i++) {
            if (ids[i] == -1) continue;
            int h = scePadGetHandle(ids[i], 0, 0);
            if (h >= 0) p->tp_handle = h;
        }
        if (p->tp_handle < 0) return;
    }

    PadData d;
    memset(&d, 0, sizeof(d));
    if (scePadReadState(p->tp_handle, &d) != 0) {
        p->tp_handle = -1;
        return;
    }

    int win_w = 1920, win_h = 1080;
    SDL_GetWindowSize(p->window, &win_w, &win_h);
    /* the touchpad's units are about a 1080p screen's pixels */
    float gain = settings_pointer_gain * (float)win_h / 1080.0f;
    Uint64 now = SDL_GetTicks();
    int nf = d.touch.fingers > 2 ? 2 : d.touch.fingers;
    bool press = (d.buttons & TP_BUTTON_PRESS) != 0;

    if (nf > 0) {
        int x = d.touch.touch[0].x, y = d.touch.touch[0].y;
        p->cursor_until = now + 4000;
        if (!p->tp_active) {
            p->tp_active = true;
            p->tp_prev_x = p->tp_start_x = x;
            p->tp_prev_y = p->tp_start_y = y;
            p->tp_start_ms = now;
            p->tp_moved = false;
            p->tp_press_used = false;
            p->tp_maxfingers = nf;
        } else {
            if (nf > p->tp_maxfingers) p->tp_maxfingers = nf;
            int dx = x - p->tp_prev_x, dy = y - p->tp_prev_y;
            p->tp_prev_x = x;
            p->tp_prev_y = y;
            if (nf == 1 && (dx != 0 || dy != 0)) {
                p->cur_x += dx * gain;
                p->cur_y += dy * gain;
                if (p->cur_x < 0) p->cur_x = 0;
                if (p->cur_y < 0) p->cur_y = 0;
                if (p->cur_x > win_w - 1) p->cur_x = (float)(win_w - 1);
                if (p->cur_y > win_h - 1) p->cur_y = (float)(win_h - 1);
                push_mouse(INPUT_MOUSE_MOVE, (int)p->cur_x, (int)p->cur_y, 0);
            }
            if (abs(x - p->tp_start_x) + abs(y - p->tp_start_y) > 60) p->tp_moved = true;
        }
    } else if (p->tp_active) {
        p->tp_active = false;
        /* a short touch without moving is a tap */
        if (!p->tp_moved && !p->tp_press_used && now - p->tp_start_ms < 300) {
            int button = p->tp_maxfingers >= 2 ? 2 : 0;
            push_mouse(INPUT_MOUSE_DOWN, (int)p->cur_x, (int)p->cur_y, button);
            push_mouse(INPUT_MOUSE_UP, (int)p->cur_x, (int)p->cur_y, button);
        }
    }

    if (press != p->tp_press) {
        p->tp_press = press;
        p->cursor_until = now + 4000;
        if (press) {
            p->tp_button = nf >= 2 ? 2 : 0;
            p->tp_press_used = true;
            push_mouse(INPUT_MOUSE_DOWN, (int)p->cur_x, (int)p->cur_y, p->tp_button);
        } else {
            push_mouse(INPUT_MOUSE_UP, (int)p->cur_x, (int)p->cur_y, p->tp_button);
        }
    }
}

/* The pointer: an arrow drawn over the finished frame (renderer_present_overlay), while the touchpad is in use */
static const char *const cursor_shape[] = {
    "X", "XX", "XWX", "XWWX", "XWWWX", "XWWWWX", "XWWWWWX", "XWWWWWWX", "XWWWWWWWX", "XWWWWWWWWX",
    "XWWWWXXXXX", "XWWXWWX", "XWX XWWX", "XX  XWWX", "X    XWWX", "     XWWX", "      XX",
};
#define CURSOR_W 10
#define CURSOR_H 17
static Platform *s_overlay_platform;

static void draw_cursor(void)
{
    Platform *p = s_overlay_platform;
    if (!p || SDL_GetTicks() > p->cursor_until) return;
    if (!p->cursor_tex) {
        uint8_t px[CURSOR_W * CURSOR_H * 4];
        memset(px, 0, sizeof(px));
        for (int row = 0; row < CURSOR_H; row++) {
            const char *line = cursor_shape[row];
            for (int col = 0; line[col] && col < CURSOR_W; col++) {
                if (line[col] == ' ') continue;
                uint8_t v = line[col] == 'W' ? 255 : 0;
                uint8_t *o = px + (row * CURSOR_W + col) * 4;   /* first row = the arrow's tip */
                o[0] = o[1] = o[2] = v;
                o[3] = 255;
            }
        }
        p->cursor_tex = gpu_texture_create(CURSOR_W, CURSOR_H, false);
        if (!p->cursor_tex) return;
        gpu_texture_upload(p->cursor_tex, CURSOR_W, CURSOR_H, px);
    }
    int sw = 0, sh = 0;
    gpu_screen_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) return;
    float s = (float)(sh >= 1800 ? 4 : (sh >= 900 ? 2 : 1));
    /* The screen texture is bottom-up (y = 0 is the bottom row): the arrow's tip at the pointer */
    float x0 = p->cur_x, x1 = x0 + CURSOR_W * s;
    float y1 = (float)sh - p->cur_y, y0 = y1 - CURSOR_H * s;
    GpuVertex v[4] = {
        { x0, y1, 0.0f, 0.0f, 1, 1, 1, 1 },
        { x1, y1, 1.0f, 0.0f, 1, 1, 1, 1 },
        { x1, y0, 1.0f, 1.0f, 1, 1, 1, 1 },
        { x0, y0, 0.0f, 1.0f, 1, 1, 1, 1 },
    };
    float proj[16] = { 0 };
    proj[0] = 2.0f / (float)sw;
    proj[5] = 2.0f / (float)sh;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = -1.0f;
    proj[15] = 1.0f;
    gpu_record_target(0, sw, sh);
    gpu_record_viewport(0, 0, sw, sh);
    gpu_record_scissor_off();
    gpu_record_sprites(v, 1, p->cursor_tex, 0 /* BLEND_MODE_NORMAL */, proj, 0.0f);
}
#endif /* RMMZ_PS5 */

/* Gamepads */

static int find_gamepad_slot(Platform *p, SDL_JoystickID id)
{
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (p->gamepads[i] && p->gamepad_ids[i] == id)
            return i;
    }
    return -1;
}

static int find_free_slot(Platform *p)
{
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (!p->gamepads[i])
            return i;
    }
    return -1;
}

static void open_gamepad(Platform *p, SDL_JoystickID id)
{
    if (!SDL_IsGamepad(id)) return;

    /* SDL also sends GAMEPAD_ADDED for devices already open from the init
       loop; skip those. */
    if (find_gamepad_slot(p, id) >= 0) return;

    int slot = find_free_slot(p);
    if (slot < 0) return;

    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    if (!gp) return;

    p->gamepads[slot] = gp;
    p->gamepad_ids[slot] = id;

    GamepadState state;
    memset(&state, 0, sizeof(state));
    state.connected = true;
    state.index = slot;
    const char *name = SDL_GetGamepadName(gp);
    if (name) {
        strncpy(state.id, name, sizeof(state.id) - 1);
    } else {
        strncpy(state.id, "Unknown Controller", sizeof(state.id) - 1);
    }
    input_manager_set_gamepad(slot, &state);
}

static void close_gamepad(Platform *p, SDL_JoystickID id)
{
    int slot = find_gamepad_slot(p, id);
    if (slot < 0) return;

    SDL_CloseGamepad(p->gamepads[slot]);
    p->gamepads[slot] = NULL;
    p->gamepad_ids[slot] = 0;
    input_manager_clear_gamepad(slot);
}

/* Poll every open gamepad into the input manager. */
static void update_gamepad_state(Platform *p)
{
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        SDL_Gamepad *gp = p->gamepads[i];
        if (!gp) continue;

        GamepadState state;
        memset(&state, 0, sizeof(state));
        state.connected = true;
        state.index = i;
        const char *name = SDL_GetGamepadName(gp);
        if (name) {
            strncpy(state.id, name, sizeof(state.id) - 1);
        } else {
            strncpy(state.id, "Unknown Controller", sizeof(state.id) - 1);
        }

        /* W3C Standard Gamepad button order: A B X Y LB RB LT RT Back Start
           LS RS DUp DDown DLeft DRight Guide. SDL3 names face buttons by
           position, so A/B/X/Y are SOUTH/EAST/WEST/NORTH. */
        static const SDL_GamepadButton btn_map[] = {
            SDL_GAMEPAD_BUTTON_SOUTH,
            SDL_GAMEPAD_BUTTON_EAST,
            SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
            SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_INVALID,   /* LT: analog, filled in below */
            SDL_GAMEPAD_BUTTON_INVALID,   /* RT: analog, filled in below */
            SDL_GAMEPAD_BUTTON_BACK,
            SDL_GAMEPAD_BUTTON_START,
            SDL_GAMEPAD_BUTTON_LEFT_STICK,
            SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_DPAD_LEFT,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
            SDL_GAMEPAD_BUTTON_GUIDE,
        };

#ifdef RMMZ_PS5
        ps5_fill_gamepad_buttons(gp, &state);
        (void)btn_map;
#else
        for (int b = 0; b < INPUT_GAMEPAD_BUTTONS; b++) {
            if (btn_map[b] != SDL_GAMEPAD_BUTTON_INVALID) {
                bool pressed = SDL_GetGamepadButton(gp, btn_map[b]);
                state.buttons[b].pressed = pressed;
                state.buttons[b].value = pressed ? 1.0 : 0.0;
            }
        }

        int16_t lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int16_t rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        double lt_val = (double)lt / 32767.0;
        double rt_val = (double)rt / 32767.0;
        state.buttons[6].value = lt_val;
        state.buttons[6].pressed = lt_val > 0.5;
        state.buttons[7].value = rt_val;
        state.buttons[7].pressed = rt_val > 0.5;
#endif

        /* Axes LX, LY, RX, RY normalized to -1..1. */
        static const SDL_GamepadAxis axis_map[] = {
            SDL_GAMEPAD_AXIS_LEFTX,
            SDL_GAMEPAD_AXIS_LEFTY,
            SDL_GAMEPAD_AXIS_RIGHTX,
            SDL_GAMEPAD_AXIS_RIGHTY,
        };

        for (int a = 0; a < INPUT_GAMEPAD_AXES; a++) {
            int16_t raw = SDL_GetGamepadAxis(gp, axis_map[a]);
            state.axes[a] = (double)raw / 32767.0;
        }

        input_manager_set_gamepad(i, &state);
    }
}

/* Lifecycle */

Platform *platform_init(const char *title, int width, int height)
{
#ifdef RMMZ_PS5
    load_settings();
#endif
    /* Audio and gamepad are optional (absent in WSL / CI / headless). */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return NULL;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        fprintf(stderr, "Warning: audio init failed: %s\n", SDL_GetError());
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        fprintf(stderr, "Warning: gamepad init failed: %s\n", SDL_GetError());


#ifndef RMMZ_RENDER_GPU
    /* OpenGL 4.5 core: the renderer relies on direct state access. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#endif

#ifdef RMMZ_PS5
    /* The console's one window is the whole screen; the game is letterboxed inside it */
    {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (mode) {
            width = mode->w;
            height = mode->h;
        }
    }
#endif
    SDL_Window *window = SDL_CreateWindow(
        title, width, height,
#ifdef RMMZ_PS5
        SDL_WINDOW_FULLSCREEN
#elif defined(RMMZ_RENDER_GPU)
        SDL_WINDOW_RESIZABLE
#else
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
#endif
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

#ifdef RMMZ_RENDER_GPU
    SDL_GLContext gl_ctx = NULL;
    if (!gpu_backend_init(window)) {
        fprintf(stderr, "No supported GPU backend (Vulkan or D3D12) available\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
#else
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
#endif

#ifndef RMMZ_PS5
    /* Typed text (layout- and IME-aware) for HTML input overlays; key
       events keep flowing regardless. On the console this would open the
       on-screen keyboard over the game. */
    SDL_StartTextInput(window);
#endif

#ifndef RMMZ_RENDER_GPU
    /* Windows' opengl32.dll exports only GL 1.1; newer entry points must be
       resolved from the driver at runtime. */
    if (gl_loader_init() != 0) {
        fprintf(stderr, "OpenGL 4.5 entry points unavailable (driver too old?)\n");
        SDL_GL_DestroyContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    SDL_GL_SetSwapInterval(1);
#endif

    Platform *p = calloc(1, sizeof(Platform));
    if (!p) {
#ifdef RMMZ_RENDER_GPU
        gpu_backend_shutdown();
#else
        SDL_GL_DestroyContext(gl_ctx);
#endif
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
    p->window = window;
    p->gl_ctx = gl_ctx;
#ifdef RMMZ_PS5
    p->tp_handle = -1;
    p->cur_x = width / 2.0f;
    p->cur_y = height / 2.0f;
    s_overlay_platform = p;
    renderer_present_overlay = draw_cursor;
#endif

    int num_gamepads = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&num_gamepads);
    if (ids) {
        for (int i = 0; i < num_gamepads; i++)
            open_gamepad(p, ids[i]);
        SDL_free(ids);
    }

    return p;
}

void platform_shutdown(Platform *p)
{
    if (!p) return;

    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (p->gamepads[i]) {
            SDL_CloseGamepad(p->gamepads[i]);
            p->gamepads[i] = NULL;
        }
    }

#ifdef RMMZ_RENDER_GPU
    gpu_backend_shutdown();
#else
    if (p->gl_ctx) SDL_GL_DestroyContext(p->gl_ctx);
#endif
    if (p->window) SDL_DestroyWindow(p->window);
    SDL_Quit();
    free(p);
}

bool platform_poll_events(Platform *p)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                return false;

            case SDL_EVENT_KEY_DOWN:
                process_key_event(&ev.key, INPUT_KEY_DOWN);
                break;

            case SDL_EVENT_KEY_UP:
                process_key_event(&ev.key, INPUT_KEY_UP);
                break;

            case SDL_EVENT_TEXT_INPUT:
                process_text_input(&ev.text);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                process_mouse_button(&ev.button, INPUT_MOUSE_DOWN);
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                process_mouse_button(&ev.button, INPUT_MOUSE_UP);
                break;

            case SDL_EVENT_MOUSE_MOTION:
                process_mouse_motion(&ev.motion);
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                process_mouse_wheel(&ev.wheel);
                break;

            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                p->resize_pending = true;
                p->resize_w = ev.window.data1;
                p->resize_h = ev.window.data2;
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                open_gamepad(p, ev.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                close_gamepad(p, ev.gdevice.which);
                break;

            default:
                break;
        }
    }

    update_gamepad_state(p);
#ifdef RMMZ_PS5
    update_key_bindings(p);
    touchpad_update(p);
#endif

    return true;
}

void platform_swap_buffers(Platform *p)
{
    if (!p || !p->window) return;
#ifndef RMMZ_RENDER_GPU
    /* The GPU backend has already put the frame on screen in
       renderer_present; only GL needs the window's buffers swapped. */
    SDL_GL_SwapWindow(p->window);
#endif
}

void platform_request_dump(Platform *p, const char *path)
{
    (void)p; (void)path;
}

bool platform_cpu_present_available(Platform *p)
{
    (void)p;
    return false;
}

bool platform_present_pixels(Platform *p, const uint8_t *rgba, int w, int h)
{
    (void)p; (void)rgba; (void)w; (void)h;
    return false;
}

void platform_get_present_size(Platform *p, int *w, int *h)
{
    platform_get_window_size(p, w, h);
}

void platform_set_virtual_size(Platform *p, int w, int h)
{
    (void)p; (void)w; (void)h;
}

void platform_get_window_size(Platform *p, int *w, int *h)
{
    if (!p || !p->window) return;
    SDL_GetWindowSize(p->window, w, h);
}

void platform_set_window_size(Platform *p, int w, int h)
{
    if (!p || !p->window || w <= 0 || h <= 0) return;
    SDL_SetWindowSize(p->window, w, h);
    SDL_DisplayID display = SDL_GetDisplayForWindow(p->window);
    if (display == 0) display = SDL_GetPrimaryDisplay();
    SDL_SetWindowPosition(p->window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(display));
}

float platform_get_desktop_scale(Platform *p)
{
    if (!p || !p->window) return 1.0f;
    /* Where the drawable is already larger than the window (macOS HiDPI),
       SDL sizes windows in points and the scaling is applied for us. */
    if (platform_get_display_scale(p) > 1.0f) return 1.0f;

    SDL_DisplayID display = SDL_GetDisplayForWindow(p->window);
    if (display == 0) display = SDL_GetPrimaryDisplay();
    float scale = SDL_GetDisplayContentScale(display);
    if (scale <= 0.0f) return 1.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    return scale;
}

/* Fullscreen */

void platform_set_window_title(Platform *p, const char *title)
{
    if (!p || !p->window) return;
    SDL_SetWindowTitle(p->window, title ? title : "");
}

void platform_set_fullscreen(Platform *p, bool fullscreen)
{
    if (!p || !p->window) return;
    /* A NULL fullscreen mode means borderless desktop, which avoids a
       display mode switch. */
    SDL_SetWindowFullscreenMode(p->window, NULL);
    SDL_SetWindowFullscreen(p->window, fullscreen);
    /* X11 and Wayland apply the change asynchronously; wait for it so that
       platform_is_fullscreen() answers truthfully straight away, as it
       already does on Windows. */
    SDL_SyncWindow(p->window);
}

bool platform_is_fullscreen(Platform *p)
{
    if (!p || !p->window) return false;
    return (SDL_GetWindowFlags(p->window) & SDL_WINDOW_FULLSCREEN) != 0;
}

/* Display information */

void platform_get_display_size(Platform *p, int *w, int *h)
{
    SDL_DisplayID display = 0;
    if (p && p->window)
        display = SDL_GetDisplayForWindow(p->window);
    if (display == 0) display = SDL_GetPrimaryDisplay();

    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
    if (mode) {
        if (w) *w = mode->w;
        if (h) *h = mode->h;
    } else {
        if (w) *w = 1920;
        if (h) *h = 1080;
    }
}

float platform_get_display_scale(Platform *p)
{
    /* The drawable/window size ratio is a reliable HiDPI proxy, and unlike
       the display's content scale it reflects what GL actually renders to. */
    if (p && p->window) {
        int win_w = 0, px_w = 0;
        SDL_GetWindowSize(p->window, &win_w, NULL);
        SDL_GetWindowSizeInPixels(p->window, &px_w, NULL);
        if (win_w > 0) {
            float scale = (float)px_w / (float)win_w;
            if (scale >= 1.0f) return scale;
        }
    }
    return 1.0f;
}

bool platform_check_resize(Platform *p, int *w, int *h)
{
    if (!p || !p->resize_pending) return false;
    p->resize_pending = false;
    if (w) *w = p->resize_w;
    if (h) *h = p->resize_h;
    return true;
}
