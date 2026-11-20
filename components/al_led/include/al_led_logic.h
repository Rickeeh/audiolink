// AudioLink - LED de estado: logica pura (sem hardware), partilhada por TX e RX e testada no host.
//
// Convencao (docs/LED_STATUS.md):
//   TX normal = ambar fixo; RX normal = ciano fixo; sem peer = cor do papel a piscar;
//   erro real = vermelho (so erro); arranque = branco curto; PAIR/TUNE (futuro) = roxo.
// Prioridade: ERROR > BOOT > NO_PEER > RUNNING.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { AL_LED_ROLE_TX = 0, AL_LED_ROLE_RX = 1 } al_led_role_t;

typedef enum {
    AL_LED_OFF = 0,
    AL_LED_BOOT,
    AL_LED_RUNNING,
    AL_LED_NO_PEER,
    AL_LED_ERROR,
    AL_LED_PAIR,  // reservado (PAIR/TUNE futuro); nao usado na Beta
} al_led_state_t;

// Vermelho mantem-se este numero de periodos de diagnostico (~1 s cada) depois do ultimo erro observado.
#define AL_LED_ERROR_HOLD_TICKS 5

typedef struct {
    uint8_t err_hold;   // periodos restantes de ERROR
    uint8_t blink_on;   // fase do piscar (NO_PEER)
} al_led_fsm_t;

typedef struct {
    bool booting;   // ainda sem a 1.a janela de diagnostico completa
    bool error_now; // erro real observado nesta janela
    bool peer_ok;   // houve trafego do peer nesta janela
} al_led_obs_t;

// Um passo por periodo de diagnostico. Devolve o estado a mostrar.
static inline al_led_state_t al_led_step(al_led_fsm_t *f, al_led_obs_t o) {
    if (o.error_now) {
        f->err_hold = AL_LED_ERROR_HOLD_TICKS;
    }
    al_led_state_t s;
    if (f->err_hold > 0) {
        f->err_hold--;
        s = AL_LED_ERROR;
    } else if (o.booting) {
        s = AL_LED_BOOT;
    } else if (!o.peer_ok) {
        s = AL_LED_NO_PEER;
    } else {
        s = AL_LED_RUNNING;
    }
    f->blink_on = (s == AL_LED_NO_PEER) ? (uint8_t)!f->blink_on : 1;
    return s;
}

// Cor (0..255 por canal) para papel/estado/fase do piscar, escalada por 'bright' (1..255).
// Nunca vermelho para identificar papel; nunca verde como cor principal; branco so no BOOT.
static inline void al_led_rgb(al_led_role_t role, al_led_state_t s, bool blink_on, uint8_t bright, uint8_t rgb[3]) {
    static const uint8_t AMBER[3] = {255, 96, 0};
    static const uint8_t CYAN[3] = {0, 160, 255};
    static const uint8_t RED[3] = {255, 0, 0};
    static const uint8_t WHITE[3] = {160, 160, 160};
    static const uint8_t PURPLE[3] = {150, 0, 255};
    static const uint8_t OFF[3] = {0, 0, 0};
    const uint8_t *role_c = role == AL_LED_ROLE_TX ? AMBER : CYAN;
    const uint8_t *c;
    switch (s) {
        case AL_LED_BOOT: c = WHITE; break;
        case AL_LED_RUNNING: c = role_c; break;
        case AL_LED_NO_PEER: c = blink_on ? role_c : OFF; break;
        case AL_LED_ERROR: c = RED; break;
        case AL_LED_PAIR: c = PURPLE; break;
        default: c = OFF; break;
    }
    for (int i = 0; i < 3; i++) {
        rgb[i] = (uint8_t)(((uint16_t)c[i] * bright + 127) / 255);
    }
}
