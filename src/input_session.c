#define _POSIX_C_SOURCE 200809L
#include "input_session.h"
#include <stdlib.h>
#include <string.h>

_Static_assert((CONTTEST_EVENT_RING_CAP & (CONTTEST_EVENT_RING_CAP - 1)) == 0,
               "CONTTEST_EVENT_RING_CAP must be a power of two");

static gboolean ring_drain_source_dispatch(GSource *source,
                                           GSourceFunc callback,
                                           gpointer user_data) {
    (void)callback;
    g_source_set_ready_time(source, -1);
    ViewerSessionContext *ctx = user_data;
    GSourceFunc fn = atomic_load_explicit(&ctx->dispatch_func, memory_order_acquire);
    if (fn)
        fn(ctx);
    /* Do not dereference `ctx` after `fn(ctx)`: `fn` drops the in-flight
     * dispatch reference, which may destroy `source` and free `ctx`. */
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs ring_drain_source_funcs = {
    .dispatch = ring_drain_source_dispatch,
};

gboolean conttest_event_ring_push(ConttestEventRing *ring, const ConttestInputEvent *ev) {
    unsigned int tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    unsigned int next = (tail + 1u) & (CONTTEST_EVENT_RING_CAP - 1u);
    unsigned int head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (next == head) {
        atomic_store_explicit(&ring->overflow, true, memory_order_relaxed);
        atomic_fetch_add_explicit(&ring->dropped, 1u, memory_order_relaxed);
        return FALSE;
    }

    ring->events[tail] = *ev;
    atomic_store_explicit(&ring->tail, next, memory_order_release);
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

unsigned int conttest_event_ring_note_overflow(ConttestEventRing *ring) {
    unsigned int dropped =
        atomic_exchange_explicit(&ring->dropped, 0u, memory_order_relaxed);
    atomic_store_explicit(&ring->overflow, false, memory_order_relaxed);
    if (dropped > 0) {
        /* The consumer side asks the backend to republish its state so a lost
         * release cannot leave an indicator stuck. */
        atomic_store_explicit(&ring->resync, true, memory_order_relaxed);
    }
    return dropped;
}

ViewerSessionContext *viewer_session_context_new(ConttestInputViewer *viewer) {
    void *mem = NULL;
    if (posix_memalign(&mem, 64, sizeof(ViewerSessionContext)) != 0 || !mem) {
        mem = calloc(1, sizeof(ViewerSessionContext));
        if (!mem)
            g_error("Failed to allocate ViewerSessionContext");
    } else {
        memset(mem, 0, sizeof(ViewerSessionContext));
    }

    ViewerSessionContext *ctx = mem;
    ctx->viewer = viewer;
    ctx->active = 1;
    ctx->ref_count = 1;
    atomic_init(&ctx->dispatch_func, NULL);
    atomic_init(&ctx->ring.head, 0u);
    atomic_init(&ctx->ring.tail, 0u);
    atomic_init(&ctx->ring.overflow, false);
    atomic_init(&ctx->ring.scheduled, false);
    atomic_init(&ctx->ring.resync, false);
    atomic_init(&ctx->ring.dropped, 0u);

    GSource *src = g_source_new(&ring_drain_source_funcs, sizeof(GSource));
    g_source_set_priority(src, G_PRIORITY_DEFAULT);
    g_source_set_name(src, "haptika-ring-drain");
    g_source_set_callback(src, NULL, ctx, NULL);
    g_source_set_ready_time(src, -1);
    if (g_source_attach(src, NULL) != 0) {
        ctx->drain_source = src;
    } else {
        g_source_unref(src);
    }

    return ctx;
}

ViewerSessionContext *viewer_session_context_ref(ViewerSessionContext *ctx) {
    g_return_val_if_fail(ctx != NULL, NULL);
    g_atomic_int_inc(&ctx->ref_count);
    return ctx;
}

void viewer_session_context_unref(ViewerSessionContext *ctx) {
    if (!ctx) return;
    if (g_atomic_int_dec_and_test(&ctx->ref_count)) {
        if (ctx->drain_source) {
            g_source_destroy(ctx->drain_source);
            g_source_unref(ctx->drain_source);
            ctx->drain_source = NULL;
        }
        free(ctx);
    }
}

static gboolean abs_is_signed_centered(ConttestAbsAxisInfo a) {
    return a.present && a.minimum < 0 && a.maximum > 0;
}

static gboolean abs_is_nonneg(ConttestAbsAxisInfo a) {
    return a.present && a.minimum >= 0 && a.maximum > a.minimum;
}

static gboolean abs_has_deadzone(ConttestAbsAxisInfo a) {
    return a.present && a.flat > 0;
}

static gboolean abs_pair_has_deadzone(ConttestAbsAxisInfo a, ConttestAbsAxisInfo b) {
    return abs_has_deadzone(a) || abs_has_deadzone(b);
}

static int64_t abs_range_width(ConttestAbsAxisInfo a) {
    if (!a.present || a.maximum <= a.minimum)
        return 0;
    return (int64_t)a.maximum - (int64_t)a.minimum;
}

static int64_t abs_pair_range_width(ConttestAbsAxisInfo a, ConttestAbsAxisInfo b) {
    int64_t width_a = abs_range_width(a);
    int64_t width_b = abs_range_width(b);
    return width_a > width_b ? width_a : width_b;
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
            /* Static hints first: a deadzone marks a stick, and a wider range
             * marks a stick against the narrower trigger range. The rest
             * values are only consulted when both sides are indistinguishable,
             * so holding an axis while plugging in cannot flip an otherwise
             * statically resolved layout. */
            gboolean z_has_flat  = abs_pair_has_deadzone(z, rz);
            gboolean rx_has_flat = abs_pair_has_deadzone(rx, ry);
            if (z_has_flat != rx_has_flat) {
                z_rz_stick = z_has_flat;
                rx_ry_trig = z_has_flat;
            } else {
                int64_t z_range  = abs_pair_range_width(z, rz);
                int64_t rx_range = abs_pair_range_width(rx, ry);
                if (z_range > 0 && rx_range > 0 && z_range != rx_range) {
                    z_rz_stick = z_range > rx_range;
                    rx_ry_trig = z_rz_stick;
                } else if ((!z.present || abs_is_near_center(z)) &&
                           (!rz.present || abs_is_near_center(rz)) &&
                           (!rx.present || abs_is_at_minimum(rx)) &&
                           (!ry.present || abs_is_at_minimum(ry))) {
                    /* hid-sony / generic HID DS4: Z/RZ rest at center, RX/RY
                     * rest at 0. Last resort when the driver exposes no static
                     * difference between the two pairs. */
                    z_rz_stick = TRUE;
                    rx_ry_trig = TRUE;
                }
            }
        }
    } else if (has_z_pair && !has_rx_pair) {
        /* E.g. DS3 via hid-sony exposes Z/RZ as right stick without RX/RY. */
        if (abs_is_signed_centered(z) || abs_is_signed_centered(rz) ||
            abs_pair_has_deadzone(z, rz) ||
            ((!z.present || abs_is_near_center(z)) &&
             (!rz.present || abs_is_near_center(rz)))) {
            z_rz_stick = TRUE;
        }
    }

    if (out_z_rz_right_stick) *out_z_rz_right_stick = z_rz_stick;
    if (out_rx_ry_triggers)   *out_rx_ry_triggers   = rx_ry_trig;
}

void conttest_event_ring_notify(ViewerSessionContext *ctx, GSourceFunc dispatch_func) {
    if (atomic_load_explicit(&ctx->dispatch_func, memory_order_relaxed) != dispatch_func) {
        atomic_store_explicit(&ctx->dispatch_func, dispatch_func, memory_order_release);
    }
    /* Order prior release stores to `ring->tail` before testing `scheduled`. */
    atomic_thread_fence(memory_order_seq_cst);
    if (!atomic_load_explicit(&ctx->ring.scheduled, memory_order_seq_cst) &&
        !atomic_exchange_explicit(&ctx->ring.scheduled, true, memory_order_seq_cst)) {
        viewer_session_context_ref(ctx);
        if (ctx->drain_source) {
            g_source_set_ready_time(ctx->drain_source, 0);
        } else {
            g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_func, ctx, NULL);
        }
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

