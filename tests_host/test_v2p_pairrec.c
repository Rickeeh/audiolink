#include <string.h>

#include "../experiments/V2P/rx/main/v2p_pairrec.h"
#include "check.h"

static const uint8_t TX[6] = {0x28, 0x84, 0x85, 0x64, 0x1d, 0xec};
static const uint8_t DST[6] = {0xac, 0x27, 0x6e, 0xa5, 0x4e, 0x10};

static void contract_round_trip_and_crc(void) {
    uint8_t blob[V2P_PAIRREC_BYTES];
    v2p_pairrec_t rec;
    v2p_pairrec_encode(TX, DST, blob);
    CHECK_EQ_U64(v2p_pairrec_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    // Observed from the real encoder on the first host run: 0x50799e00.
    static const uint8_t known[V2P_PAIRREC_BYTES] = {
        0x41, 0x4c, 0x4b, 0x41, 1, 0, 0, 0,
        0x28, 0x84, 0x85, 0x64, 0x1d, 0xec, 0xac, 0x27, 0x6e, 0xa5, 0x4e, 0x10,
        0x00, 0x9e, 0x79, 0x50
    };
    CHECK_EQ_U64(v2p_pairrec_get32(blob + 20), 0x50799e00u);
    CHECK(!memcmp(blob, known, sizeof known));
    CHECK(blob[0] == 0x41 && blob[1] == 0x4c && blob[2] == 0x4b && blob[3] == 0x41);
    CHECK(blob[4] == 1 && blob[5] == 0 && blob[6] == 0 && blob[7] == 0);
    CHECK(!memcmp(blob + 8, TX, 6) && !memcmp(blob + 14, DST, 6));
    CHECK(v2p_pairrec_decode(blob, sizeof blob, &rec) == V2P_PAIRREC_OK);
    CHECK(!memcmp(rec.tx, TX, 6) && !memcmp(rec.dst, DST, 6));
}

static void contract_decode_errors(void) {
    uint8_t blob[V2P_PAIRREC_BYTES + 1];
    v2p_pairrec_t rec, before;
    memset(&rec, 0xa5, sizeof rec);
    before = rec;
    v2p_pairrec_encode(TX, DST, blob);
    for (size_t len = 0; len < V2P_PAIRREC_BYTES; len++)
        CHECK(v2p_pairrec_decode(blob, len, &rec) == V2P_PAIRREC_BAD_LEN);
    CHECK(v2p_pairrec_decode(NULL, 0, &rec) == V2P_PAIRREC_BAD_LEN);
    CHECK(v2p_pairrec_decode(blob, sizeof blob, &rec) == V2P_PAIRREC_BAD_LEN);
    blob[0] ^= 1;
    CHECK(v2p_pairrec_decode(blob, V2P_PAIRREC_BYTES, &rec) == V2P_PAIRREC_BAD_MAGIC);
    blob[0] ^= 1;
    // Reject both version bytes and both reserved bytes.
    for (unsigned i = 4; i < 8; i++) {
        blob[i] ^= 2;
        CHECK(v2p_pairrec_decode(blob, V2P_PAIRREC_BYTES, &rec) == V2P_PAIRREC_BAD_VERSION);
        blob[i] ^= 2;
    }
    // Cover every byte protected by the CRC, plus corruption of each CRC byte.
    for (unsigned i = 8; i < V2P_PAIRREC_BYTES; i++) {
        blob[i] ^= 0x80;
        CHECK(v2p_pairrec_decode(blob, V2P_PAIRREC_BYTES, &rec) == V2P_PAIRREC_BAD_CRC);
        blob[i] ^= 0x80;
    }
    CHECK(!memcmp(&rec, &before, sizeof rec));  // failures never partially publish a record
}

static void contract_mac_validation(void) {
    static const uint8_t bad[][6] = {
        {0, 0, 0, 0, 0, 0}, {255, 255, 255, 255, 255, 255}, {1, 2, 3, 4, 5, 6}
    };
    uint8_t blob[V2P_PAIRREC_BYTES];
    v2p_pairrec_t rec;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        v2p_pairrec_encode(bad[i], DST, blob);
        CHECK(v2p_pairrec_decode(blob, sizeof blob, &rec) == V2P_PAIRREC_BAD_MAC);
        v2p_pairrec_encode(TX, bad[i], blob);
        CHECK(v2p_pairrec_decode(blob, sizeof blob, &rec) == V2P_PAIRREC_BAD_MAC);
    }
    v2p_pairrec_encode(TX, TX, blob);
    CHECK(v2p_pairrec_decode(blob, sizeof blob, &rec) == V2P_PAIRREC_BAD_MAC);
    // Locally administered unicast addresses are valid too.
    const uint8_t local[6] = {2, 0, 0, 0, 0, 1};
    v2p_pairrec_encode(local, DST, blob);
    CHECK(v2p_pairrec_decode(blob, sizeof blob, &rec) == V2P_PAIRREC_OK);
}

int main(void) {
    puts("contract: pairrec round trip and IEEE CRC; characterization: pinned real encoder vector");
    contract_round_trip_and_crc();
    puts("contract: pairrec length, magic, version, reserved, CRC errors");
    contract_decode_errors();
    puts("contract: pairrec zero, broadcast, multicast, equal MAC rejection");
    contract_mac_validation();
    return check_done("contract: v2p_pairrec round trip, CRC, errors");
}
