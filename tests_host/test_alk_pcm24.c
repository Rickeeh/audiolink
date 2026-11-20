// deps: components/alk_proto/alk_pcm24.c
// E4: PCM de 24 bit empacotado (alk_pcm24.h) e protocolo v3 (alk_proto.h), bit-exatos.
#include <stdint.h>
#include <string.h>

#include "alk_pcm24.h"
#include "alk_proto.h"
#include "check.h"

static uint32_t rng = 0x9E3779B9u;
static uint32_t xorshift32(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int32_t le32(const uint8_t *b) {  // int32 a partir de 4 bytes LE, sem depender da ordem do host
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static void roundtrip_one(int32_t w, int *bad) {  // pack -> unpack de uma amostra
    uint8_t p[3], u[4];
    alk_pcm24_pack(&w, 1, p);
    alk_pcm24_unpack(p, 1, u);
    if (le32(u) != (int32_t)((uint32_t)w & 0xFFFFFF00u)) {
        (*bad)++;
    }
}

int main(void) {
    // 1. Tamanhos e versoes.
    CHECK_EQ_U64(ALK_V3_PKT_BYTES, 796);
    CHECK_EQ_U64(ALK_V2_PKT_BYTES, 1052);
    CHECK_EQ_U64(alk_payload_bytes(ALK_VERSION_V2, 128), 1024);
    CHECK_EQ_U64(alk_payload_bytes(ALK_VERSION_V3, 128), 768);
    CHECK_EQ_U64(alk_payload_bytes(1, 128), 0);
    CHECK_EQ_U64(alk_payload_bytes(4, 128), 0);

    // 2. Vetores conhecidos: palavra I2S -> 3 bytes no fio -> int32 de volta -> valor de 24 bit com sinal.
    struct {
        uint32_t w;        // palavra I2S (int32 alinhado a esquerda)
        uint8_t b[3];      // bytes no fio
        int32_t s24;       // valor de 24 bit com sinal
    } kv[] = {
        {0x00000000u, {0x00, 0x00, 0x00}, 0},                 // zero
        {0x7FFFFF00u, {0xFF, 0xFF, 0x7F}, 8388607},           // +FS
        {0x80000000u, {0x00, 0x00, 0x80}, -8388608},          // -FS
        {0xFFFFFF00u, {0xFF, 0xFF, 0xFF}, -1},                // -1
        {0x00000100u, {0x01, 0x00, 0x00}, 1},                 // +1 LSB
        {0x12345600u, {0x56, 0x34, 0x12}, 0x123456},          // ordem dos bytes
        {0x123456ABu, {0x56, 0x34, 0x12}, 0x123456},          // byte baixo com lixo: ignorado
        {0xFFFFFFFFu, {0xFF, 0xFF, 0xFF}, -1},                // -1 com o byte baixo cheio
        {0x55555500u, {0x55, 0x55, 0x55}, 0x555555},          // alternado 0101...
        {0xAAAAAA00u, {0xAA, 0xAA, 0xAA}, -5592406},          // alternado 1010... (negativo)
        {0x5A5A5A00u, {0x5A, 0x5A, 0x5A}, 0x5A5A5A},
        {0xA5A5A500u, {0xA5, 0xA5, 0xA5}, -5921371},
        {0x00FF0000u, {0x00, 0xFF, 0x00}, 0x00FF00},
        {0xFF00FF00u, {0xFF, 0x00, 0xFF}, -65281},
    };
    for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
        const int32_t w = (int32_t)kv[i].w;
        uint8_t p[3], u[4];
        alk_pcm24_pack(&w, 1, p);
        CHECK(memcmp(p, kv[i].b, 3) == 0);
        alk_pcm24_unpack(p, 1, u);
        CHECK_EQ_U64((uint32_t)le32(u), kv[i].w & 0xFFFFFF00u);
        CHECK(u[0] == 0);
        CHECK(alk_pcm24_s24(le32(u)) == kv[i].s24);
        CHECK(alk_pcm24_s24(w) == kv[i].s24);
    }

    // 3. Exaustivo: os 2^24 valores de 24 bit, todos bit-exatos na ida e volta e com o sinal certo.
    int bad = 0, bad_sign = 0;
    for (uint32_t v = 0; v < (1u << 24); v++) {
        const int32_t w = (int32_t)(v << 8);
        roundtrip_one(w, &bad);
        const int32_t expect = (v & 0x800000u) ? (int32_t)v - (1 << 24) : (int32_t)v;
        if (alk_pcm24_s24(w) != expect) {
            bad_sign++;
        }
    }
    CHECK_EQ_U64(bad, 0);
    CHECK_EQ_U64(bad_sign, 0);

    // 4. Aleatorio, com o byte baixo tambem aleatorio, em blocos de um pacote (256 amostras).
    static int32_t blk[ALK_FRAMES_PER_PKT * ALK_CHANNELS];
    static uint8_t wire[ALK_FRAMES_PER_PKT * ALK_CHANNELS * 3];
    static uint8_t back[ALK_FRAMES_PER_PKT * ALK_CHANNELS * 4 + 1];
    int bad_blk = 0;
    for (int r = 0; r < 4000; r++) {  // ~1 milhao de amostras
        for (size_t i = 0; i < ALK_FRAMES_PER_PKT * ALK_CHANNELS; i++) {
            blk[i] = (int32_t)xorshift32();
        }
        alk_pcm24_pack(blk, ALK_FRAMES_PER_PKT * ALK_CHANNELS, wire);
        alk_pcm24_unpack(wire, ALK_FRAMES_PER_PKT * ALK_CHANNELS, back + 1);  // destino desalinhado de proposito
        for (size_t i = 0; i < ALK_FRAMES_PER_PKT * ALK_CHANNELS; i++) {
            if (le32(back + 1 + 4 * i) != (int32_t)((uint32_t)blk[i] & 0xFFFFFF00u)) {
                bad_blk++;
            }
        }
    }
    CHECK_EQ_U64(bad_blk, 0);

    // 5. Pacote completo TX -> RX: o TX monta v3 (cabecalho + pack); o RX valida com as regras de link_rx.c e
    //    normaliza para o layout v2. Tem de dar o pacote v2 que o mesmo bloco daria, com o byte baixo a 0.
    for (size_t i = 0; i < ALK_FRAMES_PER_PKT * ALK_CHANNELS; i++) {
        blk[i] = (int32_t)xorshift32();
    }
    static uint8_t v3[ALK_V3_PKT_BYTES], v2ref[ALK_V2_PKT_BYTES], norm[ALK_V2_PKT_BYTES];
    alk_hdr_t h = {.magic = ALK_MAGIC, .version = ALK_VERSION_V3, .flags = 0, .frames = ALK_FRAMES_PER_PKT,
                   .seq = 77, .session_id = 0x1122334455667788ull, .sample_ctr = 128ull * 77};
    memcpy(v3, &h, sizeof(h));
    alk_pcm24_pack(blk, ALK_FRAMES_PER_PKT * ALK_CHANNELS, v3 + ALK_HDR_BYTES);
    // regras do RX (link_rx.c): magic, versao 2 ou 3, 1 <= frames <= 128, 28 + payload(versao, frames) == comprimento
    alk_hdr_t r;
    memcpy(&r, v3, sizeof(r));
    CHECK(r.magic == ALK_MAGIC && (r.version == ALK_VERSION_V2 || r.version == ALK_VERSION_V3));
    CHECK(r.frames >= 1 && r.frames <= ALK_FRAMES_PER_PKT);
    CHECK_EQ_U64(sizeof(alk_hdr_t) + alk_payload_bytes(r.version, r.frames), sizeof(v3));
    CHECK_EQ_U64(alk_to_v2_layout(v3, norm), ALK_V2_PKT_BYTES);
    h.version = ALK_VERSION_V2;  // referencia: o mesmo bloco em v2, com o byte baixo limpo
    memcpy(v2ref, &h, sizeof(h));
    for (size_t i = 0; i < ALK_FRAMES_PER_PKT * ALK_CHANNELS; i++) {
        const uint32_t w = (uint32_t)blk[i] & 0xFFFFFF00u;
        memcpy(v2ref + ALK_HDR_BYTES + 4 * i, &w, 4);  // host LE (verificado em test_alk_proto)
    }
    CHECK(memcmp(norm + ALK_HDR_BYTES, v2ref + ALK_HDR_BYTES, ALK_V2_PKT_BYTES - ALK_HDR_BYTES) == 0);
    CHECK(memcmp(norm, v3, ALK_HDR_BYTES) == 0);  // cabecalho copiado tal como chegou (version 3 incluida)
    CHECK(norm[4] == ALK_VERSION_V3);

    // 6. v2 passa sem alteracao (compatibilidade com o TX RC2/legado).
    static uint8_t v2in[ALK_V2_PKT_BYTES], v2out[ALK_V2_PKT_BYTES];
    for (size_t i = 0; i < sizeof(v2in); i++) {
        v2in[i] = (uint8_t)xorshift32();
    }
    memcpy(v2in, &h, sizeof(h));  // cabecalho v2 valido
    CHECK_EQ_U64(alk_to_v2_layout(v2in, v2out), ALK_V2_PKT_BYTES);
    CHECK(memcmp(v2in, v2out, ALK_V2_PKT_BYTES) == 0);

    // 7. Defesa: pacotes que violam as pre-condicoes devolvem 0 e nao escrevem nada.
    memset(norm, 0xCD, sizeof(norm));
    alk_hdr_t bad_h = h;
    bad_h.frames = ALK_FRAMES_PER_PKT + 1;
    memcpy(v3, &bad_h, sizeof(bad_h));
    CHECK_EQ_U64(alk_to_v2_layout(v3, norm), 0);
    bad_h = h;
    bad_h.version = 4;
    memcpy(v3, &bad_h, sizeof(bad_h));
    CHECK_EQ_U64(alk_to_v2_layout(v3, norm), 0);
    bad_h = h;
    bad_h.frames = 0;
    memcpy(v3, &bad_h, sizeof(bad_h));
    CHECK_EQ_U64(alk_to_v2_layout(v3, norm), 0);
    CHECK(norm[0] == 0xCD && norm[ALK_V2_PKT_BYTES - 1] == 0xCD);

    return check_done("test_alk_pcm24");
}
