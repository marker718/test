idf_component_register(
    SRCS "main.c" "doa_bench.c"
    INCLUDE_DIRS "."
    REQUIRES esp_app_format spi_flash esp_hw_support doa
)
