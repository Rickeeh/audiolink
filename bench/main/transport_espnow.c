#include "transport_espnow.h"

#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "espnow";

static volatile uint32_t s_sent_ok = 0;
static volatile uint32_t s_send_fail = 0;
static volatile uint32_t s_api_err = 0;
static volatile int32_t s_last_api_err = 0;
static uint8_t s_peer_mac[6];

static void wifi_base_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Canal fixo, sem AP nem scan - o outro lado do ESP-NOW nao precisa
    // de estar associado, so no mesmo canal.
    // HT40 exige bandwidth 40 MHz e canal secundario definidos (senao
    // esp_now_set_peer_rate_config devolve ESP_ERR_ESPNOW_ARG). TX e RX
    // usam a mesma configuracao para o RX conseguir descodificar 40 MHz.
#if CONFIG_AUDIOLINK_ESPNOW_PHYMODE_HT40
    wifi_second_chan_t second = (CONFIG_AUDIOLINK_WIFI_CHANNEL <= 7) ? WIFI_SECOND_CHAN_ABOVE
                                                                     : WIFI_SECOND_CHAN_BELOW;
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));
#else
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
#endif
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_AUDIOLINK_WIFI_CHANNEL, second));

    // Decisao validada no audiocast (H1-D1 gate, ver relatorio de
    // reconhecimento): power-save do Wi-Fi tem de estar desligado para
    // nao introduzir latencia/jitter de dezenas de ms.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

#if CONFIG_AUDIOLINK_TX_POWER_QDBM > 0
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_AUDIOLINK_TX_POWER_QDBM));
#endif
    int8_t pwr = 0;
    ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&pwr));
    ESP_LOGI(TAG, "potencia TX max = %d (x0.25 dBm = %.2f dBm)", pwr, pwr * 0.25);

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    char macstr[18];
    bench_mac_to_str(mac, macstr);
    ESP_LOGI(TAG, "MAC STA deste no: %s (canal %d)", macstr, CONFIG_AUDIOLINK_WIFI_CHANNEL);
}

static void on_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_sent_ok++;
    } else {
        s_send_fail++;
    }
}

static void on_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len) {
    int8_t rssi = 0;
    uint8_t rate = 0, mcs = 0, flags = 0;
    if (info && info->rx_ctrl) {
        const wifi_pkt_rx_ctrl_t *c = info->rx_ctrl;
        rssi = c->rssi;
        rate = (uint8_t)c->rate;
        mcs = (uint8_t)c->mcs;
        flags = (uint8_t)((c->sig_mode & 3) | (c->cwb << 2) | (c->sgi << 3) | (c->aggregation << 4));
    }
    bench_on_packet_received(data, (size_t)data_len, rssi, rate, mcs, flags);
}

void espnow_tx_init(void) {
    wifi_base_init();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send_cb));

    uint8_t peer_mac[6];
    if (!bench_str_to_mac(CONFIG_AUDIOLINK_ESPNOW_PEER_MAC, peer_mac)) {
        ESP_LOGE(TAG, "MAC do peer invalido em Kconfig: '%s'", CONFIG_AUDIOLINK_ESPNOW_PEER_MAC);
        abort();
    }
    static const uint8_t zero_mac[6] = {0};
    if (memcmp(peer_mac, zero_mac, 6) == 0) {
        ESP_LOGE(TAG, "MAC do peer nao configurado (00:00:00:00:00:00). "
                      "Le o MAC do RX no log dele e configura em menuconfig.");
        abort();
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, 6);
    peer.channel = CONFIG_AUDIOLINK_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    esp_now_rate_config_t rate_cfg = {
        .phymode = BENCH_PHYMODE,
        .rate = BENCH_PHYRATE,
        .ersu = false,
        .dcm = false,
    };
    esp_err_t err = esp_now_set_peer_rate_config(peer_mac, &rate_cfg);
    if (err != ESP_OK) {
        // Nao degradar em silencio: um run rotulado com uma taxa que nao
        // foi aplicada produziria resultados falsos.
        ESP_LOGE(TAG, "esp_now_set_peer_rate_config falhou (%s) - combinacao "
                      "phymode/rate invalida ou canal/bandwidth incoerente. A abortar.",
                 esp_err_to_name(err));
        abort();
    }

    memcpy(s_peer_mac, peer_mac, 6);
    char macstr[18];
    bench_mac_to_str(peer_mac, macstr);
    ESP_LOGI(TAG, "ESP-NOW TX pronto, peer=%s phymode=%d rate=%d", macstr, BENCH_PHYMODE, BENCH_PHYRATE);
}

esp_err_t espnow_tx_apply_step(uint8_t phymode, uint8_t rate, uint8_t power_qdbm) {
    if (power_qdbm > 0) {
        esp_err_t e = esp_wifi_set_max_tx_power(power_qdbm);
        if (e != ESP_OK) {
            return e;
        }
    }
    esp_now_rate_config_t rate_cfg = {
        .phymode = (wifi_phy_mode_t)phymode,
        .rate = (wifi_phy_rate_t)rate,
        .ersu = false,
        .dcm = false,
    };
    return esp_now_set_peer_rate_config(s_peer_mac, &rate_cfg);
}

void espnow_tx_send(const uint8_t *pkt, size_t len) {
    esp_err_t err = esp_now_send(s_peer_mac, pkt, len);
    if (err != ESP_OK) {
        s_api_err++;
        s_last_api_err = err;
    }
}

void espnow_tx_get_stats(bench_tx_stats_t *out) {
    out->sent_ok = s_sent_ok;
    out->send_fail = s_send_fail;
    out->api_err = s_api_err;
    out->last_api_err = s_last_api_err;
}

void espnow_rx_init(void) {
    wifi_base_init();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv_cb));

    ESP_LOGI(TAG, "ESP-NOW RX pronto, a espera de pacotes no canal %d", CONFIG_AUDIOLINK_WIFI_CHANNEL);
}

void espnow_tx_attach(const uint8_t peer_mac[6]) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send_cb));
    memcpy(s_peer_mac, peer_mac, 6);
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, 6);
    peer.channel = 0;  // canal atual (o do AP a que o STA esta associado)
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    char macstr[18];
    bench_mac_to_str(peer_mac, macstr);
    ESP_LOGI(TAG, "ESP-NOW (dual) TX pronto, peer=%s", macstr);
}

void espnow_rx_attach(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv_cb));
    ESP_LOGI(TAG, "ESP-NOW (dual) RX pronto");
}
