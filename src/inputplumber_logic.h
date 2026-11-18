#pragma once

#include "inputplumber.h"
#include <gio/gio.h>

G_BEGIN_DECLS

/* Compute InputPlumber's 64-bit FNV-1a hash of a string (empty/NULL returns basis). */
guint64 conttest_inputplumber_fnv1a_64(const char *str);

/* Format InputPlumber's deterministic UHID uniq string for a 3-byte vendor prefix
 * and PersistentId. */
void conttest_inputplumber_format_mac_uniq(const guint8 prefix[3],
                                           const char *persistent_id,
                                           char *buf, gsize buf_len);

gboolean conttest_inputplumber_target_type_expected_ids(const char *target_type,
                                                        uint16_t *out_vid,
                                                        uint16_t *out_pid);

gboolean conttest_inputplumber_target_type_has_deterministic_uniq(const char *target_type,
                                                                  const char *persistent_id,
                                                                  char *buf, gsize buf_len);

const char *conttest_inputplumber_target_type_virtual_bus(const char *target_type);

gboolean conttest_inputplumber_is_gamepad_target_type(const char *type);

/* Filter D-Bus ObjectManager / Properties signals to the 4 InputPlumber
 * interfaces actually consumed by Haptika so high-rate telemetry or DBusTarget
 * property updates never trigger spurious GetManagedObjects() round-trips.
 * Sets *out_version_invalidated when InputManager properties/interfaces change. */
gboolean conttest_inputplumber_dbus_signal_is_relevant(
    const char *interface_name,
    const char *signal_name,
    GVariant *parameters,
    gboolean *out_version_invalidated);

/* Parse org.freedesktop.DBus.ObjectManager.GetManagedObjects() reply "(a{oa{sa{sv}}})"
 * into ConttestInputPlumberInfo* candidates and an optional set of source devnodes. */
GPtrArray *conttest_inputplumber_parse_managed_objects(GVariant *reply,
                                                       const char *version,
                                                       GHashTable **out_source_devnodes);

/* Match a virtual controller against parsed ConttestInputPlumberInfo* candidates. */
ConttestInputPlumberInfo *conttest_inputplumber_match_candidates(
    GPtrArray *candidates,
    const char *daemon_version,
    const char *name,
    uint16_t vendor_id,
    uint16_t product_id,
    const char *uniq,
    const char *virtual_bus);

G_END_DECLS
