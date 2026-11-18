#include "input_session.h"

_Static_assert((CONTTEST_EVENT_RING_CAP & (CONTTEST_EVENT_RING_CAP - 1)) == 0,
               "CONTTEST_EVENT_RING_CAP must be a power of two");

gboolean conttest_event_ring_push(ConttestEventRing *ring, const ConttestInputEvent *ev) {
    unsigned int tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    unsigned int next = (tail + 1u) & (CONTTEST_EVENT_RING_CAP - 1u);
    unsigned int head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (next == head) {
        atomic_store_explicit(&ring->overflow, true, memory_order_relaxed);
        return FALSE;
    }

    ring->events[tail] = *ev;
    atomic_store_explicit(&ring->tail, next, memory_order_seq_cst);
    return TRUE;
}

gboolean conttest_event_ring_pop(ConttestEventRing *ring, ConttestInputEvent *out) {
    unsigned int head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    unsigned int tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (head == tail)
        return FALSE;

    *out = ring->events[head];
    atomic_store_explicit(&ring->head, (head + 1u) & (CONTTEST_EVENT_RING_CAP - 1u),
                          memory_order_release);
    return TRUE;
}

ViewerSessionContext *viewer_session_context_new(ConttestInputViewer *viewer) {
    ViewerSessionContext *ctx = g_new0(ViewerSessionContext, 1);
    ctx->viewer = viewer;
    ctx->active = 1;
    ctx->ref_count = 1;
    atomic_init(&ctx->ring.head, 0u);
    atomic_init(&ctx->ring.tail, 0u);
    atomic_init(&ctx->ring.overflow, false);
    atomic_init(&ctx->ring.scheduled, false);
    return ctx;
}

ViewerSessionContext *viewer_session_context_ref(ViewerSessionContext *ctx) {
    g_return_val_if_fail(ctx != NULL, NULL);
    g_atomic_int_inc(&ctx->ref_count);
    return ctx;
}

void viewer_session_context_unref(ViewerSessionContext *ctx) {
    if (!ctx) return;
    if (g_atomic_int_dec_and_test(&ctx->ref_count))
        g_free(ctx);
}

static gboolean abs_is_signed_centered(ConttestAbsAxisInfo a) {
    return a.present && a.minimum < 0 && a.maximum > 0;
}

static gboolean abs_is_nonneg(ConttestAbsAxisInfo a) {
    return a.present && a.minimum >= 0 && a.maximum > a.minimum;
}

static gboolean abs_is_at_minimum(ConttestAbsAxisInfo a) {
    if (!a.present || a.maximum <= a.minimum)
        return FALSE;
    int64_t range = (int64_t)a.maximum - (int64_t)a.minimum;
    int64_t delta = (int64_t)a.value - (int64_t)a.minimum;
    return delta * 20 < range * 3; /* within bottom 15% of range */
}

static gboolean abs_is_near_center(ConttestAbsAxisInfo a) {
    if (!a.present || a.maximum <= a.minimum)
        return FALSE;
    int64_t range = (int64_t)a.maximum - (int64_t)a.minimum;
    int64_t off2 = 2 * ((int64_t)a.value - (int64_t)a.minimum) - range;
    if (off2 < 0) off2 = -off2;
    return off2 * 2 <= range; /* within middle 50% (25%..75%) */
}

void conttest_evdev_classify_axes(ConttestAbsAxisInfo z,
                                  ConttestAbsAxisInfo rz,
                                  ConttestAbsAxisInfo rx,
                                  ConttestAbsAxisInfo ry,
                                  gboolean *out_z_rz_right_stick,
                                  gboolean *out_rx_ry_triggers) {
    gboolean z_rz_stick = FALSE;
    gboolean rx_ry_trig = FALSE;

    gboolean has_z_pair = z.present || rz.present;
    gboolean has_rx_pair = rx.present || ry.present;

    if (has_z_pair && has_rx_pair) {
        if ((abs_is_signed_centered(rx) || abs_is_signed_centered(ry)) &&
            (!z.present || abs_is_nonneg(z)) && (!rz.present || abs_is_nonneg(rz))) {
            /* Standard Linux gamepad / xpad: RX/RY signed stick, Z/RZ unsigned triggers. */
        } else if ((abs_is_signed_centered(z) || abs_is_signed_centered(rz)) &&
                   (!rx.present || abs_is_nonneg(rx)) && (!ry.present || abs_is_nonneg(ry))) {
            z_rz_stick = TRUE;
            rx_ry_trig = TRUE;
        } else {
            gboolean rx_has_flat = (rx.present && rx.flat > 0) || (ry.present && ry.flat > 0);
            gboolean z_has_flat  = (z.present && z.flat > 0)   || (rz.present && rz.flat > 0);
            if (rx_has_flat && !z_has_flat) {
                /* hid-playstation exposes flat deadzone on sticks only, not triggers. */
            } else if (z_has_flat && !rx_has_flat) {
                z_rz_stick = TRUE;
                rx_ry_trig = TRUE;
            } else if ((!z.present || abs_is_near_center(z)) &&
                       (!rz.present || abs_is_near_center(rz)) &&
                       (!rx.present || abs_is_at_minimum(rx)) &&
                       (!ry.present || abs_is_at_minimum(ry))) {
                /* hid-sony / generic HID DS4: Z/RZ rest at center, RX/RY rest at 0. */
                z_rz_stick = TRUE;
                rx_ry_trig = TRUE;
            }
        }
    } else if (has_z_pair && !has_rx_pair) {
        /* E.g. DS3 via hid-sony exposes Z/RZ as right stick without RX/RY. */
        if (abs_is_signed_centered(z) || abs_is_signed_centered(rz) ||
            ((!z.present || abs_is_near_center(z)) &&
             (!rz.present || abs_is_near_center(rz)))) {
            z_rz_stick = TRUE;
        }
    }

    if (out_z_rz_right_stick) *out_z_rz_right_stick = z_rz_stick;
    if (out_rx_ry_triggers)   *out_rx_ry_triggers   = rx_ry_trig;
}

void conttest_event_ring_notify(ViewerSessionContext *ctx, GSourceFunc dispatch_func) {
    if (!atomic_exchange_explicit(&ctx->ring.scheduled, true, memory_order_seq_cst)) {
        viewer_session_context_ref(ctx);
        g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_func, ctx, NULL);
    }
}

void conttest_input_session_stop(ConttestInputSession *session) {
    if (!session) return;
    g_atomic_int_set(&session->atomic_stop_flag, 1);
    if (session->thread) {
        g_thread_join(session->thread);
    }
    if (session->free_func) {
        session->free_func(session);
    }
    g_free(session);
}

