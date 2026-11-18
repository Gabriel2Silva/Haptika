#pragma once
#include <SDL3/SDL.h>
#include <glib-object.h>
#include "inputplumber.h"

G_BEGIN_DECLS

/* ConttestDevice is opaque; fields are accessed through read-only accessors. */
#define CONTTEST_TYPE_DEVICE (conttest_device_get_type())
G_DECLARE_FINAL_TYPE(ConttestDevice, conttest_device, CONTTEST, DEVICE, GObject)

const char *conttest_device_get_name(const ConttestDevice *device);
gboolean conttest_device_get_has_sdl(const ConttestDevice *device);
SDL_JoystickID conttest_device_get_sdl_id(const ConttestDevice *device);
gboolean conttest_device_get_has_evdev(const ConttestDevice *device);
const char *conttest_device_get_evdev_syspath(const ConttestDevice *device);
const char *conttest_device_get_evdev_devnode(const ConttestDevice *device);
SDL_PowerState conttest_device_get_battery_state(const ConttestDevice *device);
int conttest_device_get_battery_percent(const ConttestDevice *device);
SDL_JoystickConnectionState
conttest_device_get_connection_state(const ConttestDevice *device);
uint16_t conttest_device_get_vendor_id(const ConttestDevice *device);
uint16_t conttest_device_get_product_id(const ConttestDevice *device);
ConttestVirtualKind conttest_device_get_virtual_kind(const ConttestDevice *device);
gboolean conttest_device_is_virtual(const ConttestDevice *device);
const ConttestInputPlumberInfo *conttest_device_get_inputplumber_info(const ConttestDevice *device);

/* --- Sony VID/PID constants --- */
#define SONY_VID      0x054c
#define DS3_PID       0x0268
#define DS4_PID_V1    0x05c4  /* CUH-ZCT1x (first gen)   */
#define DS4_PID_V2    0x09cc  /* CUH-ZCT2x (second gen)  */
#define DS4_PID_BT    0x0ba0  /* USB-BT dongle adapter   */
#define DS5_PID_V1    0x0ce6  /* CFI-ZCT1x               */
#define DS5_PID_V2    0x0df2  /* CFI-ZCP1 (DualSense Edge) */

/* Convenience predicates: check VID+PID via the device accessors. */
gboolean conttest_device_is_ds3(const ConttestDevice *device);
gboolean conttest_device_is_ds4(const ConttestDevice *device);
gboolean conttest_device_is_ds5(const ConttestDevice *device);


#define CONTTEST_TYPE_DEVICE_MANAGER (conttest_device_manager_get_type())
G_DECLARE_FINAL_TYPE(ConttestDeviceManager, conttest_device_manager, CONTTEST,
                     DEVICE_MANAGER, GObject)

ConttestDeviceManager *conttest_device_manager_new(void);

typedef void (*ConttestDeviceForeachFunc)(ConttestDevice *device,
                                          gpointer user_data);

/*
 * Iterate over a snapshot of all currently known visible devices. Callers that
 * need a ConttestDevice after the callback returns may take a reference with
 * g_object_ref().
 */
void conttest_device_manager_foreach(ConttestDeviceManager *self,
                                     ConttestDeviceForeachFunc func,
                                     gpointer user_data);

/* Disable unused high-frequency SDL input event types (axis, button, touchpad,
 * sensor, update-complete) while preserving hotplug, remap, and battery events.
 * Haptika samples SDL state directly at 1 kHz, so suppressing unused per-sample
 * queue events avoids thousands of mutex-locked SDL_Event queue operations/sec. */
void conttest_sdl_configure_events(void);

/* Pump the SDL event queue and record the pump timestamp so periodic
 * hotplug timers can skip redundant back-to-back pumps on active frames. */
void conttest_sdl_pump_events(void);
void conttest_sdl_pump_events_at(gint64 monotonic_now_us);

G_END_DECLS
