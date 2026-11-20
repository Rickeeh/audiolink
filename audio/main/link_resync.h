// Ressincronizacao do RX que toca a sequencia que a prova (RESYNC-KEEP-RUN, docs/RX_CONCEAL_FADE_PROPOSAL.md §6).
// Logica pura, sem IDF, com teste de host (tests_host/test_link_resync.c); a fila entra por link_resync_q_t.
//
// No REBUFFERING o RX so volta a tocar depois de ver n blocos com sample_ctr contiguos. Uma fila FreeRTOS so deixa ver
// o 1.o item, por isso verificar a contiguidade obriga a retirar os itens. Antes eram retirados e perdidos. Aqui vao
// para um anel de n itens (ficam os n mais recentes da sequencia) e, no resync, voltam a frente da fila.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    bool (*recv)(void *q, void *item);             // retira o 1.o item para item; false se a fila esta vazia
    bool (*send_front)(void *q, const void *item);  // poe uma copia de item a frente da fila; false se esta cheia
    uint64_t (*ctr)(const void *item);              // sample_ctr de um item
    void *q;
} link_resync_q_t;

// Um periodo de REBUFFERING. Retira tudo o que esta na fila para stage (anel de n itens de item_size bytes), seguindo
// a sequencia contigua mais recente (*run_start, *run_count; *items_seen conta os retirados). Ha resync quando essa
// sequencia tem >= n blocos E neste periodo se retiraram <= steady_pops itens: chegadas normais, nao a rajada com que o
// TX descarrega a sua fila depois de uma falha (ancorar no mais antigo da rajada deixava a fila acima do HIGH_WATER).
// No resync, os n mais recentes voltam a frente da fila, do mais recente para o mais antigo (a cabeca fica o mais
// antigo deles; o que chegar entretanto fica atras), e devolve true com *back = quantos voltaram (< n so com a fila
// cheia) e *head_ctr = sample_ctr do mais antigo que voltou, o novo expected_ctr.
static inline bool link_resync_period(uint64_t *run_start, uint32_t *run_count, uint32_t *items_seen, uint8_t *stage,
                                      size_t item_size, uint32_t n, uint32_t frames, uint32_t steady_pops,
                                      const link_resync_q_t *ops, uint32_t *back, uint64_t *head_ctr) {
    uint32_t popped = 0;
    for (;;) {
        uint8_t *slot = stage + (size_t)(*run_count % n) * item_size;
        if (!ops->recv(ops->q, slot)) {
            break;
        }
        const uint64_t ctr = ops->ctr(slot);
        (*items_seen)++;
        popped++;
        if (*run_count > 0 && ctr == *run_start + (uint64_t)*run_count * frames) {
            (*run_count)++;
        } else {
            if (slot != stage) {
                memcpy(stage, slot, item_size);  // quebra de contiguidade: a sequencia recomeca neste item
            }
            *run_start = ctr;
            *run_count = 1;
        }
    }
    *back = 0;
    if (*run_count < n || popped > steady_pops) {
        return false;
    }
    while (*back < n && ops->send_front(ops->q, stage + (size_t)((*run_count - 1 - *back) % n) * item_size)) {
        (*back)++;
    }
    *head_ctr = *run_start + (uint64_t)(*run_count - *back) * frames;
    return true;
}
