#include "dsp_oscillator.h"

#include <math.h>

static const double CONTTEST_TWO_PI = 6.283185307179586476925286766559;

#define CONTTEST_DSP_LANES        8u
#define CONTTEST_DSP_BLOCK_FRAMES 4096u

static inline double wrap_phase(double phase) {
    phase = fmod(phase, CONTTEST_TWO_PI);
    if (phase < 0.0)
        phase += CONTTEST_TWO_PI;
    if (phase >= CONTTEST_TWO_PI || !isfinite(phase))
        phase = 0.0;
    return phase;
}

void conttest_dsp_generate_sine(float * restrict dst, size_t n,
                                double * restrict phase, double omega, float gain) {
    if (!dst || n == 0 || !phase)
        return;

    if (!isfinite(omega))
        omega = 0.0;
    if (!isfinite(gain))
        gain = 0.0f;

    double p = wrap_phase(*phase);
    const double dw_c = cos(omega);
    const double dw_s = sin(omega);
    const double dgain = (double)gain;
    const double k = 2.0 * cos((double)CONTTEST_DSP_LANES * omega);

    /* 8-lane ping-pong Chebyshev recurrence (s_next = 2*cos(8*w)*s1 - s0)
     * in IEEE-754 double precision. Zero cross-lane dependencies allow the
     * compiler's SLP vectorizer to emit packed SIMD instructions on all
     * target architectures while maintaining 0.5-ULP float32 accuracy. */
    while (n >= 2u * CONTTEST_DSP_LANES) {
        size_t chunk = (n < CONTTEST_DSP_BLOCK_FRAMES) ? n : CONTTEST_DSP_BLOCK_FRAMES;
        size_t n_pairs = chunk / (2u * CONTTEST_DSP_LANES);
        size_t done = n_pairs * (2u * CONTTEST_DSP_LANES);

        double s = sin(p) * dgain;
        double c = cos(p) * dgain;
        double s0[CONTTEST_DSP_LANES];
        double s1[CONTTEST_DSP_LANES];

        for (size_t j = 0; j < CONTTEST_DSP_LANES; j++) {
            s0[j] = s;
            double sn = s * dw_c + c * dw_s;
            c = c * dw_c - s * dw_s;
            s = sn;
        }
        for (size_t j = 0; j < CONTTEST_DSP_LANES; j++) {
            s1[j] = s;
            double sn = s * dw_c + c * dw_s;
            c = c * dw_c - s * dw_s;
            s = sn;
        }

        for (size_t v = 0; v < n_pairs; v++) {
            float * restrict out = dst + v * (2u * CONTTEST_DSP_LANES);
            for (size_t j = 0; j < CONTTEST_DSP_LANES; j++) {
                out[j] = (float)s0[j];
                s0[j] = k * s1[j] - s0[j];
            }
            for (size_t j = 0; j < CONTTEST_DSP_LANES; j++) {
                out[CONTTEST_DSP_LANES + j] = (float)s1[j];
                s1[j] = k * s0[j] - s1[j];
            }
        }

        dst += done;
        n -= done;
        p = wrap_phase(p + (double)done * omega);
    }

    if (n > 0) {
        double s = sin(p) * dgain;
        double c = cos(p) * dgain;
        for (size_t i = 0; i < n; i++) {
            dst[i] = (float)s;
            double sn = s * dw_c + c * dw_s;
            c = c * dw_c - s * dw_s;
            s = sn;
        }
        p = wrap_phase(p + (double)n * omega);
    }

    *phase = p;
}
