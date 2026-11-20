#include "esp_log.h"
#include "audio_common.h"

static const char *TAG = "audio";

void app_main(void) {
    audio_clk_t c = audio_clk_get();
    ESP_LOGI(TAG, "AUDIOLINK audio bring-up, relogio: %s", c.name);
    audio_enable_i2s_debug_log();
#if CONFIG_AUDIOLINK_AUDIO_ROLE_ADC
    ESP_LOGI(TAG, "papel: ADC_TEST (S3 + PCM1808, S3 mestre I2S, PCM1808 em slave)");
    adc_test_run();
#elif CONFIG_AUDIOLINK_AUDIO_ROLE_DAC
    ESP_LOGI(TAG, "papel: DAC_TEST (S3 + PCM5102A, S3 mestre I2S, PLL do DAC a partir de BCK)");
    dac_test_run();
#elif CONFIG_AUDIOLINK_AUDIO_ROLE_LINK_TX
    ESP_LOGI(TAG, "papel: LINK_TX passo 3 (S3 + PCM1808 -> ESP-NOW)");
    link_tx_run();
#else
    ESP_LOGI(TAG, "papel: LINK_RX passo 3 (ESP-NOW -> S3 + PCM5102A)");
    link_rx_run();
#endif
}
