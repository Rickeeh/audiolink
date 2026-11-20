#pragma once

#include "bench_common.h"

// RX = SoftAP + servidor UDP. Arranca o AP, abre um socket UDP e lanca
// uma tarefa que faz recvfrom() em loop, encaminhando cada datagrama
// para bench_on_packet_received() com rssi=0, phy_rate=0 (nao exposto
// nesta via nesta versao do bench).
void softap_udp_rx_init(void);

// TX = STA a ligar ao SoftAP acima, depois cliente UDP unicast para o
// gateway do AP. Bloqueia ate obter IP (ou aborta ao fim de um timeout).
void softap_udp_tx_init(void);
void softap_udp_tx_send(const uint8_t *pkt, size_t len);
void softap_udp_tx_get_stats(bench_tx_stats_t *out);

// TX: BSSID do AP a que o STA esta associado (= MAC da interface AP do RX, alvo do ESP-NOW em modo dual).
void softap_udp_tx_get_bssid(uint8_t out[6]);
