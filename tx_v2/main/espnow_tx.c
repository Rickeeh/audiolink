// TX v2 - transporte ESP-NOW.
// Ancestral: bench/main/transport_espnow.c (BEN da Fase 1; braco BEN do EXP-DEEP-TX-DIFF-01, 0/15 DEEP).
// wifi_base_init e espnow_tx_init sao os do BEN, quase literais:
//   - so o ramo HT20;
//   - o peer vem de CONFIG_TXV2_PEER_MAC;
//   - PHY, taxa e canal sao constantes.
// Novo face ao BEN: a contabilidade (tx_ledger.h). Sem limite de envios em voo, sem fila, sem rollback, sem reenvio.
#include "espnow_tx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#define TXV2_CHANNEL 6
#define TXV2_PHYMODE WIFI_PHY_MODE_HT20
#define TXV2_RATE WIFI_PHY_RATE_MCS4_LGI

static const char *TAG = "tx_espnow";

static tx_ledger_t s_ledger;
static uint8_t s_peer[6];

static void mac_to_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool str_to_mac(const char *s, uint8_t out[6]) {
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

static void wifi_base_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Canal fixo, sem AP nem scan; 20 MHz sem canal secundario (Fase 1: HT20 alcanca mais do que HT40).
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_channel(TXV2_CHANNEL, WIFI_SECOND_CHAN_NONE));

    // Power save desligado: senao dezenas de ms de latencia e jitter (audiocast H1-D1, Fase 1).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    int8_t pwr = 0;
    ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&pwr));
    ESP_LOGI(TAG, "potencia TX max = %d (x0.25 dBm = %.2f dBm)", pwr, pwr * 0.25);

    uint8_t mac[6];
    char macstr[18];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    mac_to_str(mac, macstr);
    ESP_LOGI(TAG, "MAC STA deste no: %s (canal %d)", macstr, TXV2_CHANNEL);
}

// Callback de envio (tarefa Wi-Fi, core 0): so contabilidade, sem logs nem locks.
static void on_send(const uint8_t *mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    tx_ledger_done(&s_ledger, status == ESP_NOW_SEND_SUCCESS, (uint32_t)esp_timer_get_time());
}

void espnow_tx_init(void) {
    wifi_base_init();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));

    static const uint8_t zero[6] = {0};
    if (!str_to_mac(CONFIG_TXV2_PEER_MAC, s_peer) || memcmp(s_peer, zero, 6) == 0) {
        ESP_LOGE(TAG, "MAC do peer invalido ou nao configurado: '%s' (CONFIG_TXV2_PEER_MAC). A abortar.",
                 CONFIG_TXV2_PEER_MAC);
        abort();
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer, 6);
    peer.channel = TXV2_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    esp_now_rate_config_t rate_cfg = {.phymode = TXV2_PHYMODE, .rate = TXV2_RATE, .ersu = false, .dcm = false};
    const esp_err_t err = esp_now_set_peer_rate_config(s_peer, &rate_cfg);
    if (err != ESP_OK) {
        // Nunca degradar em silencio para a taxa por omissao (Fase 1, run 001).
        ESP_LOGE(TAG, "esp_now_set_peer_rate_config falhou (%s). A abortar.", esp_err_to_name(err));
        abort();
    }

    char macstr[18];
    mac_to_str(s_peer, macstr);
    ESP_LOGI(TAG, "ESP-NOW TX pronto, peer=%s phymode=%d rate=%d", macstr, TXV2_PHYMODE, TXV2_RATE);
}

void espnow_tx_send(const uint8_t *pkt, size_t len) {
    tx_ledger_begin(&s_ledger, (uint32_t)esp_timer_get_time());
    const esp_err_t err = esp_now_send(s_peer, pkt, len);
    if (err == ESP_OK) {
        tx_ledger_accepted(&s_ledger);
    } else {
        tx_ledger_rejected(&s_ledger, err);  // descartado: nunca reenviar nem bloquear
    }
}

void espnow_tx_read(tx_ledger_snap_t *out) {
    tx_ledger_read(&s_ledger, out);
}
