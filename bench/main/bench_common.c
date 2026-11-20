#include "bench_common.h"

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bench_common";

void bench_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS sem espaco/versao antiga, a apagar e reiniciar");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void bench_mac_to_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool bench_str_to_mac(const char *s, uint8_t out[6]) {
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

void bench_scan_loop(uint32_t period_s) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t sc = {0};  // todos os canais, scan ativo, sem SSIDs escondidos
    for (uint32_t n = 0;; n++) {
        esp_err_t e = esp_wifi_scan_start(&sc, true);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "scan falhou: %s", esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(period_s * 1000));
            continue;
        }
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num > 60) {
            num = 60;
        }
        wifi_ap_record_t *aps = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
        if (aps && esp_wifi_scan_get_ap_records(&num, aps) == ESP_OK) {
            int cnt[14] = {0};
            int mx[14];
            for (int i = 0; i < 14; i++) {
                mx[i] = -127;
            }
            for (int i = 0; i < num; i++) {
                int c = aps[i].primary;
                if (c >= 1 && c <= 13) {
                    cnt[c]++;
                    if (aps[i].rssi > mx[c]) {
                        mx[c] = aps[i].rssi;
                    }
                }
            }
            // so canais 1..13, sem SSIDs nem BSSIDs (dados de terceiros)
            printf("SCAN t=%lld total=%u", (long long)(esp_timer_get_time() / 1000000), (unsigned)num);
            for (int c = 1; c <= 13; c++) {
                if (cnt[c]) {
                    printf(" ch%d:%d,%d", c, cnt[c], mx[c]);
                }
            }
            printf("\n");
        } else {
            esp_wifi_clear_ap_list();
        }
        free(aps);
        vTaskDelay(pdMS_TO_TICKS(period_s * 1000));
    }
}
