idf_component_register(
    SRCS
        "bench_main.c"
        "bench_common.c"
        "transport_espnow.c"
        "transport_softap_udp.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer lwip esp_psram
)
