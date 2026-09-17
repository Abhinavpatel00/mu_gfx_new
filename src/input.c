#include "input_rgfw.h"
#include "platform.h"
#include <string.h>

static const InputKey key_map[RGFW_keyLast] = {
    [RGFW_keySpace] = KEY_SPACE, [RGFW_keyApostrophe] = KEY_APOSTROPHE,
    [RGFW_keyComma] = KEY_COMMA, [RGFW_keyMinus] = KEY_MINUS,
    [RGFW_keyPeriod] = KEY_PERIOD, [RGFW_keySlash] = KEY_SLASH,
    [RGFW_key0] = KEY_0, [RGFW_key1] = KEY_1, [RGFW_key2] = KEY_2,
    [RGFW_key3] = KEY_3, [RGFW_key4] = KEY_4, [RGFW_key5] = KEY_5,
    [RGFW_key6] = KEY_6, [RGFW_key7] = KEY_7, [RGFW_key8] = KEY_8, [RGFW_key9] = KEY_9,
    [RGFW_keyA] = KEY_A, [RGFW_keyB] = KEY_B, [RGFW_keyC] = KEY_C,
    [RGFW_keyD] = KEY_D, [RGFW_keyE] = KEY_E, [RGFW_keyF] = KEY_F,
    [RGFW_keyG] = KEY_G, [RGFW_keyH] = KEY_H, [RGFW_keyI] = KEY_I,
    [RGFW_keyJ] = KEY_J, [RGFW_keyK] = KEY_K, [RGFW_keyL] = KEY_L,
    [RGFW_keyM] = KEY_M, [RGFW_keyN] = KEY_N, [RGFW_keyO] = KEY_O,
    [RGFW_keyP] = KEY_P, [RGFW_keyQ] = KEY_Q, [RGFW_keyR] = KEY_R,
    [RGFW_keyS] = KEY_S, [RGFW_keyT] = KEY_T, [RGFW_keyU] = KEY_U,
    [RGFW_keyV] = KEY_V, [RGFW_keyW] = KEY_W, [RGFW_keyX] = KEY_X,
    [RGFW_keyY] = KEY_Y, [RGFW_keyZ] = KEY_Z,
    [RGFW_keySemicolon] = KEY_SEMICOLON, [RGFW_keyEqual] = KEY_EQUAL,
    [RGFW_keyBracket] = KEY_LEFT_BRACKET, [RGFW_keyCloseBracket] = KEY_RIGHT_BRACKET,
    [RGFW_keyBackSlash] = KEY_BACKSLASH, [RGFW_keyBacktick] = KEY_GRAVE,
    [RGFW_keyEscape] = KEY_ESCAPE, [RGFW_keyReturn] = KEY_ENTER,
    [RGFW_keyTab] = KEY_TAB, [RGFW_keyBackSpace] = KEY_BACKSPACE,
    [RGFW_keyInsert] = KEY_INSERT, [RGFW_keyDelete] = KEY_DELETE,
    [RGFW_keyRight] = KEY_RIGHT, [RGFW_keyLeft] = KEY_LEFT,
    [RGFW_keyDown] = KEY_DOWN, [RGFW_keyUp] = KEY_UP,
    [RGFW_keyPageUp] = KEY_PAGE_UP, [RGFW_keyPageDown] = KEY_PAGE_DOWN,
    [RGFW_keyHome] = KEY_HOME, [RGFW_keyEnd] = KEY_END,
    [RGFW_keyShiftL] = KEY_LEFT_SHIFT, [RGFW_keyShiftR] = KEY_RIGHT_SHIFT,
    [RGFW_keyControlL] = KEY_LEFT_CTRL, [RGFW_keyControlR] = KEY_RIGHT_CTRL,
    [RGFW_keyAltL] = KEY_LEFT_ALT, [RGFW_keyAltR] = KEY_RIGHT_ALT,
    [RGFW_keySuperL] = KEY_LEFT_SUPER, [RGFW_keySuperR] = KEY_RIGHT_SUPER,
    [RGFW_keyF1] = KEY_F1, [RGFW_keyF2] = KEY_F2, [RGFW_keyF3] = KEY_F3,
    [RGFW_keyF4] = KEY_F4, [RGFW_keyF5] = KEY_F5, [RGFW_keyF6] = KEY_F6,
    [RGFW_keyF7] = KEY_F7, [RGFW_keyF8] = KEY_F8, [RGFW_keyF9] = KEY_F9,
    [RGFW_keyF10] = KEY_F10, [RGFW_keyF11] = KEY_F11, [RGFW_keyF12] = KEY_F12,
    [RGFW_keyF13] = KEY_F13, [RGFW_keyF14] = KEY_F14, [RGFW_keyF15] = KEY_F15,
    [RGFW_keyF16] = KEY_F16, [RGFW_keyF17] = KEY_F17, [RGFW_keyF18] = KEY_F18,
    [RGFW_keyF19] = KEY_F19, [RGFW_keyF20] = KEY_F20, [RGFW_keyF21] = KEY_F21,
    [RGFW_keyF22] = KEY_F22, [RGFW_keyF23] = KEY_F23, [RGFW_keyF24] = KEY_F24,
    [RGFW_keyF25] = KEY_F25, [RGFW_keyCapsLock] = KEY_CAPS_LOCK,
    [RGFW_keyNumLock] = KEY_NUM_LOCK, [RGFW_keyScrollLock] = KEY_SCROLL_LOCK,
    [RGFW_keyPrintScreen] = KEY_PRINT_SCREEN, [RGFW_keyPause] = KEY_PAUSE,
    [RGFW_keyMenu] = KEY_MENU,
    [RGFW_keyPad0] = KEY_PAD_0, [RGFW_keyPad1] = KEY_PAD_1, [RGFW_keyPad2] = KEY_PAD_2,
    [RGFW_keyPad3] = KEY_PAD_3, [RGFW_keyPad4] = KEY_PAD_4, [RGFW_keyPad5] = KEY_PAD_5,
    [RGFW_keyPad6] = KEY_PAD_6, [RGFW_keyPad7] = KEY_PAD_7, [RGFW_keyPad8] = KEY_PAD_8,
    [RGFW_keyPad9] = KEY_PAD_9, [RGFW_keyPadSlash] = KEY_PAD_DIVIDE,
    [RGFW_keyPadMultiply] = KEY_PAD_MULTIPLY, [RGFW_keyPadPlus] = KEY_PAD_ADD,
    [RGFW_keyPadMinus] = KEY_PAD_SUBTRACT, [RGFW_keyPadEqual] = KEY_PAD_EQUAL,
    [RGFW_keyPadPeriod] = KEY_PAD_DECIMAL, [RGFW_keyPadReturn] = KEY_PAD_ENTER,
    [RGFW_keyWorld1] = KEY_WORLD_1, [RGFW_keyWorld2] = KEY_WORLD_2
};

void input_init(Input *input, RGFW_window *window) {
    assert(input && window);
    memset(input, 0, sizeof(*input));
    input->window = window;
    input->focused = RGFW_window_isInFocus(window) != 0;
}

void input_begin(Input *input) {
    for (unsigned i = 0; i < KEY_COUNT; ++i)
        input->keys[i].pressed = input->keys[i].released = false;
    for (unsigned i = 0; i < MOUSE_BUTTON_COUNT; ++i)
        input->mouse[i].pressed = input->mouse[i].released = false;
    input->mouse_dx = input->mouse_dy = 0;
    input->scroll_x = input->scroll_y = 0;
}

void input_feed_rgfw(Input *input, const RGFW_event *event) {
    if (event->common.win != input->window)
        return;
    switch (event->type) {
    case RGFW_keyPressed:
    case RGFW_keyReleased: {
        InputKey key = key_map[event->key.value];
        if (key == KEY_NONE) break;
        InputButton *button = &input->keys[key];
        if (event->type == RGFW_keyPressed) {
            button->pressed |= !button->down && !event->key.repeat;
            button->down = true;
        } else {
            button->released |= button->down;
            button->down = false;
        }
        break;
    }
    case RGFW_mouseButtonPressed:
    case RGFW_mouseButtonReleased: {
        static const InputMouseButton buttons[RGFW_mouseFinal] = {
            [RGFW_mouseLeft] = MOUSE_LEFT, [RGFW_mouseRight] = MOUSE_RIGHT,
            [RGFW_mouseMiddle] = MOUSE_MIDDLE, [RGFW_mouseMisc1] = MOUSE_4,
            [RGFW_mouseMisc2] = MOUSE_5, [RGFW_mouseMisc3] = MOUSE_6,
            [RGFW_mouseMisc4] = MOUSE_7, [RGFW_mouseMisc5] = MOUSE_8
        };
        if (event->button.value >= RGFW_mouseFinal) break;
        InputButton *button = &input->mouse[buttons[event->button.value]];
        if (event->type == RGFW_mouseButtonPressed) {
            button->pressed |= !button->down;
            button->down = true;
        } else {
            button->released |= button->down;
            button->down = false;
        }
        break;
    }
    case RGFW_mouseMotion:
        if (input->position_valid) {
            input->mouse_dx += event->mouse.x - input->mouse_x;
            input->mouse_dy += event->mouse.y - input->mouse_y;
        }
        input->mouse_x = event->mouse.x;
        input->mouse_y = event->mouse.y;
        input->position_valid = true;
        break;
    case RGFW_mouseScroll:
        input->scroll_x += event->delta.x;
        input->scroll_y += event->delta.y;
        break;
    case RGFW_windowFocusIn:
        input->focused = true;
        input->position_valid = false;
        break;
    case RGFW_windowFocusOut:
        input->focused = false;
        input->position_valid = false;
        input->mouse_dx = input->mouse_dy = 0;
        input->scroll_x = input->scroll_y = 0;
        for (unsigned i = 0; i < KEY_COUNT; ++i) {
            input->keys[i].released |= input->keys[i].down;
            input->keys[i].down = false;
        }
        for (unsigned i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
            input->mouse[i].released |= input->mouse[i].down;
            input->mouse[i].down = false;
        }
        break;
    default: break;
    }
}

void input_update(Input *input) {
    input_begin(input);
    RGFW_event event;
    while (RGFW_checkEvent(&event))
        input_feed_rgfw(input, &event);
}
