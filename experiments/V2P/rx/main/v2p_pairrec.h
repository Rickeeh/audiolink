// Portable, byte-oriented NVS association format (no packed structs or host endianness).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define V2P_PAIRREC_BYTES 24u
#define V2P_PAIRREC_MAGIC 0x414B4C41u
#define V2P_PAIRREC_VERSION 1u

typedef enum {
    V2P_PAIRREC_OK, V2P_PAIRREC_BAD_LEN, V2P_PAIRREC_BAD_MAGIC,
    V2P_PAIRREC_BAD_VERSION, V2P_PAIRREC_BAD_CRC, V2P_PAIRREC_BAD_MAC
} v2p_pairrec_result_t;
typedef struct { uint8_t tx[6], dst[6]; } v2p_pairrec_t;

static inline uint32_t v2p_pairrec_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ UINT32_MAX;
}

static inline uint32_t v2p_pairrec_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void v2p_pairrec_put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i));
}

static inline bool v2p_pairrec_mac_ok(const uint8_t mac[6]) {
    static const uint8_t zero[6] = {0};
    // Broadcast is also multicast, so the bit test rejects both.
    return !(mac[0] & 1u) && memcmp(mac, zero, 6) != 0;
}

static inline void v2p_pairrec_encode(const uint8_t tx[6], const uint8_t dst[6], uint8_t buf[V2P_PAIRREC_BYTES]) {
    v2p_pairrec_put32(buf, V2P_PAIRREC_MAGIC);
    buf[4] = (uint8_t)V2P_PAIRREC_VERSION;
    buf[5] = buf[6] = buf[7] = 0;
    memcpy(buf + 8, tx, 6);
    memcpy(buf + 14, dst, 6);
    v2p_pairrec_put32(buf + 20, v2p_pairrec_crc32(buf, 20));
}

static inline v2p_pairrec_result_t v2p_pairrec_decode(const uint8_t *buf, size_t len, v2p_pairrec_t *rec) {
    if (len != V2P_PAIRREC_BYTES) return V2P_PAIRREC_BAD_LEN;
    if (v2p_pairrec_get32(buf) != V2P_PAIRREC_MAGIC) return V2P_PAIRREC_BAD_MAGIC;
    // Reserved bits belong to the format version; version 1 requires all zero.
    if (buf[4] != V2P_PAIRREC_VERSION || buf[5] || buf[6] || buf[7]) return V2P_PAIRREC_BAD_VERSION;
    if (v2p_pairrec_get32(buf + 20) != v2p_pairrec_crc32(buf, 20)) return V2P_PAIRREC_BAD_CRC;
    if (!v2p_pairrec_mac_ok(buf + 8) || !v2p_pairrec_mac_ok(buf + 14) || !memcmp(buf + 8, buf + 14, 6))
        return V2P_PAIRREC_BAD_MAC;
    memcpy(rec->tx, buf + 8, 6);
    memcpy(rec->dst, buf + 14, 6);
    return V2P_PAIRREC_OK;
}
