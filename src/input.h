#ifndef MU_INPUT_H
#define MU_INPUT_H

#include <assert.h>
#include <stdbool.h>

struct GLFWwindow;

typedef enum InputKey {
    KEY_NONE,
    KEY_SPACE, KEY_APOSTROPHE, KEY_COMMA, KEY_MINUS, KEY_PERIOD, KEY_SLASH,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_SEMICOLON, KEY_EQUAL, KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_GRAVE,
    KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_INSERT, KEY_DELETE,
    KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
    KEY_LEFT_SHIFT, KEY_LEFT_CTRL, KEY_LEFT_ALT, KEY_LEFT_SUPER,
    KEY_RIGHT_SHIFT, KEY_RIGHT_CTRL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
    KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17,
    KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25,
    KEY_CAPS_LOCK, KEY_NUM_LOCK, KEY_SCROLL_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE, KEY_MENU,
    KEY_PAD_0, KEY_PAD_1, KEY_PAD_2, KEY_PAD_3, KEY_PAD_4,
    KEY_PAD_5, KEY_PAD_6, KEY_PAD_7, KEY_PAD_8, KEY_PAD_9,
    KEY_PAD_DIVIDE, KEY_PAD_MULTIPLY, KEY_PAD_ADD, KEY_PAD_SUBTRACT,
    KEY_PAD_EQUAL, KEY_PAD_DECIMAL, KEY_PAD_ENTER, KEY_WORLD_1, KEY_WORLD_2,
    KEY_COUNT
} InputKey;

typedef enum InputMouseButton {
    MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE,
    MOUSE_4, MOUSE_5, MOUSE_6, MOUSE_7, MOUSE_8,
    MOUSE_BUTTON_COUNT
} InputMouseButton;

typedef struct InputButton {
    bool down;
    bool pressed;
    bool released;
} InputButton;

/* Read-only state. Window ownership remains with the application. No allocations. */
typedef struct Input {
    struct GLFWwindow *window;
    InputButton keys[KEY_COUNT];
    InputButton mouse[MOUSE_BUTTON_COUNT];
    double mouse_x, mouse_y;
    double mouse_dx, mouse_dy;
    double scroll_x, scroll_y;
    bool focused;
    bool position_valid; /* Internal motion baseline. */
} Input;

/* Initialize before pumping. Held inputs start up; first motion sets the baseline. */
void input_init(Input *input, struct GLFWwindow *window);
/* Single-window convenience pump: clears then polls. Requires the adapter's
 * callbacks to be installed, either by input_attach or by the application.
 * Down/up sets both edges; repeat never sets pressed.
 * Focus loss releases held buttons as cancellation, not an activation.
 * Coordinates/deltas are window-local; raw motion is not accumulated here.
 */
void input_update(Input *input);

/* Arguments must be valid enum values; KEY_NONE always reads false. */
static inline bool key_down(const Input *input, InputKey key) {
    assert((unsigned)key < KEY_COUNT);
    return input->keys[key].down;
}
static inline bool key_pressed(const Input *input, InputKey key) {
    assert((unsigned)key < KEY_COUNT);
    return input->keys[key].pressed;
}
static inline bool key_released(const Input *input, InputKey key) {
    assert((unsigned)key < KEY_COUNT);
    return input->keys[key].released;
}
static inline bool mouse_down(const Input *input, InputMouseButton button) {
    assert((unsigned)button < MOUSE_BUTTON_COUNT);
    return input->mouse[button].down;
}
static inline bool mouse_pressed(const Input *input, InputMouseButton button) {
    assert((unsigned)button < MOUSE_BUTTON_COUNT);
    return input->mouse[button].pressed;
}
static inline bool mouse_released(const Input *input, InputMouseButton button) {
    assert((unsigned)button < MOUSE_BUTTON_COUNT);
    return input->mouse[button].released;
}
static inline double mouse_x(const Input *input) { return input->mouse_x; }
static inline double mouse_y(const Input *input) { return input->mouse_y; }
static inline double mouse_dx(const Input *input) { return input->mouse_dx; }
static inline double mouse_dy(const Input *input) { return input->mouse_dy; }
static inline double scroll_x(const Input *input) { return input->scroll_x; }
static inline double scroll_y(const Input *input) { return input->scroll_y; }
static inline float input_axis(const Input *input, InputKey negative, InputKey positive) {
    return (float)key_down(input, positive) - (float)key_down(input, negative);
}

#endif
