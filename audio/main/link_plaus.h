// Limite de plausibilidade do sample_ctr no RX (docs/RX_REANCHOR_PROPOSAL.md). Logica pura, sem IDF, com teste de host
// (tests_host/test_link_plaus.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// true se sample_ctr esta implausivelmente a frente da referencia max_ctr (aceite em max_at_us).
// O sample_ctr do TX avanca com o tempo real, haja ou nao rececao: o salto plausivel e a margem fixa MAIS o que o TX
// avancou desde que a referencia foi aceite. Com o limite fixo antigo (so a margem), uma falha de radio maior do que
// a margem com o TX a correr deixava todos os pacotes seguintes implausiveis para sempre (W2, 2026-10-29).
// O tempo decorrido leva +1000 ppm de tolerancia: o relogio do TX pode ser mais rapido do que o do RX e, numa falha
// longa, a diferenca acumula (100 ppm em 24 h = 8,6 s, mais do que a margem fixa).
// Saltos para tras nunca sao implausiveis (atrasos/reordenacao tratam-se no playout).
static inline bool link_ctr_implausible(uint64_t sample_ctr, uint64_t max_ctr, int64_t now_us, int64_t max_at_us,
                                        uint32_t fs_hz, uint32_t margin_s) {
    const int64_t jump = (int64_t)sample_ctr - (int64_t)max_ctr;
    int64_t elapsed_us = now_us - max_at_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    const int64_t elapsed_frames = elapsed_us * (int64_t)fs_hz / 1000000;
    const int64_t limit = (int64_t)margin_s * (int64_t)fs_hz + elapsed_frames + elapsed_frames / 1000;
    return jump > limit;
}

// --- TESTE R2 (docs/RX_REANCHOR_PROPOSAL.md §5): so em builds de teste; em producao as macros sao inertes. ---
// Ficam aqui, e nao em link_rx.c, para os ganchos entrarem em linhas ja existentes de link_rx.c: acrescentar linhas
// la deslocaria os __LINE__ dos ESP_ERROR_CHECK e a build de producao deixaria de ser identica a auditada (R1).
#if defined(CONFIG_AUDIOLINK_TEST_RX_DEAF) && CONFIG_AUDIOLINK_TEST_RX_DEAF
// true dentro de uma das 3 janelas programadas (segundos de uptime, esp_timer)
static inline bool link_test_rx_deaf(int64_t now_us) {
    const int64_t s = now_us / 1000000;
    return (s >= CONFIG_AUDIOLINK_TEST_RX_DEAF_W1_START_S &&
            s < CONFIG_AUDIOLINK_TEST_RX_DEAF_W1_START_S + CONFIG_AUDIOLINK_TEST_RX_DEAF_W1_DUR_S) ||
           (s >= CONFIG_AUDIOLINK_TEST_RX_DEAF_W2_START_S &&
            s < CONFIG_AUDIOLINK_TEST_RX_DEAF_W2_START_S + CONFIG_AUDIOLINK_TEST_RX_DEAF_W2_DUR_S) ||
           (s >= CONFIG_AUDIOLINK_TEST_RX_DEAF_W3_START_S &&
            s < CONFIG_AUDIOLINK_TEST_RX_DEAF_W3_START_S + CONFIG_AUDIOLINK_TEST_RX_DEAF_W3_DUR_S);
}
// o pacote "nunca chegou": sai antes de qualquer contador ou estado do RX
#define LINK_RX_TEST_DEAF_HOOK()                       \
    do {                                               \
        if (link_test_rx_deaf(esp_timer_get_time())) { \
            return;                                    \
        }                                              \
    } while (0)
#else
#define LINK_RX_TEST_DEAF_HOOK() \
    do {                         \
    } while (0)
#endif
#if defined(CONFIG_AUDIOLINK_TEST_REANCHOR_OLD_RULE) && CONFIG_AUDIOLINK_TEST_REANCHOR_OLD_RULE
#define LINK_RX_TEST_REF_AT(now_us, ref_at_us) (now_us)  // tempo decorrido 0 = regra antiga (so a margem fixa)
#else
#define LINK_RX_TEST_REF_AT(now_us, ref_at_us) (ref_at_us)
#endif
