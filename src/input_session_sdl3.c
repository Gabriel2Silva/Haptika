#include "input_viewer.h"
#include "device_manager.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>

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
    SDL_Gamepad          *gamepad;
    SDL_Joystick         *joystick;   /* borrowed from gamepad, not closed separately */
    int                   joy_extra_axes; /* number of raw joystick axes beyond the gamepad axes */
    ViewerSessionContext *ctx;
    gint                 *atomic_stop_flag;
    guint                 timer_id;
    gboolean              btn_state[32];
    double                axis_state[32];
#define JOY_EXTRA_AXES_MAX 64
    double                joy_axis_state[JOY_EXTRA_AXES_MAX];
#define TOUCHPAD_FINGERS_MAX 8
    gboolean              touchpad_finger_state[TOUCHPAD_FINGERS_MAX]; /* touched = TRUE */
    int                   num_touchpad_fingers;
} SdlSessionData;

/* GDestroyNotify called when the timer source is removed, in all cases.
 * Releases the ctx ref held on behalf of the timer. */
static void sdl3_tick_destroyed(gpointer user_data) {
    SdlSessionData *data = user_data;
    data->timer_id = 0;
    if (g_atomic_int_dec_and_test(&data->ctx->ref_count)) {
        g_free(data->ctx);
    }
}

/* Main-thread timer: poll gamepad state and dispatch events directly.
 * No SDL calls are made from any background thread. */
static gboolean sdl3_poll_tick(gpointer user_data) {
    SdlSessionData *data = user_data;

    /* Stop if externally requested or viewer context deactivated. */
    if (g_atomic_int_get(data->atomic_stop_flag) ||
        !g_atomic_int_get(&data->ctx->active)) {
        return G_SOURCE_REMOVE;
    }

    /* Stop if gamepad was unplugged; device manager handles the UI. */

    if (!SDL_GamepadConnected(data->gamepad)) {
        return G_SOURCE_REMOVE;
    }

    /* We are on the main thread, so dispatch directly via apply_input_event
     * using stack-allocated event structs — no heap allocation or refcount
     * manipulation needed. The evdev path still uses g_idle_add because it
     * crosses a thread boundary. */

    /* Pump at ~1000 Hz so SDL_GetGamepad*() returns the freshest state.
     * The device manager also pumps at 50 ms for hotplug events, but that
     * is too slow for responsive input visualization. Both pumps are on
     * the main thread and SDL_PumpEvents is idempotent. */
    SDL_PumpEvents();

    ConttestInputViewer *viewer = data->ctx->viewer;

    for (int i = 0; i < 32; i++) {
        if (!SDL_GamepadHasButton(data->gamepad, i)) continue;

        gboolean pressed = SDL_GetGamepadButton(data->gamepad, i) != 0;
        if (pressed == data->btn_state[i]) continue;
        data->btn_state[i] = pressed;

        ConttestInputEvent ev = {
            .ctx            = data->ctx,
            .type           = INPUT_EVENT_BUTTON,
            .element_id     = i,
            .button_pressed = pressed,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
    }

    for (int i = 0; i < 32; i++) {
        if (!SDL_GamepadHasAxis(data->gamepad, i)) continue;

        Sint16 raw  = SDL_GetGamepadAxis(data->gamepad, i);
        double norm = (i == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || i == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            ? (double)raw / 32767.0
            : (double)raw / 32768.0;

        /* Noise filter: compare normalised values to avoid the asymmetric
         * integer-cast rounding that plagued the previous implementation. */
        if (fabs(norm - data->axis_state[i]) <= (256.0 / 32768.0)) continue;
        data->axis_state[i] = norm;

        ConttestInputEvent ev = {
            .ctx        = data->ctx,
            .type       = INPUT_EVENT_AXIS,
            .element_id = ELEMENT_NS_SDL_AXIS_BASE + i,
            .axis_value = norm,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
    }

    /* Poll raw joystick axes that the gamepad abstraction doesn't expose
     * (e.g. DS3 pressure-sensitive buttons on axes 6–15).
     * Values come in as -32768 (unpressed) to 32767 (fully pressed);
     * normalise to 0–1 for the trigger bar widget. */
    for (int i = 0; i < data->joy_extra_axes; i++) {
        int axis_idx = SDL_GetNumJoystickAxes(data->joystick) - data->joy_extra_axes + i;
        Sint16 raw   = SDL_GetJoystickAxis(data->joystick, axis_idx);
        double norm  = (double)(raw - (-32768)) / 65535.0;

        if (fabs(norm - data->joy_axis_state[i]) <= (256.0 / 65535.0)) continue;
        data->joy_axis_state[i] = norm;

        ConttestInputEvent ev = {
            .ctx        = data->ctx,
            .type       = INPUT_EVENT_AXIS,
            .element_id = ELEMENT_NS_SDL_EXTRA_BASE + i,
            .axis_value = norm,
        };
        conttest_input_viewer_apply_event(viewer, &ev);
    }

    /* Poll touchpad fingers.
     * Finger 0 (element_id ELEMENT_NS_TOUCHPAD_BASE+0): fires every tick when
     * down for real-time position updates, and once on lift.
     * Finger 1+ (element_id ELEMENT_NS_TOUCHPAD_BASE+f): fires only on state change. */
    for (int f = 0; f < data->num_touchpad_fingers; f++) {
        float x, y, pressure;
        bool down = false;
        if (!SDL_GetGamepadTouchpadFinger(data->gamepad, 0, f, &down, &x, &y, &pressure)) continue;
        gboolean touched = (gboolean)down;
        gboolean state_changed = (touched != data->touchpad_finger_state[f]);
        if (f == 0 ? (touched || state_changed) : state_changed) {
            data->touchpad_finger_state[f] = touched;
            ConttestInputEvent ev = {
                .ctx            = data->ctx,
                .type           = INPUT_EVENT_TOUCHPAD,
                .element_id     = ELEMENT_NS_TOUCHPAD_BASE + f,
                .button_pressed = touched,
                .touch_x        = x,
                .touch_y        = y,
            };
            conttest_input_viewer_apply_event(viewer, &ev);
        }
    }

    /* Poll accelerometer (element_id ELEMENT_NS_ACCEL) and gyro (element_id ELEMENT_NS_GYRO). */
    static const struct { SDL_SensorType type; int id; } sensors[] = {
        { SDL_SENSOR_ACCEL, ELEMENT_NS_ACCEL },
        { SDL_SENSOR_GYRO,  ELEMENT_NS_GYRO  },
    };
    for (int s = 0; s < 2; s++) {
        if (!SDL_GamepadSensorEnabled(data->gamepad, sensors[s].type)) continue;
        float vals[3] = {0};
        if (!SDL_GetGamepadSensorData(data->gamepad, sensors[s].type, vals, 3)) continue;
        ConttestInputEvent ev = {
            .ctx        = data->ctx,
            .type       = INPUT_EVENT_SENSOR,
            .element_id = sensors[s].id,
            .sensor     = { vals[0], vals[1], vals[2] },
        };
        conttest_input_viewer_apply_event(viewer, &ev);
    }

    return G_SOURCE_CONTINUE;

}

static void free_sdl3_session(ConttestInputSession *session) {
    SdlSessionData *data = session->impl_data;
    if (data->timer_id) {
        /* sdl3_tick_destroyed is invoked synchronously here, releasing the
         * ctx ref held by the timer and zeroing data->timer_id. */
        g_source_remove(data->timer_id);
    }
    /* gamepad handle is borrowed from the device manager; do not close it. */
    g_free(data);
}

ConttestInputSession *conttest_input_session_sdl3_start(const char *id_str, ViewerSessionContext *ctx) {
    SDL_JoystickID id     = (SDL_JoystickID)strtoul(id_str, NULL, 10);
    /* Borrow the handle already open by the device manager. */
    SDL_Gamepad   *gamepad = SDL_GetGamepadFromID(id);
    if (!gamepad) return NULL;

    ConttestInputSession *session = g_new0(ConttestInputSession, 1);
    session->free_func = free_sdl3_session;
    /* session->thread remains NULL — no background thread for the SDL backend. */

    SdlSessionData *data = g_new0(SdlSessionData, 1);
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

    /* Detect touchpad fingers on touchpad 0 (DS4 has one touchpad). */
    int num_touchpads = SDL_GetNumGamepadTouchpads(gamepad);
    if (num_touchpads > 0) {
        int fingers = SDL_GetNumGamepadTouchpadFingers(gamepad, 0);
        data->num_touchpad_fingers = MIN(fingers, TOUCHPAD_FINGERS_MAX);
    }

    /* Build layout. */
    int max_elements = (int)(G_N_ELEMENTS(SDL_BUTTONS) + G_N_ELEMENTS(SDL_AXES)) + data->joy_extra_axes + data->num_touchpad_fingers;
    ConttestInputElementDef *defs = g_new0(ConttestInputElementDef, max_elements);
    int num = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(SDL_BUTTONS); i++) {
        if (!SDL_GamepadHasButton(gamepad, SDL_BUTTONS[i].btn)) continue;
        defs[num].element_id = SDL_BUTTONS[i].btn;

        /* GameCube adapter (057e:0337): rename generic SDL labels to GC names. */
        gboolean is_gc = (SDL_GetGamepadVendorForID(id) == 0x057e &&
                          SDL_GetGamepadProductForID(id) == 0x0337);
        /* DualSense: MISC1 is the mute button. */
        gboolean is_ds5 = (SDL_GetGamepadVendorForID(id) == SONY_VID &&
                           (SDL_GetGamepadProductForID(id) == DS5_PID_V1 ||
                            SDL_GetGamepadProductForID(id) == DS5_PID_V2));
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
        defs[num].element_id = ELEMENT_NS_SDL_AXIS_BASE + SDL_AXES[i].axis;
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
    int joy_axis_base = total_joy_axes - data->joy_extra_axes;
    for (int i = 0; i < data->joy_extra_axes; i++) {
        int axis_idx = joy_axis_base + i;
        defs[num].element_id = ELEMENT_NS_SDL_EXTRA_BASE + i;
        defs[num].is_axis    = TRUE;
        defs[num].is_trigger = TRUE;
        if (axis_idx >= 6 && axis_idx <= 15 && (axis_idx - 6) < (int)G_N_ELEMENTS(ds3_pressure_names))
            defs[num].name = g_strdup(ds3_pressure_names[axis_idx - 6]);
        else
            defs[num].name = g_strdup_printf("Axis %d", axis_idx);
        num++;
    }

    /* Touchpad: ELEMENT_NS_TOUCHPAD_SENTINEL = sentinel to create the visualizer widget.
     * ELEMENT_NS_TOUCHPAD_BASE+f = button indicators for each finger ("Touchpad Finger N"). */
    if (data->num_touchpad_fingers > 0) {
        defs[num].element_id = ELEMENT_NS_TOUCHPAD_SENTINEL;
        defs[num].name       = g_strdup("Touchpad");
        num++;
        for (int f = 0; f < data->num_touchpad_fingers; f++) {
            defs[num].element_id = ELEMENT_NS_TOUCHPAD_BASE + f;
            defs[num].name       = g_strdup_printf("Touchpad Finger %d", f + 1);
            num++;
        }
    }

    ConttestInputLayout *layout = g_new0(ConttestInputLayout, 1);
    layout->elements     = defs;
    layout->num_elements = num;
    layout->ctx          = ctx;

    /* Always called from the main thread; g_main_context_invoke executes
     * handle_layout synchronously, so the ref is acquired and released inline. */
    g_atomic_int_inc(&ctx->ref_count);
    g_main_context_invoke(NULL, conttest_input_viewer_handle_layout, layout);

    /* Start the main-thread polling timer at ~1000 Hz (1 ms) for low-latency
     * input visualization. Average added latency: ~0.5 ms. The GDestroyNotify
     * holds one ctx ref for the lifetime of the timer source. */
    g_atomic_int_inc(&ctx->ref_count);
    data->timer_id = g_timeout_add_full(G_PRIORITY_DEFAULT, 1,
                                        sdl3_poll_tick, data, sdl3_tick_destroyed);

    return session;
}
