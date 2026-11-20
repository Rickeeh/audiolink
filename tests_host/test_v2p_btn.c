#include <string.h>

#include "../experiments/V2P/rx/main/v2p_btn.h"
#include "check.h"

static const uint8_t TX[6] = {0x28, 0x84, 0x85, 0x64, 0x1d, 0xec};
static const uint8_t DST[6] = {0xac, 0x27, 0x6e, 0xa5, 0x4e, 0x10};

static void released(v2p_btn_t *b, uint32_t start) {
    v2p_btn_init(b);
    CHECK(v2p_btn_sample(b, false, start) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(b, false, start + 30) == V2_BTN_NONE);
}

static void contract_thresholds_and_wrap(void) {
    // Durations are between the stable edges, which are 30 ms after raw edges.
    static const struct { uint32_t held; v2p_btn_event_t event; } cases[] = {
        {30, V2_BTN_SHORT}, {998, V2_BTN_SHORT}, {999, V2_BTN_SHORT},
        {1000, V2_BTN_NONE}, {1001, V2_BTN_NONE}, {2000, V2_BTN_NONE}, {2999, V2_BTN_NONE},
        {3000, V2_BTN_LONG}, {3001, V2_BTN_LONG}, {9999, V2_BTN_LONG},
        {10000, V2_BTN_VERY_LONG}, {10001, V2_BTN_VERY_LONG}
    };
    const uint32_t starts[] = {0, UINT32_MAX - 5000, UINT32_MAX - 115};
    for (unsigned j = 0; j < sizeof starts / sizeof starts[0]; j++) {
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const uint32_t t = starts[j];
            v2p_btn_t b;
            released(&b, t);
            CHECK(v2p_btn_sample(&b, true, t + 100) == V2_BTN_NONE);
            CHECK(v2p_btn_sample(&b, true, t + 129) == V2_BTN_NONE);
            CHECK(v2p_btn_sample(&b, true, t + 130) == V2_BTN_NONE);
            CHECK(v2p_btn_sample(&b, true, t + 100 + cases[i].held) == V2_BTN_NONE);
            CHECK(v2p_btn_sample(&b, false, t + 100 + cases[i].held) == V2_BTN_NONE);
            CHECK(v2p_btn_sample(&b, false, t + 129 + cases[i].held) == V2_BTN_NONE);
            CHECK(v2p_btn_sample(&b, false, t + 130 + cases[i].held) == cases[i].event);
            CHECK(v2p_btn_sample(&b, false, t + 131 + cases[i].held) == V2_BTN_NONE);
        }
    }
}

static void contract_debounce_and_boot_guard(void) {
    v2p_btn_t b;
    released(&b, 0);
    CHECK(v2p_btn_sample(&b, true, 100) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 129) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 129) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 200) == V2_BTN_NONE);
    CHECK(!b.stable);
    CHECK(v2p_btn_sample(&b, true, 300) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 330) == V2_BTN_NONE);
    CHECK(b.stable);
    // A short release bounce does not split the held press.
    CHECK(v2p_btn_sample(&b, false, 400) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 429) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 459) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 3300) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 3330) == V2_BTN_LONG);

    v2p_btn_init(&b);
    CHECK(v2p_btn_sample(&b, true, 0) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 29) == V2_BTN_NONE);
    CHECK(!b.stable_known);
    CHECK(v2p_btn_sample(&b, true, 30) == V2_BTN_NONE);
    CHECK(b.boot_held);
    CHECK(v2p_btn_sample(&b, true, 20000) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 20001) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 20020) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 20030) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 20059) == V2_BTN_NONE);
    CHECK(b.boot_held);
    CHECK(v2p_btn_sample(&b, false, 20060) == V2_BTN_NONE);
    CHECK(!b.boot_held);
    CHECK(v2p_btn_sample(&b, true, 20100) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, true, 20130) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 20200) == V2_BTN_NONE);
    CHECK(v2p_btn_sample(&b, false, 20230) == V2_BTN_SHORT);
}

static void enter(v2p_pair_t *p, uint32_t now) {
    CHECK(v2p_pair_step(p, V2_BTN_LONG, now, NULL) == V2P_BTN_ENTER_PAIR);
    CHECK(p->state == V2P_PAIR_PAIR);
    CHECK(!p->count && !p->overflow);
}

static void seen(v2p_pair_t *p, uint32_t start, uint64_t session) {
    for (unsigned i = 0; i <= 3; i++) v2p_pair_record(p, TX, DST, session, start + i * 1000);
}

// Added by the reviewer: mutation M3 (accept the last of two recent candidates) survived the original tests,
// because their second candidate was never mature. Two mature, simultaneous sources must never confirm.
static void contract_two_mature_candidates_never_confirm(void) {
    uint8_t other[6];
    memcpy(other, TX, 6);
    other[5]++;
    for (int order = 0; order < 2; order++) {
        v2p_pair_t p = {0};
        v2p_candidate_t out = {0}, before = out;
        enter(&p, 0);
        for (uint32_t t = 0; t <= 4000; t += 500) {
            v2p_pair_record(&p, order ? other : TX, DST, 7, t);
            v2p_pair_record(&p, order ? TX : other, DST, 9, t);
        }
        CHECK(!v2p_pair_candidate(&p, 4000, &out));
        CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 4000, &out) == V2P_BTN_ACTION_NONE);
        CHECK(p.state == V2P_PAIR_PAIR && !memcmp(&out, &before, sizeof out));
    }
}

static void contract_candidates_confirm_and_ambiguity(void) {
    v2p_pair_t p = {0};
    v2p_candidate_t out = {0}, before = out;
    v2p_pair_record(&p, TX, DST, 7, 0);
    CHECK(!p.count);  // IDLE callback does nothing
    CHECK(v2p_pair_step(&p, V2_BTN_NONE, 0, NULL) == V2P_BTN_ACTION_NONE);
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 0, NULL) == V2P_BTN_RESYNC);
    enter(&p, 0);
    CHECK(!v2p_pair_candidate(&p, 0, &out));
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 0, &out) == V2P_BTN_ACTION_NONE);
    CHECK(p.state == V2P_PAIR_PAIR && !memcmp(&out, &before, sizeof out));
    v2p_pair_record(&p, TX, DST, 7, 0);
    v2p_pair_record(&p, TX, DST, 7, 1000);
    v2p_pair_record(&p, TX, DST, 7, 2000);
    v2p_pair_record(&p, TX, DST, 7, 2999);
    CHECK(!v2p_pair_candidate(&p, 2999, NULL));
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 2999, NULL) == V2P_BTN_ACTION_NONE);
    v2p_pair_record(&p, TX, DST, 7, 3000);
    CHECK(v2p_pair_candidate(&p, 3000, NULL));
    CHECK(v2p_pair_candidate(&p, 4000, NULL));
    CHECK(!v2p_pair_candidate(&p, 4001, NULL));
    v2p_pair_t stale = p;
    CHECK(v2p_pair_step(&stale, V2_BTN_SHORT, 4001, NULL) == V2P_BTN_ACTION_NONE);
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 4000, &out) == V2P_BTN_CONFIRM);
    CHECK(p.state == V2P_PAIR_IDLE);
    CHECK(!memcmp(out.src, TX, 6) && !memcmp(out.dst, DST, 6));
    CHECK_EQ_U64(out.session_id, 7);
    CHECK_EQ_U64(out.first_ms, 0);
    CHECK_EQ_U64(out.last_ms, 3000);
    // No exclusion for the current association: re-confirm the same (TX,DST).
    enter(&p, 10000);
    seen(&p, 10000, 7);
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 13000, &out) == V2P_BTN_CONFIRM);

    enter(&p, 20000);
    seen(&p, 20000, 7);
    uint8_t other[6];
    memcpy(other, TX, 6);
    other[5]++;
    v2p_pair_record(&p, other, DST, 8, 23000);
    CHECK(!v2p_pair_candidate(&p, 23000, NULL));  // even an immature second entry blocks
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 23000, NULL) == V2P_BTN_ACTION_NONE);
    v2p_pair_record(&p, TX, DST, 7, 24000);
    CHECK(!v2p_pair_candidate(&p, 24000, NULL));
    CHECK(v2p_pair_candidate(&p, 24001, NULL));  // only the first entry is recent
    // The table keys on both addresses, including another destination of the same TX.
    v2p_pair_record(&p, TX, other, 7, 24001);
    CHECK_EQ_U64(p.count, 3);
    CHECK(!v2p_pair_candidate(&p, 24001, NULL));
}

static void contract_overflow_session_and_continuity(void) {
    v2p_pair_t p = {0};
    enter(&p, 0);
    seen(&p, 0, 7);
    for (unsigned i = 1; i < 5; i++) {
        uint8_t src[6];
        memcpy(src, TX, 6);
        src[5] = (uint8_t)i;
        v2p_pair_record(&p, src, DST, 7, 3000);
        CHECK(p.overflow == (i == 4));
    }
    CHECK_EQ_U64(p.count, 4);
    CHECK(p.overflow);
    CHECK(!v2p_pair_candidate(&p, 3000, NULL));
    v2p_pair_record(&p, TX, DST, 7, 4000);
    CHECK(!v2p_pair_candidate(&p, 4001, NULL));  // overflow is sticky even after others go stale
    CHECK(v2p_pair_step(&p, V2_BTN_SHORT, 4001, NULL) == V2P_BTN_ACTION_NONE);
    CHECK(v2p_pair_step(&p, V2_BTN_LONG, 4001, NULL) == V2P_BTN_CANCEL);
    enter(&p, 5000);
    seen(&p, 5000, 7);
    CHECK(v2p_pair_candidate(&p, 8000, NULL));
    v2p_pair_record(&p, TX, DST, 8, 8000);
    CHECK_EQ_U64(p.entries[0].first_ms, 8000);
    CHECK(!v2p_pair_candidate(&p, 8000, NULL));
    v2p_pair_record(&p, TX, DST, 8, 9000);
    v2p_pair_record(&p, TX, DST, 8, 10000);
    v2p_pair_record(&p, TX, DST, 8, 10999);
    CHECK(!v2p_pair_candidate(&p, 10999, NULL));
    v2p_pair_record(&p, TX, DST, 8, 11000);
    CHECK(v2p_pair_candidate(&p, 11000, NULL));
    v2p_pair_record(&p, TX, DST, 8, 12001);
    CHECK_EQ_U64(p.entries[0].first_ms, 12001);
    CHECK(!v2p_pair_candidate(&p, 12001, NULL));  // a gap >1000 ms breaks continuity
}

static void contract_cancel_clear_timeout_and_wrap(void) {
    const uint32_t starts[] = {0, UINT32_MAX - 1500};
    for (unsigned i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        const uint32_t t = starts[i];
        v2p_pair_t p = {0};
        CHECK(v2p_pair_step(&p, V2_BTN_VERY_LONG, t, NULL) == V2P_BTN_CLEAR_RECORD);
        CHECK(p.state == V2P_PAIR_IDLE);
        enter(&p, t);
        seen(&p, t, 9);
        CHECK(v2p_pair_candidate(&p, t + 3000, NULL));
        CHECK(v2p_pair_step(&p, V2_BTN_NONE, t + 59999, NULL) == V2P_BTN_ACTION_NONE);
        CHECK(v2p_pair_step(&p, V2_BTN_NONE, t + 60000, NULL) == V2P_BTN_CANCEL);
        CHECK(p.state == V2P_PAIR_IDLE);
        enter(&p, t + 60001);
        CHECK(v2p_pair_step(&p, V2_BTN_LONG, t + 60002, NULL) == V2P_BTN_CANCEL);
        CHECK(p.state == V2P_PAIR_IDLE);
        enter(&p, t + 60003);
        CHECK(v2p_pair_step(&p, V2_BTN_VERY_LONG, t + 60004, NULL) == V2P_BTN_CLEAR_RECORD);
        CHECK(p.state == V2P_PAIR_IDLE);
    }
}

int main(void) {
    puts("contract: button release thresholds and uint32 wrap");
    contract_thresholds_and_wrap();
    puts("contract: button debounce, bounce, boot-held guard");
    contract_debounce_and_boot_guard();
    puts("contract: pairing zero, one, two recent candidates and explicit confirmation");
    contract_candidates_confirm_and_ambiguity();
    puts("contract: two mature simultaneous candidates never confirm");
    contract_two_mature_candidates_never_confirm();
    puts("contract: pairing fifth-source overflow, session restart, continuity");
    contract_overflow_session_and_continuity();
    puts("contract: pairing cancel, clear, timeout and uint32 wrap");
    contract_cancel_clear_timeout_and_wrap();
    return check_done("contract: v2p_btn gestures, pairing, wrap");
}
