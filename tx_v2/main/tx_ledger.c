// TX v2 - contabilidade do envio ESP-NOW (ver tx_ledger.h).
#include "tx_ledger.h"

static inline uint32_t ld(_Atomic uint32_t *v) {
    return atomic_load_explicit(v, memory_order_relaxed);
}

static inline void max_u32(_Atomic uint32_t *m, uint32_t v) {
    uint32_t cur = ld(m);
    while (v > cur && !atomic_compare_exchange_weak_explicit(m, &cur, v, memory_order_relaxed, memory_order_relaxed)) {
    }
}

void tx_ledger_begin(tx_ledger_t *l, uint32_t now_us) {
    atomic_store_explicit(&l->ts_us[ld(&l->accept) % TX_LEDGER_TS_SLOTS], now_us, memory_order_relaxed);
}

void tx_ledger_accepted(tx_ledger_t *l) {
    const uint32_t a = atomic_fetch_add_explicit(&l->accept, 1, memory_order_relaxed) + 1;
    const uint32_t in_flight = a - (ld(&l->ok) + ld(&l->fail));
    if ((int32_t)in_flight >= 0) {  // a callback deste envio pode ja ter chegado (0); nunca "negativo"
        max_u32(&l->pend_max, in_flight);
    }
}

void tx_ledger_rejected(tx_ledger_t *l, int32_t err) {
    atomic_fetch_add_explicit(&l->reject, 1, memory_order_relaxed);
    atomic_store_explicit(&l->last_err, err, memory_order_relaxed);
}

void tx_ledger_done(tx_ledger_t *l, bool success, uint32_t now_us) {
    const uint32_t done = ld(&l->ok) + ld(&l->fail);  // so esta callback escreve ok/fail
    const uint32_t lat = now_us - ld(&l->ts_us[done % TX_LEDGER_TS_SLOTS]);
    atomic_fetch_add_explicit(&l->lat_sum_us, lat, memory_order_relaxed);
    max_u32(&l->lat_max_us, lat);
    atomic_fetch_add_explicit(success ? &l->ok : &l->fail, 1, memory_order_relaxed);
}

void tx_ledger_read(tx_ledger_t *l, tx_ledger_snap_t *out) {
    out->ok = ld(&l->ok);
    out->fail = ld(&l->fail);
    out->lat_sum_us = ld(&l->lat_sum_us);
    out->accept = ld(&l->accept);
    out->reject = ld(&l->reject);
    out->last_err = atomic_load_explicit(&l->last_err, memory_order_relaxed);
    // Uma callback pode chegar antes de a tarefa RT contar o aceite do mesmo envio: em voo = -1 por instantes.
    const uint32_t in_flight = out->accept - (out->ok + out->fail);
    out->pending = (int32_t)in_flight < 0 ? 0 : in_flight;
    out->lat_max_us = atomic_exchange_explicit(&l->lat_max_us, 0, memory_order_relaxed);
    out->pend_max = atomic_exchange_explicit(&l->pend_max, 0, memory_order_relaxed);
}
