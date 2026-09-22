#include "input_glfw.h"

#include "GLFW/glfw3.h"
#include <string.h>

_Static_assert(GLFW_MOUSE_BUTTON_LEFT == MOUSE_LEFT && GLFW_MOUSE_BUTTON_RIGHT == MOUSE_RIGHT &&
                   GLFW_MOUSE_BUTTON_MIDDLE == MOUSE_MIDDLE && GLFW_MOUSE_BUTTON_LAST + 1 == MOUSE_BUTTON_COUNT,
               "GLFW mouse buttons map directly onto InputMouseButton");

/* GLFW key codes are sparse (32..348), so the table is indexed by code and
 * unmapped codes stay KEY_NONE. */
static const InputKey key_map[GLFW_KEY_LAST + 1] = {
    [GLFW_KEY_SPACE]         = KEY_SPACE,
    [GLFW_KEY_APOSTROPHE]    = KEY_APOSTROPHE,
    [GLFW_KEY_COMMA]         = KEY_COMMA,
    [GLFW_KEY_MINUS]         = KEY_MINUS,
    [GLFW_KEY_PERIOD]        = KEY_PERIOD,
    [GLFW_KEY_SLASH]         = KEY_SLASH,
    [GLFW_KEY_0]             = KEY_0,
    [GLFW_KEY_1]             = KEY_1,
    [GLFW_KEY_2]             = KEY_2,
    [GLFW_KEY_3]             = KEY_3,
    [GLFW_KEY_4]             = KEY_4,
    [GLFW_KEY_5]             = KEY_5,
    [GLFW_KEY_6]             = KEY_6,
    [GLFW_KEY_7]             = KEY_7,
    [GLFW_KEY_8]             = KEY_8,
    [GLFW_KEY_9]             = KEY_9,
    [GLFW_KEY_A]             = KEY_A,
    [GLFW_KEY_B]             = KEY_B,
    [GLFW_KEY_C]             = KEY_C,
    [GLFW_KEY_D]             = KEY_D,
    [GLFW_KEY_E]             = KEY_E,
    [GLFW_KEY_F]             = KEY_F,
    [GLFW_KEY_G]             = KEY_G,
    [GLFW_KEY_H]             = KEY_H,
    [GLFW_KEY_I]             = KEY_I,
    [GLFW_KEY_J]             = KEY_J,
    [GLFW_KEY_K]             = KEY_K,
    [GLFW_KEY_L]             = KEY_L,
    [GLFW_KEY_M]             = KEY_M,
    [GLFW_KEY_N]             = KEY_N,
    [GLFW_KEY_O]             = KEY_O,
    [GLFW_KEY_P]             = KEY_P,
    [GLFW_KEY_Q]             = KEY_Q,
    [GLFW_KEY_R]             = KEY_R,
    [GLFW_KEY_S]             = KEY_S,
    [GLFW_KEY_T]             = KEY_T,
    [GLFW_KEY_U]             = KEY_U,
    [GLFW_KEY_V]             = KEY_V,
    [GLFW_KEY_W]             = KEY_W,
    [GLFW_KEY_X]             = KEY_X,
    [GLFW_KEY_Y]             = KEY_Y,
    [GLFW_KEY_Z]             = KEY_Z,
    [GLFW_KEY_SEMICOLON]     = KEY_SEMICOLON,
    [GLFW_KEY_EQUAL]         = KEY_EQUAL,
    [GLFW_KEY_LEFT_BRACKET]  = KEY_LEFT_BRACKET,
    [GLFW_KEY_BACKSLASH]     = KEY_BACKSLASH,
    [GLFW_KEY_RIGHT_BRACKET] = KEY_RIGHT_BRACKET,
    [GLFW_KEY_GRAVE_ACCENT]  = KEY_GRAVE,
    [GLFW_KEY_ESCAPE]        = KEY_ESCAPE,
    [GLFW_KEY_ENTER]         = KEY_ENTER,
    [GLFW_KEY_TAB]           = KEY_TAB,
    [GLFW_KEY_BACKSPACE]     = KEY_BACKSPACE,
    [GLFW_KEY_INSERT]        = KEY_INSERT,
    [GLFW_KEY_DELETE]        = KEY_DELETE,
    [GLFW_KEY_RIGHT]         = KEY_RIGHT,
    [GLFW_KEY_LEFT]          = KEY_LEFT,
    [GLFW_KEY_DOWN]          = KEY_DOWN,
    [GLFW_KEY_UP]            = KEY_UP,
    [GLFW_KEY_PAGE_UP]       = KEY_PAGE_UP,
    [GLFW_KEY_PAGE_DOWN]     = KEY_PAGE_DOWN,
    [GLFW_KEY_HOME]          = KEY_HOME,
    [GLFW_KEY_END]           = KEY_END,
    [GLFW_KEY_CAPS_LOCK]     = KEY_CAPS_LOCK,
    [GLFW_KEY_SCROLL_LOCK]   = KEY_SCROLL_LOCK,
    [GLFW_KEY_NUM_LOCK]      = KEY_NUM_LOCK,
    [GLFW_KEY_PRINT_SCREEN]  = KEY_PRINT_SCREEN,
    [GLFW_KEY_PAUSE]         = KEY_PAUSE,
    [GLFW_KEY_LEFT_SHIFT]    = KEY_LEFT_SHIFT,
    [GLFW_KEY_LEFT_CONTROL]  = KEY_LEFT_CTRL,
    [GLFW_KEY_LEFT_ALT]      = KEY_LEFT_ALT,
    [GLFW_KEY_LEFT_SUPER]    = KEY_LEFT_SUPER,
    [GLFW_KEY_RIGHT_SHIFT]   = KEY_RIGHT_SHIFT,
    [GLFW_KEY_RIGHT_CONTROL] = KEY_RIGHT_CTRL,
    [GLFW_KEY_RIGHT_ALT]     = KEY_RIGHT_ALT,
    [GLFW_KEY_RIGHT_SUPER]   = KEY_RIGHT_SUPER,
    [GLFW_KEY_MENU]          = KEY_MENU,
    [GLFW_KEY_F1]            = KEY_F1,
    [GLFW_KEY_F2]            = KEY_F2,
    [GLFW_KEY_F3]            = KEY_F3,
    [GLFW_KEY_F4]            = KEY_F4,
    [GLFW_KEY_F5]            = KEY_F5,
    [GLFW_KEY_F6]            = KEY_F6,
    [GLFW_KEY_F7]            = KEY_F7,
    [GLFW_KEY_F8]            = KEY_F8,
    [GLFW_KEY_F9]            = KEY_F9,
    [GLFW_KEY_F10]           = KEY_F10,
    [GLFW_KEY_F11]           = KEY_F11,
    [GLFW_KEY_F12]           = KEY_F12,
    [GLFW_KEY_F13]           = KEY_F13,
    [GLFW_KEY_F14]           = KEY_F14,
    [GLFW_KEY_F15]           = KEY_F15,
    [GLFW_KEY_F16]           = KEY_F16,
    [GLFW_KEY_F17]           = KEY_F17,
    [GLFW_KEY_F18]           = KEY_F18,
    [GLFW_KEY_F19]           = KEY_F19,
    [GLFW_KEY_F20]           = KEY_F20,
    [GLFW_KEY_F21]           = KEY_F21,
    [GLFW_KEY_F22]           = KEY_F22,
    [GLFW_KEY_F23]           = KEY_F23,
    [GLFW_KEY_F24]           = KEY_F24,
    [GLFW_KEY_F25]           = KEY_F25,
    [GLFW_KEY_KP_0]          = KEY_PAD_0,
    [GLFW_KEY_KP_1]          = KEY_PAD_1,
    [GLFW_KEY_KP_2]          = KEY_PAD_2,
    [GLFW_KEY_KP_3]          = KEY_PAD_3,
    [GLFW_KEY_KP_4]          = KEY_PAD_4,
    [GLFW_KEY_KP_5]          = KEY_PAD_5,
    [GLFW_KEY_KP_6]          = KEY_PAD_6,
    [GLFW_KEY_KP_7]          = KEY_PAD_7,
    [GLFW_KEY_KP_8]          = KEY_PAD_8,
    [GLFW_KEY_KP_9]          = KEY_PAD_9,
    [GLFW_KEY_KP_DIVIDE]     = KEY_PAD_DIVIDE,
    [GLFW_KEY_KP_MULTIPLY]   = KEY_PAD_MULTIPLY,
    [GLFW_KEY_KP_ADD]        = KEY_PAD_ADD,
    [GLFW_KEY_KP_SUBTRACT]   = KEY_PAD_SUBTRACT,
    [GLFW_KEY_KP_EQUAL]      = KEY_PAD_EQUAL,
    [GLFW_KEY_KP_DECIMAL]    = KEY_PAD_DECIMAL,
    [GLFW_KEY_KP_ENTER]      = KEY_PAD_ENTER,
    [GLFW_KEY_WORLD_1]       = KEY_WORLD_1,
    [GLFW_KEY_WORLD_2]       = KEY_WORLD_2};

void input_init(Input *input, GLFWwindow *window) {
    assert(input && window);
    memset(input, 0, sizeof(*input));
    input->window  = window;
    input->focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
}

void input_begin(Input *input) {
    for (unsigned i = 0; i < KEY_COUNT; ++i)
        input->keys[i].pressed = input->keys[i].released = false;
    for (unsigned i = 0; i < MOUSE_BUTTON_COUNT; ++i)
        input->mouse[i].pressed = input->mouse[i].released = false;
    input->mouse_dx = input->mouse_dy = 0;
    input->scroll_x = input->scroll_y = 0;
}

void input_feed_key(Input *input, int key, int action) {
    if ((unsigned)key > (unsigned)GLFW_KEY_LAST)
        return;
    InputKey mapped = key_map[key];
    if (mapped == KEY_NONE)
        return;
    InputButton *button = &input->keys[mapped];
    if (action == GLFW_PRESS) {
        button->pressed |= !button->down;
        button->down = true;
    } else if (action == GLFW_RELEASE) {
        button->released |= button->down;
        button->down = false;
    } else /* GLFW_REPEAT: still held, no new edge. */
        button->down = true;
}

void input_feed_button(Input *input, int button, int action) {
    if ((unsigned)button >= MOUSE_BUTTON_COUNT)
        return;
    InputButton *state = &input->mouse[button];
    if (action == GLFW_PRESS) {
        state->pressed |= !state->down;
        state->down = true;
    } else if (action == GLFW_RELEASE) {
        state->released |= state->down;
        state->down = false;
    }
}

void input_feed_cursor(Input *input, double x, double y) {
    if (input->position_valid) {
        input->mouse_dx += x - input->mouse_x;
        input->mouse_dy += y - input->mouse_y;
    }
    input->mouse_x        = x;
    input->mouse_y        = y;
    input->position_valid = true;
}

void input_feed_scroll(Input *input, double x, double y) {
    input->scroll_x += x;
    input->scroll_y += y;
}

void input_feed_focus(Input *input, bool focused) {
    input->focused        = focused;
    input->position_valid = false;
    input->mouse_dx = input->mouse_dy = 0;
    input->scroll_x = input->scroll_y = 0;
    if (focused)
        return;
    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        input->keys[i].released |= input->keys[i].down;
        input->keys[i].down = false;
    }
    for (unsigned i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        input->mouse[i].released |= input->mouse[i].down;
        input->mouse[i].down = false;
    }
}

void input_feed_cursor_enter(Input *input, bool entered) {
    input->position_valid = !entered;
}

void input_update(Input *input) {
    input_begin(input);
    glfwPollEvents();
}

/* Standalone path: the window user pointer holds the Input the callbacks feed. */
static void input_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
    input_feed_key(glfwGetWindowUserPointer(window), key, action);
}

static void input_button_callback(GLFWwindow *window, int button, int action, int mods) {
    input_feed_button(glfwGetWindowUserPointer(window), button, action);
}

static void input_cursor_callback(GLFWwindow *window, double x, double y) {
    input_feed_cursor(glfwGetWindowUserPointer(window), x, y);
}

static void input_scroll_callback(GLFWwindow *window, double x, double y) {
    input_feed_scroll(glfwGetWindowUserPointer(window), x, y);
}

static void input_focus_callback(GLFWwindow *window, int focused) {
    input_feed_focus(glfwGetWindowUserPointer(window), focused != 0);
}

static void input_cursor_enter_callback(GLFWwindow *window, int entered) {
    input_feed_cursor_enter(glfwGetWindowUserPointer(window), entered != 0);
}

void input_attach(Input *input, GLFWwindow *window) {
    assert(input && window);
    input->window = window;
    glfwSetWindowUserPointer(window, input);
    glfwSetKeyCallback(window, input_key_callback);
    glfwSetMouseButtonCallback(window, input_button_callback);
    glfwSetCursorPosCallback(window, input_cursor_callback);
    glfwSetScrollCallback(window, input_scroll_callback);
    glfwSetWindowFocusCallback(window, input_focus_callback);
    glfwSetCursorEnterCallback(window, input_cursor_enter_callback);
}
