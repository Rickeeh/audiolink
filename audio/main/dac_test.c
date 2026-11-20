// Passo 2: validar DAC/I2S no RX.
//
// S3 = mestre I2S: BCK, LRCK, DOUT. PCM5102A em modo PLL sem SCK (SCK a GND): a PLL interna gera
// o relogio de sistema a partir de BCK (BCK = 64*fs, slot de 32 bit). FMT=GND (I2S), FLT=GND,
// DEMP=GND. XSMT: mantido em mute enquanto o I2S arranca e solto (soft un-mute, 104 amostras)
// depois de a PLL do DAC estabilizar.
//
// W1 (WM8805 -> S/PDIF): com AUDIOLINK_DAC_MCLK_OUT o S3 emite tambem MCLK = 256*fs, que relogia o
// transmissor S/PDIF do WM8805 (escravo). O sinal LSB24 verifica se o caminho preserva 24 bit (V5).
#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "audio_common.h"
#include "dac_signal.h"

#if CONFIG_AUDIOLINK_DAC_SIGNAL_LSB24
#if CONFIG_AUDIOLINK_DAC_LEVEL_DBFS > -97
#error "DAC_SIGNAL_LSB24 exige AUDIOLINK_DAC_LEVEL_DBFS <= -97 (amplitude <= 127 LSB de 24 bit)"
#endif
#define DAC_SIGNAL_NOTE " sobre DC de 128 LSB24 (teste de 24 bit, 2 canais)"
#else
#define DAC_SIGNAL_NOTE ""
#endif

static const char *TAG = "dac";

#define FRAMES 480

void dac_test_run(void) {
    audio_clk_t clk = audio_clk_get();

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;
    cc.dma_frame_num = FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx, NULL));

    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = clk.sample_rate_hz, .clk_src = clk.clk_src,
                    .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        // MCLK so com AUDIOLINK_DAC_MCLK_OUT (WM8805); no PCM5102A o SCK fica a GND (modo PLL a partir de BCK)
        .gpio_cfg = {.mclk = AUDIOLINK_DAC_GPIO_MCLK, .bclk = CONFIG_AUDIOLINK_PIN_BCLK,
                     .ws = CONFIG_AUDIOLINK_PIN_WS, .dout = CONFIG_AUDIOLINK_PIN_DATA,
                     .din = I2S_GPIO_UNUSED},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));

#if CONFIG_AUDIOLINK_PIN_XSMT < 0
    ESP_LOGW(TAG, "XSMT NAO comandado pelo S3: mute/un-mute depende da ponte H3L da placa (ver docs/PHASE2_WIRING.md)");
#endif
#if CONFIG_AUDIOLINK_PIN_XSMT >= 0
    gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_AUDIOLINK_PIN_XSMT, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(CONFIG_AUDIOLINK_PIN_XSMT, 0);  // mute enquanto o relogio arranca
#endif

    static int32_t buf[FRAMES * 2];
    const double amp = pow(10.0, CONFIG_AUDIOLINK_DAC_LEVEL_DBFS / 20.0) * 8388607.0;  // 24 bit
    const double dphi = 2.0 * M_PI * CONFIG_AUDIOLINK_DAC_FREQ_HZ / (double)clk.sample_rate_hz;
    double phase = 0.0;
    uint64_t frame_idx = 0;
    bool unmuted = false;

    // pre-carrega silencio para o DMA antes de habilitar
    for (int i = 0; i < FRAMES * 2; i++) {
        buf[i] = 0;
    }
    size_t w = 0;
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx, buf, sizeof(buf), &w));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    ESP_LOGI(TAG, "I2S a correr: MCLK=%d BCLK=%d WS=%d DOUT=%d XSMT=%d, %s, seno %d Hz a %d dBFS%s",
             (int)AUDIOLINK_DAC_GPIO_MCLK, CONFIG_AUDIOLINK_PIN_BCLK, CONFIG_AUDIOLINK_PIN_WS,
             CONFIG_AUDIOLINK_PIN_DATA, CONFIG_AUDIOLINK_PIN_XSMT, clk.name, CONFIG_AUDIOLINK_DAC_FREQ_HZ,
             CONFIG_AUDIOLINK_DAC_LEVEL_DBFS, DAC_SIGNAL_NOTE);

    uint64_t last_log = 0;
    for (;;) {
        int64_t sec = frame_idx / clk.sample_rate_hz;
        (void)sec;  // so usado no modo ALT
        for (int i = 0; i < FRAMES; i++) {
            int32_t v = 0;
#if !CONFIG_AUDIOLINK_DAC_SIGNAL_SILENCE
#if CONFIG_AUDIOLINK_DAC_SIGNAL_LSB24
            v = dac_lsb24_sample(amp, phase) << 8;  // DC + seno, sempre abaixo de 1 LSB de 16 bit
#else
            v = (int32_t)lrint(amp * sin(phase)) << 8;  // 24 bit alinhados a esquerda em 32
#endif
            phase += dphi;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
#endif
            int32_t l = v, r = v;
#if CONFIG_AUDIOLINK_DAC_SIGNAL_ALT
            if (sec & 1) {
                l = 0;   // segundos impares: so o direito
            } else {
                r = 0;   // segundos pares: so o esquerdo
            }
#endif
            buf[2 * i] = l;
            buf[2 * i + 1] = r;
            frame_idx++;
        }
        size_t written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(tx, buf, sizeof(buf), &written, 1000));

        if (!unmuted && frame_idx > (uint64_t)clk.sample_rate_hz / 2) {  // ~0,5 s de BCK/LRCK estaveis
#if CONFIG_AUDIOLINK_PIN_XSMT >= 0
            gpio_set_level(CONFIG_AUDIOLINK_PIN_XSMT, 1);
            ESP_LOGI(TAG, "XSMT alto (comandado pelo S3): un-mute");
#else
            ESP_LOGI(TAG, "BCK/LRCK estaveis ha ~0,5 s (XSMT nao comandado: estado dado pela ponte H3L)");
#endif
            unmuted = true;
        }
        if (frame_idx - last_log >= (uint64_t)clk.sample_rate_hz * 10) {
            last_log = frame_idx;
            ESP_LOGI(TAG, "a emitir (%llu s)", (unsigned long long)(frame_idx / clk.sample_rate_hz));
        }
    }
}
