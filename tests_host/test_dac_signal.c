// Sinal LSB24 do DAC_TEST (W1/V5): em todos os niveis permitidos (-97..-120 dBFS) as amostras ficam em [0, 255],
// logo os 16 bit de cima da palavra I2S de 32 bit sao sempre 0 (um caminho de 16 bit entrega silencio), e o seno
// continua presente nos 24 bit com a amplitude pedida. -96 dBFS ja sai do intervalo: confirma o limite do #error.
#include <math.h>
#include <stdint.h>

#include "../audio/main/dac_signal.h"
#include "check.h"

#define FS 48077.0  // INT160_13, como em audio_common.c
#define F0 997.0
#define N 48077     // 1 s

static double amp_for(int dbfs) { return pow(10.0, dbfs / 20.0) * 8388607.0; }  // igual a dac_test.c

// Gera N amostras como dac_test.c (mesma fase e mesmo wrap) e devolve min/max e a amplitude a F0 nos dois caminhos.
static void run(int dbfs, int32_t *mn, int32_t *mx, double *a24, double *a16) {
    const double amp = amp_for(dbfs), dphi = 2.0 * M_PI * F0 / FS;
    double phase = 0.0, re24 = 0, im24 = 0, re16 = 0, im16 = 0;
    *mn = INT32_MAX;
    *mx = INT32_MIN;
    for (int n = 0; n < N; n++) {
        const int32_t s24 = dac_lsb24_sample(amp, phase);
        const int32_t word = (int32_t)((uint32_t)s24 << 8);  // palavra I2S: 24 bit alinhados a esquerda
        const int32_t s16 = word >> 16;                       // o que um recetor de 16 bit le
        phase += dphi;
        if (phase > 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
        if (s24 < *mn) *mn = s24;
        if (s24 > *mx) *mx = s24;
        const double w = 2.0 * M_PI * F0 * n / FS;
        re24 += (s24 - DAC_LSB24_DC) * cos(w);
        im24 += (s24 - DAC_LSB24_DC) * sin(w);
        re16 += s16 * cos(w);
        im16 += s16 * sin(w);
    }
    *a24 = 2.0 * hypot(re24, im24) / N;
    *a16 = 2.0 * hypot(re16, im16) / N;
}

int main(void) {
    for (int db = -97; db >= -120; db--) {
        int32_t mn, mx;
        double a24, a16;
        run(db, &mn, &mx, &a24, &a16);
        CHECK(mn >= 0 && mx <= 255);             // sempre abaixo de 1 LSB de 16 bit
        CHECK(a16 == 0.0);                       // truncado a 16 bit: silencio exato
        CHECK(fabs(a24 - amp_for(db)) < 0.05 * amp_for(db) + 0.5);  // 24 bit: o seno esta la, ao nivel pedido
        CHECK(mx - mn >= 2 * (int32_t)floor(amp_for(db)) - 1);
    }
    int32_t mn, mx;
    double a24, a16;
    run(-96, &mn, &mx, &a24, &a16);
    CHECK(mx > 255);  // -96 dBFS ja atravessa o degrau de 16 bit: o limite -97 do #error e necessario
    return check_done("test_dac_signal");
}
