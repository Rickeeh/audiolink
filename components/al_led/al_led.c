// AudioLink - LED de estado: driver WS2812 por RMT (ver al_led.h para as regras de uso).
#include "al_led.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "al_led";

#define WS2812_RES_HZ 10000000  // 10 MHz: 1 tick = 0,1 us

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static bool s_ok;
static al_led_role_t s_role;
static al_led_fsm_t s_fsm;
static uint8_t s_last[3] = {0xFF, 0xFF, 0xFF};  // forca a 1.a transmissao
static uint8_t s_buf[3];                        // GRB; estatico: valido durante a transmissao (~30 us)

static void show(const uint8_t rgb[3]) {
    if (rgb[0] == s_last[0] && rgb[1] == s_last[1] && rgb[2] == s_last[2]) {
        return;  // so por evento: nada a transmitir se a cor nao mudou
    }
    s_buf[0] = rgb[1];  // WS2812: ordem G, R, B
    s_buf[1] = rgb[0];
    s_buf[2] = rgb[2];
    const rmt_transmit_config_t tc = {.loop_count = 0};
    if (rmt_transmit(s_chan, s_enc, s_buf, sizeof(s_buf), &tc) == ESP_OK) {
        s_last[0] = rgb[0];
        s_last[1] = rgb[1];
        s_last[2] = rgb[2];
    }
}

void al_led_init(al_led_role_t role) {
    s_role = role;
#if CONFIG_ALK_LED_GPIO >= 0
    const rmt_tx_channel_config_t cc = {
        .gpio_num = CONFIG_ALK_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RES_HZ,
        .mem_block_symbols = 48,  // 24 bits num so bloco: sem recarga por interrupcao
        .trans_queue_depth = 2,
    };
    const rmt_bytes_encoder_config_t ec = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},  // 0,3 us alto + 0,9 us baixo
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},  // 0,9 us alto + 0,3 us baixo
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cc, &s_chan) != ESP_OK || rmt_new_bytes_encoder(&ec, &s_enc) != ESP_OK ||
        rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "LED de estado indisponivel (GPIO %d): segue sem LED", CONFIG_ALK_LED_GPIO);
        return;
    }
    s_ok = true;
    ESP_LOGI(TAG, "LED de estado WS2812 no GPIO %d, papel %s, brilho %d", CONFIG_ALK_LED_GPIO,
             role == AL_LED_ROLE_TX ? "TX (ambar)" : "RX (ciano)", CONFIG_ALK_LED_BRIGHTNESS);
    uint8_t rgb[3];
    al_led_rgb(s_role, AL_LED_BOOT, true, CONFIG_ALK_LED_BRIGHTNESS, rgb);
    show(rgb);
#else
    ESP_LOGI(TAG, "LED de estado desativado (CONFIG_ALK_LED_GPIO = -1)");
#endif
}

void al_led_update(al_led_obs_t obs) {
    if (!s_ok) {
        return;
    }
    const al_led_state_t st = al_led_step(&s_fsm, obs);
    uint8_t rgb[3];
    al_led_rgb(s_role, st, s_fsm.blink_on, CONFIG_ALK_LED_BRIGHTNESS, rgb);
    show(rgb);
}
