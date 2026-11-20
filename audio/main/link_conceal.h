// Ocultacao do RX sem repeticao de blocos (CONCEAL-FADE-01, docs/RX_CONCEAL_FADE_PROPOSAL.md §6). Logica pura, sem
// IDF, com teste de host (tests_host/test_link_conceal.c).
//
// Antes, cada periodo sem bloco real voltava a escrever o ultimo bloco a amplitude total: numa falha longa, um tom de
// fs/128 = 375,6 Hz ate o rebuffer disparar (0,6-1,0 s), e degraus de amplitude em todas as fronteiras. Agora:
//   - sem bloco, depois de som: o ultimo bloco real AO CONTRARIO, com ganho (N-1)/N .. 0. Comeca no valor da ultima
//     amostra escrita e acaba em zero: sem degrau e sem periodicidade. Nunca se repete mais do que este bloco;
//   - sem bloco, depois disso: silencio;
//   - bloco real depois de silencio ou de um fade-out: fade-in linear 1/N .. N/N, a terminar com ganho 1;
//   - bloco real depois de som: o proprio bloco, sem copia nem multiplicacao (bit-exato).
// Cada transicao deixa um unico degrau de no maximo 1/N do valor da amostra (N = 128: -42 dB da amostra).
// Nao e PLC: nao se estima conteudo novo.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool audible;  // true se a ultima amostra escrita foi sinal com ganho 1 (bloco real ou fim de um fade-in)
} link_conceal_t;

// Bloco a escrever neste periodo. real: o bloco devido chegou e foi copiado para last_block; senao last_block e o
// ultimo bloco real. Blocos estereo int32 intercalados de N = 2^frames_log2 tramas; out e silence com N tramas.
// Devolve last_block, out ou silence. O ganho aplica-se em int64 (amostras int32 alinhadas a esquerda, ganho <= N)
// com arredondamento para baixo: no maximo 1 unidade do int32, abaixo da resolucao de 24 bit.
static inline int32_t *link_conceal_block(link_conceal_t *st, bool real, int32_t *last_block, int32_t *out,
                                          int32_t *silence, uint32_t frames_log2) {
    const uint32_t n = 1u << frames_log2;
    if (real) {
        if (st->audible) {
            return last_block;
        }
        for (uint32_t i = 0; i < n; i++) {
            const int64_t g = (int64_t)i + 1;
            out[2 * i] = (int32_t)(((int64_t)last_block[2 * i] * g) >> frames_log2);
            out[2 * i + 1] = (int32_t)(((int64_t)last_block[2 * i + 1] * g) >> frames_log2);
        }
        st->audible = true;
        return out;
    }
    if (!st->audible) {
        return silence;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t j = n - 1 - i;  // ao contrario: a 1.a amostra do fade e a ultima que se escreveu
        const int64_t g = (int64_t)j;
        out[2 * i] = (int32_t)(((int64_t)last_block[2 * j] * g) >> frames_log2);
        out[2 * i + 1] = (int32_t)(((int64_t)last_block[2 * j + 1] * g) >> frames_log2);
    }
    st->audible = false;
    return out;
}
