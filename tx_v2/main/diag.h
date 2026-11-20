// TX v2 - diagnostico fora do tempo real.
// A tarefa RT publica um snapshot minimo (spinlock curto, sem I/O). A tarefa tx_diag (core 0, prioridade 1) imprime uma
// linha LINKTX por segundo. Pode bloquear na consola; a tarefa RT nunca espera por ela nem escreve na consola.
#pragma once

#include <stdint.h>

void diag_start(uint64_t session_id, int64_t t0_us);
void diag_publish(uint32_t blocks, uint64_t sample_ctr);
void diag_task(void *arg);
