#include "input_session.h"
#include "device_matching.h"
#include "input_viewer.h"
#include "device_manager.h"
#include <SDL3/SDL.h>
#include <glib/gi18n.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define JOY_EXTRA_AXES_MAX     64
#define TOUCHPAD_FINGERS_MAX   8
#define AXIS_NOISE_EPSILON     (256.0 / 32768.0)
/* Main-thread state sampling.
 *
 * Active input is sampled every millisecond so presses shorter than a display
 * frame are not missed. After ~250 ms without any state change the sampler
 * backs off to 4 ms, which still catches realistic presses well inside a frame
 * and cuts idle wakeups by 4x; the first change restores the 1 ms rate.
 * Suspended/unmapped windows back off to 50 ms. GTK paints on the frame clock,
 * so rendering is never tied to the sampling rate. */
#define SDL_POLL_ACTIVE_US     (1 * G_TIME_SPAN_MILLISECOND)
#define SDL_POLL_IDLE_US       (4 * G_TIME_SPAN_MILLISECOND)
#define SDL_POLL_SUSPENDED_US  (50 * G_TIME_SPAN_MILLISECOND)
#define SDL_POLL_IDLE_TICKS    250
#define SDL_POLL_PRIORITY      (GDK_PRIORITY_REDRAW + 5)

static const struct {
    SDL_GamepadButton btn;
    const char *name;
} SDL_BUTTONS[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH,          "A / Cross" },
    { SDL_GAMEPAD_BUTTON_EAST,           "B / Circle" },
    { SDL_GAMEPAD_BUTTON_WEST,           "X / Square" },
    { SDL_GAMEPAD_BUTTON_NORTH,          "Y / Triangle" },
    { SDL_GAMEPAD_BUTTON_BACK,           "Back / Share" },
    { SDL_GAMEPAD_BUTTON_GUIDE,          "Guide" },
    { SDL_GAMEPAD_BUTTON_START,          "Start / Options" },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK,     "L3" },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    "R3" },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  "L1" },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1" },
    { SDL_GAMEPAD_BUTTON_DPAD_UP,        "D-pad Up" },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      "D-pad Down" },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      "D-pad Left" },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     "D-pad Right" },
    { SDL_GAMEPAD_BUTTON_MISC1,          "Misc" },
    { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,  "Right Paddle 1" },
    { SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,   "Left Paddle 1" },
    { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,  "Right Paddle 2" },
    { SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,   "Left Paddle 2" },
    { SDL_GAMEPAD_BUTTON_TOUCHPAD,       "Touchpad Click" },
    { SDL_GAMEPAD_BUTTON_MISC2,          "Misc 2" },
    { SDL_GAMEPAD_BUTTON_MISC3,          "Misc 3" },
    { SDL_GAMEPAD_BUTTON_MISC4,          "Misc 4" },
    { SDL_GAMEPAD_BUTTON_MISC5,          "Misc 5" },
    { SDL_GAMEPAD_BUTTON_MISC6,          "Misc 6" }
};

static const struct {
    SDL_GamepadAxis axis;
    const char *name;
    gboolean is_trigger;
    gboolean is_x;
    gboolean is_y;
    int stick_id;
} SDL_AXES[] = {
    { SDL_GAMEPAD_AXIS_LEFTX,          "Left Stick X",  FALSE, TRUE,  FALSE,  0 },
    { SDL_GAMEPAD_AXIS_LEFTY,          "Left Stick Y",  FALSE, FALSE, TRUE,   0 },
    { SDL_GAMEPAD_AXIS_RIGHTX,         "Right Stick X", FALSE, TRUE,  FALSE,  1 },
    { SDL_GAMEPAD_AXIS_RIGHTY,         "Right Stick Y", FALSE, FALSE, TRUE,   1 },
    { SDL_GAMEPAD_AXIS_LEFT_TRIGGER,   "L2",            TRUE,  FALSE, FALSE, -1 },
    { SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,  "R2",            TRUE,  FALSE, FALSE, -1 },
};

_Static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32,
               "SDL_GamepadButton mask must fit in uint32_t");
_Static_assert(SDL_GAMEPAD_AXIS_COUNT <= 8,
               "SDL_GamepadAxis mask must fit in uint8_t");

typedef struct {
    gint                  ref_count;     /* 1 for session + 1 while poll source is attached */
    gint                  stopped;       /* atomic boolean owned by SdlSessionData */
    SDL_JoystickID        sdl_id;
    SDL_Gamepad          *gamepad;       /* owned refcounted handle closed on cleanup */
    SDL_Joystick         *joystick;      /* borrowed from gamepad; valid until SDL_CloseGamepad */
    uint32_t              button_mask;   /* static present-button bitmask */
    uint8_t               axis_mask;     /* static present-axis bitmask */
    uint8_t               sensor_mask;   /* bit 0 = ACCEL, bit 1 = GYRO */
    uint8_t               sensors_probed;
    int                   joy_axis_base;
    int                   joy_extra_axes;
    gboolean              joy_extra_unipolar[JOY_EXTRA_AXES_MAX];
    ViewerSessionContext *ctx;
    GSource              *poll_source;
    gint64                dispatch_time_us;
    gboolean              window_active;
    guint                 idle_ticks;
    gboolean              btn_state[SDL_GAMEPAD_BUTTON_COUNT];
    double                axis_state[SDL_GAMEPAD_AXIS_COUNT];
    double                joy_axis_state[JOY_EXTRA_AXES_MAX];
    gboolean              touchpad_finger_state[TOUCHPAD_FINGERS_MAX];
    float                 touchpad_x[TOUCHPAD_FINGERS_MAX];
    float                 touchpad_y[TOUCHPAD_FINGERS_MAX];
    int                   num_touchpad_fingers;
    gboolean              sensor_has_prev[2];
    float                 sensor_prev[2][3];
} SdlSessionData;

static void sdl_session_data_close_gamepad(SdlSessionData *data) {
    if (data->gamepad) {
        SDL_SetGamepadSensorEnabled(data->gamepad, SDL_SENSOR_ACCEL, false);
        SDL_SetGamepadSensorEnabled(data->gamepad, SDL_SENSOR_GYRO, false);
        SDL_CloseGamepad(data->gamepad);
        data->gamepad = NULL;
        data->joystick = NULL;
    }
}

static void sdl_session_data_unref(SdlSessionData *data) {
    if (!data)
        return;
    if (g_atomic_int_dec_and_test(&data->ref_count)) {
        sdl_session_data_close_gamepad(data);
        g_free(data);
    }
}

static double normalize_gamepad_axis(SDL_GamepadAxis axis, Sint16 raw) {
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return CLAMP((double)raw / (double)SDL_JOYSTICK_AXIS_MAX, 0.0, 1.0);
    if (raw >= 0)
        return (double)raw / (double)SDL_JOYSTICK_AXIS_MAX;
    return (double)raw / (-(double)SDL_JOYSTICK_AXIS_MIN);
}

static gboolean axis_value_changed(double prev, double next, double epsilon) {
    if (next == prev)
        return FALSE;
    /* Always flush boundary transitions so slow releases return to exact 0.0 or +/-1.0. */
    if (next == 0.0 || fabs(next) >= 1.0)
        return TRUE;
    return fabs(next - prev) > epsilon;
}

static void sdl3_poll_source_destroyed(gpointer user_data) {
    SdlSessionData *data = user_data;
    /* The session keeps its own source reference until teardown, so the
     * pointer in `data` stays valid even when the source removes itself. */
    viewer_session_context_unref(data->ctx);
    sdl_session_data_unref(data);
}

typedef struct {
    ConttestInputViewer *viewer;
    ViewerSessionContext *ctx;
} SessionLostEvent;

static gboolean session_lost_idle(gpointer user_data) {
    SessionLostEvent *event = user_data;
    conttest_input_viewer_session_lost(event->viewer, event->ctx);
    return G_SOURCE_REMOVE;
}

static void session_lost_event_free(gpointer data) {
    SessionLostEvent *event = data;
    g_object_unref(event->viewer);
    viewer_session_context_unref(event->ctx);
    g_free(event);
}

/* The session cannot be torn down from inside the dispatch callback that
 * detected the disconnect, so hand the loss to the viewer on the next
 * main-loop iteration, once the source and its data are gone. */
static void schedule_session_lost(SdlSessionData *data) {
    ConttestInputViewer *viewer = data->ctx->viewer;
    if (!viewer)
        return;
    SessionLostEvent *event = g_new(SessionLostEvent, 1);
    event->viewer = g_object_ref(viewer);
    event->ctx = viewer_session_context_ref(data->ctx);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, session_lost_idle, event,
                    session_lost_event_free);
}

static gboolean viewer_is_actively_visible(ConttestInputViewer *viewer) {
    if (!gtk_widget_get_mapped(GTK_WIDGET(viewer)))
        return FALSE;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(viewer));
    if (GTK_IS_WINDOW(root) && gtk_window_is_suspended(GTK_WINDOW(root)))
        return FALSE;
    return TRUE;
}

/* Main-thread ~1 ms state sampler. Only widget state is updated here; the
 * widgets queue draws and GTK coalesces them on the frame clock, so rendering
 * remains at the display refresh rate while input state is sampled at ~1 kHz. */
static gboolean sdl3_poll_timer(gpointer user_data) {
    SdlSessionData *data = user_data;

    if (g_atomic_int_get(&data->stopped) ||
        !g_atomic_int_get(&data->ctx->active)) {
        return G_SOURCE_REMOVE;
    }

    if (!data->gamepad || !SDL_GamepadConnected(data->gamepad)) {
        schedule_session_lost(data);
        return G_SOURCE_REMOVE;
    }

    ConttestInputViewer *viewer = data->ctx->viewer;
    if (!viewer)
        return G_SOURCE_REMOVE;

    data->window_active = viewer_is_actively_visible(viewer);
    if (!data->window_active)
        return G_SOURCE_CONTINUE;

    conttest_sdl_pump_events_at(data->dispatch_time_us);

    gboolean changed = FALSE;
    uint32_t btn_mask = data->button_mask;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        if (!(btn_mask & (UINT32_C(1) << i)))
            continue;
        SDL_GamepadButton btn = (SDL_GamepadButton)i;

        gboolean pressed = SDL_GetGamepadButton(data->gamepad, btn) != 0;
        if (pressed == data->btn_state[i])
            continue;
        data->btn_state[i] = pressed;

        ConttestInputEvent ev = {
            .type           = INPUT_EVENT_BUTTON,
            .domain         = CONTTEST_ELEMENT_BUTTON,
            .element_id     = i,
            .button_pressed = (unsigned int)pressed,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
        changed = TRUE;
    }

    uint8_t axis_mask = data->axis_mask;
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) {
        if (!(axis_mask & (1u << i)))
            continue;
        SDL_GamepadAxis axis = (SDL_GamepadAxis)i;

        Sint16 raw = SDL_GetGamepadAxis(data->gamepad, axis);
        double norm = normalize_gamepad_axis(axis, raw);

        if (!axis_value_changed(data->axis_state[i], norm, AXIS_NOISE_EPSILON))
            continue;
        data->axis_state[i] = norm;

        ConttestInputEvent ev = {
            .type       = INPUT_EVENT_AXIS,
            .domain     = CONTTEST_ELEMENT_SDL_AXIS,
            .element_id = i,
            .axis_value = (float)norm,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
        changed = TRUE;
    }

    /* Poll raw joystick axes beyond standard gamepad axes (e.g. DS3 pressures). */
    const double joy_span = (double)SDL_JOYSTICK_AXIS_MAX - (double)SDL_JOYSTICK_AXIS_MIN;
    for (int i = 0; i < data->joy_extra_axes; i++) {
        int axis_idx = data->joy_axis_base + i;
        Sint16 raw   = SDL_GetJoystickAxis(data->joystick, axis_idx);
        double norm;
        if (data->joy_extra_unipolar[i])
            norm = CLAMP(((double)raw - (double)SDL_JOYSTICK_AXIS_MIN) / joy_span, 0.0, 1.0);
        else if (raw >= 0)
            norm = (double)raw / (double)SDL_JOYSTICK_AXIS_MAX;
        else
            norm = (double)raw / (-(double)SDL_JOYSTICK_AXIS_MIN);

        if (!axis_value_changed(data->joy_axis_state[i], norm, AXIS_NOISE_EPSILON))
            continue;
        data->joy_axis_state[i] = norm;

        ConttestInputEvent ev = {
            .type       = INPUT_EVENT_AXIS,
            .domain     = CONTTEST_ELEMENT_SDL_EXTRA,
            .element_id = i,
            .axis_value = (float)norm,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
        changed = TRUE;
    }

    for (int f = 0; f < data->num_touchpad_fingers; f++) {
        float x = 0.0f, y = 0.0f, pressure = 0.0f;
        bool down = false;
        if (!SDL_GetGamepadTouchpadFinger(data->gamepad, 0, f, &down, &x, &y, &pressure))
            continue;

        gboolean touched = (gboolean)down;
        gboolean state_changed = (touched != data->touchpad_finger_state[f]);
        gboolean moved = touched && (fabsf(x - data->touchpad_x[f]) > 0.001f ||
                                     fabsf(y - data->touchpad_y[f]) > 0.001f);
        if (state_changed || moved) {
            data->touchpad_finger_state[f] = touched;
            data->touchpad_x[f] = x;
            data->touchpad_y[f] = y;
            ConttestInputEvent ev = {
                .type           = INPUT_EVENT_TOUCHPAD,
                .domain         = CONTTEST_ELEMENT_TOUCHPAD_FINGER,
                .element_id     = f,
                .button_pressed = (unsigned int)touched,
                .touch_x        = x,
                .touch_y        = y,
            };
            conttest_input_viewer_apply_event(viewer, &ev);
            changed = TRUE;
        }
    }

    static const struct { SDL_SensorType type; ConttestSensorKind id; float eps; } sensors[] = {
        { SDL_SENSOR_ACCEL, CONTTEST_SENSOR_ACCEL, 0.015f },
        { SDL_SENSOR_GYRO,  CONTTEST_SENSOR_GYRO,  0.002f },
    };
    if (G_UNLIKELY(!data->sensors_probed)) {
        uint8_t smask = 0;
        for (int s = 0; s < 2; s++) {
            if (SDL_GamepadSensorEnabled(data->gamepad, sensors[s].type))
                smask |= (uint8_t)(1u << s);
        }
        data->sensor_mask    = smask;
        data->sensors_probed = 1;
    }
    uint8_t sensor_mask = data->sensor_mask;
    for (int s = 0; s < 2; s++) {
        if (!(sensor_mask & (1u << s)))
            continue;
        float vals[3] = {0};
        if (!SDL_GetGamepadSensorData(data->gamepad, sensors[s].type, vals, 3))
            continue;
        if (data->sensor_has_prev[s] &&
            fabsf(vals[0] - data->sensor_prev[s][0]) <= sensors[s].eps &&
            fabsf(vals[1] - data->sensor_prev[s][1]) <= sensors[s].eps &&
            fabsf(vals[2] - data->sensor_prev[s][2]) <= sensors[s].eps) {
            continue;
        }
        data->sensor_has_prev[s] = TRUE;
        memcpy(data->sensor_prev[s], vals, sizeof(vals));

        ConttestInputEvent ev = {
            .type       = INPUT_EVENT_SENSOR,
            .domain     = CONTTEST_ELEMENT_SENSOR,
            .element_id = sensors[s].id,
            .sensor     = { vals[0], vals[1], vals[2] },
        };
        conttest_input_viewer_apply_event(viewer, &ev);
        changed = TRUE;
    }

    /* Back off only after a run of ticks that produced no state change. */
    if (changed)
        data->idle_ticks = 0;
    else if (data->idle_ticks < SDL_POLL_IDLE_TICKS)
        data->idle_ticks++;

    return G_SOURCE_CONTINUE;
}

static gboolean sdl3_poll_source_dispatch(GSource *source,
                                          GSourceFunc callback,
                                          gpointer user_data) {
    if (!callback)
        return G_SOURCE_REMOVE;
    SdlSessionData *data = user_data;
    gint64 now_us = g_source_get_time(source);
    data->dispatch_time_us = now_us;
    if (!callback(user_data)) {
        g_source_set_ready_time(source, -1);
        return G_SOURCE_REMOVE;
    }
    gint64 delay;
    if (!data->window_active)
        delay = SDL_POLL_SUSPENDED_US;
    else if (data->idle_ticks >= SDL_POLL_IDLE_TICKS)
        delay = SDL_POLL_IDLE_US;
    else
        delay = SDL_POLL_ACTIVE_US;
    g_source_set_ready_time(source, now_us + delay);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs sdl3_poll_source_funcs = {
    .dispatch = sdl3_poll_source_dispatch,
};

static void free_sdl3_session(ConttestInputSession *session) {
    SdlSessionData *data = session->impl_data;
    g_atomic_int_set(&data->stopped, 1);
    GSource *src = data->poll_source;
    if (src) {
        g_source_destroy(src);
        g_source_unref(src);
        data->poll_source = NULL;
    }
    sdl_session_data_close_gamepad(data);
    sdl_session_data_unref(data);
}

_Static_assert(sizeof(SDL_JoystickID) == sizeof(uint32_t),
               "SDL_JoystickID must match uint32_t");

ConttestInputSession *conttest_input_session_sdl3_start(uint32_t sdl_id, ViewerSessionContext *ctx) {
    if (sdl_id == 0 || !ctx)
        return NULL;
    SDL_JoystickID id = (SDL_JoystickID)sdl_id;
    SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
    if (!gamepad)
        return NULL;

    ConttestInputSession *session = g_new0(ConttestInputSession, 1);
    session->free_func = free_sdl3_session;
    /* session->thread remains NULL; no background thread for the SDL backend. */

    SdlSessionData *data = g_new0(SdlSessionData, 1);
    data->ref_count        = 1;
    data->sdl_id           = id;
    data->gamepad          = gamepad;
    data->joystick         = SDL_GetGamepadJoystick(gamepad);
    data->ctx              = ctx;
    data->window_active    = TRUE;
    session->impl_data     = data;

    /* Detect raw joystick axes beyond what the gamepad abstraction exposes.
     * SDL_GAMEPAD_AXIS_COUNT is the enum sentinel value for SDL_GamepadAxis
     * (currently 6: LX, LY, RX, RY, LT, RT). If SDL3 adds new standard
     * gamepad axes in the future, this will automatically adjust.
     * Any joystick axes beyond that count are device-specific
     * (e.g. DS3 pressure-sensitive buttons). */
    int total_joy_axes = data->joystick ? SDL_GetNumJoystickAxes(data->joystick) : 0;
    data->joy_extra_axes = MIN(MAX(0, total_joy_axes - SDL_GAMEPAD_AXIS_COUNT), JOY_EXTRA_AXES_MAX);
    data->joy_axis_base  = total_joy_axes - data->joy_extra_axes;

    /* Detect touchpad fingers on touchpad 0 (DS4 has one touchpad). */
    int num_touchpads = SDL_GetNumGamepadTouchpads(gamepad);
    if (num_touchpads > 0) {
        int fingers = SDL_GetNumGamepadTouchpadFingers(gamepad, 0);
        data->num_touchpad_fingers = CLAMP(fingers, 0, TOUCHPAD_FINGERS_MAX);
    }

    /* Build layout. Reserve +1 for CONTTEST_ELEMENT_TOUCHPAD_PAD when a touchpad is present. */
    int max_elements = (int)(G_N_ELEMENTS(SDL_BUTTONS) + G_N_ELEMENTS(SDL_AXES))
                     + data->joy_extra_axes
                     + (data->num_touchpad_fingers > 0 ? 1 + data->num_touchpad_fingers : 0);
    ConttestInputElementDef *defs = g_new0(ConttestInputElementDef, (gsize)max_elements);
    int num = 0;

    Uint16 vid = SDL_GetGamepadVendorForID(id);
    Uint16 pid = SDL_GetGamepadProductForID(id);
    /* GameCube adapter (057e:0337): rename generic SDL labels to GC names. */
    gboolean is_gc = (vid == 0x057e && pid == 0x0337);
    /* DualSense: MISC1 is the mute button. */
    gboolean is_ds5 = (vid == SONY_VID &&
                       (pid == DS5_PID_V1 || pid == DS5_PID_V2));

    for (size_t i = 0; i < G_N_ELEMENTS(SDL_BUTTONS); i++) {
        SDL_GamepadButton btn = SDL_BUTTONS[i].btn;
        if (!SDL_GamepadHasButton(gamepad, btn)) continue;
        if ((int)btn >= 0 && (int)btn < SDL_GAMEPAD_BUTTON_COUNT)
            data->button_mask |= (UINT32_C(1) << (unsigned int)btn);
        defs[num].domain     = CONTTEST_ELEMENT_BUTTON;
        defs[num].element_id = btn;

        const char *name = SDL_BUTTONS[i].name;
        if (is_gc) {
            if (btn == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) name = "Z";
            else if (btn == SDL_GAMEPAD_BUTTON_MISC3)     name = "ZL Full Press";
            else if (btn == SDL_GAMEPAD_BUTTON_MISC4)     name = "ZR Full Press";
        } else if (is_ds5) {
            if (btn == SDL_GAMEPAD_BUTTON_MISC1)          name = "Mute Microphone";
        }
        defs[num].name = g_strdup(name);
        num++;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(SDL_AXES); i++) {
        SDL_GamepadAxis axis = SDL_AXES[i].axis;
        if (!SDL_GamepadHasAxis(gamepad, axis)) continue;
        if ((int)axis >= 0 && (int)axis < SDL_GAMEPAD_AXIS_COUNT)
            data->axis_mask |= (uint8_t)(1u << (unsigned int)axis);
        defs[num].domain     = CONTTEST_ELEMENT_SDL_AXIS;
        defs[num].element_id = axis;
        defs[num].name       = g_strdup(SDL_AXES[i].name);
        defs[num].is_axis    = TRUE;
        defs[num].is_trigger = SDL_AXES[i].is_trigger;
        defs[num].is_stick_x = SDL_AXES[i].is_x;
        defs[num].is_stick_y = SDL_AXES[i].is_y;
        defs[num].stick_id   = SDL_AXES[i].stick_id;
        num++;
    }

    /* Extra raw joystick axes.
     *
     * The Linux hid-sony driver exposes 10 DualShock 3 button pressures as
     * joystick axes beyond the standard gamepad axes, starting at index 6
     * (after LX, LY, RX, RY, L2, R2). Order matches the sixaxis_mapping GD
     * Pointer usage indices in hid-sony.c:
     *   Cross, Circle, Square, Triangle, L1, R1, Up, Down, Left, Right
     * Anything else keeps its native polarity and a neutral name. */
    static const char *ds3_pressure_names[] = {
        "✕", "○", "□", "△",
        "L1", "R1", "↑", "↓", "←", "→"
    };
    for (int i = 0; i < data->joy_extra_axes; i++) {
        int axis_idx = data->joy_axis_base + i;
        defs[num].domain     = CONTTEST_ELEMENT_SDL_EXTRA;
        defs[num].element_id = i;
        defs[num].is_axis    = TRUE;

        int pressure = conttest_ds3_pressure_axis_index(vid, pid, axis_idx);
        if (pressure >= 0 && pressure < (int)G_N_ELEMENTS(ds3_pressure_names)) {
            data->joy_extra_unipolar[i] = TRUE;
            defs[num].is_trigger = TRUE;
            defs[num].name = g_strdup(ds3_pressure_names[pressure]);
        } else {
            defs[num].is_bipolar = TRUE;
            defs[num].name = g_strdup_printf("Axis %d", axis_idx);
        }
        num++;
    }

    if (data->num_touchpad_fingers > 0) {
        defs[num].domain = CONTTEST_ELEMENT_TOUCHPAD_PAD;
        defs[num].name   = g_strdup(_("Touchpad"));
        num++;
        for (int f = 0; f < data->num_touchpad_fingers; f++) {
            defs[num].domain     = CONTTEST_ELEMENT_TOUCHPAD_FINGER;
            defs[num].element_id = f;
            defs[num].name       = g_strdup_printf(_("Touchpad Finger %d"), f + 1);
            num++;
        }
    }

    ConttestInputLayout *layout = g_new0(ConttestInputLayout, 1);
    layout->elements     = defs;
    layout->num_elements = num;
    layout->ctx          = ctx;

    viewer_session_context_ref(ctx);
    g_main_context_invoke(NULL, conttest_input_viewer_handle_layout, layout);

    gboolean polling = FALSE;
    if (ctx->viewer) {
        GSource *src = g_source_new(&sdl3_poll_source_funcs, sizeof(GSource));
        g_source_set_priority(src, SDL_POLL_PRIORITY);
        g_source_set_name(src, "haptika-sdl3-poll");
        viewer_session_context_ref(ctx);
        g_atomic_int_inc(&data->ref_count);
        g_source_set_callback(src, sdl3_poll_timer, data, sdl3_poll_source_destroyed);
        g_source_set_ready_time(src, g_get_monotonic_time() + SDL_POLL_ACTIVE_US);
        data->poll_source = src;
        polling = g_source_attach(src, NULL) != 0;
        if (!polling) {
            g_source_destroy(src);
            g_source_unref(src);
            data->poll_source = NULL;
        }
    }

    if (!polling) {
        /* Without a poll source the page would stay frozen; report the start
         * failure so the viewer can fall back or return to the selector. */
        conttest_input_session_stop(session);
        return NULL;
    }

    return session;
}
