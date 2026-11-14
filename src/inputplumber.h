#pragma once

#include <glib-object.h>
#include <libudev.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef enum {
    CONTTEST_VIRTUAL_NONE = 0,
    CONTTEST_VIRTUAL_GENERIC,
    CONTTEST_VIRTUAL_INPUTPLUMBER
} ConttestVirtualKind;

typedef struct {
    char *name;
    char *device_path;
    char *phys_path;
    char *vendor_id;
    char *product_id;
    char *subsystem;
} ConttestInputPlumberSource;

typedef struct {
    char *daemon_version;
    char *composite_path;
    char *composite_name;
    char *persistent_id;
    char *profile_name;
    char *profile_path;
    guint32 intercept_mode;
    char *target_type;
    char *target_name;
    char *target_uniq;
    char *virtual_bus;
    GPtrArray *output_caps; /* element-type: char* */
    GPtrArray *sources;     /* element-type: ConttestInputPlumberSource* */
} ConttestInputPlumberInfo;

ConttestInputPlumberInfo *conttest_inputplumber_info_copy(const ConttestInputPlumberInfo *info);
void conttest_inputplumber_info_free(ConttestInputPlumberInfo *info);
gboolean conttest_inputplumber_info_equal(const ConttestInputPlumberInfo *a,
                                          const ConttestInputPlumberInfo *b);

/* Check whether an already-opened udev device lives under
 * /sys/devices/virtual/ and is not a physical Bluetooth HID bridge. Extracts
 * the virtual bus ("uhid" or "uinput") and unique ID (HID_UNIQ / uniq)
 * when available. */
gboolean conttest_udev_device_probe_virtual(struct udev_device *dev,
                                            char **out_uniq,
                                            char **out_virtual_bus);

/* Convenience wrapper opening syspath or devnode via udev. Performs
 * synchronous udev/sysfs lookups; call only from a worker thread or unit tests. */
gboolean conttest_udev_probe_virtual(struct udev *udev,
                                     const char *syspath,
                                     const char *devnode,
                                     char **out_uniq,
                                     char **out_virtual_bus);

/* Returns TRUE if an already-opened udev device carries an InputPlumber
 * hidden devlink (/dev/inputplumber/by-hidden/...). */
gboolean conttest_udev_device_is_inputplumber_hidden(struct udev_device *dev);

/* Convenience wrapper opening syspath or devnode via udev. Performs
 * synchronous udev/sysfs lookups; call only from a worker thread or unit tests. */
gboolean conttest_udev_is_inputplumber_hidden(struct udev *udev,
                                              const char *syspath,
                                              const char *devnode);

#define CONTTEST_TYPE_INPUTPLUMBER_MONITOR (conttest_inputplumber_monitor_get_type())
G_DECLARE_FINAL_TYPE(ConttestInputPlumberMonitor, conttest_inputplumber_monitor,
                     CONTTEST, INPUTPLUMBER_MONITOR, GObject)

ConttestInputPlumberMonitor *conttest_inputplumber_monitor_new(void);

/* Match a virtual controller against the latest InputPlumber D-Bus snapshot.
 * Returns a newly-allocated ConttestInputPlumberInfo* or NULL if no match. */
ConttestInputPlumberInfo *conttest_inputplumber_monitor_match(
    ConttestInputPlumberMonitor *self,
    const char *name,
    uint16_t vendor_id,
    uint16_t product_id,
    const char *uniq,
    const char *virtual_bus);

/* Returns TRUE if devnode (e.g. "/dev/input/event23") is currently listed in
 * any active InputPlumber CompositeDevice.SourceDevicePaths. */
gboolean conttest_inputplumber_monitor_is_source_devnode(
    ConttestInputPlumberMonitor *self,
    const char *devnode);

G_END_DECLS
