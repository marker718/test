/*
 * Firmware entry point. Skeleton only: prints build/chip identity and runs a
 * heartbeat so you can prove the toolchain, flashing and console all work
 * before anything interesting gets added.
 */
#include <inttypes.h>
#include <stdio.h>

#include "doa_bench.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void log_identity(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s, built %s %s)",
             app->project_name, app->version, app->idf_ver,
             app->date, app->time);

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0;
    }

    ESP_LOGI(TAG, "chip model %d rev v%d.%d, %d core(s), flash %" PRIu32 " MB%s",
             chip.model, chip.revision / 100, chip.revision % 100, chip.cores,
             flash_bytes / (1024 * 1024),
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? ", embedded PSRAM" : "");
}

void app_main(void)
{
    log_identity();
    doa_bench_run();

    uint32_t tick = 0;
    for (;;) {
        ESP_LOGI(TAG, "alive %" PRIu32 " | free heap %" PRIu32 " B",
                 tick++, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
