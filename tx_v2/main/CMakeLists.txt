idf_component_register(
    SRCS "tx_main.c" "tx_audio.c" "acq.c" "espnow_tx.c" "tx_ledger.c" "diag.c"
    INCLUDE_DIRS "."
    REQUIRES alk_proto al_led esp_driver_i2s esp_event esp_netif esp_timer esp_wifi nvs_flash
)
# Codigo novo: avisos extra como erros (so neste componente).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
