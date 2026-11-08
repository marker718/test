#include "doa_fft.h"

#include <math.h>
#include <stdlib.h>

int doa_fft_init(doa_fft_t *f, int n)
{
    if (!f || n < 2 || n > 65536 || (n & (n - 1)) != 0) {
        return -1;
    }
    f->n = n;
    f->tw = malloc(sizeof(doa_cpx_t) * (size_t)(n / 2));
    f->rev = malloc(sizeof(uint16_t) * (size_t)n);
    if (!f->tw || !f->rev) {
        doa_fft_free(f);
        return -1;
    }

    /* Init-time only: double here is fine and gives exact twiddles. */
    const double pi = 3.14159265358979323846;
    for (int k = 0; k < n / 2; k++) {
        double a = -2.0 * pi * (double)k / (double)n;
        f->tw[k].re = (float)cos(a);
        f->tw[k].im = (float)sin(a);
    }

    int bits = 0;
    while ((1 << bits) < n) {
        bits++;
    }
    for (int i = 0; i < n; i++) {
        unsigned r = 0;
        for (int b = 0; b < bits; b++) {
            r |= (((unsigned)i >> b) & 1u) << (bits - 1 - b);
        }
        f->rev[i] = (uint16_t)r;
    }
    return 0;
}

void doa_fft_free(doa_fft_t *f)
{
    if (!f) {
        return;
    }
    free(f->tw);
    free(f->rev);
    f->tw = NULL;
    f->rev = NULL;
    f->n = 0;
}

void doa_fft_forward(const doa_fft_t *f, doa_cpx_t *x)
{
    const int n = f->n;

    for (int i = 0; i < n; i++) {
        int r = f->rev[i];
        if (r > i) {
            doa_cpx_t t = x[i];
            x[i] = x[r];
            x[r] = t;
        }
    }

    for (int size = 2; size <= n; size <<= 1) {
        const int half = size >> 1;
        const int step = n / size;
        for (int base = 0; base < n; base += size) {
            for (int k = 0; k < half; k++) {
                const doa_cpx_t w = f->tw[k * step];
                doa_cpx_t *a = &x[base + k];
                doa_cpx_t *b = &x[base + k + half];
                const float tr = w.re * b->re - w.im * b->im;
                const float ti = w.re * b->im + w.im * b->re;
                b->re = a->re - tr;
                b->im = a->im - ti;
                a->re += tr;
                a->im += ti;
            }
        }
    }
}

void doa_fft_inverse(const doa_fft_t *f, doa_cpx_t *x)
{
    /* IFFT(x) = conj(FFT(conj(x))), unnormalised. */
    const int n = f->n;
    for (int i = 0; i < n; i++) {
        x[i].im = -x[i].im;
    }
    doa_fft_forward(f, x);
    for (int i = 0; i < n; i++) {
        x[i].im = -x[i].im;
    }
}

void doa_fft_split_real_pair(int n, const doa_cpx_t *z, doa_cpx_t *X, doa_cpx_t *Y)
{
    for (int k = 0; k <= n / 2; k++) {
        const doa_cpx_t a = z[k];
        const doa_cpx_t b = z[(n - k) & (n - 1)]; /* Z[N-k], Z[0] for k = 0 */
        /* X = (a + conj b) / 2 */
        X[k].re = 0.5f * (a.re + b.re);
        X[k].im = 0.5f * (a.im - b.im);
        /* Y = (a - conj b) / (2j) */
        const float dre = a.re - b.re;
        const float dim = a.im + b.im;
        Y[k].re = 0.5f * dim;
        Y[k].im = -0.5f * dre;
    }
}
