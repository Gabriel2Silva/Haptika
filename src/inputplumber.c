#define _POSIX_C_SOURCE 200809L
#include "inputplumber.h"
#include "inputplumber_logic.h"
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>

#define IP_BUS_NAME        "org.shadowblip.InputPlumber"
#define IP_ROOT_PATH       "/org/shadowblip/InputPlumber"
#define IP_MANAGER_PATH    "/org/shadowblip/InputPlumber/Manager"
#define IP_IFACE_MANAGER   "org.shadowblip.InputManager"
#define IP_IFACE_COMPOSITE "org.shadowblip.Input.CompositeDevice"
#define IP_IFACE_TARGET    "org.shadowblip.Input.Target"
#define IP_IFACE_UDEV      "org.shadowblip.Input.Source.UdevDevice"

/* --- Sysfs / udev virtual & hidden detection --- */

static struct udev_device *resolve_udev_device(struct udev *udev,
                                               const char *syspath,
                                               const char *devnode) {
    if (!udev) return NULL;
    if (syspath) {
        return udev_device_new_from_syspath(udev, syspath);
    }
    if (devnode) {
        struct stat st;
        if (stat(devnode, &st) == 0) {
            return udev_device_new_from_devnum(
                udev, S_ISBLK(st.st_mode) ? 'b' : 'c', st.st_rdev);
        }
    }
    return NULL;
}

gboolean conttest_udev_device_probe_virtual(struct udev_device *dev,
                                            char **out_uniq,
                                            char **out_virtual_bus) {
    if (out_uniq) *out_uniq = NULL;
    if (out_virtual_bus) *out_virtual_bus = NULL;
    if (!dev) return FALSE;

    const char *resolved_syspath = udev_device_get_syspath(dev);
    gboolean is_virtual = resolved_syspath &&
                          g_str_has_prefix(resolved_syspath, "/sys/devices/virtual/");
    if (!is_virtual)
        return FALSE;

    struct udev_device *hid_parent =
        udev_device_get_parent_with_subsystem_devtype(dev, "hid", NULL);
    struct udev_device *input_parent =
        udev_device_get_parent_with_subsystem_devtype(dev, "input", NULL);

    /* BlueZ exposes physical Bluetooth HID devices through /dev/uhid under
     * /sys/devices/virtual/misc/uhid/0005:...; exclude BUS_BLUETOOTH (0005)
     * so real wireless controllers are never treated as synthetic virtual pads. */
    const char *hid_id = hid_parent
        ? udev_device_get_property_value(hid_parent, "HID_ID") : NULL;
    const char *bustype = input_parent
        ? udev_device_get_sysattr_value(input_parent, "id/bustype") : NULL;
    if ((hid_id && g_str_has_prefix(hid_id, "0005:")) ||
        g_strcmp0(bustype, "0005") == 0) {
        return FALSE;
    }

    if (out_virtual_bus) {
        if (hid_parent || strstr(resolved_syspath, "/virtual/misc/uhid/") != NULL) {
            *out_virtual_bus = g_strdup("uhid");
        } else {
            *out_virtual_bus = g_strdup("uinput");
        }
    }

    if (out_uniq) {
        const char *uniq = NULL;
        if (hid_parent) {
            uniq = udev_device_get_property_value(hid_parent, "HID_UNIQ");
        }
        if ((!uniq || !*uniq) && input_parent) {
            uniq = udev_device_get_sysattr_value(input_parent, "uniq");
            if (!uniq || !*uniq)
                uniq = udev_device_get_sysattr_value(input_parent, "id/uniq");
        }
        if (uniq && *uniq) {
            char *trimmed = g_strstrip(g_strdup(uniq));
            if (*trimmed) {
                *out_uniq = trimmed;
            } else {
                g_free(trimmed);
            }
        }
    }

    return TRUE;
}

gboolean conttest_udev_probe_virtual(struct udev *udev,
                                     const char *syspath,
                                     const char *devnode,
                                     char **out_uniq,
                                     char **out_virtual_bus) {
    if (out_uniq) *out_uniq = NULL;
    if (out_virtual_bus) *out_virtual_bus = NULL;

    struct udev_device *dev = resolve_udev_device(udev, syspath, devnode);
    if (!dev) return FALSE;

    gboolean is_virtual = conttest_udev_device_probe_virtual(dev, out_uniq, out_virtual_bus);
    udev_device_unref(dev);
    return is_virtual;
}

gboolean conttest_udev_device_is_inputplumber_hidden(struct udev_device *dev) {
    if (!dev) return FALSE;

    gboolean hidden = FALSE;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_device_get_devlinks_list_entry(dev)) {
        const char *link = udev_list_entry_get_name(entry);
        if (link && g_str_has_prefix(link, "/dev/inputplumber/by-hidden/")) {
            hidden = TRUE;
            break;
        }
    }
    return hidden;
}

gboolean conttest_udev_is_inputplumber_hidden(struct udev *udev,
                                              const char *syspath,
                                              const char *devnode) {
    struct udev_device *dev = resolve_udev_device(udev, syspath, devnode);
    if (!dev) return FALSE;

    gboolean hidden = conttest_udev_device_is_inputplumber_hidden(dev);
    udev_device_unref(dev);
    return hidden;
}

/* --- Deterministic InputPlumber ID helpers --- */

/* --- D-Bus Monitor GObject --- */

struct _ConttestInputPlumberMonitor {
    GObject parent_instance;

    GDBusConnection *bus;
    GCancellable *cancellable;
    guint sub_name_owner;
    guint sub_iface_added;
    guint sub_iface_removed;
    guint sub_props_changed;
    guint refresh_debounce_id;

    guint refresh_seq;
    guint pending_calls;
    gboolean pending_version_ok;
    gboolean pending_objects_ok;
    char *pending_version;
    GPtrArray *pending_candidates;
    GHashTable *pending_source_devnodes;

    char *daemon_version;
    GPtrArray *candidates; /* element-type: ConttestInputPlumberInfo* */
    GHashTable *source_devnodes; /* set of active CompositeDevice SourceDevicePaths */
    gboolean disposed;
};

static GWeakRef *monitor_weak_ref_new(ConttestInputPlumberMonitor *self) {
    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, self);
    return wr;
}

static void monitor_weak_ref_free(gpointer data) {
    GWeakRef *wr = data;
    g_weak_ref_clear(wr);
    g_free(wr);
}

enum {
    SIGNAL_CHANGED,
    LAST_SIGNAL
};

static guint monitor_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE(ConttestInputPlumberMonitor, conttest_inputplumber_monitor, G_TYPE_OBJECT)

static void start_refresh(ConttestInputPlumberMonitor *self);

static gboolean on_debounce_timeout(gpointer user_data) {
    GWeakRef *wr = user_data;
    g_autoptr(ConttestInputPlumberMonitor) self = g_weak_ref_get(wr);
    if (!self || self->disposed) {
        return G_SOURCE_REMOVE;
    }
    self->refresh_debounce_id = 0;
    start_refresh(self);
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(ConttestInputPlumberMonitor *self) {
    if (!self || self->disposed || !self->bus) return;
    if (self->refresh_debounce_id) return;
    self->refresh_debounce_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 50, on_debounce_timeout,
        monitor_weak_ref_new(self), monitor_weak_ref_free);
}

static void cancel_pending_refresh(ConttestInputPlumberMonitor *self) {
    self->refresh_seq++;
    self->pending_calls = 0;
    self->pending_version_ok = FALSE;
    self->pending_objects_ok = FALSE;
    if (self->refresh_debounce_id) {
        g_source_remove(self->refresh_debounce_id);
        self->refresh_debounce_id = 0;
    }
    g_clear_pointer(&self->pending_version, g_free);
    g_clear_pointer(&self->pending_candidates, g_ptr_array_unref);
    g_clear_pointer(&self->pending_source_devnodes, g_hash_table_destroy);
}

static void clear_snapshot(ConttestInputPlumberMonitor *self) {
    if (!self || self->disposed) return;
    gboolean had_data = (self->daemon_version != NULL) ||
                        (self->candidates && self->candidates->len > 0) ||
                        (self->source_devnodes && g_hash_table_size(self->source_devnodes) > 0);
    g_clear_pointer(&self->daemon_version, g_free);
    if (self->candidates) {
        g_ptr_array_set_size(self->candidates, 0);
    }
    if (self->source_devnodes) {
        g_hash_table_remove_all(self->source_devnodes);
    }
    if (had_data) {
        g_signal_emit(self, monitor_signals[SIGNAL_CHANGED], 0);
    }
}

typedef struct {
    GWeakRef monitor_ref;
    guint seq;
} RefreshCallCtx;

static void backfill_daemon_version(GPtrArray *candidates, const char *version) {
    if (!version || !candidates) return;
    for (guint i = 0; i < candidates->len; i++) {
        ConttestInputPlumberInfo *info = g_ptr_array_index(candidates, i);
        if (g_strcmp0(info->daemon_version, version) != 0) {
            g_free(info->daemon_version);
            info->daemon_version = g_strdup(version);
        }
    }
}

static gboolean candidates_equal(const GPtrArray *a, const GPtrArray *b) {
    guint len_a = a ? a->len : 0;
    guint len_b = b ? b->len : 0;
    if (len_a != len_b) return FALSE;
    for (guint i = 0; i < len_a; i++) {
        const ConttestInputPlumberInfo *ia = g_ptr_array_index((GPtrArray *)a, i);
        const ConttestInputPlumberInfo *ib = g_ptr_array_index((GPtrArray *)b, i);
        if (!conttest_inputplumber_info_equal(ia, ib)) return FALSE;
    }
    return TRUE;
}

static gboolean str_set_equal(GHashTable *a, GHashTable *b) {
    guint size_a = a ? g_hash_table_size(a) : 0;
    guint size_b = b ? g_hash_table_size(b) : 0;
    if (size_a != size_b) return FALSE;
    if (size_a == 0) return TRUE;
    GHashTableIter iter;
    gpointer key = NULL;
    g_hash_table_iter_init(&iter, a);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!g_hash_table_contains(b, key)) return FALSE;
    }
    return TRUE;
}

static void finish_refresh_if_complete(ConttestInputPlumberMonitor *self) {
    if (!self || self->disposed || self->pending_calls > 0) return;

    gboolean changed = FALSE;

    if (self->pending_version_ok) {
        if (g_strcmp0(self->daemon_version, self->pending_version) != 0) {
            changed = TRUE;
        }
        g_free(self->daemon_version);
        self->daemon_version = self->pending_version;
        self->pending_version = NULL;
    } else {
        g_clear_pointer(&self->pending_version, g_free);
    }

    if (self->pending_objects_ok) {
        if (!self->pending_candidates) {
            self->pending_candidates = g_ptr_array_new_with_free_func(
                (GDestroyNotify)conttest_inputplumber_info_free);
        }
        if (!self->pending_source_devnodes) {
            self->pending_source_devnodes = g_hash_table_new_full(
                g_str_hash, g_str_equal, g_free, NULL);
        }

        /* Populate daemon_version before comparing so arrival order between
         * Get("Version") and GetManagedObjects never triggers spurious diffs. */
        backfill_daemon_version(self->pending_candidates, self->daemon_version);

        if (!candidates_equal(self->candidates, self->pending_candidates) ||
            !str_set_equal(self->source_devnodes, self->pending_source_devnodes)) {
            changed = TRUE;
        }

        if (self->candidates) g_ptr_array_free(self->candidates, TRUE);
        self->candidates = self->pending_candidates;
        self->pending_candidates = NULL;

        if (self->source_devnodes) g_hash_table_destroy(self->source_devnodes);
        self->source_devnodes = self->pending_source_devnodes;
        self->pending_source_devnodes = NULL;
    } else {
        /* Preserve cached candidates and source_devnodes across transient D-Bus errors;
         * daemon exit is handled explicitly by NameOwnerChanged -> clear_snapshot(). */
        g_clear_pointer(&self->pending_candidates, g_ptr_array_unref);
        g_clear_pointer(&self->pending_source_devnodes, g_hash_table_destroy);
        backfill_daemon_version(self->candidates, self->daemon_version);
    }

    if (changed) {
        g_signal_emit(self, monitor_signals[SIGNAL_CHANGED], 0);
    }
}

static void on_get_version_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    RefreshCallCtx *ctx = user_data;
    g_autoptr(ConttestInputPlumberMonitor) self = g_weak_ref_get(&ctx->monitor_ref);
    guint seq = ctx->seq;
    g_weak_ref_clear(&ctx->monitor_ref);
    g_free(ctx);

    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (!self || self->disposed || seq != self->refresh_seq) {
        if (reply) g_variant_unref(reply);
        g_clear_error(&err);
        return;
    }

    if (reply) {
        GVariant *inner = NULL;
        g_variant_get(reply, "(v)", &inner);
        if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)) {
            g_free(self->pending_version);
            self->pending_version = g_variant_dup_string(inner, NULL);
            self->pending_version_ok = TRUE;
        }
        if (inner) g_variant_unref(inner);
        g_variant_unref(reply);
    } else {
        g_clear_error(&err);
    }

    if (self->pending_calls > 0) self->pending_calls--;
    finish_refresh_if_complete(self);
}

static void on_get_objects_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    RefreshCallCtx *ctx = user_data;
    g_autoptr(ConttestInputPlumberMonitor) self = g_weak_ref_get(&ctx->monitor_ref);
    guint seq = ctx->seq;
    g_weak_ref_clear(&ctx->monitor_ref);
    g_free(ctx);

    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (!self || self->disposed || seq != self->refresh_seq) {
        if (reply) g_variant_unref(reply);
        g_clear_error(&err);
        return;
    }

    if (reply) {
        if (self->pending_candidates) g_ptr_array_free(self->pending_candidates, TRUE);
        g_clear_pointer(&self->pending_source_devnodes, g_hash_table_destroy);
        self->pending_candidates = conttest_inputplumber_parse_managed_objects(
            reply, self->pending_version, &self->pending_source_devnodes);
        self->pending_objects_ok = TRUE;
        g_variant_unref(reply);
    } else {
        gboolean no_owner = err &&
            (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
             g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN));
        g_clear_error(&err);
        g_clear_pointer(&self->pending_candidates, g_ptr_array_unref);
        g_clear_pointer(&self->pending_source_devnodes, g_hash_table_destroy);
        if (no_owner) {
            cancel_pending_refresh(self);
            clear_snapshot(self);
            return;
        }
    }

    if (self->pending_calls > 0) self->pending_calls--;
    finish_refresh_if_complete(self);
}

static void start_refresh(ConttestInputPlumberMonitor *self) {
    if (!self || self->disposed || !self->bus || !self->cancellable) return;

    self->refresh_seq++;
    self->pending_calls = 2;
    self->pending_version_ok = FALSE;
    self->pending_objects_ok = FALSE;
    g_clear_pointer(&self->pending_version, g_free);
    g_clear_pointer(&self->pending_candidates, g_ptr_array_unref);
    g_clear_pointer(&self->pending_source_devnodes, g_hash_table_destroy);

    RefreshCallCtx *vctx = g_new0(RefreshCallCtx, 1);
    g_weak_ref_init(&vctx->monitor_ref, self);
    vctx->seq = self->refresh_seq;
    g_dbus_connection_call(
        self->bus,
        IP_BUS_NAME,
        IP_MANAGER_PATH,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", IP_IFACE_MANAGER, "Version"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        2000,
        self->cancellable,
        on_get_version_done,
        vctx);

    RefreshCallCtx *octx = g_new0(RefreshCallCtx, 1);
    g_weak_ref_init(&octx->monitor_ref, self);
    octx->seq = self->refresh_seq;
    g_dbus_connection_call(
        self->bus,
        IP_BUS_NAME,
        IP_ROOT_PATH,
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        2000,
        self->cancellable,
        on_get_objects_done,
        octx);
}

static void on_name_owner_changed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data) {
    (void)conn; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    GWeakRef *wr = user_data;
    g_autoptr(ConttestInputPlumberMonitor) self = g_weak_ref_get(wr);
    if (!self || self->disposed) return;

    if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)")))
        return;

    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (new_owner && *new_owner) {
        schedule_refresh(self);
    } else {
        cancel_pending_refresh(self);
        clear_snapshot(self);
    }
}

static void on_ip_dbus_signal(GDBusConnection *conn,
                              const gchar *sender_name,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *signal_name,
                              GVariant *parameters,
                              gpointer user_data) {
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name; (void)parameters;
    GWeakRef *wr = user_data;
    g_autoptr(ConttestInputPlumberMonitor) self = g_weak_ref_get(wr);
    if (!self || self->disposed) return;

    schedule_refresh(self);
}

static void on_bus_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    GWeakRef *wr = user_data;
    g_autoptr(ConttestInputPlumberMonitor) self = g_weak_ref_get(wr);
    monitor_weak_ref_free(wr);

    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_finish(res, &err);
    if (!self || self->disposed || !bus) {
        if (bus) g_object_unref(bus);
        g_clear_error(&err);
        return;
    }

    self->bus = bus;

    self->sub_name_owner = g_dbus_connection_signal_subscribe(
        bus,
        "org.freedesktop.DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        "/org/freedesktop/DBus",
        IP_BUS_NAME,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_name_owner_changed,
        monitor_weak_ref_new(self),
        monitor_weak_ref_free);

    self->sub_iface_added = g_dbus_connection_signal_subscribe(
        bus,
        IP_BUS_NAME,
        "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded",
        IP_ROOT_PATH,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_ip_dbus_signal,
        monitor_weak_ref_new(self),
        monitor_weak_ref_free);

    self->sub_iface_removed = g_dbus_connection_signal_subscribe(
        bus,
        IP_BUS_NAME,
        "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved",
        IP_ROOT_PATH,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_ip_dbus_signal,
        monitor_weak_ref_new(self),
        monitor_weak_ref_free);

    self->sub_props_changed = g_dbus_connection_signal_subscribe(
        bus,
        IP_BUS_NAME,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        NULL,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_ip_dbus_signal,
        monitor_weak_ref_new(self),
        monitor_weak_ref_free);

    start_refresh(self);
}

static void conttest_inputplumber_monitor_dispose(GObject *object) {
    ConttestInputPlumberMonitor *self = CONTTEST_INPUTPLUMBER_MONITOR(object);

    self->disposed = TRUE;
    cancel_pending_refresh(self);

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    if (self->bus) {
        if (self->sub_name_owner)
            g_dbus_connection_signal_unsubscribe(self->bus, self->sub_name_owner);
        if (self->sub_iface_added)
            g_dbus_connection_signal_unsubscribe(self->bus, self->sub_iface_added);
        if (self->sub_iface_removed)
            g_dbus_connection_signal_unsubscribe(self->bus, self->sub_iface_removed);
        if (self->sub_props_changed)
            g_dbus_connection_signal_unsubscribe(self->bus, self->sub_props_changed);
        self->sub_name_owner = self->sub_iface_added = self->sub_iface_removed = self->sub_props_changed = 0;
        g_clear_object(&self->bus);
    }

    g_clear_pointer(&self->daemon_version, g_free);
    g_clear_pointer(&self->candidates, g_ptr_array_unref);
    g_clear_pointer(&self->source_devnodes, g_hash_table_destroy);

    G_OBJECT_CLASS(conttest_inputplumber_monitor_parent_class)->dispose(object);
}

static void conttest_inputplumber_monitor_class_init(ConttestInputPlumberMonitorClass *klass) {
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->dispose = conttest_inputplumber_monitor_dispose;

    monitor_signals[SIGNAL_CHANGED] =
        g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void conttest_inputplumber_monitor_init(ConttestInputPlumberMonitor *self) {
    self->cancellable = g_cancellable_new();
    self->candidates = g_ptr_array_new_with_free_func((GDestroyNotify)conttest_inputplumber_info_free);
    self->source_devnodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_bus_get(G_BUS_TYPE_SYSTEM, self->cancellable, on_bus_ready, monitor_weak_ref_new(self));
}

ConttestInputPlumberMonitor *conttest_inputplumber_monitor_new(void) {
    return g_object_new(CONTTEST_TYPE_INPUTPLUMBER_MONITOR, NULL);
}

ConttestInputPlumberInfo *conttest_inputplumber_monitor_match(
    ConttestInputPlumberMonitor *self,
    const char *name,
    uint16_t vendor_id,
    uint16_t product_id,
    const char *uniq,
    const char *virtual_bus) {
    if (!self || self->disposed || !self->candidates) return NULL;
    return conttest_inputplumber_match_candidates(self->candidates, self->daemon_version,
                                                  name, vendor_id, product_id,
                                                  uniq, virtual_bus);
}

gboolean conttest_inputplumber_monitor_is_source_devnode(
    ConttestInputPlumberMonitor *self,
    const char *devnode) {
    if (!self || self->disposed || !self->source_devnodes || !devnode || !*devnode) return FALSE;
    return g_hash_table_contains(self->source_devnodes, devnode);
}
