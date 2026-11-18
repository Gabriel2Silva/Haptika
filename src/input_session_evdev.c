#define _POSIX_C_SOURCE 200809L
#include "input_session.h"
#include "input_viewer.h"
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>

/* Sub-pixel noise gate for 16-bit axes (~0.00049): lossless for 8-bit/10-bit
 * pads and hats while suppressing +-1..8 LSB idle ADC jitter before waking GTK. */
#define EVDEV_AXIS_NOISE_EPSILON (16.0f / 32768.0f)

typedef struct {
    double  scale;
    double  offset;
    float   min_clamp;
    float   last_norm;
    int32_t minimum;
    int32_t maximum;
    uint8_t valid;
    uint8_t has_prev;
} EvdevAbsAxisLut;

typedef struct {
    struct libevdev *dev;
    int fd;
    ViewerSessionContext *ctx;
    gint *atomic_stop_flag;
    EvdevAbsAxisLut abs_lut[ABS_CNT];
} EvdevThreadData;

static ConttestAbsAxisInfo read_abs_axis_info(struct libevdev *dev, unsigned int code) {
    const struct input_absinfo *info = libevdev_has_event_code(dev, EV_ABS, code)
        ? libevdev_get_abs_info(dev, code)
        : NULL;
    if (!info)
        return (ConttestAbsAxisInfo){ .present = false };
    return (ConttestAbsAxisInfo){
        .present = true,
        .minimum = info->minimum,
        .maximum = info->maximum,
        .value   = info->value,
        .flat    = info->flat,
    };
}

static char *evdev_code_name(unsigned int type, unsigned int code, const char *prefix) {
    const char *name = libevdev_event_code_get_name(type, code);
    if (name)
        return g_strdup(name);
    return g_strdup_printf("%s_0x%x", prefix, code);
}

static void evdev_init_abs_lut(struct libevdev *dev,
                               unsigned int code,
                               gboolean is_trigger,
                               EvdevAbsAxisLut *out) {
    if (code >= ABS_CNT)
        return;
    const struct input_absinfo *info = libevdev_get_abs_info(dev, code);
    if (!info)
        return;

    out->valid   = 1;
    out->minimum = info->minimum;
    out->maximum = info->maximum;

    double range = (double)info->maximum - (double)info->minimum;
    if (range <= 0.0) {
        out->scale     = 0.0;
        out->offset    = 0.0;
        out->min_clamp = 0.0f;
    } else if (is_trigger) {
        out->scale     = 1.0 / range;
        out->offset    = -(double)info->minimum / range;
        out->min_clamp = 0.0f;
    } else {
        double half    = range * 0.5;
        double center  = (double)info->minimum + half;
        out->scale     = 1.0 / half;
        out->offset    = -center / half;
        out->min_clamp = -1.0f;
    }
}

static gboolean evdev_push_event(EvdevAbsAxisLut abs_lut[ABS_CNT],
                                 ViewerSessionContext *ctx,
                                 unsigned int type,
                                 unsigned int code,
                                 int value) {
    if (type == EV_ABS) {
        if (code >= ABS_CNT || !abs_lut[code].valid)
            return FALSE;

        EvdevAbsAxisLut *a = &abs_lut[code];
        float norm;
        if (a->maximum <= a->minimum) {
            norm = 0.0f;
        } else if (value <= a->minimum) {
            norm = a->min_clamp;
        } else if (value >= a->maximum) {
            norm = 1.0f;
        } else {
            double d = (double)value * a->scale + a->offset;
            norm = (float)CLAMP(d, (double)a->min_clamp, 1.0);
        }

        if (a->has_prev) {
            float prev = a->last_norm;
            if (norm == prev)
                return FALSE;
            if (norm != 0.0f && fabsf(norm) < 1.0f &&
                fabsf(norm - prev) <= EVDEV_AXIS_NOISE_EPSILON) {
                return FALSE;
            }
        }
        a->has_prev  = 1;
        a->last_norm = norm;

        ConttestInputEvent cev = {
            .type       = INPUT_EVENT_AXIS,
            .domain     = CONTTEST_ELEMENT_EVDEV_ABS,
            .element_id = (int)code,
            .axis_value = norm,
        };
        conttest_event_ring_push(&ctx->ring, &cev);
        return TRUE;
    }

    if (type == EV_KEY) {
        /* Ignore kernel key-repeat events (value == 2): button is already held. */
        if (value == 2 || code > KEY_MAX)
            return FALSE;
        ConttestInputEvent cev = {
            .type           = INPUT_EVENT_BUTTON,
            .domain         = CONTTEST_ELEMENT_BUTTON,
            .element_id     = (int)code,
            .button_pressed = (unsigned int)(value != 0),
        };
        conttest_event_ring_push(&ctx->ring, &cev);
        return TRUE;
    }

    return FALSE;
}

static gpointer evdev_polling_thread(gpointer user_data) {
    EvdevThreadData *data = (EvdevThreadData *)user_data;
    struct pollfd pfd = { .fd = data->fd, .events = POLLIN };

    while (!g_atomic_int_get(data->atomic_stop_flag)) {
        int ret = poll(&pfd, 1, 50);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        gboolean pushed = FALSE;
        struct input_event ev;
        int rc;

        do {
            rc = libevdev_next_event(data->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                /* Drain kernel SYN_DROPPED sync stream before resuming normal reads. */
                while ((rc = libevdev_next_event(data->dev, LIBEVDEV_READ_FLAG_SYNC, &ev)) ==
                       LIBEVDEV_READ_STATUS_SYNC) {
                    if (evdev_push_event(data->abs_lut, data->ctx,
                                         ev.type, ev.code, ev.value))
                        pushed = TRUE;
                }
                if (rc == -EAGAIN)
                    rc = LIBEVDEV_READ_STATUS_SUCCESS;
            } else if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                if (evdev_push_event(data->abs_lut, data->ctx,
                                     ev.type, ev.code, ev.value))
                    pushed = TRUE;
            }
        } while (rc == LIBEVDEV_READ_STATUS_SUCCESS && !g_atomic_int_get(data->atomic_stop_flag));

        if (pushed)
            conttest_event_ring_notify(data->ctx, conttest_input_viewer_drain_ring);
    }

    viewer_session_context_unref(data->ctx);
    return NULL;
}

static void free_evdev_session(ConttestInputSession *session) {
    EvdevThreadData *data = (EvdevThreadData *)session->impl_data;
    if (data->dev)
        libevdev_free(data->dev);
    if (data->fd >= 0)
        close(data->fd);
    g_free(data);
}

ConttestInputSession *conttest_input_session_evdev_start(const char *devnode,
                                                         ViewerSessionContext *ctx) {
    if (!devnode || !ctx)
        return NULL;

    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return NULL;

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

    gboolean z_rz_right_stick = FALSE;
    gboolean rx_ry_triggers = FALSE;
    conttest_evdev_classify_axes(read_abs_axis_info(dev, ABS_Z),
                                 read_abs_axis_info(dev, ABS_RZ),
                                 read_abs_axis_info(dev, ABS_RX),
                                 read_abs_axis_info(dev, ABS_RY),
                                 &z_rz_right_stick, &rx_ry_triggers);

    int max_defs = 0;
    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (libevdev_has_event_code(dev, EV_KEY, code))
            max_defs++;
    }
    for (unsigned int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(dev, EV_ABS, code))
            max_defs++;
    }

    ConttestInputElementDef *defs = max_defs > 0
        ? g_new0(ConttestInputElementDef, (gsize)max_defs)
        : NULL;
    int num = 0;

    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (!libevdev_has_event_code(dev, EV_KEY, code))
            continue;
        defs[num].domain = CONTTEST_ELEMENT_BUTTON;
        defs[num].element_id = (int)code;
        defs[num].name = evdev_code_name(EV_KEY, code, "BTN");
        num++;
    }

    for (unsigned int code = 0; code <= ABS_MAX; code++) {
        if (!libevdev_has_event_code(dev, EV_ABS, code))
            continue;

        defs[num].domain     = CONTTEST_ELEMENT_EVDEV_ABS;
        defs[num].element_id = (int)code;
        defs[num].is_axis    = TRUE;
        defs[num].name       = evdev_code_name(EV_ABS, code, "ABS");

        gboolean is_trig = (code == ABS_GAS || code == ABS_BRAKE);
        if (code == ABS_X || code == ABS_Y) {
            defs[num].is_stick_x = (code == ABS_X);
            defs[num].is_stick_y = (code == ABS_Y);
            defs[num].stick_id = 0;
        } else if (z_rz_right_stick && (code == ABS_Z || code == ABS_RZ)) {
            defs[num].is_stick_x = (code == ABS_Z);
            defs[num].is_stick_y = (code == ABS_RZ);
            defs[num].stick_id = 1;
        } else if (!rx_ry_triggers && (code == ABS_RX || code == ABS_RY)) {
            defs[num].is_stick_x = (code == ABS_RX);
            defs[num].is_stick_y = (code == ABS_RY);
            defs[num].stick_id = 1;
        } else if (code == ABS_Z || code == ABS_RZ ||
                   (rx_ry_triggers && (code == ABS_RX || code == ABS_RY))) {
            is_trig = TRUE;
        }

        /* Map hat pairs into analog stick widgets after Left/Right sticks. */
        if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
            unsigned int offset = code - ABS_HAT0X;
            defs[num].stick_id   = 2 + (int)(offset / 2u);
            defs[num].is_stick_x = (offset % 2u) == 0;
            defs[num].is_stick_y = (offset % 2u) != 0;
        }

        if (is_trig) {
            defs[num].is_trigger = TRUE;
        } else if (!defs[num].is_stick_x && !defs[num].is_stick_y) {
            /* Generic non-stick, non-trigger axes are normalized to [-1, 1]. */
            defs[num].is_bipolar = TRUE;
        }
        evdev_init_abs_lut(dev, code, is_trig, &data->abs_lut[code]);
        num++;
    }

    ConttestInputLayout *layout = g_new0(ConttestInputLayout, 1);
    layout->elements     = defs;
    layout->num_elements = num;
    layout->ctx          = ctx;

    viewer_session_context_ref(ctx);
    g_main_context_invoke(NULL, conttest_input_viewer_handle_layout, layout);

    /* Seed the ring with current EV_KEY / EV_ABS hardware state before polling begins. */
    gboolean has_initial = FALSE;
    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (!libevdev_has_event_code(dev, EV_KEY, code))
            continue;
        int val = libevdev_get_event_value(dev, EV_KEY, code);
        if (evdev_push_event(data->abs_lut, ctx, EV_KEY, code, val))
            has_initial = TRUE;
    }
    for (unsigned int code = 0; code <= ABS_MAX; code++) {
        if (!libevdev_has_event_code(dev, EV_ABS, code))
            continue;
        int val = libevdev_get_event_value(dev, EV_ABS, code);
        if (evdev_push_event(data->abs_lut, ctx, EV_ABS, code, val))
            has_initial = TRUE;
    }
    if (has_initial)
        conttest_event_ring_notify(ctx, conttest_input_viewer_drain_ring);

    viewer_session_context_ref(ctx);
    session->thread = g_thread_new("evdev_poll", evdev_polling_thread, data);

    return session;
}
