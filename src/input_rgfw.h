#ifndef MU_INPUT_RGFW_H
#define MU_INPUT_RGFW_H

#include "input.h"
union RGFW_event;

/* Alternative to input_update: begin once, then feed each event from the
 * application's sole RGFW pump. Events for other windows are ignored.
 * Text and window events remain available to other application consumers.
 */
void input_begin(Input *input);
void input_feed_rgfw(Input *input, const union RGFW_event *event);

#endif
