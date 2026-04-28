#pragma once
#include <SDL3/SDL.h>
#include <glib-object.h>

G_BEGIN_DECLS

/* ConttestDevice is opaque; fields are accessed through read-only accessors. */
typedef struct _ConttestDevice ConttestDevice;

const char *conttest_device_get_name(const ConttestDevice *device);
gboolean conttest_device_get_has_sdl(const ConttestDevice *device);
SDL_JoystickID conttest_device_get_sdl_id(const ConttestDevice *device);
gboolean conttest_device_get_has_evdev(const ConttestDevice *device);
const char *conttest_device_get_evdev_syspath(const ConttestDevice *device);
SDL_PowerState conttest_device_get_battery_state(const ConttestDevice *device);
int conttest_device_get_battery_percent(const ConttestDevice *device);
SDL_JoystickConnectionState
conttest_device_get_connection_state(const ConttestDevice *device);
uint16_t conttest_device_get_vendor_id(const ConttestDevice *device);
uint16_t conttest_device_get_product_id(const ConttestDevice *device);

/* ── Sony VID/PID constants ────────────────────────────────────────────── */
#define SONY_VID      0x054c
#define DS3_PID       0x0268
#define DS4_PID_V1    0x05c4  /* CUH-ZCT1x (first gen)   */
#define DS4_PID_V2    0x09cc  /* CUH-ZCT2x (second gen)  */
#define DS4_PID_BT    0x0ba0  /* USB-BT dongle adapter   */
#define DS5_PID_V1    0x0ce6  /* CFI-ZCT1x               */
#define DS5_PID_V2    0x0df2  /* CFI-ZCP1 (DualSense Edge) */

/* Convenience predicates — check VID+PID via the device accessors. */
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
 * Iterate over a snapshot of all currently known devices. The callback
 * receives a borrowed pointer to each manager-owned ConttestDevice. Pointers
 * remain valid for the duration of the foreach call, but must not be retained
 * after it returns: the manager may free or replace device objects at any time
 * in response to hotplug events.
 */
void conttest_device_manager_foreach(ConttestDeviceManager *self,
                                     ConttestDeviceForeachFunc func,
                                     gpointer user_data);

G_END_DECLS
