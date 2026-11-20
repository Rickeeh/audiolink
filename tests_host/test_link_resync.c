// Ressincronizacao que toca a sequencia (link_resync.h, RESYNC-KEEP-RUN, docs/RX_CONCEAL_FADE_PROPOSAL.md §6).
// Modelo: uma fila FIFO de capacidade fixa como a do FreeRTOS (so se ve o 1.o item; send_front poe a frente), itens
// com sample_ctr e um conteudo que depende dele (para provar que o que volta a fila e o bloco certo, intacto).
#include <stdint.h>
#include <string.h>

#include "../audio/main/link_resync.h"
#include "check.h"

#define FR 128u   // tramas por bloco
#ifndef B
#define B 15u     // BUFFER_BLOCKS (JB40/C1/L1 = 15, L2-12 = 12); o teste tambem corre com -DB=12 -DDEPTH=16
#endif
#ifndef DEPTH
#define DEPTH (B + 4u)  // QUEUE_DEPTH = B + 4
#endif
#define STEADY 2u

typedef struct {
    uint64_t ctr;
    uint8_t payload[40];
} item_t;

typedef struct {
    item_t a[64];
    uint32_t head, n, cap;
    int push_on_front;  // >0: o "produtor" escreve um item atras na k-esima chamada a send_front (concorrencia)
    uint64_t push_ctr;
} fifo_t;

static void fill(item_t *it, uint64_t ctr) {
    it->ctr = ctr;
    for (unsigned i = 0; i < sizeof it->payload; i++) {
        it->payload[i] = (uint8_t)(ctr / FR * 31u + i);
    }
}
static int intact(const item_t *it) {
    item_t ref;
    fill(&ref, it->ctr);
    return memcmp(&ref, it, sizeof ref) == 0;
}
static int push_back(fifo_t *f, uint64_t ctr) {  // o produtor (callback Wi-Fi)
    if (f->n == f->cap) {
        return 0;
    }
    fill(&f->a[(f->head + f->n) % 64], ctr);
    f->n++;
    return 1;
}
static bool q_recv(void *q, void *item) {
    fifo_t *f = q;
    if (!f->n) {
        return false;
    }
    memcpy(item, &f->a[f->head], sizeof(item_t));
    f->head = (f->head + 1) % 64;
    f->n--;
    return true;
}
static bool q_send_front(void *q, const void *item) {
    fifo_t *f = q;
    if (f->push_on_front > 0 && --f->push_on_front == 0) {
        push_back(f, f->push_ctr);  // chega um pacote novo a meio da devolucao
    }
    if (f->n == f->cap) {
        return false;
    }
    f->head = (f->head + 63) % 64;
    memcpy(&f->a[f->head], item, sizeof(item_t));
    f->n++;
    return true;
}
static uint64_t q_ctr(const void *item) { return ((const item_t *)item)->ctr; }

typedef struct {
    fifo_t f;
    link_resync_q_t ops;
    item_t stage[B];
    uint64_t start;
    uint32_t count, seen, back;
    uint64_t head;
} rig_t;

static void rig_init(rig_t *r) {
    memset(r, 0, sizeof *r);
    r->f.cap = DEPTH;
    r->ops = (link_resync_q_t){q_recv, q_send_front, q_ctr, &r->f};
}
static bool period(rig_t *r) {
    return link_resync_period(&r->start, &r->count, &r->seen, (uint8_t *)r->stage, sizeof(item_t), B, FR, STEADY,
                              &r->ops, &r->back, &r->head);
}
// a fila depois do resync: k itens a partir de first, contiguos, intactos e por ordem
static int queue_is(const rig_t *r, uint64_t first, uint32_t k) {
    if (r->f.n != k) {
        return 0;
    }
    for (uint32_t i = 0; i < k; i++) {
        const item_t *it = &r->f.a[(r->f.head + i) % 64];
        if (it->ctr != first + (uint64_t)i * FR || !intact(it)) {
            return 0;
        }
    }
    return 1;
}

static void test_steady(void) {
    // arranque/sessao nova: 1 pacote por periodo. Resync no periodo do 15.o, com os 15 tocaveis (antes: 0-2 na fila)
    rig_t r;
    rig_init(&r);
    const uint64_t c0 = 1000 * FR;
    for (uint32_t k = 0; k < B; k++) {
        push_back(&r.f, c0 + k * FR);
        const bool rs = period(&r);
        CHECK(rs == (k == B - 1));
    }
    CHECK_EQ_U64(r.back, B);
    CHECK_EQ_U64(r.head, c0);
    CHECK(queue_is(&r, c0, B));
    CHECK_EQ_U64(r.seen, B);  // nada descartado: seen - back = 0
}

static void test_jitter_pair_still_resyncs(void) {
    // jitter normal: 2 pacotes no mesmo periodo (um atrasado, um a horas) nao e uma rajada
    rig_t r;
    rig_init(&r);
    const uint64_t c0 = 2000 * FR;
    for (uint32_t k = 0; k < B - 2; k++) {
        push_back(&r.f, c0 + k * FR);
        CHECK(!period(&r));
    }
    push_back(&r.f, c0 + (B - 2) * FR);
    push_back(&r.f, c0 + (B - 1) * FR);
    CHECK(period(&r));
    CHECK(queue_is(&r, c0, B));
    // 3 no mesmo periodo ja conta como rajada: espera pelo periodo seguinte
    rig_init(&r);
    for (uint32_t k = 0; k < B - 3; k++) {
        push_back(&r.f, c0 + k * FR);
        period(&r);
    }
    for (uint32_t k = B - 3; k < B; k++) {
        push_back(&r.f, c0 + k * FR);
    }
    CHECK(!period(&r));
    CHECK(period(&r));
    CHECK(queue_is(&r, c0, B));
}

static void test_burst_anchors_newest(void) {
    // falha longa: o TX descarrega 32 pacotes atrasados numa rajada (8 por periodo) e depois volta a 1 por periodo.
    // Nao ha resync durante a rajada; no 1.o periodo normal ficam os 15 MAIS RECENTES (ocupacao = B, nao 32).
    rig_t r;
    rig_init(&r);
    uint64_t c = 5000 * FR;
    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < 8; i++, c += FR) {
            CHECK(push_back(&r.f, c));  // 8 por periodo cabem na fila (19)
        }
        CHECK(!period(&r));
    }
    push_back(&r.f, c);
    CHECK(period(&r));
    const uint64_t newest = c;
    CHECK_EQ_U64(r.back, B);
    CHECK_EQ_U64(r.head, newest - (B - 1) * FR);
    CHECK(queue_is(&r, newest - (B - 1) * FR, B));
    CHECK_EQ_U64(r.seen - r.back, 33 - B);  // os 18 mais antigos da rajada sao descartados
}

static void test_burst_then_quiet_period(void) {
    // rajada (a fila quase cheia) e depois um periodo sem chegadas: o resync faz-se nesse periodo (0 retirados <= STEADY)
    rig_t r;
    rig_init(&r);
    uint64_t c = 7 * FR;
    for (uint32_t i = 0; i < DEPTH - 1; i++, c += FR) {
        CHECK(push_back(&r.f, c));
    }
    CHECK(!period(&r));
    CHECK(period(&r));
    CHECK(queue_is(&r, c - B * FR, B));
}

static void test_gap_restarts_run(void) {
    // buraco: a sequencia recomeca no item a seguir, que tem de ficar intacto na posicao 0 do anel
    rig_t r;
    rig_init(&r);
    uint64_t c = 300 * FR;
    for (uint32_t k = 0; k < 9; k++, c += FR) {
        push_back(&r.f, c);
        CHECK(!period(&r));
    }
    c += 3 * FR;  // perdem-se 3
    const uint64_t c1 = c;
    for (uint32_t k = 0; k < B; k++, c += FR) {
        push_back(&r.f, c);
        CHECK(period(&r) == (k == B - 1));
    }
    CHECK_EQ_U64(r.head, c1);
    CHECK(queue_is(&r, c1, B));
    CHECK_EQ_U64(r.seen - r.back, 9);
}

static void test_gap_after_ring_wrap(void) {
    // o anel ja deu a volta (rajada de DEPTH-1 > B) e depois ha um buraco: recomeca bem a partir de qualquer posicao
    rig_t r;
    rig_init(&r);
    uint64_t c = 40 * FR;
    for (uint32_t i = 0; i < DEPTH - 1; i++, c += FR) {
        CHECK(push_back(&r.f, c));
    }
    c += FR;  // perde-se 1
    CHECK(push_back(&r.f, c));  // mesma rajada: DEPTH itens (fila cheia), o ultimo quebra a sequencia
    c += FR;
    CHECK(!period(&r));
    CHECK_EQ_U64(r.count, 1);
    const uint64_t c1 = c - FR;
    for (uint32_t k = 1; k < B; k++, c += FR) {
        push_back(&r.f, c);
        CHECK(period(&r) == (k == B - 1));
    }
    CHECK(queue_is(&r, c1, B));
}

static void test_reordered_breaks_run(void) {
    rig_t r;
    rig_init(&r);
    uint64_t c = 900 * FR;
    for (int k = 0; k < 5; k++, c += FR) {
        push_back(&r.f, c);
        period(&r);
    }
    push_back(&r.f, c - 3 * FR);  // pacote antigo fora de ordem
    CHECK(!period(&r));
    CHECK_EQ_U64(r.count, 1);
    CHECK_EQ_U64(r.start, c - 3 * FR);
}

static void test_producer_during_reinsert(void) {
    // chega um pacote (o seguinte) a meio da devolucao: tem de ficar ATRAS da sequencia, que fica intacta e por ordem
    rig_t r;
    rig_init(&r);
    const uint64_t c0 = 77 * FR;
    for (uint32_t k = 0; k < B - 1; k++) {
        push_back(&r.f, c0 + k * FR);
        period(&r);
    }
    r.f.push_on_front = 6;
    r.f.push_ctr = c0 + B * FR;
    push_back(&r.f, c0 + (B - 1) * FR);
    CHECK(period(&r));
    CHECK_EQ_U64(r.back, B);
    CHECK(queue_is(&r, c0, B + 1));  // os 15 + o que chegou
}

static void test_queue_full_on_reinsert(void) {
    // degenerado: so cabem 10 na devolucao. Voltam os 10 mais recentes, por ordem, e o resync comeca no 1.o deles
    rig_t r;
    rig_init(&r);
    const uint64_t c0 = 10 * FR;
    for (uint32_t k = 0; k < B; k++) {
        push_back(&r.f, c0 + k * FR);
        if (k < B - 1) {
            period(&r);
        }
    }
    r.f.cap = 10;
    CHECK(period(&r));
    CHECK_EQ_U64(r.back, 10);
    CHECK_EQ_U64(r.head, c0 + (B - 10) * FR);
    CHECK(queue_is(&r, c0 + (B - 10) * FR, 10));
}

static void test_long_run_no_overflow(void) {
    // horas de rajadas seguidas (nunca um periodo calmo): count cresce, os indices do anel ficam validos
    rig_t r;
    rig_init(&r);
    uint64_t c = 0;
    for (int p = 0; p < 20000; p++) {
        for (int i = 0; i < 3; i++, c += FR) {
            push_back(&r.f, c);
        }
        CHECK(!period(&r));
    }
    push_back(&r.f, c);
    CHECK(period(&r));
    CHECK(queue_is(&r, c - (B - 1) * FR, B));
}

int main(void) {
    test_steady();
    test_jitter_pair_still_resyncs();
    test_burst_anchors_newest();
    test_burst_then_quiet_period();
    test_gap_restarts_run();
    test_gap_after_ring_wrap();
    test_reordered_breaks_run();
    test_producer_during_reinsert();
    test_queue_full_on_reinsert();
    test_long_run_no_overflow();
    return check_done("test_link_resync");
}
