/*
 * Synthetic acoustic scenes for the doa host tests.
 *
 * Deliberately independent of the code under test: delays are applied in the
 * time domain with a windowed-sinc fractional-delay interpolator in double
 * precision, and geometry is computed in full 3D. No shared FFT, no shared
 * TDOA table.
 */
#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

#define SYNTH_MAX_CH    8
#define SYNTH_MAX_PATHS 8

typedef enum {
    SYNTH_WHITE,  /* continuous white Gaussian noise */
    SYNTH_CLAP,   /* silence, then exponentially decaying noise burst */
} synth_kind_t;

/* One propagation path of the same source waveform: the direct path, or an
 * image source standing in for a reflection. */
typedef struct {
    float az_deg;
    float el_deg;
    float range_m;              /* 0 = far-field plane wave; > 0 = point source with 1/r */
    float gain;
    float extra_delay_samples;  /* added path delay (reflections) */
} synth_path_t;

typedef struct {
    int   num_channels;
    float mic_xy[SYNTH_MAX_CH][2];
    float fs_hz;
    float c_mps;
    int   len;                       /* samples per channel */

    synth_kind_t kind;
    float clap_onset_samples;        /* SYNTH_CLAP: burst start */
    float clap_tau_s;                /* SYNTH_CLAP: decay time constant */

    synth_path_t paths[SYNTH_MAX_PATHS];
    int          num_paths;

    float snr_db;                    /* independent sensor noise vs. frame RMS of channel 0. >= 200 disables */
    float ch_skew_samples[SYNTH_MAX_CH]; /* hardware skew: channel c delayed by this much */
    uint64_t seed;
} synth_scene_t;

typedef struct {
    uint64_t s;
} synth_rng_t;

void   synth_rng_seed(synth_rng_t *r, uint64_t seed);
double synth_rng_gauss(synth_rng_t *r);

/* out[c] must hold sc->len floats for each of sc->num_channels channels.
 * Returns 0 or -1. */
int synth_render(const synth_scene_t *sc, float *const *out);

/* Smallest absolute angular difference in degrees, [0, 180]. */
double synth_ang_err(double a_deg, double b_deg);

#endif /* SYNTH_H */
