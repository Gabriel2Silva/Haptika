#pragma once
#include <glib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Opaque types for viewer session coordination */
typedef struct _ConttestInputViewer ConttestInputViewer;
typedef struct _ViewerSessionContext ViewerSessionContext;

/* Used for passing input events from polling backends to the main thread */
typedef enum {
  INPUT_EVENT_BUTTON,
  INPUT_EVENT_AXIS,
  INPUT_EVENT_TOUCHPAD,
  INPUT_EVENT_SENSOR
} ConttestInputEventType;

/* Explicit element domains: avoid overlapping numeric ranges across backends. */
typedef enum {
  CONTTEST_ELEMENT_BUTTON = 0,      /* SDL_GamepadButton or evdev KEY/BTN code */
  CONTTEST_ELEMENT_SDL_AXIS,        /* SDL_GamepadAxis */
  CONTTEST_ELEMENT_SDL_EXTRA,       /* extra raw joystick axis index (0..N-1) */
  CONTTEST_ELEMENT_EVDEV_ABS,       /* evdev ABS_* code */
  CONTTEST_ELEMENT_TOUCHPAD_PAD,    /* touchpad surface widget */
  CONTTEST_ELEMENT_TOUCHPAD_FINGER, /* touchpad finger index (0..N-1) */
  CONTTEST_ELEMENT_SENSOR,          /* ConttestSensorKind */
} ConttestElementDomain;

typedef enum {
  CONTTEST_SENSOR_ACCEL = 0,
  CONTTEST_SENSOR_GYRO  = 1,
} ConttestSensorKind;

typedef struct {
  ConttestInputEventType type;
  ConttestElementDomain domain;
  int element_id; /* Index or code within `domain` */
  union {
    gboolean button_pressed;
    double axis_value;
  };
  float touch_x;
  float touch_y;
  /* Sensor data (type == INPUT_EVENT_SENSOR) */
  float sensor[3]; /* accel: m/s^2, gyro: rad/s */
} ConttestInputEvent;

/* Fixed lock-free SPSC ring buffer (producer: evdev thread, consumer: main loop). */
#define CONTTEST_EVENT_RING_CAP 2048

typedef struct {
  ConttestInputEvent events[CONTTEST_EVENT_RING_CAP];
  atomic_uint        head;
  uint8_t            pad_head[64];
  atomic_uint        tail;
  uint8_t            pad_tail[64];
  atomic_bool        overflow;
  atomic_bool        scheduled;
} ConttestEventRing;

struct _ViewerSessionContext {
  ConttestInputViewer *viewer;
  gint ref_count; /* atomic */
  gint active;    /* atomic boolean */
  ConttestEventRing ring;
};

gboolean conttest_event_ring_push(ConttestEventRing *ring, const ConttestInputEvent *ev);
gboolean conttest_event_ring_pop(ConttestEventRing *ring, ConttestInputEvent *out);
void     conttest_event_ring_notify(ViewerSessionContext *ctx, GSourceFunc dispatch_func);

ViewerSessionContext *viewer_session_context_new(ConttestInputViewer *viewer);
ViewerSessionContext *viewer_session_context_ref(ViewerSessionContext *ctx);
void                  viewer_session_context_unref(ViewerSessionContext *ctx);

/* POD description of a Linux ABS_* axis for pure, testable role classification. */
typedef struct {
  bool    present;
  int32_t minimum;
  int32_t maximum;
  int32_t value;
  int32_t flat;
} ConttestAbsAxisInfo;

void conttest_evdev_classify_axes(ConttestAbsAxisInfo z,
                                  ConttestAbsAxisInfo rz,
                                  ConttestAbsAxisInfo rx,
                                  ConttestAbsAxisInfo ry,
                                  gboolean *out_z_rz_right_stick,
                                  gboolean *out_rx_ry_triggers);

// Passed to conttest_input_viewer_handle_layout to initialize the UI structure
// once at session startup (via g_main_context_invoke when called off-thread,
// or synchronously when started on the GTK main thread).
typedef struct {
  ConttestElementDomain domain;
  int element_id;
  char *name;
  /* axis specific */
  gboolean is_axis;
  gboolean is_trigger; /* TRUE = 0 to 1 (unipolar), FALSE = -1 to 1 */
  gboolean is_bipolar; /* TRUE = centered zero-baseline bar ([-1, 1]) */
  gboolean is_stick_x;
  gboolean is_stick_y;
  int stick_id; /* For pairing X and Y (e.g. left stick = 0, right stick = 1) */
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

/* Factories: caller owns returned session and frees it via conttest_input_session_stop(). */
ConttestInputSession *
conttest_input_session_sdl3_start(uint32_t sdl_id,
                                  ViewerSessionContext *ctx);
ConttestInputSession *
conttest_input_session_evdev_start(const char *devnode,
                                   ViewerSessionContext *ctx);

G_END_DECLS
