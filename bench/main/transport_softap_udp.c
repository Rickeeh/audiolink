#include "transport_softap_udp.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "softap_udp";

static volatile uint32_t s_sent_ok = 0;
static volatile uint32_t s_send_fail = 0;
static volatile uint32_t s_api_err = 0;
static volatile int32_t s_last_api_err = 0;
static int s_tx_sock = -1;
static volatile int8_t s_udp_rssi = 0;  // RSSI do STA visto pelo AP (RX), atualizado a 4 Hz

// ---------------- RX (SoftAP + servidor UDP) ----------------

#define RX_UDP_STACK_SIZE 4096
#define RX_UDP_TASK_PRIORITY 20
#define RX_UDP_TASK_CORE 1

static void rssi_poll_task(void *arg) {
    (void)arg;
    while (1) {
        wifi_sta_list_t l;
        if (esp_wifi_ap_get_sta_list(&l) == ESP_OK && l.num > 0) {
            s_udp_rssi = l.sta[0].rssi;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void rx_udp_task(void *arg) {
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() falhou: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(CONFIG_AUDIOLINK_SOFTAP_UDP_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() falhou: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "servidor UDP a ouvir na porta %d", CONFIG_AUDIOLINK_SOFTAP_UDP_PORT);

    uint8_t buf[1500];
    while (1) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            ESP_LOGW(TAG, "recvfrom() erro: errno %d", errno);
            continue;
        }
        bench_on_packet_received(buf, (size_t)n, s_udp_rssi, 0, 0, 0);
    }
}

void softap_udp_rx_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, CONFIG_AUDIOLINK_SOFTAP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(CONFIG_AUDIOLINK_SOFTAP_SSID);
    strlcpy((char *)ap_cfg.ap.password, CONFIG_AUDIOLINK_SOFTAP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = CONFIG_AUDIOLINK_WIFI_CHANNEL;
    ap_cfg.ap.max_connection = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_AUDIOLINK_TX_POWER_QDBM > 0
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_AUDIOLINK_TX_POWER_QDBM));
#endif
    {
        uint8_t ap_mac[6];
        char s[18];
        ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, ap_mac));
        bench_mac_to_str(ap_mac, s);
        ESP_LOGI(TAG, "MAC da interface AP: %s", s);
    }
    xTaskCreatePinnedToCore(rssi_poll_task, "rssi_poll", 3072, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "SoftAP '%s' no canal %d", CONFIG_AUDIOLINK_SOFTAP_SSID, CONFIG_AUDIOLINK_WIFI_CHANNEL);

    xTaskCreatePinnedToCore(rx_udp_task, "rx_udp", RX_UDP_STACK_SIZE, NULL,
                             RX_UDP_TASK_PRIORITY, NULL, RX_UDP_TASK_CORE);
}

// ---------------- TX (STA + cliente UDP) ----------------

static EventGroupHandle_t s_wifi_evt;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA desligado do SoftAP, a tentar reconectar");
        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA ligado, IP=" IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

void softap_udp_tx_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, CONFIG_AUDIOLINK_SOFTAP_SSID, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, CONFIG_AUDIOLINK_SOFTAP_PASSWORD, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.channel = CONFIG_AUDIOLINK_WIFI_CHANNEL;
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#if CONFIG_AUDIOLINK_TX_POWER_QDBM > 0
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_AUDIOLINK_TX_POWER_QDBM));
#endif

    ESP_LOGI(TAG, "a ligar a '%s'...", CONFIG_AUDIOLINK_SOFTAP_SSID);
    // Nao abortar: o TX pode arrancar antes do AP existir ou com sinal fraco. Continua a tentar
    // (o handler de eventos reconecta) e regista o estado de 15 em 15 s.
    while (!(xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000)) &
             WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "ainda sem associacao a '%s' - a continuar a tentar", CONFIG_AUDIOLINK_SOFTAP_SSID);
    }

    s_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_tx_sock < 0) {
        ESP_LOGE(TAG, "socket() falhou: errno %d", errno);
        abort();
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_AUDIOLINK_SOFTAP_UDP_PORT);
    if (inet_pton(AF_INET, CONFIG_AUDIOLINK_SOFTAP_GW_IP, &dest.sin_addr) != 1) {
        ESP_LOGE(TAG, "IP de destino invalido: %s", CONFIG_AUDIOLINK_SOFTAP_GW_IP);
        abort();
    }
    // connect() em socket UDP so fixa o destino default; nao ha handshake.
    if (connect(s_tx_sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect() UDP falhou: errno %d", errno);
        abort();
    }

    ESP_LOGI(TAG, "SoftAP+UDP TX pronto, destino %s:%d", CONFIG_AUDIOLINK_SOFTAP_GW_IP,
             CONFIG_AUDIOLINK_SOFTAP_UDP_PORT);
}

void softap_udp_tx_send(const uint8_t *pkt, size_t len) {
    int n = send(s_tx_sock, pkt, len, 0);
    if (n < 0 || (size_t)n != len) {
        s_api_err++;
        s_last_api_err = errno;
    } else {
        s_sent_ok++;
    }
}

void softap_udp_tx_get_stats(bench_tx_stats_t *out) {
    out->sent_ok = s_sent_ok;
    out->send_fail = s_send_fail;
    out->api_err = s_api_err;
    out->last_api_err = s_last_api_err;
}

void softap_udp_tx_get_bssid(uint8_t out[6]) {
    wifi_ap_record_t ap;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap));
    memcpy(out, ap.bssid, 6);
}
