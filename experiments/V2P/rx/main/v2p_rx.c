// AudioLink V2 - prototipo do RX (docs/AUDIOLINK_V2_PROTOTYPE_PLAN_2026-11-01.md). Nao e producao.
//
// = RX do V2-T1 (experiments/V2T1/rx) com o minimo de mudancas:
//   - recebe o ALK v3 real (28 B de cabecalho + 128 tramas PCM24; components/alk_proto) do TX V2_PEER_MAC;
//   - playout de prazo fixo pelo indice absoluto (sample_ctr): DOUT - A = V2_LRX_US, A = envolvente das chegadas sem
//     espera (maximo da folga invariante aos slips em 32 sub-janelas de 0,5 s), slips de 1 trama no maximo 1/100 ms;
//     anel de 4 x 32 tramas, auto_clear;
//   - AUDIO LIGADO: bloco ausente no prazo = ocultado (mantem a ultima amostra e desvanece em 32 tramas; volta com
//     subida de 32 tramas). TODAS as descontinuidades sao contadas: blocos e episodios ocultados, blocos fora de prazo,
//     lacunas de sequencia, saltos do sample_ctr no TX e slips;
//   - saida PCM5102A (BCLK 5, WS 6, DIN 7, sem MCLK) ou, com V2_OUT_WM8805, WM8805/S-PDIF da placa do AudioCast
//     (MCLK 256 fs no GPIO 18, BCLK 16, WS 8, DATA 15; como o W2/L2-12);
//   - V2_PASSIVE = 1 (so para o P1b): sem ESP-NOW, modo promiscuo; le as tramas do TX de producao para o RX de producao
//     (captura do SHADOW). Valida temporizacao, nao a fiabilidade da rececao normal (sem ACK);
//   - V2_MARK = 1 (so na bancada experimental): flanco do marcador do TX no GPIO12 e o pedido do bloco marcado;
//   - V2_LED = 1 (P2, na sala sem consola): LED WS2812 (components/al_led) atualizado 1x/s: branco ate o playout
//     arrancar, ciano = a tocar, ciano a piscar = sem pacotes do TX, vermelho durante 5 s depois de um segundo com
//     ocultacao ou falha do RX (os slips nao acendem o LED; ficam so nos contadores);
//   - V2_TELE = 1 (P2): as linhas V/H/SESSAO/PLAYOUT vao tambem por ESP-NOW em difusao (HT20 MCS4, sem ACK nem
//     repeticoes; ~1 trama de ~350 B por segundo) para o recetor de telemetria no Mac (experiments/V2P/sink). Os
//     contadores sao cumulativos: uma linha perdida so tira resolucao temporal.
// Registo leve: 1 linha por segundo (V), histograma da folga a cada 10 s (H); nunca por pacote.
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alk_pcm24.h"
#include "alk_proto.h"
#include "v2p_slot.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#ifndef V2_OUT_WM8805
#define V2_OUT_WM8805 0
#endif
#ifndef V2_PASSIVE
#define V2_PASSIVE 0
#endif
#ifndef V2_MARK
#define V2_MARK 0
#endif
#ifndef V2_PEER_MAC
#define V2_PEER_MAC {0x28, 0x84, 0x85, 0x64, 0x1d, 0xec}  // TX de producao
#endif
#ifndef V2_LRX_US
#define V2_LRX_US 20500
#endif
#ifndef V2_LED
#define V2_LED 0
#endif
#ifndef V2_TELE
#define V2_TELE 0
#endif
#ifndef V2_BTN
#define V2_BTN 0
#endif
#if V2_BTN
#include "v2p_btn.h"
#include "v2p_pairrec.h"
#if V2_PASSIVE
#include "nvs.h"
#endif
#ifndef V2_BTN_GPIO
#define V2_BTN_GPIO 0
#endif
#if (V2_OUT_WM8805 && (V2_BTN_GPIO == 18 || V2_BTN_GPIO == 16 || V2_BTN_GPIO == 8 || V2_BTN_GPIO == 15)) || \
    (!V2_OUT_WM8805 && (V2_BTN_GPIO == 5 || V2_BTN_GPIO == 6 || V2_BTN_GPIO == 7)) || \
    (V2_LED && V2_BTN_GPIO == 48) || (V2_MARK && V2_BTN_GPIO == 12)
#error "V2_BTN_GPIO collides with an active I2S, LED or MARK pin (use a numeric GPIO)"
#endif
#endif
#if V2_TELE && V2_PASSIVE
#error "V2_TELE precisa de ESP-NOW (modo normal)"
#endif
#if V2_LED
#include "al_led.h"
#endif
#define V2_CHANNEL 6
#define V2_FS 48076.923
#define V2_T_FRAME_US (1e6 / V2_FS)
#define V2_T_BLK_US (ALK_FRAMES_PER_PKT * V2_T_FRAME_US)
#define V2_SAMPLES (ALK_FRAMES_PER_PKT * ALK_CHANNELS)
#define V2_DMA_DESC 4
#define V2_DMA_FRAMES 32
#define V2_STORE 96u  // blocos (256 ms)
#define V2_DEDUP 1024u
#define V2_START 750
#define V2_SUBWIN_US 500000
#define V2_NSUB 32
#define V2_SLIP_GAP 150
#define V2_FADE 32
#define V2_QLEN 48
#define V2_HB 40
#define V2_MARK_EVERY_FRAMES (752u * 64u)  // o TX de teste marca o pedaco c % 752 == 0 (c em pedacos de 64 tramas)
#if V2_OUT_WM8805
#define V2_PIN_MCLK GPIO_NUM_18
#define V2_PIN_BCLK GPIO_NUM_16
#define V2_PIN_WS GPIO_NUM_8
#define V2_PIN_DOUT GPIO_NUM_15
#else
#define V2_PIN_MCLK I2S_GPIO_UNUSED
#define V2_PIN_BCLK GPIO_NUM_5
#define V2_PIN_WS GPIO_NUM_6
#define V2_PIN_DOUT GPIO_NUM_7
#endif
#define V2_PIN_MARK GPIO_NUM_12

static const char *TAG = "v2p_rx";
static const uint8_t PEER[6] = V2_PEER_MAC;

typedef struct {
    int64_t t_us;
    int8_t rssi;
#if V2_BTN && V2_PASSIVE
    uint32_t assoc_generation;
#endif
    uint8_t data[ALK_V3_PKT_BYTES];
} rx_frame_t;

static QueueHandle_t s_q;
static rx_frame_t s_scratch;
static RingbufHandle_t s_log;
static volatile uint32_t c_logdrop, c_qdrop, c_badlen, c_foreign;
static volatile int64_t s_edges[16];
static volatile uint32_t c_edges;

static v2p_slot_t *s_store;
static portMUX_TYPE s_store_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_pub_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_pub_p = -1, s_pub_t;  // indice relativo da 1.a trama do ultimo pedaco escrito e o seu DOUT previsto
static int64_t s_wr_blk = -1;          // sob s_store_mux
static volatile int32_t s_slips, s_envinv_us;
static volatile bool s_env_valid, s_start_ready, s_reset_req;
static volatile double s_omin;
static volatile int64_t s_t_eof, s_base;  // s_base: sample_ctr do bloco 0 da sessao
// contadores cumulativos (todas as descontinuidades)
static volatile uint32_t c_played, c_conc_blk, c_conc_ev, c_conc_fr, c_late, c_slip_d, c_slip_i, c_dmaovf, c_wslow;
static volatile int32_t c_wmax_us;
static volatile uint32_t c_hist[V2_HB];
static volatile int32_t c_min_slack = INT32_MAX;
#if V2_TELE
static volatile uint32_t c_tele_fail;  // esp_now_send recusado (a linha so fica na consola USB)
#define V2_TELE_FMT " tfail=%" PRIu32
#define V2_TELE_ARG , c_tele_fail
#else
#define V2_TELE_FMT
#define V2_TELE_ARG
#endif

#if V2_BTN
static portMUX_TYPE s_btn_mux = portMUX_INITIALIZER_UNLOCKED;
static v2p_pair_t s_btn_pair;
static uint32_t c_btn_resync, c_btn_nvsfail;  // capture owns resync; mux protects nvsfail
#define V2_BTN_FMT " btn=%" PRIu32 ",%" PRIu32 ",%u"
#define V2_BTN_ARG , c_btn_resync, btn_nvsfail, btn_pair ? 1u : 0u
#if V2_PASSIVE
static v2p_pairrec_t s_btn_assoc, s_btn_requested;
static uint32_t s_btn_generation;
static uint32_t s_btn_request_serial;
static bool s_btn_pending, s_btn_working, s_btn_resync_req;
static v2p_btn_action_t s_btn_request;
#endif
#endif

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0 && xRingbufferSend(s_log, b, n < (int)sizeof b ? n : (int)sizeof b - 1, 0) != pdTRUE) c_logdrop++;
}

static void enqueue(const uint8_t *data, int64_t t_us, int8_t rssi) {
    rx_frame_t *f = &s_scratch;
    f->t_us = t_us;
    f->rssi = rssi;
    memcpy(f->data, data, ALK_V3_PKT_BYTES);
    if (xQueueSend(s_q, f, 0) != pdTRUE) c_qdrop++;
}

#if V2_PASSIVE
// captura do SHADOW (experiments/SHADOW): tramas de acao ESP-NOW v2 do TX para o RX de producao
static const uint8_t PROD_RX[6] = {0xac, 0x27, 0x6e, 0xa5, 0x4e, 0x10};
#if V2_BTN
static void btn_defaults(v2p_pairrec_t *rec) {
    memcpy(rec->tx, PEER, 6);
    memcpy(rec->dst, PROD_RX, 6);
}

static void btn_load_record(void) {
    btn_defaults(&s_btn_assoc);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("alk", NVS_READONLY, &handle);
    v2p_pairrec_result_t result = V2P_PAIRREC_BAD_LEN;
    if (err == ESP_OK) {
        uint8_t blob[V2P_PAIRREC_BYTES];
        size_t len = sizeof blob;
        err = nvs_get_blob(handle, "assoc", blob, &len);
        v2p_pairrec_t rec;
        if (err == ESP_OK) result = v2p_pairrec_decode(blob, len, &rec);
        if (result == V2P_PAIRREC_OK) s_btn_assoc = rec;
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "BTN assoc result=%d (OK=0 LEN=1 MAGIC=2 VERSION=3 CRC=4 MAC=5) nvs=%s",
             (int)result, esp_err_to_name(err));
}

// Only log_task calls this: NVS flash writes never run in capture or the Wi-Fi callback.
// An NVS commit writes flash, suspending the flash cache on both cores for its duration.
// With the 4x32-frame DMA ring (~2.7 ms), explicit CONFIRM/CLEAR can cause one audible
// glitch/DMA underrun. This single, user-initiated glitch is accepted by design; no mitigation here.
static void btn_save_request(void) {
    v2p_pairrec_t next;
    v2p_btn_action_t action;
    taskENTER_CRITICAL(&s_btn_mux);
    const bool ready = s_btn_pending && !s_btn_working;
    action = s_btn_request;
    next = s_btn_requested;
    const uint32_t serial = s_btn_request_serial;
    if (ready) s_btn_working = true;
    taskEXIT_CRITICAL(&s_btn_mux);
    if (!ready) return;

    uint8_t blob[V2P_PAIRREC_BYTES];
    v2p_pairrec_t checked;
    if (action == V2P_BTN_CLEAR_RECORD) btn_defaults(&next);
    v2p_pairrec_encode(next.tx, next.dst, blob);
    esp_err_t err = v2p_pairrec_decode(blob, sizeof blob, &checked) == V2P_PAIRREC_OK
                        ? ESP_OK : ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (err == ESP_OK) {
        err = nvs_open("alk", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            if (action == V2P_BTN_CLEAR_RECORD) {
                err = nvs_erase_key(handle, "assoc");
                if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            } else {
                err = nvs_set_blob(handle, "assoc", blob, sizeof blob);
            }
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    taskENTER_CRITICAL(&s_btn_mux);
    if (err == ESP_OK) {
        s_btn_assoc = next;
        s_btn_generation++;
        s_btn_resync_req = true;
    } else {
        c_btn_nvsfail++;
    }
    if (serial == s_btn_request_serial) s_btn_pending = false;
    s_btn_working = false;
    taskEXIT_CRITICAL(&s_btn_mux);
    logf_("BTN NVS %s: %s\n", action == V2P_BTN_CLEAR_RECORD ? "CLEAR" : "CONFIRM", esp_err_to_name(err));
}
#endif
static const uint8_t ESP_OUI[3] = {0x18, 0xfe, 0x34};
static uint8_t s_body[ALK_V3_PKT_BYTES];
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const int len = (int)p->rx_ctrl.sig_len - 4;
    const uint8_t *f = p->payload;
#if V2_BTN
    if (len < 32 || f[0] != 0xD0) return;
    v2p_pairrec_t assoc;
    v2p_pair_state_t pair_state;
    taskENTER_CRITICAL(&s_btn_mux);
    assoc = s_btn_assoc;
    pair_state = s_btn_pair.state;
    taskEXIT_CRITICAL(&s_btn_mux);
    const bool associated_stream = !memcmp(f + 10, assoc.tx, 6) && !memcmp(f + 4, assoc.dst, 6);
    if (pair_state != V2P_PAIR_PAIR && !associated_stream) return;
#else
    if (len < 32 || f[0] != 0xD0 || memcmp(f + 10, PEER, 6) != 0 || memcmp(f + 4, PROD_RX, 6) != 0) return;
#endif
    if (f[24] != 127 || memcmp(f + 25, ESP_OUI, 3) != 0) {
#if V2_BTN
        if (!associated_stream) return;
#endif
        c_badlen++;
        return;
    }
    int off = 32, out = 0;
    while (off + 2 <= len && f[off] == 221) {
        const int el = f[off + 1];
        if (el < 5 || off + 2 + el > len || out + el - 5 > (int)ALK_V3_PKT_BYTES) {
#if V2_BTN
            if (!associated_stream) return;
#endif
            c_badlen++;
            return;
        }
        memcpy(s_body + out, f + off + 7, el - 5);
        out += el - 5;
        off += 2 + el;
    }
    if (out != (int)ALK_V3_PKT_BYTES) {
#if V2_BTN
        if (!associated_stream) return;
#endif
        c_badlen++;
        return;
    }
#if V2_BTN
    alk_hdr_t hdr;
    memcpy(&hdr, s_body, sizeof hdr);
    if (hdr.magic != ALK_MAGIC || hdr.version != ALK_VERSION_V3 || hdr.frames != ALK_FRAMES_PER_PKT) return;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&s_btn_mux);
    v2p_pair_record(&s_btn_pair, f + 10, f + 4, hdr.session_id, now_ms);
    const bool associated = !memcmp(f + 10, s_btn_assoc.tx, 6) && !memcmp(f + 4, s_btn_assoc.dst, 6);
    s_scratch.assoc_generation = s_btn_generation;  // carried unchanged by enqueue (Wi-Fi callbacks are serialized)
    taskEXIT_CRITICAL(&s_btn_mux);
    if (!associated) return;
#endif
    enqueue(s_body, esp_timer_get_time(), (int8_t)p->rx_ctrl.rssi);
}
#else
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    const int64_t now = esp_timer_get_time();
    if (!info->src_addr || memcmp(info->src_addr, PEER, 6) != 0) {
        c_foreign++;
        return;
    }
    if (len != (int)ALK_V3_PKT_BYTES) {
        c_badlen++;
        return;
    }
    enqueue(data, now, info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0);
}
#endif

#if V2_MARK
static void IRAM_ATTR mark_isr(void *arg) {
    s_edges[c_edges % 16] = esp_timer_get_time();
    c_edges++;
}
#endif

static void capture_task(void *arg) {
    static rx_frame_t fr;
    static uint64_t seen[V2_DEDUP];
    static int32_t pcm[V2_SAMPLES];
    uint64_t sess = 0, base = 0;
    bool have = false, have_seq = false;
    uint32_t last_seq = 0, n_start = 0, edges_seen = 0;
    int64_t last_idx = 0;
    double omin = 1e18, sub[V2_NSUB], cur_max = -1e18;
    int nsub = 0, sub_i = 0;
    int64_t sub_end = 0, t_next = esp_timer_get_time() + 1000000;
    uint32_t up = 0, n_pk = 0, dup = 0, badalk = 0, sess_n = 0, gap_pk = 0, gap_ev = 0, txj = 0;
#if V2_BTN
    v2p_btn_t button;
    v2p_btn_init(&button);
    int64_t btn_next_us = 0;
#if V2_LED
    uint32_t btn_led_nvsfail = 0;
#endif
#endif
    for (;;) {
#if V2_BTN
        const int64_t btn_now_us = esp_timer_get_time();
        bool btn_resync = false;
        if (btn_now_us >= btn_next_us) {
            btn_next_us = btn_now_us + 10000;
            const uint32_t btn_now_ms = (uint32_t)(btn_now_us / 1000);
            const v2p_btn_event_t event = v2p_btn_sample(&button, gpio_get_level(V2_BTN_GPIO) == 0, btn_now_ms);
#if V2_PASSIVE
            v2p_candidate_t confirmed = {0};
            taskENTER_CRITICAL(&s_btn_mux);
            const v2p_btn_action_t action = v2p_pair_step(&s_btn_pair, event, btn_now_ms, &confirmed);
            if (action == V2P_BTN_CONFIRM || action == V2P_BTN_CLEAR_RECORD) {
                s_btn_request = action;
                if (action == V2P_BTN_CONFIRM) {
                    memcpy(s_btn_requested.tx, confirmed.src, 6);
                    memcpy(s_btn_requested.dst, confirmed.dst, 6);
                }
                s_btn_pending = true;
                s_btn_request_serial++;
            }
            taskEXIT_CRITICAL(&s_btn_mux);
            btn_resync = action == V2P_BTN_RESYNC;
            if (action != V2P_BTN_ACTION_NONE && action != V2P_BTN_RESYNC)
                logf_("BTN action=%d (PAIR=2 CANCEL=3 CONFIRM=4 CLEAR=5)\n", (int)action);
#else
            btn_resync = event == V2_BTN_SHORT;
            if (event == V2_BTN_LONG || event == V2_BTN_VERY_LONG)
                logf_("BTN pairing/clear ignored: normal ESP-NOW mode\n");
#endif
        }
#if V2_PASSIVE
        taskENTER_CRITICAL(&s_btn_mux);
        btn_resync = btn_resync || s_btn_resync_req;
        s_btn_resync_req = false;
        taskEXIT_CRITICAL(&s_btn_mux);
#endif
        if (btn_resync) {
            have = false;
            s_start_ready = false;
            s_env_valid = false;
            s_reset_req = true;  // have=false alone does not trigger the session-change reset
            c_btn_resync++;
            logf_("BTN RESYNC %" PRIu32 "\n", c_btn_resync);
        }
#endif
#if V2_BTN
        if (xQueueReceive(s_q, &fr, pdMS_TO_TICKS(10)) == pdTRUE) {
#else
        if (xQueueReceive(s_q, &fr, pdMS_TO_TICKS(20)) == pdTRUE) {
#endif
#if V2_BTN && V2_PASSIVE
            taskENTER_CRITICAL(&s_btn_mux);
            const bool current_assoc = fr.assoc_generation == s_btn_generation;
            taskEXIT_CRITICAL(&s_btn_mux);
            if (!current_assoc) goto tick;  // never play queued audio from a replaced association
#endif
            alk_hdr_t h;
            memcpy(&h, fr.data, sizeof h);
            if (h.magic != ALK_MAGIC || h.version != ALK_VERSION_V3 || h.frames != ALK_FRAMES_PER_PKT) {
                badalk++;
                goto tick;
            }
            if (!have || h.session_id != sess) {
                if (have) {
                    sess_n++;
                    s_reset_req = true;
                }
                have = true;
                have_seq = false;
                sess = h.session_id;
                base = h.sample_ctr;
                s_base = (int64_t)base;
                memset(seen, 0, sizeof seen);
                n_start = 0;
                omin = 1e18;
                nsub = 0;
                cur_max = -1e18;
                sub_end = 0;
                s_start_ready = false;
                s_env_valid = false;
                logf_("SESSAO %016" PRIx64 " seq=%" PRIu32 " sample_ctr=%" PRIu64 "\n", sess, h.seq, h.sample_ctr);
            }
            if (h.sample_ctr < base || (h.sample_ctr - base) % ALK_FRAMES_PER_PKT) {
                badalk++;
                goto tick;
            }
            const int64_t idx = (int64_t)((h.sample_ctr - base) / ALK_FRAMES_PER_PKT);
            uint64_t *sl = &seen[idx % V2_DEDUP];
            if (*sl == (uint64_t)idx + 1) {
                dup++;  // retransmissao vista (modo passivo) ou duplicado
                goto tick;
            }
            *sl = (uint64_t)idx + 1;
            n_pk++;
            // lacunas de sequencia; saltos do sample_ctr sem pacote em falta = perda no ADC do TX
            if (have_seq && h.seq > last_seq + 1) {
                gap_pk += h.seq - last_seq - 1;
                gap_ev++;
            }
            if (have_seq && h.seq == last_seq + 1 && idx > last_idx + 1) txj += (uint32_t)(idx - last_idx - 1);
            if (!have_seq || h.seq > last_seq) {
                last_seq = h.seq;
                last_idx = idx;
            }
            have_seq = true;

            alk_pcm24_unpack(fr.data + ALK_HDR_BYTES, V2_SAMPLES, (uint8_t *)pcm);
            const double x = (double)idx * ALK_FRAMES_PER_PKT;
            if (!s_start_ready) {
                const double o = (double)fr.t_us - x * V2_T_FRAME_US;
                if (o < omin) omin = o;
                if (++n_start >= V2_START) {
                    s_omin = omin;
                    s_start_ready = true;
                }
            }
            int64_t pp, pt;
            taskENTER_CRITICAL(&s_pub_mux);
            pp = s_pub_p;
            pt = s_pub_t;
            taskEXIT_CRITICAL(&s_pub_mux);
            double slack = NAN;
            if (pp >= 0) {
                slack = (double)pt + (x - (double)pp) * V2_T_FRAME_US - (double)fr.t_us;
                if (sub_end == 0) sub_end = fr.t_us + V2_SUBWIN_US;
                if (fr.t_us >= sub_end) {
                    sub[sub_i] = cur_max;
                    sub_i = (sub_i + 1) % V2_NSUB;
                    if (nsub < V2_NSUB) nsub++;
                    cur_max = -1e18;
                    sub_end += V2_SUBWIN_US;
                }
                const double sl_t = (double)s_slips * V2_T_FRAME_US;
                if (slack + sl_t > cur_max) cur_max = slack + sl_t;
                if (nsub >= 4) {
                    double env = cur_max;
                    for (int i = 0; i < nsub; i++)
                        if (sub[i] > env) env = sub[i];
                    s_envinv_us = (int32_t)lrint(env);
                    s_env_valid = true;
                }
            }
            v2p_slot_t *st = &s_store[idx % V2_STORE];
            bool first = false, late_first = false;
            taskENTER_CRITICAL(&s_store_mux);
            const bool late = s_wr_blk >= 0 && idx <= s_wr_blk;
            const bool same = st->tag == idx + 1;
            if (!late && !(same && st->ready)) {
                memcpy(st->pcm, pcm, sizeof pcm);
                st->tag = idx + 1;
                st->ready = 1;
                st->played = 0;
                st->late_seen = 0;
                first = true;
            } else if (late) {
                late_first = v2p_slot_note_late(st, idx);
            }
            taskEXIT_CRITICAL(&s_store_mux);
            if (late_first) c_late++;
            if ((first || late_first) && !isnan(slack)) {
                int b = (int)floor(slack / 1000.0) + 10;
                c_hist[b < 0 ? 0 : (b >= V2_HB ? V2_HB - 1 : b)]++;
                if (first && slack < c_min_slack) c_min_slack = (int32_t)lrint(slack);
            }
        }
    tick:;
        while (edges_seen != c_edges) {
            logf_("E %" PRId64 "\n", s_edges[edges_seen % 16]);
            edges_seen++;
        }
        const int64_t now = esp_timer_get_time();
        if (now < t_next) continue;
        t_next += 1000000;
        up++;
        const int32_t wmax = c_wmax_us;
        c_wmax_us = 0;
#if V2_BTN
        taskENTER_CRITICAL(&s_btn_mux);
        const bool btn_pair = s_btn_pair.state == V2P_PAIR_PAIR;
        const uint32_t btn_nvsfail = c_btn_nvsfail;
        taskEXIT_CRITICAL(&s_btn_mux);
#endif
        logf_("V up=%" PRIu32 " pk=%" PRIu32 " dup=%" PRIu32 " gap=%" PRIu32 " gapev=%" PRIu32 " txj=%" PRIu32
              " badalk=%" PRIu32
              " badlen=%" PRIu32 " foreign=%" PRIu32 " qdrop=%" PRIu32 " sess=%" PRIu32 " played=%" PRIu32
              " cblk=%" PRIu32 " cev=%" PRIu32 " cfr=%" PRIu32 " late=%" PRIu32 " slipd=%" PRIu32 " slipi=%" PRIu32
              " dmaovf=%" PRIu32 " wslow=%" PRIu32 " wmax=%" PRId32 " env=%" PRId32 " tgt=%d min=%" PRId32
#if V2_BTN
              " logdrop=%" PRIu32 V2_TELE_FMT V2_BTN_FMT "\n",
#else
              " logdrop=%" PRIu32 V2_TELE_FMT "\n",
#endif
              up, n_pk, dup, gap_pk, gap_ev, txj, badalk, c_badlen, c_foreign, c_qdrop, sess_n, c_played, c_conc_blk,
              c_conc_ev, c_conc_fr, c_late, c_slip_d, c_slip_i, c_dmaovf, c_wslow, wmax,
              s_env_valid ? (int32_t)lrint(s_envinv_us - s_slips * V2_T_FRAME_US) : -1, V2_LRX_US, c_min_slack,
#if V2_BTN
              c_logdrop V2_TELE_ARG V2_BTN_ARG);
#else
              c_logdrop V2_TELE_ARG);
#endif
        if (up % 10 == 0) {
            char hb[V2_HB * 8 + 8];
            int o = snprintf(hb, sizeof hb, "H");
            for (int i = 0; i < V2_HB; i++) o += snprintf(hb + o, sizeof hb - o, " %" PRIu32, c_hist[i]);
            logf_("%s\n", hb);
        }
#if V2_LED
        {
            static uint32_t led_prev;
            const uint32_t bad = c_conc_blk + c_dmaovf + c_wslow + c_qdrop + c_badlen + badalk;
#if V2_BTN
            al_led_obs_t obs = {.booting = !s_start_ready, .error_now = bad != led_prev, .peer_ok = n_pk > 0};
            if (btn_pair && (up & 1u)) obs.booting = true;
            obs.error_now = obs.error_now || btn_nvsfail != btn_led_nvsfail;
            btn_led_nvsfail = btn_nvsfail;
            al_led_update(obs);
#else
            al_led_update((al_led_obs_t){.booting = !s_start_ready, .error_now = bad != led_prev, .peer_ok = n_pk > 0});
#endif
            led_prev = bad;
        }
#endif
        n_pk = dup = 0;
    }
}

static int64_t t_eof_read(void) {
    int64_t a, b;
    do {
        a = s_t_eof;
        b = s_t_eof;
    } while (a != b);
    return a;
}

static bool IRAM_ATTR on_sent(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx) {
    s_t_eof = esp_timer_get_time();
    return false;
}

static bool IRAM_ATTR on_q_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx) {
    c_dmaovf++;
    return false;
}

static void writer_task(void *arg) {
    i2s_chan_handle_t tx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = V2_DMA_DESC;
    cc.dma_frame_num = V2_DMA_FRAMES;
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = 48077, .clk_src = I2S_CLK_SRC_PLL_160M, .ext_clk_freq_hz = 0,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = V2_PIN_MCLK, .bclk = V2_PIN_BCLK, .ws = V2_PIN_WS, .dout = V2_PIN_DOUT,
                     .din = I2S_GPIO_UNUSED, .invert_flags = {0}},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));
    const i2s_event_callbacks_t cbs = {.on_sent = on_sent, .on_send_q_ovf = on_q_ovf};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx, &cbs, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    static int32_t chunk[V2_DMA_FRAMES * 2];
    static int32_t cur[V2_SAMPLES];
    const double ring_us = (double)(V2_DMA_DESC - 1) * V2_DMA_FRAMES * V2_T_FRAME_US;
    int64_t P = -1, cur_b = -1, t_prev = 0;
    bool cur_ok = false, prev_ok = true;
    int32_t hold[2] = {0, 0};
    int gain = 0, since_slip = V2_SLIP_GAP;
    for (;;) {
        if (s_reset_req) {
            s_reset_req = false;
            P = -1;
            cur_b = -1;
            gain = 0;
            prev_ok = true;
            taskENTER_CRITICAL(&s_store_mux);
            s_wr_blk = -1;
            taskEXIT_CRITICAL(&s_store_mux);
            taskENTER_CRITICAL(&s_pub_mux);
            s_pub_p = -1;
            taskEXIT_CRITICAL(&s_pub_mux);
        }
        int slip = 0;
        if (P < 0) {
            memset(chunk, 0, sizeof chunk);
            const int64_t teof = t_eof_read();
            if (s_start_ready && teof) {
                const double t_dout = (double)teof + (double)V2_DMA_DESC * V2_DMA_FRAMES * V2_T_FRAME_US;
                P = (int64_t)llround((t_dout - V2_LRX_US - s_omin) / V2_T_FRAME_US);
                if (P < 0) P = 0;
                logf_("PLAYOUT inicio X=%" PRId64 " omin=%.0f alvo=%d\n", P, s_omin, V2_LRX_US);
            }
        } else {
            if (s_env_valid && since_slip >= V2_SLIP_GAP) {
                const int32_t err = (int32_t)lrint(s_envinv_us - s_slips * V2_T_FRAME_US) - V2_LRX_US;
                if (err > (int32_t)(0.6 * V2_T_FRAME_US)) slip = 1;
                else if (err < -(int32_t)(0.6 * V2_T_FRAME_US)) slip = -1;
            }
            since_slip = slip ? 0 : since_slip + 1;
            for (int j = 0; j < V2_DMA_FRAMES; j++) {
                int64_t xx = P + j;
                if (slip > 0 && j >= V2_DMA_FRAMES / 2) xx += 1;
                if (slip < 0 && j > V2_DMA_FRAMES / 2) xx -= 1;
                const int64_t b = xx / ALK_FRAMES_PER_PKT;
                if (b != cur_b) {
                    v2p_slot_t *st = &s_store[b % V2_STORE];
                    const int64_t t_load = esp_timer_get_time();
                    taskENTER_CRITICAL(&s_store_mux);
                    cur_ok = st->ready && st->tag == b + 1;
                    if (cur_ok) {
                        memcpy(cur, st->pcm, sizeof cur);
                        st->ready = 0;
                        st->played = 1;
                    }
                    s_wr_blk = b;
                    taskEXIT_CRITICAL(&s_store_mux);
                    c_played++;
                    if (!cur_ok) {
                        c_conc_blk++;
                        if (prev_ok) c_conc_ev++;  // novo episodio de descontinuidade
                    }
                    prev_ok = cur_ok;
                    cur_b = b;
#if V2_MARK
                    if (((uint64_t)s_base + (uint64_t)b * ALK_FRAMES_PER_PKT) % V2_MARK_EVERY_FRAMES == 0) {
                        const double dout = (double)t_eof_read() + ring_us + (double)(b * ALK_FRAMES_PER_PKT - P) *
                                                                               V2_T_FRAME_US;
                        logf_("L %" PRId64 " %" PRId64 " %.0f %d\n", ((int64_t)s_base / 64) + b * 2, t_load, dout, cur_ok);
                    }
#else
                    (void)t_load;
#endif
                }
                const int o = (int)(xx % ALK_FRAMES_PER_PKT) * 2;
                if (cur_ok) {
                    if (gain < V2_FADE) gain++;
                    hold[0] = cur[o];
                    hold[1] = cur[o + 1];
                } else {
                    if (gain > 0) gain--;
                    c_conc_fr++;
                }
                chunk[2 * j] = (int32_t)(((int64_t)hold[0] * gain) / V2_FADE) & ~0xFF;
                chunk[2 * j + 1] = (int32_t)(((int64_t)hold[1] * gain) / V2_FADE) & ~0xFF;
            }
            if (slip > 0) c_slip_d++;
            if (slip < 0) c_slip_i++;
        }
        size_t wr = 0;
        i2s_channel_write(tx, chunk, sizeof chunk, &wr, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        if (t_prev) {
            const int32_t dt = (int32_t)(now - t_prev);
            if (dt > c_wmax_us) c_wmax_us = dt;
            if (dt > ring_us) c_wslow++;
        }
        t_prev = now;
        if (P >= 0) {
            taskENTER_CRITICAL(&s_pub_mux);
            s_pub_p = P;
            s_pub_t = t_eof_read() + (int64_t)llround(ring_us);
            taskEXIT_CRITICAL(&s_pub_mux);
            P += V2_DMA_FRAMES + slip;
            if (slip) s_slips += slip;
        }
    }
}

#if V2_TELE
static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static volatile bool s_tele_ok;
#endif

static void log_task(void *arg) {
    for (;;) {
#if V2_BTN && V2_PASSIVE
        btn_save_request();
#endif
        size_t n;
        char *item = xRingbufferReceive(s_log, &n, pdMS_TO_TICKS(100));
        if (item) {
#if V2_TELE
            if (s_tele_ok && (item[0] == 'V' || item[0] == 'H' || item[0] == 'S' || item[0] == 'P') &&
                esp_now_send(BCAST, (const uint8_t *)item, n) != ESP_OK)
                c_tele_fail++;
#endif
            fwrite(item, 1, n, stdout);
            vRingbufferReturnItem(s_log, item);
        }
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
#if V2_BTN
#if V2_PASSIVE
    btn_load_record();
#endif
    const gpio_config_t btn_gpio = {.pin_bit_mask = 1ULL << V2_BTN_GPIO, .mode = GPIO_MODE_INPUT,
                                    .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                    .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&btn_gpio));
#endif
    s_log = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_NOSPLIT);
    s_q = xQueueCreate(V2_QLEN, sizeof(rx_frame_t));
    s_store = heap_caps_calloc(V2_STORE, sizeof(v2p_slot_t), MALLOC_CAP_INTERNAL);
    if (!s_log || !s_q || !s_store) {
        ESP_LOGE(TAG, "sem memoria");
        abort();
    }
#if V2_MARK
    const gpio_config_t mk = {.pin_bit_mask = 1ULL << V2_PIN_MARK, .mode = GPIO_MODE_INPUT,
                              .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_POSEDGE};
    ESP_ERROR_CHECK(gpio_config(&mk));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(V2_PIN_MARK, mark_isr, NULL));
#endif
#if V2_LED
    al_led_init(AL_LED_ROLE_RX);  // aqui, no core 0: a interrupcao do RMT fica longe do escritor (core 1)
#endif
    xTaskCreatePinnedToCore(log_task, "v2_log", 6144, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(capture_task, "v2_cap", 8192, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(writer_task, "v2_wr", 6144, NULL, 22, NULL, 1);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
#if V2_PASSIVE
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    const wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(V2_CHANNEL, WIFI_SECOND_CHAN_NONE));
    const char *bwtxt = "promiscuo";
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    wifi_bandwidth_t bw = 0;
    esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    if (bw != WIFI_BW_HT20) abort();
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(V2_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    const char *bwtxt = "HT20";
#endif
#if V2_TELE
    // difusao a MCS4: sem a taxa, a omissao (1 Mb/s) ocuparia o canal ~3 ms por linha; sem ela, sem telemetria
    const esp_now_peer_info_t bp = {.peer_addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, .channel = V2_CHANNEL,
                                    .ifidx = WIFI_IF_STA, .encrypt = false};
    esp_now_rate_config_t rc = {.phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS4_LGI};
    const esp_err_t te = esp_now_add_peer(&bp);
    const esp_err_t tr = te == ESP_OK ? esp_now_set_peer_rate_config(BCAST, &rc) : te;
    s_tele_ok = tr == ESP_OK;
    const char *teletxt = s_tele_ok ? "difusao HT20 MCS4" : esp_err_to_name(tr);
#else
    const char *teletxt = "nao";
#endif
    ESP_LOGI(TAG,
             "V2P RX: canal %d, %s, %s; par %02x:%02x:%02x:%02x:%02x:%02x; saida %s; alvo DOUT-A %d us; anel %dx%d; "
             "audio ligado; marcador %s; LED %s; telemetria %s",
             V2_CHANNEL, V2_PASSIVE ? "ESCUTA PASSIVA (sem ACK)" : "rececao ESP-NOW normal (ACK)", bwtxt, PEER[0],
             PEER[1], PEER[2], PEER[3], PEER[4], PEER[5], V2_OUT_WM8805 ? "WM8805 (MCLK 18, BCLK 16, WS 8, DATA 15)"
                                                                       : "PCM5102A (BCLK 5, WS 6, DIN 7)",
             V2_LRX_US, V2_DMA_DESC, V2_DMA_FRAMES, V2_MARK ? "GPIO12" : "nao", V2_LED ? "GPIO48" : "nao", teletxt);
}
