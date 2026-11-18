#include "device_matching.h"
#include "inputplumber_logic.h"
#include <glib.h>

static void test_vid_pid_predicates_and_parsing(void) {
    g_assert_true(conttest_vid_pid_is_ds3(SONY_VID, DS3_PID));
    g_assert_false(conttest_vid_pid_is_ds3(SONY_VID, DS4_PID_V1));
    g_assert_false(conttest_vid_pid_is_ds3(0x045e, DS3_PID));

    g_assert_true(conttest_vid_pid_is_ds4(SONY_VID, DS4_PID_V1));
    g_assert_true(conttest_vid_pid_is_ds4(SONY_VID, DS4_PID_V2));
    g_assert_true(conttest_vid_pid_is_ds4(SONY_VID, DS4_PID_BT));
    g_assert_false(conttest_vid_pid_is_ds4(SONY_VID, DS5_PID_V1));

    g_assert_true(conttest_vid_pid_is_ds5(SONY_VID, DS5_PID_V1));
    g_assert_true(conttest_vid_pid_is_ds5(SONY_VID, DS5_PID_V2));
    g_assert_false(conttest_vid_pid_is_ds5(0x045e, DS5_PID_V1));

    uint16_t v = 0, p = 0;
    g_assert_true(conttest_parse_vid_pid("3/54c/ce6/8111", '/', &v, &p));
    g_assert_cmpuint(v, ==, 0x054c);
    g_assert_cmpuint(p, ==, 0x0ce6);

    g_assert_true(conttest_parse_vid_pid("0005:0000054C:000009CC", ':', &v, &p));
    g_assert_cmpuint(v, ==, 0x054c);
    g_assert_cmpuint(p, ==, 0x09cc);

    g_assert_false(conttest_parse_vid_pid(NULL, '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("", '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/54c", '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/54c/ce6", ':', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/54c/ce6", ';', &v, &p));
    /* Reject >16-bit overflow, signed tokens, and trailing garbage */
    g_assert_false(conttest_parse_vid_pid("3/1054c/ce6/8111", '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/54c/10ce6/8111", '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("0005:0001054C:000009CC", ':', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/-1/ce6", '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/+54c/ce6", '/', &v, &p));
    g_assert_false(conttest_parse_vid_pid("3/54c/ce6xyz", '/', &v, &p));
}

static void test_connection_state_classification(void) {
    /* Linux BUS_USB = 0x03, BUS_BLUETOOTH = 0x05, BUS_VIRTUAL = 0x06 */
    g_assert_cmpint(conttest_resolve_connection_state_from_props("0003", FALSE, FALSE),
                    ==, SDL_JOYSTICK_CONNECTION_WIRED);
    g_assert_cmpint(conttest_resolve_connection_state_from_props("0005\n", FALSE, FALSE),
                    ==, SDL_JOYSTICK_CONNECTION_WIRELESS);
    g_assert_cmpint(conttest_resolve_connection_state_from_props("0006", TRUE, FALSE),
                    ==, SDL_JOYSTICK_CONNECTION_UNKNOWN);
    g_assert_cmpint(conttest_resolve_connection_state_from_props("invalid", TRUE, FALSE),
                    ==, SDL_JOYSTICK_CONNECTION_UNKNOWN);

    /* Fallback to parent subsystem only when bustype attribute is absent/empty. */
    g_assert_cmpint(conttest_resolve_connection_state_from_props(NULL, TRUE, FALSE),
                    ==, SDL_JOYSTICK_CONNECTION_WIRED);
    g_assert_cmpint(conttest_resolve_connection_state_from_props("", FALSE, TRUE),
                    ==, SDL_JOYSTICK_CONNECTION_WIRELESS);
    g_assert_cmpint(conttest_resolve_connection_state_from_props(NULL, FALSE, FALSE),
                    ==, SDL_JOYSTICK_CONNECTION_UNKNOWN);

    struct udev *udev = udev_new();
    g_assert_nonnull(udev);
    g_assert_false(conttest_udev_is_same_hardware(udev, NULL, "/sys/nonexistent"));
    g_assert_cmpint(conttest_resolve_connection_state_udev(udev, TRUE, "/sys/nonexistent", NULL),
                    ==, SDL_JOYSTICK_CONNECTION_UNKNOWN);
    udev_unref(udev);
}

static void test_inputplumber_fnv1a_and_mac_uniq(void) {
    /* Standard FNV-1a 64-bit offset basis and IETF test vector ("foobar"). */
    g_assert_cmpuint(conttest_inputplumber_fnv1a_64(NULL), ==, G_GUINT64_CONSTANT(0xcbf29ce484222325));
    g_assert_cmpuint(conttest_inputplumber_fnv1a_64(""), ==, G_GUINT64_CONSTANT(0xcbf29ce484222325));
    g_assert_cmpuint(conttest_inputplumber_fnv1a_64("foobar"), ==, G_GUINT64_CONSTANT(0x85944171f73967e8));

    char uniq[32] = {0};
    g_assert_true(conttest_inputplumber_target_type_has_deterministic_uniq(
        "ds5", "evdev_ds5_alpha", uniq, sizeof(uniq)));
    g_assert_true(g_str_has_suffix(uniq, ":d5:05:02"));
    g_assert_cmpuint(strlen(uniq), ==, 17);

    char uniq_edge[32] = {0};
    g_assert_true(conttest_inputplumber_target_type_has_deterministic_uniq(
        "ds5-edge", "evdev_ds5_alpha", uniq_edge, sizeof(uniq_edge)));
    g_assert_true(g_str_has_suffix(uniq_edge, ":d6:0e:02"));

    char uniq_8b[32] = {0};
    g_assert_true(conttest_inputplumber_target_type_has_deterministic_uniq(
        "8bitdo-u2", "evdev_ds5_alpha", uniq_8b, sizeof(uniq_8b)));
    g_assert_true(g_str_has_suffix(uniq_8b, ":c8:2d:02"));

    char uniq_deck[32] = {0};
    g_assert_true(conttest_inputplumber_target_type_has_deterministic_uniq(
        "deck-uhid", "evdev_ds5_alpha", uniq_deck, sizeof(uniq_deck)));
    g_assert_cmpuint(strlen(uniq_deck), ==, 10);

    g_assert_false(conttest_inputplumber_target_type_has_deterministic_uniq(
        "xb360", "evdev_ds5_alpha", uniq, sizeof(uniq)));
    g_assert_false(conttest_inputplumber_target_type_has_deterministic_uniq(
        "ds5", "", uniq, sizeof(uniq)));

    g_assert_cmpstr(conttest_inputplumber_target_type_virtual_bus("ds5"), ==, "uhid");
    g_assert_cmpstr(conttest_inputplumber_target_type_virtual_bus("xb360"), ==, "uinput");
    g_assert_true(conttest_inputplumber_is_gamepad_target_type("ds5"));
    g_assert_false(conttest_inputplumber_is_gamepad_target_type("dbus"));
    g_assert_false(conttest_inputplumber_is_gamepad_target_type("keyboard"));
}

static void add_prop_str(GVariantBuilder *b, const char *k, const char *v) {
    g_variant_builder_add(b, "{sv}", k, g_variant_new_string(v));
}

static void add_prop_u32(GVariantBuilder *b, const char *k, guint32 v) {
    g_variant_builder_add(b, "{sv}", k, g_variant_new_uint32(v));
}

static void add_prop_strv(GVariantBuilder *b, const char *k, const char * const *items) {
    GVariantBuilder sb;
    g_variant_builder_init(&sb, G_VARIANT_TYPE("as"));
    for (const char * const *p = items; p && *p; p++)
        g_variant_builder_add(&sb, "s", *p);
    g_variant_builder_add(b, "{sv}", k, g_variant_builder_end(&sb));
}

static void add_managed_object(GVariantBuilder *objs,
                               const char *path,
                               const char *iface,
                               GVariant *props_dict) {
    GVariantBuilder ifaces;
    g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&ifaces, "{s@a{sv}}", iface, props_dict);
    g_variant_builder_add(objs, "{o@a{sa{sv}}}", path, g_variant_builder_end(&ifaces));
}

static void test_inputplumber_managed_objects_and_matching(void) {
    GVariantBuilder objs;
    g_variant_builder_init(&objs, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

    /* Udev source */
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "Name", "Sony DualSense");
        add_prop_str(&p, "DevicePath", "/dev/input/event12");
        add_prop_str(&p, "PhysPath", "usb-0000:00:14.0-1/input3");
        add_prop_str(&p, "IdVendor", "054c");
        add_prop_str(&p, "IdProduct", "0ce6");
        add_prop_str(&p, "Subsystem", "input");
        add_managed_object(&objs, "/org/shadowblip/Input/Source/udev0",
                           "org.shadowblip.Input.Source.UdevDevice",
                           g_variant_builder_end(&p));
    }

    /* Non-gamepad dbus target (must be skipped) + ds5 UHID target */
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "DeviceType", "dbus");
        add_prop_str(&p, "Name", "DBus");
        add_managed_object(&objs, "/org/shadowblip/Input/Target/dbus0",
                           "org.shadowblip.Input.Target", g_variant_builder_end(&p));
    }
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "DeviceType", "ds5");
        add_prop_str(&p, "Name", "Sony Interactive Entertainment DualSense Wireless Controller");
        add_managed_object(&objs, "/org/shadowblip/Input/Target/ds5_0",
                           "org.shadowblip.Input.Target", g_variant_builder_end(&p));
    }

    /* Unique xb360 target */
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "DeviceType", "xb360");
        add_prop_str(&p, "Name", "Microsoft X-Box 360 pad");
        add_managed_object(&objs, "/org/shadowblip/Input/Target/xb360_0",
                           "org.shadowblip.Input.Target", g_variant_builder_end(&p));
    }

    /* Two ambiguous identical xbox-series targets */
    for (int i = 0; i < 2; i++) {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "DeviceType", "xbox-series");
        add_prop_str(&p, "Name", "Microsoft X-Box Series X|S pad");
        char *tpath = g_strdup_printf("/org/shadowblip/Input/Target/xsx_%d", i);
        add_managed_object(&objs, tpath, "org.shadowblip.Input.Target",
                           g_variant_builder_end(&p));
        g_free(tpath);
    }

    /* Composite 0: ds5 + dbus */
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "Name", "DualSense Pad");
        add_prop_str(&p, "PersistentId", "persist_ds5_0");
        add_prop_str(&p, "ProfileName", "Default");
        add_prop_str(&p, "ProfilePath", "/etc/inputplumber/profiles/default.yaml");
        add_prop_u32(&p, "InterceptMode", 1);
        const char *srcs[] = { "/dev/input/event12", NULL };
        const char *tgts[] = { "/org/shadowblip/Input/Target/dbus0",
                               "/org/shadowblip/Input/Target/ds5_0", NULL };
        const char *caps[] = { "Gamepad:Button:South", NULL };
        add_prop_strv(&p, "SourceDevicePaths", srcs);
        add_prop_strv(&p, "TargetDevices", tgts);
        add_prop_strv(&p, "OutputCapabilities", caps);
        add_managed_object(&objs, "/org/shadowblip/Input/CompositeDevice0",
                           "org.shadowblip.Input.CompositeDevice",
                           g_variant_builder_end(&p));
    }

    /* Composite 1: xb360 + xsx_0 */
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "Name", "XBox Composite A");
        add_prop_str(&p, "PersistentId", "persist_xb_a");
        const char *srcs[] = { "/dev/input/event15", NULL };
        const char *tgts[] = { "/org/shadowblip/Input/Target/xb360_0",
                               "/org/shadowblip/Input/Target/xsx_0", NULL };
        add_prop_strv(&p, "SourceDevicePaths", srcs);
        add_prop_strv(&p, "TargetDevices", tgts);
        add_managed_object(&objs, "/org/shadowblip/Input/CompositeDevice1",
                           "org.shadowblip.Input.CompositeDevice",
                           g_variant_builder_end(&p));
    }

    /* Composite 2: xsx_1 */
    {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        add_prop_str(&p, "Name", "XBox Composite B");
        add_prop_str(&p, "PersistentId", "persist_xb_b");
        const char *srcs[] = { "/dev/input/event16", NULL };
        const char *tgts[] = { "/org/shadowblip/Input/Target/xsx_1", NULL };
        add_prop_strv(&p, "SourceDevicePaths", srcs);
        add_prop_strv(&p, "TargetDevices", tgts);
        add_managed_object(&objs, "/org/shadowblip/Input/CompositeDevice2",
                           "org.shadowblip.Input.CompositeDevice",
                           g_variant_builder_end(&p));
    }

    GVariant *reply = g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objs));
    GHashTable *source_devnodes = NULL;
    GPtrArray *candidates = conttest_inputplumber_parse_managed_objects(
        reply, "0.49.1", &source_devnodes);
    g_variant_unref(reply);

    /* Unexpected GVariant types must return empty results cleanly. */
    GVariant *bad_str = g_variant_ref_sink(g_variant_new_string("not-a-tuple"));
    GPtrArray *bad_res1 = conttest_inputplumber_parse_managed_objects(bad_str, "0.49.1", NULL);
    g_assert_cmpuint(bad_res1->len, ==, 0);
    g_ptr_array_unref(bad_res1);
    g_variant_unref(bad_str);

    GVariant *bad_tuple = g_variant_ref_sink(g_variant_new("(s)", "wrong-inner-type"));
    GPtrArray *bad_res2 = conttest_inputplumber_parse_managed_objects(bad_tuple, "0.49.1", NULL);
    g_assert_cmpuint(bad_res2->len, ==, 0);
    g_ptr_array_unref(bad_res2);
    g_variant_unref(bad_tuple);

    g_assert_cmpuint(candidates->len, ==, 4);
    g_assert_true(g_hash_table_contains(source_devnodes, "/dev/input/event12"));
    g_assert_true(g_hash_table_contains(source_devnodes, "/dev/input/event15"));
    g_assert_true(g_hash_table_contains(source_devnodes, "/dev/input/event16"));
    g_assert_false(g_hash_table_contains(source_devnodes, "/dev/input/event99"));

    /* Pass 1: Exact deterministic uniq match for ds5 on uhid (case-insensitive MAC) */
    char expected_ds5_uniq[32] = {0};
    g_assert_true(conttest_inputplumber_target_type_has_deterministic_uniq(
        "ds5", "persist_ds5_0", expected_ds5_uniq, sizeof(expected_ds5_uniq)));
    char *upper_uniq = g_ascii_strup(expected_ds5_uniq, -1);

    ConttestInputPlumberInfo *m1 = conttest_inputplumber_match_candidates(
        candidates, "0.49.1", "Ignored Name", 0x054c, 0x0ce6, upper_uniq, "uhid");
    g_assert_nonnull(m1);
    g_assert_cmpstr(m1->composite_path, ==, "/org/shadowblip/Input/CompositeDevice0");
    g_assert_cmpstr(m1->target_type, ==, "ds5");
    g_assert_cmpstr(m1->virtual_bus, ==, "uhid");
    g_assert_cmpstr(m1->daemon_version, ==, "0.49.1");
    g_assert_cmpuint(m1->sources->len, ==, 1);

    ConttestInputPlumberInfo *m1_copy = conttest_inputplumber_info_copy(m1);
    g_assert_true(conttest_inputplumber_info_equal(m1, m1_copy));
    conttest_inputplumber_info_free(m1_copy);
    conttest_inputplumber_info_free(m1);

    /* Reject contradicting VID/PID even if uniq matches, and wrong bus */
    g_assert_null(conttest_inputplumber_match_candidates(
        candidates, "0.49.1", NULL, 0x045e, 0x028e, upper_uniq, "uhid"));
    g_assert_null(conttest_inputplumber_match_candidates(
        candidates, "0.49.1", NULL, 0x054c, 0x0ce6, upper_uniq, "uinput"));
    g_free(upper_uniq);

    /* Deterministic ds5 must NEVER fall back to Pass 2 VID/PID + name without uniq */
    g_assert_null(conttest_inputplumber_match_candidates(
        candidates, "0.49.1",
        "Sony Interactive Entertainment DualSense Wireless Controller",
        0x054c, 0x0ce6, NULL, "uhid"));

    /* Pass 2: Unique xb360 uinput target matches cleanly */
    ConttestInputPlumberInfo *m2 = conttest_inputplumber_match_candidates(
        candidates, "0.49.1", "Microsoft X-Box 360 pad", 0x045e, 0x028e, NULL, "uinput");
    g_assert_nonnull(m2);
    g_assert_cmpstr(m2->target_type, ==, "xb360");
    g_assert_cmpstr(m2->composite_path, ==, "/org/shadowblip/Input/CompositeDevice1");
    conttest_inputplumber_info_free(m2);

    /* Pass 2: Ambiguous tie across two identical xbox-series candidates must return NULL */
    g_assert_null(conttest_inputplumber_match_candidates(
        candidates, "0.49.1", "Microsoft X-Box Series X|S pad", 0x045e, 0x0b12, NULL, "uinput"));

    g_ptr_array_free(candidates, TRUE);
    g_hash_table_destroy(source_devnodes);
}

typedef struct {
    const char *syspath;
    const char *subsystem;
    const char *devtype;
} MockAncestor;

static char **collect_mock_hw_keys(const MockAncestor *chain, gsize len) {
    GPtrArray *keys = g_ptr_array_new();
    gboolean saw_bt = FALSE;
    for (gsize i = 0; i < len; i++) {
        if (conttest_udev_ancestor_is_hw_key(chain[i].subsystem,
                                             chain[i].devtype, &saw_bt)) {
            g_ptr_array_add(keys, g_strdup(chain[i].syspath));
        }
    }
    g_ptr_array_add(keys, NULL);
    return (char **)g_ptr_array_free(keys, FALSE);
}

static void test_hw_keys_matching(void) {
    /* Identical syspaths match even without ancestor keys */
    g_assert_true(conttest_hw_keys_match(
        "/sys/devices/pci0/usb1/event5", NULL,
        "/sys/devices/pci0/usb1/event5", NULL));

    /* NULL / mismatched inputs without shared keys do not match */
    g_assert_false(conttest_hw_keys_match(NULL, NULL, NULL, NULL));
    g_assert_false(conttest_hw_keys_match(
        "/sys/devices/a", NULL, "/sys/devices/b", NULL));

    const char *keys_evdev[] = {
        "/sys/devices/pci0/usb1/1-1:1.0/0003:054C:09CC.0001/input/input12/event5",
        "/sys/devices/pci0/usb1/1-1:1.0/0003:054C:09CC.0001/input/input12",
        "/sys/devices/pci0/usb1/1-1:1.0/0003:054C:09CC.0001",
        "/sys/devices/pci0/usb1/1-1:1.0",
        NULL,
    };
    const char *keys_hidraw[] = {
        "/sys/devices/pci0/usb1/1-1:1.0/0003:054C:09CC.0001",
        "/sys/devices/pci0/usb1/1-1:1.0",
        NULL,
    };
    const char *keys_other[] = {
        "/sys/devices/pci0/usb1/1-2:1.0/0003:054C:09CC.0002",
        "/sys/devices/pci0/usb1/1-2:1.0",
        NULL,
    };
    const char *empty_keys[] = { NULL };

    /* Shared HID/USB-interface ancestor keys match across event* and hidraw* nodes */
    g_assert_true(conttest_hw_keys_match(
        "/sys/devices/pci0/usb1/event5", keys_evdev,
        "/sys/devices/pci0/usb1/hidraw3", keys_hidraw));

    /* Disjoint ancestor keys do not match */
    g_assert_false(conttest_hw_keys_match(
        "/sys/devices/pci0/usb1/event5", keys_evdev,
        "/sys/devices/pci0/usb1/hidraw4", keys_other));
    g_assert_false(conttest_hw_keys_match(
        "/sys/devices/pci0/usb1/event5", keys_evdev,
        "/sys/devices/pci0/usb1/hidraw4", empty_keys));

    /* Identical key ordering permutations still match cleanly */
    const char *keys_parent_self[] = {
        "/sys/devices/virtual/input/input99",
        NULL,
    };
    const char *keys_child_with_parent[] = {
        "/sys/devices/virtual/input/input99/event9",
        "/sys/devices/virtual/input/input99",
        NULL,
    };
    g_assert_true(conttest_hw_keys_match(
        "/sys/devices/virtual/input/input99", keys_parent_self,
        "/sys/devices/virtual/input/input99/event9", keys_child_with_parent));
    g_assert_true(conttest_hw_keys_match(
        "/sys/devices/virtual/input/input99/event9", keys_child_with_parent,
        "/sys/devices/virtual/input/input99", keys_parent_self));

    /* Classic Bluetooth HIDP: two distinct controllers paired to the same USB
     * Bluetooth adapter (1-14:1.0 -> bluetooth/hci0) must NOT coalesce via the
     * shared host radio or USB Bluetooth interface, while event* and hidraw*
     * of the same ACL link (hci0:256) DO match. */
    static const MockAncestor bt_pad1_evdev[] = {
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256/0005:054C:09CC.0001/input/input30/event20", "input", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256/0005:054C:09CC.0001/input/input30", "input", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256/0005:054C:09CC.0001", "hid", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256", "bluetooth", "link" },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0", "bluetooth", "host" },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0", "usb", "usb_interface" },
        { "/sys/devices/pci0/usb1/1-14", "usb", "usb_device" },
    };
    static const MockAncestor bt_pad1_hidraw[] = {
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256/0005:054C:09CC.0001/hidraw/hidraw4", "hidraw", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256/0005:054C:09CC.0001", "hid", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:256", "bluetooth", "link" },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0", "bluetooth", "host" },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0", "usb", "usb_interface" },
        { "/sys/devices/pci0/usb1/1-14", "usb", "usb_device" },
    };
    static const MockAncestor bt_pad2_hidraw[] = {
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:257/0005:054C:0CE6.0002/hidraw/hidraw5", "hidraw", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:257/0005:054C:0CE6.0002", "hid", NULL },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0/hci0:257", "bluetooth", "link" },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0/bluetooth/hci0", "bluetooth", "host" },
        { "/sys/devices/pci0/usb1/1-14/1-14:1.0", "usb", "usb_interface" },
        { "/sys/devices/pci0/usb1/1-14", "usb", "usb_device" },
    };

    g_auto(GStrv) bt1_ev_keys = collect_mock_hw_keys(bt_pad1_evdev, G_N_ELEMENTS(bt_pad1_evdev));
    g_auto(GStrv) bt1_hr_keys = collect_mock_hw_keys(bt_pad1_hidraw, G_N_ELEMENTS(bt_pad1_hidraw));
    g_auto(GStrv) bt2_hr_keys = collect_mock_hw_keys(bt_pad2_hidraw, G_N_ELEMENTS(bt_pad2_hidraw));

    g_assert_true(conttest_hw_keys_match(
        bt_pad1_evdev[0].syspath, (const char * const *)bt1_ev_keys,
        bt_pad1_hidraw[0].syspath, (const char * const *)bt1_hr_keys));
    g_assert_false(conttest_hw_keys_match(
        bt_pad1_evdev[0].syspath, (const char * const *)bt1_ev_keys,
        bt_pad2_hidraw[0].syspath, (const char * const *)bt2_hr_keys));
}

static void test_serial_and_candidate_corroboration(void) {
    g_assert_false(conttest_serial_is_usable(NULL));
    g_assert_false(conttest_serial_is_usable(""));
    g_assert_false(conttest_serial_is_usable("0"));
    g_assert_false(conttest_serial_is_usable("00:00:00:00:00:00"));
    g_assert_false(conttest_serial_is_usable("abc"));
    g_assert_true(conttest_serial_is_usable("00:1a:7d:da:71:13"));
    g_assert_true(conttest_serial_is_usable("1000"));

    g_assert_true(conttest_serial_matches("aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-FF"));
    g_assert_true(conttest_serial_matches("aabbccddeeff", "AA:BB:CC:DD:EE:FF"));
    g_assert_false(conttest_serial_matches("aa:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:00"));
    g_assert_false(conttest_serial_matches(NULL, "aabb"));

    g_assert_false(conttest_conn_contradicts(SDL_JOYSTICK_CONNECTION_UNKNOWN,
                                             SDL_JOYSTICK_CONNECTION_WIRED));
    g_assert_true(conttest_conn_contradicts(SDL_JOYSTICK_CONNECTION_WIRELESS,
                                            SDL_JOYSTICK_CONNECTION_WIRED));

    /* Matching usable serials yield STRONG even if connection types disagree */
    g_assert_cmpint(
        conttest_classify_candidate_pair("aa:bb:cc:dd:ee:ff",
                                         SDL_JOYSTICK_CONNECTION_WIRED,
                                         "AABBCCDDEEFF",
                                         SDL_JOYSTICK_CONNECTION_WIRELESS),
        ==, CONTTEST_MATCH_STRONG);

    /* Distinct usable serials always REJECT */
    g_assert_cmpint(
        conttest_classify_candidate_pair("aa:bb:cc:dd:ee:01",
                                         SDL_JOYSTICK_CONNECTION_WIRED,
                                         "aa:bb:cc:dd:ee:02",
                                         SDL_JOYSTICK_CONNECTION_WIRED),
        ==, CONTTEST_MATCH_REJECT);

    /* Unusable/missing serials fall back to connection compatibility */
    g_assert_cmpint(
        conttest_classify_candidate_pair("0000",
                                         SDL_JOYSTICK_CONNECTION_WIRED,
                                         NULL,
                                         SDL_JOYSTICK_CONNECTION_WIRELESS),
        ==, CONTTEST_MATCH_REJECT);
    g_assert_cmpint(
        conttest_classify_candidate_pair(NULL,
                                         SDL_JOYSTICK_CONNECTION_WIRED,
                                         NULL,
                                         SDL_JOYSTICK_CONNECTION_UNKNOWN),
        ==, CONTTEST_MATCH_WEAK);
}

static void test_inputplumber_dbus_signal_filtering(void) {
    gboolean ver_inv = FALSE;

    /* PropertiesChanged: relevant non-manager interfaces */
    GVariant *pc_comp = g_variant_ref_sink(g_variant_new("(sa{sv}as)",
        "org.shadowblip.Input.CompositeDevice", NULL, NULL));
    g_assert_true(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.Properties", "PropertiesChanged", pc_comp, &ver_inv));
    g_assert_false(ver_inv);
    g_variant_unref(pc_comp);

    /* PropertiesChanged: InputManager marks version invalidated */
    GVariant *pc_mgr = g_variant_ref_sink(g_variant_new("(sa{sv}as)",
        "org.shadowblip.InputManager", NULL, NULL));
    g_assert_true(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.Properties", "PropertiesChanged", pc_mgr, &ver_inv));
    g_assert_true(ver_inv);
    g_variant_unref(pc_mgr);

    /* PropertiesChanged: high-rate unrelated DBus target / gamepad interfaces rejected */
    GVariant *pc_dbus = g_variant_ref_sink(g_variant_new("(sa{sv}as)",
        "org.shadowblip.Input.DBusDevice", NULL, NULL));
    g_assert_false(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.Properties", "PropertiesChanged", pc_dbus, &ver_inv));
    g_assert_false(ver_inv);
    g_variant_unref(pc_dbus);

    /* InterfacesAdded: relevant vs irrelevant */
    GVariantBuilder added_rel;
    g_variant_builder_init(&added_rel, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&added_rel, "{sa{sv}}", "org.shadowblip.Input.Target", NULL);
    GVariant *ia_rel = g_variant_ref_sink(g_variant_new("(o@a{sa{sv}})",
        "/org/shadowblip/InputPlumber/devices/target0", g_variant_builder_end(&added_rel)));
    g_assert_true(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", ia_rel, &ver_inv));
    g_assert_false(ver_inv);
    g_variant_unref(ia_rel);

    GVariantBuilder added_irrel;
    g_variant_builder_init(&added_irrel, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&added_irrel, "{sa{sv}}", "org.shadowblip.Input.DBusDevice", NULL);
    GVariant *ia_irrel = g_variant_ref_sink(g_variant_new("(o@a{sa{sv}})",
        "/org/shadowblip/InputPlumber/devices/dbus0", g_variant_builder_end(&added_irrel)));
    g_assert_false(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", ia_irrel, &ver_inv));
    g_variant_unref(ia_irrel);

    /* InterfacesRemoved: relevant vs irrelevant */
    const char *rem_rel_list[] = { "org.shadowblip.Input.Source.UdevDevice", NULL };
    GVariant *ir_rel = g_variant_ref_sink(g_variant_new("(o^as)",
        "/org/shadowblip/InputPlumber/devices/source0", rem_rel_list));
    g_assert_true(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", ir_rel, &ver_inv));
    g_variant_unref(ir_rel);

    const char *rem_irrel_list[] = { "org.shadowblip.Input.Gamepad", NULL };
    GVariant *ir_irrel = g_variant_ref_sink(g_variant_new("(o^as)",
        "/org/shadowblip/InputPlumber/devices/dbus0", rem_irrel_list));
    g_assert_false(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", ir_irrel, &ver_inv));
    g_variant_unref(ir_irrel);

    /* Malformed / NULL inputs handled safely */
    g_assert_false(conttest_inputplumber_dbus_signal_is_relevant(NULL, "PropertiesChanged", NULL, &ver_inv));
    g_assert_false(conttest_inputplumber_dbus_signal_is_relevant(
        "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, &ver_inv));
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/device_match/vid_pid_predicates_and_parsing",
                    test_vid_pid_predicates_and_parsing);
    g_test_add_func("/device_match/connection_state_classification",
                    test_connection_state_classification);
    g_test_add_func("/device_match/inputplumber_fnv1a_and_mac_uniq",
                    test_inputplumber_fnv1a_and_mac_uniq);
    g_test_add_func("/device_match/inputplumber_managed_objects_and_matching",
                    test_inputplumber_managed_objects_and_matching);
    g_test_add_func("/device_match/inputplumber_dbus_signal_filtering",
                    test_inputplumber_dbus_signal_filtering);
    g_test_add_func("/device_match/hw_keys_matching",
                    test_hw_keys_matching);
    g_test_add_func("/device_match/serial_and_candidate_corroboration",
                    test_serial_and_candidate_corroboration);
    return g_test_run();
}
