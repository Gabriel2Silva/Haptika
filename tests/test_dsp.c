#include "dsp_oscillator.h"
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static void verify_sine_case(ConttestDspImpl impl,
                             size_t n,
                             size_t misalign,
                             double phase0,
                             double omega,
                             float gain) {
    const float sentinel = -999.0f;
    size_t alloc_elems = misalign + n + 2;
    float *raw = g_new(float, alloc_elems);
    for (size_t i = 0; i < alloc_elems; i++)
        raw[i] = sentinel;

    float *dst = raw + 1 + misalign;
    double phase = phase0;
    conttest_dsp_generate_sine_impl(dst, n, &phase, omega, gain, impl);

    /* Guard sentinels before and after dst must remain untouched. */
    g_assert_cmpfloat(raw[misalign], ==, sentinel);
    g_assert_cmpfloat(dst[n], ==, sentinel);

    const double two_pi = 2.0 * G_PI;
    for (size_t i = 0; i < n; i++) {
        double angle = fmod(phase0 + (double)i * omega, two_pi);
        if (angle < 0.0)
            angle += two_pi;
        float expected = gain * (float)sin(angle);
        float err = fabsf(dst[i] - expected);
        g_assert_cmpfloat(err, <=, 2e-5f);
    }

    if (n > 0) {
        g_assert_cmpfloat(phase, >=, 0.0);
        g_assert_cmpfloat(phase, <, two_pi);
        double expected_phase = fmod(phase0 + (double)n * omega, two_pi);
        if (expected_phase < 0.0)
            expected_phase += two_pi;
        double diff = fabs(phase - expected_phase);
        if (diff > G_PI)
            diff = two_pi - diff;
        g_assert_cmpfloat(diff, <=, 1e-12);
    } else {
        g_assert_cmpfloat(phase, ==, phase0);
    }

    g_free(raw);
}

static void test_dsp_active_impl(void) {
    ConttestDspImpl active = conttest_dsp_active_impl();
    g_assert_true(active == CONTTEST_DSP_IMPL_C ||
                  active == CONTTEST_DSP_IMPL_SSE2 ||
                  active == CONTTEST_DSP_IMPL_AVX2_FMA);
}

static void test_dsp_accuracy_and_sizes(void) {
    static const size_t sizes[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 63, 255, 1024, 48000
    };
    static const ConttestDspImpl impls[] = {
        CONTTEST_DSP_IMPL_C,
        CONTTEST_DSP_IMPL_SSE2,
        CONTTEST_DSP_IMPL_AVX2_FMA,
    };

    const double omega = (2.0 * G_PI * 440.0) / 48000.0;

    for (size_t k = 0; k < G_N_ELEMENTS(impls); k++) {
        for (size_t s = 0; s < G_N_ELEMENTS(sizes); s++) {
            for (size_t align = 0; align < 4; align++) {
                verify_sine_case(impls[k], sizes[s], align, 0.0, omega, 0.25f);
            }
        }
        verify_sine_case(impls[k], 1025, 1, 5.9, omega, 0.5f);
    }
}

static void test_dsp_chunk_continuity(void) {
    static const ConttestDspImpl impls[] = {
        CONTTEST_DSP_IMPL_C,
        CONTTEST_DSP_IMPL_SSE2,
        CONTTEST_DSP_IMPL_AVX2_FMA,
    };
    static const size_t chunks[] = { 3, 7, 17, 1, 8, 63, 925 };

    const size_t total = 1024;
    const double omega = (2.0 * G_PI * 440.0) / 48000.0;
    float *single = g_new0(float, total);
    float *chunked = g_new0(float, total);

    for (size_t k = 0; k < G_N_ELEMENTS(impls); k++) {
        double phase_single = 0.75;
        conttest_dsp_generate_sine_impl(single, total, &phase_single, omega, 0.2f, impls[k]);

        double phase_chunk = 0.75;
        size_t offset = 0;
        for (size_t c = 0; c < G_N_ELEMENTS(chunks); c++) {
            conttest_dsp_generate_sine_impl(chunked + offset, chunks[c],
                                            &phase_chunk, omega, 0.2f, impls[k]);
            offset += chunks[c];
        }
        g_assert_cmpuint(offset, ==, total);

        for (size_t i = 0; i < total; i++) {
            g_assert_cmpfloat(fabsf(single[i] - chunked[i]), <=, 1e-5f);
        }
        g_assert_cmpfloat(fabs(phase_single - phase_chunk), <=, 1e-11);
    }

    g_free(single);
    g_free(chunked);
}

static void test_dsp_edge_cases(void) {
    const double omega = (2.0 * G_PI * 440.0) / 48000.0;
    double phase = 1.0;

    conttest_dsp_generate_sine(NULL, 10, &phase, omega, 0.5f);
    g_assert_cmpfloat(phase, ==, 1.0);

    float buf[32];
    conttest_dsp_generate_sine(buf, 0, &phase, omega, 0.5f);
    g_assert_cmpfloat(phase, ==, 1.0);

    for (size_t i = 0; i < G_N_ELEMENTS(buf); i++)
        buf[i] = 1.23f;
    conttest_dsp_generate_sine(buf, G_N_ELEMENTS(buf), &phase, omega, 0.0f);
    for (size_t i = 0; i < G_N_ELEMENTS(buf); i++)
        g_assert_cmpfloat(buf[i], ==, 0.0f);

    /* Tiny negative phase near -0.0 must wrap strictly inside [0, 2*pi). */
    const double two_pi = 2.0 * G_PI;
    double neg_tiny = -1e-17;
    conttest_dsp_generate_sine(buf, 8, &neg_tiny, 0.0, 1.0f);
    g_assert_cmpfloat(neg_tiny, >=, 0.0);
    g_assert_cmpfloat(neg_tiny, <, two_pi);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dsp/active_impl", test_dsp_active_impl);
    g_test_add_func("/dsp/accuracy_and_sizes", test_dsp_accuracy_and_sizes);
    g_test_add_func("/dsp/chunk_continuity", test_dsp_chunk_continuity);
    g_test_add_func("/dsp/edge_cases", test_dsp_edge_cases);
    return g_test_run();
}
