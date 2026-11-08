/*
 * Minimal radix-2 complex FFT, private to the doa component.
 *
 * Portable reference implementation. On the S3 this is the obvious place to
 * swap in esp-dsp (dsps_fft2r_fc32) behind the same four functions; the host
 * tests then double as a regression check for the swap.
 */
#ifndef DOA_FFT_H
#define DOA_FFT_H

#include <stdint.h>

typedef struct {
    float re, im;
} doa_cpx_t;

typedef struct {
    int        n;
    doa_cpx_t *tw;   /* e^{-j 2 pi k / n}, k < n/2 */
    uint16_t  *rev;  /* bit-reversal permutation */
} doa_fft_t;

/* n must be a power of two in [2, 65536]. Returns 0 or -1. */
int  doa_fft_init(doa_fft_t *f, int n);
void doa_fft_free(doa_fft_t *f);

/* In place, unnormalised. forward: e^{-j}, inverse: e^{+j} (no 1/n). */
void doa_fft_forward(const doa_fft_t *f, doa_cpx_t *x);
void doa_fft_inverse(const doa_fft_t *f, doa_cpx_t *x);

/* z = FFT(x + j y) for real x, y. Recovers X[k], Y[k] for k = 0..n/2. */
void doa_fft_split_real_pair(int n, const doa_cpx_t *z, doa_cpx_t *X, doa_cpx_t *Y);

#endif /* DOA_FFT_H */
