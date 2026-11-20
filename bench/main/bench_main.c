// AUDIOLINK Fase 1 - benchmark de radio (ESP-NOW vs SoftAP+UDP)
//
// Um binario, combinacoes via menuconfig: ROLE={TX,RX} x TRANSPORT={ESPNOW,SOFTAP_UDP}
// e, so no TX+ESPNOW, opcionalmente TX_SWEEP (tabela de pontos de operacao em ciclo).
//
// O RX acumula um registo por pacote em PSRAM e despeja tudo em CSV pela
// consola serial no fim de cada run (marcador de fim ou 3s de silencio). A
// analise (debito, perdas, rajadas, jitter) e feita depois em Python -
// propositadamente nao ha estatisticas no firmware, para nao esconder
// nenhum pacote bruto antes da analise.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "bench_common.h"

#if BENCH_HAS_ESPNOW
#include "transport_espnow.h"
#endif
#if BENCH_HAS_UDP
#include "transport_softap_udp.h"
#endif

#if CONFIG_AUDIOLINK_TX_SWEEP && !BENCH_HAS_ESPNOW
#error "AUDIOLINK_TX_SWEEP requer ESP-NOW ou o modo DUAL"
#endif

static const char *TAG = "bench";

// ================= RX: acumulacao e dump =================

#if CONFIG_AUDIOLINK_ROLE_RX

static bench_record_t *s_records;
static uint32_t s_record_count;
static uint32_t s_record_capacity;
static bool s_capacity_warned;

static uint32_t s_current_run_id;
static bool s_run_active;
static bool s_run_finalized;
static volatile bool s_dump_busy;  // enquanto true, pacotes que chegam sao ignorados
static int64_t s_last_activity_us;
static TaskHandle_t s_dump_task;

// parametros do run, tirados do primeiro pacote
static bench_pkt_hdr_t s_run_hdr;
static size_t s_run_len;
static uint32_t s_planned;  // total de pacotes de dados planeado, vindo do marcador de fim (0 = fim nao recebido)

static void do_dump(void) {
    printf("===AUDIOLINK_BENCH_DUMP_BEGIN===\n");
    printf("run_id=%lu records=%lu capacity=%lu overflowed=%d transport=%s channel=%d "
           "payload_size=%u tx_interval_us=%lu tx_power_qdbm=%u tx_phymode=%u tx_rate=%u "
           "step=%u cycle=%u planned=%lu proto=%u\n",
           (unsigned long)s_current_run_id, (unsigned long)s_record_count,
           (unsigned long)s_record_capacity, s_capacity_warned ? 1 : 0,
#if CONFIG_AUDIOLINK_TRANSPORT_DUAL
           "dual",
#elif CONFIG_AUDIOLINK_TRANSPORT_ESPNOW
           "espnow",
#else
           "softap_udp",
#endif
           CONFIG_AUDIOLINK_WIFI_CHANNEL, (unsigned)s_run_len, (unsigned long)s_run_hdr.interval_us,
           (unsigned)s_run_hdr.tx_power_qdbm, (unsigned)s_run_hdr.phymode, (unsigned)s_run_hdr.rate,
           (unsigned)s_run_hdr.step_idx, (unsigned)s_run_hdr.cycle, (unsigned long)s_planned,
           (unsigned)s_run_hdr.proto);
    printf("seq,tx_time_us,rx_time_us,rssi,phy_rate,mcs,flags\n");
    for (uint32_t i = 0; i < s_record_count; i++) {
        bench_record_t *r = &s_records[i];
        printf("%lu,%lld,%lld,%d,%u,%u,%u\n",
               (unsigned long)r->seq, (long long)r->tx_time_us, (long long)r->rx_time_us,
               (int)r->rssi, (unsigned)r->phy_rate, (unsigned)r->mcs, (unsigned)r->flags);
    }
    printf("===AUDIOLINK_BENCH_DUMP_END===\n");
}

// O dump corre numa task propria: bloquear a task wifi durante centenas de KB
// de printf estragaria a recepcao e o proprio timing que estamos a medir.
static void dump_task(void *arg) {
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(50));
        do_dump();
        ESP_LOGI(TAG, "run_id=%lu despejado (%lu registos) em %lld ms",
                 (unsigned long)s_current_run_id, (unsigned long)s_record_count,
                 (long long)((esp_timer_get_time() - t0) / 1000));
        s_dump_busy = false;
    }
}

static void request_finalize(void) {
    if (!s_run_active || s_run_finalized) {
        return;
    }
    s_run_finalized = true;
    s_dump_busy = true;
    xTaskNotifyGive(s_dump_task);
}

static void idle_watchdog_task(void *arg) {
    (void)arg;
    const int64_t idle_timeout_us = 3 * 1000 * 1000;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_run_active && !s_run_finalized) {
            int64_t now = esp_timer_get_time();
            if (now - s_last_activity_us > idle_timeout_us) {
                ESP_LOGW(TAG, "3s sem pacotes, run_id=%lu presumido terminado "
                              "(marcador de fim pode ter-se perdido)",
                         (unsigned long)s_current_run_id);
                request_finalize();
            }
        }
    }
}

void bench_on_packet_received(const uint8_t *data, size_t len, int8_t rssi, uint8_t phy_rate,
                              uint8_t mcs, uint8_t flags) {
    if (s_dump_busy) {
        return;  // a despejar o run anterior; a pausa entre passos do TX deve cobrir isto
    }
    if (len < BENCH_HDR_SIZE) {
        ESP_LOGW(TAG, "pacote curto demais (%u bytes), ignorado", (unsigned)len);
        return;
    }

    bench_pkt_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != BENCH_MAGIC_DATA) {
        ESP_LOGW(TAG, "magic invalido (0x%08lx), ignorado", (unsigned long)hdr.magic);
        return;
    }

    int64_t rx_time_us = esp_timer_get_time();
    s_last_activity_us = rx_time_us;

    if (!s_run_active || hdr.run_id != s_current_run_id) {
        ESP_LOGI(TAG, "novo run detectado: run_id=%lu step=%u cycle=%u (anterior=%lu, ativo=%d)",
                 (unsigned long)hdr.run_id, (unsigned)hdr.step_idx, (unsigned)hdr.cycle,
                 (unsigned long)s_current_run_id, s_run_active);
        s_current_run_id = hdr.run_id;
        s_record_count = 0;
        s_capacity_warned = false;
        s_run_active = true;
        s_run_finalized = false;
        s_run_hdr = hdr;
        s_run_len = len;
        s_planned = 0;
    }

    if (hdr.is_end) {
        s_planned = hdr.seq;  // no marcador de fim, seq = total de pacotes de dados enviados
        ESP_LOGI(TAG, "marcador de fim recebido para run_id=%lu", (unsigned long)hdr.run_id);
        request_finalize();
        return;
    }

    if (s_run_finalized) {
        return;  // dados atrasados a chegar depois do fim
    }

    if (s_record_count >= s_record_capacity) {
        if (!s_capacity_warned) {
            ESP_LOGW(TAG, "buffer de registos cheio (%lu), a descartar pacotes daqui "
                          "em diante - reduz a duracao do teste ou aumenta "
                          "AUDIOLINK_RX_RECORD_CAPACITY",
                     (unsigned long)s_record_capacity);
            s_capacity_warned = true;
        }
        return;
    }

    bench_record_t *r = &s_records[s_record_count++];
    r->seq = hdr.seq;
    r->tx_time_us = hdr.tx_time_us;
    r->rx_time_us = rx_time_us;
    r->rssi = rssi;
    r->phy_rate = phy_rate;
    r->mcs = mcs;
    r->flags = flags;
}

static void rx_role_main(void) {
#if CONFIG_AUDIOLINK_SCAN_ONLY
    ESP_LOGI(TAG, "modo SCAN_ONLY: scan do espectro a cada 10 s (sem SSIDs), sem ESP-NOW");
    bench_scan_loop(10);
#endif
    s_record_capacity = CONFIG_AUDIOLINK_RX_RECORD_CAPACITY;
    s_records = heap_caps_malloc(sizeof(bench_record_t) * s_record_capacity, MALLOC_CAP_SPIRAM);
    if (!s_records) {
        ESP_LOGE(TAG, "falha a alocar %lu registos em PSRAM",
                 (unsigned long)s_record_capacity);
        abort();
    }
    ESP_LOGI(TAG, "buffer de registos: %lu entradas (%lu bytes) em PSRAM",
             (unsigned long)s_record_capacity,
             (unsigned long)(sizeof(bench_record_t) * s_record_capacity));

    xTaskCreatePinnedToCore(dump_task, "dump", 4096, NULL, 4, &s_dump_task, 1);

#if CONFIG_AUDIOLINK_TRANSPORT_DUAL
    softap_udp_rx_init();  // inicia wifi (AP) + servidor UDP
    espnow_rx_attach();    // ESP-NOW por cima do mesmo wifi
#elif CONFIG_AUDIOLINK_TRANSPORT_ESPNOW
    espnow_rx_init();
#else
    softap_udp_rx_init();
#endif

    xTaskCreatePinnedToCore(idle_watchdog_task, "idle_wd", 3072, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "RX pronto, a espera do primeiro pacote...");
}

#endif // CONFIG_AUDIOLINK_ROLE_RX

// ================= TX: gerador de trafego =================

#if CONFIG_AUDIOLINK_ROLE_TX

#define TX_TASK_STACK 4096
#define TX_TASK_PRIORITY 20
#define TX_TASK_CORE 1
#define SWEEP_GAP_MS 6000  // pausa entre passos: cobre o dump do RX (medido no log dele)
#define MAX_PKT_LEN 1470

#if CONFIG_AUDIOLINK_TX_SWEEP
// Tabela de pontos de operacao. HT40 em toda a tabela (a largura de banda e
// fixada no arranque, igual no RX). Potencias: 84 = 21 dBm, 40 = 10 dBm, 8 = 2 dBm.
#define HT40 WIFI_PHY_MODE_HT40
#if CONFIG_AUDIOLINK_SWEEP_PROFILE == 0
// Perfil 0: exploracao inicial (runs 008/009).
static const bench_step_t s_sweep[] = {
    // phymode, rate,                  pot, payload, intervalo_us, dur_s
    {BENCH_PROTO_ESPNOW, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M, 84, 200, 5000, 10},        // robusto, 0.32 Mbps
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS0_LGI, 84, 200, 5000, 10},               // 0.32 Mbps
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS7_SGI, 84, 200, 5000, 10},               // falhou a curta distancia
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS7_SGI, 40, 200, 5000, 10},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS7_SGI, 8, 200, 5000, 10},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS4_LGI, 84, 1470, 2500, 10},              // 4.70 Mbps
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS7_SGI, 84, 1470, 1500, 10},              // 7.84 Mbps
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS7_SGI, 40, 1470, 1500, 10},
};
#elif CONFIG_AUDIOLINK_SWEEP_PROFILE == 2
// Perfil 2: alcance. Largura de banda de 20 MHz (PHYMODE_HT20 no Kconfig, RX e TX):
// testa a hipotese de que HT20/11g alcancam mais que HT40 (medido: a ~-85 dBm o
// 11g 6M teve 0% de perdas e o HT40 MCS0 93%). Tudo a 1.92 Mbps (480 B / 2 ms).
#define HT20 WIFI_PHY_MODE_HT20
static const bench_step_t s_sweep[] = {
    {BENCH_PROTO_ESPNOW, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M, 84, 200, 5000, 10},        // 0.32 Mbps, sanidade
    {BENCH_PROTO_ESPNOW, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M, 84, 480, 2000, 30},        // 1.92 Mbps
    {BENCH_PROTO_ESPNOW, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_12M, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT20, WIFI_PHY_RATE_MCS0_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT20, WIFI_PHY_RATE_MCS1_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT20, WIFI_PHY_RATE_MCS2_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT20, WIFI_PHY_RATE_MCS3_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT20, WIFI_PHY_RATE_MCS4_LGI, 84, 480, 2000, 30},
};
#elif CONFIG_AUDIOLINK_SWEEP_PROFILE == 3
// Perfil 3 (so DUAL): ESP-NOW vs UDP alternados no mesmo link, 1.92 Mbps (480 B / 2 ms), 30 s por passo.
// UDP usa a taxa escolhida pelo rate control (SoftAP+UDP "como seria em produto"); ESP-NOW usa taxas
// fixas. Cada transporte aparece varias vezes por ciclo, intercalado, para o ambiente (pessoas,
// interferencia) os afetar por igual. Largura de banda 20 MHz (AP e STA).
#define HT20 WIFI_PHY_MODE_HT20
#define UDPA BENCH_PHYMODE_UDP_AUTO
static const bench_step_t s_sweep[] = {
    {BENCH_PROTO_ESPNOW, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M, 84, 480, 2000, 30},
    {BENCH_PROTO_UDP, UDPA, UDPA, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT20, WIFI_PHY_RATE_MCS0_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_UDP, UDPA, UDPA, 84, 480, 2000, 30},
};
#else
// Perfil 1: trafego com a forma do produto (48 kHz/20 bit estereo = 1.92 Mbps
// = 480 B a cada 2 ms) para cada MCS, a potencia maxima, 30 s por passo.
// O passo 0 e uma sanidade robusta; o ultimo e a referencia de 4.7 Mbps.
static const bench_step_t s_sweep[] = {
    {BENCH_PROTO_ESPNOW, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M, 84, 200, 5000, 10},        // 0.32 Mbps, sanidade
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS0_LGI, 84, 480, 2000, 30},               // 1.92 Mbps
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS1_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS2_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS3_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS4_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS5_LGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS7_SGI, 84, 480, 2000, 30},
    {BENCH_PROTO_ESPNOW, HT40, WIFI_PHY_RATE_MCS4_LGI, 84, 1470, 2500, 10},              // 4.70 Mbps, referencia
};
#endif
#define SWEEP_LEN (sizeof(s_sweep) / sizeof(s_sweep[0]))
#endif

static void send_pkt(uint8_t proto, const uint8_t *pkt, size_t len) {
#if BENCH_HAS_ESPNOW
    if (proto == BENCH_PROTO_ESPNOW) {
        espnow_tx_send(pkt, len);
        return;
    }
#endif
#if BENCH_HAS_UDP
    if (proto == BENCH_PROTO_UDP) {
        softap_udp_tx_send(pkt, len);
    }
#endif
}

static void get_tx_stats(uint8_t proto, bench_tx_stats_t *out) {
    memset(out, 0, sizeof(*out));
#if BENCH_HAS_ESPNOW
    if (proto == BENCH_PROTO_ESPNOW) {
        espnow_tx_get_stats(out);
        return;
    }
#endif
#if BENCH_HAS_UDP
    if (proto == BENCH_PROTO_UDP) {
        softap_udp_tx_get_stats(out);
    }
#endif
}

// Dorme ate target_us: vTaskDelay para a maior parte e espera ativa fina para
// o resto (pacing sub-tick). O IDLE1 nao corre durante a espera ativa - o
// watchdog do IDLE1 esta desligado no sdkconfig.defaults por essa razao.
static void sleep_until(int64_t target_us) {
    int64_t remaining = target_us - esp_timer_get_time();
    if (remaining <= 0) {
        return;
    }
    if (remaining > 2000) {
        vTaskDelay(pdMS_TO_TICKS((remaining - 1000) / 1000));
    }
    while (esp_timer_get_time() < target_us) {
    }
}

static void run_step(const bench_step_t *st, uint8_t step_idx, uint8_t cycle, uint8_t *pkt) {
#if BENCH_HAS_ESPNOW && CONFIG_AUDIOLINK_TX_SWEEP
    // so passos ESP-NOW aplicam PHY/potencia; os passos UDP usam a taxa escolhida pelo driver
    esp_err_t e = (st->proto == BENCH_PROTO_ESPNOW) ? espnow_tx_apply_step(st->phymode, st->rate, st->power_qdbm) : ESP_OK;
    if (e != ESP_OK) {
        // nunca correr um passo rotulado com uma config que nao foi aplicada
        // em sweep salta-se o passo (aparece como ausente no RX) em vez de abortar o
        // ciclo inteiro; continua a nao existir nenhum run com config mal rotulada.
        ESP_LOGE(TAG, "passo %u: falha a aplicar phymode=%u rate=%u pot=%u (%s) - PASSO SALTADO",
                 step_idx, st->phymode, st->rate, st->power_qdbm, esp_err_to_name(e));
        return;
    }
#endif

    const size_t pkt_len = st->payload;
    const uint32_t total_pkts = (uint32_t)(((uint64_t)st->duration_s * 1000000ULL) / st->interval_us);
    const double target_mbps = (8.0 * pkt_len * 1e6 / st->interval_us) / 1e6;

    uint32_t run_id = esp_random();
    if (run_id == 0) {
        run_id = 1;
    }
    memset(pkt, 0, pkt_len);

    ESP_LOGI(TAG, "=== TX a arrancar: run_id=%lu pkts=%lu payload=%uB intervalo=%luus duracao=%us "
                  "alvo=%.3f Mbps proto=%u phymode=%u rate=%u pot=%u step=%u cycle=%u ===",
             (unsigned long)run_id, (unsigned long)total_pkts, (unsigned)pkt_len,
             (unsigned long)st->interval_us, (unsigned)st->duration_s, target_mbps,
             st->proto, st->phymode, st->rate, st->power_qdbm, step_idx, cycle);

    bench_tx_stats_t before;
    get_tx_stats(st->proto, &before);

    bench_pkt_hdr_t hdr = {0};
    hdr.magic = BENCH_MAGIC_DATA;
    hdr.run_id = run_id;
    hdr.tx_power_qdbm = st->power_qdbm;
    hdr.phymode = st->phymode;
    hdr.rate = st->rate;
    hdr.interval_us = st->interval_us;
    hdr.step_idx = step_idx;
    hdr.cycle = cycle;
    hdr.proto = st->proto;

    int64_t next_send_us = esp_timer_get_time();
    for (uint32_t seq = 0; seq < total_pkts; seq++) {
        hdr.seq = seq;
        hdr.is_end = 0;
        hdr.tx_time_us = esp_timer_get_time();
        memcpy(pkt, &hdr, sizeof(hdr));
        send_pkt(st->proto, pkt, pkt_len);

        next_send_us += st->interval_us;
        sleep_until(next_send_us);
    }

    hdr.is_end = 1;
    hdr.seq = total_pkts;  // o RX usa isto como total planeado (ve perdas no fim do run)
    for (int i = 0; i < 10; i++) {
        hdr.tx_time_us = esp_timer_get_time();
        memcpy(pkt, &hdr, sizeof(hdr));
        send_pkt(st->proto, pkt, pkt_len);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // deixa chegar os ultimos callbacks de envio

    bench_tx_stats_t after;
    get_tx_stats(st->proto, &after);
    ESP_LOGI(TAG, "=== TX TERMINADO run_id=%lu sent_ok=%lu send_fail=%lu api_err=%lu last_api_err=%ld ===",
             (unsigned long)run_id, (unsigned long)(after.sent_ok - before.sent_ok),
             (unsigned long)(after.send_fail - before.send_fail),
             (unsigned long)(after.api_err - before.api_err), (long)after.last_api_err);
}

static void tx_task(void *arg) {
    (void)arg;

#if CONFIG_AUDIOLINK_TRANSPORT_DUAL
    softap_udp_tx_init();  // associa ao AP do RX (bloqueia ate ter IP) e abre o socket UDP
    {
        uint8_t bssid[6];
        softap_udp_tx_get_bssid(bssid);
        espnow_tx_attach(bssid);  // ESP-NOW unicast para a interface AP do RX
    }
#elif CONFIG_AUDIOLINK_TRANSPORT_ESPNOW
    espnow_tx_init();
#else
    softap_udp_tx_init();
#endif

    vTaskDelay(pdMS_TO_TICKS(500));  // margem para o RX estar pronto a ouvir

    uint8_t *pkt = calloc(1, MAX_PKT_LEN);
    if (!pkt) {
        ESP_LOGE(TAG, "sem memoria para pacote");
        abort();
    }

#if CONFIG_AUDIOLINK_TX_SWEEP
    ESP_LOGI(TAG, "modo SWEEP: %u passos, ciclo infinito, %d ms entre passos",
             (unsigned)SWEEP_LEN, SWEEP_GAP_MS);
    for (uint8_t cycle = 0;; cycle++) {
        for (uint8_t i = 0; i < SWEEP_LEN; i++) {
            run_step(&s_sweep[i], i, cycle, pkt);
            vTaskDelay(pdMS_TO_TICKS(SWEEP_GAP_MS));
        }
    }
#else
    bench_step_t st = {
#if CONFIG_AUDIOLINK_TRANSPORT_ESPNOW
        .proto = BENCH_PROTO_ESPNOW,
        .phymode = BENCH_PHYMODE,
        .rate = BENCH_PHYRATE,
#else
        .proto = BENCH_PROTO_UDP,
        .phymode = BENCH_PHYMODE_UDP_AUTO,
        .rate = BENCH_PHYMODE_UDP_AUTO,
#endif
        .power_qdbm = CONFIG_AUDIOLINK_TX_POWER_QDBM,
        .payload = CONFIG_AUDIOLINK_PAYLOAD_SIZE,
        .interval_us = CONFIG_AUDIOLINK_TX_INTERVAL_US,
        .duration_s = CONFIG_AUDIOLINK_TEST_DURATION_S,
    };
    run_step(&st, 0, 0, pkt);
    free(pkt);
    vTaskDelete(NULL);
#endif
}

static void tx_role_main(void) {
    xTaskCreatePinnedToCore(tx_task, "bench_tx", TX_TASK_STACK, NULL,
                             TX_TASK_PRIORITY, NULL, TX_TASK_CORE);
}

#endif // CONFIG_AUDIOLINK_ROLE_TX

// ================= app_main =================

void app_main(void) {
    bench_nvs_init();

    ESP_LOGI(TAG, "AUDIOLINK bench - Fase 1 (transporte=%s, papel=%s)",
#if CONFIG_AUDIOLINK_TRANSPORT_ESPNOW
             "ESP-NOW",
#else
             "SoftAP+UDP",
#endif
#if CONFIG_AUDIOLINK_ROLE_TX
             "TX"
#else
             "RX"
#endif
    );

#if CONFIG_AUDIOLINK_ROLE_TX
    tx_role_main();
#else
    rx_role_main();
#endif
}
