// TX v2 - aquisicao I2S (ver acq.h).
// E3: os pinos validados do PCM1808 (PHASE2_WIRING.md): MCLK 4, BCLK 5, WS 6, DIN 7. O ADC real alimenta o stream.
// Na E2 eram I2S_GPIO_UNUSED (como o NOPINS-01); esta e a unica diferenca de codigo entre E2 e E3.
#include "acq.h"

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ACQ_SAMPLE_RATE_HZ 48077  // INT160_13: 160 MHz / (13 x 256) = 48 076,923 Hz (o driver arredonda)
#define ACQ_DMA_DESC 8
#define ACQ_DISCARD_US 500000
#define ACQ_READ_TIMEOUT_MS 100  // ~37 blocos; um timeout so acontece se o relogio do I2S parar
#define ACQ_PIN_MCLK GPIO_NUM_4
#define ACQ_PIN_BCLK GPIO_NUM_5
#define ACQ_PIN_WS GPIO_NUM_6
#define ACQ_PIN_DIN GPIO_NUM_7

static const char *TAG = "tx_acq";

static i2s_chan_handle_t s_rx;
static volatile uint32_t s_ovf;       // escrito so pela interrupcao (core 1)
static volatile uint32_t s_read_err;  // escrito so pela tarefa tx_audio

static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_ovf++;
    return false;
}

void acq_start(void) {
    // So no arranque: as linhas de debug do driver mostram o divisor e o DMA reais. Voltam a INFO antes do ciclo RT.
    esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
    esp_log_level_set("i2s_std", ESP_LOG_DEBUG);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = ACQ_DMA_DESC;
    cc.dma_frame_num = ALK_FRAMES_PER_PKT;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &s_rx));

    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = ACQ_SAMPLE_RATE_HZ, .clk_src = I2S_CLK_SRC_PLL_160M, .ext_clk_freq_hz = 0,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = ACQ_PIN_MCLK, .bclk = ACQ_PIN_BCLK, .ws = ACQ_PIN_WS, .dout = I2S_GPIO_UNUSED,
                     .din = ACQ_PIN_DIN},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    const i2s_event_callbacks_t cbs = {.on_recv_q_ovf = on_recv_q_ovf};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx, &cbs, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    esp_log_level_set("i2s_common", ESP_LOG_INFO);
    esp_log_level_set("i2s_std", ESP_LOG_INFO);
    ESP_LOGI(TAG, "I2S: mestre, Philips 32 bit estereo, fs pedida %d Hz (PLL_F160M), DMA %d x %u tramas, pinos MCLK %d BCLK %d WS %d DIN %d",
             ACQ_SAMPLE_RATE_HZ, ACQ_DMA_DESC, ALK_FRAMES_PER_PKT, ACQ_PIN_MCLK, ACQ_PIN_BCLK, ACQ_PIN_WS, ACQ_PIN_DIN);

    static int32_t scratch[ACQ_BLOCK_BYTES / 4];
    const int64_t until = esp_timer_get_time() + ACQ_DISCARD_US;
    while (esp_timer_get_time() < until) {
        acq_read_block(scratch);  // bloqueia no DMA: 500 ms = ~188 blocos descartados
    }
    ESP_LOGI(TAG, "arrancou (descartados os 1.os %d ms depois de ligar o relogio)", ACQ_DISCARD_US / 1000);
}

void acq_read_block(int32_t *dst) {
    size_t got = 0;
    while (got < ACQ_BLOCK_BYTES) {
        size_t n = 0;
        const esp_err_t e = i2s_channel_read(s_rx, (uint8_t *)dst + got, ACQ_BLOCK_BYTES - got, &n, ACQ_READ_TIMEOUT_MS);
        got += n;
        if (e != ESP_OK) {
            s_read_err++;  // so conta: nada de consola nem de recuperacao no caminho RT
        }
    }
}

uint32_t acq_overflows(void) {
    return s_ovf;
}

uint32_t acq_read_errors(void) {
    return s_read_err;
}
