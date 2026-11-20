#include "audio_common.h"
#include "esp_log.h"

audio_clk_t audio_clk_get(void) {
#if CONFIG_AUDIOLINK_CLK_INT240_20
    // 240e6 / (256 * 20) = 46875 Hz exatos: divisor inteiro, MCLK = 12,000 MHz.
    return (audio_clk_t){.sample_rate_hz = 46875, .clk_src = I2S_CLK_SRC_PLL_240M,
                         .name = "PLL240/20 -> 46875 Hz (inteiro)"};
#elif CONFIG_AUDIOLINK_CLK_INT160_13
    // 160e6 / (256 * 13) = 48076,9 Hz (nao e inteiro em Hz): o driver arredonda; ver log de debug.
    return (audio_clk_t){.sample_rate_hz = 48077, .clk_src = I2S_CLK_SRC_PLL_160M,
                         .name = "PLL160/13 -> ~48077 Hz"};
#else
    return (audio_clk_t){.sample_rate_hz = 48000, .clk_src = I2S_CLK_SRC_DEFAULT,
                         .name = "48000 Hz nominais (possivelmente fracionario)"};
#endif
}

void audio_enable_i2s_debug_log(void) {
    esp_log_level_set("i2s_std", ESP_LOG_DEBUG);
    esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
}
