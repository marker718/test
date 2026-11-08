#include "synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI       3.14159265358979323846
#define SINC_HALF 48 /* taps each side of the fractional-delay kernel */

void synth_rng_seed(synth_rng_t *r, uint64_t seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ull;
}

static uint64_t rng_next(synth_rng_t *r)
{
    /* xorshift64* */
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1Dull;
}

static double rng_uniform(synth_rng_t *r)
{
    return ((double)(rng_next(r) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

double synth_rng_gauss(synth_rng_t *r)
{
    /* Box-Muller, one output per call; good enough for test signals. */
    const double u1 = rng_uniform(r);
    const double u2 = rng_uniform(r);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);
}

double synth_ang_err(double a, double b)
{
    double d = fmod(fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

/* Blackman-windowed sinc, support |x| < SINC_HALF. */
static double kernel(double x)
{
    if (fabs(x) >= (double)SINC_HALF) return 0.0;
    const double s = (x == 0.0) ? 1.0 : sin(PI * x) / (PI * x);
    const double w = 0.42 + 0.5 * cos(PI * x / SINC_HALF) + 0.08 * cos(2.0 * PI * x / SINC_HALF);
    return s * w;
}

/* Value of the band-limited waveform b at fractional time t (samples),
 * where b[0] corresponds to t = -pad. */
static double sample_at(const double *b, int blen, int pad, double t)
{
    const double tt = t + pad;
    const int i0 = (int)floor(tt);
    const double frac = tt - i0;
    double acc = 0.0;
    for (int k = -SINC_HALF + 1; k <= SINC_HALF; k++) {
        const int idx = i0 + k;
        if (idx < 0 || idx >= blen) continue;
        acc += b[idx] * kernel(frac - k);
    }
    return acc;
}

int synth_render(const synth_scene_t *sc, float *const *out)
{
    if (!sc || !out || sc->num_channels < 1 || sc->num_channels > SYNTH_MAX_CH ||
        sc->num_paths < 0 || sc->num_paths > SYNTH_MAX_PATHS || sc->len <= 0) {
        return -1;
    }
    const int M = sc->num_channels;
    const int len = sc->len;

    /* Per-path, per-mic delay (samples, relative) and gain. */
    double delay[SYNTH_MAX_PATHS][SYNTH_MAX_CH];
    double gain[SYNTH_MAX_PATHS][SYNTH_MAX_CH];
    double max_abs_delay = 0.0;
    for (int p = 0; p < sc->num_paths; p++) {
        const synth_path_t *pa = &sc->paths[p];
        const double az = pa->az_deg * PI / 180.0;
        const double el = pa->el_deg * PI / 180.0;
        const double ux = cos(el) * cos(az), uy = cos(el) * sin(az), uz = sin(el);
        for (int m = 0; m < M; m++) {
            const double mx = sc->mic_xy[m][0], my = sc->mic_xy[m][1];
            double dsec, g;
            if (pa->range_m > 0.0f) {
                const double R = pa->range_m;
                const double sx = R * ux - mx, sy = R * uy - my, sz = R * uz;
                const double r = sqrt(sx * sx + sy * sy + sz * sz);
                dsec = (r - R) / sc->c_mps;   /* relative to array centre */
                g = R / r;                    /* 1/r, normalised to the centre */
            } else {
                dsec = -(mx * ux + my * uy) / sc->c_mps;
                g = 1.0;
            }
            delay[p][m] = dsec * sc->fs_hz + pa->extra_delay_samples + sc->ch_skew_samples[m];
            gain[p][m] = g * pa->gain;
            if (fabs(delay[p][m]) > max_abs_delay) max_abs_delay = fabs(delay[p][m]);
        }
    }

    /* Base waveform with padding on both sides so every delayed read is
     * backed by real samples (no edge transients in the frame). */
    const int pad = SINC_HALF + (int)ceil(max_abs_delay) + 4;
    const int blen = len + 2 * pad;
    double *b = malloc(sizeof(double) * (size_t)blen);
    if (!b) return -1;

    synth_rng_t rng;
    synth_rng_seed(&rng, sc->seed);
    for (int i = 0; i < blen; i++) {
        const double t = (double)(i - pad); /* sample time in frame coordinates */
        double v = synth_rng_gauss(&rng);
        if (sc->kind == SYNTH_CLAP) {
            const double dt = t - sc->clap_onset_samples;
            v = (dt < 0.0) ? 0.0 : v * exp(-dt / (sc->clap_tau_s * sc->fs_hz));
        }
        b[i] = v;
    }

    for (int m = 0; m < M; m++) {
        for (int n = 0; n < len; n++) {
            double acc = 0.0;
            for (int p = 0; p < sc->num_paths; p++) {
                acc += gain[p][m] * sample_at(b, blen, pad, (double)n - delay[p][m]);
            }
            out[m][n] = (float)acc;
        }
    }
    free(b);

    /* Independent sensor noise at the requested frame SNR (vs channel 0). */
    if (sc->snr_db < 200.0f) {
        double e = 0.0;
        for (int n = 0; n < len; n++) e += (double)out[0][n] * out[0][n];
        const double rms = sqrt(e / len);
        const double nrms = (rms > 0.0 ? rms : 1.0) * pow(10.0, -sc->snr_db / 20.0);
        synth_rng_t nr;
        synth_rng_seed(&nr, sc->seed ^ 0xA5A5A5A5DEADBEEFull);
        for (int m = 0; m < M; m++) {
            for (int n = 0; n < len; n++) {
                out[m][n] += (float)(nrms * synth_rng_gauss(&nr));
            }
        }
    }
    return 0;
}
