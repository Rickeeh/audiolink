// TX v2 - tempo logico do stream: sample_ctr = indice, no relogio do ADC, da 1.a trama de cada bloco enviado. Pura;
// testada no host.
//
// Um overflow do DMA perde exatamente um bloco de 128 tramas, e e o mais antigo por ler (acq.h). Esses blocos sao mais
// antigos do que o bloco acabado de ler, por isso contam ANTES dele: o sample_ctr salta 128 por bloco perdido e o RX ve
// um buraco (ocultacao) em vez de uma timeline comprimida.
//
// Ler o contador de overflows logo a seguir a leitura e exato. Depois de a leitura tirar um buffer da fila do driver ha
// um lugar livre, e o overflow seguinte so pode acontecer >= 1 periodo (2,66 ms) depois.
#pragma once

#include <stdint.h>

#include "alk_proto.h"

typedef struct {
    uint64_t next;      // sample_ctr do proximo bloco (ja inclui as tramas perdidas conhecidas)
    uint32_t ovf_seen;  // ultimo valor lido do contador de overflows
    uint32_t blocks;    // blocos lidos (= pacotes produzidos; o 1.o e o bloco 0)
} tx_timeline_t;

static inline void tx_timeline_init(tx_timeline_t *t, uint32_t ovf_now) {
    t->next = 0;
    t->ovf_seen = ovf_now;
    t->blocks = 0;
}

// Chamar logo a seguir a cada bloco lido, com o contador de overflows desse instante. Devolve o sample_ctr do bloco.
static inline uint64_t tx_timeline_stamp(tx_timeline_t *t, uint32_t ovf_now) {
    t->next += (uint64_t)(uint32_t)(ovf_now - t->ovf_seen) * ALK_FRAMES_PER_PKT;
    t->ovf_seen = ovf_now;
    const uint64_t stamp = t->next;
    t->next += ALK_FRAMES_PER_PKT;
    t->blocks++;
    return stamp;
}
