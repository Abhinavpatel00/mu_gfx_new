#ifndef MU_INPUT_GLFW_H
#define MU_INPUT_GLFW_H

#include "input.h"

/* Alternative to input_update: begin once, then feed each callback from the
 * application's own GLFW window. Events for other windows are ignored.
 * Text, window and command events remain available to other consumers.
 */
void input_begin(Input *input);
void input_feed_key(Input *input, int key, int action);
void input_feed_button(Input *input, int button, int action);
void input_feed_cursor(Input *input, double x, double y);
void input_feed_scroll(Input *input, double x, double y);
void input_feed_focus(Input *input, bool focused);
/* Entering the window re-establishes the motion baseline instead of reporting
 * the jump from the previous exit position. */
void input_feed_cursor_enter(Input *input, bool entered);

/* Standalone path: installs the adapter's callbacks on the window. The window
 * user pointer then belongs to the Input, so this is mutually exclusive with
 * application callbacks.
 */
void input_attach(Input *input, struct GLFWwindow *window);

#endif
