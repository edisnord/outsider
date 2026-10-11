/*
 * SDL2 platform layer for the PlayStation 5 port of Outsider.
 *
 * Same interface as platform_sdl3.c (see platform.h), written against SDL2, which is what the PS5
 * homebrew SDK ships. Only the OpenGL rendering backend is supported: the console has no GPU API
 * that SDL exposes, so OpenGL comes from Mesa's software rasteriser (OSMesa), loaded at run time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <fcntl.h>
#include "engine/error_handler.h"
#include "platform/platform.h"
#include "rendering/present_cpu.h"
#include "input/input_manager.h"
#include "rendering/gl_loader.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SCE__
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sys/ucontext.h>
extern char __executable_start;
#endif

struct Platform {
    SDL_Window   *window;
    SDL_GLContext  gl_ctx;

    SDL_GameController *gamepads[INPUT_MAX_GAMEPADS];
    SDL_JoystickID      gamepad_ids[INPUT_MAX_GAMEPADS];

    /* Set when the window is resized; consumed by platform_check_resize. */
    bool  resize_pending;
    int   resize_w;
    int   resize_h;

    bool     kb_down[12];             /* pad buttons currently held that press a keyboard key */

    /* PS5: size the game is drawn at (see platform_set_virtual_size) */
    int      vw, vh;

    /* DualSense touchpad used as a mouse (PlayStation 5) */
    int      tp_handle;                 /* pad handle, -1 until found */
    unsigned tp_tries;
    bool     tp_active, tp_press, tp_moved, tp_press_used;
    int      tp_prev_x, tp_prev_y, tp_start_x, tp_start_y, tp_maxfingers, tp_button;
    unsigned tp_start_ms;
    float    cur_x, cur_y;              /* pointer position in window pixels */
    unsigned cursor_until;              /* the pointer is drawn until this time (ms) */
};

#ifdef __SCE__
/* A crash on the console only shows up as the app disappearing, so print where it happened.
   Addresses are relative to the load address (feed them to llvm-symbolizer with the unstripped
   binary). */
static void *stack_lo, *stack_hi;

static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    char buf[256];
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t base = (uintptr_t)&__executable_start;
    uintptr_t text_end = base + 0x4000000;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
    uintptr_t rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;

    int n = snprintf(buf, sizeof(buf), "\n*** CRASH signal=%d code=%d fault_addr=%p base=%p rip=0x%lx\n",
                     sig, info->si_code, info->si_addr, (void *)base,
                     (unsigned long)(rip >= base ? rip - base : rip));
    write(2, buf, n);

    n = snprintf(buf, sizeof(buf), "  rsp=%p rbp=%p stack=%p..%p\n", (void *)rsp,
                 (void *)(uintptr_t)uc->uc_mcontext.mc_rbp, stack_lo, stack_hi);
    write(2, buf, n);

    /* Words on the stack that point into our own code: return addresses, roughly the call chain. */
    uintptr_t *stack = (uintptr_t *)rsp;
    uintptr_t *stack_end = (uintptr_t *)stack_hi;
    if (!stack_hi || rsp < (uintptr_t)stack_lo || rsp >= (uintptr_t)stack_hi) stack_end = stack + 2048;
    int found = 0;
    for (uintptr_t *w = stack; w < stack_end && found < 60; w++) {
        uintptr_t v = *w;
        if (v >= base && v < text_end) {
            n = snprintf(buf, sizeof(buf), "  stack[%ld] = 0x%lx\n", (long)(w - stack), (unsigned long)(v - base));
            write(2, buf, n);
            found++;
        }
    }
    if (sig != SIGUSR1) _exit(128 + sig);
}

/* Hang finder: with RMMZ_WATCHDOG=<seconds> in the environment, a helper thread interrupts the main
   thread every that many seconds (four times) and it prints where it is, using the same report as a
   crash but without exiting. */
static pthread_t watchdog_target;

static void *watchdog_thread(void *arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < 4; i++) {
        sleep(secs);
        pthread_kill(watchdog_target, SIGUSR1);
    }
    return NULL;
}

static void install_crash_handler(void)
{
    /* The launcher only pipes stdout back to the developer; send error output the same way. */
    /* RMMZ_LOG=<file>: the launcher reads the output from a file to tell the player why a game closed. */
    {
        const char *log_path = getenv("RMMZ_LOG");
        if (log_path) {
            int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
                setvbuf(stdout, NULL, _IOLBF, 0);
            }
        }
    }
    dup2(STDOUT_FILENO, STDERR_FILENO);

    {
        pthread_attr_t attr;
        void *addr = NULL;
        size_t size = 0;
        if (pthread_attr_init(&attr) == 0 && pthread_attr_get_np(pthread_self(), &attr) == 0 &&
            pthread_attr_getstack(&attr, &addr, &size) == 0) {
            stack_lo = addr;
            stack_hi = (char *)addr + size;
        }
    }

    const char *wd = getenv("RMMZ_WATCHDOG");
    if (wd && atoi(wd) > 0) {
        struct sigaction hs;
        memset(&hs, 0, sizeof(hs));
        hs.sa_sigaction = crash_handler;
        hs.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR1, &hs, NULL);
        watchdog_target = pthread_self();
        pthread_t t;
        pthread_create(&t, NULL, watchdog_thread, (void *)(uintptr_t)atoi(wd));
    }

    /* Handle the fault on its own stack: a JS stack overflow would otherwise leave no room. */
    static char altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0 };
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
#endif

/* SDL keycode -> DOM keyCode */

static int sdl_to_dom_keycode(SDL_Keycode key)
{
    /* Letters (DOM uses the uppercase ASCII code; SDL keycodes are lowercase). */
    if (key >= SDLK_a && key <= SDLK_z)
        return (key - SDLK_a) + 65;

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
        case SDLK_BACKQUOTE:    return 192;
        case SDLK_LEFTBRACKET:  return 219;
        case SDLK_BACKSLASH:    return 220;
        case SDLK_RIGHTBRACKET: return 221;
        case SDLK_QUOTE:        return 222;
        default:                return 0;
    }
}

/* SDL keycode -> DOM "key" string */

static const char *sdl_to_dom_key(SDL_Keycode key, Uint16 mod)
{
    static char buf[8];

    if (key >= SDLK_a && key <= SDLK_z) {
        buf[0] = (mod & KMOD_SHIFT) ? (char)('A' + (key - SDLK_a))
                                    : (char)('a' + (key - SDLK_a));
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
    ev.keyCode = sdl_to_dom_keycode(kev->keysym.sym);
    ev.repeat = kev->repeat;
    ev.shiftKey = (kev->keysym.mod & KMOD_SHIFT) != 0;
    ev.ctrlKey  = (kev->keysym.mod & KMOD_CTRL)  != 0;
    ev.altKey   = (kev->keysym.mod & KMOD_ALT)   != 0;
    ev.metaKey  = (kev->keysym.mod & KMOD_GUI)   != 0;

    const char *key_str = sdl_to_dom_key(kev->keysym.sym, kev->keysym.mod);
    strncpy(ev.key, key_str, sizeof(ev.key) - 1);

    const char *code_str = sdl_to_dom_code(kev->keysym.scancode);
    strncpy(ev.code, code_str, sizeof(ev.code) - 1);

    input_manager_push_event(&ev);
}

static void process_text_input(const SDL_TextInputEvent *tev)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TEXT;
    strncpy(ev.key, tev->text, sizeof(ev.key) - 1);
    input_manager_push_event(&ev);
}

static void process_mouse_button(const SDL_MouseButtonEvent *mev, InputEventType type)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = type;
    ev.mouseX = mev->x;
    ev.mouseY = mev->y;
    ev.button = sdl_to_dom_button(mev->button);

    input_manager_push_event(&ev);
}

static void process_mouse_motion(const SDL_MouseMotionEvent *mev)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = INPUT_MOUSE_MOVE;
    ev.mouseX = mev->x;
    ev.mouseY = mev->y;

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
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    ev.mouseX = mx;
    ev.mouseY = my;

    input_manager_push_event(&ev);
}

/* User settings: button mapping and a few options, written by the launcher's Settings screen to
   $HOME/settings.ini (or the file named by RMMZ_SETTINGS). */

#define SETTINGS_PAD_BUTTONS 12
/* order as in the launcher: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad */
static int settings_map[SETTINGS_PAD_BUTTONS] = { 0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 9, 8 };
static const SDL_GameControllerButton settings_buttons[SETTINGS_PAD_BUTTONS] = {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID,   /* L2 and R2 are analog */
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_TOUCHPAD,
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
        } else if (strcmp(line, "scale") == 0) {
            /* 1: sharp, 2: smooth, anything else: chosen by the size of the picture (see renderer_present) */
            if (value == 1) setenv("RMMZ_SCALE", "nearest", 0);
            else if (value == 2) setenv("RMMZ_SCALE", "smooth", 0);
        }
    }
    fclose(f);
}

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

/* device_index is what SDL2 hands out in SDL_CONTROLLERDEVICEADDED and SDL_NumJoysticks(). */
static void open_gamepad(Platform *p, int device_index)
{
    if (!SDL_IsGameController(device_index)) return;

    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);

    /* SDL also sends CONTROLLERDEVICEADDED for devices already open from the init loop. */
    if (find_gamepad_slot(p, id) >= 0) return;

    int slot = find_free_slot(p);
    if (slot < 0) return;

    SDL_GameController *gp = SDL_GameControllerOpen(device_index);
    if (!gp) return;

    p->gamepads[slot] = gp;
    p->gamepad_ids[slot] = id;

    GamepadState state;
    memset(&state, 0, sizeof(state));
    state.connected = true;
    state.index = slot;
    const char *name = SDL_GameControllerName(gp);
    strncpy(state.id, name ? name : "Unknown Controller", sizeof(state.id) - 1);
    input_manager_set_gamepad(slot, &state);
}

static void close_gamepad(Platform *p, SDL_JoystickID id)
{
    int slot = find_gamepad_slot(p, id);
    if (slot < 0) return;

    SDL_GameControllerClose(p->gamepads[slot]);
    p->gamepads[slot] = NULL;
    p->gamepad_ids[slot] = 0;
    input_manager_clear_gamepad(slot);
}

/* Poll every open gamepad into the input manager. */
static void update_gamepad_state(Platform *p)
{
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        SDL_GameController *gp = p->gamepads[i];
        if (!gp) continue;

        GamepadState state;
        memset(&state, 0, sizeof(state));
        state.connected = true;
        state.index = i;
        const char *name = SDL_GameControllerName(gp);
        strncpy(state.id, name ? name : "Unknown Controller", sizeof(state.id) - 1);

        /* W3C Standard Gamepad button order: A B X Y LB RB LT RT Back Start
           LS RS DUp DDown DLeft DRight Guide. SDL2 names the face buttons by
           their Xbox labels, which is the W3C order as well. */
        static const SDL_GameControllerButton btn_map[] = {
            SDL_CONTROLLER_BUTTON_A,
            SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_X,
            SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
            SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_INVALID,   /* LT: analog, filled in below */
            SDL_CONTROLLER_BUTTON_INVALID,   /* RT: analog, filled in below */
            SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START,
            SDL_CONTROLLER_BUTTON_LEFTSTICK,
            SDL_CONTROLLER_BUTTON_RIGHTSTICK,
            SDL_CONTROLLER_BUTTON_DPAD_UP,
            SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT,
            SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            SDL_CONTROLLER_BUTTON_GUIDE,
        };

        int16_t lt = SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t rt = SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        double lt_val = (double)lt / 32767.0;
        double rt_val = (double)rt / 32767.0;

        /* The D-pad, the guide button and Back keep their places. */
        for (int b = 12; b < INPUT_GAMEPAD_BUTTONS; b++) {
            bool pressed = SDL_GameControllerGetButton(gp, btn_map[b]) != 0;
            state.buttons[b].pressed = pressed;
            state.buttons[b].value = pressed ? 1.0 : 0.0;
        }
        if (SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_BACK)) {
            state.buttons[8].pressed = true;
            state.buttons[8].value = 1.0;
        }

        /* Every other pad button is assigned to a virtual button by the settings (see the launcher's
           Settings screen): settings_map[i] is the virtual button pad button i triggers, -1 for none. */
        for (int i = 0; i < SETTINGS_PAD_BUTTONS; i++) {
            int target = settings_map[i];
            if (target < 0) continue;
            bool down;
            double value = 1.0;
            if (i == 6) { down = lt_val > 0.5; value = lt_val; }
            else if (i == 7) { down = rt_val > 0.5; value = rt_val; }
            else down = SDL_GameControllerGetButton(gp, settings_buttons[i]) != 0;
            if (!down) continue;
            state.buttons[target].pressed = true;
            if (value > state.buttons[target].value) state.buttons[target].value = value;
        }

        /* Axes LX, LY, RX, RY normalized to -1..1. */
        static const SDL_GameControllerAxis axis_map[] = {
            SDL_CONTROLLER_AXIS_LEFTX,
            SDL_CONTROLLER_AXIS_LEFTY,
            SDL_CONTROLLER_AXIS_RIGHTX,
            SDL_CONTROLLER_AXIS_RIGHTY,
        };

        for (int a = 0; a < INPUT_GAMEPAD_AXES; a++) {
            int16_t raw = SDL_GameControllerGetAxis(gp, axis_map[a]);
            state.axes[a] = (double)raw / 32767.0;
        }

        input_manager_set_gamepad(i, &state);
    }
}

/* ---- DualSense touchpad as a mouse (PlayStation 5 only) -----------------------------------------------
   The console's SDL port only exposes the touchpad as a button, but the pad API reports the finger positions.
   One finger moves a pointer (relative, like a laptop touchpad), a tap or a press of the touchpad clicks, and
   the same with two fingers is a right click (cancel in RPG Maker). The pointer is drawn as a small arrow while
   it is in use. The events are ordinary mouse events, which RPG Maker's menus already understand. */
#ifdef __SCE__
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
#define TP_GAIN         1.6f

/* The pointer lives in screen pixels; the game is drawn scaled and centred on the screen (see
   renderer_present), so map it back to game pixels. */
static Platform *s_mouse_platform;

static void push_mouse(InputEventType type, int x, int y, int button)
{
    Platform *p = s_mouse_platform;
    if (p && p->vw > 0 && p->vh > 0) {
        int rw = 1920, rh = 1080;
        SDL_GetWindowSize(p->window, &rw, &rh);
        float sx = (float)rw / (float)p->vw, sy = (float)rh / (float)p->vh;
        float scale = sx < sy ? sx : sy;
        float ox = ((float)rw - (float)p->vw * scale) / 2.0f, oy = ((float)rh - (float)p->vh * scale) / 2.0f;
        int gx = (int)(((float)x - ox) / scale), gy = (int)(((float)y - oy) / scale);
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx > p->vw - 1) gx = p->vw - 1;
        if (gy > p->vh - 1) gy = p->vh - 1;
        x = gx;
        y = gy;
    }
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
    s_mouse_platform = p;
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
    unsigned now = SDL_GetTicks();
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
                p->cur_x += dx * settings_pointer_gain;
                p->cur_y += dy * settings_pointer_gain;
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

/* The pointer: an arrow drawn last, straight into the window's framebuffer, with scissored clears. */
static const char *const cursor_shape[] = {
    "X", "XX", "XWX", "XWWX", "XWWWX", "XWWWWX", "XWWWWWX", "XWWWWWWX", "XWWWWWWWX", "XWWWWWWWWX",
    "XWWWWXXXXX", "XWWXWWX", "XWX XWWX", "XX  XWWX", "X    XWWX", "     XWWX", "      XX",
};

static void draw_cursor(Platform *p)
{
    if (SDL_GetTicks() > p->cursor_until) return;
    int win_w = 1920, win_h = 1080;
    SDL_GetWindowSize(p->window, &win_w, &win_h);
    int scale = win_h >= 900 ? 2 : 1;
    int cx = (int)p->cur_x, cy = (int)p->cur_y;

    GLint box[4];
    glGetIntegerv(GL_SCISSOR_BOX, box);
    GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
    glEnable(GL_SCISSOR_TEST);
    for (size_t row = 0; row < sizeof(cursor_shape) / sizeof(cursor_shape[0]); row++) {
        const char *line = cursor_shape[row];
        size_t n = strlen(line);
        for (size_t col = 0; col < n;) {
            char c = line[col];
            if (c == ' ') { col++; continue; }
            size_t end = col;
            while (end < n && line[end] == c) end++;
            float v = c == 'W' ? 1.0f : 0.0f;
            glClearColor(v, v, v, 1.0f);
            int x = cx + (int)col * scale;
            int y = win_h - (cy + (int)row * scale) - scale;   /* GL rows count from the bottom */
            glScissor(x, y, (GLsizei)(end - col) * scale, scale);
            glClear(GL_COLOR_BUFFER_BIT);
            col = end;
        }
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);   /* the renderer relies on a black default */
    glScissor(box[0], box[1], box[2], box[3]);
    if (!scissor_was_on) glDisable(GL_SCISSOR_TEST);
}
#endif

/* Pad buttons that press keyboard keys. Some PC games read the keyboard directly ("press E", Shift, Ctrl, F-keys)
   and have no gamepad support for these; the launcher's Settings screen assigns keys to pad buttons. */

/* DOM values of the keys the launcher can assign. */
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
    SDL_GameController *gp = NULL;
    for (int i = 0; i < INPUT_MAX_GAMEPADS && !gp; i++) gp = p->gamepads[i];
    if (!gp) return;
    double lt = (double)SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0;
    double rt = (double)SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0;
    for (int i = 0; i < SETTINGS_PAD_BUTTONS; i++) {
        if (settings_key[i] == 0) continue;
        bool down;
        if (i == 6) down = lt > 0.5;
        else if (i == 7) down = rt > 0.5;
        else down = SDL_GameControllerGetButton(gp, settings_buttons[i]) != 0;
        if (down != p->kb_down[i]) {
            p->kb_down[i] = down;
            push_key(settings_key[i], down);
        }
    }
}

/* Lifecycle */

Platform *platform_init(const char *title, int width, int height)
{
    int req_w = width, req_h = height;   /* the size asked for; on the PS5 the window is the whole screen */
#ifdef __SCE__
    install_crash_handler();
#endif

    load_settings();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return NULL;
    }
    /* Audio and gamepad are optional. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        fprintf(stderr, "Warning: audio init failed: %s\n", SDL_GetError());
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "Warning: gamepad init failed: %s\n", SDL_GetError());

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
#ifdef __SCE__
    /* The console's video driver draws a smaller window centred and unscaled, so always take the
       whole screen; the game is letterboxed inside it by the JS layer. */
    flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP;
    {
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
            width = mode.w;
            height = mode.h;
        }
    }
#else
    /* OpenGL 4.5: the renderer relies on direct state access. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#endif

    SDL_Window *window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          width, height, flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

#ifndef __SCE__
    /* Typed text for HTML input overlays; key events keep flowing regardless. On the console this
       would open the system on-screen keyboard over the game, so it is left off there. */
    SDL_StartTextInput();
#endif

    if (gl_loader_init() != 0) {
        fprintf(stderr, "OpenGL 4.5 entry points unavailable\n");
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    SDL_GL_SetSwapInterval(1);

    Platform *p = calloc(1, sizeof(Platform));
    if (!p) {
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
    p->window = window;
    p->gl_ctx = gl_ctx;
    p->tp_handle = -1;
    p->cur_x = width / 2.0f;
    p->cur_y = height / 2.0f;
#ifdef __SCE__
    p->vw = req_w;
    p->vh = req_h;
#else
    /* On a PC the same mode can be tried out with RMMZ_VIRTUAL=1 (the game is drawn at its own size in a window of
       whatever size and scaled to it). */
    const char *virt = getenv("RMMZ_VIRTUAL");
    if (virt) {
        p->vw = req_w;
        p->vh = req_h;
        int vw = 0, vh = 0;
        if (sscanf(virt, "%dx%d", &vw, &vh) == 2 && vw > 0 && vh > 0) {   /* RMMZ_VIRTUAL=816x624 */
            p->vw = vw;
            p->vh = vh;
        }
    }
#endif

    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; i++)
        open_gamepad(p, i);

    return p;
}

#ifdef __SCE__
static void presenter_stop(void);
#endif

void platform_shutdown(Platform *p)
{
    if (!p) return;
#ifdef __SCE__
    presenter_stop();
#endif

    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (p->gamepads[i]) {
            SDL_GameControllerClose(p->gamepads[i]);
            p->gamepads[i] = NULL;
        }
    }

    if (p->gl_ctx) SDL_GL_DeleteContext(p->gl_ctx);
    if (p->window) SDL_DestroyWindow(p->window);
    SDL_Quit();
    free(p);
}

bool platform_poll_events(Platform *p)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                return false;

            case SDL_KEYDOWN:
                process_key_event(&ev.key, INPUT_KEY_DOWN);
                break;

            case SDL_KEYUP:
                process_key_event(&ev.key, INPUT_KEY_UP);
                break;

            case SDL_TEXTINPUT:
                process_text_input(&ev.text);
                break;

            case SDL_MOUSEBUTTONDOWN:
                process_mouse_button(&ev.button, INPUT_MOUSE_DOWN);
                break;

            case SDL_MOUSEBUTTONUP:
                process_mouse_button(&ev.button, INPUT_MOUSE_UP);
                break;

            case SDL_MOUSEMOTION:
                process_mouse_motion(&ev.motion);
                break;

            case SDL_MOUSEWHEEL:
                process_mouse_wheel(&ev.wheel);
                break;

            case SDL_WINDOWEVENT:
                if ((ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                     ev.window.event == SDL_WINDOWEVENT_RESIZED) && p->vw <= 0) {
                    p->resize_pending = true;
                    p->resize_w = ev.window.data1;
                    p->resize_h = ev.window.data2;
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
                open_gamepad(p, ev.cdevice.which);
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                close_gamepad(p, ev.cdevice.which);
                break;

            default:
                break;
        }
    }

    update_gamepad_state(p);
    update_key_bindings(p);
#ifdef __SCE__
    touchpad_update(p);
#endif

    return true;
}

/* Debug aid: RMMZ_DUMP_PRESENT=<file.ppm> writes what is about to be shown (the window's framebuffer) once. */
static void dump_present(Platform *p)
{
    static int frames;
    const char *path = getenv("RMMZ_DUMP_PRESENT");
    if (!path || ++frames != 240) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(p->window, &w, &h);
    unsigned char *px = malloc((size_t)w * h * 3);
    if (!px) return;
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        fprintf(stderr, "dumped the presented picture (%dx%d) to %s\n", w, h, path);
    }
    free(px);
}


#ifdef __SCE__
static int s_cpu_present_off = -1;   /* -1: not decided yet */

/* The video driver's part of showing a frame (converting it to the display's memory layout, submitting the flip and
   waiting for the previous one) takes several milliseconds. It runs on a thread of its own, so the game can already
   work on the next frame; the game waits for it only before it writes into the window's pixel buffer again. */
static struct {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    SDL_Window     *window;
    int             started, busy, quit;
} s_presenter = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void *presenter_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_presenter.mutex);
    for (;;) {
        while (!s_presenter.busy && !s_presenter.quit) pthread_cond_wait(&s_presenter.cond, &s_presenter.mutex);
        if (s_presenter.quit) break;
        SDL_Window *win = s_presenter.window;
        pthread_mutex_unlock(&s_presenter.mutex);
        if (SDL_UpdateWindowSurface(win) != 0) fprintf(stderr, "SDL_UpdateWindowSurface failed: %s\n", SDL_GetError());
        pthread_mutex_lock(&s_presenter.mutex);
        s_presenter.busy = 0;
        pthread_cond_broadcast(&s_presenter.cond);
    }
    pthread_mutex_unlock(&s_presenter.mutex);
    return NULL;
}

/* Waits until the presenter is done with the window's pixel buffer. */
static void presenter_wait(void)
{
    if (!s_presenter.started) return;
    pthread_mutex_lock(&s_presenter.mutex);
    while (s_presenter.busy) pthread_cond_wait(&s_presenter.cond, &s_presenter.mutex);
    pthread_mutex_unlock(&s_presenter.mutex);
}

static void presenter_stop(void)
{
    if (!s_presenter.started) return;
    presenter_wait();
    pthread_mutex_lock(&s_presenter.mutex);
    s_presenter.quit = 1;
    pthread_cond_broadcast(&s_presenter.cond);
    pthread_mutex_unlock(&s_presenter.mutex);
    pthread_join(s_presenter.thread, NULL);
    s_presenter.started = 0;
}

/* Hands the window's pixel buffer to the presenter. */
static void presenter_submit(SDL_Window *window)
{
    if (!s_presenter.started) {
        s_presenter.window = window;
        s_presenter.quit = 0;
        s_presenter.busy = 0;
        if (pthread_create(&s_presenter.thread, NULL, presenter_main, NULL) != 0) {
            if (SDL_UpdateWindowSurface(window) != 0) fprintf(stderr, "SDL_UpdateWindowSurface failed: %s\n", SDL_GetError());
            return;
        }
        s_presenter.started = 1;
    }
    pthread_mutex_lock(&s_presenter.mutex);
    s_presenter.busy = 1;
    pthread_cond_broadcast(&s_presenter.cond);
    pthread_mutex_unlock(&s_presenter.mutex);
}

/* Software cursor: the same shape as draw_cursor(), painted into the frame that goes to the video driver. */
static void draw_cursor_cpu(Platform *p, uint8_t *pixels, int pitch, int w, int h)
{
    if (SDL_GetTicks() > p->cursor_until) return;
    int scale = h >= 900 ? 2 : 1;
    int cx = (int)p->cur_x, cy = (int)p->cur_y;
    for (size_t row = 0; row < sizeof(cursor_shape) / sizeof(cursor_shape[0]); row++) {
        const char *line = cursor_shape[row];
        size_t n = strlen(line);
        for (size_t col = 0; col < n; col++) {
            char c = line[col];
            if (c == ' ') continue;
            uint32_t v = c == 'W' ? 0xFFFFFFFFu : 0xFF000000u;
            for (int sy = 0; sy < scale; sy++) {
                int y = cy + (int)row * scale + sy;
                if (y < 0 || y >= h) continue;
                uint32_t *dst = (uint32_t *)(pixels + (size_t)y * (size_t)pitch);
                for (int sx = 0; sx < scale; sx++) {
                    int x = cx + (int)col * scale + sx;
                    if (x >= 0 && x < w) dst[x] = v;
                }
            }
        }
    }
}
#endif

#ifdef __SCE__
static char s_dump_request[512];
#endif

void platform_request_dump(Platform *p, const char *path)
{
    (void)p;
#ifdef __SCE__
    if (path) snprintf(s_dump_request, sizeof s_dump_request, "%s", path);
#else
    (void)path;
#endif
}

bool platform_cpu_present_available(Platform *p)
{
#ifdef __SCE__
    if (s_cpu_present_off < 0) s_cpu_present_off = getenv("RMMZ_GL_PRESENT") != NULL;
    return !s_cpu_present_off && p && p->window;
#else
    (void)p;
    return false;
#endif
}

bool platform_present_pixels(Platform *p, const uint8_t *rgba, int w, int h)
{
#ifdef __SCE__
    static int mode = -1, frames = 0;
    if (mode < 0) {
        const char *e = getenv("RMMZ_SCALE");
        mode = (e && strcmp(e, "nearest") == 0) ? 1 : ((e && (strcmp(e, "smooth") == 0 || strcmp(e, "linear") == 0)) ? 2 : 0);
    }
    if (!platform_cpu_present_available(p) || !rgba) return false;
    SDL_Surface *surf = SDL_GetWindowSurface(p->window);
    if (!surf || !surf->pixels || surf->format->BytesPerPixel != 4 || surf->format->format != SDL_PIXELFORMAT_ABGR8888) {
        s_cpu_present_off = 1;
        fprintf(stderr, "platform_present_pixels: window surface not usable, using the OpenGL path\n");
        return false;
    }
    presenter_wait();   /* the buffer is still being read by the video driver */
    present_cpu_scale(rgba, w, h, 1, (uint8_t *)surf->pixels, surf->pitch, surf->w, surf->h, mode);
    draw_cursor_cpu(p, (uint8_t *)surf->pixels, surf->pitch, surf->w, surf->h);

    if (s_dump_request[0]) {
        FILE *df = fopen(s_dump_request, "wb");
        if (df) {
            fprintf(df, "P6\n%d %d\n255\n", surf->w, surf->h);
            for (int y = 0; y < surf->h; y++) {
                const uint8_t *row = (const uint8_t *)surf->pixels + (size_t)y * (size_t)surf->pitch;
                for (int x = 0; x < surf->w; x++) fwrite(row + x * 4, 1, 3, df);
            }
            fclose(df);
            fprintf(stderr, "dumped the presented picture to %s\n", s_dump_request);
        }
        s_dump_request[0] = '\0';
    }
    const char *dump = getenv("RMMZ_DUMP_PRESENT");
    if (dump && ++frames == 240) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", surf->w, surf->h);
            for (int y = 0; y < surf->h; y++) {
                const uint8_t *row = (const uint8_t *)surf->pixels + (size_t)y * (size_t)surf->pitch;
                for (int x = 0; x < surf->w; x++) fwrite(row + x * 4, 1, 3, f);
            }
            fclose(f);
            fprintf(stderr, "dumped the presented picture (%dx%d) to %s\n", surf->w, surf->h, dump);
        }
    }
    presenter_submit(p->window);
    return true;
#else
    (void)p; (void)rgba; (void)w; (void)h;
    return false;
#endif
}

void platform_swap_buffers(Platform *p)
{
    if (!p || !p->window) return;
#ifdef __SCE__
    presenter_wait();
#endif
    dump_present(p);
#ifdef __SCE__
    draw_cursor(p);
#endif
    {
        /* sizes_log: what the console reports for the window and the screen */
        static int last[6];
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_DisplayMode mode = {0};
        SDL_GetWindowSize(p->window, &ww, &wh);
        SDL_GL_GetDrawableSize(p->window, &dw, &dh);
        SDL_GetDesktopDisplayMode(0, &mode);
        if (last[0] != ww || last[1] != wh || last[2] != dw || last[3] != dh || last[4] != mode.w || last[5] != mode.h) {
            last[0] = ww; last[1] = wh; last[2] = dw; last[3] = dh; last[4] = mode.w; last[5] = mode.h;
            fprintf(stderr, "window %dx%d, drawable %dx%d, desktop %dx%d, game %dx%d\n", ww, wh, dw, dh, mode.w, mode.h, p->vw, p->vh);
            error_handler_log(LOG_INFO, "window %dx%d, drawable %dx%d, desktop %dx%d, game %dx%d", ww, wh, dw, dh, mode.w, mode.h, p->vw, p->vh);
        }
    }
    SDL_GL_SwapWindow(p->window);
}

void platform_get_window_size(Platform *p, int *w, int *h)
{
    if (!p || !p->window) return;
    /* The game sees a window of the size it is drawn at; the screen is scaled to at the end. */
    if (p->vw > 0 && p->vh > 0) {
        if (w) *w = p->vw;
        if (h) *h = p->vh;
        return;
    }
    SDL_GetWindowSize(p->window, w, h);
}

void platform_get_present_size(Platform *p, int *w, int *h)
{
    if (!p || !p->window) return;
    SDL_GetWindowSize(p->window, w, h);
}

void platform_set_virtual_size(Platform *p, int w, int h)
{
    if (!p || p->vw <= 0 || w <= 0 || h <= 0 || (w == p->vw && h == p->vh)) return;
    p->vw = w;
    p->vh = h;
    p->resize_pending = true;
    p->resize_w = w;
    p->resize_h = h;
}

void platform_set_window_size(Platform *p, int w, int h)
{
    if (!p || !p->window || w <= 0 || h <= 0) return;
#ifndef __SCE__
    SDL_SetWindowSize(p->window, w, h);
    SDL_SetWindowPosition(p->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
}

float platform_get_desktop_scale(Platform *p)
{
    (void)p;
    return 1.0f;
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
#ifndef __SCE__
    /* Borderless desktop fullscreen avoids a display mode switch. */
    SDL_SetWindowFullscreen(p->window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#else
    (void)fullscreen; /* the console screen is always fullscreen */
#endif
}

bool platform_is_fullscreen(Platform *p)
{
    if (!p || !p->window) return false;
#ifdef __SCE__
    return true;
#else
    return (SDL_GetWindowFlags(p->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
#endif
}

/* Display information */

void platform_get_display_size(Platform *p, int *w, int *h)
{
    int display = 0;
    if (p && p->window) {
        display = SDL_GetWindowDisplayIndex(p->window);
        if (display < 0) display = 0;
    }

    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(display, &mode) == 0) {
        if (w) *w = mode.w;
        if (h) *h = mode.h;
    } else {
        if (w) *w = 1920;
        if (h) *h = 1080;
    }
}

float platform_get_display_scale(Platform *p)
{
    /* The drawable/window size ratio is a reliable HiDPI proxy. */
    if (p && p->window) {
        int win_w = 0, px_w = 0;
        SDL_GetWindowSize(p->window, &win_w, NULL);
        SDL_GL_GetDrawableSize(p->window, &px_w, NULL);
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
