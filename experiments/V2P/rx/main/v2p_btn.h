// Pure button and passive-pairing logic. The caller serializes access to the pair table.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef V2_BTN_DEBOUNCE_MS
#define V2_BTN_DEBOUNCE_MS 30u
#endif
#ifndef V2_BTN_SHORT_MS
#define V2_BTN_SHORT_MS 1000u
#endif
#ifndef V2_BTN_LONG_MS
#define V2_BTN_LONG_MS 3000u
#endif
#ifndef V2_BTN_VERY_LONG_MS
#define V2_BTN_VERY_LONG_MS 10000u
#endif
#ifndef V2_BTN_PAIR_TIMEOUT_MS
#define V2_BTN_PAIR_TIMEOUT_MS 60000u
#endif
#ifndef V2_BTN_RECENT_MS
#define V2_BTN_RECENT_MS 1000u
#endif
#ifndef V2_BTN_SEEN_MS
#define V2_BTN_SEEN_MS 3000u
#endif
#ifndef V2_BTN_CANDIDATES
#define V2_BTN_CANDIDATES 4u
#endif
#if V2_BTN_CANDIDATES < 1 || V2_BTN_CANDIDATES > 4
#error "V2_BTN_CANDIDATES must be between 1 and 4"
#endif

typedef enum { V2_BTN_NONE, V2_BTN_SHORT, V2_BTN_LONG, V2_BTN_VERY_LONG } v2p_btn_event_t;
typedef struct {
    bool sampled, raw, stable_known, stable, boot_held;
    uint32_t raw_ms, press_ms;
} v2p_btn_t;

static inline void v2p_btn_init(v2p_btn_t *b) {
    memset(b, 0, sizeof *b);
}

// Unsigned subtraction also handles the millisecond counter wrapping during a gesture.
static inline v2p_btn_event_t v2p_btn_sample(v2p_btn_t *b, bool pressed, uint32_t now_ms) {
    if (!b->sampled || pressed != b->raw) {
        b->sampled = true;
        b->raw = pressed;
        b->raw_ms = now_ms;
    }
    if ((uint32_t)(now_ms - b->raw_ms) < V2_BTN_DEBOUNCE_MS) return V2_BTN_NONE;
    if (!b->stable_known) {
        b->stable_known = true;
        b->stable = pressed;
        b->boot_held = pressed;
        return V2_BTN_NONE;
    }
    if (pressed == b->stable) return V2_BTN_NONE;
    b->stable = pressed;
    if (pressed) {
        b->press_ms = now_ms;
        return V2_BTN_NONE;
    }
    if (b->boot_held) {
        b->boot_held = false;
        return V2_BTN_NONE;
    }
    const uint32_t held = now_ms - b->press_ms;
    if (held >= V2_BTN_VERY_LONG_MS) return V2_BTN_VERY_LONG;
    if (held >= V2_BTN_LONG_MS) return V2_BTN_LONG;
    return held < V2_BTN_SHORT_MS ? V2_BTN_SHORT : V2_BTN_NONE;
}

/* Physical confirmation is NOT cryptographic authentication of the TX: passive
 * ESP-NOW has no encryption or verifiable identity, and MACs can be forged.
 * The existing association is never replaced automatically: only explicit
 * CONFIRM in PAIR replaces it, and VERY_LONG clears it.
 */
typedef enum { V2P_PAIR_IDLE, V2P_PAIR_PAIR } v2p_pair_state_t;
typedef enum {
    V2P_BTN_ACTION_NONE, V2P_BTN_RESYNC, V2P_BTN_ENTER_PAIR,
    V2P_BTN_CANCEL, V2P_BTN_CONFIRM, V2P_BTN_CLEAR_RECORD
} v2p_btn_action_t;
typedef struct {
    uint8_t src[6], dst[6];
    uint64_t session_id;
    uint32_t first_ms, last_ms;
} v2p_candidate_t;
typedef struct {
    v2p_pair_state_t state;
    uint32_t start_ms;
    unsigned count;
    bool overflow;
    v2p_candidate_t entries[V2_BTN_CANDIDATES];
} v2p_pair_t;

// Bounded O(4); no allocations, packet counts or RSSI. No eviction hides ambiguity.
static inline void v2p_pair_record(v2p_pair_t *p, const uint8_t src[6], const uint8_t dst[6],
                                   uint64_t session_id, uint32_t now_ms) {
    if (p->state != V2P_PAIR_PAIR) return;
    for (unsigned i = 0; i < p->count; i++) {
        v2p_candidate_t *c = &p->entries[i];
        if (memcmp(c->src, src, 6) || memcmp(c->dst, dst, 6)) continue;
        if (c->session_id != session_id || (uint32_t)(now_ms - c->last_ms) > V2_BTN_RECENT_MS)
            c->first_ms = now_ms;
        c->session_id = session_id;
        c->last_ms = now_ms;
        return;
    }
    if (p->count == V2_BTN_CANDIDATES) {
        p->overflow = true;
        return;
    }
    v2p_candidate_t *c = &p->entries[p->count++];
    memcpy(c->src, src, 6);
    memcpy(c->dst, dst, 6);
    c->session_id = session_id;
    c->first_ms = c->last_ms = now_ms;
}

static inline bool v2p_pair_candidate(const v2p_pair_t *p, uint32_t now_ms, v2p_candidate_t *out) {
    if (p->state != V2P_PAIR_PAIR || p->overflow) return false;
    const v2p_candidate_t *only = NULL;
    for (unsigned i = 0; i < p->count; i++) {
        const v2p_candidate_t *c = &p->entries[i];
        if ((uint32_t)(now_ms - c->last_ms) > V2_BTN_RECENT_MS) continue;
        if (only) return false;
        only = c;
    }
    if (!only || (uint32_t)(only->last_ms - only->first_ms) < V2_BTN_SEEN_MS) return false;
    if (out) *out = *only;
    return true;
}

static inline v2p_btn_action_t v2p_pair_step(v2p_pair_t *p, v2p_btn_event_t event,
                                           uint32_t now_ms, v2p_candidate_t *confirmed) {
    if (event == V2_BTN_VERY_LONG) {
        p->state = V2P_PAIR_IDLE;
        return V2P_BTN_CLEAR_RECORD;
    }
    if (p->state == V2P_PAIR_PAIR && (uint32_t)(now_ms - p->start_ms) >= V2_BTN_PAIR_TIMEOUT_MS) {
        p->state = V2P_PAIR_IDLE;
        return V2P_BTN_CANCEL;
    }
    if (p->state == V2P_PAIR_IDLE) {
        if (event == V2_BTN_SHORT) return V2P_BTN_RESYNC;
        if (event == V2_BTN_LONG) {
            memset(p, 0, sizeof *p);
            p->state = V2P_PAIR_PAIR;
            p->start_ms = now_ms;
            return V2P_BTN_ENTER_PAIR;
        }
    } else {
        if (event == V2_BTN_LONG) {
            p->state = V2P_PAIR_IDLE;
            return V2P_BTN_CANCEL;
        }
        if (event == V2_BTN_SHORT && v2p_pair_candidate(p, now_ms, confirmed)) {
            p->state = V2P_PAIR_IDLE;
            return V2P_BTN_CONFIRM;
        }
    }
    return V2P_BTN_ACTION_NONE;
}
