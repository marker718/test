#include "doa.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doa_fft.h"

#define DOA_MAX_PAIRS ((DOA_MAX_CH * (DOA_MAX_CH - 1)) / 2)
#define DOA_PI_F      3.14159265358979f
#define DOA_MAX_RINGS 8
#define DOA_RING_STEP 0.2f   /* max spacing in cos(el) between scale rings */
#define DOA_COARSE_DEG 5.0f  /* coarse azimuth step; PHAT lobe is ~10+ deg wide */

struct doa {
    doa_config_t cfg;
    int M;          /* channels */
    int P;          /* pairs */
    int N;          /* frame length */
    int k_lo, k_hi; /* inclusive GCC band, 1 <= k_lo <= k_hi <= N/2 - 1 */
    int L;          /* max lag */
    int G;          /* azimuth grid size */
    int S;          /* coarse azimuth stride, grid points */
    int E;          /* in-plane scale rings */
    float rho[DOA_MAX_RINGS]; /* cos(el) per ring, rho[0] = 1 */
    float rho_half; /* half ring spacing, for the fine scale pick */
    float gcc_scale;

    doa_fft_t fft;
    float *window;    /* N */
    doa_cpx_t *work;  /* N */
    doa_cpx_t *spec;  /* M * (N/2 + 1) */
    float *gcc;       /* P * (2L + 1) */
    float *tau;       /* G * P, expected TDOA in samples */
    float *srp;       /* G */
    doa_cpx_t *rot;   /* M * (N/2 + 1), delay-trim phase rotation; NULL until used */
    float ch_delay[DOA_MAX_CH];
    int any_delay;

    int pair_i[DOA_MAX_PAIRS];
    int pair_j[DOA_MAX_PAIRS];
};

doa_config_t doa_default_config(void)
{
    doa_config_t c;
    memset(&c, 0, sizeof(c));
    c.num_channels = 4;
    const float a = 0.030f; /* half-side of the 60 mm square */
    /* CCW starting at +45 deg. */
    c.mic_xy[0][0] = +a; c.mic_xy[0][1] = +a;
    c.mic_xy[1][0] = -a; c.mic_xy[1][1] = +a;
    c.mic_xy[2][0] = -a; c.mic_xy[2][1] = -a;
    c.mic_xy[3][0] = +a; c.mic_xy[3][1] = -a;
    c.fs_hz = 48000.0f;
    c.frame_len = 1024;
    c.c_mps = 343.0f;
    c.f_lo_hz = 200.0f;
    c.f_hi_hz = 8000.0f;
    c.az_grid = 360;
    c.el_max_deg = 70.0f;
    return c;
}

static int config_valid(const doa_config_t *c)
{
    if (!c) return 0;
    if (c->num_channels < 2 || c->num_channels > DOA_MAX_CH) return 0;
    if (c->frame_len < 64 || c->frame_len > 4096) return 0;
    if ((c->frame_len & (c->frame_len - 1)) != 0) return 0;
    if (!(c->fs_hz > 0.0f) || !(c->c_mps > 0.0f)) return 0;
    if (!(c->f_lo_hz > 0.0f) || !(c->f_hi_hz > c->f_lo_hz)) return 0;
    if (!(c->f_hi_hz < 0.5f * c->fs_hz)) return 0;
    if (c->az_grid < 36 || c->az_grid > 3600) return 0;
    if (!(c->el_max_deg >= 0.0f && c->el_max_deg <= 80.0f)) return 0;
    for (int m = 0; m < c->num_channels; m++) {
        if (!isfinite(c->mic_xy[m][0]) || !isfinite(c->mic_xy[m][1])) return 0;
    }
    return 1;
}

void doa_destroy(doa_t *d)
{
    if (!d) return;
    doa_fft_free(&d->fft);
    free(d->window);
    free(d->work);
    free(d->spec);
    free(d->gcc);
    free(d->tau);
    free(d->srp);
    free(d->rot);
    free(d);
}

doa_t *doa_create(const doa_config_t *cfg)
{
    if (!config_valid(cfg)) return NULL;

    doa_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cfg = *cfg;
    d->M = cfg->num_channels;
    d->N = cfg->frame_len;
    d->G = cfg->az_grid;
    d->S = (int)lroundf((float)d->G * DOA_COARSE_DEG / 360.0f);
    if (d->S < 1) d->S = 1;

    /* Scale rings, uniform in cos(el) from 1 down to cos(el_max). */
    const float rho_min = cosf(cfg->el_max_deg * DOA_PI_F / 180.0f);
    if (cfg->el_max_deg <= 0.0f) {
        d->E = 1;
    } else {
        d->E = (int)ceilf((1.0f - rho_min) / DOA_RING_STEP) + 1;
    }
    if (d->E > DOA_MAX_RINGS) d->E = DOA_MAX_RINGS;
    const float spacing = (d->E > 1) ? (1.0f - rho_min) / (float)(d->E - 1) : 0.0f;
    for (int e = 0; e < d->E; e++) d->rho[e] = 1.0f - (float)e * spacing;
    d->rho_half = 0.5f * spacing;

    const int N = d->N;
    const float df = cfg->fs_hz / (float)N;
    d->k_lo = (int)ceilf(cfg->f_lo_hz / df);
    d->k_hi = (int)floorf(cfg->f_hi_hz / df);
    if (d->k_lo < 1) d->k_lo = 1;
    if (d->k_hi > N / 2 - 1) d->k_hi = N / 2 - 1;
    if (d->k_hi < d->k_lo) goto fail;
    /* PHAT bins have unit magnitude; both +k and -k are populated, so the
     * unnormalised IFFT of a perfectly coherent pair peaks at 2*nbins. */
    d->gcc_scale = 1.0f / (2.0f * (float)(d->k_hi - d->k_lo + 1));

    /* Pairs and max lag. */
    float max_dist = 0.0f;
    int p = 0;
    for (int i = 0; i < d->M; i++) {
        for (int j = i + 1; j < d->M; j++) {
            d->pair_i[p] = i;
            d->pair_j[p] = j;
            const float dx = cfg->mic_xy[j][0] - cfg->mic_xy[i][0];
            const float dy = cfg->mic_xy[j][1] - cfg->mic_xy[i][1];
            const float dist = sqrtf(dx * dx + dy * dy);
            if (dist > max_dist) max_dist = dist;
            p++;
        }
    }
    d->P = p;
    /* +2 lags of headroom for the 4-tap cubic interpolator. */
    d->L = (int)ceilf(max_dist / cfg->c_mps * cfg->fs_hz) + 2;
    if (max_dist <= 0.0f || d->L >= N / 4) goto fail; /* coincident mics or array too big for frame */

    if (doa_fft_init(&d->fft, N) != 0) goto fail;
    d->window = malloc(sizeof(float) * (size_t)N);
    d->work = malloc(sizeof(doa_cpx_t) * (size_t)N);
    d->spec = malloc(sizeof(doa_cpx_t) * (size_t)(d->M * (N / 2 + 1)));
    d->gcc = calloc((size_t)(d->P * (2 * d->L + 1)), sizeof(float));
    d->tau = malloc(sizeof(float) * (size_t)(d->G * d->P));
    d->srp = calloc((size_t)d->G, sizeof(float));
    if (!d->window || !d->work || !d->spec || !d->gcc || !d->tau || !d->srp) goto fail;

    for (int n = 0; n < N; n++) {
        d->window[n] = 0.5f - 0.5f * cosf(2.0f * DOA_PI_F * (float)n / (float)N);
    }

    /* Far-field TDOA table. Plane wave from unit direction u: mic m hears it
     * at t_m = -(p_m . u) / c. GCC of (i, j) peaks at t_i - t_j
     * = (p_j - p_i) . u / c. */
    for (int g = 0; g < d->G; g++) {
        const float az = 2.0f * DOA_PI_F * (float)g / (float)d->G;
        const float ux = cosf(az), uy = sinf(az);
        for (int q = 0; q < d->P; q++) {
            const int i = d->pair_i[q], j = d->pair_j[q];
            const float dx = cfg->mic_xy[j][0] - cfg->mic_xy[i][0];
            const float dy = cfg->mic_xy[j][1] - cfg->mic_xy[i][1];
            d->tau[g * d->P + q] = (dx * ux + dy * uy) / cfg->c_mps * cfg->fs_hz;
        }
    }
    return d;

fail:
    doa_destroy(d);
    return NULL;
}

int doa_set_channel_delay(doa_t *d, int ch, float delay_samples)
{
    if (!d || ch < 0 || ch >= d->M || !isfinite(delay_samples)) return -1;
    const int nb = d->N / 2 + 1;
    if (!d->rot) {
        d->rot = malloc(sizeof(doa_cpx_t) * (size_t)(d->M * nb));
        if (!d->rot) return -1;
        for (int i = 0; i < d->M * nb; i++) {
            d->rot[i].re = 1.0f;
            d->rot[i].im = 0.0f;
        }
    }
    d->ch_delay[ch] = delay_samples;
    /* A channel delayed by D samples carries e^{-j w D}; undo with e^{+j w D}. */
    for (int k = 0; k < nb; k++) {
        const float w = 2.0f * DOA_PI_F * (float)k / (float)d->N;
        d->rot[ch * nb + k].re = cosf(w * delay_samples);
        d->rot[ch * nb + k].im = sinf(w * delay_samples);
    }
    d->any_delay = 0;
    for (int m = 0; m < d->M; m++) {
        if (d->ch_delay[m] != 0.0f) d->any_delay = 1;
    }
    return 0;
}

static inline doa_cpx_t phat_cross(doa_cpx_t a, doa_cpx_t b)
{
    /* a * conj(b), normalised to unit magnitude. */
    doa_cpx_t g;
    g.re = a.re * b.re + a.im * b.im;
    g.im = a.im * b.re - a.re * b.im;
    const float mag2 = g.re * g.re + g.im * g.im;
    if (mag2 > 1e-30f) {
        const float inv = 1.0f / sqrtf(mag2);
        g.re *= inv;
        g.im *= inv;
    } else {
        g.re = 0.0f;
        g.im = 0.0f;
    }
    return g;
}

/* Catmull-Rom cubic interpolation of r at fractional index x.
 * Caller guarantees 1 <= floor(x) and floor(x) + 2 <= len - 1. */
static inline float interp_cubic(const float *r, float x)
{
    const int i = (int)floorf(x);
    const float t = x - (float)i;
    const float p0 = r[i - 1], p1 = r[i], p2 = r[i + 1], p3 = r[i + 2];
    return p1 + 0.5f * t * (p2 - p0 +
                t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                t * (3.0f * (p1 - p2) + p3 - p0)));
}

/* SRP at grid azimuth g for in-plane scale rho (= cos el). */
static inline float srp_at(const doa_t *d, int g, float rho)
{
    const int W = 2 * d->L + 1;
    const float *tau = &d->tau[g * d->P];
    const float off = (float)d->L;
    float s = 0.0f;
    for (int q = 0; q < d->P; q++) {
        s += interp_cubic(&d->gcc[q * W], rho * tau[q] + off);
    }
    return s;
}

int doa_process(doa_t *d, const float *const *ch, doa_result_t *out)
{
    if (!d || !ch || !out) return -1;
    for (int m = 0; m < d->M; m++) {
        if (!ch[m]) return -1;
    }

    const int N = d->N;
    const int nb = N / 2 + 1;
    const int L = d->L;
    const int W = 2 * L + 1;

    /* 1. Windowed spectra, two real channels per complex FFT. */
    for (int m = 0; m < d->M; m += 2) {
        const float *x = ch[m];
        const float *y = (m + 1 < d->M) ? ch[m + 1] : NULL;
        for (int n = 0; n < N; n++) {
            d->work[n].re = x[n] * d->window[n];
            d->work[n].im = y ? y[n] * d->window[n] : 0.0f;
        }
        doa_fft_forward(&d->fft, d->work);
        if (y) {
            doa_fft_split_real_pair(N, d->work, &d->spec[m * nb], &d->spec[(m + 1) * nb]);
        } else {
            /* Odd channel count: imaginary input was zero, so X = Z. */
            memcpy(&d->spec[m * nb], d->work, sizeof(doa_cpx_t) * (size_t)nb);
        }
    }

    /* 2. Calibration trim. */
    if (d->any_delay) {
        for (int m = 0; m < d->M; m++) {
            if (d->ch_delay[m] == 0.0f) continue;
            doa_cpx_t *X = &d->spec[m * nb];
            const doa_cpx_t *R = &d->rot[m * nb];
            for (int k = d->k_lo; k <= d->k_hi; k++) {
                const float re = X[k].re * R[k].re - X[k].im * R[k].im;
                const float im = X[k].re * R[k].im + X[k].im * R[k].re;
                X[k].re = re;
                X[k].im = im;
            }
        }
    }

    /* 3. GCC-PHAT, two pairs per complex IFFT. Each cross spectrum is
     * Hermitian (real GCC), so r_a + j r_b = IFFT(G_a + j G_b). */
    for (int q = 0; q < d->P; q += 2) {
        const int has_b = (q + 1 < d->P);
        memset(d->work, 0, sizeof(doa_cpx_t) * (size_t)N);
        const doa_cpx_t *Xa = &d->spec[d->pair_i[q] * nb];
        const doa_cpx_t *Ya = &d->spec[d->pair_j[q] * nb];
        const doa_cpx_t *Xb = has_b ? &d->spec[d->pair_i[q + 1] * nb] : NULL;
        const doa_cpx_t *Yb = has_b ? &d->spec[d->pair_j[q + 1] * nb] : NULL;
        for (int k = d->k_lo; k <= d->k_hi; k++) {
            const doa_cpx_t ga = phat_cross(Xa[k], Ya[k]);
            doa_cpx_t gb = {0.0f, 0.0f};
            if (has_b) gb = phat_cross(Xb[k], Yb[k]);
            /* bin k:   ga + j gb */
            d->work[k].re = ga.re - gb.im;
            d->work[k].im = ga.im + gb.re;
            /* bin N-k: conj(ga) + j conj(gb) */
            d->work[N - k].re = ga.re + gb.im;
            d->work[N - k].im = -ga.im + gb.re;
        }
        doa_fft_inverse(&d->fft, d->work);
        float *ra = &d->gcc[q * W];
        float *rb = has_b ? &d->gcc[(q + 1) * W] : NULL;
        for (int l = -L; l <= L; l++) {
            const doa_cpx_t v = d->work[(l + N) & (N - 1)];
            ra[l + L] = v.re * d->gcc_scale;
            if (rb) rb[l + L] = v.im * d->gcc_scale;
        }
    }

    /* 4a. Coarse: stride-S azimuths x all scale rings. */
    int best = 0, best_e = 0;
    float best_v = -1e30f;
    for (int e = 0; e < d->E; e++) {
        for (int g = 0; g < d->G; g += d->S) {
            const float s = srp_at(d, g, d->rho[e]);
            if (s > best_v) {
                best_v = s;
                best = g;
                best_e = e;
            }
        }
    }

    /* 4b. Fine scale pick: ring and +/- half spacing, full-res window. */
    float rho_f = d->rho[best_e];
    if (d->E > 1) {
        const float cand[3] = {d->rho[best_e] - d->rho_half, d->rho[best_e], d->rho[best_e] + d->rho_half};
        float cv = -1e30f;
        for (int c = 0; c < 3; c++) {
            const float rho = cand[c];
            if (rho <= 0.0f || rho > 1.0f) continue;
            for (int k = -d->S; k <= d->S; k++) {
                const float s = srp_at(d, (best + k + d->G) % d->G, rho);
                if (s > cv) {
                    cv = s;
                    rho_f = rho;
                }
            }
        }
    }

    /* 4c. Full-resolution azimuth scan at the chosen scale (also the curve
     * exported to the UI). */
    best_v = -1e30f;
    for (int g = 0; g < d->G; g++) {
        const float s = srp_at(d, g, rho_f);
        d->srp[g] = s;
        if (s > best_v) {
            best_v = s;
            best = g;
        }
    }

    /* 5. Parabolic refinement on the circular grid. */
    const float sm = d->srp[(best - 1 + d->G) % d->G];
    const float s0 = d->srp[best];
    const float sp = d->srp[(best + 1) % d->G];
    const float den = sm - 2.0f * s0 + sp;
    float delta = 0.0f;
    if (den < 0.0f) {
        delta = 0.5f * (sm - sp) / den;
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
    }
    float az = ((float)best + delta) * 360.0f / (float)d->G;
    if (az < 0.0f) az += 360.0f;
    if (az >= 360.0f) az -= 360.0f;

    float conf = s0 / (float)d->P;
    if (!(conf > 0.0f)) conf = 0.0f; /* also catches NaN */
    if (conf > 1.0f) conf = 1.0f;

    out->azimuth_deg = az;
    out->confidence = conf;
    return 0;
}

int doa_num_pairs(const doa_t *d) { return d ? d->P : 0; }
int doa_max_lag(const doa_t *d) { return d ? d->L : 0; }

const float *doa_pair_gcc(const doa_t *d, int pair)
{
    if (!d || pair < 0 || pair >= d->P) return NULL;
    return &d->gcc[pair * (2 * d->L + 1)];
}

const float *doa_srp(const doa_t *d) { return d ? d->srp : NULL; }

void doa_pair_channels(const doa_t *d, int pair, int *i, int *j)
{
    if (!d || pair < 0 || pair >= d->P) {
        if (i) *i = -1;
        if (j) *j = -1;
        return;
    }
    if (i) *i = d->pair_i[pair];
    if (j) *j = d->pair_j[pair];
}
