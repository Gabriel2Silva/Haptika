#include "input_session.h"
#include "input_viewer.h"
#include "device_manager.h"
#include <SDL3/SDL.h>
#include <glib/gi18n.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define JOY_EXTRA_AXES_MAX   64
#define TOUCHPAD_FINGERS_MAX 8
#define AXIS_NOISE_EPSILON   (256.0 / 32768.0)

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

typedef struct {
    gint                  ref_count;  /* 1 for session + 1 while tick callback is attached */
    SDL_JoystickID        sdl_id;
    SDL_Gamepad          *gamepad;    /* owned refcounted handle closed on cleanup */
    SDL_Joystick         *joystick;   /* borrowed from gamepad; valid until SDL_CloseGamepad */
    int                   joy_axis_base;
    int                   joy_extra_axes;
    ViewerSessionContext *ctx;
    GtkWidget            *viewer;     /* borrowed; non-NULL only while tick callback is attached */
    gint                 *atomic_stop_flag;
    guint                 tick_id;
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

static void sdl3_tick_destroyed(gpointer user_data) {
    SdlSessionData *data = user_data;
    data->tick_id = 0;
    data->viewer = NULL;
    viewer_session_context_unref(data->ctx);
    sdl_session_data_unref(data);
}

static gboolean sdl3_poll_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    (void)widget;
    (void)frame_clock;
    SdlSessionData *data = user_data;

    if (g_atomic_int_get(data->atomic_stop_flag) ||
        !g_atomic_int_get(&data->ctx->active)) {
        return G_SOURCE_REMOVE;
    }

    if (!SDL_GamepadConnected(data->gamepad))
        return G_SOURCE_REMOVE;

    conttest_sdl_pump_events();

    ConttestInputViewer *viewer = data->ctx->viewer;

    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        SDL_GamepadButton btn = (SDL_GamepadButton)i;
        if (!SDL_GamepadHasButton(data->gamepad, btn))
            continue;

        gboolean pressed = SDL_GetGamepadButton(data->gamepad, btn) != 0;
        if (pressed == data->btn_state[i])
            continue;
        data->btn_state[i] = pressed;

        ConttestInputEvent ev = {
            .type           = INPUT_EVENT_BUTTON,
            .domain         = CONTTEST_ELEMENT_BUTTON,
            .element_id     = i,
            .button_pressed = pressed,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
    }

    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) {
        SDL_GamepadAxis axis = (SDL_GamepadAxis)i;
        if (!SDL_GamepadHasAxis(data->gamepad, axis))
            continue;

        Sint16 raw = SDL_GetGamepadAxis(data->gamepad, axis);
        double norm = normalize_gamepad_axis(axis, raw);

        if (!axis_value_changed(data->axis_state[i], norm, AXIS_NOISE_EPSILON))
            continue;
        data->axis_state[i] = norm;

        ConttestInputEvent ev = {
            .type       = INPUT_EVENT_AXIS,
            .domain     = CONTTEST_ELEMENT_SDL_AXIS,
            .element_id = i,
            .axis_value = norm,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
    }

    /* Poll raw joystick axes beyond standard gamepad axes (e.g. DS3 pressures). */
    const double joy_span = (double)SDL_JOYSTICK_AXIS_MAX - (double)SDL_JOYSTICK_AXIS_MIN;
    for (int i = 0; i < data->joy_extra_axes; i++) {
        int axis_idx = data->joy_axis_base + i;
        Sint16 raw   = SDL_GetJoystickAxis(data->joystick, axis_idx);
        double norm  = CLAMP(((double)raw - (double)SDL_JOYSTICK_AXIS_MIN) / joy_span, 0.0, 1.0);

        if (!axis_value_changed(data->joy_axis_state[i], norm, AXIS_NOISE_EPSILON))
            continue;
        data->joy_axis_state[i] = norm;

        ConttestInputEvent ev = {
            .type       = INPUT_EVENT_AXIS,
            .domain     = CONTTEST_ELEMENT_SDL_EXTRA,
            .element_id = i,
            .axis_value = norm,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
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
                .button_pressed = touched,
                .touch_x        = x,
                .touch_y        = y,
            };
            conttest_input_viewer_apply_event(viewer, &ev);
        }
    }

    static const struct { SDL_SensorType type; ConttestSensorKind id; float eps; } sensors[] = {
        { SDL_SENSOR_ACCEL, CONTTEST_SENSOR_ACCEL, 0.005f },
        { SDL_SENSOR_GYRO,  CONTTEST_SENSOR_GYRO,  0.002f },
    };
    for (int s = 0; s < 2; s++) {
        if (!SDL_GamepadSensorEnabled(data->gamepad, sensors[s].type))
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
    }

    return G_SOURCE_CONTINUE;
}

static void free_sdl3_session(ConttestInputSession *session) {
    SdlSessionData *data = session->impl_data;
    if (data->viewer && data->tick_id != 0) {
        /* Invokes sdl3_tick_destroyed synchronously, clearing viewer/tick_id and releasing refs. */
        gtk_widget_remove_tick_callback(data->viewer, data->tick_id);
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
    /* session->thread remains NULL — no background thread for the SDL backend. */

    SdlSessionData *data = g_new0(SdlSessionData, 1);
    data->ref_count        = 1;
    data->sdl_id           = id;
    data->gamepad          = gamepad;
    data->joystick         = SDL_GetGamepadJoystick(gamepad);
    data->ctx              = ctx;
    data->atomic_stop_flag = &session->atomic_stop_flag;
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
        if (!SDL_GamepadHasButton(gamepad, SDL_BUTTONS[i].btn)) continue;
        defs[num].domain     = CONTTEST_ELEMENT_BUTTON;
        defs[num].element_id = SDL_BUTTONS[i].btn;

        const char *name = SDL_BUTTONS[i].name;
        if (is_gc) {
            if (SDL_BUTTONS[i].btn == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) name = "Z";
            else if (SDL_BUTTONS[i].btn == SDL_GAMEPAD_BUTTON_MISC3)     name = "ZL Full Press";
            else if (SDL_BUTTONS[i].btn == SDL_GAMEPAD_BUTTON_MISC4)     name = "ZR Full Press";
        } else if (is_ds5) {
            if (SDL_BUTTONS[i].btn == SDL_GAMEPAD_BUTTON_MISC1)          name = "Mute Microphone";
        }
        defs[num].name = g_strdup(name);
        num++;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(SDL_AXES); i++) {
        if (!SDL_GamepadHasAxis(gamepad, SDL_AXES[i].axis)) continue;
        defs[num].domain     = CONTTEST_ELEMENT_SDL_AXIS;
        defs[num].element_id = SDL_AXES[i].axis;
        defs[num].name       = g_strdup(SDL_AXES[i].name);
        defs[num].is_axis    = TRUE;
        defs[num].is_trigger = SDL_AXES[i].is_trigger;
        defs[num].is_stick_x = SDL_AXES[i].is_x;
        defs[num].is_stick_y = SDL_AXES[i].is_y;
        defs[num].stick_id   = SDL_AXES[i].stick_id;
        num++;
    }

    /* Extra raw joystick axes (e.g. DS3 pressure-sensitive buttons).
     *
     * The Linux hid-sony.c kernel driver exposes 10 analog button pressures
     * as joystick axes beyond the standard gamepad axes. SDL3 maps them
     * starting at joystick axis index 6 (after LX, LY, RX, RY, L2, R2).
     * Order matches the sixaxis_mapping GD Pointer usage indices in hid-sony.c:
     *   Cross, Circle, Square, Triangle, L1, R1, Up, Down, Left, Right */
    static const char *ds3_pressure_names[] = {
        "✕", "○", "□", "△",
        "L1", "R1", "↑", "↓", "←", "→"
    };
    for (int i = 0; i < data->joy_extra_axes; i++) {
        int axis_idx = data->joy_axis_base + i;
        defs[num].domain     = CONTTEST_ELEMENT_SDL_EXTRA;
        defs[num].element_id = i;
        defs[num].is_axis    = TRUE;
        defs[num].is_trigger = TRUE;
        if (axis_idx >= 6 && axis_idx <= 15 && (axis_idx - 6) < (int)G_N_ELEMENTS(ds3_pressure_names))
            defs[num].name = g_strdup(ds3_pressure_names[axis_idx - 6]);
        else
            defs[num].name = g_strdup_printf("Axis %d", axis_idx);
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

    if (ctx->viewer) {
        viewer_session_context_ref(ctx);
        g_atomic_int_inc(&data->ref_count);
        data->viewer = GTK_WIDGET(ctx->viewer);
        data->tick_id = gtk_widget_add_tick_callback(data->viewer, sdl3_poll_tick,
                                                     data, sdl3_tick_destroyed);
        if (data->tick_id == 0) {
            data->viewer = NULL;
            sdl_session_data_unref(data);
            viewer_session_context_unref(ctx);
        }
    }

    return session;
}
