// TX v2 - aquisicao I2S do PCM1808: a configuracao validada (audio/main/link_tx.c + audio_common.c, INT160_13), sem
// tuning:
//   - S3 mestre, Philips, slot de 32 bit, estereo (o PCM1808 em slave exige 64 BCK por trama);
//   - PLL_F160M com fs pedida de 48077 Hz -> divisor 13, MCLK 256 fs = 12,3077 MHz, fs real 48 076,923 Hz;
//   - DMA de 8 descritores x 128 tramas. O bloco natural = 1 descritor = 1 pacote = 2,66 ms;
//   - 500 ms descartados depois de ativar (o PCM1808 so da dados validos ~187 ms depois do relogio, mais o fade-in).
// O canal e criado e ativado pela tarefa tx_audio, por isso a interrupcao do DMA fica no core 1.
#pragma once

#include <stdint.h>

#include "alk_proto.h"

#define ACQ_BLOCK_BYTES (ALK_FRAMES_PER_PKT * ALK_CHANNELS * 4u)  // 128 tramas x (L, R) int32 = 1024 B

void acq_start(void);
// Bloqueia ate ter um bloco completo de 128 tramas: o ritmo e o do DMA (relogio do ADC), nunca espera ativa.
// Um timeout ou erro do driver e contado (acq_read_errors) e a leitura continua: o bloco nunca sai incompleto, e a grelha
// de 128 tramas mantem-se.
void acq_read_block(int32_t *dst);
// Descritores DMA perdidos por overflow (contador da interrupcao). Cada evento = 1 bloco de 128 tramas, o mais antigo
// por ler (IDF 5.4.4, i2s_common.c: i2s_dma_rx_callback).
uint32_t acq_overflows(void);
uint32_t acq_read_errors(void);
