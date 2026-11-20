// Passo 3 (TX): PCM1808 -> I2S -> ESP-NOW, em bruto, sem FEC/PLC/buffer.
//
// Mesma configuracao I2S validada no passo 1 (adc_test.c): S3 mestre, PCM1808
// slave, GPIO4/5/6/7 = SCK/BCK/LRC/OUT, mesmo descarte inicial de ~500 ms
// (o PCM1808 so entrega dados validos ~190 ms + fade-in apos o arranque do
// relogio). Cada leitura I2S de LINK_FRAMES_PER_PKT frames vira um pacote
// ESP-NOW, sem conversao: o payload e o proprio buffer que sai do driver.
//
// PATCH-OBS-TX-01 (2026-10-01): mesmo principio do PATCH-OBS-01 do RX - nenhuma operacao de
// logging/consola corre no loop real-time do TX (leitura I2S + packetizacao + envio ESP-NOW, tudo
// na mesma tarefa - ver inventario abaixo). Evidencia que motivou isto (PATCH-OBS-01, corrida de
// 2 h): fechar a porta serie do TX fazia o TX perder ~12 blocos de ADC (~32 ms, adc_ovf+12) sem
// avancar sample_ctr - o stream atrasava e o RX entrava em rebuffer_low. Confirmado 3/3. Mecanismo:
// a consola USB-Serial-JTAG sem driver faz busy-wait ate 50 ms quando o FIFO nao esvazia (mesmo
// usb_serial_jtag_vfs.c do RX) - aqui dentro da linha ESP_LOGI periodica do proprio loop do TX.
//
// Inventario do caminho real-time (antes deste patch):
//   - UMA SO tarefa faz tudo: produtor ADC (i2s_channel_read, bloqueante) e packetizacao/envio
//     (link_espnow_tx_send) estao na MESMA tarefa (link_tx_run, chamada de app_main - tarefa
//     "main", prioridade CONFIG_ESP_MAIN_TASK_PRIO=1, pinned CPU0, stack 3584 - identico ao RX).
//     Ao contrario do RX, nao ha aqui separacao entre tarefa de receive e tarefa de playout.
//   - esp_now_send() e sincrono a devolver (enfileira no driver Wi-Fi); so o callback de conclusao
//     (on_send_cb, link_espnow.c) corre noutro contexto (tarefa interna Wi-Fi/ESP-NOW, pinned
//     core 0 - mesmo core do loop TX e por omissao do stack Wi-Fi).
//   - Blocos potencialmente bloqueantes encontrados DENTRO do loop do TX:
//       1. ESP_LOGW em cada erro de i2s_channel_read() (raro, mas sem guard) - substituido por
//          contador (s_i2s_read_err/_last), log diferido so quando sobe.
//       2. bloco 1x/s: link_espnow_tx_get_stats() (ok, so escalares O(1)) + a antiga ordenacao das
//          amostras de latencia (qsort ate 1024 elementos, agora saiu daqui - ver link_espnow.c) +
//          esp_wifi_get_channel() (chamada ao driver Wi-Fi, nao e leitura de registo) + a propria
//          linha ESP_LOGI (o bloqueio real, USB-Serial-JTAG) + esp_get_free/minimum_heap_size().
//   - Posse dos contadores usados no log (quem escreve, sem lock hoje): s_api_accept/err_*/
//     backpressure_drop/last_err escritos SO pela tarefa TX (em link_espnow_tx_send, chamada
//     sincrona desta mesma tarefa); s_send_ok/send_fail e s_lat_samples/s_lat_n escritos por
//     on_send_cb (tarefa Wi-Fi/ESP-NOW) - mesmo padrao de contadores volatile sem lock ja usado
//     no RX para s_pkts_recv etc (incrementos de palavra unica, sem risco de corrupcao); pending
//     vem de uxQueueMessagesWaiting (API da fila, thread-safe). s_i2s_overrun e escrito pela ISR
//     on_recv_q_ovf, igual a s_dac_underrun do RX.
//
// Arquitetura (identica ao RX): o loop so preenche um snapshot fixo (tx_diag_t) 1x/s e publica-o
// com uma copia curta sob spinlock; tx_diag_task (prioridade baixa, outro core) formata e imprime,
// incluindo agora o qsort das latencias (que tambem saiu do loop). Nao mexe em ADC/I2S, packetizacao
// de 128 frames, ESP-NOW, MAX_PENDING/backpressure, PHY, clocks, ou em sample_ctr (semantica
// inalterada - nao decide aqui se deve avancar durante overflow do ADC, questao separada).
//
// PATCH-TX-SESSION (2026-10-01, protocolo v2): session_id de 64 bit, gerado 1x aqui (esp_fill_random,
// DEPOIS de link_espnow_tx_init() deixar o Wi-Fi ativo - a fonte de entropia de hardware do S3 e mais
// forte com o radio ligado), NUNCA persistido em NVS (nem precisa de o ser - cada boot e uma timeline
// nova de proposito). Ver comentario em link_proto.h para o porque (o RX nao tinha como saber que o
// TX tinha reiniciado) e em link_rx.c para o lado que consome isto.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_random.h"
#include "audio_common.h"
#include "link_proto.h"
#include "link_espnow.h"

static const char *TAG = "link_tx";

#define DISCARD_MS 500

static volatile uint32_t s_i2s_overrun = 0;     // ISR on_recv_q_ovf, leitura direta (ver RX s_dac_underrun)
static uint32_t s_i2s_read_err = 0;             // so a tarefa TX escreve e le (mesmo loop)
static esp_err_t s_i2s_read_err_last = ESP_OK;

static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_i2s_overrun++;
    return false;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

// --- PATCH-OBS-TX-01: snapshot de diagnostico, produtor unico = loop do TX ---
// Mesmo padrao do rx_diag_t em link_rx.c: o loop preenche s_diag_work (so dele) 1x/s e copia-o
// para s_diag_pub dentro de uma critical section curta; tx_diag_task copia s_diag_pub para
// s_diag_log dentro da mesma critical section e so depois ordena/formata/imprime, ja sem lock.
typedef struct {
    uint32_t seq;
    int64_t now_us;
    int64_t t0_us;
    uint32_t adc_blocks;  // = seq do loop TX (nome do campo no log historico: adc_blocks)
    uint64_t sample_ctr;
    uint32_t sent_since;  // pacotes enviados nesta janela de 1 s
    // link_espnow: escalares O(1), lidos pela propria tarefa TX via link_espnow_tx_get_stats()
    uint32_t api_accept, send_ok, send_fail, api_err_no_mem, api_err_other, pending, backpressure_drop;
    esp_err_t last_err;
    // latencia send()->callback: amostras RAW desta janela (so copia+reset na tarefa TX; ordenar e
    // calcular media/percentis fica com tx_diag_task - ver link_espnow_tx_get_lat_samples)
    uint32_t lat_n;
    uint32_t lat_samples[LINK_TX_LAT_SAMPLE_CAP];
    uint32_t i2s_overrun;
    uint32_t i2s_read_err;
    esp_err_t i2s_read_err_last;
    uint32_t pub_cs_max_ever_us;  // pior duracao da publicacao (inclui espera pelo spinlock)
#if CONFIG_AUDIOLINK_WIFI_REINIT
    uint32_t reinit_abandoned, link_down_drop;
#endif
} tx_diag_t;

#if CONFIG_AUDIOLINK_WIFI_REINIT
#define LINKTX_RI_FMT " reinit_abandoned=%lu link_down_drop=%lu"
#define LINKTX_RI_ARGS , (unsigned long)d->reinit_abandoned, (unsigned long)d->link_down_drop
static const char *const s_ri_outcome[] = {"OK", "OK_RETRY", "FAILED"};
static const char *const s_ri_op[] = {"REINIT", "PAUSE", "ESPNOW"};  // indice = LINK_OP_*
#if CONFIG_AUDIOLINK_WIFI_REINIT_QUAL
// escritos so pela tarefa TX (uma vez), lidos pela tx_diag_task
static volatile int64_t s_qual_armed_us = 0;
static volatile uint32_t s_qual_healthy_s = 0;
#endif

#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE
// DEEP-REINIT-PROBE (gate v3, G2-agg20): uma decisao por boot no tick de t0+60 s, sobre os ultimos 20 ticks
// de 1 s (o periodo de 20 s do healthy() do orquestrador). REINIT so se as DUAS familias falham:
//   T (throughput/backpressure): media d(api_accept) < 370  OU  media d(backpressure_drop) > 1
//   L (latencia/fila):           media cb_lat_avg > 3000 us OU  media pending > 2
// (limiares do healthy(); comparados em somas inteiras: soma < 370*20, > 1*20, > 3000*20, > 2*20).
// frac15 (aceites/oportunidades ate t0+15 s; "strong" = < 0,568 do severity.py) so descritivo, nao decide.
// O FLASH_BOOT nao chega aqui: o host faz reset_single logo depois de validar o banner/ELF.
#define PROBE_FRAC15_US 15000000
#define PROBE_DECIDE_US 60000000
#define PROBE_STRONG_MILLI 568
#define PROBE_WIN 20
typedef struct {
    uint32_t lat_avg, pending, dacc, dbp;
    bool ok;
} probe_tick_t;
typedef struct {
    int64_t t_us, t15_us;
    uint32_t opp15, acc15, frac15_milli;
    bool strong;  // so descritivo
    uint32_t win_n, sum_dacc, sum_dbp, sum_lat, sum_pend;
    bool fam_t, fam_l, reinit;
} probe_pre_t;
typedef struct {
    int64_t t_us;
    uint32_t win_s, opp, acc, frac_milli, ok_ticks, ticks;
} probe_post_t;
static portMUX_TYPE s_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static probe_pre_t s_probe_pre;      // sob s_probe_lock
static probe_post_t s_probe_post[2]; // sob s_probe_lock
static uint32_t s_probe_pub = 0;     // sob s_probe_lock: bit0 PRE, bit1 POST30, bit2 POST60, bit3 TRIGGER, bit4 TIMEOUT
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
// ESPNOW-RECOVERY-PROBE: se o G2-agg20 falhar em t0+60 s, nao intervir ai; armar e, a partir do tick seguinte,
// intervir no PRIMEIRO tick em que >= 4 dos ultimos 10 ticks passam a regra de tick do healthy(); sem isso ate
// 600 s de uptime -> TIMEOUT, sem intervencao.
#define PROBE_REC_WIN 10
#define PROBE_REC_MIN_OK 4
#define PROBE_REC_TIMEOUT_US 600000000LL
typedef struct {
    int64_t t_us;
    uint32_t healthy10, ticks_since_arm;
} probe_trig_t;
static probe_trig_t s_probe_trig;    // sob s_probe_lock
#endif

static void probe_log(void) {
    static uint32_t logged = 0;
    static probe_pre_t p;
    static probe_post_t q[2];
    portENTER_CRITICAL(&s_probe_lock);
    const uint32_t pub = s_probe_pub;
    p = s_probe_pre;
    q[0] = s_probe_post[0];
    q[1] = s_probe_post[1];
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
    const probe_trig_t tr = s_probe_trig;  // escrito antes de publicar o bit (mesma seccao critica)
#endif
    portEXIT_CRITICAL(&s_probe_lock);
    if ((pub & 1) && !(logged & 1)) {
        ESP_LOGI(TAG, "WIFIREINIT PROBE PRE t_us=%lld t15_us=%lld opp15=%lu acc15=%lu frac15_milli=%lu strong=%d "
                      "win_n=%lu sum_dacc=%lu sum_dbp=%lu sum_lat=%lu sum_pend=%lu fam_t=%d fam_l=%d decision=%s",
                 (long long)p.t_us, (long long)p.t15_us, (unsigned long)p.opp15, (unsigned long)p.acc15,
                 (unsigned long)p.frac15_milli, (int)p.strong, (unsigned long)p.win_n, (unsigned long)p.sum_dacc,
                 (unsigned long)p.sum_dbp, (unsigned long)p.sum_lat, (unsigned long)p.sum_pend, (int)p.fam_t,
                 (int)p.fam_l, p.reinit ? "REINIT" : "SKIP");
        logged |= 1;
    }
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
    for (int i = 3; i <= 4; i++) {
        if ((pub & (1u << i)) && !(logged & (1u << i))) {
            ESP_LOGI(TAG, "WIFIREINIT PROBE %s t_us=%lld healthy10=%lu min_ok=%d ticks_since_arm=%lu",
                     i == 3 ? "TRIGGER" : "TIMEOUT", (long long)tr.t_us, (unsigned long)tr.healthy10, PROBE_REC_MIN_OK,
                     (unsigned long)tr.ticks_since_arm);
            logged |= (1u << i);
        }
    }
#endif
    for (int i = 0; i < 2; i++) {
        if ((pub & (2u << i)) && !(logged & (2u << i))) {
            ESP_LOGI(TAG, "WIFIREINIT PROBE POST t_us=%lld win_s=%lu opp=%lu acc=%lu frac_milli=%lu ok_ticks=%lu ticks=%lu",
                     (long long)q[i].t_us, (unsigned long)q[i].win_s, (unsigned long)q[i].opp,
                     (unsigned long)q[i].acc, (unsigned long)q[i].frac_milli, (unsigned long)q[i].ok_ticks,
                     (unsigned long)q[i].ticks);
            logged |= (2u << i);
        }
    }
}
#endif

// WIFI-REINIT-01: marcadores de inicio/fim, impressos so por tx_diag_task (a tarefa TX nunca espera pela
// consola). Os instantes sao os da tarefa TX (esp_timer), nao os da impressao.
static void reinit_log(void) {
    static uint32_t begin_logged = 0, end_logged = 0;
    static link_reinit_result_t rr;
    static char errs[LINK_REINIT_STEPS * 12], us[LINK_REINIT_STEPS * 11];
#if CONFIG_AUDIOLINK_WIFI_REINIT_QUAL
    static bool armed_logged = false;
    if (!armed_logged && s_qual_armed_us != 0) {
        ESP_LOGI(TAG, "WIFIREINIT QUAL armado t_us=%lld healthy_s=%lu", (long long)s_qual_armed_us,
                 (unsigned long)s_qual_healthy_s);
        armed_logged = true;
    }
#endif
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE
    probe_log();  // antes do BEGIN: o PRE e publicado antes de o reinit comecar
#endif
    int64_t tb = 0;
    uint8_t op = 0;
    const uint32_t nb = link_espnow_tx_reinit_begun(&tb, &op);
    if (nb != begin_logged) {
        ESP_LOGI(TAG, "WIFIREINIT BEGIN n=%lu op=%s t_begin_us=%lld missed_begin=%lu", (unsigned long)nb,
                 s_ri_op[op], (long long)tb, (unsigned long)(nb - begin_logged - 1));
        begin_logged = nb;
    }
    const uint32_t ne = link_espnow_tx_reinit_last(&rr);
    if (ne == end_logged) {
        return;
    }
    size_t oe = 0, ou = 0;
    for (int i = 0; i < LINK_REINIT_STEPS; i++) {
        oe += snprintf(errs + oe, sizeof(errs) - oe, "%s%x", i ? "," : "", (unsigned)rr.err[i]);
        ou += snprintf(us + ou, sizeof(us) - ou, "%s%lu", i ? "," : "", (unsigned long)rr.step_us[i]);
    }
    int first_bad = -1;
    for (int i = 0; i < LINK_REINIT_STEPS && first_bad < 0; i++) {
        if (rr.err[i] != ESP_OK) {
            first_bad = i;
        }
    }
    ESP_LOGI(TAG, "WIFIREINIT END n=%lu op=%s hold_us=%lu outcome=%s attempts=%u t_begin_us=%lld t_end_us=%lld dur_us=%lld "
                  "pending_before=%lu drain_ok=%d drain_wait_us=%lu pending_after_drain=%lu abandoned=%lu ch=%u->%u bw=%d->%d ps=%d->%d peer_ok=%d first_err_step=%s "
                  "heap=%lu->%lu heap_int=%lu->%lu tx_stack_free=%lu missed_end=%lu err=%s step_us=%s",
             (unsigned long)rr.n, s_ri_op[rr.op], (unsigned long)rr.hold_us,
             s_ri_outcome[rr.outcome], (unsigned)rr.attempts, (long long)rr.t_begin_us,
             (long long)rr.t_end_us, (long long)(rr.t_end_us - rr.t_begin_us), (unsigned long)rr.pending_before,
             (int)rr.drain_ok, (unsigned long)rr.drain_wait_us, (unsigned long)rr.pending_after_drain,
             (unsigned long)rr.abandoned, (unsigned)rr.ch_before, (unsigned)rr.ch_after, rr.bw_before, rr.bw_after,
             rr.ps_before, rr.ps_after, (int)rr.peer_ok, first_bad < 0 ? "-" : link_espnow_reinit_step_name(first_bad),
             (unsigned long)rr.heap_before, (unsigned long)rr.heap_after, (unsigned long)rr.heap_int_before,
             (unsigned long)rr.heap_int_after, (unsigned long)rr.tx_stack_free,
             (unsigned long)(ne - end_logged - 1), errs, us);
    end_logged = ne;
}
#else
#define LINKTX_RI_FMT ""
#define LINKTX_RI_ARGS
#endif

#if CONFIG_AUDIOLINK_STARTUP_SUPERVISOR
// STARTUP-SUPERVISOR: a politica do BOOT-QUALIFIER-02 (host) no firmware. Constantes = as do BQ-02 (sem limiares novos).
#define SUP_FRAC15_US 15000000
#define SUP_STRONG_MILLI 568
#define SUP_READY_TICKS 20
#define SUP_TIMEOUT_US 120000000LL
#define SUP_MAX_ATTEMPTS 20
#define SUP_MAGIC 0x53555056u
typedef struct {
    uint32_t magic, magic_inv;
    uint32_t failed;  // tentativas falhadas nesta sequencia (STRONG ou TIMEOUT)
} sup_rtc_t;
static RTC_NOINIT_ATTR sup_rtc_t s_sup;
#endif

static tx_diag_t s_diag_work;  // so a tarefa TX
static tx_diag_t s_diag_pub;   // partilhado, so sob s_diag_lock
static tx_diag_t s_diag_log;   // so tx_diag_task
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_diag_seq = 0;
static uint32_t s_pub_cs_max_ever_us = 0;

#define TX_DIAG_STACK 6144
#define TX_DIAG_PRIO tskIDLE_PRIORITY  // abaixo da tarefa TX (main task, prioridade 1)
#define TX_DIAG_CORE 1                 // tarefa TX e tarefa Wi-Fi estao no core 0
#define TX_DIAG_POLL_MS 100

// PATCH-OBS-TX-01: unico sitio que formata/imprime o diagnostico periodico do TX. Le so o
// snapshot publicado pela tarefa TX (+ canal Wi-Fi e heap, consultados aqui). Pode bloquear na
// consola o tempo que for preciso; a tarefa TX nunca espera por ele.
static void tx_diag_task(void *arg) {
    (void)arg;
    tx_diag_t *d = &s_diag_log;
    uint32_t last_seq = 0;
    uint32_t log_missed = 0;
    uint32_t last_i2s_err_logged = 0;
    uint32_t prev_print_us = 0;
    uint32_t print_max_ever_us = 0;
    static uint32_t lat_sorted[LINK_TX_LAT_SAMPLE_CAP];
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    bool trace_finished = false;
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TX_DIAG_POLL_MS));
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
        // TRACE-TX-BOOT-01: despejo muito depois da janela (longe da fase de recuperacao); so esta
        // tarefa imprime, a tarefa TX nunca espera por isto.
        if (!trace_finished &&
            esp_timer_get_time() >= (int64_t)CONFIG_AUDIOLINK_TRACE_TX_BOOT_DUMP_AT_S * 1000000) {
            trace_finished = link_espnow_trace_dump();
        }
#endif
#if CONFIG_AUDIOLINK_WIFI_REINIT
        reinit_log();
#endif
        bool fresh = false;
        portENTER_CRITICAL(&s_diag_lock);
        if (s_diag_pub.seq != last_seq) {
            *d = s_diag_pub;
            fresh = true;
        }
        portEXIT_CRITICAL(&s_diag_lock);
        if (!fresh) {
            continue;
        }
        const int64_t t_copy = esp_timer_get_time();
        if (last_seq != 0 && d->seq - last_seq > 1) {
            log_missed += d->seq - last_seq - 1;
        }
        last_seq = d->seq;

        // evento diferido: erro de i2s_channel_read() (antes era ESP_LOGW inline no loop do TX)
        if (d->i2s_read_err != last_i2s_err_logged) {
            ESP_LOGW(TAG, "I2S_READ_ERR err=%s count=%lu", esp_err_to_name(d->i2s_read_err_last),
                     (unsigned long)d->i2s_read_err);
            last_i2s_err_logged = d->i2s_read_err;
        }

        uint32_t n = d->lat_n;
        uint32_t lat_avg = 0, lat_p95 = 0, lat_max = 0;
        if (n > 0) {
            memcpy(lat_sorted, d->lat_samples, (size_t)n * sizeof(uint32_t));
            qsort(lat_sorted, n, sizeof(uint32_t), cmp_u32);
            uint64_t sum = 0;
            for (uint32_t i = 0; i < n; i++) {
                sum += lat_sorted[i];
            }
            lat_avg = (uint32_t)(sum / n);
            lat_p95 = lat_sorted[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
            lat_max = lat_sorted[n - 1];
        }

        uint8_t chan = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&chan, &sec);

        // pipeline: adc_blocks(=seq) = backpressure_drop + api_err_no_mem + api_err_other + api_accept;
        // api_accept = send_ok + send_fail + pending (neste instante)
        ESP_LOGI(TAG, "LINKTX t=%d adc_blocks=%lu sample_ctr=%llu sent=%lu backpressure_drop=%lu "
                      "api_err_no_mem=%lu api_err_other=%lu last_err=%s api_accept=%lu send_ok=%lu "
                      "send_fail=%lu pending=%lu cb_lat_avg_us=%lu cb_lat_p95_us=%lu cb_lat_max_us=%lu "
                      "adc_ovf=%lu i2s_read_err=%lu chan=%u heap_free=%lu heap_min=%lu uptime_s=%lld "
                      "pub_seq=%lu pub_cs_max_ever_us=%lu log_missed=%lu log_lag_us=%lld "
                      "log_prev_print_us=%lu log_print_max_ever_us=%lu log_stack_free=%lu" LINKTX_RI_FMT,
                 (int)((d->now_us - d->t0_us) / 1000000), (unsigned long)d->adc_blocks,
                 (unsigned long long)d->sample_ctr, (unsigned long)d->sent_since,
                 (unsigned long)d->backpressure_drop, (unsigned long)d->api_err_no_mem,
                 (unsigned long)d->api_err_other, d->last_err ? esp_err_to_name(d->last_err) : "-",
                 (unsigned long)d->api_accept, (unsigned long)d->send_ok, (unsigned long)d->send_fail,
                 (unsigned long)d->pending, (unsigned long)lat_avg, (unsigned long)lat_p95,
                 (unsigned long)lat_max, (unsigned long)d->i2s_overrun, (unsigned long)d->i2s_read_err,
                 (unsigned)chan, (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size(), (long long)(d->now_us / 1000000),
                 (unsigned long)d->seq, (unsigned long)d->pub_cs_max_ever_us, (unsigned long)log_missed,
                 (long long)(t_copy - d->now_us), (unsigned long)prev_print_us,
                 (unsigned long)print_max_ever_us, (unsigned long)uxTaskGetStackHighWaterMark(NULL) LINKTX_RI_ARGS);

        prev_print_us = (uint32_t)(esp_timer_get_time() - t_copy);
        if (prev_print_us > print_max_ever_us) {
            print_max_ever_us = prev_print_us;
        }
    }
}

void link_tx_run(void) {
    audio_clk_t clk = audio_clk_get();
    link_espnow_tx_init();

    // PATCH-TX-SESSION: 1x por boot, depois do Wi-Fi estar ativo. esp_fill_random() usa o RNG de
    // hardware do S3 (ruido do ADC + entropia de RF quando o Wi-Fi esta ligado, como aqui) - sem
    // seed, sem NVS, disponivel de imediato. NAO persistir: cada boot e deliberadamente uma
    // timeline nova para o RX (ver link_proto.h e link_rx.c).
    uint64_t session_id = 0;
    esp_fill_random(&session_id, sizeof(session_id));
    ESP_LOGI(TAG, "sessao: session_id=%016llX", (unsigned long long)session_id);

    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;
    cc.dma_frame_num = LINK_FRAMES_PER_PKT;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &rx));

    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = clk.sample_rate_hz, .clk_src = clk.clk_src,
                    .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = CONFIG_AUDIOLINK_PIN_MCLK, .bclk = CONFIG_AUDIOLINK_PIN_BCLK,
                     .ws = CONFIG_AUDIOLINK_PIN_WS, .dout = I2S_GPIO_UNUSED,
                     .din = CONFIG_AUDIOLINK_PIN_DATA},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std));

    i2s_event_callbacks_t cbs = {.on_recv_q_ovf = on_recv_q_ovf};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx, &cbs, NULL));

    uint8_t *pkt = heap_caps_malloc(LINK_PKT_MAX_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!pkt) {
        abort();
    }
    link_pkt_hdr_t *hdr = (link_pkt_hdr_t *)pkt;
    int32_t *payload = (int32_t *)(pkt + sizeof(link_pkt_hdr_t));
    const size_t payload_bytes = (size_t)LINK_FRAMES_PER_PKT * LINK_BYTES_PER_FRAME;

    ESP_LOGI(TAG, "pacote: %u frames/pacote, %u bytes cabecalho + %u bytes audio = %u bytes; ~%.2f pacotes/s a fs=%lu",
             (unsigned)LINK_FRAMES_PER_PKT, (unsigned)sizeof(link_pkt_hdr_t), (unsigned)payload_bytes,
             (unsigned)(sizeof(link_pkt_hdr_t) + payload_bytes),
             (double)clk.sample_rate_hz / LINK_FRAMES_PER_PKT, (unsigned long)clk.sample_rate_hz);

    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    int64_t t_enable = esp_timer_get_time();
    int64_t discard_until = t_enable + DISCARD_MS * 1000;

    // Descarta leituras ate o PCM1808 assentar; usa o proprio buffer do pacote como scratch.
    while (esp_timer_get_time() < discard_until) {
        size_t bytes = 0;
        i2s_channel_read(rx, payload, payload_bytes, &bytes, 1000);
    }
    ESP_LOGI(TAG, "arrancou (descartados os 1os %d ms apos o relogio do PCM1808 ligar)", DISCARD_MS);

    // PATCH-OBS-TX-01: logger de diagnostico fora do caminho real-time. Se nao for possivel cria-lo,
    // o audio segue sem diagnostico periodico (perder logs e aceitavel, perder audio nao).
    if (xTaskCreatePinnedToCore(tx_diag_task, "tx_diag", TX_DIAG_STACK, NULL, TX_DIAG_PRIO, NULL,
                                TX_DIAG_CORE) == pdPASS) {
        ESP_LOGI(TAG, "tx_diag: tarefa de diagnostico criada (snapshot=%u B, prio=%d, core=%d, stack=%d)",
                 (unsigned)sizeof(tx_diag_t), (int)TX_DIAG_PRIO, TX_DIAG_CORE, TX_DIAG_STACK);
    } else {
        ESP_LOGE(TAG, "tx_diag: falha a criar a tarefa de diagnostico - audio continua sem logs periodicos");
    }

#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    // TRACE-TX-BOOT-01: depois de todas as reservas do init (Wi-Fi, I2S, pacote, tx_diag), antes do
    // 1o envio - mede a RAM interna real e reserva os buffers (ou desativa, sem fallback).
    link_espnow_trace_init();
#endif

    uint32_t seq = 0;
    uint64_t sample_ctr = 0;
    int64_t t0 = esp_timer_get_time();
    int64_t next_stat = t0 + 1000000;
    uint32_t sent_since = 0;
#if CONFIG_AUDIOLINK_STARTUP_SUPERVISOR
    // contador valido so atraves de esp_restart(); qualquer outro reset comeca uma sequencia nova
    if (esp_reset_reason() != ESP_RST_SW || s_sup.magic != SUP_MAGIC || s_sup.magic_inv != ~SUP_MAGIC ||
        s_sup.failed >= SUP_MAX_ATTEMPTS) {
        s_sup.magic = SUP_MAGIC;
        s_sup.magic_inv = ~SUP_MAGIC;
        s_sup.failed = 0;
    }
    bool sup_active = true, sup_have15 = false;
    // RC2: sem nenhum send_ok aos 15 s o peer nao existe -> WAIT_PEER (nao conta tentativa, sem reinicio nem timeout);
    // no 1o send_ok -> PEER_SEEN e nova janela de qualificacao neste boot (READY ou 120 s desde o PEER_SEEN)
    enum { SUP_QUAL, SUP_WAIT_PEER, SUP_PEER_QUAL } sup_mode = SUP_QUAL;
    int64_t sup_peer_t = 0;
    uint32_t sup_ok_run = 0, sup_pend_sum = 0, sup_prev_acc = 0, sup_prev_bp = 0;
    uint32_t sup_pend_win[SUP_READY_TICKS] = {0};
    int64_t sup_prev_now = 0;
    ESP_LOGW(TAG, "STARTUP SUPERVISOR tentativa=%lu/%d reset_reason=%d", (unsigned long)(s_sup.failed + 1),
             SUP_MAX_ATTEMPTS, (int)esp_reset_reason());
#endif
#if CONFIG_AUDIOLINK_SELFRESET_PROBE
    bool sr_checked = false;
    const esp_reset_reason_t sr_reason = esp_reset_reason();
#endif
#if CONFIG_AUDIOLINK_WIFI_REINIT_QUAL
    static link_reinit_result_t reinit_res;  // static: fora da stack da tarefa main (3584 B)
    uint32_t reinit_count = 0;
    const uint32_t reinit_max = (uint32_t)CONFIG_AUDIOLINK_WIFI_REINIT_QUAL_MAX;
    int64_t next_reinit = (int64_t)CONFIG_AUDIOLINK_WIFI_REINIT_QUAL_START_S * 1000000;
    int64_t q_prev_now = 0, q_start = 0;  // gate de saude do 1o reinit (ver bloco de 1 s)
    uint32_t q_prev_acc = 0, q_prev_bp = 0, q_n = 0, q_pend_sum = 0;
    ESP_LOGW(TAG, "WIFIREINIT QUAL ativo: start_s=%d period_s=%d max=%d", CONFIG_AUDIOLINK_WIFI_REINIT_QUAL_START_S,
             CONFIG_AUDIOLINK_WIFI_REINIT_QUAL_PERIOD_S, CONFIG_AUDIOLINK_WIFI_REINIT_QUAL_MAX);
#endif
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE
    // arranque, fora do loop real-time: log direto aceitavel
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_PAUSE
    ESP_LOGW(TAG, "WIFIREINIT PROBE ativo: op=PAUSE hold_us=%d frac15_at_s=15 decide_at_s=60 strong_milli=%d",
             CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_PAUSE_HOLD_US, PROBE_STRONG_MILLI);
#elif CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
    ESP_LOGW(TAG, "WIFIREINIT PROBE ativo: op=ESPNOW recovery=1 min_ok=%d/%d timeout_s=600 frac15_at_s=15 decide_at_s=60",
             PROBE_REC_MIN_OK, PROBE_REC_WIN);
#elif CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW
    ESP_LOGW(TAG, "WIFIREINIT PROBE ativo: op=ESPNOW frac15_at_s=15 decide_at_s=60 strong_milli=%d", PROBE_STRONG_MILLI);
#else
    ESP_LOGW(TAG, "WIFIREINIT PROBE ativo: frac15_at_s=15 decide_at_s=60 strong_milli=%d", PROBE_STRONG_MILLI);
#endif
    static link_reinit_result_t probe_res;  // static: fora da stack da tarefa main
    static probe_tick_t p_win[PROBE_WIN];  // ultimos 20 ticks (anel)
    uint32_t p_nticks = 0, p_prev_acc = 0, p_prev_bp = 0;
    bool p_decided = false, p_have15 = false, p_fire = false;
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
    bool p_armed = false, p_rec_done = false;
    uint32_t p_rec_ticks = 0;
#endif
    probe_pre_t pre = {0};
    int64_t p_end_us = 0;
    uint32_t p_end_seq = 0, p_end_acc = 0, p_post_ok = 0, p_post_ticks = 0, p_post_done = 0;
#endif

    for (;;) {
        size_t bytes = 0;
        esp_err_t e = i2s_channel_read(rx, payload, payload_bytes, &bytes, 1000);
        if (e != ESP_OK) {
            // PATCH-OBS-TX-01: so conta; tx_diag_task imprime quando o contador sobe.
            s_i2s_read_err++;
            s_i2s_read_err_last = e;
            continue;
        }
        uint32_t frames = (uint32_t)(bytes / LINK_BYTES_PER_FRAME);
        hdr->magic = LINK_PROTO_MAGIC;
        hdr->version = LINK_PROTO_VERSION;
        hdr->flags = 0;
        hdr->frames = (uint16_t)frames;
        hdr->seq = seq++;
        hdr->session_id = session_id;
        hdr->sample_ctr = sample_ctr;
        sample_ctr += frames;

        link_espnow_tx_send(pkt, sizeof(link_pkt_hdr_t) + (size_t)frames * LINK_BYTES_PER_FRAME);
        sent_since++;

        int64_t now = esp_timer_get_time();
        if (now >= next_stat) {
            // PATCH-OBS-TX-01: so preenche o snapshot e publica-o. Nada aqui pode esperar - sem
            // ESP_LOG, sem qsort, sem esp_wifi_get_channel/heap (ficam em tx_diag_task).
            tx_diag_t *d = &s_diag_work;
            d->now_us = now;
            d->t0_us = t0;
            d->adc_blocks = seq;
            d->sample_ctr = sample_ctr;
            d->sent_since = sent_since;

            link_tx_stats_t st;
            link_espnow_tx_get_stats(&st);  // so escalares O(1)
            d->api_accept = st.api_accept;
            d->send_ok = st.send_ok;
            d->send_fail = st.send_fail;
            d->api_err_no_mem = st.api_err_no_mem;
            d->api_err_other = st.api_err_other;
            d->last_err = st.last_err;
            d->pending = st.pending;
            d->backpressure_drop = st.backpressure_drop;
#if CONFIG_AUDIOLINK_WIFI_REINIT
            d->reinit_abandoned = st.reinit_abandoned;
            d->link_down_drop = st.link_down_drop;
#endif

            d->lat_n = link_espnow_tx_get_lat_samples(d->lat_samples, LINK_TX_LAT_SAMPLE_CAP);
#if CONFIG_AUDIOLINK_STARTUP_SUPERVISOR
            if (sup_active) {
                // tick de 1 s com a regra do healthy() do lado do TX; um tick fora de 1 s +-100 ms reinicia a contagem
                uint64_t lsum = 0;
                for (uint32_t i = 0; i < d->lat_n; i++) {
                    lsum += d->lat_samples[i];
                }
                const uint32_t lat = d->lat_n ? (uint32_t)(lsum / d->lat_n) : 0;
                const int64_t dt = now - sup_prev_now;
                const bool tick_ok = sup_prev_now != 0 && dt >= 900000 && dt <= 1100000 && lat <= 3000 &&
                                     st.pending <= 5 && st.api_accept - sup_prev_acc >= 370 &&
                                     st.backpressure_drop - sup_prev_bp <= 1;
                sup_prev_now = now;
                sup_prev_acc = st.api_accept;
                sup_prev_bp = st.backpressure_drop;
                if (tick_ok) {
                    sup_pend_sum += st.pending - sup_pend_win[sup_ok_run % SUP_READY_TICKS];
                    sup_pend_win[sup_ok_run % SUP_READY_TICKS] = st.pending;
                    sup_ok_run++;
                } else {
                    sup_ok_run = 0;
                    sup_pend_sum = 0;
                    memset(sup_pend_win, 0, sizeof(sup_pend_win));
                }
                const char *fail = NULL;
                uint32_t f_milli = 1000;
                if (!sup_have15 && now - t0 >= SUP_FRAC15_US) {
                    sup_have15 = true;
                    f_milli = seq ? (uint32_t)((uint64_t)st.api_accept * 1000 / seq) : 1000;
                    if (st.send_ok == 0) {
                        sup_mode = SUP_WAIT_PEER;
                        ESP_LOGW(TAG, "STARTUP WAIT_PEER tentativa=%lu send_ok=0 send_fail=%lu frac15_milli=%lu uptime_s=%lld",
                                 (unsigned long)(s_sup.failed + 1), (unsigned long)st.send_fail, (unsigned long)f_milli,
                                 (long long)(now / 1000000));
                    } else if (f_milli < SUP_STRONG_MILLI) {
                        fail = "STRONG";
                    }
                }
                if (sup_mode == SUP_WAIT_PEER && st.send_ok > 0) {
                    // nova janela de qualificacao a partir deste instante; o frac15 deste boot e ignorado
                    sup_mode = SUP_PEER_QUAL;
                    sup_peer_t = now;
                    sup_ok_run = 0;
                    sup_pend_sum = 0;
                    memset(sup_pend_win, 0, sizeof(sup_pend_win));
                    ESP_LOGW(TAG, "STARTUP PEER_SEEN tentativa=%lu send_ok=%lu uptime_s=%lld",
                             (unsigned long)(s_sup.failed + 1), (unsigned long)st.send_ok, (long long)(now / 1000000));
                } else if (sup_mode == SUP_WAIT_PEER) {
                    // sem peer: nao conta tentativa, sem reinicio, sem timeout
                } else if (!fail && sup_ok_run >= SUP_READY_TICKS && sup_pend_sum <= 2u * SUP_READY_TICKS) {
                    ESP_LOGW(TAG, "STARTUP READY tentativa=%lu uptime_s=%lld", (unsigned long)(s_sup.failed + 1),
                             (long long)(now / 1000000));
                    s_sup.failed = 0;
                    sup_active = false;
                } else if (!fail && sup_mode == SUP_QUAL && now >= SUP_TIMEOUT_US) {
                    fail = "TIMEOUT";
                } else if (!fail && sup_mode == SUP_PEER_QUAL && now - sup_peer_t >= SUP_TIMEOUT_US) {
                    fail = "PEER_TIMEOUT";
                }
                if (fail) {
                    s_sup.failed++;  // antes de qualquer auto-reinicio
                    if (s_sup.failed >= SUP_MAX_ATTEMPTS) {
                        ESP_LOGE(TAG, "STARTUP GIVEUP tentativas=%lu ultima=%s frac15_milli=%lu - sem mais reinicios",
                                 (unsigned long)s_sup.failed, fail, (unsigned long)f_milli);
                        sup_active = false;
                    } else {
                        ESP_LOGW(TAG, "STARTUP RESTART motivo=%s tentativa=%lu/%d frac15_milli=%lu uptime_s=%lld -> esp_restart()",
                                 fail, (unsigned long)s_sup.failed, SUP_MAX_ATTEMPTS, (unsigned long)f_milli,
                                 (long long)(now / 1000000));
                        vTaskDelay(pdMS_TO_TICKS(50));
                        esp_restart();
                    }
                }
            }
#endif
#if CONFIG_AUDIOLINK_SELFRESET_PROBE
            // SELFRESET-PROBE-01: uma unica decisao no tick de t0+15 s. Log direto na tarefa TX (uma linha por
            // boot; o atraso da consola e irrelevante aqui) e 50 ms para a linha sair antes do reset.
            if (!sr_checked && now - t0 >= 15000000) {
                sr_checked = true;
                const uint32_t f_milli = seq ? (uint32_t)((uint64_t)st.api_accept * 1000 / seq) : 1000;
                if (f_milli < 568 && sr_reason != ESP_RST_SW) {
                    ESP_LOGW(TAG, "SELFRESET RESTART frac15_milli=%lu opp15=%lu acc15=%lu reset_reason=%d -> esp_restart()",
                             (unsigned long)f_milli, (unsigned long)seq, (unsigned long)st.api_accept, (int)sr_reason);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    esp_restart();
                }
                ESP_LOGW(TAG, "SELFRESET CHECK frac15_milli=%lu reset_reason=%d decision=%s", (unsigned long)f_milli,
                         (int)sr_reason, f_milli >= 568 ? "NAO_STRONG" : "STRONG_MAS_BOOT_SW_skip");
            }
#endif
#if CONFIG_AUDIOLINK_WIFI_REINIT_QUAL
            // Gate do 1o reinit: TX saudavel de forma continua durante >= 20 s, com a regra da pre-condicao
            // do orquestrador (healthy() do phycal_block/series_run2): em cada tick de 1 s cb_lat_avg <= 3000 us,
            // pending <= 5, d(api_accept) >= 370, d(backpressure_drop) <= 1, e pending medio da janela <= 2.
            // Um tick fora de 1 s +-100 ms (pausa da tarefa) reinicia a janela. So antes do 1o reinit.
            if (s_qual_armed_us == 0) {
                uint64_t lsum = 0;
                for (uint32_t i = 0; i < d->lat_n; i++) {
                    lsum += d->lat_samples[i];
                }
                const uint32_t lat_avg = d->lat_n ? (uint32_t)(lsum / d->lat_n) : 0;
                const int64_t dt = now - q_prev_now;
                const bool sec_ok = q_prev_now != 0 && dt >= 900000 && dt <= 1100000 && lat_avg <= 3000 &&
                                    st.pending <= 5 && st.api_accept - q_prev_acc >= 370 &&
                                    st.backpressure_drop - q_prev_bp <= 1;
                if (!sec_ok) {
                    q_n = 0;
                    q_pend_sum = 0;
                } else {
                    if (q_n == 0) {
                        q_start = q_prev_now;
                    }
                    q_n++;
                    q_pend_sum += st.pending;
                    if (now - q_start >= 20000000 && q_pend_sum <= 2 * q_n) {
                        s_qual_healthy_s = (uint32_t)((now - q_start) / 1000000);
                        s_qual_armed_us = now;
                    }
                }
                q_prev_now = now;
                q_prev_acc = st.api_accept;
                q_prev_bp = st.backpressure_drop;
            }
#endif
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE
            {
                // tick de 1 s com a regra de tick do healthy() do orquestrador
                uint64_t lsum = 0;
                for (uint32_t i = 0; i < d->lat_n; i++) {
                    lsum += d->lat_samples[i];
                }
                probe_tick_t tk = {.lat_avg = d->lat_n ? (uint32_t)(lsum / d->lat_n) : 0,
                                   .pending = st.pending,
                                   .dacc = st.api_accept - p_prev_acc,
                                   .dbp = st.backpressure_drop - p_prev_bp};
                tk.ok = p_nticks > 0 && tk.lat_avg <= 3000 && tk.pending <= 5 && tk.dacc >= 370 && tk.dbp <= 1;
                p_prev_acc = st.api_accept;
                p_prev_bp = st.backpressure_drop;
                p_win[p_nticks % PROBE_WIN] = tk;
                p_nticks++;
                if (p_end_us != 0 && p_post_done < 2) {
                    p_post_ticks++;
                    p_post_ok += tk.ok ? 1 : 0;
                    const uint32_t win = p_post_done == 0 ? 30 : 60;
                    if (now - p_end_us >= (int64_t)win * 1000000) {
                        const uint32_t opp = seq - p_end_seq, acc = st.api_accept - p_end_acc;
                        probe_post_t q = {.t_us = now, .win_s = win, .opp = opp, .acc = acc,
                                          .frac_milli = opp ? (uint32_t)((uint64_t)acc * 1000 / opp) : 0,
                                          .ok_ticks = p_post_ok, .ticks = p_post_ticks};
                        portENTER_CRITICAL(&s_probe_lock);
                        s_probe_post[p_post_done] = q;
                        s_probe_pub |= (2u << p_post_done);
                        portEXIT_CRITICAL(&s_probe_lock);
                        p_post_done++;
                    }
                }
                if (!p_have15 && now - t0 >= PROBE_FRAC15_US) {
                    // classificacao: oportunidades e aceites desde t0 (a 1a oportunidade) ate ao tick de t0+15 s
                    p_have15 = true;
                    pre.t15_us = now;
                    pre.opp15 = seq;
                    pre.acc15 = st.api_accept;
                    pre.frac15_milli = seq ? (uint32_t)((uint64_t)st.api_accept * 1000 / seq) : 1000;
                    pre.strong = pre.frac15_milli < PROBE_STRONG_MILLI;
                }
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
                // armado no tick de t0+60 s: avaliar a partir do tick seguinte (inclui ticks anteriores na janela de 10)
                if (p_armed && !p_rec_done) {
                    p_rec_ticks++;
                    uint32_t h10 = 0;
                    for (uint32_t i = 1; i <= PROBE_REC_WIN; i++) {
                        h10 += p_win[(p_nticks - i) % PROBE_WIN].ok ? 1 : 0;
                    }
                    const bool trig = h10 >= PROBE_REC_MIN_OK;
                    if (trig || now >= PROBE_REC_TIMEOUT_US) {
                        p_rec_done = true;
                        p_fire = trig;
                        probe_trig_t tr = {.t_us = now, .healthy10 = h10, .ticks_since_arm = p_rec_ticks};
                        portENTER_CRITICAL(&s_probe_lock);
                        s_probe_trig = tr;
                        s_probe_pub |= trig ? (1u << 3) : (1u << 4);
                        portEXIT_CRITICAL(&s_probe_lock);
                    }
                }
#endif
                if (p_have15 && !p_decided && now - t0 >= PROBE_DECIDE_US) {
                    // decisao unica deste boot, no tick de t0+60 s
                    p_decided = true;
                    pre.t_us = now;
                    // so com 20 ticks que excluem o 1o (cujo d() e contra zero); a t0+60 s ha ~60
                    if (p_nticks > PROBE_WIN) {
                        pre.win_n = PROBE_WIN;
                        for (int i = 0; i < PROBE_WIN; i++) {
                            pre.sum_dacc += p_win[i].dacc;
                            pre.sum_dbp += p_win[i].dbp;
                            pre.sum_lat += p_win[i].lat_avg;
                            pre.sum_pend += p_win[i].pending;
                        }
                        pre.fam_t = pre.sum_dacc < 370u * PROBE_WIN || pre.sum_dbp > 1u * PROBE_WIN;
                        pre.fam_l = pre.sum_lat > 3000u * PROBE_WIN || pre.sum_pend > 2u * PROBE_WIN;
                    }
                    pre.reinit = pre.fam_t && pre.fam_l;
                    portENTER_CRITICAL(&s_probe_lock);
                    s_probe_pre = pre;
                    s_probe_pub |= 1;
                    portEXIT_CRITICAL(&s_probe_lock);
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW_RECOVERY
                    p_armed = pre.reinit;  // nao intervir aqui: esperar pela fase de recuperacao
#else
                    p_fire = pre.reinit;
#endif
                }
                if (p_fire) {
                    p_fire = false;
                    {
#if CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_PAUSE
                        link_espnow_tx_pause(&probe_res, CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_PAUSE_HOLD_US);
#elif CONFIG_AUDIOLINK_WIFI_REINIT_PROBE_ESPNOW
                        link_espnow_tx_espnow_reinit(&probe_res);  // so o retry interno da primitive
#else
                        link_espnow_tx_reinit(&probe_res);  // so o retry interno da primitive
#endif
                        p_end_us = probe_res.t_end_us;
                        p_end_seq = seq;
                        link_tx_stats_t st2;
                        link_espnow_tx_get_stats(&st2);
                        p_end_acc = st2.api_accept;
                        p_prev_acc = st2.api_accept;  // o 1o tick POST nao conta envios de antes do reinit
                        p_prev_bp = st2.backpressure_drop;
                    }
                }
            }
#endif

            d->i2s_overrun = s_i2s_overrun;
            d->i2s_read_err = s_i2s_read_err;
            d->i2s_read_err_last = s_i2s_read_err_last;
            d->seq = ++s_diag_seq;

            // publicacao: unica secao partilhada com o logger, so uma copia de struct
            const int64_t t_cs0 = esp_timer_get_time();
            portENTER_CRITICAL(&s_diag_lock);
            s_diag_pub = *d;
            portEXIT_CRITICAL(&s_diag_lock);
            const uint32_t cs_us = (uint32_t)(esp_timer_get_time() - t_cs0);
            if (cs_us > s_pub_cs_max_ever_us) {
                s_pub_cs_max_ever_us = cs_us;
            }
            d->pub_cs_max_ever_us = s_pub_cs_max_ever_us;  // so no snapshot seguinte (informativo, 1 s de atraso)

            sent_since = 0;
            next_stat += 1000000;
        }
#if CONFIG_AUDIOLINK_WIFI_REINIT_QUAL
        // WIFI-REINIT-01, so qualificacao de seguranca (nao e o desenho experimental): reinit periodico a
        // partir de START_S de uptime, a cada PERIOD_S (contado do fim do anterior), ate MAX (0 = sem limite).
        if (s_qual_armed_us != 0 && now >= next_reinit && (reinit_max == 0 || reinit_count < reinit_max)) {
            link_espnow_tx_reinit(&reinit_res);
            reinit_count++;
            next_reinit = esp_timer_get_time() + (int64_t)CONFIG_AUDIOLINK_WIFI_REINIT_QUAL_PERIOD_S * 1000000;
        }
#endif
    }
}
