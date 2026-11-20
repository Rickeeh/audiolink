// AudioLink - LED de estado (WS2812/RGB onboard). So para ser usado pela tarefa de DIAGNOSTICO de cada no
// (TX: tx_diag, core 0; RX: rx_diag, core 1), nunca pela tarefa de audio:
//  - al_led_init() cria o canal RMT na tarefa que a chama, por isso a interrupcao do RMT fica nesse core
//    (o core sem a tarefa de tempo real);
//  - al_led_update() e chamada uma vez por periodo de diagnostico (~1 s) e so transmite quando a cor muda;
//  - sem alocacao depois do init, sem logs depois do init, sem espera (rmt_transmit nao bloqueia).
// Se o GPIO for -1 (Kconfig) ou o init falhar, tudo vira no-op.
#pragma once

#include "al_led_logic.h"

void al_led_init(al_led_role_t role);
void al_led_update(al_led_obs_t obs);
