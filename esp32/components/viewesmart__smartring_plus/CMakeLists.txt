idf_component_register(
    SRCS "bsp_sd.c" "bsp_imu.c" "smartring_bsp.c" "bsp_battery.c" "bsp_axp2101.c" "bsp_power.c" "bsp_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_lcd_touch esp_lcd_st77916 esp_lcd_touch_cst816s esp_lvgl_adapter lvgl
             esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_i2s esp_adc
             waveshare__qmi8658 espressif__esp_codec_dev esp_timer fatfs sdmmc
)
