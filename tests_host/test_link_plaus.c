// Limite de plausibilidade do sample_ctr (link_plaus.h, docs/RX_REANCHOR_PROPOSAL.md §5 R0).
// Modelo do fluxo: o TX gera FRAMES tramas a cada FRAMES/FS s; o pacote k chega ao RX d_k us depois da captura.
#include <stdint.h>

#include "../audio/main/link_plaus.h"
#include "check.h"

#define FS 48077u
#define MARGIN 2u
#define FRAMES 128u
#define US 1000000LL

// sample_ctr de um pacote capturado no instante t_us (desde o arranque do TX), na grelha de FRAMES tramas
static uint64_t ctr_at(int64_t t_us, double ppm) {
    const double fs = FS * (1.0 + ppm * 1e-6);
    return ((uint64_t)((double)t_us * fs / 1e6) / FRAMES) * FRAMES;
}

// falha de radio de gap_us com o TX a correr: referencia aceite em t0, proximo pacote capturado em t0+gap e recebido
// com atraso d_now; a referencia tinha chegado com atraso d_ref.
static int after_gap(int64_t t0, int64_t gap_us, int64_t d_ref, int64_t d_now, double ppm) {
    const uint64_t ref = ctr_at(t0, ppm);
    const uint64_t cur = ctr_at(t0 + gap_us, ppm);
    return link_ctr_implausible(cur, ref, t0 + gap_us + d_now, t0 + d_ref, FS, MARGIN);
}

static void test_normal_flow(void) {
    // pacote seguinte (+128 tramas), jitter de chegada de +-5 ms
    CHECK(!link_ctr_implausible(1000 * FRAMES + FRAMES, 1000 * FRAMES, 2662 + 5000, 0, FS, MARGIN));
    CHECK(!link_ctr_implausible(1000 * FRAMES + FRAMES, 1000 * FRAMES, 0, 5000, FS, MARGIN));  // chegou "antes": elapsed<0
    for (int64_t t = 0; t < 3600 * US; t += 7 * US + 1234) {  // fluxo continuo, referencia sempre o pacote anterior
        CHECK(!after_gap(t, 2662, 0, 0, 0.0));
    }
}

static void test_outages_accepted(void) {
    const int64_t gaps[] = {1 * US, 2 * US, 4 * US, 30 * US, 60 * US, 300 * US, 3600 * US, 24 * 3600 * US};
    for (unsigned i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        CHECK(!after_gap(10 * US, gaps[i], 0, 0, 0.0));
        CHECK(!after_gap(10 * US, gaps[i], 0, 0, +100.0));   // TX 100 ppm mais rapido
        CHECK(!after_gap(10 * US, gaps[i], 0, 0, -100.0));
        CHECK(!after_gap(10 * US, gaps[i], 85000, 0, +100.0));  // referencia atrasada pela fila do TX (32 pacotes)
        CHECK(!after_gap(10 * US, gaps[i], 0, 0, +900.0));   // muito alem de qualquer cristal: dentro dos 1000 ppm
    }
}

static void test_old_rule_would_block(void) {
    // regressao do W2: falha de 4 s; com o limite fixo antigo (elapsed ignorado) era implausivel
    const uint64_t ref = ctr_at(10 * US, 0.0), cur = ctr_at(14 * US, 0.0);
    CHECK(link_ctr_implausible(cur, ref, 10 * US, 10 * US, FS, MARGIN));    // sem tempo decorrido = regra antiga
    CHECK(!link_ctr_implausible(cur, ref, 14 * US, 10 * US, FS, MARGIN));   // com o tempo real decorrido
}

static void test_poison_rejected(void) {
    // caso de 2026-09-30: contador congelado em ~562320000 quando o real andava muito abaixo
    const uint64_t real = ctr_at(600 * US, 0.0);
    CHECK(link_ctr_implausible(562320000ull, real, 601 * US, 600 * US, FS, MARGIN));
    CHECK(link_ctr_implausible(real + (uint64_t)3 * 3600 * FS, real, 601 * US, 600 * US, FS, MARGIN));  // +3 h em 1 s
    CHECK(link_ctr_implausible(real + (uint64_t)(MARGIN + 2) * FS, real, 601 * US, 600 * US, FS, MARGIN));
}

static void test_backwards_never_implausible(void) {
    CHECK(!link_ctr_implausible(0, 5000000, 10 * US, 0, FS, MARGIN));
    CHECK(!link_ctr_implausible(4999872, 5000000, 0, 0, FS, MARGIN));
}

static void test_delay_boundary(void) {
    // limite conhecido (proposta §4): referencia chegou d_ref mais atrasada do que o pacote atual
    CHECK(!after_gap(10 * US, 4 * US, 1900000, 0, 0.0));  // 1,9 s a mais: aceite
    CHECK(after_gap(10 * US, 4 * US, 2500000, 0, 0.0));   // 2,5 s a mais: rejeitado (caso residual documentado)
}

static void test_exact_limit(void) {
    // salto == limite e aceite; limite + 1 trama e rejeitado
    const int64_t el = 4 * US;
    const uint64_t ef = (uint64_t)(el * FS / US);
    const uint64_t lim = (uint64_t)MARGIN * FS + ef + ef / 1000;
    CHECK(!link_ctr_implausible(lim, 0, el, 0, FS, MARGIN));
    CHECK(link_ctr_implausible(lim + 1, 0, el, 0, FS, MARGIN));
}

static void test_no_overflow_long_uptime(void) {
    // ~1 ano de referencia antiga: elapsed_us * fs cabe em int64 (3,2e13 * 48077 = 1,5e18 < 9,2e18)
    const int64_t year = 365LL * 24 * 3600 * US;
    CHECK(!link_ctr_implausible((uint64_t)(year / US) * FS, 0, year, 0, FS, MARGIN));
}

int main(void) {
    test_normal_flow();
    test_outages_accepted();
    test_old_rule_would_block();
    test_poison_rejected();
    test_backwards_never_implausible();
    test_delay_boundary();
    test_exact_limit();
    test_no_overflow_long_uptime();
    return check_done("test_link_plaus");
}
