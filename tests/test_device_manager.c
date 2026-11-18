/*
 * Device manager integration test.
 *
 * Creates a synthetic uinput gamepad so the manager exercises the same
 * SDL/evdev coalescing path as real hardware: attach, fast replug, replug
 * with a lost SDL removal event, and a slow reconnect. Each phase must leave
 * exactly one visible device carrying both backends and a live device node.
 *
 * Requires writable /dev/uinput and the udev daemon; skipped otherwise.
 */
#define _POSIX_C_SOURCE 200809L
#include "device_manager.h"

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Test-only IDs: no real controller uses them, and SDL keeps the gamepad
 * mapping registered for the whole process, so every replug is re-classified
 * as a gamepad. */
#define TEST_VENDOR  0x0f0f
#define TEST_PRODUCT 0x0001
#define TEST_NAME    "Haptika Test Pad"

static int uinput_fd = -1;

static void set_abs(int fd, int code, int minimum, int maximum, int flat) {
    struct uinput_abs_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.code = (unsigned short)code;
    setup.absinfo.minimum = minimum;
    setup.absinfo.maximum = maximum;
    setup.absinfo.flat = flat;
    ioctl(fd, UI_ABS_SETUP, &setup);
}

static int uinput_create(void) {
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    static const int buttons[] = {
        BTN_SOUTH,     BTN_EAST,       BTN_WEST,      BTN_NORTH,
        BTN_TL,        BTN_TR,         BTN_SELECT,    BTN_START,
        BTN_MODE,      BTN_THUMBL,     BTN_THUMBR,    BTN_DPAD_UP,
        BTN_DPAD_DOWN, BTN_DPAD_LEFT,  BTN_DPAD_RIGHT,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++)
        ioctl(fd, UI_SET_KEYBIT, buttons[i]);

    static const int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ };
    for (size_t i = 0; i < G_N_ELEMENTS(axes); i++)
        ioctl(fd, UI_SET_ABSBIT, axes[i]);

    set_abs(fd, ABS_X, -32768, 32767, 128);
    set_abs(fd, ABS_Y, -32768, 32767, 128);
    set_abs(fd, ABS_RX, -32768, 32767, 128);
    set_abs(fd, ABS_RY, -32768, 32767, 128);
    set_abs(fd, ABS_Z, 0, 255, 0);
    set_abs(fd, ABS_RZ, 0, 255, 0);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = TEST_VENDOR;
    setup.id.product = TEST_PRODUCT;
    setup.id.version = 0x0100;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, TEST_NAME);
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    return fd;
}

static void uinput_destroy(void) {
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }
}

static SDL_JoystickID find_test_joystick(void) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    SDL_JoystickID found = 0;
    for (int i = 0; i < count; i++) {
        if (SDL_GetJoystickVendorForID(ids[i]) == TEST_VENDOR &&
            SDL_GetJoystickProductForID(ids[i]) == TEST_PRODUCT)
            found = ids[i];
    }
    SDL_free(ids);
    return found;
}

/* SDL only emits GAMEPAD_ADDED for mapped devices. Register a mapping for the
 * synthetic pad once; SDL keeps it for the process, so replugs stay gamepads. */
static gboolean register_gamepad_mapping(void) {
    SDL_PumpEvents();
    SDL_JoystickID id = find_test_joystick();
    if (id == 0)
        return FALSE;

    char guid[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));

    char mapping[512];
    snprintf(mapping, sizeof(mapping),
             "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,"
             "leftshoulder:b9,rightshoulder:b10,leftstick:b7,rightstick:b8,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,"
             "righttrigger:a5,",
             guid, TEST_NAME);
    if (SDL_AddGamepadMapping(mapping) < 0)
        return FALSE;

    SDL_PumpEvents();
    SDL_Delay(30);
    return TRUE;
}

typedef struct {
    guint total;
    guint dual;
    guint sdl_only;
    guint evdev_only;
    char devnode[64];
} TestDeviceStats;

static gboolean is_test_device(const ConttestDevice *dev) {
    const char *syspath = conttest_device_get_evdev_syspath(dev);
    return conttest_device_get_vendor_id(dev) == TEST_VENDOR &&
           conttest_device_get_product_id(dev) == TEST_PRODUCT &&
           syspath && g_str_has_prefix(syspath, "/sys/devices/virtual/");
}

static void collect_device(ConttestDevice *dev, gpointer user_data) {
    TestDeviceStats *stats = user_data;
    if (!is_test_device(dev))
        return;

    stats->total++;
    gboolean has_sdl = conttest_device_get_has_sdl(dev);
    gboolean has_evdev = conttest_device_get_has_evdev(dev);
    if (has_sdl && has_evdev) {
        stats->dual++;
        const char *node = conttest_device_get_evdev_devnode(dev);
        if (node)
            g_strlcpy(stats->devnode, node, sizeof(stats->devnode));
    } else if (has_sdl) {
        stats->sdl_only++;
    } else if (has_evdev) {
        stats->evdev_only++;
    }
}

static TestDeviceStats test_device_stats(ConttestDeviceManager *manager) {
    TestDeviceStats stats;
    memset(&stats, 0, sizeof(stats));
    conttest_device_manager_foreach(manager, collect_device, &stats);
    return stats;
}

static gboolean stats_are_settled(const TestDeviceStats *stats) {
    return stats->total == 1 && stats->dual == 1 && stats->sdl_only == 0 &&
           stats->evdev_only == 0 && stats->devnode[0] != '\0' &&
           access(stats->devnode, F_OK) == 0;
}

/* Poll the manager until it reports exactly one fully attached test device.
 * The manager's hotplug and metadata work run asynchronously, so a fixed delay
 * would be both slower and racier than watching for the expected state. */
static void dump_device(ConttestDevice *dev, gpointer user_data) {
    (void)user_data;
    g_test_message("visible: \"%s\" vid=%04x pid=%04x sdl=%d evdev=%d node=%s syspath=%s",
                   conttest_device_get_name(dev),
                   conttest_device_get_vendor_id(dev),
                   conttest_device_get_product_id(dev),
                   conttest_device_get_has_sdl(dev),
                   conttest_device_get_has_evdev(dev),
                   conttest_device_get_evdev_devnode(dev)
                       ? conttest_device_get_evdev_devnode(dev) : "-",
                   conttest_device_get_evdev_syspath(dev)
                       ? conttest_device_get_evdev_syspath(dev) : "-");
}

static gboolean wait_for_settled(ConttestDeviceManager *manager,
                                 const char *phase,
                                 guint timeout_ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    TestDeviceStats stats;
    do {
        stats = test_device_stats(manager);
        if (stats_are_settled(&stats))
            return TRUE;
        g_main_context_iteration(NULL, FALSE);
        g_usleep(5 * 1000);
    } while (g_get_monotonic_time() < deadline);

    g_test_message("%s: total=%u dual=%u sdl_only=%u evdev_only=%u node=%s",
                   phase, stats.total, stats.dual, stats.sdl_only,
                   stats.evdev_only, stats.devnode[0] ? stats.devnode : "-");
    int gamepad_count = 0;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&gamepad_count);
    for (int i = 0; i < gamepad_count; i++) {
        g_test_message("SDL gamepad id=%u name=%s path=%s",
                       (unsigned)gamepads[i],
                       SDL_GetGamepadNameForID(gamepads[i])
                           ? SDL_GetGamepadNameForID(gamepads[i]) : "-",
                       SDL_GetGamepadPathForID(gamepads[i])
                           ? SDL_GetGamepadPathForID(gamepads[i]) : "-");
    }
    SDL_free(gamepads);
    conttest_device_manager_foreach(manager, dump_device, NULL);
    return FALSE;
}

static void set_removal_events_enabled(gboolean enabled) {
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_REMOVED, enabled);
    SDL_SetEventEnabled(SDL_EVENT_JOYSTICK_REMOVED, enabled);
}

/* Replug without letting the manager see the SDL removal event, like a device
 * that re-enumerates before SDL's hotplug thread reports the disconnect. */
static void replug_with_lost_removal(void) {
    set_removal_events_enabled(FALSE);
    uinput_destroy();
    uinput_fd = uinput_create();
    SDL_Delay(120);
    set_removal_events_enabled(TRUE);
}

static void test_hotplug_lifecycle(void) {
    if (access("/dev/uinput", W_OK) != 0) {
        g_test_skip("/dev/uinput is not writable");
        return;
    }
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        g_test_skip("SDL joystick subsystem unavailable");
        return;
    }

    uinput_fd = uinput_create();
    if (uinput_fd < 0) {
        SDL_Quit();
        g_test_skip("could not create a uinput device");
        return;
    }
    SDL_Delay(200);
    if (!register_gamepad_mapping()) {
        uinput_destroy();
        SDL_Quit();
        g_test_skip("SDL did not enumerate the synthetic gamepad");
        return;
    }

    ConttestDeviceManager *manager = conttest_device_manager_new();
    g_assert_true(wait_for_settled(manager, "initial attach", 3000));

    /* Fast replug: both the removal and the addition reach the manager. */
    uinput_destroy();
    uinput_fd = uinput_create();
    SDL_Delay(50);
    g_assert_true(wait_for_settled(manager, "ordered replug", 2000));

    /* SDL reports the new instance but the removal event is lost, leaving the
     * old handle behind until the stale-attachment check releases it. */
    replug_with_lost_removal();
    g_assert_true(wait_for_settled(manager, "lost SDL removal", 2000));

    /* A second consecutive lost removal must not accumulate ghost rows. */
    replug_with_lost_removal();
    g_assert_true(wait_for_settled(manager, "second lost SDL removal", 2000));

    /* Slow reconnect as a control: the removal is fully processed first. */
    uinput_destroy();
    SDL_Delay(700);
    uinput_fd = uinput_create();
    SDL_Delay(50);
    g_assert_true(wait_for_settled(manager, "slow replug", 2000));

    g_object_unref(manager);
    uinput_destroy();
    SDL_Quit();
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/device_manager/hotplug_lifecycle",
                    test_hotplug_lifecycle);
    return g_test_run();
}
