#include "dsp_oscillator.h"

#include <math.h>
#include <stdatomic.h>

#if defined(__x86_64__) && !defined(__ILP32__)
#include <cpuid.h>

void conttest_osc_avx2_fma(float *dst, size_t n_vecs,
                           const float *init_s, const float *init_c,
                           float cos_step, float sin_step);

void conttest_osc_sse2(float *dst, size_t n_vecs,
                       const float *init_s, const float *init_c,
                       float cos_step, float sin_step);
#endif

static const double CONTTEST_TWO_PI = 6.283185307179586476925286766559;

/* Re-seed exact double phase per block to prevent single-precision drift. */
#define CONTTEST_DSP_BLOCK_FRAMES 512u

static inline double wrap_phase(double phase) {
    phase = fmod(phase, CONTTEST_TWO_PI);
    if (phase < 0.0)
        phase += CONTTEST_TWO_PI;
    if (phase >= CONTTEST_TWO_PI || !isfinite(phase))
        phase = 0.0;
    return phase;
}

static ConttestDspImpl detect_best_impl(void) {
#if defined(__x86_64__) && !defined(__ILP32__)
    unsigned int max_leaf = __get_cpuid_max(0, NULL);
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

    if (max_leaf >= 7 && __get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        const unsigned int want_ecx = (1u << 12) | /* FMA */
                                      (1u << 26) | /* XSAVE */
                                      (1u << 27) | /* OSXSAVE */
                                      (1u << 28);  /* AVX */
        if ((ecx & want_ecx) == want_ecx) {
            unsigned int xcr0_lo = 0, xcr0_hi = 0;
            __asm__ volatile ("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
            (void)xcr0_hi;
            if ((xcr0_lo & 0x6u) == 0x6u &&
                __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) &&
                (ebx & (1u << 5)) != 0u) { /* AVX2 */
                return CONTTEST_DSP_IMPL_AVX2_FMA;
            }
        }
    }
    return CONTTEST_DSP_IMPL_SSE2;
#else
    return CONTTEST_DSP_IMPL_C;
#endif
}

ConttestDspImpl conttest_dsp_active_impl(void) {
    static atomic_int cached = 0;
    int val = atomic_load_explicit(&cached, memory_order_acquire);
    if (val == 0) {
        val = (int)detect_best_impl();
        atomic_store_explicit(&cached, val, memory_order_release);
    }
    return (ConttestDspImpl)val;
}

static void seed_lanes(double phase, double omega, double gain,
                       size_t width, float *init_s, float *init_c) {
    double s = sin(phase) * gain;
    double c = cos(phase) * gain;
    double dw_c = cos(omega);
    double dw_s = sin(omega);

    for (size_t j = 0; j < width; j++) {
        init_s[j] = (float)s;
        init_c[j] = (float)c;
        double s_next = s * dw_c + c * dw_s;
        c = c * dw_c - s * dw_s;
        s = s_next;
    }
}

static void generate_c_quadrature(float *dst, size_t n,
                                  double *phase, double omega, float gain) {
    double p = *phase;
    const double dw_c = cos(omega);
    const double dw_s = sin(omega);
    const double dgain = (double)gain;

    while (n > 0) {
        size_t chunk = (n < CONTTEST_DSP_BLOCK_FRAMES) ? n : CONTTEST_DSP_BLOCK_FRAMES;
        double s = sin(p) * dgain;
        double c = cos(p) * dgain;

        for (size_t i = 0; i < chunk; i++) {
            dst[i] = (float)s;
            double s_next = s * dw_c + c * dw_s;
            c = c * dw_c - s * dw_s;
            s = s_next;
        }

        dst += chunk;
        n -= chunk;
        p = wrap_phase(p + (double)chunk * omega);
    }

    *phase = p;
}

#if defined(__x86_64__) && !defined(__ILP32__)
typedef void (*ConttestSimdKernel)(float *dst, size_t n_vecs,
                                   const float *init_s, const float *init_c,
                                   float cos_step, float sin_step);

static void run_simd_blocks(float **dst_io, size_t *n_io, double *p_io,
                            double omega, float gain, size_t width,
                            ConttestSimdKernel kernel) {
    float *dst = *dst_io;
    size_t n = *n_io;
    double p = *p_io;
    const float cos_step = (float)cos((double)width * omega);
    const float sin_step = (float)sin((double)width * omega);

    while (n >= width) {
        size_t chunk = (n < CONTTEST_DSP_BLOCK_FRAMES) ? n : CONTTEST_DSP_BLOCK_FRAMES;
        size_t n_vecs = chunk / width;
        size_t done = n_vecs * width;
        float init_s[8];
        float init_c[8];

        seed_lanes(p, omega, (double)gain, width, init_s, init_c);
        kernel(dst, n_vecs, init_s, init_c, cos_step, sin_step);

        dst += done;
        n -= done;
        p = wrap_phase(p + (double)done * omega);
    }

    *dst_io = dst;
    *n_io = n;
    *p_io = p;
}
#endif

void conttest_dsp_generate_sine_impl(float *dst, size_t n,
                                     double *phase, double omega, float gain,
                                     ConttestDspImpl impl) {
    if (!dst || n == 0 || !phase)
        return;

    ConttestDspImpl active = conttest_dsp_active_impl();
    if (impl == CONTTEST_DSP_IMPL_AUTO)
        impl = active;

    double p = wrap_phase(*phase);

#if defined(__x86_64__) && !defined(__ILP32__)
    if (impl == CONTTEST_DSP_IMPL_AVX2_FMA && active == CONTTEST_DSP_IMPL_AVX2_FMA) {
        run_simd_blocks(&dst, &n, &p, omega, gain, 8, conttest_osc_avx2_fma);
    } else if (impl == CONTTEST_DSP_IMPL_SSE2 ||
               (impl == CONTTEST_DSP_IMPL_AVX2_FMA && active == CONTTEST_DSP_IMPL_SSE2)) {
        run_simd_blocks(&dst, &n, &p, omega, gain, 4, conttest_osc_sse2);
    }
#endif

    if (n > 0)
        generate_c_quadrature(dst, n, &p, omega, gain);

    *phase = p;
}

void conttest_dsp_generate_sine(float *dst, size_t n,
                                double *phase, double omega, float gain) {
    conttest_dsp_generate_sine_impl(dst, n, phase, omega, gain, CONTTEST_DSP_IMPL_AUTO);
}
