#include "input_session.h"
#include <glib.h>
#include <stddef.h>
#include <stdint.h>

static void test_ring_empty_and_types(void) {
    g_assert_cmpuint(sizeof(ConttestInputEvent), ==, 16u);
    g_assert_cmpuint(_Alignof(ConttestInputEvent), ==, 16u);
    g_assert_cmpuint(offsetof(ConttestEventRing, head) -
                     (offsetof(ConttestEventRing, events[CONTTEST_EVENT_RING_CAP - 1]) +
                      sizeof(ConttestInputEvent)),
                     >=, 64u);
    g_assert_cmpuint(offsetof(ConttestEventRing, tail) - offsetof(ConttestEventRing, head), >=, 64u);
    g_assert_cmpuint(offsetof(ConttestEventRing, scheduled) - offsetof(ConttestEventRing, tail), >=, 64u);

    ConttestEventRing ring = {0};
    ConttestInputEvent out = {0};

    g_assert_false(conttest_event_ring_pop(&ring, &out));

    ConttestInputEvent btn = {
        .type = INPUT_EVENT_BUTTON,
        .domain = CONTTEST_ELEMENT_BUTTON,
        .element_id = 3,
        .button_pressed = TRUE,
    };
    ConttestInputEvent axis = {
        .type = INPUT_EVENT_AXIS,
        .domain = CONTTEST_ELEMENT_SDL_AXIS,
        .element_id = 1,
        .axis_value = -0.625,
    };
    ConttestInputEvent tp = {
        .type = INPUT_EVENT_TOUCHPAD,
        .domain = CONTTEST_ELEMENT_TOUCHPAD_FINGER,
        .element_id = 0,
        .button_pressed = TRUE,
        .touch_x = 0.25f,
        .touch_y = 0.75f,
    };
    ConttestInputEvent sensor = {
        .type = INPUT_EVENT_SENSOR,
        .domain = CONTTEST_ELEMENT_SENSOR,
        .element_id = CONTTEST_SENSOR_GYRO,
        .sensor = { 1.25f, -2.5f, 3.75f },
    };

    g_assert_true(conttest_event_ring_push(&ring, &btn));
    g_assert_true(conttest_event_ring_push(&ring, &axis));
    g_assert_true(conttest_event_ring_push(&ring, &tp));
    g_assert_true(conttest_event_ring_push(&ring, &sensor));

    g_assert_true(conttest_event_ring_pop(&ring, &out));
    g_assert_cmpint(out.type, ==, INPUT_EVENT_BUTTON);
    g_assert_cmpint(out.domain, ==, CONTTEST_ELEMENT_BUTTON);
    g_assert_cmpint(out.element_id, ==, 3);
    g_assert_true(out.button_pressed);

    g_assert_true(conttest_event_ring_pop(&ring, &out));
    g_assert_cmpint(out.type, ==, INPUT_EVENT_AXIS);
    g_assert_cmpint(out.domain, ==, CONTTEST_ELEMENT_SDL_AXIS);
    g_assert_cmpint(out.element_id, ==, 1);
    g_assert_cmpfloat(out.axis_value, ==, -0.625);

    g_assert_true(conttest_event_ring_pop(&ring, &out));
    g_assert_cmpint(out.type, ==, INPUT_EVENT_TOUCHPAD);
    g_assert_cmpint(out.domain, ==, CONTTEST_ELEMENT_TOUCHPAD_FINGER);
    g_assert_cmpfloat(out.touch_x, ==, 0.25f);
    g_assert_cmpfloat(out.touch_y, ==, 0.75f);

    g_assert_true(conttest_event_ring_pop(&ring, &out));
    g_assert_cmpint(out.type, ==, INPUT_EVENT_SENSOR);
    g_assert_cmpint(out.domain, ==, CONTTEST_ELEMENT_SENSOR);
    g_assert_cmpint(out.element_id, ==, CONTTEST_SENSOR_GYRO);
    g_assert_cmpfloat(out.sensor[0], ==, 1.25f);
    g_assert_cmpfloat(out.sensor[1], ==, -2.5f);
    g_assert_cmpfloat(out.sensor[2], ==, 3.75f);

    g_assert_false(conttest_event_ring_pop(&ring, &out));
}

static void test_ring_capacity_and_overflow(void) {
    ConttestEventRing ring = {0};
    const int max_items = CONTTEST_EVENT_RING_CAP - 1;

    for (int i = 0; i < max_items; i++) {
        ConttestInputEvent ev = { .type = INPUT_EVENT_BUTTON, .element_id = i };
        g_assert_true(conttest_event_ring_push(&ring, &ev));
    }
    g_assert_false(atomic_load(&ring.overflow));

    ConttestInputEvent extra = { .type = INPUT_EVENT_BUTTON, .element_id = 999999 };
    g_assert_false(conttest_event_ring_push(&ring, &extra));
    g_assert_true(atomic_load(&ring.overflow));
    g_assert_cmpuint(atomic_load(&ring.dropped), ==, 1u);

    /* Further drops keep counting so the drain can report how much was lost. */
    g_assert_false(conttest_event_ring_push(&ring, &extra));
    g_assert_cmpuint(atomic_load(&ring.dropped), ==, 2u);

    /* The consumer's overflow check reports the lost count and requests a
     * backend resync, then clears both flags. */
    g_assert_cmpuint(conttest_event_ring_note_overflow(&ring), ==, 2u);
    g_assert_true(atomic_load(&ring.resync));
    g_assert_false(atomic_load(&ring.overflow));
    g_assert_cmpuint(atomic_load(&ring.dropped), ==, 0u);
    g_assert_cmpuint(conttest_event_ring_note_overflow(&ring), ==, 0u);

    ConttestInputEvent out = {0};
    g_assert_true(conttest_event_ring_pop(&ring, &out));
    g_assert_cmpint(out.element_id, ==, 0);

    ConttestInputEvent tail = { .type = INPUT_EVENT_BUTTON, .element_id = max_items };
    g_assert_true(conttest_event_ring_push(&ring, &tail));

    for (int i = 1; i <= max_items; i++) {
        g_assert_true(conttest_event_ring_pop(&ring, &out));
        g_assert_cmpint(out.element_id, ==, i);
    }
    g_assert_false(conttest_event_ring_pop(&ring, &out));
}

static void test_ring_wraparound(void) {
    ConttestEventRing ring = {0};
    int next_push = 0;
    int next_pop = 0;

    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 500; i++) {
            ConttestInputEvent ev = {
                .type = INPUT_EVENT_AXIS,
                .element_id = next_push,
                .axis_value = (double)next_push * 0.25,
            };
            g_assert_true(conttest_event_ring_push(&ring, &ev));
            next_push++;
        }
        for (int i = 0; i < 500; i++) {
            ConttestInputEvent out = {0};
            g_assert_true(conttest_event_ring_pop(&ring, &out));
            g_assert_cmpint(out.element_id, ==, next_pop);
            g_assert_cmpfloat(out.axis_value, ==, (double)next_pop * 0.25);
            next_pop++;
        }
    }
    g_assert_false(atomic_load(&ring.overflow));
}

typedef struct {
    ConttestEventRing *ring;
    int count;
} ProducerCtx;

static gpointer spsc_producer(gpointer data) {
    ProducerCtx *ctx = data;
    for (int i = 0; i < ctx->count; i++) {
        ConttestInputEvent ev = {
            .type = INPUT_EVENT_AXIS,
            .element_id = i,
            .axis_value = (double)i * 0.5,
        };
        while (!conttest_event_ring_push(ctx->ring, &ev)) {
            g_thread_yield();
        }
    }
    return NULL;
}

static void test_ring_concurrent_spsc(void) {
    ConttestEventRing *ring = g_new0(ConttestEventRing, 1);
    const int total = 200000;
    ProducerCtx ctx = { .ring = ring, .count = total };

    GThread *prod = g_thread_new("spsc-prod", spsc_producer, &ctx);
    for (int i = 0; i < total; i++) {
        ConttestInputEvent out = {0};
        while (!conttest_event_ring_pop(ring, &out)) {
            g_thread_yield();
        }
        g_assert_cmpint(out.type, ==, INPUT_EVENT_AXIS);
        g_assert_cmpint(out.element_id, ==, i);
        g_assert_cmpfloat(out.axis_value, ==, (double)i * 0.5);
    }

    g_thread_join(prod);
    ConttestInputEvent trailing = {0};
    g_assert_false(conttest_event_ring_pop(ring, &trailing));
    g_free(ring);
}

static void test_evdev_axis_classification(void) {
    gboolean z_rz_stick = FALSE, rx_ry_trig = FALSE;

    /* Xbox / xpad: RX/RY signed stick, Z/RZ unsigned triggers. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = -32768, .maximum = 32767, .value = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = -32768, .maximum = 32767, .value = 0 },
        &z_rz_stick, &rx_ry_trig);
    g_assert_false(z_rz_stick);
    g_assert_false(rx_ry_trig);

    /* hid-playstation DS4/DS5: 0..255 with flat>0 on sticks (RX/RY), flat==0 & value==0 on Z/RZ. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0, .flat = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0, .flat = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 128, .flat = 8 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 128, .flat = 8 },
        &z_rz_stick, &rx_ry_trig);
    g_assert_false(z_rz_stick);
    g_assert_false(rx_ry_trig);

    /* hid-sony DS4: 0..255 everywhere, Z/RZ rest centered (~128), RX/RY triggers rest at 0. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 128, .flat = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 128, .flat = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0, .flat = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0, .flat = 0 },
        &z_rz_stick, &rx_ry_trig);
    g_assert_true(z_rz_stick);
    g_assert_true(rx_ry_trig);

    /* hid-sony DS3: only Z/RZ present (resting centered ~128), no RX/RY. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 128 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 127 },
        (ConttestAbsAxisInfo){ .present = false },
        (ConttestAbsAxisInfo){ .present = false },
        &z_rz_stick, &rx_ry_trig);
    g_assert_true(z_rz_stick);
    g_assert_false(rx_ry_trig);

    /* Single-stick pad with unipolar triggers on Z/RZ resting at 0, no RX/RY. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0 },
        (ConttestAbsAxisInfo){ .present = false },
        (ConttestAbsAxisInfo){ .present = false },
        &z_rz_stick, &rx_ry_trig);
    g_assert_false(z_rz_stick);
    g_assert_false(rx_ry_trig);

    /* Static range width: a wide Z/RZ pair against a narrow RX/RY pair marks
     * Z/RZ as the stick even when both are held away from rest at open. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 32000, .value = 31000 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 32000, .value = 500 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0 },
        &z_rz_stick, &rx_ry_trig);
    g_assert_true(z_rz_stick);
    g_assert_true(rx_ry_trig);

    /* A deadzone on Z/RZ only resolves the same layout when ranges match,
     * regardless of the values at open. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 240, .flat = 8 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 20, .flat = 8 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0, .flat = 0 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 0, .flat = 0 },
        &z_rz_stick, &rx_ry_trig);
    g_assert_true(z_rz_stick);
    g_assert_true(rx_ry_trig);

    /* DS3-style Z/RZ pair with a deadzone is a stick even off-centre. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 30, .flat = 8 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 220, .flat = 8 },
        (ConttestAbsAxisInfo){ .present = false },
        (ConttestAbsAxisInfo){ .present = false },
        &z_rz_stick, &rx_ry_trig);
    g_assert_true(z_rz_stick);
    g_assert_false(rx_ry_trig);

    /* Documented limitation: with identical ranges, no deadzone and both pairs
     * held away from rest, the remaining rest-value heuristic cannot classify
     * the layout. The default keeps RX/RY as the stick (hid-playstation, xpad),
     * which is wrong only for the legacy hid-sony DS4 layout. */
    conttest_evdev_classify_axes(
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 200 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 60 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 180 },
        (ConttestAbsAxisInfo){ .present = true, .minimum = 0, .maximum = 255, .value = 70 },
        &z_rz_stick, &rx_ry_trig);
    g_assert_false(z_rz_stick);
    g_assert_false(rx_ry_trig);
}

typedef struct {
    ViewerSessionContext *ctx;
    GMainLoop *loop;
    int target_count;
    int received_count;
    int expected_next;
} CoalesceTestState;

static gboolean coalesce_drain_cb(gpointer data) {
    ViewerSessionContext *ctx = data;
    ConttestEventRing *ring = &ctx->ring;
    CoalesceTestState *st = (CoalesceTestState *)ctx->viewer;

    for (;;) {
        ConttestInputEvent ev;
        while (conttest_event_ring_pop(ring, &ev)) {
            g_assert_cmpint(ev.element_id, ==, st->expected_next);
            st->expected_next++;
            st->received_count++;
        }

        atomic_store_explicit(&ring->scheduled, false, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        unsigned int head = atomic_load_explicit(&ring->head, memory_order_relaxed);
        unsigned int tail = atomic_load_explicit(&ring->tail, memory_order_seq_cst);
        if (head == tail)
            break;
        if (atomic_exchange_explicit(&ring->scheduled, true, memory_order_seq_cst))
            break;
    }

    if (st->received_count >= st->target_count)
        g_main_loop_quit(st->loop);

    viewer_session_context_unref(ctx);
    return G_SOURCE_REMOVE;
}

static gpointer coalesce_producer(gpointer data) {
    CoalesceTestState *st = data;
    for (int i = 0; i < st->target_count; i++) {
        ConttestInputEvent ev = {
            .type = INPUT_EVENT_BUTTON,
            .element_id = i,
        };
        while (!conttest_event_ring_push(&st->ctx->ring, &ev)) {
            g_thread_yield();
        }
        conttest_event_ring_notify(st->ctx, coalesce_drain_cb);
    }
    return NULL;
}

static void test_ring_notify_coalescing(void) {
    CoalesceTestState st = {
        .loop = g_main_loop_new(NULL, FALSE),
        .target_count = 25000,
    };
    st.ctx = viewer_session_context_new((ConttestInputViewer *)&st);
    g_assert_cmpuint((uintptr_t)st.ctx & 63u, ==, 0u);

    GThread *prod = g_thread_new("spsc-notify-prod", coalesce_producer, &st);
    g_main_loop_run(st.loop);
    g_thread_join(prod);

    /* Drain any leftover idle source that raced on the final item */
    while (g_main_context_iteration(NULL, FALSE)) {
    }

    g_assert_cmpint(st.received_count, ==, st.target_count);
    g_assert_cmpint(st.ctx->ref_count, ==, 1);
    viewer_session_context_unref(st.ctx);
    g_main_loop_unref(st.loop);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ring/empty_and_types", test_ring_empty_and_types);
    g_test_add_func("/ring/capacity_and_overflow", test_ring_capacity_and_overflow);
    g_test_add_func("/ring/wraparound", test_ring_wraparound);
    g_test_add_func("/ring/concurrent_spsc", test_ring_concurrent_spsc);
    g_test_add_func("/ring/notify_coalescing", test_ring_notify_coalescing);
    g_test_add_func("/evdev/axis_classification", test_evdev_axis_classification);
    return g_test_run();
}
