// TX v2 - ciclo de vida: inicia o NVS, cria as duas tarefas e termina (ARCH_V2_PROPOSAL.md, TX V2):
//   tx_audio: core 1, prioridade 20 (o regime do BEN). O arranque do Wi-Fi/ESP-NOW corre dentro dela, como no BEN.
//   tx_diag:  core 0, prioridade 1. Fora do core de tempo real; pode bloquear na consola.
#include <stdlib.h>

#include "diag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "tx_audio.h"

#define TX_AUDIO_CORE 1
#define TX_AUDIO_PRIO 20
#define TX_AUDIO_STACK 4096
#define TX_DIAG_CORE 0
#define TX_DIAG_PRIO 1
#define TX_DIAG_STACK 4096

static const char *TAG = "tx_main";

static void nvs_init(void) {  // = bench_nvs_init (BEN)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS sem espaco/versao antiga, a apagar e reiniciar");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void) {
    nvs_init();
    if (xTaskCreatePinnedToCore(tx_audio_task, "tx_audio", TX_AUDIO_STACK, NULL, TX_AUDIO_PRIO, NULL, TX_AUDIO_CORE) !=
            pdPASS ||
        xTaskCreatePinnedToCore(diag_task, "tx_diag", TX_DIAG_STACK, NULL, TX_DIAG_PRIO, NULL, TX_DIAG_CORE) != pdPASS) {
        ESP_LOGE(TAG, "falha a criar as tarefas - a abortar");
        abort();
    }
    ESP_LOGI(TAG, "tarefas: tx_audio core=%d prio=%d stack=%d; tx_diag core=%d prio=%d stack=%d", TX_AUDIO_CORE,
             TX_AUDIO_PRIO, TX_AUDIO_STACK, TX_DIAG_CORE, TX_DIAG_PRIO, TX_DIAG_STACK);
}
