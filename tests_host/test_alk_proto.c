// Layout do cabecalho ALK no fio (alk_proto.h): bytes exatos por offset, little-endian, sem padding.
#include <stdint.h>
#include <string.h>

#include "alk_proto.h"
#include "check.h"

int main(void) {
    const uint32_t probe = 0x01020304u;  // o formato no fio e LE; o teste so e valido num host LE (como o S3)
    CHECK(((const uint8_t *)&probe)[0] == 0x04);

    CHECK_EQ_U64(sizeof(alk_hdr_t), 28);
    CHECK_EQ_U64(ALK_HDR_BYTES, 28);
    CHECK_EQ_U64(ALK_V2_PKT_BYTES, 1052);
    CHECK(ALK_V2_PKT_BYTES <= ALK_ESPNOW_MAX_BYTES);

    alk_hdr_t h;
    memset(&h, 0xEE, sizeof(h));
    h.magic = ALK_MAGIC;
    h.version = ALK_VERSION_V2;
    h.flags = 0;
    h.frames = ALK_FRAMES_PER_PKT;
    h.seq = 0x11223344u;
    h.session_id = 0x0102030405060708ull;
    h.sample_ctr = 0xA1A2A3A4A5A6A7A8ull;
    const uint8_t *b = (const uint8_t *)&h;
    const uint8_t want[28] = {0x31, 0x4B, 0x4C, 0x41,                          // magic "1KLA" em LE = 0x414C4B31
                              0x02, 0x00, 0x80, 0x00,                          // version, flags, frames = 128
                              0x44, 0x33, 0x22, 0x11,                          // seq
                              0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // session_id
                              0xA8, 0xA7, 0xA6, 0xA5, 0xA4, 0xA3, 0xA2, 0xA1}; // sample_ctr
    CHECK(memcmp(b, want, sizeof(want)) == 0);
    for (int i = 0; i < 28; i++) {
        if (b[i] != want[i]) {
            fprintf(stderr, "  byte %d: %02X (esperado %02X)\n", i, b[i], want[i]);
        }
    }
    return check_done("test_alk_proto");
}
