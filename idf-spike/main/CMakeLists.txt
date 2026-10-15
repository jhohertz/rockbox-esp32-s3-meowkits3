idf_component_register(
    SRCS "spike_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_sdmmc
        esp_lcd
        sdmmc
        fatfs
        nvs_flash
        esp_psram
        bt
)
