// deps: tx_v2/main/tx_ledger.c
// Contabilidade do envio (tx_ledger): sem limite em voo, recusa sem rollback, latencia por ordem, janelas, wrap.
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "tx_ledger.h"

static tx_ledger_t L;

static void reset(void) {
    memset(&L, 0, sizeof(L));
}

static void send_ok(uint32_t t) {  // uma tentativa aceite
    tx_ledger_begin(&L, t);
    tx_ledger_accepted(&L);
}

int main(void) {
    tx_ledger_snap_t s;

    // 1. Ciclo basico: um envio, uma callback.
    reset();
    send_ok(100);
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.pending, 1);
    CHECK_EQ_U64(s.pend_max, 1);
    tx_ledger_done(&L, true, 950);
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.accept, 1);
    CHECK_EQ_U64(s.ok, 1);
    CHECK_EQ_U64(s.pending, 0);
    CHECK_EQ_U64(s.lat_sum_us, 850);
    CHECK_EQ_U64(s.lat_max_us, 850);
    CHECK_EQ_U64(s.pend_max, 0);  // janela lida e reposta na leitura anterior

    // 2. Sem limite (E1b): 40 envios em voo sao todos aceites; o em voo e so metrica; a latencia continua certa
    //    com mais em voo do que os 8 de antes (posicoes = accept % SLOTS).
    reset();
    for (uint32_t i = 0; i < 40; i++) {
        send_ok(1000 + i);
    }
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.accept, 40);
    CHECK_EQ_U64(s.pending, 40);
    CHECK_EQ_U64(s.pend_max, 40);
    for (uint32_t i = 0; i < 40; i++) {
        tx_ledger_done(&L, true, 5000 + i * 10);  // latencia do i-esimo = 4000 + 9 i
    }
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.pending, 0);
    CHECK_EQ_U64(s.ok, 40);
    CHECK_EQ_U64(s.lat_max_us, 4000 + 9 * 39);
    CHECK_EQ_U64(s.lat_sum_us, 40 * 4000 + 9 * (39 * 40 / 2));

    // 3. Recusa: nao conta como aceite, guarda o erro, e a tentativa seguinte reutiliza a posicao do timestamp.
    reset();
    tx_ledger_begin(&L, 500);
    tx_ledger_rejected(&L, 0x3066);
    send_ok(700);  // mesma posicao (accept continua 0), timestamp 700 substitui o 500
    tx_ledger_done(&L, true, 1700);
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.reject, 1);
    CHECK_EQ_U64(s.last_err, 0x3066);
    CHECK_EQ_U64(s.accept, 1);
    CHECK_EQ_U64(s.lat_sum_us, 1000);  // 1700 - 700, nao 1700 - 500: sem rollback e sem atribuicao errada

    // 4. Latencia por ordem, com falhas no meio.
    reset();
    send_ok(100);
    send_ok(200);
    send_ok(300);
    tx_ledger_done(&L, true, 1100);
    tx_ledger_done(&L, false, 1250);
    tx_ledger_done(&L, true, 1400);
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.ok, 2);
    CHECK_EQ_U64(s.fail, 1);
    CHECK_EQ_U64(s.lat_sum_us, 1000 + 1050 + 1100);
    CHECK_EQ_U64(s.lat_max_us, 1100);
    CHECK_EQ_U64(s.pending, 0);

    // 5. Atribuicao correta ao longo de muitas voltas do anel (posicoes = accept % SLOTS).
    reset();
    uint32_t t = 0, lat_sum = 0;
    for (int i = 0; i < 1000; i++) {
        send_ok(t);
        const uint32_t lat = 300 + (uint32_t)(i % 7) * 100;
        tx_ledger_done(&L, true, t + lat);
        lat_sum += lat;
        t += 2662;
    }
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.lat_sum_us, lat_sum);
    CHECK_EQ_U64(s.lat_max_us, 900);

    // 6. Wrap dos contadores de 32 bit e do relogio de 32 bit em us.
    reset();
    atomic_store(&L.accept, 0xFFFFFFFEu);
    atomic_store(&L.ok, 0xFFFFFFFEu);
    send_ok(0xFFFFFF00u);  // accept passa a 0xFFFFFFFF
    send_ok(0xFFFFFFF0u);  // accept passa a 0 (wrap)
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.pending, 2);
    tx_ledger_done(&L, true, 0x00000100u);  // relogio deu a volta: 0x100 - 0xFFFFFF00 = 0x200
    tx_ledger_done(&L, true, 0x00000010u);  // 0x10 - 0xFFFFFFF0 = 0x20
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.pending, 0);
    CHECK_EQ_U64(s.lat_max_us, 0x200);
    CHECK_EQ_U64(s.lat_sum_us, 0x220);

    // 7. Callback antes de a tarefa RT contar o aceite (outro core): em voo nunca aparece negativo na leitura.
    reset();
    tx_ledger_begin(&L, 10);
    tx_ledger_done(&L, true, 20);  // chegou antes do tx_ledger_accepted
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.pending, 0);
    tx_ledger_accepted(&L);
    tx_ledger_read(&L, &s);
    CHECK_EQ_U64(s.pending, 0);
    CHECK_EQ_U64(s.accept, 1);
    CHECK_EQ_U64(s.lat_sum_us, 10);

    return check_done("test_tx_ledger");
}
