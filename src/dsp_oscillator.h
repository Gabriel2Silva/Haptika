#pragma once
#include <stddef.h>

typedef enum {
    CONTTEST_DSP_IMPL_AUTO = 0,
    CONTTEST_DSP_IMPL_C,
    CONTTEST_DSP_IMPL_SSE2,
    CONTTEST_DSP_IMPL_AVX2_FMA
} ConttestDspImpl;

/* Return the best quadrature oscillator backend supported by the current CPU. */
ConttestDspImpl conttest_dsp_active_impl(void);

/* Fill `n` mono F32 samples with gain * sin(phase + i * omega), advancing and
 * wrapping `*phase` into [0, 2*pi). Dispatches via cached CPUID detection. */
void conttest_dsp_generate_sine(float *dst, size_t n,
                                double *phase, double omega, float gain);

/* Generate using an explicit backend implementation (useful for unit tests;
 * unsupported SIMD backends fall back to the active implementation). */
void conttest_dsp_generate_sine_impl(float *dst, size_t n,
                                     double *phase, double omega, float gain,
                                     ConttestDspImpl impl);
