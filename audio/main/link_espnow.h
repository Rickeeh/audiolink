#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parser partilhado de "AA:BB:CC:DD:EE:FF" (usado por ambos TX e RX para ler
// AUDIOLINK_LINK_PEER_MAC). false se a string nao tiver o formato esperado.
bool link_espnow_mac_from_str(const char *s, uint8_t out[6]);

// TX: liga Wi-Fi (STA, canal fixo, sem AP/scan) + ESP-NOW, regista o peer
// (MAC de Kconfig) e aplica o PHY mode/taxa fixos escolhidos. Aborta se o
// MAC do peer nao estiver configurado - mais vale falhar alto e bom som do
// que um TX a "enviar" para o vazio sem ninguem perceber.
void link_espnow_tx_init(void);
void link_espnow_tx_send(const uint8_t *pkt, size_t len);

typedef struct {
    uint32_t api_accept;         // esp_now_send() devolveu ESP_OK (aceite pela API; ainda nao e "entregue")
    uint32_t send_ok;            // callback ESP_NOW_SEND_SUCCESS (ACK recebido)
    uint32_t send_fail;          // callback ESP_NOW_SEND_FAIL (sem ACK apos retransmissoes MAC)
    uint32_t api_err_no_mem;     // esp_now_send() devolveu ESP_ERR_ESPNOW_NO_MEM (fila interna cheia)
    uint32_t api_err_other;      // esp_now_send() devolveu qualquer outro erro
    esp_err_t last_err;          // ultimo codigo exato devolvido por esp_now_send() (0 = nenhum ainda)
    uint32_t pending;            // envios com esp_now_send() OK mas sem callback ainda (fila interna estimada)
    uint32_t backpressure_drop;  // blocos que o TX preferiu NAO enviar (pending >= AUDIOLINK_LINK_TX_MAX_PENDING)
#if CONFIG_AUDIOLINK_WIFI_REINIT
    // WIFI-REINIT-01: com reinits, api_accept = send_ok + send_fail + pending + reinit_abandoned
    uint32_t reinit_abandoned;   // entradas de pending descartadas pelos reinits (envios aceites cuja callback nunca vira)
    uint32_t link_down_drop;     // blocos nao enviados porque o ultimo reinit falhou (ESP-NOW nao reposto)
#endif
} link_tx_stats_t;
// So contadores escalares (leitura O(1), sem ordenar nada) - seguro chamar do caminho real-time do TX.
// PATCH-OBS-TX-01 (2026-10-01): as latencias send()->callback SAIRAM daqui (qsort deixou de correr no
// loop de playout do TX) - ver link_espnow_tx_get_lat_samples() abaixo.
void link_espnow_tx_get_stats(link_tx_stats_t *out);

// Capacidade do buffer de amostras de latencia send()->callback (ver link_espnow_tx_get_lat_samples).
#define LINK_TX_LAT_SAMPLE_CAP 1024

// PATCH-OBS-TX-01: copia ATE cap amostras RAW (sem ordenar) da janela atual de latencia
// send()->callback para out, e reinicia a janela (mesma semantica de sempre: janela desde a ultima
// chamada). Devolve quantas copiou. Nao faz qsort nem calcula media/percentis - isso e
// responsabilidade de quem chama, fora do caminho real-time do TX (ver tx_diag_task em link_tx.c,
// mesmo principio do rx_diag_task em link_rx.c: o produtor so copia+reseta, o custo de CPU de
// ordenar fica com a tarefa de diagnostico). Chamar com cap >= LINK_TX_LAT_SAMPLE_CAP.
uint32_t link_espnow_tx_get_lat_samples(uint32_t *out, uint32_t cap);

#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
// TRACE-TX-BOOT-01 (diagnostico, DEEP-TX-DEGRADATION-02). trace_init: chamar 1x no init do TX,
// depois de link_espnow_tx_init() e do resto das reservas, ANTES do 1o link_espnow_tx_send() -
// mede a RAM interna e reserva os 2 buffers (ou desativa o trace, sem fallback). trace_dump: so da
// tx_diag_task; devolve true quando ja nao ha nada a fazer (despejado ou desativado), false se a
// janela ainda nao fechou nos dois produtores (tentar mais tarde).
void link_espnow_trace_init(void);
bool link_espnow_trace_dump(void);
#endif

#if CONFIG_AUDIOLINK_WIFI_REINIT
// WIFI-REINIT-01 (experimental, Kconfig desligado por omissao): reinicializacao completa do caminho
// Wi-Fi/ESP-NOW do TX, sem reboot. Sequencia (desmontagem e a inversa exata da montagem):
//   desmontar: snapshot (canal/largura/PS) -> bloquear novos envios -> esperar pending == 0 (timeout
//              LINK_REINIT_DRAIN_TIMEOUT_MS; se nao drenar, fica registado e segue) -> unregister_send_cb
//              -> esp_now_deinit -> descartar o pending residual -> esp_wifi_stop
//   montar:    esp_wifi_start -> largura -> canal -> PS (valores do snapshot) -> esp_now_init
//              -> register_send_cb -> add_peer -> rate -> verificacao (canal/largura/PS/peer relidos)
// Se a montagem ou a verificacao falharem, desmonta e volta a montar UMA vez. Nenhuma falha faz abort()
// ou reboot: se a 2a tentativa falhar, o ESP-NOW fica em baixo, link_espnow_tx_send() deixa de chamar
// esp_now_send() (conta link_down_drop) e o TX fica calado - quem orquestra deteta e decide.
// Pending: antes de desmontar espera-se que todas as callbacks em voo cheguem (pending == 0). Timeout
// acima do lifetime maximo derivado dos frames (~1,57 s) e ~3x a maior latencia send->callback observada
// (0,687 s, 20 boots do PHYCAL-01). Se mesmo assim sobrar pending, as entradas no momento do
// esp_now_deinit sao descartadas e contadas em reinit_abandoned (o callback ja nao esta registado,
// nunca chegam), e drain_ok=0 fica no resultado. Chamar SO da tarefa TX (a mesma
// que chama link_espnow_tx_send), nunca do contexto Wi-Fi/callback. Bloqueia a tarefa TX durante a
// sequencia (o I2S transborda: adc_ovf sobe, esperado).
#define LINK_REINIT_STEPS 15
#define LINK_REINIT_DRAIN_TIMEOUT_MS 2000
typedef enum { LINK_REINIT_OK = 0, LINK_REINIT_OK_RETRY = 1, LINK_REINIT_FAILED = 2 } link_reinit_outcome_t;
enum { LINK_OP_REINIT = 0, LINK_OP_PAUSE = 1, LINK_OP_ESPNOW = 2 };
typedef struct {
    uint32_t n;                        // ordinal da operacao desde o boot (1, 2, ...)
    uint8_t op;                        // LINK_OP_REINIT, LINK_OP_PAUSE ou LINK_OP_ESPNOW
    uint32_t hold_us;                  // so PAUSE: tempo real com os envios bloqueados depois da drenagem
    int64_t t_begin_us, t_end_us;      // esp_timer
    uint32_t pending_before;           // pending no inicio
    bool drain_ok;                     // pending chegou a 0 antes do timeout (1.a desmontagem)
    uint32_t drain_wait_us;            // tempo de espera pela drenagem (1.a desmontagem)
    uint32_t pending_after_drain;      // pending no fim da espera (0 se drenou)
    uint32_t abandoned;                // entradas de pending descartadas neste reinit
    uint8_t ch_before, ch_after;
    int bw_before, bw_after, ps_before, ps_after;  // -1 = leitura falhou
    bool peer_ok;                      // peer existe com canal/ifidx/encrypt esperados (verificacao final)
    uint8_t attempts;                  // montagens feitas (1 ou 2)
    link_reinit_outcome_t outcome;
    esp_err_t err[LINK_REINIT_STEPS];  // ultimo codigo de cada passo (ESP_OK se nao correu)
    uint32_t step_us[LINK_REINIT_STEPS];
    uint32_t heap_before, heap_after, heap_int_before, heap_int_after;
    uint32_t tx_stack_free;            // high-water mark da tarefa que chamou (bytes livres, minimo de sempre)
} link_reinit_result_t;
link_reinit_outcome_t link_espnow_tx_reinit(link_reinit_result_t *r);
// ESPNOW-REINIT-PROBE: o mesmo quiesce e ciclo de vida do ESP-NOW do reinit (unreg_cb -> deinit -> init ->
// register_send_cb -> add_peer -> taxa), mesma 2a tentativa, SEM esp_wifi_stop/start nem setters de
// canal/largura/PS (estado Wi-Fi so lido, antes e depois). Mesmo resultado/marcadores (op=ESPNOW).
link_reinit_outcome_t link_espnow_tx_espnow_reinit(link_reinit_result_t *r);
// PAUSE-PROBE (controlo do REINIT): o mesmo quiesce (bloquear envios, esperar pending == 0 com o mesmo
// timeout), envios bloqueados mais hold_us, reabrir. Nao toca em ESP-NOW/Wi-Fi/canal/largura/PS/peer (so
// leituras para snapshot e verificacao). Mesmo resultado/marcadores que o REINIT (op=PAUSE, attempts=0).
link_reinit_outcome_t link_espnow_tx_pause(link_reinit_result_t *r, uint32_t hold_us);
// Para o logger (tx_diag_task): ultima operacao iniciada (n, t_begin, op) e ultimo resultado completo.
// Devolvem o ordinal (0 = nenhum ainda).
uint32_t link_espnow_tx_reinit_begun(int64_t *t_begin_us, uint8_t *op);
uint32_t link_espnow_tx_reinit_last(link_reinit_result_t *out);
const char *link_espnow_reinit_step_name(int i);
#endif

// RX: liga Wi-Fi+ESP-NOW e regista o callback interno que entrega cada
// pacote a link_rx_on_packet() (definido em link_rx.c).
void link_espnow_rx_init(void);

// Implementado em link_rx.c: chamado (em contexto da tarefa Wi-Fi, nao
// bloquear aqui) sempre que chega um pacote. rssi vem de wifi_pkt_rx_ctrl_t (dBm).
// src_mac: MAC de origem (info->src_addr do ESP-NOW), 6 bytes, ou NULL se
// indisponivel - usado para detetar/filtrar emissores ALK1 que nao sejam o
// peer configurado (2026-09-30, investigacao de contaminacao por 2o TX).
void link_rx_on_packet(const uint8_t *data, size_t len, int8_t rssi, const uint8_t src_mac[6]);

#ifdef __cplusplus
}
#endif
