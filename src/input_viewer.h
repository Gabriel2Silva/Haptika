#pragma once
#include <gtk/gtk.h>
#include "input_session.h"
#include "device_manager.h"

G_BEGIN_DECLS

typedef struct _ConttestWindow ConttestWindow;

#define CONTTEST_TYPE_INPUT_VIEWER (conttest_input_viewer_get_type())
G_DECLARE_FINAL_TYPE(ConttestInputViewer, conttest_input_viewer, CONTTEST, INPUT_VIEWER, GtkBox)

GtkWidget *conttest_input_viewer_new(ConttestWindow *window);

gboolean conttest_input_viewer_start(ConttestInputViewer *self, ConttestBackend api, const ConttestDevice *device);
void conttest_input_viewer_refresh_device(ConttestInputViewer *self, const ConttestDevice *device);
void conttest_input_viewer_stop(ConttestInputViewer *self);

/* Called on the main loop when a backend reports that its device disappeared.
 * Tears down the session if `ctx` is still current and asks the window to
 * return to the selector. */
void conttest_input_viewer_session_lost(ConttestInputViewer *self,
                                        ViewerSessionContext *ctx);

/* Apply a single input event directly. Only call from the main thread.
 * The SDL3 backend calls this directly since it already runs on the main thread.
 * The evdev backend pushes into the lock-free SPSC ring and coalesces
 * wakeups via conttest_input_viewer_drain_ring. */
void conttest_input_viewer_apply_event(ConttestInputViewer *self, const ConttestInputEvent *ev);

/* Called by main thread dispatchers */
gboolean conttest_input_viewer_handle_layout(gpointer data);
gboolean conttest_input_viewer_drain_ring(gpointer data);


G_END_DECLS
