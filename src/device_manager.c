#include "device_manager.h"
#include <libudev.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ConttestDevice definition is private to this file. */
struct _ConttestDevice {
  char *name;
  gboolean has_sdl;
  SDL_JoystickID sdl_id;
  char *sdl_path;
  SDL_Gamepad *sdl_gamepad; /* kept open while attached, for battery polling */
  gboolean has_evdev;
  char *evdev_syspath;

  /* Hardware identity for narrow fallbacks (e.g. DS3 battery). */

  uint16_t vendor_id;
  uint16_t product_id;
  char *battery_syspath; /* Resolved power_supply node */

  /* Battery state — only meaningful when has_sdl is TRUE. */
  SDL_PowerState battery_state;
  int battery_percent; /* 0–100, or -1 if unknown */
  SDL_JoystickConnectionState connection_state;
};

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

struct _ConttestDeviceManager {
  GObject parent_instance;
  GPtrArray *devices; // Array of ConttestDevice*

  struct udev *udev;
  struct udev_monitor *udev_mon;
  GIOChannel *udev_channel;
  guint udev_watch_id;

  guint sdl_timer_id;
  guint battery_poll_counter; /* throttle: poll battery every N ticks */
};

enum {
  SIGNAL_DEVICE_ADDED,
  SIGNAL_DEVICE_REMOVED,
  SIGNAL_DEVICE_UPDATED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE(ConttestDeviceManager, conttest_device_manager, G_TYPE_OBJECT)

static ConttestDevice *conttest_device_new(const char *name) {
  ConttestDevice *dev = g_new0(ConttestDevice, 1);
  dev->name = g_strdup(name);
  dev->battery_state = SDL_POWERSTATE_UNKNOWN;
  dev->battery_percent = -1;
  dev->connection_state = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  return dev;
}

static void conttest_device_free(ConttestDevice *device) {
  if (!device)
    return;
  if (device->sdl_gamepad) {
    SDL_CloseGamepad(device->sdl_gamepad);
    device->sdl_gamepad = NULL;
  }
  g_free(device->name);
  g_free(device->sdl_path);
  g_free(device->evdev_syspath);
  g_free(device->battery_syspath);
  g_free(device);
}

// Device Mutation Helpers

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
  if (sdl_path && g_strcmp0(d->sdl_path, sdl_path) != 0) {
    g_free(d->sdl_path);
    d->sdl_path = g_strdup(sdl_path);
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
  d->battery_state = SDL_POWERSTATE_UNKNOWN;
  d->battery_percent = -1;
  d->connection_state = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  if (!d->has_evdev) {
    d->vendor_id = 0;
    d->product_id = 0;
  }
}

static void device_attach_evdev(ConttestDevice *d, const char *evdev_syspath) {
  d->has_evdev = TRUE;
  if (evdev_syspath && g_strcmp0(d->evdev_syspath, evdev_syspath) != 0) {
    g_free(d->evdev_syspath);
    d->evdev_syspath = g_strdup(evdev_syspath);
  }
}

static void device_detach_evdev(ConttestDevice *d) {
  d->has_evdev = FALSE;
  g_free(d->evdev_syspath);
  d->evdev_syspath = NULL;
  g_free(d->battery_syspath);
  d->battery_syspath = NULL;
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
    return device->vendor_id == SONY_VID && device->product_id == DS3_PID;
}

gboolean conttest_device_is_ds4(const ConttestDevice *device) {
    return device->vendor_id == SONY_VID &&
           (device->product_id == DS4_PID_V1 ||
            device->product_id == DS4_PID_V2 ||
            device->product_id == DS4_PID_BT);
}

gboolean conttest_device_is_ds5(const ConttestDevice *device) {
    return device->vendor_id == SONY_VID &&
           (device->product_id == DS5_PID_V1 ||
            device->product_id == DS5_PID_V2);
}


static gboolean parse_vid_pid(const char *str, char sep, uint16_t *v,
                              uint16_t *p) {
  if (!str)
    return FALSE;
  uint16_t tv, tp;
  int matches = (sep == '/')
      ? sscanf(str, "%*x/%hx/%hx", &tv, &tp)
      : sscanf(str, "%*x:%hx:%hx", &tv, &tp);
  if (matches == 2) {
    *v = tv;
    *p = tp;
    return TRUE;
  }
  return FALSE;
}

static char *resolve_ds3_battery_path(ConttestDeviceManager *self,
                                      ConttestDevice *d) {
  if (!d->evdev_syspath)
    return NULL;

  struct udev_device *udev_dev =
      udev_device_new_from_syspath(self->udev, d->evdev_syspath);
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
  struct udev_enumerate *en = udev_enumerate_new(self->udev);
  if (!en) {
    udev_device_unref(udev_dev);
    return NULL;
  }

  udev_enumerate_add_match_subsystem(en, "power_supply");
  udev_enumerate_scan_devices(en);

  struct udev_list_entry *l, *entries = udev_enumerate_get_list_entry(en);
  udev_list_entry_foreach(l, entries) {
    const char *name = udev_list_entry_get_name(l);
    struct udev_device *bat_dev =
        udev_device_new_from_syspath(self->udev, name);
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

static void poll_ds3_battery_sysfs(const char *syspath, SDL_PowerState *state,
                                   int *percent) {
  *state = SDL_POWERSTATE_UNKNOWN;
  *percent = -1;

  char path[512];
  char buf[64];
  FILE *f;

  /* Read capacity */
  snprintf(path, sizeof(path), "%s/capacity", syspath);
  f = fopen(path, "r");
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
  snprintf(path, sizeof(path), "%s/status", syspath);
  f = fopen(path, "r");
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
}

// Identifiers fallbacks removed: exclusively topological.

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
                                               const char *sdl_path) {
  if (!sdl_path) return NULL;
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (d->has_sdl && d->sdl_path && g_strcmp0(d->sdl_path, sdl_path) == 0)
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

static struct udev_device *udev_device_from_devnode(struct udev *udev,
                                                    const char *path) {
  if (!path)
    return NULL;
  struct stat st;
  if (stat(path, &st) == 0) {
    return udev_device_new_from_devnum(udev, S_ISBLK(st.st_mode) ? 'b' : 'c',
                                       st.st_rdev);
  }
  return NULL;
}

static gboolean is_same_hardware(struct udev *udev, const char *sdl_path,
                                 const char *evdev_syspath) {
  if (!sdl_path || !evdev_syspath)
    return FALSE;

  struct udev_device *u1 = udev_device_from_devnode(udev, sdl_path);
  struct udev_device *u2 = udev_device_new_from_syspath(udev, evdev_syspath);
  gboolean match = FALSE;

  if (u1 && u2) {
    if (g_strcmp0(udev_device_get_syspath(u1), udev_device_get_syspath(u2)) ==
        0) {
      match = TRUE;
    } else {
      struct udev_device *p1 = u1;
      while (p1 && !match) {
        const char *sp1 = udev_device_get_syspath(p1);
        struct udev_device *p2 = u2;
        while (p2) {
          const char *sp2 = udev_device_get_syspath(p2);
          if (sp1 && sp2 && g_strcmp0(sp1, sp2) == 0) {
            const char *sub = udev_device_get_subsystem(p1);
            if (sub) {
              if (g_strcmp0(sub, "hid") == 0 ||
                  g_strcmp0(sub, "bluetooth") == 0 ||
                  g_strcmp0(sub, "input") == 0) {
                match = TRUE;
                break;
              } else if (g_strcmp0(sub, "usb") == 0) {
                const char *devtype = udev_device_get_devtype(p1);
                if (g_strcmp0(devtype, "usb_interface") == 0) {
                  match = TRUE;
                  break;
                }
              }
            }
          }
          p2 = udev_device_get_parent(p2);
        }
        p1 = udev_device_get_parent(p1);
      }
    }
  }
  if (u1)
    udev_device_unref(u1);
  if (u2)
    udev_device_unref(u2);
  return match;
}

static ConttestDevice *
find_device_by_hardware_for_sdl(ConttestDeviceManager *self,
                                const char *sdl_path) {
  if (!sdl_path)
    return NULL;
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (d->has_evdev && d->evdev_syspath) {
      if (is_same_hardware(self->udev, sdl_path, d->evdev_syspath))
        return d;
    }
  }
  return NULL;
}

static ConttestDevice *
find_device_by_hardware_for_evdev(ConttestDeviceManager *self,
                                  const char *evdev_syspath) {
  if (!evdev_syspath)
    return NULL;

  /* First pass: topological match via udev parent chain (most reliable). */
  for (guint i = 0; i < self->devices->len; i++) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i);
    if (d->has_sdl && d->sdl_path) {
      if (is_same_hardware(self->udev, d->sdl_path, evdev_syspath))
        return d;
    }
  }

  /* Second pass: for SDL devices without a devnode path (e.g. HIDAPI-backed
   * controllers like DS3), fall back to VID/PID matching. Extract the udev
   * device's VID/PID and compare against SDL's reported values. */
  struct udev_device *udev_dev = udev_device_new_from_syspath(self->udev, evdev_syspath);
  if (udev_dev) {
    uint16_t udev_vid = 0, udev_pid = 0;
    const char *vid_str = udev_device_get_property_value(udev_dev, "ID_VENDOR_ID");
    const char *pid_str = udev_device_get_property_value(udev_dev, "ID_MODEL_ID");
    if (vid_str) udev_vid = (uint16_t)g_ascii_strtoull(vid_str, NULL, 16);
    if (pid_str) udev_pid = (uint16_t)g_ascii_strtoull(pid_str, NULL, 16);

    if (udev_vid == 0 || udev_pid == 0) {
      parse_vid_pid(udev_device_get_property_value(udev_dev, "PRODUCT"), '/',
                    &udev_vid, &udev_pid);
    }
    if (udev_vid == 0 || udev_pid == 0) {
      struct udev_device *h =
          udev_device_get_parent_with_subsystem_devtype(udev_dev, "hid", NULL);
      if (h)
        parse_vid_pid(udev_device_get_property_value(h, "HID_ID"), ':',
                      &udev_vid, &udev_pid);
    }
    udev_device_unref(udev_dev);

    if (udev_vid != 0 && udev_pid != 0) {
      for (guint i = 0; i < self->devices->len; i++) {
        ConttestDevice *d = g_ptr_array_index(self->devices, i);
        if (d->has_sdl && !d->has_evdev) {
          uint16_t sdl_vid = SDL_GetGamepadVendorForID(d->sdl_id);
          uint16_t sdl_pid = SDL_GetGamepadProductForID(d->sdl_id);
          if (sdl_vid == udev_vid && sdl_pid == udev_pid)
            return d;
        }
      }
    }
  }

  return NULL;
}

static void check_empty_and_remove(ConttestDeviceManager *self,
                                   ConttestDevice *d) {
  if (!d->has_sdl && !d->has_evdev) {
    g_ptr_array_remove(self->devices, d);
    g_signal_emit(self, signals[SIGNAL_DEVICE_REMOVED], 0, d);
    conttest_device_free(d);
  } else {
    g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
  }
}

/* Resolve connection type from udev for any device that has an evdev syspath
 * or an SDL devnode path. Returns SDL_JOYSTICK_CONNECTION_UNKNOWN if
 * the bustype cannot be determined. */
static SDL_JoystickConnectionState
resolve_connection_state_udev(ConttestDeviceManager *self,
                               const ConttestDevice *d) {
  struct udev_device *dev = NULL;
  if (d->has_evdev && d->evdev_syspath)
    dev = udev_device_new_from_syspath(self->udev, d->evdev_syspath);
  else if (d->sdl_path)
    dev = udev_device_from_devnode(self->udev, d->sdl_path);
  if (!dev)
    return SDL_JOYSTICK_CONNECTION_UNKNOWN;

  SDL_JoystickConnectionState conn = SDL_JOYSTICK_CONNECTION_UNKNOWN;
  struct udev_device *input_parent =
      udev_device_get_parent_with_subsystem_devtype(dev, "input", NULL);
  const char *bustype_str =
      input_parent
          ? udev_device_get_sysattr_value(input_parent, "id/bustype")
          : NULL;
  if (bustype_str) {
    unsigned int bustype = (unsigned int)strtoul(bustype_str, NULL, 16);
    conn = (bustype == 0x0003) ? SDL_JOYSTICK_CONNECTION_WIRED
                               : SDL_JOYSTICK_CONNECTION_WIRELESS;
  } else {
    gboolean has_usb = udev_device_get_parent_with_subsystem_devtype(
                           dev, "usb", NULL) != NULL;
    conn = has_usb ? SDL_JOYSTICK_CONNECTION_WIRED
                   : SDL_JOYSTICK_CONNECTION_WIRELESS;
  }
  udev_device_unref(dev);
  return conn;
}

/* Poll battery for an SDL-attached device. Returns TRUE if state changed. */
static gboolean device_poll_battery(ConttestDeviceManager *self,
                                    ConttestDevice *d) {
  if (!d->has_sdl || !d->sdl_gamepad)
    return FALSE;

  int percent = -1;
  SDL_PowerState state = SDL_POWERSTATE_UNKNOWN;

  if (conttest_device_is_ds3(d)) {
    /* Confirmed DS3 controllers use Linux power_supply exclusively. */
    if (!d->battery_syspath) {
      d->battery_syspath = resolve_ds3_battery_path(self, d);
    }

    if (d->battery_syspath) {
      poll_ds3_battery_sysfs(d->battery_syspath, &state, &percent);
    }
  } else {
    /* All other controllers use SDL. */
    state = SDL_GetGamepadPowerInfo(d->sdl_gamepad, &percent);
  }

  gboolean changed = FALSE;

  if (state != d->battery_state || percent != d->battery_percent) {
    d->battery_state = state;
    d->battery_percent = percent;
    changed = TRUE;
  }

  SDL_JoystickConnectionState conn =
      SDL_GetGamepadConnectionState(d->sdl_gamepad);
  if (conn == SDL_JOYSTICK_CONNECTION_UNKNOWN) {
    conn = resolve_connection_state_udev(self, d);
  }
  if (conn != d->connection_state) {
    d->connection_state = conn;
    changed = TRUE;
  }

  return changed;
}

static void handle_sdl_added(ConttestDeviceManager *self, SDL_JoystickID id) {
  const char *name = SDL_GetGamepadNameForID(id);
  if (!name)
    name = "Unknown Gamepad";

  const char *sdl_path = SDL_GetGamepadPathForID(id);

  /* Deduplicate: if another SDL gamepad with the same path is already tracked,
   * this is the same physical device reported twice (e.g. WiiU GC adapter
   * mirroring one controller across multiple ports). Ignore the duplicate. */
  ConttestDevice *d = find_device_by_sdl_path(self, sdl_path);
  if (d) return;

  d = find_device_by_hardware_for_sdl(self, sdl_path);

  gboolean is_new = FALSE;
  if (!d) {
    d = conttest_device_new(name);
    g_ptr_array_add(self->devices, d);
    is_new = TRUE;
  } else {
    device_set_name(d, name);
  }

  device_attach_sdl(d, id, sdl_path);
  device_poll_battery(self, d);

  if (is_new) {
    g_signal_emit(self, signals[SIGNAL_DEVICE_ADDED], 0, d);
  } else {
    g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
  }
}

static void handle_sdl_removed(ConttestDeviceManager *self, SDL_JoystickID id) {
  ConttestDevice *d = find_device_by_sdl_id(self, id);
  if (d) {
    device_detach_sdl(d);
    check_empty_and_remove(self, d);
  }
}

/* Re-scan evdev devices via udev enumeration. Used as a fallback when the
 * udev netlink monitor is unavailable (e.g. inside a Flatpak sandbox). */
static void handle_udev_add(ConttestDeviceManager *self, struct udev_device *dev);

static void scan_evdev_devices(ConttestDeviceManager *self) {
  if (!self->udev) return;
  struct udev_enumerate *enumerate = udev_enumerate_new(self->udev);
  if (!enumerate) return;
  udev_enumerate_add_match_subsystem(enumerate, "input");
  udev_enumerate_scan_devices(enumerate);
  struct udev_list_entry *entry;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
    const char *path = udev_list_entry_get_name(entry);
    struct udev_device *dev = udev_device_new_from_syspath(self->udev, path);
    if (dev) {
      handle_udev_add(self, dev);
      udev_device_unref(dev);
    }
  }
  udev_enumerate_unref(enumerate);

  /* Remove evdev devices whose syspath no longer exists. */
  for (guint i = self->devices->len; i > 0; i--) {
    ConttestDevice *d = g_ptr_array_index(self->devices, i - 1);
    if (d->has_evdev && d->evdev_syspath) {
      struct udev_device *dev = udev_device_new_from_syspath(self->udev, d->evdev_syspath);
      if (!dev) {
        device_detach_evdev(d);
        check_empty_and_remove(self, d);
      } else {
        udev_device_unref(dev);
      }
    }
  }
}

static gboolean on_sdl_timer(gpointer user_data) {
  ConttestDeviceManager *self = CONTTEST_DEVICE_MANAGER(user_data);
  SDL_PumpEvents();

  gboolean sdl_hotplug = FALSE;
  SDL_Event event;
  while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED,
                        SDL_EVENT_GAMEPAD_REMOVED) > 0) {
    sdl_hotplug = TRUE;
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
      handle_sdl_added(self, event.gdevice.which);
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
      handle_sdl_removed(self, event.gdevice.which);
    }
  }

  /* Re-scan evdev devices on SDL hotplug. Needed because the udev netlink
   * monitor may be non-functional in sandboxed environments (Flatpak). */
  if (sdl_hotplug) {
    scan_evdev_devices(self);
  }

  /* Periodically refresh battery state (~every 5s).
   * Coupled to on_sdl_timer 50ms interval (100 ticks = 5000ms). */
  if (++self->battery_poll_counter >= 100) {
    self->battery_poll_counter = 0;
    for (guint i = 0; i < self->devices->len; i++) {
      ConttestDevice *d = g_ptr_array_index(self->devices, i);
      if (d->has_sdl && device_poll_battery(self, d)) {
        g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
      }
    }
  }

  return G_SOURCE_CONTINUE;
}

static void handle_udev_add(ConttestDeviceManager *self,
                            struct udev_device *dev) {
  const char *syspath = udev_device_get_syspath(dev);
  // Only care about evdev nodes /dev/input/eventX
  const char *devnode = udev_device_get_devnode(dev);
  if (!devnode || strncmp(devnode, "/dev/input/event", 16) != 0)
    return;

  // Check if it's a joystick
  const char *id_input_joystick =
      udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
  if (!id_input_joystick || strcmp(id_input_joystick, "1") != 0)
    return;

  /* Skip if this evdev device is already tracked. */
  if (find_device_by_evdev_syspath(self, syspath))
    return;

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

  // Strip surrounding quotes if present (e.g. NAME="Xbox Controller")
  char *clean_name = g_strdup(name);
  size_t len = strlen(clean_name);
  if (len >= 2 && clean_name[0] == '"' && clean_name[len - 1] == '"') {
    clean_name[len - 1] = '\0';
    memmove(clean_name, clean_name + 1, len - 1); /* copies len-2 chars + new '\0' */
  }

  ConttestDevice *d = find_device_by_hardware_for_evdev(self, syspath);

  gboolean is_new = FALSE;
  if (!d) {
    d = conttest_device_new(clean_name);
    g_ptr_array_add(self->devices, d);
    is_new = TRUE;
  } else {
    /* Only update the name from evdev if SDL hasn't already provided a
     * better (mapped) name. SDL names like "PS4 Controller" are preferred
     * over raw evdev names like "Wireless Controller". */
    if (!d->has_sdl)
      device_set_name(d, clean_name);
  }

  device_attach_evdev(d, syspath);

  /* Harvest hardware identity.
   * Bluetooth DS3s often lack ID_VENDOR_ID on the event node but have PRODUCT
   * or a HID parent with HID_ID. */
  const char *vid_str = udev_device_get_property_value(dev, "ID_VENDOR_ID");
  const char *pid_str = udev_device_get_property_value(dev, "ID_MODEL_ID");
  if (vid_str)
    d->vendor_id = (uint16_t)g_ascii_strtoull(vid_str, NULL, 16);
  if (pid_str)
    d->product_id = (uint16_t)g_ascii_strtoull(pid_str, NULL, 16);

  if (d->vendor_id == 0 || d->product_id == 0) {
    parse_vid_pid(udev_device_get_property_value(dev, "PRODUCT"), '/',
                  &d->vendor_id, &d->product_id);
  }
  if (d->vendor_id == 0 || d->product_id == 0) {
    struct udev_device *h =
        udev_device_get_parent_with_subsystem_devtype(dev, "hid", NULL);
    if (h) {
      parse_vid_pid(udev_device_get_property_value(h, "HID_ID"), ':',
                    &d->vendor_id, &d->product_id);
    }
  }

  /* Trigger immediate poll now that identity is established. */
  device_poll_battery(self, d);

  /* For evdev-only devices SDL never sets connection_state, so resolve it
   * from udev directly. */
  if (!d->has_sdl && d->connection_state == SDL_JOYSTICK_CONNECTION_UNKNOWN) {
    d->connection_state = resolve_connection_state_udev(self, d);
  }

  if (is_new) {
    g_signal_emit(self, signals[SIGNAL_DEVICE_ADDED], 0, d);
  } else {
    g_signal_emit(self, signals[SIGNAL_DEVICE_UPDATED], 0, d);
  }

  g_free(clean_name);
}

static void handle_udev_remove(ConttestDeviceManager *self,
                               struct udev_device *dev) {
  const char *syspath = udev_device_get_syspath(dev);
  ConttestDevice *d = find_device_by_evdev_syspath(self, syspath);
  if (d) {
    device_detach_evdev(d);
    check_empty_and_remove(self, d);
  }
}

static gboolean on_udev_event(GIOChannel *source, GIOCondition condition,
                              gpointer user_data) {
  (void)source;
  (void)condition;
  ConttestDeviceManager *self = CONTTEST_DEVICE_MANAGER(user_data);

  struct udev_device *dev = udev_monitor_receive_device(self->udev_mon);
  if (dev) {
    const char *action = udev_device_get_action(dev);
    if (g_strcmp0(action, "add") == 0) {
      handle_udev_add(self, dev);
    } else if (g_strcmp0(action, "remove") == 0) {
      handle_udev_remove(self, dev);
    }
    udev_device_unref(dev);
  }
  return G_SOURCE_CONTINUE;
}

static void conttest_device_manager_init(ConttestDeviceManager *self) {
  self->devices = g_ptr_array_new();
  self->udev = udev_new();
  if (self->udev) {
    self->udev_mon = udev_monitor_new_from_netlink(self->udev, "udev");
    if (self->udev_mon) {
      udev_monitor_filter_add_match_subsystem_devtype(self->udev_mon, "input",
                                                      NULL);
      udev_monitor_enable_receiving(self->udev_mon);

      int fd = udev_monitor_get_fd(self->udev_mon);
      self->udev_channel = g_io_channel_unix_new(fd);
      self->udev_watch_id =
          g_io_add_watch(self->udev_channel, G_IO_IN, on_udev_event, self);
    } else {
      /* Netlink socket blocked (e.g. Flatpak sandbox). Evdev devices will
       * be discovered via re-scan when SDL fires hotplug events. */
      g_info("udev monitor unavailable — evdev hotplug via SDL re-scan");
    }

    // Scan initial evdevs
    struct udev_enumerate *enumerate = udev_enumerate_new(self->udev);
    if (enumerate) {
      udev_enumerate_add_match_subsystem(enumerate, "input");
      udev_enumerate_scan_devices(enumerate);
      struct udev_list_entry *devices, *dev_list_entry;
      devices = udev_enumerate_get_list_entry(enumerate);
      udev_list_entry_foreach(dev_list_entry, devices) {
        const char *path = udev_list_entry_get_name(dev_list_entry);
        struct udev_device *dev =
            udev_device_new_from_syspath(self->udev, path);
        if (dev) {
          handle_udev_add(self, dev);
          udev_device_unref(dev);
        }
      }
      udev_enumerate_unref(enumerate);
    }
  }

  self->sdl_timer_id = g_timeout_add(50, on_sdl_timer, self);
  self->battery_poll_counter = 100; /* trigger immediate poll on first tick */

  // Initial SDL scan
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
  if (self->sdl_timer_id) {
    g_source_remove(self->sdl_timer_id);
    self->sdl_timer_id = 0;
  }
  if (self->udev_watch_id) {
    g_source_remove(self->udev_watch_id);
    self->udev_watch_id = 0;
  }
  if (self->udev_channel) {
    g_io_channel_unref(self->udev_channel);
    self->udev_channel = NULL;
  }
  if (self->udev_mon) {
    udev_monitor_unref(self->udev_mon);
    self->udev_mon = NULL;
  }
  if (self->udev) {
    udev_unref(self->udev);
    self->udev = NULL;
  }
  if (self->devices) {
    for (guint i = 0; i < self->devices->len; i++) {
      conttest_device_free(g_ptr_array_index(self->devices, i));
    }
    g_ptr_array_free(self->devices, TRUE);
    self->devices = NULL;
  }

  G_OBJECT_CLASS(conttest_device_manager_parent_class)->dispose(object);
}

static void
conttest_device_manager_class_init(ConttestDeviceManagerClass *klass) {
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  obj_class->dispose = conttest_device_manager_dispose;

  signals[SIGNAL_DEVICE_ADDED] =
      g_signal_new("device-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_DEVICE_REMOVED] = g_signal_new(
      "device-removed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_DEVICE_UPDATED] = g_signal_new(
      "device-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

ConttestDeviceManager *conttest_device_manager_new(void) {
  return g_object_new(CONTTEST_TYPE_DEVICE_MANAGER, NULL);
}

void conttest_device_manager_foreach(ConttestDeviceManager *self,
                                     ConttestDeviceForeachFunc func,
                                     gpointer user_data) {
  if (!self->devices) return;
  GPtrArray *snapshot = g_ptr_array_new();
  for (guint i = 0; i < self->devices->len; i++) {
    g_ptr_array_add(snapshot, g_ptr_array_index(self->devices, i));
  }
  for (guint i = 0; i < snapshot->len; i++) {
    func((ConttestDevice *)g_ptr_array_index(snapshot, i), user_data);
  }
  g_ptr_array_free(snapshot, TRUE);
}
