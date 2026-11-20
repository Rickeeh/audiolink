// Metadados do armazenamento V2P. O chamador detem s_store_mux; indices relativos a sessao.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alk_proto.h"

typedef struct {
    int64_t tag;  // bloco + 1 (0 = vazio)
    uint8_t ready, played, late_seen;
    int32_t pcm[ALK_FRAMES_PER_PKT * ALK_CHANNELS];  // PCM24 alinhado a esquerda para o I2S
} v2p_slot_t;

// Chamado apenas para um pacote tardio que passou a deduplicacao da captura.
// true: primeira chegada tardia a contabilizar; false: ja reproduzido/contabilizado.
static inline bool v2p_slot_note_late(v2p_slot_t *st, int64_t idx) {
    const bool same = st->tag == idx + 1;
    if (same && (st->played || st->late_seen)) return false;
    // Uma chegada antiga pode colidir por modulo com um bloco futuro ja pronto.
    // Contar o atraso sem recuar o tag nem invalidar esse bloco. A deduplicacao
    // anterior da captura evita contar novamente o pacote cujo tag nao guardamos.
    if (st->tag <= idx + 1) {
        st->tag = idx + 1;
        st->ready = 0;
        st->played = 0;
        st->late_seen = 1;
    }
    return true;
}
