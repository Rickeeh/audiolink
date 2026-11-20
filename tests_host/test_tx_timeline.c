// E2: tempo logico (tx_timeline.h). sample_ctr de cada bloco, com overflows do DMA (tramas perdidas contam antes do bloco
// lido) e wrap do contador de overflows de 32 bit.
#include <stdint.h>

#include "check.h"
#include "tx_timeline.h"

int main(void) {
    tx_timeline_t t;

    // 1. Sem perdas: 0, 128, 256, ...; next e blocks acompanham.
    tx_timeline_init(&t, 0);
    for (uint64_t k = 0; k < 1000; k++) {
        CHECK_EQ_U64(tx_timeline_stamp(&t, 0), k * 128);
    }
    CHECK_EQ_U64(t.blocks, 1000);
    CHECK_EQ_U64(t.next, 1000 * 128);

    // 2. Overflow de 2 blocos antes do bloco lido: o carimbo salta 3 x 128 face ao anterior (2 perdidos + 1 normal).
    tx_timeline_init(&t, 7);  // o contador ja vinha de 7 (perdas antes do arranque nao contam)
    CHECK_EQ_U64(tx_timeline_stamp(&t, 7), 0);
    CHECK_EQ_U64(tx_timeline_stamp(&t, 7), 128);
    CHECK_EQ_U64(tx_timeline_stamp(&t, 9), 128 + 3 * 128);
    CHECK_EQ_U64(tx_timeline_stamp(&t, 9), 128 + 4 * 128);
    CHECK_EQ_U64(t.blocks, 4);                  // blocos lidos (= pacotes)
    CHECK_EQ_U64(t.next, (4 + 2) * 128);        // sample_ctr seguinte = (lidos + perdidos) x 128
    CHECK(t.next % 128 == 0);                   // a grelha de 128 tramas nunca se perde

    // 3. Wrap do contador de overflows (uint32 da interrupcao).
    tx_timeline_init(&t, 0xFFFFFFFFu);
    CHECK_EQ_U64(tx_timeline_stamp(&t, 0xFFFFFFFFu), 0);
    CHECK_EQ_U64(tx_timeline_stamp(&t, 1u), 128 + 2 * 128);  // 0xFFFFFFFF -> 1 = 2 perdidos
    CHECK_EQ_U64(t.ovf_seen, 1);

    // 4. sample_ctr de 64 bit nao da a volta onde um de 32 daria (~24,8 h a 48 kHz).
    tx_timeline_init(&t, 0);
    t.next = 0xFFFFFF80ull;  // ultimo bloco antes de 2^32 tramas
    CHECK_EQ_U64(tx_timeline_stamp(&t, 0), 0xFFFFFF80ull);
    CHECK_EQ_U64(tx_timeline_stamp(&t, 0), 0x100000000ull);
    return check_done("test_tx_timeline");
}
