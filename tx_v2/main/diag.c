// TX v2 - diagnostico (ver diag.h). Os nomes de campo da linha LINKTX sao os que os avaliadores congelados ja leem
// (t, adc_blocks, sample_ctr, api_accept, send_ok, cb_lat_avg_us, adc_ovf, i2s_read_err, chan).
#include "diag.h"

#include <stdbool.h>

#include "acq.h"
#include "al_led.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espnow_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tx_diag";

typedef struct {
    bool started;
    int64_t t0_us;
    uint64_t session_id;
    uint32_t blocks;
    uint64_t sample_ctr;
} diag_pub_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static diag_pub_t s_pub;

void diag_start(uint64_t session_id, int64_t t0_us) {
    portENTER_CRITICAL(&s_mux);
    s_pub.session_id = session_id;
    s_pub.t0_us = t0_us;
    s_pub.started = true;
    portEXIT_CRITICAL(&s_mux);
}

void diag_publish(uint32_t blocks, uint64_t sample_ctr) {
    portENTER_CRITICAL(&s_mux);
    s_pub.blocks = blocks;
    s_pub.sample_ctr = sample_ctr;
    portEXIT_CRITICAL(&s_mux);
}

void diag_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    uint32_t prev_done = 0, prev_lat_sum = 0;
    // LED de estado: criado aqui (core 0, fora da tarefa de audio) para a interrupcao do RMT ficar neste core.
    al_led_init(AL_LED_ROLE_TX);
    uint32_t led_prev_ok = 0, led_prev_err = 0;
    bool led_first = true;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        portENTER_CRITICAL(&s_mux);
        const diag_pub_t p = s_pub;
        portEXIT_CRITICAL(&s_mux);
        if (!p.started) {
            al_led_update((al_led_obs_t){.booting = true, .error_now = false, .peer_ok = false});
            continue;
        }
        tx_ledger_snap_t st;
        espnow_tx_read(&st);
        // LED: erro real = perda de dados na aquisicao (overflow do DMA ou erro de leitura I2S) nesta janela;
        // peer = houve ACKs (send_ok) nesta janela. A 1.a janela so serve de referencia (BOOT).
        const uint32_t led_err = acq_overflows() + acq_read_errors();
        al_led_update((al_led_obs_t){.booting = led_first, .error_now = !led_first && led_err != led_prev_err,
                                     .peer_ok = !led_first && st.ok != led_prev_ok});
        led_prev_ok = st.ok;
        led_prev_err = led_err;
        led_first = false;
        const uint32_t done = st.ok + st.fail;
        const uint32_t n = done - prev_done;
        const uint32_t lat_avg = n ? (st.lat_sum_us - prev_lat_sum) / n : 0;
        prev_done = done;
        prev_lat_sum = st.lat_sum_us;
        uint8_t chan = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&chan, &sec);
        const int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG,
                 "LINKTX t=%ld adc_blocks=%lu sample_ctr=%llu tx_attempt=%lu api_accept=%lu api_reject=%lu last_err=0x%lx "
                 "send_ok=%lu send_fail=%lu pending=%lu pending_max=%lu cb_lat_avg_us=%lu "
                 "cb_lat_max_us=%lu adc_ovf=%lu i2s_read_err=%lu chan=%u heap_min=%lu session_id=%016llX uptime_s=%lld",
                 (long)((now - p.t0_us) / 1000000), (unsigned long)p.blocks, (unsigned long long)p.sample_ctr,
                 (unsigned long)(st.accept + st.reject), (unsigned long)st.accept, (unsigned long)st.reject,
                 (unsigned long)(uint32_t)st.last_err, (unsigned long)st.ok,
                 (unsigned long)st.fail, (unsigned long)st.pending, (unsigned long)st.pend_max, (unsigned long)lat_avg,
                 (unsigned long)st.lat_max_us, (unsigned long)acq_overflows(), (unsigned long)acq_read_errors(),
                 (unsigned)chan, (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long long)p.session_id,
                 (long long)(now / 1000000));
    }
}
