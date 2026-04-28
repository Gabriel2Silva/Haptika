#include "input_viewer.h"
#include <glib/gi18n.h>
#include "speaker_test.h"
#include "window.h"
#include "widgets/analog_stick.h"
#include "widgets/button_indicator.h"
#include "widgets/tilt_indicator.h"
#include "widgets/touchpad.h"
#include "widgets/trigger_bar.h"
#include <SDL3/SDL.h>

/* Maximum number of analog sticks the UI can display. Backends reporting
 * a stick_id outside 0..CONTTEST_MAX_STICKS-1 will log a warning and be
 * ignored in the UI. */
#define CONTTEST_MAX_STICKS 6

struct _ConttestInputViewer {
    GtkBox parent_instance;
    ConttestWindow *window;

    GtkWidget *sticks_box;
    GtkWidget *touchpad_box;
    GtkWidget *triggers_box;
    GtkWidget *buttons_flow;

    ConttestInputSession *active_session;
    ViewerSessionContext *current_ctx;
    GHashTable *element_widgets; // element_id -> widget info

    GtkWidget *touchpad_widget; // NULL if no touchpad
    double stick_positions[CONTTEST_MAX_STICKS][2]; // [stick_id][axis (0=x, 1=y)]

    /* Motion (SDL3 only) */
    GtkWidget *tilt_widget;
    GtkWidget *gyro_x_bar;
    GtkWidget *gyro_y_bar;
    GtkWidget *gyro_z_bar;

    /* Vibration testing (SDL3 only) */
    GtkWidget *rumble_box;
    GtkWidget *rumble_low_scale;
    GtkWidget *rumble_high_scale;
    SDL_JoystickID rumble_sdl_id;

    /* Speaker testing */
    GtkWidget *speaker_btn;
    char      *evdev_syspath;
};

typedef struct {
    GtkWidget *widget;
    gboolean is_axis;
    gboolean is_trigger;
    gboolean is_stick_x;
    gboolean is_stick_y;
    int stick_id;
} ElementWidgetInfo;

G_DEFINE_TYPE(ConttestInputViewer, conttest_input_viewer, GTK_TYPE_BOX)

static void free_ewi(gpointer data) {
    g_free(data);
}

static void conttest_input_viewer_dispose(GObject *object) {
    ConttestInputViewer *self = CONTTEST_INPUT_VIEWER(object);
    conttest_input_viewer_stop(self);
    g_clear_pointer(&self->evdev_syspath, g_free);
    if (self->element_widgets) {
        g_hash_table_destroy(self->element_widgets);
        self->element_widgets = NULL;
    }
    G_OBJECT_CLASS(conttest_input_viewer_parent_class)->dispose(object);
}

static void on_rumble_test_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConttestInputViewer *self = CONTTEST_INPUT_VIEWER(user_data);
    SDL_Gamepad *gamepad = SDL_GetGamepadFromID(self->rumble_sdl_id);
    if (!gamepad) return;
    Uint16 low  = (Uint16)(gtk_range_get_value(GTK_RANGE(self->rumble_low_scale))  / 100.0 * 65535);
    Uint16 high = (Uint16)(gtk_range_get_value(GTK_RANGE(self->rumble_high_scale)) / 100.0 * 65535);
    SDL_RumbleGamepad(gamepad, low, high, 500);
}

static void on_speaker_test_done(const char *error, gpointer user_data) {
    ConttestInputViewer *self = CONTTEST_INPUT_VIEWER(user_data);
    gtk_widget_set_sensitive(self->speaker_btn, TRUE);
    if (error)
        conttest_window_show_toast(self->window, error);
}

static void on_speaker_test_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConttestInputViewer *self = CONTTEST_INPUT_VIEWER(user_data);
    if (!self->evdev_syspath) return;
    gtk_widget_set_sensitive(self->speaker_btn, FALSE);
    speaker_test_play(self->evdev_syspath, on_speaker_test_done, self);
}

static void conttest_input_viewer_init(ConttestInputViewer *self) {
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 16);
    gtk_widget_set_margin_top(GTK_WIDGET(self), 16);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self), 16);
    gtk_widget_set_margin_start(GTK_WIDGET(self), 16);
    gtk_widget_set_margin_end(GTK_WIDGET(self), 16);

    self->element_widgets = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free_ewi);

    // Containers
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(self), scroll);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), main_vbox);

    self->sticks_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_halign(self->sticks_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_vbox), self->sticks_box);

    self->touchpad_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(self->touchpad_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_vbox), self->touchpad_box);

    self->triggers_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_halign(self->triggers_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_vbox), self->triggers_box);

    self->buttons_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->buttons_flow), GTK_SELECTION_NONE);
    gtk_widget_set_halign(self->buttons_flow, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_vbox), self->buttons_flow);

    /* Vibration row (SDL3 only, hidden until a SDL3 session starts) */
    self->rumble_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(self->rumble_box, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(self->rumble_box, FALSE);

    GtkWidget *low_label = gtk_label_new(_("Low Frequency Motor"));
    gtk_widget_add_css_class(low_label, "dim-label");
    self->rumble_low_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(self->rumble_low_scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(self->rumble_low_scale), FALSE);
    gtk_widget_set_size_request(self->rumble_low_scale, 120, -1);

    GtkWidget *high_label = gtk_label_new(_("High Frequency Motor"));
    gtk_widget_add_css_class(high_label, "dim-label");
    self->rumble_high_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(self->rumble_high_scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(self->rumble_high_scale), FALSE);
    gtk_widget_set_size_request(self->rumble_high_scale, 120, -1);

    GtkWidget *test_btn = gtk_button_new_with_label(_("Test Vibration"));
    gtk_widget_add_css_class(test_btn, "pill");
    g_signal_connect(test_btn, "clicked", G_CALLBACK(on_rumble_test_clicked), self);

    self->speaker_btn = gtk_button_new_with_label(_("Test Speaker"));
    gtk_widget_add_css_class(self->speaker_btn, "pill");
    gtk_widget_set_visible(self->speaker_btn, FALSE);
    g_signal_connect(self->speaker_btn, "clicked", G_CALLBACK(on_speaker_test_clicked), self);

    gtk_box_append(GTK_BOX(self->rumble_box), low_label);
    gtk_box_append(GTK_BOX(self->rumble_box), self->rumble_low_scale);
    gtk_box_append(GTK_BOX(self->rumble_box), high_label);
    gtk_box_append(GTK_BOX(self->rumble_box), self->rumble_high_scale);
    gtk_box_append(GTK_BOX(self->rumble_box), test_btn);
    gtk_box_append(GTK_BOX(self->rumble_box), self->speaker_btn);
    gtk_box_append(GTK_BOX(main_vbox), self->rumble_box);
}

static void conttest_input_viewer_class_init(ConttestInputViewerClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = conttest_input_viewer_dispose;
}

GtkWidget *conttest_input_viewer_new(ConttestWindow *window) {
    ConttestInputViewer *self = g_object_new(CONTTEST_TYPE_INPUT_VIEWER, NULL);
    self->window = window;
    return GTK_WIDGET(self);
}

static void remove_all_children(GtkWidget *container) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
        if (GTK_IS_FLOW_BOX(container)) {
            gtk_flow_box_remove(GTK_FLOW_BOX(container), child);
        } else {
            gtk_box_remove(GTK_BOX(container), child);
        }
    }
}

gboolean conttest_input_viewer_start(ConttestInputViewer *self, ConttestBackend api, const char *device_path_or_id, const ConttestDevice *device) {
    conttest_input_viewer_stop(self);

    remove_all_children(self->sticks_box);
    remove_all_children(self->touchpad_box);
    remove_all_children(self->triggers_box);
    remove_all_children(self->buttons_flow);
    g_hash_table_remove_all(self->element_widgets);
    memset(self->stick_positions, 0, sizeof(self->stick_positions));
    self->touchpad_widget = NULL;
    self->rumble_sdl_id = 0;
    gtk_widget_set_visible(self->rumble_box, FALSE);
    g_clear_pointer(&self->evdev_syspath, g_free);
    gtk_widget_set_visible(self->speaker_btn, FALSE);
    gtk_widget_set_sensitive(self->speaker_btn, TRUE);
    self->tilt_widget = NULL;
    self->gyro_x_bar = self->gyro_y_bar = self->gyro_z_bar = NULL;

    self->current_ctx = g_new0(ViewerSessionContext, 1);
    self->current_ctx->viewer = self;
    self->current_ctx->active = 1;
    self->current_ctx->ref_count = 1;

    switch (api) {
    case CONTTEST_BACKEND_SDL3:
        self->active_session = conttest_input_session_sdl3_start(device_path_or_id, self->current_ctx);
        break;
    case CONTTEST_BACKEND_EVDEV:
        self->active_session = conttest_input_session_evdev_start(device_path_or_id, self->current_ctx);
        break;
    default:
        g_free(self->current_ctx);
        self->current_ctx = NULL;
        g_return_val_if_reached(FALSE);
    }

    if (!self->active_session) {
        g_free(self->current_ctx);
        self->current_ctx = NULL;
        return FALSE;
    }

    if (api == CONTTEST_BACKEND_SDL3) {
        self->rumble_sdl_id = (SDL_JoystickID)strtoul(device_path_or_id, NULL, 10);
        gtk_widget_set_visible(self->rumble_box, TRUE);

        const char *evdev = device ? conttest_device_get_evdev_syspath(device) : NULL;
        gboolean is_usb = device &&
            conttest_device_get_connection_state(device) == SDL_JOYSTICK_CONNECTION_WIRED;
        gboolean is_ds5 = device && conttest_device_is_ds5(device);
        if (evdev && is_usb && is_ds5) {
            self->evdev_syspath = g_strdup(evdev);
            gtk_widget_set_visible(self->speaker_btn, TRUE);
        }

        SDL_Gamepad *gp = SDL_GetGamepadFromID(self->rumble_sdl_id);
        if (gp && (SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL) ||
                   SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO))) {
            SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, TRUE);
            SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, TRUE);

            self->tilt_widget = conttest_tilt_indicator_new();
            gtk_widget_set_size_request(self->tilt_widget, 100, 100);
            gtk_box_append(GTK_BOX(self->sticks_box), self->tilt_widget);

            self->gyro_x_bar = conttest_trigger_bar_new("Gyro X");
            self->gyro_y_bar = conttest_trigger_bar_new("Gyro Y");
            self->gyro_z_bar = conttest_trigger_bar_new("Gyro Z");
            gtk_widget_set_size_request(self->gyro_x_bar, 40, 150);
            gtk_widget_set_size_request(self->gyro_y_bar, 40, 150);
            gtk_widget_set_size_request(self->gyro_z_bar, 40, 150);
            gtk_box_append(GTK_BOX(self->triggers_box), self->gyro_x_bar);
            gtk_box_append(GTK_BOX(self->triggers_box), self->gyro_y_bar);
            gtk_box_append(GTK_BOX(self->triggers_box), self->gyro_z_bar);
        }
    }

    return TRUE;
}

void conttest_input_viewer_stop(ConttestInputViewer *self) {
    if (self->current_ctx) {
        /* Signal all in-flight callbacks to discard their work. Must happen
         * before releasing our ref so that callbacks reading ctx->viewer
         * (which points to us) do so while we are still alive. */
        g_atomic_int_set(&self->current_ctx->active, 0);

        /* Release the viewer's ref (ref #1, set to 1 at allocation).
         * If the backend already cleaned up before stop was called — i.e.
         * the SDL3 timer self-removed or the evdev thread exited — this
         * decrement brings ref_count to 0 and ctx is freed here. That is
         * safe: free_sdl3_session checks timer_id==0 and skips g_source_remove;
         * free_evdev_session never dereferences ctx at all. */
        if (g_atomic_int_dec_and_test(&self->current_ctx->ref_count)) {
            g_free(self->current_ctx);
        }
        self->current_ctx = NULL;
    }
    if (self->active_session) {
        /* SDL3: removes the timer → sdl3_tick_destroyed releases ref #2.
         * evdev: joins the thread → thread-exit path releases ref #2.
         * In both cases, if the backend already released ref #2 before stop
         * was called, the cleanup path is a no-op (timer_id==0 / thread done). */
        conttest_input_session_stop(self->active_session);
        self->active_session = NULL;
    }
}

/* Map stick IDs to user-facing labels. */
/* Map stick IDs to user-facing labels. Caller must g_free() the result. */
static char *stick_display_name(int stick_id) {
    switch (stick_id) {
    case 0:  return g_strdup(_("Left Stick"));
    case 1:  return g_strdup(_("Right Stick"));
    default: return g_strdup_printf(_("Stick %d"), stick_id);
    }
}

gboolean conttest_input_viewer_handle_layout(gpointer data) {
    ConttestInputLayout *layout = (ConttestInputLayout *)data;
    ViewerSessionContext *ctx = layout->ctx;

    if (!g_atomic_int_get(&ctx->active) || ctx->viewer->current_ctx != ctx) {
        for (int i=0; i < layout->num_elements; ++i) g_free(layout->elements[i].name);
        g_free(layout->elements);
        g_free(layout);
        if (g_atomic_int_dec_and_test(&ctx->ref_count)) g_free(ctx);
        return G_SOURCE_REMOVE;
    }

    ConttestInputViewer *self = ctx->viewer;

    // Track active sticks to assign them easily
    GHashTable *stick_widgets = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (int i = 0; i < layout->num_elements; i++) {
        ConttestInputElementDef *def = &layout->elements[i];

        ElementWidgetInfo *ewi = g_new0(ElementWidgetInfo, 1);
        ewi->is_axis = def->is_axis;
        ewi->is_trigger = def->is_trigger;
        ewi->is_stick_x = def->is_stick_x;
        ewi->is_stick_y = def->is_stick_y;
        ewi->stick_id = def->stick_id;

        if (def->element_id == ELEMENT_NS_TOUCHPAD_SENTINEL) {
            /* Sentinel: create the touchpad visualizer widget, no button indicator. */
            if (!self->touchpad_widget) {
                self->touchpad_widget = conttest_touchpad_new();
                gtk_widget_set_size_request(self->touchpad_widget, 200, 98);
                gtk_box_append(GTK_BOX(self->touchpad_box), self->touchpad_widget);
            }
            ewi->widget = self->touchpad_widget;
        } else if (!def->is_axis) {
            ewi->widget = conttest_button_indicator_new(def->name);
            GtkWidget *child = gtk_flow_box_child_new();
            gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), ewi->widget);
            gtk_widget_set_focusable(child, FALSE);
            gtk_widget_add_css_class(child, "button-indicator-child");
            gtk_flow_box_insert(GTK_FLOW_BOX(self->buttons_flow), child, -1);
        } else if (def->is_trigger) {
            ewi->widget = conttest_trigger_bar_new(def->name);
            gtk_widget_set_size_request(ewi->widget, 40, 150);
            gtk_box_append(GTK_BOX(self->triggers_box), ewi->widget);
        } else if (def->is_stick_x || def->is_stick_y) {
            if (def->stick_id >= 0 && def->stick_id < CONTTEST_MAX_STICKS) {
                GtkWidget *stick = g_hash_table_lookup(stick_widgets, GINT_TO_POINTER(def->stick_id));
                if (!stick) {
                    char *name = stick_display_name(def->stick_id);
                    stick = conttest_analog_stick_new(name);
                    g_free(name);
                    g_hash_table_insert(stick_widgets, GINT_TO_POINTER(def->stick_id), stick);
                    gtk_box_append(GTK_BOX(self->sticks_box), stick);
                }
                ewi->widget = stick;
            } else {
                g_warning("Stick id %d exceeds CONTTEST_MAX_STICKS (%d), skipping", def->stick_id, CONTTEST_MAX_STICKS);
                g_free(ewi);
                continue;
            }
        } else {
            // generic axis — render as a trigger bar so it's visible
            ewi->is_trigger = TRUE;
            ewi->widget = conttest_trigger_bar_new(def->name);
            gtk_widget_set_size_request(ewi->widget, 40, 150);
            gtk_box_append(GTK_BOX(self->triggers_box), ewi->widget);
        }

        g_hash_table_insert(self->element_widgets, GINT_TO_POINTER(def->element_id), ewi);
    }

    g_hash_table_destroy(stick_widgets);

    for (int i=0; i < layout->num_elements; ++i) {
        g_free(layout->elements[i].name);
    }
    g_free(layout->elements);
    g_free(layout);

    if (g_atomic_int_dec_and_test(&ctx->ref_count)) {
        g_free(ctx);
    }
    return G_SOURCE_REMOVE;
}

/* Apply a single event to the viewer. ctx must be active and current. */
static void apply_input_event(ConttestInputViewer *self, const ConttestInputEvent *ev) {

    if (ev->type == INPUT_EVENT_SENSOR) {
        if (ev->element_id == ELEMENT_NS_ACCEL && self->tilt_widget) {
            conttest_tilt_indicator_set_accel(CONTTEST_TILT_INDICATOR(self->tilt_widget),
                                              ev->sensor[0], ev->sensor[1], ev->sensor[2]);
        } else if (ev->element_id == ELEMENT_NS_GYRO && self->gyro_x_bar) {
#define GYRO_MAX 10.0
            conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(self->gyro_x_bar),
                                           CLAMP(ev->sensor[0] / GYRO_MAX / 2.0 + 0.5, 0.0, 1.0));
            conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(self->gyro_y_bar),
                                           CLAMP(ev->sensor[1] / GYRO_MAX / 2.0 + 0.5, 0.0, 1.0));
            conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(self->gyro_z_bar),
                                           CLAMP(ev->sensor[2] / GYRO_MAX / 2.0 + 0.5, 0.0, 1.0));
#undef GYRO_MAX
        }
    } else if (ev->type == INPUT_EVENT_TOUCHPAD) {
        ElementWidgetInfo *ewi = g_hash_table_lookup(self->element_widgets, GINT_TO_POINTER(ev->element_id));
        if (ewi && !ewi->is_axis)
            conttest_button_indicator_set_pressed(CONTTEST_BUTTON_INDICATOR(ewi->widget), ev->button_pressed);
        if (ev->element_id == ELEMENT_NS_TOUCHPAD_BASE && self->touchpad_widget)
            conttest_touchpad_set_finger(CONTTEST_TOUCHPAD(self->touchpad_widget),
                                         ev->button_pressed, ev->touch_x, ev->touch_y);
    } else if (ev->type == INPUT_EVENT_BUTTON) {
        ElementWidgetInfo *ewi = g_hash_table_lookup(self->element_widgets, GINT_TO_POINTER(ev->element_id));
        if (ewi && !ewi->is_axis) {
            conttest_button_indicator_set_pressed(CONTTEST_BUTTON_INDICATOR(ewi->widget), ev->button_pressed);
        }
    } else if (ev->type == INPUT_EVENT_AXIS) {
        ElementWidgetInfo *ewi = g_hash_table_lookup(self->element_widgets, GINT_TO_POINTER(ev->element_id));
        if (ewi && ewi->is_axis) {
            if (ewi->is_trigger) {
                conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(ewi->widget), ev->axis_value);
            } else if (ewi->is_stick_x) {
                if (ewi->stick_id >= 0 && ewi->stick_id < CONTTEST_MAX_STICKS) {
                    self->stick_positions[ewi->stick_id][0] = ev->axis_value;
                    conttest_analog_stick_set_position(CONTTEST_ANALOG_STICK(ewi->widget),
                                                       self->stick_positions[ewi->stick_id][0],
                                                       self->stick_positions[ewi->stick_id][1]);
                }
            } else if (ewi->is_stick_y) {
                if (ewi->stick_id >= 0 && ewi->stick_id < CONTTEST_MAX_STICKS) {
                    self->stick_positions[ewi->stick_id][1] = ev->axis_value;
                    conttest_analog_stick_set_position(CONTTEST_ANALOG_STICK(ewi->widget),
                                                       self->stick_positions[ewi->stick_id][0],
                                                       self->stick_positions[ewi->stick_id][1]);
                }
            }
        }
    }
}

/* Public wrapper for main-thread callers (SDL3 backend). */
void conttest_input_viewer_apply_event(ConttestInputViewer *self, const ConttestInputEvent *ev) {
    apply_input_event(self, ev);
}

gboolean conttest_input_viewer_handle_event_batch(gpointer data) {
    ConttestInputEventBatch *batch = (ConttestInputEventBatch *)data;
    ViewerSessionContext *ctx = batch->ctx;

    if (g_atomic_int_get(&ctx->active) && ctx->viewer->current_ctx == ctx) {
        ConttestInputViewer *self = ctx->viewer;
        for (guint i = 0; i < batch->events->len; i++) {
            apply_input_event(self, &g_array_index(batch->events, ConttestInputEvent, i));
        }
    }

    g_array_free(batch->events, TRUE);
    g_free(batch);
    if (g_atomic_int_dec_and_test(&ctx->ref_count)) g_free(ctx);
    return G_SOURCE_REMOVE;
}
