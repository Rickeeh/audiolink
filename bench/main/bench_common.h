// AUDIOLINK Fase 1 - benchmark de radio
//
// Formato de pacote e estruturas partilhadas entre TX e RX, e entre os
// dois transportes (ESP-NOW / SoftAP+UDP). Isto NAO e o protocolo final
// do produto (secao 4.4 do brief) - e deliberadamente mais simples,
// porque o unico objetivo aqui e medir o transporte em si.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_MAGIC_DATA 0x4142454eu  // "ABEN" - AudioBench data packet
#define BENCH_MAGIC_END  0x4142454eu  // reaproveita magic, distingue por flag abaixo

// Cabecalho do pacote de benchmark. O resto do pacote ate ao tamanho do
// passo e enchimento (nao interpretado). Os parametros do passo TX vao no
// cabecalho para o RX conseguir documentar cada run sozinho (o TX pode
// estar longe, alimentado a bateria, sem ligacao serie).
typedef struct __attribute__((packed)) {
    uint32_t magic;          // BENCH_MAGIC_DATA sempre
    uint32_t run_id;         // aleatorio por passo/run
    uint32_t seq;            // 0..N-1, monotonico dentro de um run_id
    uint8_t  is_end;         // 0 = dados, 1 = marcador de fim
    uint8_t  tx_power_qdbm;  // potencia maxima aplicada no TX (x0.25 dBm)
    uint8_t  phymode;        // wifi_phy_mode_t configurado no TX
    uint8_t  rate;           // wifi_phy_rate_t configurado no TX
    int64_t  tx_time_us;     // esp_timer_get_time() no momento do envio
    uint32_t interval_us;    // intervalo de pacing do passo
    uint8_t  step_idx;       // indice do passo no sweep (0 fora de sweep)
    uint8_t  cycle;          // volta do sweep (0 fora de sweep)
    uint8_t  proto;          // BENCH_PROTO_*: 0 = ESP-NOW, 1 = UDP sobre SoftAP
    uint8_t  _pad[1];
} bench_pkt_hdr_t;

_Static_assert(sizeof(bench_pkt_hdr_t) == 32, "cabecalho deve ter 32 bytes (= tamanho minimo de pacote)");

// Um passo de teste (TX). Fora do modo sweep ha um unico passo vindo do Kconfig.
typedef struct {
    uint8_t  proto;        // BENCH_PROTO_* (so relevante em modo dual)
    uint8_t  phymode;
    uint8_t  rate;
    uint8_t  power_qdbm;   // 0 = nao alterar
    uint16_t payload;      // bytes por pacote, incl. cabecalho
    uint32_t interval_us;
    uint16_t duration_s;
} bench_step_t;

#define BENCH_HDR_SIZE (sizeof(bench_pkt_hdr_t))

#define BENCH_PROTO_ESPNOW 0
#define BENCH_PROTO_UDP    1
#define BENCH_PHYMODE_UDP_AUTO 0xFF  // marcador no cabecalho: UDP com a taxa escolhida pelo rate control do driver

// Quais transportes estao compilados neste build.
#if CONFIG_AUDIOLINK_TRANSPORT_ESPNOW || CONFIG_AUDIOLINK_TRANSPORT_DUAL
#define BENCH_HAS_ESPNOW 1
#else
#define BENCH_HAS_ESPNOW 0
#endif
#if CONFIG_AUDIOLINK_TRANSPORT_SOFTAP_UDP || CONFIG_AUDIOLINK_TRANSPORT_DUAL
#define BENCH_HAS_UDP 1
#else
#define BENCH_HAS_UDP 0
#endif

// Um registo por pacote recebido, guardado em PSRAM pelo RX e despejado
// em CSV no fim do teste. 24 bytes fixos.
typedef struct __attribute__((packed)) {
    uint32_t seq;
    int64_t  tx_time_us;
    int64_t  rx_time_us;
    int8_t   rssi;   // so preenchido em modo ESP-NOW; 0 em SoftAP+UDP
    uint8_t  phy_rate; // idem; rx_ctrl->rate (so valido em frames non-HT/11bg)
    uint8_t  mcs;      // rx_ctrl->mcs (frames HT)
    uint8_t  flags;    // sig_mode(bits0-1) | cwb<<2 | sgi<<3 | aggregation<<4
} bench_record_t;

_Static_assert(sizeof(bench_record_t) == 24, "bench_record_t deve manter-se em 24 bytes");

// Estatisticas do lado TX, para o relatorio final via log serial.
typedef struct {
    uint32_t sent_ok;        // ESP-NOW: callback ESP_NOW_SEND_SUCCESS (ACK recebido)
    uint32_t send_fail;      // ESP-NOW: callback ESP_NOW_SEND_FAIL (sem ACK apos retransmissoes MAC)
    uint32_t api_err;        // a propria chamada de envio devolveu erro (nem chegou a ser enviado)
    int32_t  last_api_err;   // ultimo esp_err_t / errno visto em api_err
} bench_tx_stats_t;

void bench_nvs_init(void);

// RX em modo SCAN_ONLY: inicia o wifi em STA e faz scans periodicos (nunca retorna).
void bench_scan_loop(uint32_t period_s);
void bench_mac_to_str(const uint8_t mac[6], char out[18]);
bool bench_str_to_mac(const char *s, uint8_t out[6]);

// Chamado pelo transporte ativo (ESP-NOW ou SoftAP+UDP) de cada vez que
// chega um pacote no RX. Implementado em bench_main.c. rssi/phy_rate sao
// 0 quando o transporte nao os expoe (SoftAP+UDP).
void bench_on_packet_received(const uint8_t *data, size_t len, int8_t rssi, uint8_t phy_rate,
                              uint8_t mcs, uint8_t flags);

#ifdef __cplusplus
}
#endif
