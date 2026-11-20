// LED de estado: logica pura (al_led_logic.h). Prioridade ERROR > BOOT > NO_PEER > RUNNING, retencao do vermelho,
// piscar so em NO_PEER, e as regras de cor (vermelho so erro, branco so arranque, verde nunca cor principal).
#include <stdbool.h>
#include <string.h>

#include "al_led_logic.h"
#include "check.h"

static al_led_obs_t obs(bool boot, bool err, bool peer) {
    return (al_led_obs_t){.booting = boot, .error_now = err, .peer_ok = peer};
}

static void test_priority(void) {
    al_led_fsm_t f = {0};
    CHECK(al_led_step(&f, obs(true, false, false)) == AL_LED_BOOT);
    CHECK(al_led_step(&f, obs(true, true, true)) == AL_LED_ERROR);  // erro ganha ao arranque
    al_led_fsm_t g = {0};
    CHECK(al_led_step(&g, obs(false, false, false)) == AL_LED_NO_PEER);
    CHECK(al_led_step(&g, obs(false, false, true)) == AL_LED_RUNNING);
    CHECK(al_led_step(&g, obs(false, true, true)) == AL_LED_ERROR);  // erro ganha ao normal
}

static void test_error_hold(void) {
    al_led_fsm_t f = {0};
    CHECK(al_led_step(&f, obs(false, true, true)) == AL_LED_ERROR);
    for (int i = 1; i < AL_LED_ERROR_HOLD_TICKS; i++) {
        CHECK(al_led_step(&f, obs(false, false, true)) == AL_LED_ERROR);  // mantem-se depois do erro
    }
    CHECK(al_led_step(&f, obs(false, false, true)) == AL_LED_RUNNING);  // e volta ao normal
    CHECK(al_led_step(&f, obs(false, true, true)) == AL_LED_ERROR);     // novo erro rearma
}

static void test_blink_only_without_peer(void) {
    al_led_fsm_t f = {0};
    uint8_t ph[4];
    for (int i = 0; i < 4; i++) {
        CHECK(al_led_step(&f, obs(false, false, false)) == AL_LED_NO_PEER);
        ph[i] = f.blink_on;
    }
    CHECK(ph[0] != ph[1] && ph[1] != ph[2] && ph[2] != ph[3]);  // alterna a cada periodo
    CHECK(al_led_step(&f, obs(false, false, true)) == AL_LED_RUNNING);
    CHECK(f.blink_on == 1);  // fixo quando ha peer
}

static void test_colors(void) {
    uint8_t c[3];
    for (int role = 0; role < 2; role++) {
        al_led_rgb((al_led_role_t)role, AL_LED_RUNNING, true, 255, c);
        CHECK(!(c[0] > 0 && c[1] == 0 && c[2] == 0));  // papel nunca vermelho
        CHECK(!(c[1] > c[0] && c[1] > c[2]));           // verde nunca dominante
        CHECK(!(c[0] == c[1] && c[1] == c[2]));         // papel nunca branco
        al_led_rgb((al_led_role_t)role, AL_LED_NO_PEER, false, 255, c);
        CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0);     // fase apagada do piscar
        al_led_rgb((al_led_role_t)role, AL_LED_ERROR, true, 255, c);
        CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0);   // erro = vermelho, igual nos dois papeis
    }
    uint8_t tx[3], rx[3];
    al_led_rgb(AL_LED_ROLE_TX, AL_LED_RUNNING, true, 255, tx);
    al_led_rgb(AL_LED_ROLE_RX, AL_LED_RUNNING, true, 255, rx);
    CHECK(tx[0] > tx[2] && tx[1] > 0 && tx[2] == 0);  // TX ambar (R alto, G medio, sem B)
    CHECK(rx[2] > rx[0] && rx[0] == 0);                // RX ciano/azul (B alto, sem R)
    CHECK(memcmp(tx, rx, 3) != 0);                     // distinguiveis
    al_led_rgb(AL_LED_ROLE_TX, AL_LED_BOOT, true, 255, c);
    CHECK(c[0] == c[1] && c[1] == c[2] && c[0] > 0);   // arranque = branco
    al_led_rgb(AL_LED_ROLE_RX, AL_LED_PAIR, true, 255, c);
    CHECK(c[0] > 0 && c[2] > 0 && c[1] == 0);          // PAIR/TUNE (reservado) = roxo
    al_led_rgb(AL_LED_ROLE_TX, AL_LED_RUNNING, true, 24, c);
    CHECK(c[0] == 24 && c[2] == 0);                     // brilho escala (255 -> 24)
    al_led_rgb(AL_LED_ROLE_TX, AL_LED_RUNNING, true, 1, c);
    CHECK(c[0] == 1);                                   // brilho minimo continua visivel no canal principal
}

int main(void) {
    test_priority();
    test_error_hold();
    test_blink_only_without_peer();
    test_colors();
    return check_done("test_al_led");
}
