// TX v2 - transporte ESP-NOW (unicast para o RX, canal 6, HT20, MCS4 LGI, power save desligado).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tx_ledger.h"

// Arranque do Wi-Fi + ESP-NOW. Chamar uma vez, da tarefa tx_audio (como o BEN: o arranque corre na tarefa de envio).
void espnow_tx_init(void);
// Caminho de tempo real: nunca bloqueia nem reenvia. Limite de pacotes em voo ou recusa da API: o bloco e descartado e
// contado (tx_ledger).
void espnow_tx_send(const uint8_t *pkt, size_t len);
// Diagnostico (fora do tempo real).
void espnow_tx_read(tx_ledger_snap_t *out);
