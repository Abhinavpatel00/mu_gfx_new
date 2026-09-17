#include "../src/input_rgfw.h"
#include "../src/platform.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>

static void send_key(RGFW_window *window, int type, Time time) {
    Display *display = RGFW_getDisplay_X11();
    XEvent event = {0};
    event.xkey = (XKeyEvent){.type = type, .display = display,
        .window = RGFW_window_getWindow_X11(window), .root = DefaultRootWindow(display),
        .time = time, .keycode = XKeysymToKeycode(display, XK_a), .same_screen = True};
    XSendEvent(display, event.xkey.window, False,
        type == KeyPress ? KeyPressMask : KeyReleaseMask, &event);
    XSync(display, False);
}

int main(void) {
    assert(RGFW_init("input_test", RGFW_initX11) == 0);
    RGFW_window *window = RGFW_createWindow("Input test", 0, 0, 100, 100, 0);
    assert(window);
    Input input;
    input_init(&input, window);
    input_update(&input);
    send_key(window, KeyPress, 100);
    input_update(&input);
    assert(key_down(&input, KEY_A) && key_pressed(&input, KEY_A));
    assert(input_axis(&input, KEY_A, KEY_D) == -1);
    input_update(&input);
    assert(key_down(&input, KEY_A) && !key_pressed(&input, KEY_A));
    send_key(window, KeyRelease, 200);
    input_update(&input);
    assert(!key_down(&input, KEY_A) && key_released(&input, KEY_A));
    send_key(window, KeyPress, 300);
    send_key(window, KeyRelease, 400);
    input_update(&input);
    assert(!key_down(&input, KEY_A) && key_pressed(&input, KEY_A) && key_released(&input, KEY_A));

    input_begin(&input);
    RGFW_event event = {.key = {.type = RGFW_keyPressed, .win = window,
        .value = RGFW_keyF9, .repeat = true}};
    input_feed_rgfw(&input, &event);
    assert(key_down(&input, KEY_F9) && !key_pressed(&input, KEY_F9));
    event.key.value = RGFW_keyPadReturn;
    event.key.repeat = false;
    input_feed_rgfw(&input, &event);
    assert(key_pressed(&input, KEY_PAD_ENTER));
    event.key.value = RGFW_keyNULL;
    input_feed_rgfw(&input, &event);
    assert(!key_down(&input, KEY_NONE));
    event.key.value = RGFW_keyB;
    event.key.win = NULL;
    input_feed_rgfw(&input, &event);
    assert(!key_down(&input, KEY_B));

    event = (RGFW_event){.button = {.type = RGFW_mouseButtonPressed, .win = window,
        .value = RGFW_mouseRight}};
    input_feed_rgfw(&input, &event);
    assert(mouse_pressed(&input, MOUSE_RIGHT) && !mouse_down(&input, MOUSE_MIDDLE));
    event.button.value = RGFW_mouseMiddle;
    input_feed_rgfw(&input, &event);
    assert(mouse_pressed(&input, MOUSE_MIDDLE));
    event = (RGFW_event){.common = {.type = RGFW_windowFocusIn, .win = window}};
    input_feed_rgfw(&input, &event);
    event = (RGFW_event){.mouse = {.type = RGFW_mouseMotion, .win = window, .x = 100, .y = 50}};
    input_feed_rgfw(&input, &event);
    assert(mouse_dx(&input) == 0);
    event.mouse.x = 110;
    event.mouse.y = 45;
    input_feed_rgfw(&input, &event);
    assert(mouse_dx(&input) == 10 && mouse_dy(&input) == -5);
    event = (RGFW_event){.delta = {.type = RGFW_mouseScroll, .win = window, .x = 0.25f, .y = -0.5f}};
    input_feed_rgfw(&input, &event);
    assert(scroll_x(&input) == 0.25 && scroll_y(&input) == -0.5);
    event.type = RGFW_mouseRawMotion;
    input_feed_rgfw(&input, &event);
    assert(mouse_dx(&input) == 10);
    event = (RGFW_event){.common = {.type = RGFW_windowFocusOut, .win = window}};
    input_feed_rgfw(&input, &event);
    assert(!input.focused && !key_down(&input, KEY_F9) && key_released(&input, KEY_F9));
    assert(!mouse_down(&input, MOUSE_RIGHT) && mouse_released(&input, MOUSE_RIGHT));
    assert(mouse_dx(&input) == 0 && scroll_y(&input) == 0);
    input_begin(&input);
    assert(!key_released(&input, KEY_F9) && !mouse_released(&input, MOUSE_RIGHT));
    RGFW_window_close(window);
    RGFW_deinit();
    puts("PASS: native RGFW pump and injected polling edge cases");
    return 0;
}
