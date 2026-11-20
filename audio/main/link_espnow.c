// Wi-Fi + ESP-NOW para o link basico (passo 3). Baseado no bring-up ja
// validado na Fase 1 (bench/main/transport_espnow.c): mesmo init de Wi-Fi
// (STA, canal fixo, sem AP/scan, WIFI_PS_NONE), mesma logica de peer/rate
// fixos. Aqui simplificado para um unico transporte, sem sweep de taxa.
#include "link_espnow.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if CONFIG_AUDIOLINK_WIFI_REINIT
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#endif

static const char *TAG = "link_espnow";

// Quantos esp_now_send() podem estar "no ar" (chamada OK, sem callback ainda) antes do
// TX parar de tentar enviar mais. E o proprio mecanismo de backpressure: em vez de
// continuar a chamar esp_now_send() e a fila interna da stack devolver
// ESP_ERR_ESPNOW_NO_MEM (documentado no header: "you can delay a while before sending
// the next data"), o TX simplesmente NAO tenta este bloco. Valor de partida, nao um
// numero documentado pela Espressif - afinar por medicao (ver AUDIOLINK_LINK_TX_MAX_PENDING).
#define MAX_PENDING CONFIG_AUDIOLINK_LINK_TX_MAX_PENDING

static volatile uint32_t s_api_accept = 0;
static volatile uint32_t s_send_ok = 0;
static volatile uint32_t s_send_fail = 0;
static volatile uint32_t s_api_err_no_mem = 0;
static volatile uint32_t s_api_err_other = 0;
static volatile uint32_t s_backpressure_drop = 0;
static volatile esp_err_t s_last_err = 0;
static uint8_t s_peer_mac[6];

// Fila SPSC (produtor = tarefa TX em link_espnow_tx_send, consumidor = callback do
// ESP-NOW) com o instante de cada esp_now_send() aceite pela API. O seu comprimento
// atual (uxQueueMessagesWaiting) e o "pending" - quantos envios ainda nao tiveram
// callback -, e serve tambem para medir o tempo send()->callback.
static QueueHandle_t s_pending_ts;

// Amostras de latencia send()->callback da janela atual (desde a ultima leitura), para dar
// media/p95/maximo exatos, nao so media+maximo. ~376 envios/s cabem confortavelmente num intervalo
// de 1 s; se a janela crescer mais (leitura atrasada), o excedente e ignorado (nao overflow, so
// perde alguma resolucao do p95 dessa janela). PATCH-OBS-TX-01: so RAW aqui - ordenar e computar
// percentis e feito por quem le (fora do caminho real-time), ver link_espnow_tx_get_lat_samples.
//
// CONCORRENCIA (achado na revisao do PATCH-OBS-TX-01, 2026-10-01): on_send_cb corre na tarefa
// interna Wi-Fi/ESP-NOW (nao e ISR - nao tem IRAM_ATTR, e o ESP-NOW despacha os callbacks de uma
// tarefa); link_espnow_tx_get_lat_samples() corre na tarefa TX. Sao DUAS tarefas diferentes, mesmo
// core (CPU0, como a tarefa main). Antes deste lock, "n = s_lat_n; s_lat_n = 0; memcpy(...)" no
// leitor nao era atomico com "n = s_lat_n; if (n<cap) { s_lat_samples[n]=...; s_lat_n=n+1; }" no
// escritor - uma preempcao do leitor pelo escritor (ou vice-versa) a meio podia fazer o leitor
// copiar amostras obsoletas da janela anterior (indice reescrito por engano) ou perder uma amostra
// nova. Isto e so diagnostico (nunca afetou o audio), mas e uma race real, nao hipotetica - corrigida
// com um spinlock curtissimo (so 2-3 atribuicoes de palavra, ou um memcpy de <4 KB): nunca protege o
// qsort/calculo de percentis, que fica inteiramente em tx_diag_task, fora de qualquer lock.
static portMUX_TYPE s_lat_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_lat_samples[LINK_TX_LAT_SAMPLE_CAP];
static uint32_t s_lat_n = 0;  // so acedido sob s_lat_lock (deixou de ser volatile - o lock e que protege)

#if CONFIG_AUDIOLINK_WIFI_REINIT
// WIFI-REINIT-01: so a tarefa TX escreve estes tres (reinit e send correm na mesma tarefa).
static bool s_link_up = true;
static volatile uint32_t s_reinit_abandoned = 0;
static volatile uint32_t s_link_down_drop = 0;
#endif

#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
// --- TRACE-TX-BOOT-01 (diagnostico DEEP-TX-DEGRADATION-02) ---
// Duas timelines independentes, cada uma com UM produtor e sem locks por evento:
//   SEND = cada oportunidade de envio produzida pelo audio (tarefa TX, link_espnow_tx_send)
//   CB   = cada callback do ESP-NOW (tarefa Wi-Fi, on_send_cb)
// NAO se emparelha SEND<->CB (com varios em voo a FIFO de timestamps nao prova correspondencia).
// Janela: [t0, t0 + WINDOW) com t0 = 1a oportunidade de envio. Buffers em RAM interna reservados
// no init (link_espnow_trace_init); sem fallback para PSRAM. Fecho da janela: cada produtor
// publica o seu "done" UMA vez sob s_tr_lock (barreira) e nunca mais escreve; a tx_diag_task
// (core 1) so le os buffers depois de ver os dois "done" sob o mesmo lock.
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
typedef struct {
    uint32_t t_us;     // esp_timer (us desde o boot)
    uint8_t outcome;   // TR_* abaixo
    uint8_t pending;   // pending lido para a decisao de backpressure (antes do envio)
    uint16_t call_us;  // duracao da chamada esp_now_send() (us, satura 65535); 0 se nao chamou
} trace_send_t;
typedef struct {
    uint32_t t_us;     // esp_timer no inicio do callback
    uint8_t status;    // ESP_NOW_SEND_SUCCESS(0) / FAIL(1)
    uint8_t pad[3];
} trace_cb_t;
_Static_assert(sizeof(trace_send_t) == 8, "trace_send_t tem de ter 8 B");
_Static_assert(sizeof(trace_cb_t) == 8, "trace_cb_t tem de ter 8 B");
enum { TR_ACCEPTED = 0, TR_BACKPRESSURE = 1, TR_NO_MEM = 2, TR_OTHER = 3 };
// capacidade: janela x 376 oportunidades/s x 1,15 de margem
#define TRACE_CAP ((uint32_t)(((uint64_t)CONFIG_AUDIOLINK_TRACE_TX_BOOT_WINDOW_MS * 376u * 115u) / 100000u))
#define TRACE_WINDOW_US ((int64_t)CONFIG_AUDIOLINK_TRACE_TX_BOOT_WINDOW_MS * 1000)
static trace_send_t *s_tr_send;
static trace_cb_t *s_tr_cb;
static uint32_t s_tr_send_n, s_tr_cb_n, s_tr_send_ovf, s_tr_cb_ovf;  // cada um: so o seu produtor escreve
static bool s_tr_enabled, s_tr_started, s_tr_send_done, s_tr_cb_done;
static int64_t s_tr_t0, s_tr_end;  // escritos pela tarefa TX antes do 1o esp_now_send (core 0, como o Wi-Fi)
static int64_t s_tr_t_wifi_start, s_tr_t_espnow_init, s_tr_t_peer_ready;
static size_t s_tr_free_before, s_tr_largest_before, s_tr_free_after, s_tr_largest_after;
static portMUX_TYPE s_tr_lock = portMUX_INITIALIZER_UNLOCKED;

// Chamado a entrada de cada oportunidade; so faz algo na 1a (t0). Fica ANTES do 1o esp_now_send
// para que a callback dessa 1a trama (que pode preemptar a tarefa TX logo a seguir) ja conte.
static inline void trace_start_if_needed(void) {
    if (s_tr_enabled && !s_tr_started) {
        s_tr_t0 = esp_timer_get_time();
        s_tr_end = s_tr_t0 + TRACE_WINDOW_US;
        s_tr_started = true;
    }
}

static inline void trace_send_record(int64_t t, uint8_t outcome, uint32_t pending, int64_t call_us) {
    if (!s_tr_started || s_tr_send_done) {
        return;
    }
    if (t >= s_tr_end) {
        portENTER_CRITICAL(&s_tr_lock);
        s_tr_send_done = true;
        portEXIT_CRITICAL(&s_tr_lock);
        return;
    }
    uint32_t n = s_tr_send_n;
    if (n >= TRACE_CAP) {
        s_tr_send_ovf++;
        return;
    }
    s_tr_send[n].t_us = (uint32_t)t;
    s_tr_send[n].outcome = outcome;
    s_tr_send[n].pending = (uint8_t)pending;
    s_tr_send[n].call_us = (uint16_t)(call_us > 65535 ? 65535 : (call_us < 0 ? 0 : call_us));
    s_tr_send_n = n + 1;
}

static inline void trace_cb_record(int64_t t, uint8_t status) {
    if (!s_tr_started || s_tr_cb_done) {
        return;
    }
    if (t >= s_tr_end) {
        portENTER_CRITICAL(&s_tr_lock);
        s_tr_cb_done = true;
        portEXIT_CRITICAL(&s_tr_lock);
        return;
    }
    uint32_t n = s_tr_cb_n;
    if (n >= TRACE_CAP) {
        s_tr_cb_ovf++;
        return;
    }
    s_tr_cb[n].t_us = (uint32_t)t;
    s_tr_cb[n].status = status;
    s_tr_cb_n = n + 1;
}
#endif

#if CONFIG_AUDIOLINK_LINK_PHYMODE_HT20
#define LINK_PHYMODE WIFI_PHY_MODE_HT20
#else
#define LINK_PHYMODE WIFI_PHY_MODE_11G
#endif

#if CONFIG_AUDIOLINK_LINK_RATE_6M
#define LINK_PHYRATE WIFI_PHY_RATE_6M
#elif CONFIG_AUDIOLINK_LINK_RATE_24M
#define LINK_PHYRATE WIFI_PHY_RATE_24M
#elif CONFIG_AUDIOLINK_LINK_RATE_MCS0_LGI
#define LINK_PHYRATE WIFI_PHY_RATE_MCS0_LGI
#else
#define LINK_PHYRATE WIFI_PHY_RATE_MCS4_LGI
#endif

static void mac_to_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool link_espnow_mac_from_str(const char *s, uint8_t out[6]) {
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

static void nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void wifi_base_init(void) {
    nvs_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    s_tr_t_wifi_start = esp_timer_get_time();  // ancora (fim de esp_wifi_start)
#endif

    // So HT20/20 MHz nesta fase (Fase 1: HT20 >= HT40 no alcance real, +3 dB de sensibilidade); sem canal secundario.
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_AUDIOLINK_LINK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    // Validado no audiocast (H1-D1) e reconfirmado na Fase 1 do AUDIOLINK: power-save
    // do Wi-Fi tem de estar desligado, senao introduz latencia/jitter de dezenas de ms.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    char macstr[18];
    mac_to_str(mac, macstr);
    ESP_LOGI(TAG, "MAC STA deste no: %s (canal %d)", macstr, CONFIG_AUDIOLINK_LINK_WIFI_CHANNEL);
}

static void on_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    const int64_t t_cb = esp_timer_get_time();  // 1 so leitura, partilhada com o bookkeeping existente
#endif
    int64_t t_send;
    if (xQueueReceive(s_pending_ts, &t_send, 0) == pdTRUE) {
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
        uint32_t lat_us = (uint32_t)(t_cb - t_send);
#else
        uint32_t lat_us = (uint32_t)(esp_timer_get_time() - t_send);
#endif
        portENTER_CRITICAL(&s_lat_lock);
        uint32_t n = s_lat_n;
        if (n < LINK_TX_LAT_SAMPLE_CAP) {
            s_lat_samples[n] = lat_us;
            s_lat_n = n + 1;
        }
        portEXIT_CRITICAL(&s_lat_lock);
    }
    // (fila vazia so pode acontecer se link_espnow_tx_send tiver falhado a inserir o
    // timestamp por a fila estar cheia - nesse caso o "pending" ja teria bloqueado o
    // proprio envio antes de chegar aqui, ver comentario em link_espnow_tx_send)
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_send_ok++;
    } else {
        s_send_fail++;
    }
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    trace_cb_record(t_cb, (uint8_t)status);
#endif
}

static void on_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len) {
    int8_t rssi = 0;
    if (info && info->rx_ctrl) {
        rssi = info->rx_ctrl->rssi;
    }
    const uint8_t *src_mac = (info != NULL) ? info->src_addr : NULL;
    link_rx_on_packet(data, (size_t)data_len, rssi, src_mac);
}

void link_espnow_tx_init(void) {
    wifi_base_init();

    s_pending_ts = xQueueCreate(MAX_PENDING, sizeof(int64_t));
    if (!s_pending_ts) {
        abort();
    }

    ESP_ERROR_CHECK(esp_now_init());
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    s_tr_t_espnow_init = esp_timer_get_time();  // ancora (fim de esp_now_init)
#endif
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send_cb));

    uint8_t peer_mac[6];
    if (!link_espnow_mac_from_str(CONFIG_AUDIOLINK_LINK_PEER_MAC, peer_mac)) {
        ESP_LOGE(TAG, "MAC do peer invalido em Kconfig: '%s'", CONFIG_AUDIOLINK_LINK_PEER_MAC);
        abort();
    }
    static const uint8_t zero_mac[6] = {0};
    if (memcmp(peer_mac, zero_mac, 6) == 0) {
        ESP_LOGE(TAG, "MAC do peer nao configurado (00:00:00:00:00:00). "
                      "Le o MAC do RX no log dele (link_espnow: MAC STA deste no) e configura em menuconfig "
                      "(AUDIOLINK_LINK_PEER_MAC) ou no overlay de build.");
        abort();
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, 6);
    peer.channel = CONFIG_AUDIOLINK_LINK_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    esp_now_rate_config_t rate_cfg = {.phymode = LINK_PHYMODE, .rate = LINK_PHYRATE, .ersu = false, .dcm = false};
    esp_err_t err = esp_now_set_peer_rate_config(peer_mac, &rate_cfg);
    if (err != ESP_OK) {
        // Nao degradar em silencio: um TX "a enviar" numa taxa que nao foi aplicada
        // daria resultados de perdas/drift falsos. Ver Fase 1: combinacoes invalidas
        // de phymode/rate/bandwidth falham aqui, nao mais tarde a meio do stream.
        ESP_LOGE(TAG, "esp_now_set_peer_rate_config falhou (%s). A abortar.", esp_err_to_name(err));
        abort();
    }

    memcpy(s_peer_mac, peer_mac, 6);
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    s_tr_t_peer_ready = esp_timer_get_time();  // ancora (add_peer + rate config feitos)
#endif
    char macstr[18];
    mac_to_str(peer_mac, macstr);
    ESP_LOGI(TAG, "ESP-NOW TX pronto, peer=%s phymode=%d rate=%d", macstr, LINK_PHYMODE, LINK_PHYRATE);
}

void link_espnow_tx_send(const uint8_t *pkt, size_t len) {
    // Backpressure: se ja ha MAX_PENDING envios sem callback, NAO tenta mais um - e o
    // proprio esp_now_send() que devolveria ESP_ERR_ESPNOW_NO_MEM neste ponto (a fila
    // interna da stack ja esta cheia). Largar aqui, sem bloquear a tarefa do TX (que tem
    // de continuar a ler o I2S em tempo real), e sem forcar a chamada so para contar
    // outro api_err identico ao anterior.
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    trace_start_if_needed();
    // mesmo valor que decide o backpressure (lido 1x, nao uma chamada extra)
#if CONFIG_AUDIOLINK_WIFI_REINIT
    if (!s_link_up) {
        s_link_down_drop++;
        return;
    }
#endif
    const uint32_t pend = (uint32_t)uxQueueMessagesWaiting(s_pending_ts);
    if (pend >= MAX_PENDING) {
        s_backpressure_drop++;
        trace_send_record(esp_timer_get_time(), TR_BACKPRESSURE, pend, 0);
        return;
    }
#else
#if CONFIG_AUDIOLINK_WIFI_REINIT
    if (!s_link_up) {
        s_link_down_drop++;
        return;
    }
#endif
    if (uxQueueMessagesWaiting(s_pending_ts) >= MAX_PENDING) {
        s_backpressure_drop++;
        return;
    }
#endif

    int64_t t_send = esp_timer_get_time();
    // Espaco garantido pela verificacao acima (unico produtor); nao bloquear na mesma.
    xQueueSend(s_pending_ts, &t_send, 0);

    esp_err_t err = esp_now_send(s_peer_mac, pkt, len);
#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
    trace_send_record(t_send, err == ESP_OK ? TR_ACCEPTED : (err == ESP_ERR_ESPNOW_NO_MEM ? TR_NO_MEM : TR_OTHER),
                      pend, esp_timer_get_time() - t_send);
#endif
    if (err == ESP_OK) {
        s_api_accept++;
    } else {
        s_last_err = err;
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            s_api_err_no_mem++;
        } else {
            s_api_err_other++;
        }
        // A chamada falhou: nunca vai haver callback para este envio - retira o
        // timestamp que acabamos de colocar, senao o "pending" fica inflacionado para sempre.
        int64_t dummy;
        xQueueReceive(s_pending_ts, &dummy, 0);
    }
}

void link_espnow_tx_get_stats(link_tx_stats_t *out) {
    out->api_accept = s_api_accept;
    out->send_ok = s_send_ok;
    out->send_fail = s_send_fail;
    out->api_err_no_mem = s_api_err_no_mem;
    out->api_err_other = s_api_err_other;
    out->last_err = s_last_err;
    out->backpressure_drop = s_backpressure_drop;
    out->pending = (uint32_t)uxQueueMessagesWaiting(s_pending_ts);
#if CONFIG_AUDIOLINK_WIFI_REINIT
    out->reinit_abandoned = s_reinit_abandoned;
    out->link_down_drop = s_link_down_drop;
#endif
}

uint32_t link_espnow_tx_get_lat_samples(uint32_t *out, uint32_t cap) {
    // janela: copia RAW desde a ultima leitura, depois reinicia (mesma semantica de sempre).
    // So copia+reset sob o lock (protege contra on_send_cb a escrever a meio) - nunca ordena aqui
    // (ver comentario junto a s_lat_samples acima); o qsort fica inteiramente fora desta funcao.
    portENTER_CRITICAL(&s_lat_lock);
    uint32_t n = s_lat_n;
    s_lat_n = 0;
    if (n > cap) {
        n = cap;  // defesa (nao deve acontecer com cap >= LINK_TX_LAT_SAMPLE_CAP)
    }
    memcpy(out, s_lat_samples, (size_t)n * sizeof(uint32_t));
    portEXIT_CRITICAL(&s_lat_lock);
    return n;
}

#if CONFIG_AUDIOLINK_WIFI_REINIT
// --- WIFI-REINIT-01 (ver link_espnow.h) ---
enum { RS_SNAPSHOT, RS_WAIT_DRAIN, RS_UNREG_CB, RS_NOW_DEINIT, RS_DRAIN, RS_WIFI_STOP, RS_WIFI_START, RS_SET_BW, RS_SET_CH,
       RS_SET_PS, RS_NOW_INIT, RS_REG_CB, RS_ADD_PEER, RS_RATE, RS_VERIFY };
_Static_assert(RS_VERIFY + 1 == LINK_REINIT_STEPS, "passos do reinit");
static const char *const s_ri_step_names[LINK_REINIT_STEPS] = {
    "snapshot", "wait_drain", "unreg_cb", "now_deinit", "drain", "wifi_stop", "wifi_start", "set_bw",
    "set_ch", "set_ps", "now_init", "reg_cb", "add_peer", "rate", "verify"};

static uint32_t s_ri_n = 0;  // so a tarefa TX
static portMUX_TYPE s_ri_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ri_begun_n = 0;          // sob s_ri_lock
static int64_t s_ri_begun_t = 0;           // sob s_ri_lock
static uint8_t s_ri_begun_op = 0;          // sob s_ri_lock
static link_reinit_result_t s_ri_last;     // sob s_ri_lock

#define RI_STEP(r, i, call)                                                   \
    do {                                                                      \
        const int64_t t_step_ = esp_timer_get_time();                         \
        (r)->err[i] = (call);                                                 \
        (r)->step_us[i] = (uint32_t)(esp_timer_get_time() - t_step_);         \
    } while (0)

// Desmontagem. Nenhum erro interrompe: cada passo e inofensivo se o anterior falhou (ex.: deinit sem init
// devolve ESP_ERR_ESPNOW_NOT_INIT). Fica registado o ultimo codigo de cada passo.
// Quiesce partilhado (REINIT e PAUSE): novos envios ja estao bloqueados (s_link_up=false, e a propria tarefa
// TX esta aqui). Esperar que as callbacks em voo cheguem; se o timeout expirar, regista (ESP_ERR_TIMEOUT,
// drain_ok=0) e segue. first: regista drain_ok/drain_wait_us/pending_after_drain (so a 1a espera).
static void quiesce_wait_drain(link_reinit_result_t *r, bool first) {
    const int64_t t_w = esp_timer_get_time();
    const int64_t deadline = t_w + (int64_t)LINK_REINIT_DRAIN_TIMEOUT_MS * 1000;
    uint32_t left = (uint32_t)uxQueueMessagesWaiting(s_pending_ts);
    while (left > 0 && esp_timer_get_time() < deadline) {
        vTaskDelay(1);
        left = (uint32_t)uxQueueMessagesWaiting(s_pending_ts);
    }
    r->err[RS_WAIT_DRAIN] = (left == 0) ? ESP_OK : ESP_ERR_TIMEOUT;
    r->step_us[RS_WAIT_DRAIN] = (uint32_t)(esp_timer_get_time() - t_w);
    if (first) {
        r->drain_ok = (left == 0);
        r->drain_wait_us = r->step_us[RS_WAIT_DRAIN];
        r->pending_after_drain = left;
    }
}

// Desmontagem so do ESP-NOW (partilhada pelo REINIT completo e pelo ESPNOW-REINIT).
static void espnow_teardown(link_reinit_result_t *r) {
    quiesce_wait_drain(r, r->attempts == 1);
    RI_STEP(r, RS_UNREG_CB, esp_now_unregister_send_cb());
    RI_STEP(r, RS_NOW_DEINIT, esp_now_deinit());
    // Pending: depois do unregister+deinit ja nao ha callbacks. on_send_cb corre na tarefa Wi-Fi (core 0,
    // prioridade acima desta, sem bloquear) e por isso nunca esta a meio enquanto esta tarefa corre no mesmo
    // core. Os envios ainda na fila nunca terao callback: descartados e contados (estado definido = vazio).
    const int64_t t_drain = esp_timer_get_time();
    const uint32_t n = (uint32_t)uxQueueMessagesWaiting(s_pending_ts);
    xQueueReset(s_pending_ts);
    r->abandoned += n;
    s_reinit_abandoned += n;
    r->err[RS_DRAIN] = ESP_OK;
    r->step_us[RS_DRAIN] = (uint32_t)(esp_timer_get_time() - t_drain);
}

static void reinit_teardown(link_reinit_result_t *r) {
    espnow_teardown(r);
    RI_STEP(r, RS_WIFI_STOP, esp_wifi_stop());
}

static bool verify_readback(link_reinit_result_t *r, uint8_t ch, wifi_bandwidth_t bw, wifi_ps_type_t ps);

// Montagem so do ESP-NOW (partilhada) + verificacao por releitura. true = tudo OK e verificado.
static bool espnow_build(link_reinit_result_t *r, uint8_t ch, wifi_bandwidth_t bw, wifi_ps_type_t ps) {
    RI_STEP(r, RS_NOW_INIT, esp_now_init());
    RI_STEP(r, RS_REG_CB, esp_now_register_send_cb(on_send_cb));
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_mac, 6);
    peer.channel = ch;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    RI_STEP(r, RS_ADD_PEER, esp_now_add_peer(&peer));
    esp_now_rate_config_t rate_cfg = {.phymode = LINK_PHYMODE, .rate = LINK_PHYRATE, .ersu = false, .dcm = false};
    RI_STEP(r, RS_RATE, esp_now_set_peer_rate_config(s_peer_mac, &rate_cfg));
    return verify_readback(r, ch, bw, ps);
}

// Montagem completa (inversa da desmontagem): Wi-Fi e depois o mesmo espnow_build.
static bool reinit_build(link_reinit_result_t *r, uint8_t ch, wifi_bandwidth_t bw, wifi_ps_type_t ps) {
    RI_STEP(r, RS_WIFI_START, esp_wifi_start());
    RI_STEP(r, RS_SET_BW, esp_wifi_set_bandwidth(WIFI_IF_STA, bw));
    RI_STEP(r, RS_SET_CH, esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE));
    RI_STEP(r, RS_SET_PS, esp_wifi_set_ps(ps));
    return espnow_build(r, ch, bw, ps);
}

// Verificacao por releitura (so leituras: canal, largura, PS, peer). Partilhada por REINIT e PAUSE.
// true = estado igual ao esperado E nenhum passo com erro.
static bool verify_readback(link_reinit_result_t *r, uint8_t ch, wifi_bandwidth_t bw, wifi_ps_type_t ps) {
    const int64_t t_v = esp_timer_get_time();
    uint8_t c = 0;
    wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE;
    wifi_bandwidth_t b = WIFI_BW_HT20;
    wifi_ps_type_t p = WIFI_PS_NONE;
    esp_now_peer_info_t got = {0};
    const bool c_ok = esp_wifi_get_channel(&c, &sc) == ESP_OK;
    const bool b_ok = esp_wifi_get_bandwidth(WIFI_IF_STA, &b) == ESP_OK;
    const bool p_ok = esp_wifi_get_ps(&p) == ESP_OK;
    r->ch_after = c_ok ? c : 0;
    r->bw_after = b_ok ? (int)b : -1;
    r->ps_after = p_ok ? (int)p : -1;
    r->peer_ok = esp_now_get_peer(s_peer_mac, &got) == ESP_OK && got.channel == ch && got.ifidx == WIFI_IF_STA &&
                 !got.encrypt;
    const bool verified = c_ok && b_ok && p_ok && c == ch && sc == WIFI_SECOND_CHAN_NONE && b == bw && p == ps &&
                          r->peer_ok;
    r->err[RS_VERIFY] = verified ? ESP_OK : ESP_FAIL;
    r->step_us[RS_VERIFY] = (uint32_t)(esp_timer_get_time() - t_v);
    for (int i = RS_WIFI_START; i <= RS_VERIFY; i++) {  // no PAUSE estes passos nao correm (ficam ESP_OK)
        if (r->err[i] != ESP_OK) {
            return false;
        }
    }
    return true;
}

// Inicio partilhado (REINIT e PAUSE): ordinal, marcador BEGIN, heap, pending, bloquear envios e snapshot
// (so leituras) do canal/largura/PS a repor ou a confirmar.
static void quiesce_begin(link_reinit_result_t *r, uint8_t op, uint8_t *ch_out, wifi_bandwidth_t *bw_out,
                          wifi_ps_type_t *ps_out) {
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->n = ++s_ri_n;
    r->t_begin_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_ri_lock);
    s_ri_begun_n = r->n;
    s_ri_begun_t = r->t_begin_us;
    s_ri_begun_op = op;
    portEXIT_CRITICAL(&s_ri_lock);
    r->heap_before = esp_get_free_heap_size();
    r->heap_int_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    r->pending_before = (uint32_t)uxQueueMessagesWaiting(s_pending_ts);
    s_link_up = false;

    // Snapshot do estado a repor; se uma leitura falhar, usa o valor do init (link_espnow_tx_init).
    const int64_t t_s = esp_timer_get_time();
    uint8_t c = 0;
    wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE;
    wifi_bandwidth_t bw = WIFI_BW_HT20;
    wifi_ps_type_t ps = WIFI_PS_NONE;
    const esp_err_t ec = esp_wifi_get_channel(&c, &sc);
    const esp_err_t eb = esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    const esp_err_t ep = esp_wifi_get_ps(&ps);
    const uint8_t ch = (ec == ESP_OK) ? c : (uint8_t)CONFIG_AUDIOLINK_LINK_WIFI_CHANNEL;
    if (eb != ESP_OK) {
        bw = WIFI_BW_HT20;
    }
    if (ep != ESP_OK) {
        ps = WIFI_PS_NONE;
    }
    r->ch_before = (ec == ESP_OK) ? c : 0;
    r->bw_before = (eb == ESP_OK) ? (int)bw : -1;
    r->ps_before = (ep == ESP_OK) ? (int)ps : -1;
    r->err[RS_SNAPSHOT] = (ec != ESP_OK) ? ec : (eb != ESP_OK) ? eb : ep;
    r->step_us[RS_SNAPSHOT] = (uint32_t)(esp_timer_get_time() - t_s);
    *ch_out = ch;
    *bw_out = bw;
    *ps_out = ps;
}

// Fim partilhado: heap, stack, t_end e publicacao do resultado (marcador END).
static void quiesce_end(link_reinit_result_t *r) {
    r->heap_after = esp_get_free_heap_size();
    r->heap_int_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    r->tx_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    r->t_end_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_ri_lock);
    s_ri_last = *r;
    portEXIT_CRITICAL(&s_ri_lock);
}

link_reinit_outcome_t link_espnow_tx_reinit(link_reinit_result_t *r) {
    uint8_t ch;
    wifi_bandwidth_t bw;
    wifi_ps_type_t ps;
    quiesce_begin(r, LINK_OP_REINIT, &ch, &bw, &ps);
    bool up = false;
    for (uint8_t attempt = 1; attempt <= 2 && !up; attempt++) {
        r->attempts = attempt;
        reinit_teardown(r);
        up = reinit_build(r, ch, bw, ps);
    }
    // Falhou duas vezes: sem abort/reboot. O ESP-NOW pode ter ficado meio montado; nada envia
    // (s_link_up=false), quem orquestra ve o TX calado.
    s_link_up = up;
    r->outcome = !up ? LINK_REINIT_FAILED : (r->attempts == 1 ? LINK_REINIT_OK : LINK_REINIT_OK_RETRY);
    quiesce_end(r);
    return r->outcome;
}

// ESPNOW-REINIT-PROBE: o mesmo quiesce e o mesmo ciclo de vida do ESP-NOW do REINIT (unreg_cb -> esp_now_deinit
// -> descartar pending residual -> esp_now_init -> register_send_cb -> add_peer -> taxa -> verificacao por
// releitura), com a mesma 2a tentativa, mas SEM esp_wifi_stop/start e sem setters de canal/largura/PS (o
// snapshot e a verificacao do estado Wi-Fi sao so leituras). Nunca abort/reboot.
link_reinit_outcome_t link_espnow_tx_espnow_reinit(link_reinit_result_t *r) {
    uint8_t ch;
    wifi_bandwidth_t bw;
    wifi_ps_type_t ps;
    quiesce_begin(r, LINK_OP_ESPNOW, &ch, &bw, &ps);
    bool up = false;
    for (uint8_t attempt = 1; attempt <= 2 && !up; attempt++) {
        r->attempts = attempt;
        espnow_teardown(r);
        up = espnow_build(r, ch, bw, ps);
    }
    s_link_up = up;
    r->outcome = !up ? LINK_REINIT_FAILED : (r->attempts == 1 ? LINK_REINIT_OK : LINK_REINIT_OK_RETRY);
    quiesce_end(r);
    return r->outcome;
}

// PAUSE-PROBE: o mesmo quiesce do REINIT (bloquear envios, snapshot, esperar pending == 0 com o mesmo timeout),
// depois manter os envios bloqueados hold_us e reabrir. NAO toca em ESP-NOW, Wi-Fi, canal, largura, PS nem peer
// (so leituras: snapshot e verificacao). O pending que nao drenar no timeout NAO e descartado (o callback
// continua registado e as callbacks chegam). attempts=0 (nenhuma montagem).
link_reinit_outcome_t link_espnow_tx_pause(link_reinit_result_t *r, uint32_t hold_us) {
    uint8_t ch;
    wifi_bandwidth_t bw;
    wifi_ps_type_t ps;
    quiesce_begin(r, LINK_OP_PAUSE, &ch, &bw, &ps);
    quiesce_wait_drain(r, true);
    const int64_t t_h = esp_timer_get_time();
    while (esp_timer_get_time() - t_h < (int64_t)hold_us) {
        vTaskDelay(1);
    }
    r->hold_us = (uint32_t)(esp_timer_get_time() - t_h);
    const bool same = verify_readback(r, ch, bw, ps);
    s_link_up = true;  // reabrir sempre: nada foi alterado; um estado diferente e reportado (FAILED) ao host
    r->outcome = same ? LINK_REINIT_OK : LINK_REINIT_FAILED;
    quiesce_end(r);
    return r->outcome;
}

uint32_t link_espnow_tx_reinit_begun(int64_t *t_begin_us, uint8_t *op) {
    portENTER_CRITICAL(&s_ri_lock);
    const uint32_t n = s_ri_begun_n;
    *t_begin_us = s_ri_begun_t;
    *op = s_ri_begun_op;
    portEXIT_CRITICAL(&s_ri_lock);
    return n;
}

uint32_t link_espnow_tx_reinit_last(link_reinit_result_t *out) {
    portENTER_CRITICAL(&s_ri_lock);
    *out = s_ri_last;
    portEXIT_CRITICAL(&s_ri_lock);
    return out->n;
}

const char *link_espnow_reinit_step_name(int i) {
    return (i >= 0 && i < LINK_REINIT_STEPS) ? s_ri_step_names[i] : "?";
}
#endif

#if CONFIG_AUDIOLINK_TRACE_TX_BOOT
void link_espnow_trace_init(void) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t each = (size_t)TRACE_CAP * sizeof(trace_send_t);
    const size_t margin = (size_t)CONFIG_AUDIOLINK_TRACE_TX_BOOT_MIN_FREE_INTERNAL_KB * 1024;
    s_tr_free_before = heap_caps_get_free_size(caps);
    s_tr_largest_before = heap_caps_get_largest_free_block(caps);
    if (s_tr_largest_before < each || s_tr_free_before < 2 * each + margin) {
        ESP_LOGE(TAG, "TRACE-TX-BOOT-01 DESATIVADO: RAM interna livre=%u maior_bloco=%u, precisa 2x%u + margem %u. "
                      "Sem fallback para PSRAM - reduzir a janela.",
                 (unsigned)s_tr_free_before, (unsigned)s_tr_largest_before, (unsigned)each, (unsigned)margin);
        return;
    }
    s_tr_send = heap_caps_calloc(TRACE_CAP, sizeof(trace_send_t), caps);
    s_tr_cb = heap_caps_calloc(TRACE_CAP, sizeof(trace_cb_t), caps);
    s_tr_free_after = heap_caps_get_free_size(caps);
    s_tr_largest_after = heap_caps_get_largest_free_block(caps);
    if (!s_tr_send || !s_tr_cb || s_tr_free_after < margin) {
        heap_caps_free(s_tr_send);
        heap_caps_free(s_tr_cb);
        s_tr_send = NULL;
        s_tr_cb = NULL;
        ESP_LOGE(TAG, "TRACE-TX-BOOT-01 DESATIVADO: reserva falhou ou margem violada (livre depois=%u, margem %u)",
                 (unsigned)s_tr_free_after, (unsigned)margin);
        return;
    }
    s_tr_enabled = true;
    ESP_LOGI(TAG, "TRACE-TX-BOOT-01 ativo: janela=%d ms desde a 1a oportunidade, cap=%u eventos/tabela (%u B cada), "
                  "RAM interna livre %u -> %u, maior bloco %u -> %u, margem minima %u",
             CONFIG_AUDIOLINK_TRACE_TX_BOOT_WINDOW_MS, (unsigned)TRACE_CAP, (unsigned)each,
             (unsigned)s_tr_free_before, (unsigned)s_tr_free_after, (unsigned)s_tr_largest_before,
             (unsigned)s_tr_largest_after, (unsigned)margin);
}

static size_t trace_b64(const uint8_t *in, size_t n, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= in[i + 2];
        }
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? tbl[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static void trace_dump_table(char tag, const uint8_t *p, size_t len) {
    static char line[192];  // 128 B -> 172 caracteres base64
    uint32_t blk = 0;
    for (size_t off = 0; off < len; off += 128, blk++) {
        const size_t n = (len - off < 128) ? (len - off) : 128;
        const uint32_t crc = esp_rom_crc32_le(0, p + off, (uint32_t)n);
        trace_b64(p + off, n, line);
        printf("TRACE %c %lu %u %08lx %s\n", tag, (unsigned long)blk, (unsigned)n, (unsigned long)crc, line);
    }
}

bool link_espnow_trace_dump(void) {
    static bool disabled_reported = false;
    if (!s_tr_enabled) {
        if (!disabled_reported) {
            printf("TRACE DISABLED free_int_before=%u largest_int_before=%u\n", (unsigned)s_tr_free_before,
                   (unsigned)s_tr_largest_before);
            disabled_reported = true;
        }
        return true;
    }
    portENTER_CRITICAL(&s_tr_lock);  // emparelha com a publicacao do "done" de cada produtor
    const bool send_done = s_tr_send_done, cb_done = s_tr_cb_done;
    const uint32_t ns = s_tr_send_n, nc = s_tr_cb_n, os = s_tr_send_ovf, oc = s_tr_cb_ovf;
    portEXIT_CRITICAL(&s_tr_lock);
    if (!send_done || !cb_done) {
        return false;  // a janela ainda nao fechou nos dois produtores
    }
    printf("TRACE BEGIN v=1 window_ms=%d cap=%u ev_bytes=8 t0_us=%lld t_wifi_start_us=%lld t_espnow_init_us=%lld "
           "t_peer_ready_us=%lld n_send=%lu n_cb=%lu ovf_send=%lu ovf_cb=%lu max_pending=%d free_int_before=%u "
           "largest_int_before=%u free_int_after=%u largest_int_after=%u free_int_min_ever=%u\n",
           CONFIG_AUDIOLINK_TRACE_TX_BOOT_WINDOW_MS, (unsigned)TRACE_CAP, (long long)s_tr_t0,
           (long long)s_tr_t_wifi_start, (long long)s_tr_t_espnow_init, (long long)s_tr_t_peer_ready,
           (unsigned long)ns, (unsigned long)nc, (unsigned long)os, (unsigned long)oc, MAX_PENDING,
           (unsigned)s_tr_free_before, (unsigned)s_tr_largest_before, (unsigned)s_tr_free_after,
           (unsigned)s_tr_largest_after,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    trace_dump_table('S', (const uint8_t *)s_tr_send, (size_t)ns * sizeof(trace_send_t));
    trace_dump_table('C', (const uint8_t *)s_tr_cb, (size_t)nc * sizeof(trace_cb_t));
    printf("TRACE END\n");
    // os dois produtores ja nao tocam nos buffers (done publicado) - devolver a RAM interna
    heap_caps_free(s_tr_send);
    heap_caps_free(s_tr_cb);
    s_tr_send = NULL;
    s_tr_cb = NULL;
    return true;
}
#endif

void link_espnow_rx_init(void) {
    wifi_base_init();
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv_cb));
    ESP_LOGI(TAG, "ESP-NOW RX pronto, a espera de pacotes no canal %d", CONFIG_AUDIOLINK_LINK_WIFI_CHANNEL);
}
