/*
 * x86-64 coupled quadrature sine oscillator kernels (System V AMD64 ABI).
 *
 * void conttest_osc_avx2_fma(float *dst, size_t n_vecs,
 *                            const float *init_s, const float *init_c,
 *                            float cos_step, float sin_step);
 *   Generates 8 * n_vecs float32 samples using AVX2 + FMA (8 lanes/iter).
 *
 * void conttest_osc_sse2(float *dst, size_t n_vecs,
 *                        const float *init_s, const float *init_c,
 *                        float cos_step, float sin_step);
 *   Generates 4 * n_vecs float32 samples using baseline SSE2 (4 lanes/iter).
 */

.section .note.GNU-stack,"",@progbits

#if defined(__x86_64__) && !defined(__ILP32__)

#if defined(__CET__) && defined(__has_include)
# if __has_include(<cet.h>)
#  include <cet.h>
# endif
#endif
#ifndef _CET_ENDBR
# define _CET_ENDBR
#endif

.text

.p2align 4
.globl conttest_osc_avx2_fma
.type conttest_osc_avx2_fma, @function
conttest_osc_avx2_fma:
    .cfi_startproc
    _CET_ENDBR
    testq   %rsi, %rsi
    jz      .Lavx2_ret

    /* Broadcast scalar steps BEFORE overwriting ymm0/ymm1 (alias of xmm0/xmm1). */
    vbroadcastss %xmm0, %ymm2       /* ymm2 = cos(8 * omega) */
    vbroadcastss %xmm1, %ymm3       /* ymm3 = sin(8 * omega) */
    vmovups (%rdx), %ymm0           /* ymm0 = s[0..7] */
    vmovups (%rcx), %ymm1           /* ymm1 = c[0..7] */

.p2align 4
.Lavx2_loop:
    vmovups %ymm0, (%rdi)           /* store 8 output samples */

    /* s_next = cos_step * s + sin_step * c */
    vmulps      %ymm3, %ymm1, %ymm4 /* ymm4 = sin_step * c */
    vfmadd231ps %ymm2, %ymm0, %ymm4 /* ymm4 = cos_step * s + sin_step * c */

    /* c_next = cos_step * c - sin_step * s */
    vmulps      %ymm3, %ymm0, %ymm5 /* ymm5 = sin_step * s */
    vfmsub231ps %ymm2, %ymm1, %ymm5 /* ymm5 = cos_step * c - sin_step * s */

    vmovaps %ymm4, %ymm0
    vmovaps %ymm5, %ymm1
    addq    $32, %rdi
    decq    %rsi
    jnz     .Lavx2_loop

    vzeroupper
.Lavx2_ret:
    ret
    .cfi_endproc
.size conttest_osc_avx2_fma, .-conttest_osc_avx2_fma

.p2align 4
.globl conttest_osc_sse2
.type conttest_osc_sse2, @function
conttest_osc_sse2:
    .cfi_startproc
    _CET_ENDBR
    testq   %rsi, %rsi
    jz      .Lsse2_ret

    /* Broadcast scalar steps BEFORE loading xmm0/xmm1 lane vectors. */
    shufps  $0x00, %xmm0, %xmm0
    movaps  %xmm0, %xmm2            /* xmm2 = cos(4 * omega) */
    shufps  $0x00, %xmm1, %xmm1
    movaps  %xmm1, %xmm3            /* xmm3 = sin(4 * omega) */
    movups  (%rdx), %xmm0           /* xmm0 = s[0..3] */
    movups  (%rcx), %xmm1           /* xmm1 = c[0..3] */

.p2align 4
.Lsse2_loop:
    movups  %xmm0, (%rdi)           /* store 4 output samples */

    /* s_next = cos_step * s + sin_step * c */
    movaps  %xmm0, %xmm4
    mulps   %xmm2, %xmm4            /* xmm4 = cos_step * s */
    movaps  %xmm1, %xmm5
    mulps   %xmm3, %xmm5            /* xmm5 = sin_step * c */
    addps   %xmm5, %xmm4            /* xmm4 = s_next */

    /* c_next = cos_step * c - sin_step * s */
    mulps   %xmm2, %xmm1            /* xmm1 = cos_step * c */
    mulps   %xmm3, %xmm0            /* xmm0 = sin_step * s */
    subps   %xmm0, %xmm1            /* xmm1 = c_next */
    movaps  %xmm4, %xmm0            /* xmm0 = s_next */

    addq    $16, %rdi
    decq    %rsi
    jnz     .Lsse2_loop

.Lsse2_ret:
    ret
    .cfi_endproc
.size conttest_osc_sse2, .-conttest_osc_sse2

#endif /* __x86_64__ && !__ILP32__ */
