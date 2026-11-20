// TX v2 - contabilidade do envio ESP-NOW: pura (C11 + stdatomic, sem IDF), para ser testada no host.
//
// Um escritor por variavel:
//   tarefa RT (tx_audio, core 1): accept, reject, last_err, pend_max (maximo), ts_us[]
//   callback de envio (tarefa Wi-Fi, core 0): ok, fail, lat_sum_us, lat_max_us (maximo)
//   diag (core 0): so le; troca lat_max_us e pend_max por 0 em cada leitura (maximos por janela)
//
// Sem limite de envios em voo: o em voo e so metrica, nunca impede um envio (E1b).
//
// Em voo = accept - (ok + fail). O ESP-NOW chama a callback uma vez por cada envio aceite (esp_now.h), por isso a
// contagem e exata sem fila nenhuma.
//
// Latencia envio->callback:
//   - o instante de cada tentativa vai para ts_us[accept % SLOTS] ANTES do esp_now_send (as operacoes de fila do
//     esp_now_send sao barreiras: a callback ve o valor);
//   - a callback le ts_us[(ok + fail) % SLOTS];
//   - uma recusa nao incrementa accept, por isso a tentativa seguinte reutiliza a posicao (nao ha rollback);
//   - pressupoe callbacks pela ordem dos envios: PROVAVEL para um so peer, o mesmo pressuposto da fila legada. So a
//     atribuicao da latencia depende disto; as contagens nao.
// A latencia so e atribuida certa enquanto houver menos de SLOTS em voo (pending_max mostra-o). 64 > os 32 buffers TX
// dinamicos do Wi-Fi (sdkconfig), onde o esp_now_send passa a recusar: PROVAVEL, o limite interno do ESP-NOW nao e
// publico. As contagens nao dependem disto.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define TX_LEDGER_TS_SLOTS 64u

_Static_assert((TX_LEDGER_TS_SLOTS & (TX_LEDGER_TS_SLOTS - 1u)) == 0, "SLOTS potencia de 2 (indice por modulo exato com wrap)");

typedef struct {
    _Atomic uint32_t accept;
    _Atomic uint32_t reject;
    _Atomic int32_t last_err;
    _Atomic uint32_t pend_max;
    _Atomic uint32_t ok;
    _Atomic uint32_t fail;
    _Atomic uint32_t lat_sum_us;
    _Atomic uint32_t lat_max_us;
    _Atomic uint32_t ts_us[TX_LEDGER_TS_SLOTS];
} tx_ledger_t;

typedef struct {
    uint32_t accept, reject, ok, fail;
    uint32_t pending;      // em voo no instante da leitura
    uint32_t pend_max;     // maximo de em voo desde a leitura anterior
    uint32_t lat_sum_us;   // acumulado (o leitor faz deltas)
    uint32_t lat_max_us;   // maximo desde a leitura anterior
    int32_t last_err;
} tx_ledger_snap_t;

// RT, antes de cada esp_now_send: regista o instante. Nunca recusa.
void tx_ledger_begin(tx_ledger_t *l, uint32_t now_us);
// RT, depois do esp_now_send.
void tx_ledger_accepted(tx_ledger_t *l);
void tx_ledger_rejected(tx_ledger_t *l, int32_t err);
// Callback de envio.
void tx_ledger_done(tx_ledger_t *l, bool success, uint32_t now_us);
// Diagnostico.
void tx_ledger_read(tx_ledger_t *l, tx_ledger_snap_t *out);
