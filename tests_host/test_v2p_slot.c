// Regressao: chegada tardia nao pode invalidar o PCM de uma volta mais recente do anel.
#include <string.h>

#include "../experiments/V2P/rx/main/v2p_slot.h"
#include "check.h"

int main(void) {
    enum { STORE = 96 };
    static v2p_slot_t store[STORE];
    const int64_t old = 912, future = 1008, writer = 1000;
    CHECK(old <= writer && future > writer);
    CHECK_EQ_U64(old % STORE, future % STORE);
    v2p_slot_t *st = &store[future % STORE];
    st->tag = future + 1;
    st->ready = 1;
    for (unsigned i = 0; i < sizeof st->pcm / sizeof st->pcm[0]; i++)
        st->pcm[i] = (int32_t)(0x12340000u + i * 256u);
    v2p_slot_t before = *st;

    CHECK(v2p_slot_note_late(&store[old % STORE], old));  // continua a contar a chegada tardia
    CHECK(st->ready && st->tag == future + 1);  // criterio real de disponibilidade do escritor
    CHECK(memcmp(st, &before, sizeof *st) == 0);  // metadados e PCM intactos

    // Mesmo sem PCM pronto, um tag mais recente nao deve recuar nem perder o estado de reproducao.
    st->tag = future + 1;
    st->ready = 0;
    st->played = 1;
    before = *st;
    CHECK(v2p_slot_note_late(st, old));
    CHECK(memcmp(st, &before, sizeof *st) == 0);

    // Slot vazio ou antigo: manter o registo tardio e a supressao de contagens repetidas.
    const int64_t tags[] = {0, old + 1 - STORE, old + 1};
    for (unsigned i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        memset(st, 0, sizeof *st);
        st->tag = tags[i];
        CHECK(v2p_slot_note_late(st, old));
        CHECK_EQ_U64(st->tag, old + 1);
        CHECK(!st->ready && !st->played && st->late_seen);
        CHECK(!v2p_slot_note_late(st, old));
    }
    st->late_seen = 0;
    st->played = 1;
    before = *st;
    CHECK(!v2p_slot_note_late(st, old));  // duplicado de um bloco ja reproduzido
    CHECK(memcmp(st, &before, sizeof *st) == 0);

    return check_done("test_v2p_slot");
}
