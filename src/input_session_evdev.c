#include "input_session.h"
#include "input_viewer.h"
#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>

typedef struct {
    struct libevdev *dev;
    int fd;
    ViewerSessionContext *ctx;
    gint *atomic_stop_flag;
} EvdevThreadData;

static gpointer evdev_polling_thread(gpointer user_data) {
    EvdevThreadData *data = (EvdevThreadData *)user_data;
    struct pollfd pfd = { .fd = data->fd, .events = POLLIN };

    while (!g_atomic_int_get(data->atomic_stop_flag)) {
        int ret = poll(&pfd, 1, 50); // poll with timeout to check stop flag
        if (ret < 0) {
            break;
        } else if (ret == 0) {
            continue; // timeout
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }

        /* Collect all evdev events from this poll() cycle into a single batch,
         * then post one g_idle_add_full(G_PRIORITY_DEFAULT) for the whole batch
         * instead of one per event. Using DEFAULT priority matches the SDL3
         * backend's responsiveness. */
        ConttestInputEventBatch *batch = NULL;
        struct input_event ev;

        while (libevdev_next_event(data->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
            ConttestInputEvent cev = { 0 };
            cev.ctx = data->ctx;
            gboolean valid = FALSE;

            if (ev.type == EV_KEY) {
                cev.type           = INPUT_EVENT_BUTTON;
                cev.element_id     = ev.code;
                cev.button_pressed = ev.value != 0;
                valid = TRUE;
            } else if (ev.type == EV_ABS) {
                const struct input_absinfo *info = libevdev_get_abs_info(data->dev, ev.code);
                if (info) {
                    double norm = 0.0;
                    double range = info->maximum - info->minimum;
                    if (range != 0) {
                        if (ev.code == ABS_Z || ev.code == ABS_RZ || ev.code == ABS_GAS || ev.code == ABS_BRAKE) {
                            // Trigger (0 to max)
                            norm = (double)(ev.value - info->minimum) / range;
                        } else {
                            // Axis (-1 to 1)
                            double center = info->minimum + range / 2.0;
                            norm = (ev.value - center) / (range / 2.0);
                        }
                    }
                    cev.type       = INPUT_EVENT_AXIS;
                    cev.element_id = ELEMENT_NS_EVDEV_ABS_BASE + ev.code;
                    cev.axis_value = norm;
                    valid = TRUE;
                }
            }

            if (valid) {
                if (!batch) {
                    batch         = g_new0(ConttestInputEventBatch, 1);
                    batch->ctx    = data->ctx;
                    batch->events = g_array_new(FALSE, FALSE, sizeof(ConttestInputEvent));
                }
                g_array_append_val(batch->events, cev);
            }
        }

        if (batch) {
            g_atomic_int_inc(&data->ctx->ref_count);
            g_idle_add_full(G_PRIORITY_DEFAULT, conttest_input_viewer_handle_event_batch, batch, NULL);
        }
    }

    if (g_atomic_int_dec_and_test(&data->ctx->ref_count)) {
        g_free(data->ctx);
    }
    return NULL;
}

static void free_evdev_session(ConttestInputSession *session) {
    EvdevThreadData *data = (EvdevThreadData *)session->impl_data;
    if (data->dev) {
        libevdev_free(data->dev);
    }
    if (data->fd >= 0) {
        close(data->fd);
    }
    g_free(data);
}

ConttestInputSession *conttest_input_session_evdev_start(const char *syspath, ViewerSessionContext *ctx) {
    char devnode[256];
    // Convert syspath to devnode, e.g. /sys/class/input/event0 to /dev/input/event0
    char *basename = g_path_get_basename(syspath);
    snprintf(devnode, sizeof(devnode), "/dev/input/%s", basename);
    g_free(basename);

    int fd = open(devnode, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        close(fd);
        return NULL;
    }

    ConttestInputSession *session = g_new0(ConttestInputSession, 1);
    session->free_func = free_evdev_session;

    EvdevThreadData *data = g_new0(EvdevThreadData, 1);
    data->dev              = dev;
    data->fd               = fd;
    data->ctx              = ctx;
    data->atomic_stop_flag = &session->atomic_stop_flag;
    session->impl_data     = data;

    // Detect capabilities
    ConttestInputElementDef *defs = g_new0(ConttestInputElementDef, KEY_MAX + ABS_MAX + 2);
    int num = 0;

    for (int i = 0; i <= KEY_MAX; i++) {
        if (libevdev_has_event_code(dev, EV_KEY, i)) {
            defs[num].element_id = i;
            defs[num].name = g_strdup(libevdev_event_code_get_name(EV_KEY, i));
            num++;
        }
    }

    for (int i = 0; i <= ABS_MAX; i++) {
        if (libevdev_has_event_code(dev, EV_ABS, i)) {
            defs[num].element_id = ELEMENT_NS_EVDEV_ABS_BASE + i;
            // Provide sensible defaults for standard sticks
            if (i == ABS_X || i == ABS_RX) defs[num].is_stick_x = TRUE;
            if (i == ABS_Y || i == ABS_RY) defs[num].is_stick_y = TRUE;
            if (i == ABS_X || i == ABS_Y)  defs[num].stick_id   = 0;
            if (i == ABS_RX || i == ABS_RY) defs[num].stick_id  = 1;

            /* HAT axes: treat each hat as a stick pair so the D-pad renders
             * on the analog stick widget. ABS_HAT0=2, ABS_HAT1=3, etc. */
            if (i >= ABS_HAT0X && i <= ABS_HAT3Y) {
                int hat_index = (i - ABS_HAT0X) / 2;
                int is_x      = (i - ABS_HAT0X) % 2 == 0;
                defs[num].stick_id   = 2 + hat_index; /* hats start at stick slot 2 */
                defs[num].is_stick_x = is_x;
                defs[num].is_stick_y = !is_x;
            }

            if (i == ABS_Z || i == ABS_RZ || i == ABS_GAS || i == ABS_BRAKE) {
                defs[num].is_trigger = TRUE;
            }

            defs[num].is_axis = TRUE;
            defs[num].name    = g_strdup(libevdev_event_code_get_name(EV_ABS, i));
            num++;
        }
    }

    ConttestInputLayout *layout = g_new0(ConttestInputLayout, 1);
    layout->elements     = defs;
    layout->num_elements = num;
    layout->ctx          = ctx;

    g_atomic_int_inc(&ctx->ref_count);
    g_main_context_invoke(NULL, conttest_input_viewer_handle_layout, layout);

    g_atomic_int_inc(&ctx->ref_count);
    session->thread = g_thread_new("evdev_poll", evdev_polling_thread, data);

    return session;
}
