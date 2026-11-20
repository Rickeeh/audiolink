// Passo 1: validar ADC/I2S no TX.
//
// S3 = mestre I2S (gera MCLK->SCKI, BCK, LRCK a partir da mesma cadeia de divisores, logo
// LRCK e SCKI estao sincronizados, requisito do PCM1808 em slave). PCM1808: MD1=MD0=GND (slave),
// FMT=GND (I2S 24 bit). BCK = 64*fs (slot de 32 bit), como exige o modo slave.
//
// Ciclo: captura N s -> estatisticas -> (opcional) despeja o audio bruto em hex.
// O PCM1808 so entrega dados validos ~8960/fs (~190 ms) apos o inicio do relogio, mais fade-in:
// descarta-se o primeiro 500 ms de cada captura.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "audio_common.h"

static const char *TAG = "adc";

#define DISCARD_MS 500
#define FRAMES_PER_READ 480

typedef struct {
    double sum[2], sumsq[2];
    int32_t peak[2];
    uint32_t clip[2];
    uint64_t n;
} stats_t;

static inline int32_t s24(int32_t w) { return w >> 8; }  // 24 bit alinhados a esquerda em 32

static void stats_reset(stats_t *s) { memset(s, 0, sizeof(*s)); }

static void stats_add(stats_t *s, int ch, int32_t v) {
    s->sum[ch] += v;
    s->sumsq[ch] += (double)v * v;
    int32_t a = v < 0 ? -v : v;
    if (a > s->peak[ch]) {
        s->peak[ch] = a;
    }
    if (a >= 0x7FFFF0) {
        s->clip[ch]++;
    }
}

static double dbfs(double x) { return 20.0 * log10(x > 1e-12 ? x : 1e-12); }

static void stats_print(const stats_t *s, double fs_meas, int t) {
    const double FS = 8388608.0;  // 2^23
    double n = (double)s->n;
    if (n < 1) {
        return;
    }
    printf("ADCSTAT t=%d fs=%.2f", t, fs_meas);
    for (int c = 0; c < 2; c++) {
        double mean = s->sum[c] / n;
        double rms = sqrt(fmax(s->sumsq[c] / n - mean * mean, 0.0));  // RMS sem DC
        // AES17: 0 dBFS = seno de amplitude full-scale, logo o RMS de um seno le o mesmo valor que o seu pico
        printf(" %c_peak=%.1fdBFS %c_rms=%.1fdBFS %c_dc=%.3f%%FS %c_clip=%u", "LR"[c],
               dbfs(s->peak[c] / FS), "LR"[c], dbfs(rms * M_SQRT2 / FS), "LR"[c], 100.0 * mean / FS,
               "LR"[c], (unsigned)s->clip[c]);
    }
    printf("\n");
}

void adc_test_run(void) {
    audio_clk_t clk = audio_clk_get();

    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;
    cc.dma_frame_num = FRAMES_PER_READ;
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

#if CONFIG_AUDIOLINK_ADC_MONITOR
    // Modo monitor: I2S sempre ligado, so estatisticas por segundo (mesmo formato ADCSTAT), nunca pára.
    {
        int32_t *mbuf = heap_caps_malloc(FRAMES_PER_READ * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!mbuf) {
            abort();
        }
        ESP_ERROR_CHECK(i2s_channel_enable(rx));
        ESP_LOGI(TAG, "modo MONITOR: estatisticas por segundo, I2S sempre ligado (o ADC leva ~6 s a assentar)");
        stats_t st;
        stats_reset(&st);
        int64_t t0 = esp_timer_get_time(), next = t0 + 1000000, w0 = t0;
        uint64_t win_frames = 0;
        for (;;) {
            size_t bytes = 0;
            if (i2s_channel_read(rx, mbuf, FRAMES_PER_READ * 8, &bytes, 1000) != ESP_OK) {
                continue;
            }
            uint32_t frames = bytes / 8;
            for (uint32_t i = 0; i < frames; i++) {
                stats_add(&st, 0, s24(mbuf[2 * i]));
                stats_add(&st, 1, s24(mbuf[2 * i + 1]));
                st.n++;
            }
            win_frames += frames;
            int64_t now = esp_timer_get_time();
            if (now >= next) {
                stats_print(&st, win_frames * 1e6 / (double)(now - w0), (int)((now - t0) / 1000000));
                stats_reset(&st);
                win_frames = 0;
                w0 = now;
                next += 1000000;
            }
        }
    }
#endif

    const uint32_t cap_frames = (uint32_t)CONFIG_AUDIOLINK_ADC_CAPTURE_S * clk.sample_rate_hz;
    // 24 bit por amostra guardados em int32: 8 bytes por frame estereo
    int32_t *rec = heap_caps_malloc((size_t)cap_frames * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!rec) {
        ESP_LOGE(TAG, "sem PSRAM para %lu frames", (unsigned long)cap_frames);
        abort();
    }
    int32_t *buf = heap_caps_malloc(FRAMES_PER_READ * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        abort();
    }
    ESP_LOGI(TAG, "pins MCLK=%d BCLK=%d WS=%d DIN=%d; captura %d s = %lu frames (%lu KB em PSRAM)",
             CONFIG_AUDIOLINK_PIN_MCLK, CONFIG_AUDIOLINK_PIN_BCLK, CONFIG_AUDIOLINK_PIN_WS,
             CONFIG_AUDIOLINK_PIN_DATA, CONFIG_AUDIOLINK_ADC_CAPTURE_S, (unsigned long)cap_frames,
             (unsigned long)((size_t)cap_frames * 8 / 1024));

    for (int round = 0;; round++) {
        // (re)inicia o relogio: o PCM1808 faz reset ao parar SCKI e precisa de ~190 ms para dados validos
        ESP_ERROR_CHECK(i2s_channel_enable(rx));
        int64_t t0 = esp_timer_get_time();
        int64_t discard_until = t0 + DISCARD_MS * 1000;
        int64_t next_stat = t0 + 1000000;
        uint32_t got = 0;
        uint64_t frames_total = 0;
        int64_t t_first = 0;
        uint32_t first_batch = 0;
        stats_t st;
        stats_reset(&st);
        uint64_t st_frames0 = 0;
        int64_t st_t0 = 0;

        while (got < cap_frames) {
            size_t bytes = 0;
            esp_err_t e = i2s_channel_read(rx, buf, FRAMES_PER_READ * 8, &bytes, 1000);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "i2s_channel_read: %s (sem BCK/dados?)", esp_err_to_name(e));
                continue;
            }
            int64_t now = esp_timer_get_time();
            uint32_t frames = bytes / 8;
            if (now < discard_until) {
                continue;
            }
            if (!t_first) {
                t_first = now;      // fim da 1.a leitura util: esses frames nao entram na taxa (sem o tempo correspondente)
                st_t0 = now;
                first_batch = frames;
            }
            for (uint32_t i = 0; i < frames && got < cap_frames; i++) {
                int32_t l = s24(buf[2 * i]), r = s24(buf[2 * i + 1]);
                rec[2 * got] = l;
                rec[2 * got + 1] = r;
                got++;
                stats_add(&st, 0, l);
                stats_add(&st, 1, r);
                st.n++;
            }
            frames_total += frames;
            if (now >= next_stat) {
                double fs_meas = (frames_total - st_frames0) * 1e6 / (double)(now - st_t0);
                stats_print(&st, fs_meas, (int)((now - t0) / 1000000));
                stats_reset(&st);
                st_frames0 = frames_total;
                st_t0 = now;
                next_stat += 1000000;
            }
        }
        int64_t t_end = esp_timer_get_time();
        ESP_ERROR_CHECK(i2s_channel_disable(rx));

        // taxa medida vs relogio do S3 (esp_timer): confirma o divisor e mede o desvio face ao nominal
        // todos os frames lidos (incluindo os do ultimo bloco, cujo tempo ja esta em t_end) menos os do 1.o bloco
        double fs_capture = (double)(frames_total - first_batch) * 1e6 / (double)(t_end - t_first);
        ESP_LOGI(TAG, "captura %d: %lu frames, fs medida (esp_timer) = %.3f Hz (pedido %lu)", round,
                 (unsigned long)got, fs_capture, (unsigned long)clk.sample_rate_hz);

#if CONFIG_AUDIOLINK_ADC_DUMP
        printf("ADCDUMP_BEGIN clk=\"%s\" fs_req=%lu fs_meas=%.3f frames=%lu bits=24 ch=2\n", clk.name,
               (unsigned long)clk.sample_rate_hz, fs_capture, (unsigned long)got);
        for (uint32_t i = 0; i < got; i += 16) {
            // ceder o CPU de vez em quando: sem isto o IDLE0 nao corre, o task watchdog dispara e as
            // suas mensagens de erro ficam MISTURADAS nas linhas hex do dump (dump corrompido).
            if (((i / 16) & 31) == 0) {
                vTaskDelay(1);
            }
            uint32_t n = (got - i) < 16 ? (got - i) : 16;
            for (uint32_t k = 0; k < n; k++) {
                printf("%06lx%06lx", (unsigned long)(rec[2 * (i + k)] & 0xFFFFFF),
                       (unsigned long)(rec[2 * (i + k) + 1] & 0xFFFFFF));
            }
            printf("\n");
        }
        printf("ADCDUMP_END\n");
#endif
        vTaskDelay(pdMS_TO_TICKS(CONFIG_AUDIOLINK_ADC_PERIOD_S * 1000));
    }
}
