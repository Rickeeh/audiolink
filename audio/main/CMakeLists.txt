idf_component_register(
    SRCS "main.c" "audio_common.c" "adc_test.c" "dac_test.c" "link_espnow.c" "link_tx.c" "link_rx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_gpio esp_timer esp_psram esp_wifi esp_netif esp_event nvs_flash alk_proto al_led
)
