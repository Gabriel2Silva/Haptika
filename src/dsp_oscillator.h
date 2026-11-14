#pragma once
#include <stddef.h>

/* Fill `n` mono F32 samples with gain * sin(phase + i * omega), advancing and
 * wrapping `*phase` into [0, 2*pi). */
void conttest_dsp_generate_sine(float *dst, size_t n,
                                double *phase, double omega, float gain);
