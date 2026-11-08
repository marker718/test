/*
 * Host test harness for the doa component.
 *
 *   ./doa_test            run all tests, exit non-zero on any failure
 *   ./doa_test -v         also print per-angle detail for sweeps
 *
 * Thresholds are set well inside the EARSHOT G2 target (< 5 deg RMS in a
 * real room). Synthetic scenes are the best case, so if they regress, the
 * real-room numbers will be worse.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doa.h"
#include "doa_fft.h"
#include "synth.h"

static int g_fail;
static int g_verbose;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            g_fail++;                                                 \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

/* ---------- helpers ---------- */

typedef struct {
    float *buf;
    float *ch[SYNTH_MAX_CH];
    int M, len;
} frames_t;

static void frames_alloc(frames_t *f, int M, int len)
{
    f->M = M;
    f->len = len;
    f->buf = calloc((size_t)(M * len), sizeof(float));
    if (!f->buf) {
        fprintf(stderr, "oom\n");
        exit(2);
    }
    for (int m = 0; m < M; m++) f->ch[m] = &f->buf[m * len];
}

static void frames_free(frames_t *f) { free(f->buf); }

static synth_scene_t scene_from(const doa_config_t *cfg)
{
    synth_scene_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.num_channels = cfg->num_channels;
    memcpy(sc.mic_xy, cfg->mic_xy, sizeof(sc.mic_xy));
    sc.fs_hz = cfg->fs_hz;
    sc.c_mps = cfg->c_mps;
    sc.len = cfg->frame_len;
    sc.kind = SYNTH_WHITE;
    sc.clap_onset_samples = (float)cfg->frame_len / 3.0f;
    sc.clap_tau_s = 0.005f;
    sc.num_paths = 1;
    sc.paths[0].gain = 1.0f;
    sc.snr_db = 20.0f;
    sc.seed = 1;
    return sc;
}

static doa_result_t run(doa_t *d, const synth_scene_t *sc, frames_t *fr)
{
    doa_result_t r = {0};
    if (synth_render(sc, fr->ch) != 0) {
        g_fail++;
        printf("    FAIL synth_render\n");
        return r;
    }
    if (doa_process(d, (const float *const *)fr->ch, &r) != 0) {
        g_fail++;
        printf("    FAIL doa_process\n");
    }
    return r;
}

typedef struct {
    double rms, max, worst_az, min_conf;
} sweep_stats_t;

/* Sweep azimuth 0..355 in 5 deg steps. Each angle gets a different seed. */
static sweep_stats_t sweep(doa_t *d, synth_scene_t sc, frames_t *fr, const char *label)
{
    sweep_stats_t s = {0, 0, 0, 1.0};
    double acc = 0.0;
    int n = 0;
    const float base_az = sc.paths[0].az_deg;
    float rel_az[SYNTH_MAX_PATHS];
    for (int p = 0; p < sc.num_paths; p++) rel_az[p] = sc.paths[p].az_deg - base_az;

    for (int a = 0; a < 360; a += 5) {
        for (int p = 0; p < sc.num_paths; p++) sc.paths[p].az_deg = (float)a + rel_az[p];
        sc.seed = 1000u + (uint64_t)a;
        const doa_result_t r = run(d, &sc, fr);
        const double e = synth_ang_err(r.azimuth_deg, a);
        if (!isfinite(r.azimuth_deg) || !isfinite(r.confidence)) {
            g_fail++;
            printf("    FAIL %s: non-finite output at az %d\n", label, a);
        }
        if (g_verbose) printf("      %-28s az %3d -> %7.2f  err %5.2f  conf %.3f\n",
                              label, a, r.azimuth_deg, e, r.confidence);
        acc += e * e;
        n++;
        if (e > s.max) {
            s.max = e;
            s.worst_az = a;
        }
        if (r.confidence < s.min_conf) s.min_conf = r.confidence;
    }
    s.rms = sqrt(acc / n);
    printf("    %-34s rms %5.2f  max %5.2f (az %3.0f)  min conf %.3f\n",
           label, s.rms, s.max, s.worst_az, s.min_conf);
    return s;
}

/* ---------- tests ---------- */

static void test_fft(void)
{
    printf("[fft] radix-2 vs naive DFT\n");
    const int N = 256;
    doa_fft_t f;
    CHECK(doa_fft_init(&f, N) == 0, "init");
    doa_cpx_t *x = malloc(sizeof(doa_cpx_t) * N);
    doa_cpx_t *z = malloc(sizeof(doa_cpx_t) * N);
    doa_cpx_t *X = malloc(sizeof(doa_cpx_t) * (N / 2 + 1));
    doa_cpx_t *Y = malloc(sizeof(doa_cpx_t) * (N / 2 + 1));
    double *xr = malloc(sizeof(double) * N), *yr = malloc(sizeof(double) * N);
    synth_rng_t rng;
    synth_rng_seed(&rng, 42);
    for (int n = 0; n < N; n++) {
        xr[n] = synth_rng_gauss(&rng);
        yr[n] = synth_rng_gauss(&rng);
        x[n].re = (float)xr[n];
        x[n].im = (float)yr[n];
        z[n] = x[n];
    }

    /* Forward vs DFT of the complex signal. */
    doa_fft_forward(&f, z);
    double max_err = 0.0, max_ref = 0.0;
    for (int k = 0; k < N; k++) {
        double re = 0, im = 0;
        for (int n = 0; n < N; n++) {
            const double a = -2.0 * M_PI * k * n / N;
            re += xr[n] * cos(a) - yr[n] * sin(a);
            im += xr[n] * sin(a) + yr[n] * cos(a);
        }
        max_err = fmax(max_err, hypot(z[k].re - re, z[k].im - im));
        max_ref = fmax(max_ref, hypot(re, im));
    }
    printf("    forward  rel err %.2e\n", max_err / max_ref);
    CHECK(max_err / max_ref < 1e-5, "forward rel err %.3e", max_err / max_ref);

    /* Split into the two real-input spectra vs. separate DFTs. */
    doa_fft_split_real_pair(N, z, X, Y);
    double split_err = 0.0;
    for (int k = 0; k <= N / 2; k++) {
        double xre = 0, xim = 0, yre = 0, yim = 0;
        for (int n = 0; n < N; n++) {
            const double a = -2.0 * M_PI * k * n / N;
            xre += xr[n] * cos(a);
            xim += xr[n] * sin(a);
            yre += yr[n] * cos(a);
            yim += yr[n] * sin(a);
        }
        split_err = fmax(split_err, hypot(X[k].re - xre, X[k].im - xim));
        split_err = fmax(split_err, hypot(Y[k].re - yre, Y[k].im - yim));
    }
    printf("    split    rel err %.2e\n", split_err / max_ref);
    CHECK(split_err / max_ref < 1e-5, "split rel err %.3e", split_err / max_ref);

    /* Round trip. */
    doa_fft_inverse(&f, z);
    double rt = 0.0;
    for (int n = 0; n < N; n++) {
        rt = fmax(rt, hypot(z[n].re / N - xr[n], z[n].im / N - yr[n]));
    }
    printf("    roundtrip abs err %.2e\n", rt);
    CHECK(rt < 1e-5, "roundtrip err %.3e", rt);

    doa_fft_t bad;
    CHECK(doa_fft_init(&bad, 100) == -1, "non-pow2 size accepted");
    doa_fft_free(&f);
    free(x); free(z); free(X); free(Y); free(xr); free(yr);
}

static void test_config(void)
{
    printf("[config] invalid configs rejected\n");
    doa_config_t c = doa_default_config();
    doa_t *d = doa_create(&c);
    CHECK(d != NULL, "default config rejected");
    doa_destroy(d);

    struct { const char *what; doa_config_t c; } bad[10];
    int nb = 0;
    bad[nb].what = "1 channel";    bad[nb].c = c; bad[nb].c.num_channels = 1; nb++;
    bad[nb].what = "non-pow2 N";   bad[nb].c = c; bad[nb].c.frame_len = 1000; nb++;
    bad[nb].what = "f_hi >= fs/2"; bad[nb].c = c; bad[nb].c.f_hi_hz = 24000.0f; nb++;
    bad[nb].what = "f_lo > f_hi";  bad[nb].c = c; bad[nb].c.f_lo_hz = 9000.0f; nb++;
    bad[nb].what = "tiny grid";    bad[nb].c = c; bad[nb].c.az_grid = 4; nb++;
    bad[nb].what = "coincident";   bad[nb].c = c; memset(bad[nb].c.mic_xy, 0, sizeof(c.mic_xy)); nb++;
    bad[nb].what = "array > N/4";  bad[nb].c = c; bad[nb].c.mic_xy[0][0] = 5.0f; nb++;
    bad[nb].what = "NaN position"; bad[nb].c = c; bad[nb].c.mic_xy[2][1] = NAN; nb++;
    bad[nb].what = "el_max > 80";  bad[nb].c = c; bad[nb].c.el_max_deg = 85.0f; nb++;
    bad[nb].what = "el_max < 0";   bad[nb].c = c; bad[nb].c.el_max_deg = -1.0f; nb++;
    for (int i = 0; i < nb; i++) {
        doa_t *x = doa_create(&bad[i].c);
        CHECK(x == NULL, "accepted bad config: %s", bad[i].what);
        doa_destroy(x);
    }
    CHECK(doa_create(NULL) == NULL, "NULL config accepted");
}

static void test_far_field(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[far-field] white noise, el 0, 72 azimuths\n");
    synth_scene_t sc = scene_from(cfg);
    sc.snr_db = 30.0f;
    sweep_stats_t s = sweep(d, sc, fr, "SNR 30 dB");
    CHECK(s.max < 1.0, "max err %.2f deg", s.max);
    CHECK(s.min_conf > 0.5, "min conf %.3f", s.min_conf);

    sc.snr_db = 20.0f;
    s = sweep(d, sc, fr, "SNR 20 dB");
    CHECK(s.max < 1.0, "max err %.2f deg", s.max);
}

static void test_near_field(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[near-field] point source with 1/r, far-field estimator\n");
    synth_scene_t sc = scene_from(cfg);
    const float ranges[] = {1.0f, 0.5f, 0.25f};
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "range %.2f m", (double)ranges[i]);
        sc.paths[0].range_m = ranges[i];
        sweep_stats_t s = sweep(d, sc, fr, label);
        CHECK(s.max < 2.0, "%s: max err %.2f deg", label, s.max);
    }
}

static void test_clap(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[clap] 5 ms decaying burst at N/3\n");
    synth_scene_t sc = scene_from(cfg);
    sc.kind = SYNTH_CLAP;
    sweep_stats_t s = sweep(d, sc, fr, "clap, SNR 20 dB (frame RMS)");
    CHECK(s.max < 1.5, "max err %.2f deg", s.max);
}

static void test_snr(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[snr] white noise, far-field\n");
    synth_scene_t sc = scene_from(cfg);
    const float snrs[] = {10.0f, 5.0f, 0.0f, -5.0f};
    const double limit_rms[] = {1.0, 1.5, 3.0, 10.0};
    for (size_t i = 0; i < sizeof(snrs) / sizeof(snrs[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "SNR %+.0f dB", (double)snrs[i]);
        sc.snr_db = snrs[i];
        sweep_stats_t s = sweep(d, sc, fr, label);
        CHECK(s.rms < limit_rms[i], "%s: rms %.2f deg (limit %.1f)", label, s.rms, limit_rms[i]);
    }
}

static void test_reflection(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[reflection] coherent image source, white noise\n");
    synth_scene_t sc = scene_from(cfg);
    sc.num_paths = 2;
    /* Side wall: reflection 90 deg off, -3 dB, 0.83 ms (~29 cm) later. */
    sc.paths[1].az_deg = 90.0f;
    sc.paths[1].gain = 0.7f;
    sc.paths[1].extra_delay_samples = 40.0f;
    sweep_stats_t s = sweep(d, sc, fr, "wall +90 deg, g 0.7, +40 smp");
    CHECK(s.max < 3.0, "max err %.2f deg", s.max);

    /* Desk: image source directly below, same azimuth, short extra path. */
    sc.paths[1].az_deg = 0.0f;
    sc.paths[0].el_deg = 20.0f;
    sc.paths[1].el_deg = -20.0f;
    sc.paths[1].gain = 0.8f;
    sc.paths[1].extra_delay_samples = 6.0f;
    s = sweep(d, sc, fr, "desk mirror, el +/-20, g 0.8");
    CHECK(s.max < 2.0, "max err %.2f deg", s.max);
}

static void test_elevation(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[elevation] source above the array plane\n");
    synth_scene_t sc = scene_from(cfg);
    /* A head talking over a desk unit sits at 20-50 deg. Before the scale
     * rings were added, 60 deg collapsed to ~105 deg RMS error. */
    const float els[] = {15.0f, 30.0f, 45.0f, 60.0f, 70.0f};
    for (size_t i = 0; i < sizeof(els) / sizeof(els[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "el %.0f deg", (double)els[i]);
        sc.paths[0].el_deg = els[i];
        sweep_stats_t s = sweep(d, sc, fr, label);
        CHECK(s.max < 2.0, "%s: max err %.2f deg", label, s.max);
    }

    /* Beyond el_max: report only. Azimuth is ill-defined straight overhead;
     * this documents how the estimator behaves there. */
    sc.paths[0].el_deg = 85.0f;
    (void)sweep(d, sc, fr, "el 85 (report only)");

    /* Regression guard for the failure mode itself: with the scale search
     * disabled, 60 deg must still break. If it doesn't, this test has lost
     * its teeth. */
    doa_config_t flat = *cfg;
    flat.el_max_deg = 0.0f;
    doa_t *df = doa_create(&flat);
    CHECK(df != NULL, "create el_max 0");
    if (df) {
        sc.paths[0].el_deg = 60.0f;
        sweep_stats_t s = sweep(df, sc, fr, "el 60, el_max_deg = 0");
        CHECK(s.rms > 10.0, "in-plane-only search did not fail at el 60 (rms %.2f)", s.rms);
        doa_destroy(df);
    }
}

static void test_skew(const doa_config_t *cfg, frames_t *fr)
{
    printf("[skew] 1.5-sample hardware skew on ch1, then calibrated out\n");
    doa_t *d = doa_create(cfg);
    CHECK(d != NULL, "create");
    if (!d) return;
    synth_scene_t sc = scene_from(cfg);
    sc.ch_skew_samples[1] = 1.5f;

    sweep_stats_t raw = sweep(d, sc, fr, "uncalibrated");
    CHECK(raw.max > 3.0, "skew had no visible effect (max %.2f); test is not sensitive", raw.max);

    CHECK(doa_set_channel_delay(d, 1, 1.5f) == 0, "set delay");
    sweep_stats_t cal = sweep(d, sc, fr, "calibrated");
    CHECK(cal.max < 1.0, "calibrated max err %.2f deg", cal.max);

    CHECK(doa_set_channel_delay(d, 1, 0.0f) == 0, "clear delay");
    sc.ch_skew_samples[1] = 0.0f;
    sweep_stats_t clr = sweep(d, sc, fr, "cleared, no skew");
    CHECK(clr.max < 1.0, "after clearing max err %.2f deg", clr.max);

    CHECK(doa_set_channel_delay(d, 9, 1.0f) == -1, "bad channel accepted");
    CHECK(doa_set_channel_delay(d, 0, NAN) == -1, "NaN delay accepted");
    doa_destroy(d);
}

static void test_confidence(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[confidence] coherent vs diffuse vs silence\n");
    synth_scene_t sc = scene_from(cfg);
    sc.snr_db = 20.0f;
    const doa_result_t coh = run(d, &sc, fr);

    /* Diffuse: independent noise on every channel, no source. */
    synth_rng_t rng;
    double worst_diffuse = 0.0;
    for (int trial = 0; trial < 20; trial++) {
        synth_rng_seed(&rng, 777u + (uint64_t)trial);
        for (int m = 0; m < fr->M; m++)
            for (int n = 0; n < fr->len; n++) fr->ch[m][n] = (float)synth_rng_gauss(&rng);
        doa_result_t r;
        doa_process(d, (const float *const *)fr->ch, &r);
        if (r.confidence > worst_diffuse) worst_diffuse = r.confidence;
    }

    memset(fr->buf, 0, sizeof(float) * (size_t)(fr->M * fr->len));
    doa_result_t sil;
    doa_process(d, (const float *const *)fr->ch, &sil);

    printf("    coherent %.3f | diffuse worst-of-20 %.3f | silence %.3f (az %.1f)\n",
           coh.confidence, worst_diffuse, sil.confidence, sil.azimuth_deg);
    CHECK(coh.confidence > 0.7, "coherent conf %.3f", coh.confidence);
    CHECK(worst_diffuse < 0.2, "diffuse conf %.3f", worst_diffuse);
    CHECK(sil.confidence == 0.0f && isfinite(sil.azimuth_deg), "silence conf %.3f az %f",
          sil.confidence, sil.azimuth_deg);

    doa_result_t r;
    CHECK(doa_process(NULL, (const float *const *)fr->ch, &r) == -1, "NULL doa accepted");
    CHECK(doa_process(d, NULL, &r) == -1, "NULL frames accepted");
}

static void test_odd_channels(frames_t *fr)
{
    printf("[geometry] 3-mic triangle (odd channel count path)\n");
    doa_config_t c = doa_default_config();
    c.num_channels = 3;
    const float r = 0.040f;
    for (int m = 0; m < 3; m++) {
        const float a = (float)(2.0 * M_PI * m / 3.0);
        c.mic_xy[m][0] = r * cosf(a);
        c.mic_xy[m][1] = r * sinf(a);
    }
    doa_t *d = doa_create(&c);
    CHECK(d != NULL, "create");
    if (!d) return;
    CHECK(doa_num_pairs(d) == 3, "pairs %d", doa_num_pairs(d));
    synth_scene_t sc = scene_from(&c);
    sweep_stats_t s = sweep(d, sc, fr, "triangle r 40 mm, SNR 20");
    CHECK(s.max < 1.5, "max err %.2f deg", s.max);
    doa_destroy(d);
}

static void test_gcc_introspection(doa_t *d, const doa_config_t *cfg, frames_t *fr)
{
    printf("[introspection] GCC peak lag matches geometry\n");
    synth_scene_t sc = scene_from(cfg);
    sc.paths[0].az_deg = 45.0f; /* along the ch0 diagonal: pair (0,2) has max |TDOA| */
    sc.snr_db = 40.0f;
    doa_result_t r = run(d, &sc, fr);
    (void)r;
    const int L = doa_max_lag(d);
    for (int q = 0; q < doa_num_pairs(d); q++) {
        int i, j;
        doa_pair_channels(d, q, &i, &j);
        const float *g = doa_pair_gcc(d, q);
        int best = 0;
        for (int l = 0; l < 2 * L + 1; l++) if (g[l] > g[best]) best = l;
        const double dx = cfg->mic_xy[j][0] - cfg->mic_xy[i][0];
        const double dy = cfg->mic_xy[j][1] - cfg->mic_xy[i][1];
        const double expect = (dx * cos(M_PI / 4) + dy * sin(M_PI / 4)) / cfg->c_mps * cfg->fs_hz;
        if (g_verbose) printf("      pair (%d,%d) peak lag %+d expect %+.2f\n", i, j, best - L, expect);
        CHECK(fabs((best - L) - expect) <= 0.5 + 1e-6, "pair (%d,%d): peak %d, expect %.2f",
              i, j, best - L, expect);
    }
    CHECK(doa_pair_gcc(d, 99) == NULL, "bad pair index");
    printf("    %d pairs, L = %d\n", doa_num_pairs(d), L);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }

    const doa_config_t cfg = doa_default_config();
    doa_t *d = doa_create(&cfg);
    if (!d) {
        printf("FAIL: default config rejected\n");
        return 1;
    }
    frames_t fr;
    frames_alloc(&fr, SYNTH_MAX_CH, cfg.frame_len);
    fr.M = cfg.num_channels;

    test_fft();
    test_config();
    test_gcc_introspection(d, &cfg, &fr);
    test_far_field(d, &cfg, &fr);
    test_near_field(d, &cfg, &fr);
    test_clap(d, &cfg, &fr);
    test_snr(d, &cfg, &fr);
    test_reflection(d, &cfg, &fr);
    test_elevation(d, &cfg, &fr);
    test_skew(&cfg, &fr);
    test_confidence(d, &cfg, &fr);
    test_odd_channels(&fr);

    doa_destroy(d);
    frames_free(&fr);

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
