/*
 * doa: azimuth-only direction-of-arrival estimator for a planar mic array.
 *
 * Pipeline per frame:
 *   Hann window -> FFT (two real channels per complex FFT)
 *   -> optional per-channel delay trim (calibration)
 *   -> GCC-PHAT for every mic pair, band-limited to [f_lo, f_hi]
 *      (two pairs per complex IFFT)
 *   -> SRP: sum of pair GCCs evaluated at the expected TDOA (cubic
 *      interpolation between lags). Coarse pass over azimuth x in-plane scale
 *      rings (cos el), fine scale pick, then a full-resolution azimuth scan
 *   -> grid peak + parabolic refinement.
 *
 * Pure C11 + libm. No ESP-IDF dependencies, so the same code is unit-tested
 * on the host (test_host/) and runs on the ESP32-S3. The hot path uses float
 * only: the S3 FPU is single precision and double is soft-float.
 *
 * Conventions
 *   - Mic positions in metres, array plane z = 0.
 *   - Azimuth 0 deg = +x axis, counter-clockwise positive, reported in [0, 360).
 *   - Planar array: a source above the plane and its mirror image below it
 *     give the same answer. Elevation is not estimated.
 */
#ifndef DOA_H
#define DOA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOA_MAX_CH 8

typedef struct {
    int   num_channels;              /* 2..DOA_MAX_CH */
    float mic_xy[DOA_MAX_CH][2];     /* metres */
    float fs_hz;                     /* sample rate */
    int   frame_len;                 /* power of two, 64..4096 */
    float c_mps;                     /* speed of sound */
    float f_lo_hz;                   /* GCC band, lower edge */
    float f_hi_hz;                   /* GCC band, upper edge (< fs/2) */
    int   az_grid;                   /* azimuth grid points over 360 deg, 36..3600 */
    float el_max_deg;                /* highest source elevation to tolerate, 0..80.
                                        A planar array sees TDOAs shrunk by cos(el); the
                                        search covers rings of that scale down to
                                        cos(el_max). 0 = in-plane sources only. Elevation
                                        is searched over but never reported. */
} doa_config_t;

typedef struct {
    float azimuth_deg;   /* [0, 360) */
    float confidence;    /* mean pair GCC-PHAT value at the peak, ~[0, 1].
                            ~1 = single coherent source, near 0 = diffuse/silence */
} doa_result_t;

typedef struct doa doa_t;

/* EARSHOT defaults: 4 mics on a 60 mm square, CCW from (+30,+30) mm,
 * 48 kHz, 1024-sample frames, 200 Hz..8 kHz, 1 deg grid, c = 343 m/s,
 * sources up to 70 deg elevation. */
doa_config_t doa_default_config(void);

/* Returns NULL on invalid config or allocation failure. Allocates once;
 * doa_process() does no allocation. */
doa_t *doa_create(const doa_config_t *cfg);
void   doa_destroy(doa_t *d);

/* Calibration: channel `ch` lags the others by `delay_samples` (fractional,
 * may be negative). The estimator advances that channel by the same amount
 * before correlating. Pass 0 to clear. Returns 0 or -1 on bad args. */
int doa_set_channel_delay(doa_t *d, int ch, float delay_samples);

/* Process one frame. ch[c] points to frame_len float samples for channel c
 * (any consistent scale). Returns 0 or -1 on bad args. A silent or
 * all-zero frame returns confidence 0 and an arbitrary (finite) azimuth. */
int doa_process(doa_t *d, const float *const *ch, doa_result_t *out);

/* Introspection (for the web UI / debugging). Valid until the next
 * doa_process(). */
int          doa_num_pairs(const doa_t *d);
int          doa_max_lag(const doa_t *d);                 /* L: GCC covers lags -L..+L */
const float *doa_pair_gcc(const doa_t *d, int pair);      /* 2L+1 values, index 0 = lag -L */
const float *doa_srp(const doa_t *d);                     /* az_grid values, at the winning scale */
void         doa_pair_channels(const doa_t *d, int pair, int *i, int *j);

#ifdef __cplusplus
}
#endif

#endif /* DOA_H */
