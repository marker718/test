/*
 * Boot-time DOA benchmark: answers "does the estimator fit the budget on the
 * real S3" before any mics are wired up.
 *
 * Input is a white-noise plane wave from a known azimuth, using integer-sample
 * delays (the host harness does the fractional-delay accuracy work). For the
 * default geometry at 60 deg the rounded delays are (-6, -2, +6, +2) samples,
 * whose least-squares bearing is 63.5 deg: that is the expected output, not
 * an estimator error.
 *
 * Cycle counts are only meaningful on silicon. QEMU runs the code but its
 * timing is not cycle-accurate.
 */
#include "doa_bench.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>

#include "doa.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "doa_bench";

#define BENCH_AZ_DEG 60.0f
#define BENCH_PAD    32

void doa_bench_run(void)
{
#if CONFIG_APP_DOA_BENCH
    const doa_config_t cfg = doa_default_config();
    const int N = cfg.frame_len;
    const int M = cfg.num_channels;

    doa_t *d = doa_create(&cfg);
    float *buf = malloc(sizeof(float) * (size_t)(M * N));
    float *src = malloc(sizeof(float) * (size_t)(N + 2 * BENCH_PAD));
    if (!d || !buf || !src) {
        ESP_LOGE(TAG, "alloc failed");
        goto out;
    }

    /* LCG white noise, roughly uniform in [-1, 1). */
    uint32_t s = 12345u;
    for (int n = 0; n < N + 2 * BENCH_PAD; n++) {
        s = s * 1664525u + 1013904223u;
        src[n] = (float)(int32_t)s * (1.0f / 2147483648.0f);
    }

    const float az = BENCH_AZ_DEG * 3.14159265f / 180.0f;
    const float ux = cosf(az), uy = sinf(az);
    const float *ch[DOA_MAX_CH];
    for (int m = 0; m < M; m++) {
        const float delay = -(cfg.mic_xy[m][0] * ux + cfg.mic_xy[m][1] * uy) / cfg.c_mps * cfg.fs_hz;
        const int di = (int)lroundf(delay);
        float *x = &buf[m * N];
        for (int n = 0; n < N; n++) x[n] = src[n + BENCH_PAD - di];
        ch[m] = x;
    }

    doa_result_t r = {0};
    uint32_t min_cyc = UINT32_MAX;
    uint64_t sum_cyc = 0;
    const int iters = CONFIG_APP_DOA_BENCH_FRAMES;
    for (int i = 0; i < iters; i++) {
        const uint32_t t0 = esp_cpu_get_cycle_count();
        doa_process(d, ch, &r);
        const uint32_t dt = esp_cpu_get_cycle_count() - t0;
        sum_cyc += dt;
        if (dt < min_cyc) min_cyc = dt;
    }

    const uint32_t avg_cyc = (uint32_t)(sum_cyc / (uint64_t)iters);
    const uint32_t cpu_hz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
    /* 50% overlap: one frame every N/2 samples. */
    const float frames_per_s = cfg.fs_hz / (float)(N / 2);
    const float load_pct = 100.0f * (float)avg_cyc * frames_per_s / (float)cpu_hz;

    ESP_LOGI(TAG, "target az %.1f -> %.2f deg, conf %.3f",
             (double)BENCH_AZ_DEG, (double)r.azimuth_deg, (double)r.confidence);
    ESP_LOGI(TAG, "%d iters: avg %" PRIu32 " cyc (%.2f ms), min %" PRIu32 " cyc",
             iters, avg_cyc, (double)avg_cyc * 1000.0 / cpu_hz, min_cyc);
    ESP_LOGI(TAG, "real-time load at %.1f frames/s: %.1f%% of one core",
             (double)frames_per_s, (double)load_pct);

out:
    free(src);
    free(buf);
    doa_destroy(d);
#endif
}
