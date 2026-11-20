#pragma once

#include "bench_common.h"
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "sdkconfig.h"

// TX: init wifi+ESP-NOW, adiciona o peer configurado em Kconfig e aplica
// o PHY mode / rate fixos escolhidos. Aborta com ESP_ERROR_CHECK se o
// MAC do peer nao estiver configurado (00:00:00:00:00:00).
void espnow_tx_init(void);
void espnow_tx_send(const uint8_t *pkt, size_t len);
void espnow_tx_get_stats(bench_tx_stats_t *out);

// RX: init wifi+ESP-NOW, regista callback interno que reencaminha cada
// pacote recebido para bench_on_packet_received() (definido em
// bench_main.c), com rssi/phy_rate extraidos de wifi_pkt_rx_ctrl_t.
void espnow_rx_init(void);

// TX: aplica em runtime a taxa PHY/modo e (se > 0) a potencia maxima.
// Devolve ESP_OK ou o erro do driver (o chamador decide; nunca degradar em silencio).
esp_err_t espnow_tx_apply_step(uint8_t phymode, uint8_t rate, uint8_t power_qdbm);

// PHY mode / taxa escolhidos no Kconfig (usados no arranque e no cabecalho dos pacotes).
#if CONFIG_AUDIOLINK_ESPNOW_PHYMODE_11B
#define BENCH_PHYMODE WIFI_PHY_MODE_11B
#elif CONFIG_AUDIOLINK_ESPNOW_PHYMODE_11G
#define BENCH_PHYMODE WIFI_PHY_MODE_11G
#elif CONFIG_AUDIOLINK_ESPNOW_PHYMODE_HT20
#define BENCH_PHYMODE WIFI_PHY_MODE_HT20
#else
#define BENCH_PHYMODE WIFI_PHY_MODE_HT40
#endif

#if CONFIG_AUDIOLINK_ESPNOW_RATE_1M_L
#define BENCH_PHYRATE WIFI_PHY_RATE_1M_L
#elif CONFIG_AUDIOLINK_ESPNOW_RATE_11M_S
#define BENCH_PHYRATE WIFI_PHY_RATE_11M_S
#elif CONFIG_AUDIOLINK_ESPNOW_RATE_6M
#define BENCH_PHYRATE WIFI_PHY_RATE_6M
#elif CONFIG_AUDIOLINK_ESPNOW_RATE_24M
#define BENCH_PHYRATE WIFI_PHY_RATE_24M
#elif CONFIG_AUDIOLINK_ESPNOW_RATE_54M
#define BENCH_PHYRATE WIFI_PHY_RATE_54M
#elif CONFIG_AUDIOLINK_ESPNOW_RATE_MCS0_LGI
#define BENCH_PHYRATE WIFI_PHY_RATE_MCS0_LGI
#elif CONFIG_AUDIOLINK_ESPNOW_RATE_MCS4_LGI
#define BENCH_PHYRATE WIFI_PHY_RATE_MCS4_LGI
#else
#define BENCH_PHYRATE WIFI_PHY_RATE_MCS7_SGI
#endif


// Modo dual: o wifi ja foi iniciado pelo modulo SoftAP+UDP; so falta ligar o ESP-NOW.
// TX: peer = BSSID do AP a que o STA se associou (MAC da interface AP do RX).
void espnow_tx_attach(const uint8_t peer_mac[6]);
void espnow_rx_attach(void);
