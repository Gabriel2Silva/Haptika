/*
 * InputPlumber monitor recovery test.
 *
 * Starts a private D-Bus daemon with a fake InputPlumber service, points the
 * monitor at it through DBUS_SYSTEM_BUS_ADDRESS, then kills and restarts the
 * daemon. The monitor must clear its snapshot on connection loss and rebuild
 * it on its own once the bus and the service are back.
 *
 * Skips when dbus-daemon is unavailable.
 */
#define _POSIX_C_SOURCE 200809L
#include "inputplumber.h"
#include "inputplumber_logic.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define IP_NAME "org.shadowblip.InputPlumber"

static gchar *bus_dir;
static gchar *bus_socket;
static gchar *bus_address;
static GPid bus_pid = 0;

typedef struct {
    GDBusConnection *conn;
    guint props_id;
    guint om_id;
    guint owner_id;
} FakeService;

static void child_exited(GPid pid, gint status, gpointer user_data) {
    (void)status;
    (void)user_data;
    g_spawn_close_pid(pid);
}

static void stop_bus(void) {
    if (bus_pid <= 0)
        return;
    kill(bus_pid, SIGTERM);
    for (int i = 0; i < 200; i++) {
        if (!g_file_test(bus_socket, G_FILE_TEST_EXISTS))
            break;
        g_usleep(5 * 1000);
    }
    g_unlink(bus_socket);
    bus_pid = 0;
}

static gboolean start_bus(void) {
    stop_bus();
    g_unlink(bus_socket);

    gchar *argv[] = {
        (gchar *)"dbus-daemon", (gchar *)"--session",
        (gchar *)"--address", bus_address, (gchar *)"--nofork", NULL,
    };
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL,
                       G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                       NULL, NULL, &bus_pid, &err)) {
        g_clear_error(&err);
        bus_pid = 0;
        return FALSE;
    }
    g_child_watch_add(bus_pid, child_exited, NULL);

    for (int i = 0; i < 200; i++) {
        if (g_file_test(bus_socket, G_FILE_TEST_EXISTS))
            return TRUE;
        g_usleep(10 * 1000);
    }
    return FALSE;
}

static gboolean complete_delayed_version(gpointer user_data) {
    GDBusMethodInvocation *invocation = user_data;
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(v)", g_variant_new_string("0.84.0")));
    return G_SOURCE_REMOVE;
}

/* Hold the version reply back so GetManagedObjects finishes first; the monitor
 * must still merge both results into one complete snapshot. */
static void on_props_get(GDBusConnection *conn, const gchar *sender,
                         const gchar *path, const gchar *iface,
                         const gchar *method, GVariant *params,
                         GDBusMethodInvocation *invocation, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)method;
    (void)params; (void)data;
    g_timeout_add_full(G_PRIORITY_DEFAULT, 250, complete_delayed_version,
                       g_object_ref(invocation), g_object_unref);
}

static GVariant *build_managed_objects(void) {
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

    GVariantBuilder props;
    GVariantBuilder ifaces;

    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "Name", g_variant_new_string("Sony DualSense"));
    g_variant_builder_add(&props, "{sv}", "DevicePath", g_variant_new_string("/dev/input/event99"));
    g_variant_builder_add(&props, "{sv}", "IdVendor", g_variant_new_string("054c"));
    g_variant_builder_add(&props, "{sv}", "IdProduct", g_variant_new_string("0ce6"));
    g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&ifaces, "{s@a{sv}}",
                          "org.shadowblip.Input.Source.UdevDevice",
                          g_variant_builder_end(&props));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}",
                          "/org/shadowblip/Input/Source/udev0",
                          g_variant_builder_end(&ifaces));

    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "DeviceType", g_variant_new_string("ds5"));
    g_variant_builder_add(&props, "{sv}", "Name", g_variant_new_string("DualSense"));
    g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&ifaces, "{s@a{sv}}", "org.shadowblip.Input.Target",
                          g_variant_builder_end(&props));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}",
                          "/org/shadowblip/Input/Target/ds5_0",
                          g_variant_builder_end(&ifaces));

    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "Name", g_variant_new_string("DualSense Pad"));
    g_variant_builder_add(&props, "{sv}", "PersistentId", g_variant_new_string("persist_ds5_0"));
    g_variant_builder_add(&props, "{sv}", "ProfileName", g_variant_new_string("Default"));
    const char *sources[] = { "/dev/input/event99", NULL };
    const char *targets[] = { "/org/shadowblip/Input/Target/ds5_0", NULL };
    const char *caps[] = { "Gamepad:Button:South", NULL };
    g_variant_builder_add(&props, "{sv}", "SourceDevicePaths",
                          g_variant_new_strv(sources, -1));
    g_variant_builder_add(&props, "{sv}", "TargetDevices",
                          g_variant_new_strv(targets, -1));
    g_variant_builder_add(&props, "{sv}", "OutputCapabilities",
                          g_variant_new_strv(caps, -1));
    g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&ifaces, "{s@a{sv}}",
                          "org.shadowblip.Input.CompositeDevice",
                          g_variant_builder_end(&props));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}",
                          "/org/shadowblip/Input/CompositeDevice0",
                          g_variant_builder_end(&ifaces));

    return g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects));
}

static void on_get_managed_objects(GDBusConnection *conn, const gchar *sender,
                                   const gchar *path, const gchar *iface,
                                   const gchar *method, GVariant *params,
                                   GDBusMethodInvocation *invocation,
                                   gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)method;
    (void)params; (void)data;
    g_dbus_method_invocation_return_value(invocation, build_managed_objects());
}

static const char props_xml[] =
    "<node><interface name='org.freedesktop.DBus.Properties'>"
    "<method name='Get'>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='v' direction='out'/>"
    "</method></interface></node>";

static const char object_manager_xml[] =
    "<node><interface name='org.freedesktop.DBus.ObjectManager'>"
    "<method name='GetManagedObjects'>"
    "<arg type='a{oa{sa{sv}}}' direction='out'/>"
    "</method></interface></node>";

static void fake_stop(FakeService *fake) {
    if (fake->owner_id)
        g_bus_unown_name(fake->owner_id);
    if (fake->props_id)
        g_dbus_connection_unregister_object(fake->conn, fake->props_id);
    if (fake->om_id)
        g_dbus_connection_unregister_object(fake->conn, fake->om_id);
    g_clear_object(&fake->conn);
    memset(fake, 0, sizeof(*fake));
}

static gboolean fake_start(FakeService *fake) {
    GError *err = NULL;
    fake->conn = g_dbus_connection_new_for_address_sync(
        bus_address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    if (!fake->conn) {
        g_test_message("fake service connect failed: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    static const GDBusInterfaceVTable props_vtable = {
        on_props_get, NULL, NULL, { NULL }
    };
    static const GDBusInterfaceVTable om_vtable = {
        on_get_managed_objects, NULL, NULL, { NULL }
    };

    GDBusNodeInfo *props_node = g_dbus_node_info_new_for_xml(props_xml, NULL);
    GDBusNodeInfo *om_node = g_dbus_node_info_new_for_xml(object_manager_xml, NULL);
    fake->props_id = g_dbus_connection_register_object(
        fake->conn, "/org/shadowblip/InputPlumber/Manager",
        props_node->interfaces[0], &props_vtable, NULL, NULL, NULL);
    fake->om_id = g_dbus_connection_register_object(
        fake->conn, "/org/shadowblip/InputPlumber",
        om_node->interfaces[0], &om_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(props_node);
    g_dbus_node_info_unref(om_node);

    fake->owner_id = g_bus_own_name_on_connection(
        fake->conn, IP_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);

    return fake->props_id != 0 && fake->om_id != 0;
}

static gboolean wait_candidate(ConttestInputPlumberMonitor *monitor,
                               const char *uniq, guint timeout_ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    do {
        ConttestInputPlumberInfo *info = conttest_inputplumber_monitor_match(
            monitor, NULL, 0x054c, 0x0ce6, uniq, "uhid");
        if (info) {
            /* The version reply is delayed past the object list; the snapshot
             * must still carry the daemon version afterwards. */
            gboolean version_ok = g_strcmp0(info->daemon_version, "0.84.0") == 0;
            conttest_inputplumber_info_free(info);
            if (!version_ok)
                return FALSE;
            return TRUE;
        }
        g_main_context_iteration(NULL, FALSE);
        g_usleep(10 * 1000);
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

static gboolean wait_no_candidate(ConttestInputPlumberMonitor *monitor,
                                  const char *uniq, guint timeout_ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    do {
        ConttestInputPlumberInfo *info = conttest_inputplumber_monitor_match(
            monitor, NULL, 0x054c, 0x0ce6, uniq, "uhid");
        if (!info)
            return TRUE;
        conttest_inputplumber_info_free(info);
        g_main_context_iteration(NULL, FALSE);
        g_usleep(10 * 1000);
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

static gboolean run_recovery(FakeService *fake) {
    char uniq[32] = { 0 };
    if (!conttest_inputplumber_target_type_has_deterministic_uniq(
            "ds5", "persist_ds5_0", uniq, sizeof(uniq))) {
        g_test_message("no deterministic uniq for the ds5 test target");
        return FALSE;
    }

    ConttestInputPlumberMonitor *monitor = conttest_inputplumber_monitor_new();

    if (!wait_candidate(monitor, uniq, 5000)) {
        g_test_message("no candidate after the first connection");
        g_object_unref(monitor);
        return FALSE;
    }
    if (!conttest_inputplumber_monitor_is_source_devnode(monitor, "/dev/input/event99")) {
        g_test_message("source devnode was not marked hidden");
        g_object_unref(monitor);
        return FALSE;
    }

    /* Bus loss: the snapshot must be dropped, not kept stale. */
    fake_stop(fake);
    stop_bus();
    if (!wait_no_candidate(monitor, uniq, 5000)) {
        g_test_message("stale snapshot survived bus loss");
        g_object_unref(monitor);
        return FALSE;
    }

    /* Recovery: a new daemon, a fresh connection, and a new service. */
    if (!start_bus() || !fake_start(fake)) {
        g_test_message("could not restart the bus or the fake service");
        g_object_unref(monitor);
        return FALSE;
    }
    gboolean recovered = wait_candidate(monitor, uniq, 8000);
    if (!recovered)
        g_test_message("monitor did not rebuild its snapshot after bus restart");

    g_object_unref(monitor);
    return recovered;
}

static void test_bus_loss_recovery(void) {
    g_autofree char *dbus_daemon = g_find_program_in_path("dbus-daemon");
    if (!dbus_daemon) {
        g_test_skip("dbus-daemon not found");
        return;
    }

    GError *err = NULL;
    bus_dir = g_dir_make_tmp("haptika-ip-bus-XXXXXX", &err);
    if (!bus_dir) {
        g_test_skip("could not create a temporary directory");
        g_clear_error(&err);
        return;
    }
    bus_socket = g_build_filename(bus_dir, "bus.sock", NULL);
    bus_address = g_strdup_printf("unix:path=%s", bus_socket);

    gboolean ok = FALSE;
    FakeService fake = { 0 };
    if (start_bus()) {
        g_setenv("DBUS_SYSTEM_BUS_ADDRESS", bus_address, TRUE);
        if (fake_start(&fake))
            ok = run_recovery(&fake);
        else
            g_test_message("fake InputPlumber service did not start");
    } else {
        g_test_message("private D-Bus daemon did not start");
    }

    fake_stop(&fake);
    stop_bus();
    g_unlink(bus_socket);
    g_rmdir(bus_dir);
    g_clear_pointer(&bus_dir, g_free);
    g_clear_pointer(&bus_socket, g_free);
    g_clear_pointer(&bus_address, g_free);

    g_assert_true(ok);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/inputplumber_monitor/bus_loss_recovery",
                    test_bus_loss_recovery);
    return g_test_run();
}
