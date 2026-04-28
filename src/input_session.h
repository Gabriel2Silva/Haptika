#pragma once
#include <glib.h>

// Opaque type so input viewer can track state
typedef struct _ConttestInputViewer ConttestInputViewer;

typedef struct {
  ConttestInputViewer *viewer;
  gint ref_count; // atomic
  gint active;    // atomic boolean
} ViewerSessionContext;

G_BEGIN_DECLS

// Used for passing input events from polling threads to the main thread
typedef enum {
  INPUT_EVENT_BUTTON,
  INPUT_EVENT_AXIS,
  INPUT_EVENT_TOUCHPAD,
  INPUT_EVENT_SENSOR
} ConttestInputEventType;

/*
 * Element ID namespaces — shared between backends and input_viewer.
 * Each namespace is a contiguous range; backends assign IDs within their
 * range and input_viewer dispatches based on them.
 */
#define ELEMENT_NS_SDL_BUTTON_BASE      0    /* SDL button index (0–31) */
#define ELEMENT_NS_SDL_AXIS_BASE      100    /* 100 + SDL_GamepadAxis index */
#define ELEMENT_NS_SDL_EXTRA_BASE     200    /* 200 + raw joystick axis offset (e.g. DS3 pressure) */
#define ELEMENT_NS_TOUCHPAD_BASE      300    /* 300 + finger index */
#define ELEMENT_NS_TOUCHPAD_SENTINEL  399    /* sentinel: creates the touchpad visualizer widget */
#define ELEMENT_NS_ACCEL              400    /* accelerometer sensor (3-axis) */
#define ELEMENT_NS_GYRO               401    /* gyroscope sensor (3-axis) */
#define ELEMENT_NS_EVDEV_ABS_BASE    1000    /* 1000 + ABS_* evdev code */

typedef struct {
  ViewerSessionContext *ctx;
  ConttestInputEventType type;
  int element_id; // E.g. evdev code or SDL button/axis index
  union {
    gboolean button_pressed;
    double axis_value;
  };
  float touch_x;
  float touch_y;
  /* Sensor data (type == INPUT_EVENT_SENSOR) */
  float sensor[3]; /* accel: m/s², gyro: rad/s */
} ConttestInputEvent;

/* A batch of events sharing one ViewerSessionContext reference.
 * Used by the evdev backend to reduce g_idle_add overhead: one idle per
 * poll() cycle instead of one idle per event. */
typedef struct {
  ViewerSessionContext *ctx; /* owns one ref */
  GArray *events;            /* GArray<ConttestInputEvent>, by value */
} ConttestInputEventBatch;

// Called by the polling thread to initialize the UI structure *once* at startup
// This must be done via g_main_context_invoke or similar, before sending
// updates
typedef struct {
  int element_id;
  char *name;
  // axis specific
  gboolean is_axis;
  gboolean is_trigger; // true = 0 to 1, false = -1 to 1
  gboolean is_stick_x;
  gboolean is_stick_y;
  int stick_id; // For pairing X and Y (e.g. left stick = 0, right stick = 1)
} ConttestInputElementDef;

typedef struct {
  ConttestInputElementDef *elements;
  int num_elements;
  ViewerSessionContext *ctx;
} ConttestInputLayout;

// Generic interface for stopping a session
struct _ConttestInputSession {
  GThread *thread;
  gint atomic_stop_flag;
  gpointer impl_data;
  void (*free_func)(struct _ConttestInputSession *);
};
typedef struct _ConttestInputSession ConttestInputSession;

void conttest_input_session_stop(ConttestInputSession *session);

typedef enum { CONTTEST_BACKEND_SDL3, CONTTEST_BACKEND_EVDEV } ConttestBackend;

// Factories
// Note: caller owns the returned ConttestInputSession and must free it via
// conttest_input_session_stop()
ConttestInputSession *
conttest_input_session_sdl3_start(const char *id_str,
                                  ViewerSessionContext *ctx);
ConttestInputSession *
conttest_input_session_evdev_start(const char *syspath,
                                   ViewerSessionContext *ctx);

G_END_DECLS
