#include "device_manager.h"
#include "device_matching.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <libudev.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDL_POLL_INTERVAL_MS 50
#define SDL_PUMP_COALESCE_US (20 * G_TIME_SPAN_MILLISECOND)
#define BATTERY_POLL_INTERVAL_SEC 5

/* Main-thread monotonic timestamp of the most recent SDL_PumpEvents() call,
 * used to coalesce back-to-back pumps between the 50 ms poll timer and the
 * viewer's GdkFrameClock tick callback. */
static gint64 last_sdl_pump_us = 0;

void conttest_sdl_pump_events(void) {
  SDL_PumpEvents();
  last_sdl_pump_us = g_get_monotonic_time();
}

typedef enum {
  DS3_BATTERY_UNPROBED = 0,
  DS3_BATTERY_PROBING,
  DS3_BATTERY_PROBED,
} Ds3BatteryState;

/* Cached udev metadata extracted on worker threads so main-loop matching,
 * connection-state resolution, and InputPlumber visibility checks never perform
 * synchronous sysfs/udev I/O. */
typedef struct {
  char *own_syspath;
  GStrv hw_keys;
  SDL_JoystickConnectionState udev_conn;
  gboolean is_virtual;
  char *virtual_bus;
  char *virtual_uniq;
  gboolean ip_hidden_devlink;
  gboolean udev_dead;
} UdevAttachMeta;

static void udev_attach_meta_clear(UdevAttachMeta *m) {
  g_clear_pointer(&m->own_syspath, g_free);
  g_clear_pointer(&m->hw_keys, g_strfreev);
  m->udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  m->is_virtual = FALSE;
  g_clear_pointer(&m->virtual_bus, g_free);
  g_clear_pointer(&m->virtual_uniq, g_free);
  m->ip_hidden_devlink = FALSE;
  m->udev_dead = FALSE;
}

static void udev_attach_meta_move(UdevAttachMeta *dst, UdevAttachMeta *src) {
  udev_attach_meta_clear(dst);
  *dst = *src;
  memset(src, 0, sizeof(*src));
  src->udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
}

/* ConttestDevice definition is private to this file. */
struct _ConttestDevice {
  GObject parent_instance;

  char *name;
  gboolean has_sdl;
  SDL_JoystickID sdl_id;
  char *sdl_path;
  SDL_Gamepad *sdl_gamepad; /* kept open while attached, for battery polling */
  UdevAttachMeta sdl_meta;
  gboolean sdl_probe_pending;
  guint sdl_udev_seq;

  gboolean has_evdev;
  char *evdev_syspath;
  char *evdev_devnode;
  UdevAttachMeta evdev_meta;
  guint evdev_udev_seq;

  /* Hardware identity for narrow fallbacks (e.g. DS3 battery). */

  uint16_t vendor_id;
  uint16_t product_id;
  char *battery_syspath; /* Resolved power_supply node */
  Ds3BatteryState ds3_battery_state;
  gboolean ds3_battery_rescan_pending;
  gboolean ds3_battery_in_flight;
  guint ds3_battery_probe_seq;

  /* Battery state — only meaningful when has_sdl is TRUE. */
  SDL_PowerState battery_state;
  int battery_percent; /* 0–100, or -1 if unknown */
  SDL_JoystickConnectionState connection_state;

  /* Virtual / InputPlumber state */
  ConttestVirtualKind virtual_kind;
  char *virtual_bus;
  char *virtual_uniq;
  ConttestInputPlumberInfo *ip_info;
  gboolean hidden;
};

G_DEFINE_TYPE(ConttestDevice, conttest_device, G_TYPE_OBJECT)

/* Read-only accessors */

const char *conttest_device_get_name(const ConttestDevice *device) {
  return device->name;
}

gboolean conttest_device_get_has_sdl(const ConttestDevice *device) {
  return device->has_sdl;
}

SDL_JoystickID conttest_device_get_sdl_id(const ConttestDevice *device) {
  return device->sdl_id;
}

gboolean conttest_device_get_has_evdev(const ConttestDevice *device) {
  return device->has_evdev;
}

const char *conttest_device_get_evdev_syspath(const ConttestDevice *device) {
  return device->evdev_syspath;
}

const char *conttest_device_get_evdev_devnode(const ConttestDevice *device) {
  return device->evdev_devnode;
}

SDL_PowerState conttest_device_get_battery_state(const ConttestDevice *device) {
  return device->battery_state;
}

int conttest_device_get_battery_percent(const ConttestDevice *device) {
  return device->battery_percent;
}

SDL_JoystickConnectionState
conttest_device_get_connection_state(const ConttestDevice *device) {
  return device->connection_state;
}

uint16_t conttest_device_get_vendor_id(const ConttestDevice *device) {
  return device->vendor_id;
}

uint16_t conttest_device_get_product_id(const ConttestDevice *device) {
  return device->product_id;
}

ConttestVirtualKind conttest_device_get_virtual_kind(const ConttestDevice *device) {
  return device->virtual_kind;
}

gboolean conttest_device_is_virtual(const ConttestDevice *device) {
  return device->virtual_kind != CONTTEST_VIRTUAL_NONE;
}

const ConttestInputPlumberInfo *
conttest_device_get_inputplumber_info(const ConttestDevice *device) {
  return device->ip_info;
}

struct _ConttestDeviceManager {
  GObject parent_instance;
  GPtrArray *devices; /* Array of ConttestDevice* */

  GCancellable *cancellable;
  struct udev *udev;
  struct udev_monitor *udev_mon;
  guint udev_watch_id;

  guint sdl_timer_id;
  guint battery_timer_id;
  guint sdl_sync_idle_id;

  gboolean evdev_scan_in_flight;
  gboolean evdev_rescan_pending;
  gboolean udev_refresh_in_flight;
  gboolean udev_refresh_pending;

  GHashTable *evdev_seq_by_syspath; /* owned char* -> GUINT_TO_POINTER(seq) */
  guint evdev_seq;
  guint evdev_probes_in_flight;
  guint evdev_remove_seq;

  gboolean disposed;

  ConttestInputPlumberMonitor *ip_monitor;
};

static GWeakRef *manager_weak_ref_new(ConttestDeviceManager *self) {
  GWeakRef *wr = g_new0(GWeakRef, 1);
  g_weak_ref_init(wr, self);
  return wr;
}

static void manager_weak_ref_free(gpointer data) {
  GWeakRef *wr = data;
  g_weak_ref_clear(wr);
  g_free(wr);
}

enum {
  SIGNAL_DEVICE_ADDED,
  SIGNAL_DEVICE_REMOVED,
  SIGNAL_DEVICE_UPDATED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE(ConttestDeviceManager, conttest_device_manager, G_TYPE_OBJECT)

static void conttest_device_finalize(GObject *object) {
  ConttestDevice *device = CONTTEST_DEVICE(object);
  if (device->sdl_gamepad) {
    SDL_CloseGamepad(device->sdl_gamepad);
    device->sdl_gamepad = NULL;
  }
  udev_attach_meta_clear(&device->sdl_meta);
  udev_attach_meta_clear(&device->evdev_meta);
  g_free(device->name);
  g_free(device->sdl_path);
  g_free(device->evdev_syspath);
  g_free(device->evdev_devnode);
  g_free(device->battery_syspath);
  g_free(device->virtual_bus);
  g_free(device->virtual_uniq);
  conttest_inputplumber_info_free(device->ip_info);
  G_OBJECT_CLASS(conttest_device_parent_class)->finalize(object);
}

static void conttest_device_class_init(ConttestDeviceClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = conttest_device_finalize;
}

static void conttest_device_init(ConttestDevice *dev) {
  dev->battery_state = SDL_POWERSTATE_UNKNOWN;
  dev->battery_percent = -1;
  dev->connection_state = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  dev->sdl_meta.udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  dev->evdev_meta.udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
}

static ConttestDevice *conttest_device_new(const char *name) {
  ConttestDevice *dev = g_object_new(CONTTEST_TYPE_DEVICE, NULL);
  dev->name = g_strdup(name);
  return dev;
}

static void device_reset_ds3_battery(ConttestDevice *d) {
  g_clear_pointer(&d->battery_syspath, g_free);
  d->ds3_battery_state = DS3_BATTERY_UNPROBED;
  d->ds3_battery_rescan_pending = FALSE;
  d->ds3_battery_probe_seq++;
}

/* Device mutation helpers */

static void device_set_name(ConttestDevice *d, const char *name) {
  if (g_strcmp0(d->name, name) != 0 &&
      g_strcmp0(name, "Unknown Gamepad") != 0 &&
      g_strcmp0(name, "Unknown evdev") != 0) {
    g_free(d->name);
    d->name = g_strdup(name);
  }
}

static void device_attach_sdl(ConttestDevice *d, SDL_JoystickID id,
                              const char *sdl_path) {
  d->has_sdl = TRUE;
  d->sdl_id = id;
  if (g_strcmp0(d->sdl_path, sdl_path) != 0) {
    g_free(d->sdl_path);
    d->sdl_path = g_strdup(sdl_path);
    udev_attach_meta_clear(&d->sdl_meta);
    d->sdl_udev_seq++;
  }
  /* Keep a handle open so battery polling has warm driver data. */
  if (!d->sdl_gamepad) {
    d->sdl_gamepad = SDL_OpenGamepad(id);
  }
}

static void device_detach_sdl(ConttestDevice *d) {
  d->has_sdl = FALSE;
  d->sdl_id = 0;
  g_free(d->sdl_path);
  d->sdl_path = NULL;
  if (d->sdl_gamepad) {
    SDL_CloseGamepad(d->sdl_gamepad);
    d->sdl_gamepad = NULL;
  }
  udev_attach_meta_clear(&d->sdl_meta);
  d->sdl_probe_pending = FALSE;
  d->sdl_udev_seq++;
  d->battery_state = SDL_POWERSTATE_UNKNOWN;
  d->battery_percent = -1;
  d->connection_state = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  if (!d->has_evdev) {
    d->vendor_id = 0;
    d->product_id = 0;
  }
}

static void device_attach_evdev(ConttestDevice *d, const char *evdev_syspath,
                                const char *evdev_devnode) {
  d->has_evdev = TRUE;
  if (evdev_syspath && g_strcmp0(d->evdev_syspath, evdev_syspath) != 0) {
    g_free(d->evdev_syspath);
    d->evdev_syspath = g_strdup(evdev_syspath);
    udev_attach_meta_clear(&d->evdev_meta);
    d->evdev_udev_seq++;
    device_reset_ds3_battery(d);
  }
  if (evdev_devnode && g_strcmp0(d->evdev_devnode, evdev_devnode) != 0) {
    g_free(d->evdev_devnode);
    d->evdev_devnode = g_strdup(evdev_devnode);
  }
}

static void device_detach_evdev(ConttestDevice *d) {
  d->has_evdev = FALSE;
  g_free(d->evdev_syspath);
  d->evdev_syspath = NULL;
  g_free(d->evdev_devnode);
  d->evdev_devnode = NULL;
  udev_attach_meta_clear(&d->evdev_meta);
  d->evdev_udev_seq++;
  device_reset_ds3_battery(d);
  /* Clear hardware identity only if SDL is also gone, so that predicates
   * like conttest_device_is_ds3() remain valid during the window between
   * udev remove and SDL remove. */
  if (!d->has_sdl) {
    d->vendor_id = 0;
    d->product_id = 0;
  }
}

/* ── Public device-type predicates (defined in device_manager.h) ─────── */

gboolean conttest_device_is_ds3(const ConttestDevice *device) {
    return conttest_vid_pid_is_ds3(device->vendor_id, device->product_id);
}

gboolean conttest_device_is_ds4(const ConttestDevice *device) {
    return conttest_vid_pid_is_ds4(device->vendor_id, device->product_id);
}

gboolean conttest_device_is_ds5(const ConttestDevice *device) {
    return conttest_vid_pid_is_ds5(device->vendor_id, device->product_id);
}

/* Runs on a GTask worker thread using a thread-local libudev handle. */
static char *probe_ds3_battery_syspath(struct udev *udev,
                                       const char *evdev_syspath) {
  if (!udev || !evdev_syspath)
    return NULL;

  struct udev_device *udev_dev =
      udev_device_new_from_syspath(udev, evdev_syspath);
  if (!udev_dev)
    return NULL;

  /* Walk upward to find the hid parent. */
  struct udev_device *hid_parent =
      udev_device_get_parent_with_subsystem_devtype(udev_dev, "hid", NULL);
  if (!hid_parent) {
    udev_device_unref(udev_dev);
    return NULL;
  }

  const char *hid_path = udev_device_get_syspath(hid_parent);
  char *resolved_path = NULL;
  int matches = 0;

  /* Enumerate power_supply devices. */
  struct udev_enumerate *en = udev_enumerate_new(udev);
  if (!en) {
    udev_device_unref(udev_dev);
    return NULL;
  }

  udev_enumerate_add_match_subsystem(en, "power_supply");
  udev_enumerate_scan_devices(en);

  struct udev_list_entry *l, *entries = udev_enumerate_get_list_entry(en);
  udev_list_entry_foreach(l, entries) {
    const char *name = udev_list_entry_get_name(l);
    struct udev_device *bat_dev = udev_device_new_from_syspath(udev, name);
    if (bat_dev) {
      struct udev_device *bat_hid =
          udev_device_get_parent_with_subsystem_devtype(bat_dev, "hid", NULL);
      if (bat_hid &&
          g_strcmp0(udev_device_get_syspath(bat_hid), hid_path) == 0) {
        matches++;
        if (matches == 1) {
          resolved_path = g_strdup(name);
        }
      }
      udev_device_unref(bat_dev);
    }
  }

  udev_enumerate_unref(en);
  udev_device_unref(udev_dev);

  /* Enforce uniqueness. */
  if (matches != 1) {
    g_free(resolved_path);
    return NULL;
  }

  return resolved_path;
}

static gboolean poll_ds3_battery_sysfs(const char *syspath,
                                       SDL_PowerState *state,
                                       int *percent) {
  *state = SDL_POWERSTATE_UNKNOWN;
  *percent = -1;

  if (!syspath || access(syspath, F_OK) != 0)
    return FALSE;

  char buf[64];
  FILE *f;

  /* Read capacity */
  g_autofree char *cap_path = g_build_filename(syspath, "capacity", NULL);
  f = fopen(cap_path, "r");
  if (f) {
    if (fgets(buf, sizeof(buf), f)) {
      char *end;
      long val = strtol(buf, &end, 10);
      if (end != buf && val >= 0 && val <= 100) {
        *percent = (int)val;
      }
    }
    fclose(f);
  }

  /* Read status */
  g_autofree char *status_path = g_build_filename(syspath, "status", NULL);
  f = fopen(status_path, "r");
  if (f) {
    if (fgets(buf, sizeof(buf), f)) {
      g_strstrip(buf);
      if (g_strcmp0(buf, "Charging") == 0) {
        *state = SDL_POWERSTATE_CHARGING;
        *percent = -1; /* Suppress unreliable DS3 charging percentage */
      } else if (g_strcmp0(buf, "Full") == 0) {
        *state = SDL_POWERSTATE_CHARGED;
        *percent = 100;
      } else if (g_strcmp0(buf, "Discharging") == 0 ||
                 g_strcmp0(buf, "Not charging") == 0) {
        *state = SDL_POWERSTATE_ON_BATTERY;
      }
    }
    fclose(f);
  }
  return TRUE;
}

static ConttestDevice *find_device_by_sdl_id(ConttestDeviceManager *self,
                                             SDL_JoystickID id) {
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (d->has_sdl && d->sdl_id == id) {
      return d;
    }
  }
  return NULL;
}

static ConttestDevice *find_device_by_sdl_path(ConttestDeviceManager *self,
                                               const char *sdl_path,
                                               uint16_t vendor_id,
                                               uint16_t product_id,
                                               const char *name) {
  /* Never coalesce empty paths or shared multi-port nodes (e.g. /dev/hidraw*
   * or libusb paths used by multi-controller adapters). Only dedicated evdev
   * device nodes map 1:1 to a controller, and only match identical identity. */
  if (!sdl_path || !*sdl_path ||
      !g_str_has_prefix(sdl_path, "/dev/input/event"))
    return NULL;
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (d->has_sdl && d->sdl_path && g_strcmp0(d->sdl_path, sdl_path) == 0 &&
        d->vendor_id == vendor_id && d->product_id == product_id &&
        g_strcmp0(d->name, name) == 0)
      return d;
  }
  return NULL;
}

static ConttestDevice *find_device_by_evdev_syspath(ConttestDeviceManager *self,
                                                    const char *syspath) {
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (d->has_evdev && d->evdev_syspath &&
        g_strcmp0(d->evdev_syspath, syspath) == 0) {
      return d;
    }
  }
  return NULL;
}

static ConttestDevice *find_device_by_hardware_for_sdl(
    ConttestDeviceManager *self,
    const char *sdl_path,
    const char *sdl_own_syspath,
    const char *const *sdl_hw_keys,
    uint16_t sdl_vid,
    uint16_t sdl_pid) {
  /* Fast path in memory: a /dev/input/event* devnode maps 1:1 to a tracked
   * evdev device. */
  if (sdl_path && *sdl_path) {
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_evdev && !d->has_sdl && d->evdev_devnode &&
          g_strcmp0(d->evdev_devnode, sdl_path) == 0) {
        return d;
      }
    }
  }

  /* Topological match in memory via cached udev hardware ancestor keys. */
  if (sdl_own_syspath || sdl_hw_keys) {
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_evdev && !d->has_sdl &&
          conttest_hw_keys_match(sdl_own_syspath, sdl_hw_keys,
                                 d->evdev_meta.own_syspath,
                                 (const char *const *)d->evdev_meta.hw_keys)) {
        return d;
      }
    }
  }

  /* Second pass: symmetric VID/PID fallback for SDL devices without a /dev/
   * devnode path (e.g. HIDAPI DS3 when evdev attached before SDL). Only merge
   * with a single unambiguous physical evdev candidate so identical pads or
   * virtual controllers are never cross-merged. */
  if ((!sdl_path || !g_str_has_prefix(sdl_path, "/dev/")) &&
      sdl_vid != 0 && sdl_pid != 0) {
    ConttestDevice *match = NULL;
    guint match_count = 0;
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_evdev && !d->has_sdl && !d->evdev_meta.is_virtual &&
          d->virtual_kind == CONTTEST_VIRTUAL_NONE &&
          d->vendor_id == sdl_vid && d->product_id == sdl_pid) {
        match = d;
        match_count++;
      }
    }
    if (match_count == 1)
      return match;
  }

  return NULL;
}

static ConttestDevice *find_device_by_hardware_for_evdev(
    ConttestDeviceManager *self,
    const char *evdev_devnode,
    const char *evdev_own_syspath,
    const char *const *evdev_hw_keys,
    uint16_t udev_vid,
    uint16_t udev_pid,
    gboolean udev_is_virtual) {
  /* Fast path in memory: SDL devnode matches this evdev devnode directly. */
  if (evdev_devnode) {
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_sdl && !d->has_evdev && d->sdl_path &&
          g_strcmp0(d->sdl_path, evdev_devnode) == 0) {
        return d;
      }
    }
  }

  /* First pass: topological match in memory via cached udev ancestor keys. */
  if (evdev_own_syspath || evdev_hw_keys) {
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_sdl && !d->has_evdev &&
          conttest_hw_keys_match(d->sdl_meta.own_syspath,
                                 (const char *const *)d->sdl_meta.hw_keys,
                                 evdev_own_syspath, evdev_hw_keys)) {
        return d;
      }
    }
  }

  /* Second pass: for SDL devices without a devnode path (e.g. HIDAPI-backed
   * controllers like DS3), fall back to VID/PID matching only for a single
   * unambiguous non-virtual candidate. */
  if (udev_vid != 0 && udev_pid != 0 && !udev_is_virtual) {
    ConttestDevice *match = NULL;
    guint match_count = 0;
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_sdl && !d->has_evdev && !d->sdl_meta.is_virtual &&
          d->virtual_kind == CONTTEST_VIRTUAL_NONE &&
          (!d->sdl_path || !g_str_has_prefix(d->sdl_path, "/dev/"))) {
        uint16_t sdl_vid = d->vendor_id ? d->vendor_id
                                        : SDL_GetGamepadVendorForID(d->sdl_id);
        uint16_t sdl_pid = d->product_id ? d->product_id
                                         : SDL_GetGamepadProductForID(d->sdl_id);
        if (sdl_vid == udev_vid && sdl_pid == udev_pid) {
          match = d;
          match_count++;
        }
      }
    }
    if (match_count == 1)
      return match;
  }

  return NULL;
}

static void refresh_and_emit_device(ConttestDeviceManager *self,
                                    ConttestDevice *d,
                                    gboolean force_update);

static void check_empty_and_remove(ConttestDeviceManager *self,
                                   ConttestDevice *d) {
  if (!d->has_sdl && !d->has_evdev) {
    g_autoptr(ConttestDevice) keep = g_object_ref(d);
    gboolean was_visible = !keep->hidden;
    g_ptr_array_remove(self->devices, keep);
    if (was_visible) {
      g_signal_emit(self, signals[SIGNAL_DEVICE_REMOVED], 0, keep);
    }
  } else {
    refresh_and_emit_device(self, d, TRUE);
  }
}

static SDL_JoystickConnectionState
device_cached_udev_connection_state(const ConttestDevice *d) {
  if (d->has_evdev &&
      d->evdev_meta.udev_conn != SDL_JOYSTICK_CONNECTION_UNKNOWN) {
    return d->evdev_meta.udev_conn;
  }
  if (d->has_sdl &&
      d->sdl_meta.udev_conn != SDL_JOYSTICK_CONNECTION_UNKNOWN) {
    return d->sdl_meta.udev_conn;
  }
  return SDL_JOYSTICK_CONNECTION_UNKNOWN;
}

/* Worker-thread helper: opens a udev_device for a /dev/... node off the main
 * loop. */
static struct udev_device *worker_udev_new_from_devnode(struct udev *u,
                                                       const char *devnode) {
  if (!u || !devnode || !g_str_has_prefix(devnode, "/dev/"))
    return NULL;
  struct stat st;
  if (stat(devnode, &st) != 0)
    return NULL;
  char type;
  if (S_ISCHR(st.st_mode)) {
    type = 'c';
  } else if (S_ISBLK(st.st_mode)) {
    type = 'b';
  } else {
    return NULL;
  }
  return udev_device_new_from_devnum(u, type, st.st_rdev);
}

/* Worker-thread helper: extracts all needed udev metadata from an already-opened
 * udev_device in a single pass. */
static void udev_extract_attach_meta(struct udev_device *dev,
                                     UdevAttachMeta *out) {
  udev_attach_meta_clear(out);
  if (!dev)
    return;
  out->hw_keys = conttest_udev_collect_hw_keys(dev, &out->own_syspath);
  out->udev_conn = conttest_resolve_connection_state_for_udev_device(dev);
  out->is_virtual = conttest_udev_device_probe_virtual(dev, &out->virtual_uniq,
                                                       &out->virtual_bus);
  out->ip_hidden_devlink = conttest_udev_device_is_inputplumber_hidden(dev);
  out->udev_dead = FALSE;
}

typedef struct {
  char *syspath;
  char *devnode;
  char *name;
  uint16_t vendor_id;
  uint16_t product_id;
  UdevAttachMeta meta;
} UdevDeviceMeta;

static void udev_device_meta_free(gpointer data) {
  UdevDeviceMeta *m = data;
  if (!m)
    return;
  g_free(m->syspath);
  g_free(m->devnode);
  g_free(m->name);
  udev_attach_meta_clear(&m->meta);
  g_free(m);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(UdevDeviceMeta, udev_device_meta_free)

/* Worker-thread helper: inspects an evdev udev_device and returns extracted
 * metadata if it is a /dev/input/event* joystick, or NULL otherwise. */
static UdevDeviceMeta *udev_device_meta_extract_evdev(struct udev_device *dev) {
  if (!dev)
    return NULL;
  const char *syspath = udev_device_get_syspath(dev);
  const char *devnode = udev_device_get_devnode(dev);
  if (!syspath || !devnode || strncmp(devnode, "/dev/input/event", 16) != 0)
    return NULL;

  const char *id_input_joystick =
      udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
  if (!id_input_joystick || strcmp(id_input_joystick, "1") != 0)
    return NULL;

  const char *name = NULL;
  struct udev_device *parent =
      udev_device_get_parent_with_subsystem_devtype(dev, "input", NULL);
  if (parent) {
    name = udev_device_get_sysattr_value(parent, "name");
  }
  if (!name) {
    name = udev_device_get_property_value(dev, "NAME");
  }
  if (!name)
    name = "Unknown evdev";

  char *clean_name = g_strdup(name);
  size_t len = strlen(clean_name);
  if (len >= 2 && clean_name[0] == '"' && clean_name[len - 1] == '"') {
    clean_name[len - 1] = '\0';
    memmove(clean_name, clean_name + 1, len - 1);
  }

  UdevDeviceMeta *m = g_new0(UdevDeviceMeta, 1);
  m->syspath = g_strdup(syspath);
  m->devnode = g_strdup(devnode);
  m->name = clean_name;
  conttest_extract_udev_vid_pid(dev, &m->vendor_id, &m->product_id);
  udev_extract_attach_meta(dev, &m->meta);
  return m;
}

typedef struct {
  GWeakRef manager;
  GWeakRef device;
  char *arg;
  char *arg2;
  guint seq;
} UdevTaskCtx;

static UdevTaskCtx *udev_task_ctx_new_for_device(ConttestDeviceManager *self,
                                                 ConttestDevice *device,
                                                 const char *arg,
                                                 const char *arg2,
                                                 guint seq) {
  UdevTaskCtx *ctx = g_new0(UdevTaskCtx, 1);
  g_weak_ref_init(&ctx->manager, self);
  g_weak_ref_init(&ctx->device, device);
  ctx->arg = g_strdup(arg);
  ctx->arg2 = g_strdup(arg2);
  ctx->seq = seq;
  return ctx;
}

static void udev_task_ctx_free(gpointer data) {
  UdevTaskCtx *ctx = data;
  if (!ctx)
    return;
  g_weak_ref_clear(&ctx->manager);
  g_weak_ref_clear(&ctx->device);
  g_free(ctx->arg);
  g_free(ctx->arg2);
  g_free(ctx);
}

typedef struct {
  char *resolved_syspath;
  gboolean syspath_alive;
  SDL_PowerState state;
  int percent;
} Ds3BatteryResult;

static void ds3_battery_result_free(gpointer data) {
  Ds3BatteryResult *r = data;
  if (!r)
    return;
  g_free(r->resolved_syspath);
  g_free(r);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Ds3BatteryResult, ds3_battery_result_free)

static void ds3_battery_task_thread(GTask *task, gpointer source_object,
                                    gpointer task_data,
                                    GCancellable *cancellable) {
  (void)source_object;
  UdevTaskCtx *ctx = task_data;
  Ds3BatteryResult *res = g_new0(Ds3BatteryResult, 1);
  res->state = SDL_POWERSTATE_UNKNOWN;
  res->percent = -1;

  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_pointer(task, res, ds3_battery_result_free);
    return;
  }

  /* arg = evdev_syspath, arg2 = cached battery_syspath (may be NULL). */
  char *bat_path = g_strdup(ctx->arg2);
  if (!bat_path && ctx->arg) {
    struct udev *u = udev_new();
    if (u) {
      bat_path = probe_ds3_battery_syspath(u, ctx->arg);
      udev_unref(u);
    }
  }

  if (bat_path && !g_cancellable_is_cancelled(cancellable)) {
    if (poll_ds3_battery_sysfs(bat_path, &res->state, &res->percent)) {
      res->resolved_syspath = bat_path;
      res->syspath_alive = TRUE;
      g_task_return_pointer(task, res, ds3_battery_result_free);
      return;
    }
    /* Cached battery_syspath vanished; retry resolving once from evdev. */
    g_clear_pointer(&bat_path, g_free);
    if (ctx->arg2 && ctx->arg) {
      struct udev *u = udev_new();
      if (u) {
        bat_path = probe_ds3_battery_syspath(u, ctx->arg);
        udev_unref(u);
      }
      if (bat_path && !g_cancellable_is_cancelled(cancellable) &&
          poll_ds3_battery_sysfs(bat_path, &res->state, &res->percent)) {
        res->resolved_syspath = bat_path;
        res->syspath_alive = TRUE;
        g_task_return_pointer(task, res, ds3_battery_result_free);
        return;
      }
      g_clear_pointer(&bat_path, g_free);
    }
  }

  g_free(bat_path);
  g_task_return_pointer(task, res, ds3_battery_result_free);
}

static void schedule_ds3_battery_task(ConttestDeviceManager *self,
                                      ConttestDevice *d,
                                      gboolean force_probe);

static void on_ds3_battery_task_done(GObject *source, GAsyncResult *res,
                                     gpointer user_data) {
  (void)source;
  (void)user_data;
  GTask *task = G_TASK(res);
  UdevTaskCtx *ctx = g_task_get_task_data(task);
  GError *err = NULL;
  g_autoptr(Ds3BatteryResult) bat = NULL;
  Ds3BatteryResult *raw_bat = g_task_propagate_pointer(task, &err);
  g_clear_error(&err);
  bat = raw_bat;

  g_autoptr(ConttestDevice) d = g_weak_ref_get(&ctx->device);
  if (!d)
    return;

  d->ds3_battery_in_flight = FALSE;

  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(&ctx->manager);
  if (!self || self->disposed || !self->devices || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable) ||
      !g_ptr_array_find(self->devices, d, NULL) || !d->has_evdev ||
      g_strcmp0(d->evdev_syspath, ctx->arg) != 0) {
    return;
  }

  if (d->ds3_battery_probe_seq != ctx->seq) {
    if (d->ds3_battery_rescan_pending && d->has_sdl &&
        conttest_device_is_ds3(d)) {
      d->ds3_battery_rescan_pending = FALSE;
      schedule_ds3_battery_task(self, d, TRUE);
    }
    return;
  }

  gboolean rescan = d->ds3_battery_rescan_pending;
  d->ds3_battery_rescan_pending = FALSE;
  d->ds3_battery_state = DS3_BATTERY_PROBED;

  if (!d->has_sdl || !conttest_device_is_ds3(d))
    return;

  gboolean changed = FALSE;
  if (bat && bat->syspath_alive && bat->resolved_syspath) {
    if (g_strcmp0(d->battery_syspath, bat->resolved_syspath) != 0) {
      g_free(d->battery_syspath);
      d->battery_syspath = g_strdup(bat->resolved_syspath);
    }
    if (bat->state != d->battery_state || bat->percent != d->battery_percent) {
      d->battery_state = bat->state;
      d->battery_percent = bat->percent;
      changed = TRUE;
    }
  } else {
    g_clear_pointer(&d->battery_syspath, g_free);
    if (d->battery_state != SDL_POWERSTATE_UNKNOWN || d->battery_percent != -1) {
      d->battery_state = SDL_POWERSTATE_UNKNOWN;
      d->battery_percent = -1;
      changed = TRUE;
    }
  }

  if (changed && !d->hidden) {
    g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
  }

  if (rescan && !d->battery_syspath && !self->disposed &&
      g_ptr_array_find(self->devices, d, NULL)) {
    d->ds3_battery_state = DS3_BATTERY_UNPROBED;
    schedule_ds3_battery_task(self, d, TRUE);
  }
}

static void schedule_ds3_battery_task(ConttestDeviceManager *self,
                                      ConttestDevice *d,
                                      gboolean force_probe) {
  if (self->disposed || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable) || !d->has_evdev ||
      !d->evdev_syspath || !d->has_sdl)
    return;

  if (!force_probe && !d->battery_syspath &&
      d->ds3_battery_state == DS3_BATTERY_PROBED)
    return;

  if (d->ds3_battery_in_flight) {
    if (force_probe)
      d->ds3_battery_rescan_pending = TRUE;
    return;
  }

  if (!d->battery_syspath)
    d->ds3_battery_state = DS3_BATTERY_PROBING;
  d->ds3_battery_rescan_pending = FALSE;
  d->ds3_battery_in_flight = TRUE;
  guint seq = ++d->ds3_battery_probe_seq;

  GTask *task =
      g_task_new(NULL, self->cancellable, on_ds3_battery_task_done, NULL);
  g_task_set_task_data(
      task,
      udev_task_ctx_new_for_device(self, d, d->evdev_syspath, d->battery_syspath,
                                   seq),
      udev_task_ctx_free);
  g_task_run_in_thread(task, ds3_battery_task_thread);
  g_object_unref(task);
}

/* Poll battery/connection state for an SDL-attached device. Sysfs I/O for DS3
 * runs asynchronously off the main loop. Returns TRUE if cached state changed. */
static gboolean device_poll_battery(ConttestDeviceManager *self,
                                    ConttestDevice *d) {
  if (!d->has_sdl || !d->sdl_gamepad)
    return FALSE;

  gboolean changed = FALSE;

  if (conttest_device_is_ds3(d)) {
    /* Confirmed DS3 controllers use Linux power_supply exclusively, polled off
     * the main thread. Preserve current values until the worker completes. */
    schedule_ds3_battery_task(self, d, FALSE);
  } else {
    /* All other controllers use SDL. */
    int percent = -1;
    SDL_PowerState state = SDL_GetGamepadPowerInfo(d->sdl_gamepad, &percent);
    if (state != d->battery_state || percent != d->battery_percent) {
      d->battery_state = state;
      d->battery_percent = percent;
      changed = TRUE;
    }
  }

  SDL_JoystickConnectionState conn =
      SDL_GetGamepadConnectionState(d->sdl_gamepad);
  if (conn == SDL_JOYSTICK_CONNECTION_UNKNOWN) {
    conn = device_cached_udev_connection_state(d);
  }
  if (conn != d->connection_state) {
    d->connection_state = conn;
    changed = TRUE;
  }

  return changed;
}

static gboolean device_has_dead_udev(const ConttestDevice *d) {
  if (!d->has_evdev && d->has_sdl && d->sdl_meta.udev_dead)
    return TRUE;
  if (d->has_evdev && d->evdev_meta.udev_dead)
    return TRUE;
  return FALSE;
}

static gboolean device_refresh_virtual_state(ConttestDeviceManager *self,
                                             ConttestDevice *d) {
  /* If evdev has already vanished while waiting for SDL's removal event on
   * the same disappearing /dev/ node, keep virtual metadata intact so the
   * dying entry does not briefly flicker into a physical/tagless controller. */
  if (d->virtual_kind != CONTTEST_VIRTUAL_NONE &&
      device_has_dead_udev(d)) {
    return FALSE;
  }

  const UdevAttachMeta *src_meta = NULL;
  if (d->has_evdev && !d->evdev_meta.udev_dead && d->evdev_meta.own_syspath) {
    src_meta = &d->evdev_meta;
  } else if (d->has_sdl && !d->sdl_meta.udev_dead && d->sdl_meta.own_syspath) {
    src_meta = &d->sdl_meta;
  }

  gboolean is_virtual = src_meta ? src_meta->is_virtual : FALSE;
  const char *uniq = src_meta ? src_meta->virtual_uniq : NULL;
  const char *vbus = src_meta ? src_meta->virtual_bus : NULL;

  if (g_strcmp0(d->virtual_uniq, uniq) != 0) {
    g_free(d->virtual_uniq);
    d->virtual_uniq = g_strdup(uniq);
  }

  if (g_strcmp0(d->virtual_bus, vbus) != 0) {
    g_free(d->virtual_bus);
    d->virtual_bus = g_strdup(vbus);
  }

  ConttestVirtualKind new_kind = CONTTEST_VIRTUAL_NONE;
  ConttestInputPlumberInfo *new_info = NULL;

  if (is_virtual) {
    new_info = conttest_inputplumber_monitor_match(
        self->ip_monitor, d->name, d->vendor_id, d->product_id,
        d->virtual_uniq, d->virtual_bus);
    new_kind = new_info ? CONTTEST_VIRTUAL_INPUTPLUMBER
                        : CONTTEST_VIRTUAL_GENERIC;
  }

  gboolean changed = (new_kind != d->virtual_kind) ||
                     !conttest_inputplumber_info_equal(d->ip_info, new_info);

  d->virtual_kind = new_kind;
  conttest_inputplumber_info_free(d->ip_info);
  d->ip_info = new_info;

  return changed;
}

static gboolean compute_device_hidden(ConttestDeviceManager *self,
                                      const ConttestDevice *d) {
  /* Keep newly attached /dev/ SDL controllers hidden while their initial udev
   * probe runs off-thread so duplicates or InputPlumber sources never flash. */
  if (d->sdl_probe_pending)
    return TRUE;

  /* Never hide virtual devices — only physical source devices captured by
   * InputPlumber are hidden to prevent broken duplicate entries. */
  if (d->virtual_kind != CONTTEST_VIRTUAL_NONE)
    return FALSE;

  /* Do not unhide an already-hidden source device whose kernel node has just
   * been removed while its SDL removal event is still pending. */
  if (d->hidden && device_has_dead_udev(d))
    return TRUE;

  if (conttest_inputplumber_monitor_is_source_devnode(self->ip_monitor,
                                                    d->evdev_devnode))
    return TRUE;

  if (conttest_inputplumber_monitor_is_source_devnode(self->ip_monitor,
                                                    d->sdl_path))
    return TRUE;

  if (d->has_evdev && d->evdev_meta.ip_hidden_devlink)
    return TRUE;

  if (d->has_sdl && d->sdl_meta.ip_hidden_devlink)
    return TRUE;

  return FALSE;
}

static void device_emit_transition(ConttestDeviceManager *self,
                                   ConttestDevice *d,
                                   gboolean is_new,
                                   gboolean was_hidden,
                                   gboolean content_changed) {
  if (is_new) {
    if (!d->hidden) {
      g_signal_emit(self, signals[SIGNAL_DEVICE_ADDED], 0, d);
    }
    return;
  }

  if (was_hidden && !d->hidden) {
    g_signal_emit(self, signals[SIGNAL_DEVICE_ADDED], 0, d);
  } else if (!was_hidden && d->hidden) {
    g_signal_emit(self, signals[SIGNAL_DEVICE_REMOVED], 0, d);
  } else if (!d->hidden && content_changed) {
    g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
  }
}

static void refresh_and_emit_device(ConttestDeviceManager *self,
                                    ConttestDevice *d,
                                    gboolean force_update) {
  gboolean was_hidden = d->hidden;
  gboolean vchanged = device_refresh_virtual_state(self, d);
  d->hidden = compute_device_hidden(self, d);
  device_emit_transition(self, d, FALSE, was_hidden, force_update || vchanged);
}

static GPtrArray *snapshot_devices(ConttestDeviceManager *self) {
  GPtrArray *snapshot = g_ptr_array_new_with_free_func(g_object_unref);
  if (!self->devices)
    return snapshot;
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    g_ptr_array_add(snapshot, g_object_ref(d));
  }
  return snapshot;
}

static void schedule_sdl_sync(ConttestDeviceManager *self);

typedef struct {
  GWeakRef device;
  char *evdev_syspath;
  char *sdl_path;
  gboolean check_evdev;
  guint evdev_udev_seq;
  gboolean did_evdev;
  gboolean evdev_dead;
  UdevAttachMeta evdev_meta;
  gboolean check_sdl;
  guint sdl_udev_seq;
  gboolean did_sdl;
  gboolean sdl_dead;
  UdevAttachMeta sdl_meta;
} UdevRefreshItem;

static void udev_refresh_item_free(gpointer data) {
  UdevRefreshItem *item = data;
  if (!item)
    return;
  g_weak_ref_clear(&item->device);
  g_free(item->evdev_syspath);
  g_free(item->sdl_path);
  udev_attach_meta_clear(&item->evdev_meta);
  udev_attach_meta_clear(&item->sdl_meta);
  g_free(item);
}

typedef struct {
  GWeakRef manager;
  GPtrArray *items; /* element: UdevRefreshItem* */
} UdevRefreshTaskCtx;

static void udev_refresh_task_ctx_free(gpointer data) {
  UdevRefreshTaskCtx *ctx = data;
  if (!ctx)
    return;
  g_weak_ref_clear(&ctx->manager);
  g_clear_pointer(&ctx->items, g_ptr_array_unref);
  g_free(ctx);
}

static void schedule_udev_refresh_all(ConttestDeviceManager *self);

static void udev_refresh_thread(GTask *task, gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable) {
  (void)source_object;
  UdevRefreshTaskCtx *ctx = task_data;
  if (g_cancellable_is_cancelled(cancellable) || !ctx->items ||
      ctx->items->len == 0) {
    g_task_return_boolean(task, TRUE);
    return;
  }

  struct udev *u = udev_new();
  if (!u) {
    g_task_return_boolean(task, FALSE);
    return;
  }

  for (guint i = 0; i < ctx->items->len; i++) {
    if (g_cancellable_is_cancelled(cancellable))
      break;
    UdevRefreshItem *item = g_ptr_array_index(ctx->items, i);
    if (item->check_evdev && item->evdev_syspath) {
      item->did_evdev = TRUE;
      struct udev_device *dev =
          udev_device_new_from_syspath(u, item->evdev_syspath);
      if (dev) {
        udev_extract_attach_meta(dev, &item->evdev_meta);
        item->evdev_dead = FALSE;
        udev_device_unref(dev);
      } else {
        item->evdev_dead = TRUE;
      }
    }
    if (item->check_sdl && item->sdl_path) {
      item->did_sdl = TRUE;
      struct udev_device *dev =
          worker_udev_new_from_devnode(u, item->sdl_path);
      if (dev) {
        udev_extract_attach_meta(dev, &item->sdl_meta);
        item->sdl_dead = FALSE;
        udev_device_unref(dev);
      } else {
        item->sdl_dead = TRUE;
      }
    }
  }

  udev_unref(u);
  g_task_return_boolean(task, TRUE);
}

static void on_udev_refresh_done(GObject *source, GAsyncResult *res,
                                 gpointer user_data) {
  (void)source;
  (void)user_data;
  GTask *task = G_TASK(res);
  UdevRefreshTaskCtx *ctx = g_task_get_task_data(task);
  GError *err = NULL;
  gboolean ok = g_task_propagate_boolean(task, &err);
  g_clear_error(&err);

  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(&ctx->manager);
  if (!self || self->disposed || !self->devices || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable)) {
    return;
  }

  self->udev_refresh_in_flight = FALSE;

  gboolean needs_sdl_sync = FALSE;
  if (ok && ctx->items) {
    for (guint i = 0; i < ctx->items->len; i++) {
      UdevRefreshItem *item = g_ptr_array_index(ctx->items, i);
      g_autoptr(ConttestDevice) d = g_weak_ref_get(&item->device);
      if (!d || !g_ptr_array_find(self->devices, d, NULL))
        continue;

      if (item->did_evdev && d->has_evdev &&
          d->evdev_udev_seq == item->evdev_udev_seq &&
          g_strcmp0(d->evdev_syspath, item->evdev_syspath) == 0) {
        if (item->evdev_dead) {
          d->evdev_meta.udev_dead = TRUE;
        } else {
          udev_attach_meta_move(&d->evdev_meta, &item->evdev_meta);
        }
      }

      if (item->did_sdl && d->has_sdl && !d->sdl_probe_pending &&
          d->sdl_udev_seq == item->sdl_udev_seq &&
          g_strcmp0(d->sdl_path, item->sdl_path) == 0) {
        if (item->sdl_dead) {
          d->sdl_meta.udev_dead = TRUE;
          needs_sdl_sync = TRUE;
        } else {
          udev_attach_meta_move(&d->sdl_meta, &item->sdl_meta);
        }
      }

      refresh_and_emit_device(self, d, FALSE);
      if (self->disposed || !self->devices)
        return;
    }
  }

  if (needs_sdl_sync)
    schedule_sdl_sync(self);

  if (self->udev_refresh_pending && !self->disposed) {
    self->udev_refresh_pending = FALSE;
    schedule_udev_refresh_all(self);
  }
}

static void schedule_udev_refresh_all(ConttestDeviceManager *self) {
  if (self->disposed || !self->devices || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable))
    return;

  if (self->udev_refresh_in_flight) {
    self->udev_refresh_pending = TRUE;
    return;
  }

  GPtrArray *items = g_ptr_array_new_with_free_func(udev_refresh_item_free);
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    gboolean need_evdev = d->has_evdev && d->evdev_syspath != NULL;
    gboolean need_sdl = d->has_sdl && !d->sdl_probe_pending && d->sdl_path &&
                        g_str_has_prefix(d->sdl_path, "/dev/");
    if (!need_evdev && !need_sdl)
      continue;

    UdevRefreshItem *item = g_new0(UdevRefreshItem, 1);
    g_weak_ref_init(&item->device, d);
    item->evdev_meta.udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
    item->sdl_meta.udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
    if (need_evdev) {
      item->check_evdev = TRUE;
      item->evdev_udev_seq = d->evdev_udev_seq;
      item->evdev_syspath = g_strdup(d->evdev_syspath);
    }
    if (need_sdl) {
      item->check_sdl = TRUE;
      item->sdl_udev_seq = d->sdl_udev_seq;
      item->sdl_path = g_strdup(d->sdl_path);
    }
    g_ptr_array_add(items, item);
  }

  if (items->len == 0) {
    g_ptr_array_unref(items);
    return;
  }

  self->udev_refresh_in_flight = TRUE;
  self->udev_refresh_pending = FALSE;

  UdevRefreshTaskCtx *ctx = g_new0(UdevRefreshTaskCtx, 1);
  g_weak_ref_init(&ctx->manager, self);
  ctx->items = items;

  GTask *task =
      g_task_new(NULL, self->cancellable, on_udev_refresh_done, NULL);
  g_task_set_task_data(task, ctx, udev_refresh_task_ctx_free);
  g_task_run_in_thread(task, udev_refresh_thread);
  g_object_unref(task);
}

static void on_inputplumber_changed(ConttestInputPlumberMonitor *monitor,
                                    gpointer user_data) {
  (void)monitor;
  ConttestDeviceManager *self = CONTTEST_DEVICE_MANAGER(user_data);
  if (self->disposed || !self->devices)
    return;

  /* Apply in-memory updates immediately and coalesce an off-thread udev
   * refresh for by-hidden devlinks and vanishing devnodes. */
  g_autoptr(GPtrArray) snapshot = snapshot_devices(self);
  gboolean needs_sdl_sync = FALSE;
  for (guint i = 0; i < snapshot->len; i++) {
    ConttestDevice *d = g_ptr_array_index(snapshot, i);
    if (!self->devices || !g_ptr_array_find(self->devices, d, NULL))
      continue;
    refresh_and_emit_device(self, d, FALSE);
    if (self->disposed || !self->devices)
      return;
    if (g_ptr_array_find(self->devices, d, NULL) && d->has_sdl &&
        d->sdl_meta.udev_dead)
      needs_sdl_sync = TRUE;
  }
  if (needs_sdl_sync)
    schedule_sdl_sync(self);

  schedule_udev_refresh_all(self);
}

typedef struct {
  gboolean found;
  UdevAttachMeta meta;
} SdlProbeResult;

static void sdl_probe_result_free(gpointer data) {
  SdlProbeResult *r = data;
  if (!r)
    return;
  udev_attach_meta_clear(&r->meta);
  g_free(r);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SdlProbeResult, sdl_probe_result_free)

static void probe_sdl_udev_thread(GTask *task, gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable) {
  (void)source_object;
  UdevTaskCtx *ctx = task_data;
  SdlProbeResult *res = g_new0(SdlProbeResult, 1);
  res->meta.udev_conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;

  if (!g_cancellable_is_cancelled(cancellable) && ctx->arg) {
    struct udev *u = udev_new();
    if (u) {
      struct udev_device *dev = worker_udev_new_from_devnode(u, ctx->arg);
      if (dev) {
        res->found = TRUE;
        udev_extract_attach_meta(dev, &res->meta);
        udev_device_unref(dev);
      }
      udev_unref(u);
    }
  }

  g_task_return_pointer(task, res, sdl_probe_result_free);
}

static void on_sdl_udev_probe_done(GObject *source, GAsyncResult *res,
                                   gpointer user_data) {
  (void)source;
  (void)user_data;
  GTask *task = G_TASK(res);
  UdevTaskCtx *ctx = g_task_get_task_data(task);
  GError *err = NULL;
  g_autoptr(SdlProbeResult) probe = g_task_propagate_pointer(task, &err);
  g_clear_error(&err);

  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(&ctx->manager);
  if (!self || self->disposed || !self->devices || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable))
    return;

  g_autoptr(ConttestDevice) d = g_weak_ref_get(&ctx->device);
  if (!d || !g_ptr_array_find(self->devices, d, NULL) || !d->has_sdl ||
      d->sdl_udev_seq != ctx->seq)
    return;

  if (d->sdl_gamepad && !SDL_GamepadConnected(d->sdl_gamepad)) {
    device_detach_sdl(d);
    check_empty_and_remove(self, d);
    if (!self->disposed)
      schedule_sdl_sync(self);
    return;
  }

  gboolean was_pending = d->sdl_probe_pending;
  d->sdl_probe_pending = FALSE;

  if (probe && probe->found) {
    udev_attach_meta_move(&d->sdl_meta, &probe->meta);
    d->sdl_udev_seq++;

    ConttestDevice *existing = NULL;
    if (!d->has_evdev) {
      existing = find_device_by_hardware_for_sdl(
          self, d->sdl_path, d->sdl_meta.own_syspath,
          (const char *const *)d->sdl_meta.hw_keys, d->vendor_id,
          d->product_id);
      if (existing == d || (existing && existing->has_sdl))
        existing = NULL;
    }

    if (existing) {
      /* Transfer SDL ownership into the matching evdev-only device and drop
       * the still-hidden placeholder without emitting spurious signals. */
      gboolean existing_was_hidden = existing->hidden;
      device_set_name(existing, d->name);
      existing->has_sdl = TRUE;
      existing->sdl_id = d->sdl_id;
      g_free(existing->sdl_path);
      existing->sdl_path = g_steal_pointer(&d->sdl_path);
      if (existing->sdl_gamepad) {
        SDL_CloseGamepad(existing->sdl_gamepad);
      }
      existing->sdl_gamepad = d->sdl_gamepad;
      d->sdl_gamepad = NULL;
      d->has_sdl = FALSE;
      d->sdl_id = 0;
      d->sdl_udev_seq++;
      udev_attach_meta_move(&existing->sdl_meta, &d->sdl_meta);
      existing->sdl_udev_seq++;
      if (existing->vendor_id == 0 || existing->product_id == 0) {
        existing->vendor_id = d->vendor_id;
        existing->product_id = d->product_id;
      }
      g_ptr_array_remove(self->devices, d);

      device_poll_battery(self, existing);
      device_refresh_virtual_state(self, existing);
      existing->hidden = compute_device_hidden(self, existing);
      device_emit_transition(self, existing, FALSE, existing_was_hidden, TRUE);
      return;
    }
  }

  if (!was_pending) {
    if (probe && probe->found) {
      gboolean changed = device_poll_battery(self, d);
      refresh_and_emit_device(self, d, changed);
    }
    return;
  }

  device_poll_battery(self, d);
  device_refresh_virtual_state(self, d);
  d->hidden = compute_device_hidden(self, d);
  device_emit_transition(self, d, TRUE, TRUE, TRUE);
}

static void schedule_sdl_udev_probe(ConttestDeviceManager *self,
                                    ConttestDevice *d,
                                    const char *sdl_path,
                                    guint seq) {
  if (self->disposed || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable))
    return;

  GTask *task =
      g_task_new(NULL, self->cancellable, on_sdl_udev_probe_done, NULL);
  g_task_set_task_data(
      task, udev_task_ctx_new_for_device(self, d, sdl_path, NULL, seq),
      udev_task_ctx_free);
  g_task_run_in_thread(task, probe_sdl_udev_thread);
  g_object_unref(task);
}

static void handle_sdl_added(ConttestDeviceManager *self, SDL_JoystickID id) {
  if (self->disposed || !self->devices || find_device_by_sdl_id(self, id))
    return;

  const char *name = SDL_GetGamepadNameForID(id);
  if (!name)
    name = "Unknown Gamepad";

  const char *sdl_path = SDL_GetGamepadPathForID(id);
  uint16_t sdl_vid = SDL_GetGamepadVendorForID(id);
  uint16_t sdl_pid = SDL_GetGamepadProductForID(id);

  /* Deduplicate only dedicated /dev/input/event* nodes with matching identity
   * so empty or shared multi-port adapter paths never coalesce distinct pads. */
  ConttestDevice *d =
      find_device_by_sdl_path(self, sdl_path, sdl_vid, sdl_pid, name);
  if (d)
    return;

  d = find_device_by_hardware_for_sdl(self, sdl_path, NULL, NULL, sdl_vid,
                                      sdl_pid);

  if (d) {
    gboolean was_hidden = d->hidden;
    device_set_name(d, name);
    device_attach_sdl(d, id, sdl_path);
    if (d->vendor_id == 0 || d->product_id == 0) {
      d->vendor_id = sdl_vid;
      d->product_id = sdl_pid;
    }
    device_poll_battery(self, d);
    device_refresh_virtual_state(self, d);
    d->hidden = compute_device_hidden(self, d);
    device_emit_transition(self, d, FALSE, was_hidden, TRUE);
    return;
  }

  d = conttest_device_new(name);
  g_ptr_array_add(self->devices, d);
  device_attach_sdl(d, id, sdl_path);
  d->vendor_id = sdl_vid;
  d->product_id = sdl_pid;

  if (sdl_path && g_str_has_prefix(sdl_path, "/dev/") && self->cancellable &&
      !g_cancellable_is_cancelled(self->cancellable)) {
    /* Defer emission for unmatched /dev/ paths until off-thread udev metadata
     * tells us whether it matches an evdev node or is an InputPlumber node. */
    d->sdl_probe_pending = TRUE;
    d->hidden = TRUE;
    guint seq = ++d->sdl_udev_seq;
    schedule_sdl_udev_probe(self, d, sdl_path, seq);
    return;
  }

  device_poll_battery(self, d);
  device_refresh_virtual_state(self, d);
  d->hidden = compute_device_hidden(self, d);
  device_emit_transition(self, d, TRUE, FALSE, TRUE);
}

static void handle_sdl_removed(ConttestDeviceManager *self, SDL_JoystickID id) {
  ConttestDevice *d = find_device_by_sdl_id(self, id);
  if (d) {
    device_detach_sdl(d);
    check_empty_and_remove(self, d);
  }
}

/* Shared helper applying off-thread extracted evdev udev metadata on main loop. */
static void apply_probed_evdev_meta(ConttestDeviceManager *self,
                                    UdevDeviceMeta *m) {
  if (!self || self->disposed || !self->devices || !m || !m->syspath)
    return;

  ConttestDevice *existing = find_device_by_evdev_syspath(self, m->syspath);
  if (existing) {
    gboolean changed = FALSE;
    if (m->devnode && g_strcmp0(existing->evdev_devnode, m->devnode) != 0) {
      g_free(existing->evdev_devnode);
      existing->evdev_devnode = g_strdup(m->devnode);
      changed = TRUE;
    }
    udev_attach_meta_move(&existing->evdev_meta, &m->meta);
    existing->evdev_udev_seq++;
    if (m->vendor_id != 0 && existing->vendor_id != m->vendor_id) {
      existing->vendor_id = m->vendor_id;
      changed = TRUE;
    }
    if (m->product_id != 0 && existing->product_id != m->product_id) {
      existing->product_id = m->product_id;
      changed = TRUE;
    }
    if (!existing->has_sdl &&
        existing->connection_state == SDL_JOYSTICK_CONNECTION_UNKNOWN) {
      SDL_JoystickConnectionState conn =
          device_cached_udev_connection_state(existing);
      if (conn != existing->connection_state) {
        existing->connection_state = conn;
        changed = TRUE;
      }
    }
    if (device_poll_battery(self, existing))
      changed = TRUE;
    refresh_and_emit_device(self, existing, changed);
    return;
  }

  ConttestDevice *d = find_device_by_hardware_for_evdev(
      self, m->devnode, m->meta.own_syspath,
      (const char *const *)m->meta.hw_keys, m->vendor_id, m->product_id,
      m->meta.is_virtual);

  gboolean is_new = FALSE;
  gboolean was_hidden = FALSE;
  if (!d) {
    d = conttest_device_new(m->name);
    g_ptr_array_add(self->devices, d);
    is_new = TRUE;
  } else {
    was_hidden = d->hidden;
    /* Only update the name from evdev if SDL hasn't already provided a
     * better (mapped) name. SDL names like "PS4 Controller" are preferred
     * over raw evdev names like "Wireless Controller". */
    if (!d->has_sdl)
      device_set_name(d, m->name);
    if (d->sdl_probe_pending) {
      d->sdl_probe_pending = FALSE;
      d->sdl_udev_seq++;
    }
  }

  device_attach_evdev(d, m->syspath, m->devnode);
  udev_attach_meta_move(&d->evdev_meta, &m->meta);
  d->evdev_udev_seq++;

  if (m->vendor_id != 0)
    d->vendor_id = m->vendor_id;
  if (m->product_id != 0)
    d->product_id = m->product_id;

  device_poll_battery(self, d);

  /* For evdev-only devices SDL never sets connection_state, so use cached udev
   * metadata directly. */
  if (!d->has_sdl && d->connection_state == SDL_JOYSTICK_CONNECTION_UNKNOWN) {
    d->connection_state = device_cached_udev_connection_state(d);
  }

  device_refresh_virtual_state(self, d);
  d->hidden = compute_device_hidden(self, d);
  device_emit_transition(self, d, is_new, was_hidden, TRUE);
}

/* Re-scan evdev devices asynchronously via a thread-local udev handle. Used on
 * startup and as a fallback when the netlink monitor is unavailable (Flatpak). */
static void schedule_evdev_scan(ConttestDeviceManager *self);

typedef struct {
  GWeakRef manager;
  GPtrArray *known_syspaths; /* element: char* */
  guint remove_seq;
} EvdevScanTaskCtx;

static void evdev_scan_task_ctx_free(gpointer data) {
  EvdevScanTaskCtx *ctx = data;
  if (!ctx)
    return;
  g_weak_ref_clear(&ctx->manager);
  g_clear_pointer(&ctx->known_syspaths, g_ptr_array_unref);
  g_free(ctx);
}

typedef struct {
  GPtrArray *scanned;           /* element: UdevDeviceMeta* */
  GPtrArray *vanished_syspaths; /* element: char* */
  guint remove_seq;
} EvdevScanResult;

static void evdev_scan_result_free(gpointer data) {
  EvdevScanResult *r = data;
  if (!r)
    return;
  g_clear_pointer(&r->scanned, g_ptr_array_unref);
  g_clear_pointer(&r->vanished_syspaths, g_ptr_array_unref);
  g_free(r);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(EvdevScanResult, evdev_scan_result_free)

static void scan_evdev_thread(GTask *task, gpointer source_object,
                              gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  EvdevScanTaskCtx *ctx = task_data;
  EvdevScanResult *res = g_new0(EvdevScanResult, 1);
  res->scanned = g_ptr_array_new_with_free_func(udev_device_meta_free);
  res->vanished_syspaths = g_ptr_array_new_with_free_func(g_free);
  res->remove_seq = ctx->remove_seq;

  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_pointer(task, res, evdev_scan_result_free);
    return;
  }

  struct udev *u = udev_new();
  if (u) {
    g_autoptr(GHashTable) found_set =
        g_hash_table_new(g_str_hash, g_str_equal);
    struct udev_enumerate *enumerate = udev_enumerate_new(u);
    if (enumerate) {
      udev_enumerate_add_match_subsystem(enumerate, "input");
      udev_enumerate_add_match_property(enumerate, "ID_INPUT_JOYSTICK", "1");
      udev_enumerate_scan_devices(enumerate);
      struct udev_list_entry *entry;
      udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        if (g_cancellable_is_cancelled(cancellable))
          break;
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(u, path);
        if (dev) {
          UdevDeviceMeta *meta = udev_device_meta_extract_evdev(dev);
          if (meta) {
            g_hash_table_add(found_set, meta->syspath);
            g_ptr_array_add(res->scanned, meta);
          }
          udev_device_unref(dev);
        }
      }
      udev_enumerate_unref(enumerate);
    }

    /* Check previously known evdev syspaths that were absent from enumeration
     * on the worker thread so main loop never calls udev_device_new_from_syspath. */
    if (!g_cancellable_is_cancelled(cancellable) && ctx->known_syspaths) {
      for (guint i = 0; i < ctx->known_syspaths->len; i++) {
        if (g_cancellable_is_cancelled(cancellable))
          break;
        const char *old_syspath = g_ptr_array_index(ctx->known_syspaths, i);
        if (!g_hash_table_contains(found_set, old_syspath)) {
          struct udev_device *dev = udev_device_new_from_syspath(u, old_syspath);
          if (!dev) {
            g_ptr_array_add(res->vanished_syspaths, g_strdup(old_syspath));
          } else {
            udev_device_unref(dev);
          }
        }
      }
    }

    udev_unref(u);
  }

  g_task_return_pointer(task, res, evdev_scan_result_free);
}

static void on_evdev_scan_done(GObject *source, GAsyncResult *res,
                               gpointer user_data) {
  (void)source;
  (void)user_data;
  GTask *task = G_TASK(res);
  EvdevScanTaskCtx *ctx = g_task_get_task_data(task);
  GError *err = NULL;
  g_autoptr(EvdevScanResult) scan_res = g_task_propagate_pointer(task, &err);
  g_clear_error(&err);

  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(&ctx->manager);
  if (!self || self->disposed || !self->devices || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable))
    return;

  self->evdev_scan_in_flight = FALSE;

  if (scan_res) {
    if (scan_res->remove_seq != self->evdev_remove_seq) {
      /* A netlink removal raced with the background scan; run a clean pass. */
      self->evdev_rescan_pending = TRUE;
    } else {
      if (scan_res->vanished_syspaths) {
        for (guint i = 0; i < scan_res->vanished_syspaths->len; i++) {
          const char *gone = g_ptr_array_index(scan_res->vanished_syspaths, i);
          ConttestDevice *d = find_device_by_evdev_syspath(self, gone);
          if (d) {
            device_detach_evdev(d);
            check_empty_and_remove(self, d);
            if (self->disposed || !self->devices)
              return;
          }
        }
      }

      if (scan_res->scanned) {
        for (guint i = 0; i < scan_res->scanned->len; i++) {
          UdevDeviceMeta *meta = g_ptr_array_index(scan_res->scanned, i);
          apply_probed_evdev_meta(self, meta);
          if (self->disposed || !self->devices)
            return;
        }
      }
    }
  }

  if (self->evdev_rescan_pending && !self->disposed) {
    self->evdev_rescan_pending = FALSE;
    schedule_evdev_scan(self);
  }
}

static void schedule_evdev_scan(ConttestDeviceManager *self) {
  if (self->disposed || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable))
    return;

  if (self->evdev_scan_in_flight) {
    self->evdev_rescan_pending = TRUE;
    return;
  }

  self->evdev_scan_in_flight = TRUE;

  EvdevScanTaskCtx *ctx = g_new0(EvdevScanTaskCtx, 1);
  g_weak_ref_init(&ctx->manager, self);
  ctx->known_syspaths = g_ptr_array_new_with_free_func(g_free);
  ctx->remove_seq = self->evdev_remove_seq;
  if (self->devices) {
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_evdev && d->evdev_syspath) {
        g_ptr_array_add(ctx->known_syspaths, g_strdup(d->evdev_syspath));
      }
    }
  }

  GTask *task = g_task_new(NULL, self->cancellable, on_evdev_scan_done, NULL);
  g_task_set_task_data(task, ctx, evdev_scan_task_ctx_free);
  g_task_run_in_thread(task, scan_evdev_thread);
  g_object_unref(task);
}

static void poll_sdl_events(ConttestDeviceManager *self, gboolean force_pump) {
  if (self->disposed || !self->devices)
    return;

  gint64 now = g_get_monotonic_time();
  if (force_pump || last_sdl_pump_us == 0 ||
      (now - last_sdl_pump_us) >= SDL_PUMP_COALESCE_US) {
    conttest_sdl_pump_events();
  }

  gboolean sdl_hotplug = FALSE;
  SDL_Event event;
  /* Drain the entire SDL event queue each tick: axis, joystick, sensor, and
   * battery events would otherwise accumulate up to SDL's 65,535-event cap
   * and cause future GAMEPAD_ADDED / GAMEPAD_REMOVED events to be dropped. */
  while (!self->disposed && self->devices &&
         SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) > 0) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
      if (!find_device_by_sdl_id(self, event.gdevice.which)) {
        sdl_hotplug = TRUE;
        handle_sdl_added(self, event.gdevice.which);
      }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
      sdl_hotplug = TRUE;
      handle_sdl_removed(self, event.gdevice.which);
    } else if (event.type == SDL_EVENT_JOYSTICK_BATTERY_UPDATED) {
      ConttestDevice *d = find_device_by_sdl_id(self, event.jbattery.which);
      if (d && !d->hidden && device_poll_battery(self, d)) {
        g_autoptr(ConttestDevice) keep = g_object_ref(d);
        g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, keep);
      }
    }
  }

  if (self->disposed || !self->devices)
    return;

  /* Safety net: detach any open SDL gamepad that SDL now reports disconnected
   * even if no removal event remained in the queue. */
  g_autoptr(GPtrArray) snapshot = snapshot_devices(self);
  for (guint i = 0; i < snapshot->len; i++) {
    if (self->disposed || !self->devices)
      return;
    ConttestDevice *d = g_ptr_array_index(snapshot, i);
    if (!g_ptr_array_find(self->devices, d, NULL))
      continue;
    if (d->has_sdl && d->sdl_gamepad && !SDL_GamepadConnected(d->sdl_gamepad)) {
      sdl_hotplug = TRUE;
      device_detach_sdl(d);
      check_empty_and_remove(self, d);
    }
  }

  /* Re-scan evdev devices on SDL hotplug. Needed because the udev netlink
   * monitor may be non-functional in sandboxed environments (Flatpak). */
  if (sdl_hotplug && !self->disposed) {
    schedule_evdev_scan(self);
  }
}

static gboolean on_sdl_sync_idle(gpointer user_data) {
  GWeakRef *wr = user_data;
  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(wr);
  if (!self || self->disposed)
    return G_SOURCE_REMOVE;
  self->sdl_sync_idle_id = 0;
  poll_sdl_events(self, TRUE);
  return G_SOURCE_REMOVE;
}

static void schedule_sdl_sync(ConttestDeviceManager *self) {
  if (!self->disposed && !self->sdl_sync_idle_id) {
    self->sdl_sync_idle_id = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE, on_sdl_sync_idle, manager_weak_ref_new(self),
        manager_weak_ref_free);
  }
}

static gboolean on_sdl_timer(gpointer user_data) {
  GWeakRef *wr = user_data;
  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(wr);
  if (!self || self->disposed)
    return G_SOURCE_REMOVE;
  poll_sdl_events(self, FALSE);
  return self->disposed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static gboolean on_battery_poll_timer(gpointer user_data) {
  GWeakRef *wr = user_data;
  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(wr);
  if (!self || self->disposed || !self->devices)
    return G_SOURCE_REMOVE;

  g_autoptr(GPtrArray) snapshot = snapshot_devices(self);
  for (guint i = 0; i < snapshot->len; i++) {
    ConttestDevice *d = g_ptr_array_index(snapshot, i);
    if (!self->devices || !g_ptr_array_find(self->devices, d, NULL))
      continue;
    if (!d->hidden && d->has_sdl && device_poll_battery(self, d)) {
      g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
      if (self->disposed || !self->devices)
        return G_SOURCE_REMOVE;
    }
  }
  return G_SOURCE_CONTINUE;
}

static void probe_evdev_syspath_thread(GTask *task, gpointer source_object,
                                       gpointer task_data,
                                       GCancellable *cancellable) {
  (void)source_object;
  UdevTaskCtx *ctx = task_data;
  if (g_cancellable_is_cancelled(cancellable) || !ctx->arg) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }

  UdevDeviceMeta *meta = NULL;
  struct udev *u = udev_new();
  if (u) {
    struct udev_device *dev = udev_device_new_from_syspath(u, ctx->arg);
    if (dev) {
      meta = udev_device_meta_extract_evdev(dev);
      udev_device_unref(dev);
    }
    udev_unref(u);
  }

  g_task_return_pointer(task, meta, udev_device_meta_free);
}

static void on_evdev_probe_done(GObject *source, GAsyncResult *res,
                                gpointer user_data) {
  (void)source;
  (void)user_data;
  GTask *task = G_TASK(res);
  UdevTaskCtx *ctx = g_task_get_task_data(task);
  GError *err = NULL;
  g_autoptr(UdevDeviceMeta) meta = g_task_propagate_pointer(task, &err);
  g_clear_error(&err);

  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(&ctx->manager);
  if (!self || self->disposed || !self->devices || !self->evdev_seq_by_syspath)
    return;

  if (self->evdev_probes_in_flight > 0)
    self->evdev_probes_in_flight--;

  if (!self->cancellable || g_cancellable_is_cancelled(self->cancellable)) {
    if (self->evdev_probes_in_flight == 0)
      g_hash_table_remove_all(self->evdev_seq_by_syspath);
    return;
  }

  gpointer val = NULL;
  if (ctx->arg &&
      g_hash_table_lookup_extended(self->evdev_seq_by_syspath, ctx->arg, NULL,
                                   &val) &&
      GPOINTER_TO_UINT(val) == ctx->seq) {
    if (meta) {
      apply_probed_evdev_meta(self, meta);
    }
  }

  if (!self->disposed && self->evdev_seq_by_syspath &&
      self->evdev_probes_in_flight == 0) {
    g_hash_table_remove_all(self->evdev_seq_by_syspath);
  }
}

static void schedule_evdev_probe(ConttestDeviceManager *self,
                                 const char *syspath) {
  if (self->disposed || !self->cancellable ||
      g_cancellable_is_cancelled(self->cancellable) || !syspath ||
      !self->evdev_seq_by_syspath)
    return;

  guint seq = ++self->evdev_seq;
  g_hash_table_replace(self->evdev_seq_by_syspath, g_strdup(syspath),
                       GUINT_TO_POINTER(seq));
  self->evdev_probes_in_flight++;

  GTask *task =
      g_task_new(NULL, self->cancellable, on_evdev_probe_done, NULL);
  g_task_set_task_data(
      task, udev_task_ctx_new_for_device(self, NULL, syspath, NULL, seq),
      udev_task_ctx_free);
  g_task_run_in_thread(task, probe_evdev_syspath_thread);
  g_object_unref(task);
}

static void handle_evdev_syspath_remove(ConttestDeviceManager *self,
                                        const char *syspath) {
  if (!syspath || self->disposed || !self->devices)
    return;

  self->evdev_remove_seq++;
  if (self->evdev_seq_by_syspath && self->evdev_probes_in_flight > 0) {
    /* Supersede any in-flight add/change probe for this disappearing syspath. */
    g_hash_table_replace(self->evdev_seq_by_syspath, g_strdup(syspath),
                         GUINT_TO_POINTER(++self->evdev_seq));
  }

  ConttestDevice *d = find_device_by_evdev_syspath(self, syspath);
  if (d) {
    gboolean still_has_sdl = d->has_sdl;
    gboolean had_matching_sdl_node =
        d->has_sdl && d->sdl_path && d->evdev_devnode &&
        g_strcmp0(d->sdl_path, d->evdev_devnode) == 0;
    device_detach_evdev(d);
    if (had_matching_sdl_node) {
      d->sdl_meta.udev_dead = TRUE;
      d->sdl_udev_seq++;
    }
    check_empty_and_remove(self, d);
    if (still_has_sdl && !self->disposed)
      schedule_sdl_sync(self);
  } else if (!self->disposed) {
    /* An SDL placeholder still probing udev may belong to this vanishing node;
     * force an SDL sync so disconnect state is reflected promptly. */
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *cand = g_ptr_array_index(self->devices, i);
      if (cand->has_sdl && cand->sdl_probe_pending) {
        schedule_sdl_sync(self);
        break;
      }
    }
  }
}

static void handle_power_supply_event(ConttestDeviceManager *self,
                                      const char *syspath,
                                      const char *action) {
  if (!syspath || self->disposed || !self->devices)
    return;

  if (g_strcmp0(action, "remove") == 0) {
    g_autoptr(GPtrArray) snapshot = snapshot_devices(self);
    for (guint i = 0; i < snapshot->len; i++) {
      ConttestDevice *d = g_ptr_array_index(snapshot, i);
      if (!self->devices || !g_ptr_array_find(self->devices, d, NULL))
        continue;
      if (d->battery_syspath && g_strcmp0(d->battery_syspath, syspath) == 0) {
        device_reset_ds3_battery(d);
        d->battery_state = SDL_POWERSTATE_UNKNOWN;
        d->battery_percent = -1;
        if (!d->hidden) {
          g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
          if (self->disposed || !self->devices)
            return;
        }
      }
    }
    return;
  }

  if (g_strcmp0(action, "add") == 0 || g_strcmp0(action, "change") == 0) {
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (!conttest_device_is_ds3(d) || !d->has_sdl)
        continue;
      if (d->battery_syspath && g_strcmp0(d->battery_syspath, syspath) == 0) {
        schedule_ds3_battery_task(self, d, TRUE);
      } else if (d->has_evdev && !d->battery_syspath) {
        if (d->ds3_battery_state != DS3_BATTERY_PROBING)
          d->ds3_battery_state = DS3_BATTERY_UNPROBED;
        schedule_ds3_battery_task(self, d, TRUE);
      }
    }
  }
}

static gboolean on_udev_event(gint fd, GIOCondition condition,
                              gpointer user_data) {
  (void)fd;
  GWeakRef *wr = user_data;
  g_autoptr(ConttestDeviceManager) self = g_weak_ref_get(wr);
  if (!self || self->disposed || !self->udev_mon ||
      (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))) {
    if (self)
      self->udev_watch_id = 0;
    return G_SOURCE_REMOVE;
  }

  struct udev_device *dev;
  while (!self->disposed &&
         (dev = udev_monitor_receive_device(self->udev_mon)) != NULL) {
    const char *sub = udev_device_get_subsystem(dev);
    const char *action = udev_device_get_action(dev);
    const char *syspath = udev_device_get_syspath(dev);
    if (g_strcmp0(sub, "power_supply") == 0) {
      handle_power_supply_event(self, syspath, action);
    } else if (g_strcmp0(action, "add") == 0 ||
               g_strcmp0(action, "change") == 0) {
      const char *devnode = udev_device_get_devnode(dev);
      const char *id_input_joystick =
          udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
      if (syspath && devnode && strncmp(devnode, "/dev/input/event", 16) == 0 &&
          id_input_joystick && strcmp(id_input_joystick, "1") == 0) {
        schedule_evdev_probe(self, syspath);
      }
    } else if (g_strcmp0(action, "remove") == 0) {
      handle_evdev_syspath_remove(self, syspath);
    }
    udev_device_unref(dev);
  }
  return self->disposed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void conttest_device_manager_init(ConttestDeviceManager *self) {
  self->devices = g_ptr_array_new_with_free_func(g_object_unref);
  self->cancellable = g_cancellable_new();
  self->evdev_seq_by_syspath =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->ip_monitor = conttest_inputplumber_monitor_new();
  g_signal_connect_object(self->ip_monitor, "changed",
                          G_CALLBACK(on_inputplumber_changed), self, 0);
  self->udev = udev_new();
  if (self->udev) {
    self->udev_mon = udev_monitor_new_from_netlink(self->udev, "udev");
    if (self->udev_mon) {
      udev_monitor_filter_add_match_subsystem_devtype(self->udev_mon, "input",
                                                      NULL);
      udev_monitor_filter_add_match_subsystem_devtype(self->udev_mon,
                                                      "power_supply", NULL);
      udev_monitor_enable_receiving(self->udev_mon);

      int fd = udev_monitor_get_fd(self->udev_mon);
      if (fd >= 0) {
        self->udev_watch_id = g_unix_fd_add_full(
            G_PRIORITY_DEFAULT, fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
            on_udev_event, manager_weak_ref_new(self), manager_weak_ref_free);
      }
    } else {
      /* Netlink socket blocked (e.g. Flatpak sandbox). Evdev devices will
       * be discovered via re-scan when SDL fires hotplug events. */
      g_info("udev monitor unavailable — evdev hotplug via SDL re-scan");
    }

    /* Enumerate initial evdev devices asynchronously off the main thread. */
    schedule_evdev_scan(self);
  }

  self->sdl_timer_id = g_timeout_add_full(
      G_PRIORITY_DEFAULT, SDL_POLL_INTERVAL_MS, on_sdl_timer,
      manager_weak_ref_new(self), manager_weak_ref_free);
  self->battery_timer_id = g_timeout_add_seconds_full(
      G_PRIORITY_DEFAULT, BATTERY_POLL_INTERVAL_SEC, on_battery_poll_timer,
      manager_weak_ref_new(self), manager_weak_ref_free);

  /* Initial SDL scan */
  int count = 0;
  SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
  if (gamepads) {
    for (int i = 0; i < count; ++i) {
      handle_sdl_added(self, gamepads[i]);
    }
    SDL_free(gamepads);
  }
}

static void conttest_device_manager_dispose(GObject *object) {
  ConttestDeviceManager *self = CONTTEST_DEVICE_MANAGER(object);
  self->disposed = TRUE;

  if (self->ip_monitor) {
    g_signal_handlers_disconnect_by_data(self->ip_monitor, self);
    g_object_run_dispose(G_OBJECT(self->ip_monitor));
    g_clear_object(&self->ip_monitor);
  }
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  if (self->sdl_sync_idle_id) {
    g_source_remove(self->sdl_sync_idle_id);
    self->sdl_sync_idle_id = 0;
  }
  if (self->sdl_timer_id) {
    g_source_remove(self->sdl_timer_id);
    self->sdl_timer_id = 0;
  }
  if (self->battery_timer_id) {
    g_source_remove(self->battery_timer_id);
    self->battery_timer_id = 0;
  }
  if (self->udev_watch_id) {
    g_source_remove(self->udev_watch_id);
    self->udev_watch_id = 0;
  }
  if (self->udev_mon) {
    udev_monitor_unref(self->udev_mon);
    self->udev_mon = NULL;
  }
  if (self->udev) {
    udev_unref(self->udev);
    self->udev = NULL;
  }
  g_clear_pointer(&self->evdev_seq_by_syspath, g_hash_table_unref);
  g_clear_pointer(&self->devices, g_ptr_array_unref);

  G_OBJECT_CLASS(conttest_device_manager_parent_class)->dispose(object);
}

static void
conttest_device_manager_class_init(ConttestDeviceManagerClass *klass) {
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  obj_class->dispose = conttest_device_manager_dispose;

  signals[SIGNAL_DEVICE_ADDED] =
      g_signal_new("device-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE, 1, CONTTEST_TYPE_DEVICE);
  signals[SIGNAL_DEVICE_REMOVED] = g_signal_new(
      "device-removed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, CONTTEST_TYPE_DEVICE);
  signals[SIGNAL_DEVICE_UPDATED] = g_signal_new(
      "device-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, CONTTEST_TYPE_DEVICE);
}

ConttestDeviceManager *conttest_device_manager_new(void) {
  return g_object_new(CONTTEST_TYPE_DEVICE_MANAGER, NULL);
}

void conttest_device_manager_foreach(ConttestDeviceManager *self,
                                     ConttestDeviceForeachFunc func,
                                     gpointer user_data) {
  g_return_if_fail(CONTTEST_IS_DEVICE_MANAGER(self));
  g_return_if_fail(func != NULL);
  if (!self->devices) return;
  g_autoptr(ConttestDeviceManager) self_guard = g_object_ref(self);
  g_autoptr(GPtrArray) snapshot = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (!d->hidden) {
      g_ptr_array_add(snapshot, g_object_ref(d));
    }
  }
  for (guint i = 0; i < snapshot->len; i++) {
    ConttestDevice *d = g_ptr_array_index(snapshot, i);
    if (self->devices && !d->hidden && g_ptr_array_find(self->devices, d, NULL)) {
      func(d, user_data);
    }
  }
}
