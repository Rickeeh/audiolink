// Sinais de teste do DAC_TEST com teste de host (tests_host/test_dac_signal.c); sem dependencias do IDF.
#pragma once

#include <math.h>
#include <stdint.h>

// W1/V5 (docs/RX_WM8805_SPDIF_PROPOSAL.md §9.8): amostra de 24 bit = DC de meio LSB de 16 bit + seno de
// amplitude amp (em LSB de 24 bit). Com amp <= 127 fica sempre em [0, 255]: os 16 bit de cima valem sempre 0,
// por isso um caminho truncado a 16 bit entrega silencio constante e so um caminho de 24 bit deixa passar o seno.
#define DAC_LSB24_DC 128

static inline int32_t dac_lsb24_sample(double amp, double phase) {
    return DAC_LSB24_DC + (int32_t)lrint(amp * sin(phase));
}
