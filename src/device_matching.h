#pragma once

#include "device_manager.h"
#include <libudev.h>

G_BEGIN_DECLS

gboolean conttest_vid_pid_is_ds3(uint16_t vid, uint16_t pid);
gboolean conttest_vid_pid_is_ds4(uint16_t vid, uint16_t pid);
gboolean conttest_vid_pid_is_ds5(uint16_t vid, uint16_t pid);

/* Map a raw SDL joystick axis index beyond the standard gamepad axes to a
 * DualShock 3 pressure channel (0..9: Cross, Circle, Square, Triangle, L1, R1,
 * Up, Down, Left, Right). Returns -1 for other hardware and axis indices. */
int conttest_ds3_pressure_axis_index(uint16_t vid, uint16_t pid, int axis_index);

/* Parse VID and PID out of sysfs PRODUCT ("3/54c/ce6/8111") or HID_ID ("0005:0000054C:000009CC"). */
gboolean conttest_parse_vid_pid(const char *str, char sep, uint16_t *v, uint16_t *p);

gboolean conttest_extract_udev_vid_pid(struct udev_device *dev,
                                       uint16_t *vid,
                                       uint16_t *pid);

/* Pure predicate deciding whether an ancestor (subsystem, devtype) in a
 * bottom-up udev parent walk qualifies as a per-controller hardware key.
 * Updates *inout_saw_bluetooth so shared Bluetooth host adapters (DEVTYPE=host)
 * and their parent USB Bluetooth dongle interfaces are never collected. */
gboolean conttest_udev_ancestor_is_hw_key(const char *subsystem,
                                          const char *devtype,
                                          gboolean *inout_saw_bluetooth);

/* Collect qualifying ancestor hardware keys (hid, bluetooth link, input,
 * non-Bluetooth usb_interface) from an already-opened udev device, plus its own
 * syspath in out_own_syspath. Caller frees returned strv with g_strfreev() and
 * *out_own_syspath with g_free(). */
char **conttest_udev_collect_hw_keys(struct udev_device *dev,
                                     char **out_own_syspath);

/* Check whether two (own_syspath, hw_keys) pairs describe the same physical
 * hardware purely in memory. */
gboolean conttest_hw_keys_match(const char *own_a,
                                const char * const *keys_a,
                                const char *own_b,
                                const char * const *keys_b);

/* Select the single candidate a hardware-key match may safely identify.
 * `matches` marks the entries whose keys matched and `preferred` marks the
 * ones still believed live (may be NULL). A live entry only wins when it is
 * the only live match; stale entries are used only when no live entry matched
 * and exactly one stale entry did. Returns the index or -1 when the best class
 * is ambiguous. Shared keys (USB receivers, multi-port adapters) must refuse
 * the merge instead of picking the first entry. */
int conttest_select_unique_candidate(const gboolean *matches,
                                     const gboolean *preferred,
                                     guint count);

typedef enum {
    CONTTEST_MATCH_REJECT = 0,
    CONTTEST_MATCH_WEAK,
    CONTTEST_MATCH_STRONG,
} ConttestCandidateMatch;

gboolean conttest_serial_is_usable(const char *serial);
gboolean conttest_serial_matches(const char *a, const char *b);
gboolean conttest_conn_contradicts(SDL_JoystickConnectionState a,
                                   SDL_JoystickConnectionState b);
ConttestCandidateMatch conttest_classify_candidate_pair(
    const char *serial_a,
    SDL_JoystickConnectionState conn_a,
    const char *serial_b,
    SDL_JoystickConnectionState conn_b);

/* Pure classification of Linux input bustype hex string + parent subsystem flags. */
SDL_JoystickConnectionState conttest_resolve_connection_state_from_props(
    const char *bustype_str,
    gboolean has_usb_parent,
    gboolean has_bt_parent);

/* Resolve connection type from an already-opened udev device. */
SDL_JoystickConnectionState conttest_resolve_connection_state_for_udev_device(
    struct udev_device *dev);

G_END_DECLS
