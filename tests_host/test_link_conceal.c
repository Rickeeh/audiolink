// Ocultacao sem repeticao (link_conceal.h, CONCEAL-FADE-01, docs/RX_CONCEAL_FADE_PROPOSAL.md §6).
// Modelo: um sinal continuo cortado em blocos de N tramas estereo int32 (24 bit alinhados a esquerda); um padrao de
// blocos que chegam ou nao; o fluxo de saida e a concatenacao do que link_conceal_block devolve em cada periodo.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../audio/main/link_conceal.h"
#include "check.h"

#define LOG2 7u
#define N (1u << LOG2)
#define FS 48077.0
#define FULL 8388607.0  // 2^23 - 1, em unidades de 24 bit

static int32_t s_silence[2 * N];

// amostra 24 bit alinhada a esquerda (byte baixo 0), como o RX a recebe
static int32_t s24(double v) {
    long x = lround(v * FULL);
    if (x > 8388607) x = 8388607;
    if (x < -8388608) x = -8388608;
    return (int32_t)((uint32_t)x << 8);
}

// bloco k de um sinal sem periodo de N tramas: L = soma de dois senos, R = outro seno (canais distinguiveis)
static void make_block(uint64_t k, int32_t *b, double amp) {
    for (uint32_t i = 0; i < N; i++) {
        const double t = (double)(k * N + i) / FS;
        b[2 * i] = s24(amp * (0.6 * sin(2 * M_PI * 997.0 * t) + 0.4 * sin(2 * M_PI * 3271.0 * t)));
        b[2 * i + 1] = s24(amp * sin(2 * M_PI * 1499.0 * t + 1.0));
    }
}

static int64_t absd(int64_t a, int64_t b) { return a > b ? a - b : b - a; }

static void test_normal_bitexact(void) {
    link_conceal_t st = {.audible = true};
    static int32_t last[2 * N], out[2 * N];
    for (uint64_t k = 0; k < 50; k++) {
        make_block(k, last, 0.9);
        CHECK(link_conceal_block(&st, true, last, out, s_silence, LOG2) == last);  // o proprio bloco, sem copia
        CHECK(st.audible);
    }
}

static void test_fade_in(void) {
    link_conceal_t st = {.audible = false};  // arranque
    static int32_t last[2 * N], out[2 * N];
    make_block(7, last, 1.0);
    CHECK(link_conceal_block(&st, true, last, out, s_silence, LOG2) == out);
    CHECK(st.audible);
    for (uint32_t i = 0; i < N; i++) {
        for (uint32_t c = 0; c < 2; c++) {
            CHECK_EQ_U64((int64_t)out[2 * i + c], ((int64_t)last[2 * i + c] * (int64_t)(i + 1)) >> LOG2);
        }
    }
    CHECK_EQ_U64(out[2 * (N - 1)], last[2 * (N - 1)]);  // termina com ganho 1: o bloco seguinte entra sem degrau
    CHECK_EQ_U64(out[2 * (N - 1) + 1], last[2 * (N - 1) + 1]);
    CHECK(absd(out[0], 0) <= absd(last[0], 0) / N + 1);  // comeca perto de zero
}

static void test_fade_out_mirrored(void) {
    link_conceal_t st = {.audible = true};
    static int32_t last[2 * N], out[2 * N];
    make_block(11, last, 1.0);
    CHECK(link_conceal_block(&st, false, last, out, s_silence, LOG2) == out);
    CHECK(!st.audible);
    for (uint32_t i = 0; i < N; i++) {
        const uint32_t j = N - 1 - i;
        for (uint32_t c = 0; c < 2; c++) {  // L continua L, R continua R
            CHECK_EQ_U64((int64_t)out[2 * i + c], ((int64_t)last[2 * j + c] * (int64_t)j) >> LOG2);
        }
    }
    // comeca junto da ultima amostra escrita (last[N-1] com ganho 1) e acaba em zero
    CHECK(absd(out[0], last[2 * (N - 1)]) <= absd(last[2 * (N - 1)], 0) / N + 1);
    CHECK(absd(out[1], last[2 * (N - 1) + 1]) <= absd(last[2 * (N - 1) + 1], 0) / N + 1);
    CHECK_EQ_U64(out[2 * (N - 1)], 0);
    CHECK_EQ_U64(out[2 * (N - 1) + 1], 0);
}

static void test_long_outage_no_repeat(void) {
    link_conceal_t st = {.audible = true};
    static int32_t last[2 * N], out[2 * N];
    make_block(3, last, 1.0);
    CHECK(link_conceal_block(&st, false, last, out, s_silence, LOG2) == out);  // 1 fade-out
    for (int k = 0; k < 400; k++) {  // ~1 s sem blocos (o LOW do JB40 dispara ao fim de ~1,0 s)
        CHECK(link_conceal_block(&st, false, last, out, s_silence, LOG2) == s_silence);
    }
    for (uint32_t i = 0; i < 2 * N; i++) {
        CHECK_EQ_U64(s_silence[i], 0);  // ninguem escreveu no silencio
    }
    CHECK(link_conceal_block(&st, true, last, out, s_silence, LOG2) == out);  // regresso com fade-in
    CHECK(st.audible);
}

static void test_extremes_no_overflow(void) {
    link_conceal_t st = {.audible = false};
    static int32_t last[2 * N], out[2 * N];
    for (uint32_t i = 0; i < 2 * N; i++) {
        last[i] = (i & 1) ? INT32_MIN : INT32_MAX;  // fora do formato 24 bit, de proposito
    }
    link_conceal_block(&st, true, last, out, s_silence, LOG2);
    CHECK_EQ_U64(out[2 * (N - 1)], INT32_MAX);
    CHECK_EQ_U64((int64_t)out[2 * (N - 1) + 1], (int64_t)INT32_MIN);
    for (uint32_t i = 0; i < 2 * N; i++) {
        CHECK((i & 1) ? out[i] < 0 : out[i] > 0);
    }
    link_conceal_block(&st, false, last, out, s_silence, LOG2);
    CHECK_EQ_U64(out[2 * (N - 1)], 0);
}

// Fluxo completo: padroes de perda pseudo-aleatorios sobre musica sintetica. Em toda a saida:
//   - nenhum salto entre amostras consecutivas maior do que o maior salto natural do sinal + 1/N da amplitude
//     (o regresso com fade-in pode juntar dois saltos pequenos, dai a folga de 2/N);
//   - nenhum bloco de saida nao nulo e igual ao anterior (repeticao periodica = o "beep");
//   - os blocos que chegam depois de um bloco que chegou saem bit-exatos.
static void test_stream(uint32_t seed, int loss_permille, int burst_max) {
    link_conceal_t st = {.audible = false};
    static int32_t src[2 * N], last[2 * N], out[2 * N], prev[2 * N];
    int64_t max_nat = 0, max_out = 0, prev_s[2] = {0, 0};
    int have_prev = 0, prev_real = 0;
    uint32_t r = seed;
    int burst = 0;
    for (uint64_t k = 0; k < 3000; k++) {
        r = r * 1664525u + 1013904223u;
        int real;
        if (burst > 0) {
            burst--;
            real = 0;
        } else if ((int)((r >> 8) % 1000) < loss_permille) {
            burst = (int)((r >> 20) % (uint32_t)burst_max);
            real = 0;
        } else {
            real = 1;
        }
        make_block(k, src, 0.95);
        for (uint32_t i = 1; i < N; i++) {
            const int64_t d = absd(src[2 * i], src[2 * (i - 1)]);
            max_nat = d > max_nat ? d : max_nat;
        }
        if (real) {
            memcpy(last, src, sizeof(src));
        }
        int32_t *o = link_conceal_block(&st, real, last, out, s_silence, LOG2);
        if (real && prev_real) {
            CHECK(o == last);
        }
        for (uint32_t i = 0; i < N; i++) {
            for (uint32_t c = 0; c < 2; c++) {
                const int64_t d = absd(o[2 * i + c], prev_s[c]);
                max_out = d > max_out ? d : max_out;
                prev_s[c] = o[2 * i + c];
            }
        }
        int nonzero = 0;
        for (uint32_t i = 0; i < 2 * N; i++) {
            nonzero |= o[i] != 0;
        }
        if (have_prev && nonzero) {
            CHECK(memcmp(o, prev, sizeof(prev)) != 0);
        }
        memcpy(prev, o, sizeof(prev));
        have_prev = 1;
        prev_real = real;
    }
    CHECK(max_out <= max_nat + 2 * (int64_t)(0.95 * FULL * 256.0) / N + 2);
}

int main(void) {
    test_normal_bitexact();
    test_fade_in();
    test_fade_out_mirrored();
    test_long_outage_no_repeat();
    test_extremes_no_overflow();
    test_stream(1, 5, 2);      // perdas isoladas
    test_stream(2, 20, 6);     // rajadas curtas
    test_stream(3, 3, 400);    // falhas longas
    test_stream(4, 300, 3);    // radio muito mau
    return check_done("test_link_conceal");
}
