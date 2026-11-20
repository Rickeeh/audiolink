// AUDIOLINK audio bring-up: configuracao de relogio partilhada por ADC e DAC.
#pragma once

#include <stdint.h>
#include "driver/i2s_std.h"
#include "sdkconfig.h"

typedef struct {
    uint32_t sample_rate_hz;   // pedido ao driver
    i2s_clock_src_t clk_src;
    const char *name;
} audio_clk_t;

// GPIO de MCLK do canal de saida (DAC/LINK_RX): so emitido para o WM8805 (ver AUDIOLINK_DAC_MCLK_OUT).
#if CONFIG_AUDIOLINK_DAC_MCLK_OUT
#define AUDIOLINK_DAC_GPIO_MCLK CONFIG_AUDIOLINK_PIN_MCLK
#else
#define AUDIOLINK_DAC_GPIO_MCLK I2S_GPIO_UNUSED
#endif

// Devolve a configuracao de relogio escolhida em menuconfig.
audio_clk_t audio_clk_get(void);

// Liga o log de debug do driver I2S: mostra o divisor real (inteiro + fracao) escolhido.
void audio_enable_i2s_debug_log(void);

void adc_test_run(void);  // nunca retorna
void dac_test_run(void);  // nunca retorna
void link_tx_run(void);   // passo 3: PCM1808 -> ESP-NOW; nunca retorna
void link_rx_run(void);   // passo 3: ESP-NOW -> PCM5102A; nunca retorna
