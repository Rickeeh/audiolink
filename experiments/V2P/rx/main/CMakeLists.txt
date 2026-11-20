idf_component_register(SRCS "v2p_rx.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_gpio esp_timer esp_wifi esp_netif esp_event nvs_flash esp_ringbuf alk_proto al_led)
