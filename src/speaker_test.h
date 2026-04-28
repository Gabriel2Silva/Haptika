#pragma once
#include <glib.h>

G_BEGIN_DECLS

typedef void (*SpeakerTestCallback)(const char *error, gpointer user_data);

/* Play a short test tone through the internal speaker of the controller
 * whose evdev device lives at evdev_syspath. Async; callback on main thread. */
void speaker_test_play(const char *evdev_syspath,
                       SpeakerTestCallback callback,
                       gpointer user_data);

G_END_DECLS
