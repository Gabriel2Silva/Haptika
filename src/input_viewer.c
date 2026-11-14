#include "input_viewer.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include <linux/input-event-codes.h>
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
#define CONTTEST_MAX_STICKS     6
#define CONTTEST_MAX_BUTTONS    (KEY_MAX + 1)
#define CONTTEST_MAX_SDL_AXES   32
#define CONTTEST_MAX_SDL_EXTRA  64
#define CONTTEST_MAX_EVDEV_ABS  (ABS_MAX + 1)

typedef struct {
    GtkWidget *widget;
    gboolean   is_axis;
    gboolean   is_trigger;
    gboolean   is_bipolar;
    gboolean   is_stick_x;
    gboolean   is_stick_y;
    int        stick_id;
} ElementWidgetSlot;

struct _ConttestInputViewer {
    GtkBox parent_instance;
    ConttestWindow *window;

    GtkWidget *sticks_box;
    GtkWidget *touchpad_box;
    GtkWidget *triggers_box;
    GtkWidget *buttons_flow;

    ConttestInputSession *active_session;
    ViewerSessionContext *current_ctx;
    ConttestBackend active_api;

    /* Flat O(1) lookup tables partitioned by ConttestElementDomain so IDs
     * from different domains never collide. */
    ElementWidgetSlot buttons[CONTTEST_MAX_BUTTONS];
    ElementWidgetSlot touchpad_buttons[CONTTEST_TOUCHPAD_MAX_FINGERS];
    ElementWidgetSlot sdl_axes[CONTTEST_MAX_SDL_AXES];
    ElementWidgetSlot sdl_extra_axes[CONTTEST_MAX_SDL_EXTRA];
    ElementWidgetSlot evdev_abs_axes[CONTTEST_MAX_EVDEV_ABS];

    GtkWidget *touchpad_widget;                     /* NULL if no touchpad */
    double stick_positions[CONTTEST_MAX_STICKS][2]; /* [stick_id][axis (0=x, 1=y)] */

    /* Motion (SDL3 only) */
    GtkWidget *tilt_widget;
    GtkWidget *gyro_x_bar;
    GtkWidget *gyro_y_bar;
    GtkWidget *gyro_z_bar;

    /* Bottom actions row (vibration controls on SDL3; speaker test on USB DualSense) */
    GtkWidget *actions_box;
    GtkWidget *rumble_low_group;
    GtkWidget *rumble_high_group;
    GtkWidget *rumble_test_btn;
    GtkWidget *rumble_low_scale;
    GtkWidget *rumble_high_scale;
    SDL_JoystickID rumble_sdl_id;
    gboolean sensors_enabled;

    /* Speaker testing */
    GtkWidget    *speaker_group;
    GtkWidget    *speaker_btn;
    GtkWidget    *speaker_hint;
    char         *evdev_syspath;
    GCancellable *speaker_cancellable;
    gboolean      speaker_supported;
    gboolean      speaker_is_wired;
    gboolean      speaker_is_wireless;
};

G_DEFINE_TYPE(ConttestInputViewer, conttest_input_viewer, GTK_TYPE_BOX)

static inline ElementWidgetSlot *lookup_slot(ConttestInputViewer *self,
                                             ConttestElementDomain domain,
                                             int element_id) {
    if (element_id < 0)
        return NULL;
    switch (domain) {
    case CONTTEST_ELEMENT_BUTTON:
        return (element_id < CONTTEST_MAX_BUTTONS) ? &self->buttons[element_id] : NULL;
    case CONTTEST_ELEMENT_TOUCHPAD_FINGER:
        return (element_id < CONTTEST_TOUCHPAD_MAX_FINGERS) ? &self->touchpad_buttons[element_id] : NULL;
    case CONTTEST_ELEMENT_SDL_AXIS:
        return (element_id < CONTTEST_MAX_SDL_AXES) ? &self->sdl_axes[element_id] : NULL;
    case CONTTEST_ELEMENT_SDL_EXTRA:
        return (element_id < CONTTEST_MAX_SDL_EXTRA) ? &self->sdl_extra_axes[element_id] : NULL;
    case CONTTEST_ELEMENT_EVDEV_ABS:
        return (element_id < CONTTEST_MAX_EVDEV_ABS) ? &self->evdev_abs_axes[element_id] : NULL;
    default:
        return NULL;
    }
}

static void clear_element_slots(ConttestInputViewer *self) {
    memset(self->buttons, 0, sizeof(self->buttons));
    memset(self->touchpad_buttons, 0, sizeof(self->touchpad_buttons));
    memset(self->sdl_axes, 0, sizeof(self->sdl_axes));
    memset(self->sdl_extra_axes, 0, sizeof(self->sdl_extra_axes));
    memset(self->evdev_abs_axes, 0, sizeof(self->evdev_abs_axes));
}

static void conttest_input_viewer_dispose(GObject *object) {
    ConttestInputViewer *self = CONTTEST_INPUT_VIEWER(object);
    conttest_input_viewer_stop(self);
    g_clear_pointer(&self->evdev_syspath, g_free);
    clear_element_slots(self);
    G_OBJECT_CLASS(conttest_input_viewer_parent_class)->dispose(object);
    self->sticks_box = NULL;
    self->touchpad_box = NULL;
    self->triggers_box = NULL;
    self->buttons_flow = NULL;
    self->touchpad_widget = NULL;
    self->tilt_widget = NULL;
    self->gyro_x_bar = NULL;
    self->gyro_y_bar = NULL;
    self->gyro_z_bar = NULL;
    self->actions_box = NULL;
    self->rumble_low_group = NULL;
    self->rumble_high_group = NULL;
    self->rumble_test_btn = NULL;
    self->rumble_low_scale = NULL;
    self->rumble_high_scale = NULL;
    self->speaker_group = NULL;
    self->speaker_btn = NULL;
    self->speaker_hint = NULL;
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

static void cancel_speaker_test(ConttestInputViewer *self) {
    if (self->speaker_cancellable) {
        g_cancellable_cancel(self->speaker_cancellable);
        g_clear_object(&self->speaker_cancellable);
    }
}

static void sync_action_controls(ConttestInputViewer *self) {
    gboolean show_rumble = (self->active_session != NULL &&
                            self->active_api == CONTTEST_BACKEND_SDL3);
    gtk_widget_set_visible(self->rumble_low_group, show_rumble);
    gtk_widget_set_visible(self->rumble_high_group, show_rumble);
    gtk_widget_set_visible(self->rumble_test_btn, show_rumble);

    if (!self->active_session || !self->speaker_supported) {
        cancel_speaker_test(self);
        g_clear_pointer(&self->evdev_syspath, g_free);
        gtk_widget_set_visible(self->speaker_group, FALSE);
        gtk_widget_set_visible(self->actions_box, show_rumble);
        return;
    }

    gboolean can_test = (self->evdev_syspath != NULL && self->speaker_is_wired);
    if (!can_test)
        cancel_speaker_test(self);

    gtk_widget_set_visible(self->speaker_group, TRUE);
    gtk_widget_set_visible(self->speaker_hint, self->speaker_is_wireless);
    gtk_widget_set_sensitive(self->speaker_btn,
                             can_test && self->speaker_cancellable == NULL);
    gtk_widget_set_visible(self->actions_box, TRUE);
}

static GWeakRef *viewer_weak_ref_new(ConttestInputViewer *self) {
    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, self);
    return wr;
}

static void viewer_weak_ref_free(gpointer data) {
    GWeakRef *wr = data;
    g_weak_ref_clear(wr);
    g_free(wr);
}

static void on_speaker_test_done(const char *error, gpointer user_data) {
    GWeakRef *wr = user_data;
    g_autoptr(ConttestInputViewer) self = g_weak_ref_get(wr);
    if (!self)
        return;
    g_clear_object(&self->speaker_cancellable);
    sync_action_controls(self);
    if (error)
        conttest_window_show_toast(self->window, error);
}

static void on_speaker_test_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConttestInputViewer *self = CONTTEST_INPUT_VIEWER(user_data);
    if (!self->evdev_syspath || !self->speaker_is_wired)
        return;
    cancel_speaker_test(self);
    self->speaker_cancellable = g_cancellable_new();
    sync_action_controls(self);
    speaker_test_play(self->evdev_syspath, self->speaker_cancellable,
                      on_speaker_test_done, viewer_weak_ref_new(self),
                      viewer_weak_ref_free);
}

static GtkWidget *create_wrap_section(int child_spacing, int line_spacing) {
    GtkWidget *box = adw_wrap_box_new();
    adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(box), child_spacing);
    adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(box), line_spacing);
    adw_wrap_box_set_align(ADW_WRAP_BOX(box), 0.5f);
    gtk_widget_set_visible(box, FALSE);
    return box;
}

static void conttest_input_viewer_init(ConttestInputViewer *self) {
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 16);
    gtk_widget_set_margin_top(GTK_WIDGET(self), 16);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self), 16);
    gtk_widget_set_margin_start(GTK_WIDGET(self), 16);
    gtk_widget_set_margin_end(GTK_WIDGET(self), 16);

    /* Containers */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(self), scroll);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), main_vbox);

    self->sticks_box = create_wrap_section(32, 16);
    gtk_box_append(GTK_BOX(main_vbox), self->sticks_box);

    self->touchpad_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(self->touchpad_box, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(self->touchpad_box, FALSE);
    gtk_box_append(GTK_BOX(main_vbox), self->touchpad_box);

    self->triggers_box = create_wrap_section(24, 16);
    gtk_widget_set_vexpand(self->triggers_box, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), self->triggers_box);

    self->buttons_flow = create_wrap_section(4, 4);
    gtk_box_append(GTK_BOX(main_vbox), self->buttons_flow);

    /* Bottom actions row (vibration on SDL3; speaker test on real DualSense) */
    self->actions_box = create_wrap_section(16, 12);

    self->rumble_low_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(self->rumble_low_group, GTK_ALIGN_CENTER);
    GtkWidget *low_label = gtk_label_new(_("Low Frequency Motor"));
    gtk_widget_add_css_class(low_label, "dimmed");
    self->rumble_low_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(self->rumble_low_scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(self->rumble_low_scale), FALSE);
    gtk_widget_set_size_request(self->rumble_low_scale, 120, -1);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(self->rumble_low_scale),
                                   GTK_ACCESSIBLE_RELATION_LABELLED_BY, low_label, NULL,
                                   -1);
    gtk_box_append(GTK_BOX(self->rumble_low_group), low_label);
    gtk_box_append(GTK_BOX(self->rumble_low_group), self->rumble_low_scale);

    self->rumble_high_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(self->rumble_high_group, GTK_ALIGN_CENTER);
    GtkWidget *high_label = gtk_label_new(_("High Frequency Motor"));
    gtk_widget_add_css_class(high_label, "dimmed");
    self->rumble_high_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(self->rumble_high_scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(self->rumble_high_scale), FALSE);
    gtk_widget_set_size_request(self->rumble_high_scale, 120, -1);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(self->rumble_high_scale),
                                   GTK_ACCESSIBLE_RELATION_LABELLED_BY, high_label, NULL,
                                   -1);
    gtk_box_append(GTK_BOX(self->rumble_high_group), high_label);
    gtk_box_append(GTK_BOX(self->rumble_high_group), self->rumble_high_scale);

    self->rumble_test_btn = gtk_button_new_with_label(_("Test Vibration"));
    gtk_widget_add_css_class(self->rumble_test_btn, "pill");
    gtk_widget_set_valign(self->rumble_test_btn, GTK_ALIGN_CENTER);
    g_signal_connect(self->rumble_test_btn, "clicked", G_CALLBACK(on_rumble_test_clicked), self);

    self->speaker_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(self->speaker_group, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(self->speaker_group, FALSE);

    self->speaker_btn = gtk_button_new_with_label(_("Test Speaker"));
    gtk_widget_add_css_class(self->speaker_btn, "pill");
    gtk_widget_set_valign(self->speaker_btn, GTK_ALIGN_CENTER);
    g_signal_connect(self->speaker_btn, "clicked", G_CALLBACK(on_speaker_test_clicked), self);

    self->speaker_hint = gtk_label_new(_("Requires USB"));
    gtk_widget_add_css_class(self->speaker_hint, "dimmed");
    gtk_widget_add_css_class(self->speaker_hint, "caption");
    gtk_widget_set_valign(self->speaker_hint, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(self->speaker_hint, FALSE);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(self->speaker_btn),
                                   GTK_ACCESSIBLE_RELATION_DESCRIBED_BY, self->speaker_hint,
                                   NULL, -1);

    gtk_box_append(GTK_BOX(self->speaker_group), self->speaker_btn);
    gtk_box_append(GTK_BOX(self->speaker_group), self->speaker_hint);

    adw_wrap_box_append(ADW_WRAP_BOX(self->actions_box), self->rumble_low_group);
    adw_wrap_box_append(ADW_WRAP_BOX(self->actions_box), self->rumble_high_group);
    adw_wrap_box_append(ADW_WRAP_BOX(self->actions_box), self->rumble_test_btn);
    adw_wrap_box_append(ADW_WRAP_BOX(self->actions_box), self->speaker_group);
    gtk_box_append(GTK_BOX(main_vbox), self->actions_box);
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
    if (ADW_IS_WRAP_BOX(container)) {
        adw_wrap_box_remove_all(ADW_WRAP_BOX(container));
    } else {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(container)) != NULL) {
            gtk_box_remove(GTK_BOX(container), child);
        }
    }
    gtk_widget_set_visible(container, FALSE);
}

gboolean conttest_input_viewer_start(ConttestInputViewer *self, ConttestBackend api, const ConttestDevice *device) {
    g_return_val_if_fail(CONTTEST_IS_INPUT_VIEWER(self), FALSE);
    if (!device) return FALSE;

    uint32_t sdl_id = 0;
    const char *evdev_devnode = NULL;
    switch (api) {
    case CONTTEST_BACKEND_SDL3:
        if (!conttest_device_get_has_sdl(device)) return FALSE;
        sdl_id = (uint32_t)conttest_device_get_sdl_id(device);
        if (sdl_id == 0) return FALSE;
        break;
    case CONTTEST_BACKEND_EVDEV:
        if (!conttest_device_get_has_evdev(device)) return FALSE;
        evdev_devnode = conttest_device_get_evdev_devnode(device);
        if (!evdev_devnode) return FALSE;
        break;
    default:
        g_return_val_if_reached(FALSE);
    }

    conttest_input_viewer_stop(self);

    remove_all_children(self->sticks_box);
    remove_all_children(self->touchpad_box);
    remove_all_children(self->triggers_box);
    remove_all_children(self->buttons_flow);
    clear_element_slots(self);
    memset(self->stick_positions, 0, sizeof(self->stick_positions));
    self->touchpad_widget = NULL;
    self->rumble_sdl_id = 0;
    self->sensors_enabled = FALSE;
    self->active_api = api;
    gtk_widget_set_visible(self->actions_box, FALSE);
    g_clear_pointer(&self->evdev_syspath, g_free);
    self->speaker_supported = FALSE;
    self->speaker_is_wired = FALSE;
    self->speaker_is_wireless = FALSE;
    self->tilt_widget = NULL;
    self->gyro_x_bar = self->gyro_y_bar = self->gyro_z_bar = NULL;

    self->current_ctx = viewer_session_context_new(self);

    switch (api) {
    case CONTTEST_BACKEND_SDL3:
        self->active_session = conttest_input_session_sdl3_start(sdl_id, self->current_ctx);
        break;
    case CONTTEST_BACKEND_EVDEV:
        self->active_session = conttest_input_session_evdev_start(evdev_devnode, self->current_ctx);
        break;
    default:
        g_atomic_int_set(&self->current_ctx->active, 0);
        g_clear_pointer(&self->current_ctx, viewer_session_context_unref);
        g_return_val_if_reached(FALSE);
    }

    if (!self->active_session) {
        g_atomic_int_set(&self->current_ctx->active, 0);
        g_clear_pointer(&self->current_ctx, viewer_session_context_unref);
        return FALSE;
    }

    conttest_input_viewer_refresh_device(self, device);

    if (api == CONTTEST_BACKEND_SDL3) {
        self->rumble_sdl_id = (SDL_JoystickID)sdl_id;

        SDL_Gamepad *gp = SDL_GetGamepadFromID(self->rumble_sdl_id);
        if (gp && (SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL) ||
                   SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO))) {
            if (SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL))
                SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, true);
            if (SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO))
                SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true);
            self->sensors_enabled = TRUE;

            self->tilt_widget = conttest_tilt_indicator_new();
            adw_wrap_box_append(ADW_WRAP_BOX(self->sticks_box), self->tilt_widget);
            gtk_widget_set_visible(self->sticks_box, TRUE);

            self->gyro_x_bar = conttest_trigger_bar_new(_("Gyro X"));
            self->gyro_y_bar = conttest_trigger_bar_new(_("Gyro Y"));
            self->gyro_z_bar = conttest_trigger_bar_new(_("Gyro Z"));
            conttest_trigger_bar_set_bipolar(CONTTEST_TRIGGER_BAR(self->gyro_x_bar), TRUE);
            conttest_trigger_bar_set_bipolar(CONTTEST_TRIGGER_BAR(self->gyro_y_bar), TRUE);
            conttest_trigger_bar_set_bipolar(CONTTEST_TRIGGER_BAR(self->gyro_z_bar), TRUE);
            adw_wrap_box_append(ADW_WRAP_BOX(self->triggers_box), self->gyro_x_bar);
            adw_wrap_box_append(ADW_WRAP_BOX(self->triggers_box), self->gyro_y_bar);
            adw_wrap_box_append(ADW_WRAP_BOX(self->triggers_box), self->gyro_z_bar);
            gtk_widget_set_visible(self->triggers_box, TRUE);
        }
    }

    return TRUE;
}

void conttest_input_viewer_refresh_device(ConttestInputViewer *self,
                                          const ConttestDevice *device) {
    g_return_if_fail(CONTTEST_IS_INPUT_VIEWER(self));
    if (!self->active_session)
        return;

    gboolean is_ds5 = device && conttest_device_is_ds5(device);
    gboolean is_virtual = device && conttest_device_is_virtual(device);
    self->speaker_supported = (is_ds5 && !is_virtual);

    SDL_JoystickConnectionState conn = device
        ? conttest_device_get_connection_state(device)
        : SDL_JOYSTICK_CONNECTION_UNKNOWN;
    self->speaker_is_wired = (conn == SDL_JOYSTICK_CONNECTION_WIRED);
    self->speaker_is_wireless = (conn == SDL_JOYSTICK_CONNECTION_WIRELESS);

    const char *evdev = device ? conttest_device_get_evdev_syspath(device) : NULL;
    if (g_strcmp0(self->evdev_syspath, evdev) != 0) {
        cancel_speaker_test(self);
        g_free(self->evdev_syspath);
        self->evdev_syspath = g_strdup(evdev);
    }

    sync_action_controls(self);
}

void conttest_input_viewer_stop(ConttestInputViewer *self) {
    if (self->sensors_enabled && self->rumble_sdl_id != 0) {
        SDL_Gamepad *gp = SDL_GetGamepadFromID(self->rumble_sdl_id);
        if (gp) {
            if (SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL))
                SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, false);
            if (SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO))
                SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, false);
        }
    }
    self->sensors_enabled = FALSE;
    self->rumble_sdl_id = 0;

    cancel_speaker_test(self);
    self->speaker_supported = FALSE;
    self->speaker_is_wired = FALSE;
    self->speaker_is_wireless = FALSE;
    g_clear_pointer(&self->evdev_syspath, g_free);
    if (self->actions_box)
        gtk_widget_set_visible(self->actions_box, FALSE);
    if (self->current_ctx) {
        /* Signal all in-flight callbacks to discard their work. Must happen
         * before releasing our ref so that callbacks reading ctx->viewer
         * (which points to us) do so while we are still alive. */
        g_atomic_int_set(&self->current_ctx->active, 0);

        /* Release the viewer's ref (ref #1, set to 1 at allocation). */
        g_clear_pointer(&self->current_ctx, viewer_session_context_unref);
    }
    if (self->active_session) {
        /* SDL3: removes the tick callback → sdl3_tick_destroyed releases ref #2.
         * evdev: joins the thread → thread-exit path releases ref #2.
         * In both cases, if the backend already released ref #2 before stop
         * was called, the cleanup path is a no-op (tick_id==0 / thread done). */
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
        viewer_session_context_unref(ctx);
        return G_SOURCE_REMOVE;
    }

    ConttestInputViewer *self = ctx->viewer;

    GtkWidget *stick_widgets[CONTTEST_MAX_STICKS] = { NULL };

    for (int i = 0; i < layout->num_elements; i++) {
        const ConttestInputElementDef *def = &layout->elements[i];

        if (def->domain == CONTTEST_ELEMENT_TOUCHPAD_PAD) {
            /* Create the touchpad visualizer widget; no button indicator slot. */
            if (!self->touchpad_widget) {
                self->touchpad_widget = conttest_touchpad_new();
                gtk_box_append(GTK_BOX(self->touchpad_box), self->touchpad_widget);
                gtk_widget_set_visible(self->touchpad_box, TRUE);
            }
            continue;
        }

        ElementWidgetSlot *slot = lookup_slot(self, def->domain, def->element_id);
        if (!slot) {
            g_warning("Element (%d, %d) outside flat lookup bounds, skipping",
                      def->domain, def->element_id);
            continue;
        }

        GtkWidget *widget = NULL;
        gboolean slot_is_trigger = def->is_trigger;

        if (!def->is_axis) {
            widget = conttest_button_indicator_new(def->name);
            adw_wrap_box_append(ADW_WRAP_BOX(self->buttons_flow), widget);
            gtk_widget_set_visible(self->buttons_flow, TRUE);
        } else if (def->is_trigger) {
            widget = conttest_trigger_bar_new(def->name);
            adw_wrap_box_append(ADW_WRAP_BOX(self->triggers_box), widget);
            gtk_widget_set_visible(self->triggers_box, TRUE);
        } else if (def->is_stick_x || def->is_stick_y) {
            if (def->stick_id >= 0 && def->stick_id < CONTTEST_MAX_STICKS) {
                if (!stick_widgets[def->stick_id]) {
                    char *name = stick_display_name(def->stick_id);
                    GtkWidget *stick = conttest_analog_stick_new(name);
                    g_free(name);
                    stick_widgets[def->stick_id] = stick;
                    adw_wrap_box_append(ADW_WRAP_BOX(self->sticks_box), stick);
                    gtk_widget_set_visible(self->sticks_box, TRUE);
                }
                widget = stick_widgets[def->stick_id];
            } else {
                g_warning("Stick id %d exceeds CONTTEST_MAX_STICKS (%d), skipping", def->stick_id, CONTTEST_MAX_STICKS);
                continue;
            }
        } else {
            /* Generic axis: render as a trigger bar (bipolar if signed range) */
            slot_is_trigger = TRUE;
            widget = conttest_trigger_bar_new(def->name);
            if (def->is_bipolar)
                conttest_trigger_bar_set_bipolar(CONTTEST_TRIGGER_BAR(widget), TRUE);
            adw_wrap_box_append(ADW_WRAP_BOX(self->triggers_box), widget);
            gtk_widget_set_visible(self->triggers_box, TRUE);
        }

        slot->widget = widget;
        slot->is_axis = def->is_axis;
        slot->is_trigger = slot_is_trigger;
        slot->is_bipolar = def->is_bipolar;
        slot->is_stick_x = def->is_stick_x;
        slot->is_stick_y = def->is_stick_y;
        slot->stick_id = def->stick_id;
    }

    for (int i = 0; i < layout->num_elements; ++i) {
        g_free(layout->elements[i].name);
    }
    g_free(layout->elements);
    g_free(layout);

    viewer_session_context_unref(ctx);
    return G_SOURCE_REMOVE;
}

/* Apply a single event to the viewer. ctx must be active and current. */
static void apply_input_event(ConttestInputViewer *self, const ConttestInputEvent *ev) {

    if (ev->type == INPUT_EVENT_SENSOR) {
        if (ev->domain != CONTTEST_ELEMENT_SENSOR)
            return;
        if (ev->element_id == CONTTEST_SENSOR_ACCEL && self->tilt_widget) {
            conttest_tilt_indicator_set_accel(CONTTEST_TILT_INDICATOR(self->tilt_widget),
                                              ev->sensor[0], ev->sensor[1], ev->sensor[2]);
        } else if (ev->element_id == CONTTEST_SENSOR_GYRO && self->gyro_x_bar) {
            static const double GYRO_MAX = 10.0;
            conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(self->gyro_x_bar),
                                           CLAMP((double)ev->sensor[0] / GYRO_MAX, -1.0, 1.0));
            conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(self->gyro_y_bar),
                                           CLAMP((double)ev->sensor[1] / GYRO_MAX, -1.0, 1.0));
            conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(self->gyro_z_bar),
                                           CLAMP((double)ev->sensor[2] / GYRO_MAX, -1.0, 1.0));
        }
    } else if (ev->type == INPUT_EVENT_TOUCHPAD) {
        ElementWidgetSlot *slot = lookup_slot(self, ev->domain, ev->element_id);
        if (slot) {
            if (slot->widget && !slot->is_axis)
                conttest_button_indicator_set_pressed(CONTTEST_BUTTON_INDICATOR(slot->widget),
                                                      ev->button_pressed);
            if (self->touchpad_widget)
                conttest_touchpad_set_finger_indexed(CONTTEST_TOUCHPAD(self->touchpad_widget),
                                                     (guint)ev->element_id,
                                                     ev->button_pressed,
                                                     ev->touch_x,
                                                     ev->touch_y);
        }
    } else if (ev->type == INPUT_EVENT_BUTTON) {
        ElementWidgetSlot *slot = lookup_slot(self, ev->domain, ev->element_id);
        if (slot && slot->widget && !slot->is_axis) {
            conttest_button_indicator_set_pressed(CONTTEST_BUTTON_INDICATOR(slot->widget),
                                                  ev->button_pressed);
        }
    } else if (ev->type == INPUT_EVENT_AXIS) {
        ElementWidgetSlot *slot = lookup_slot(self, ev->domain, ev->element_id);
        if (slot && slot->widget && slot->is_axis) {
            if (slot->is_trigger) {
                conttest_trigger_bar_set_value(CONTTEST_TRIGGER_BAR(slot->widget), ev->axis_value);
            } else if (slot->is_stick_x) {
                if (slot->stick_id >= 0 && slot->stick_id < CONTTEST_MAX_STICKS) {
                    self->stick_positions[slot->stick_id][0] = ev->axis_value;
                    conttest_analog_stick_set_position(CONTTEST_ANALOG_STICK(slot->widget),
                                                       self->stick_positions[slot->stick_id][0],
                                                       self->stick_positions[slot->stick_id][1]);
                }
            } else if (slot->is_stick_y) {
                if (slot->stick_id >= 0 && slot->stick_id < CONTTEST_MAX_STICKS) {
                    self->stick_positions[slot->stick_id][1] = ev->axis_value;
                    conttest_analog_stick_set_position(CONTTEST_ANALOG_STICK(slot->widget),
                                                       self->stick_positions[slot->stick_id][0],
                                                       self->stick_positions[slot->stick_id][1]);
                }
            }
        }
    }
}

/* Public wrapper for main-thread callers (SDL3 backend). */
void conttest_input_viewer_apply_event(ConttestInputViewer *self, const ConttestInputEvent *ev) {
    apply_input_event(self, ev);
}

gboolean conttest_input_viewer_drain_ring(gpointer data) {
    ViewerSessionContext *ctx = (ViewerSessionContext *)data;
    ConttestEventRing *ring = &ctx->ring;

    for (;;) {
        gboolean active = g_atomic_int_get(&ctx->active) &&
                          ctx->viewer && ctx->viewer->current_ctx == ctx;
        ConttestInputViewer *self = active ? ctx->viewer : NULL;
        ConttestInputEvent ev;

        while (conttest_event_ring_pop(ring, &ev)) {
            if (self)
                apply_input_event(self, &ev);
        }

        if (atomic_exchange_explicit(&ring->overflow, false, memory_order_relaxed)) {
            g_debug("Evdev event ring overflowed; dropped excess events");
        }

        /* Re-arm coalescing flag under seq_cst, then verify no producer pushed
         * between the final empty pop() and clearing scheduled. */
        atomic_store_explicit(&ring->scheduled, false, memory_order_seq_cst);
        unsigned int head = atomic_load_explicit(&ring->head, memory_order_relaxed);
        unsigned int tail = atomic_load_explicit(&ring->tail, memory_order_seq_cst);
        if (head == tail)
            break;

        /* Another event landed in the window: reclaim ownership unless the
         * producer already queued a fresh idle source. */
        if (atomic_exchange_explicit(&ring->scheduled, true, memory_order_seq_cst))
            break;
    }

    viewer_session_context_unref(ctx);
    return G_SOURCE_REMOVE;
}
