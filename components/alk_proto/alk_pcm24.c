// AudioLink - PCM estereo de 24 bit empacotado (ver alk_pcm24.h). So operacoes de byte e de inteiros sem sinal:
// comportamento definido em C, sem acessos desalinhados.
#include "alk_pcm24.h"

#include <string.h>

#include "alk_proto.h"

void alk_pcm24_pack(const int32_t *src, size_t n, uint8_t *dst) {
    for (size_t i = 0; i < n; i++) {
        const uint32_t w = (uint32_t)src[i];
        dst[3 * i] = (uint8_t)(w >> 8);
        dst[3 * i + 1] = (uint8_t)(w >> 16);
        dst[3 * i + 2] = (uint8_t)(w >> 24);
    }
}

void alk_pcm24_unpack(const uint8_t *src, size_t n, uint8_t *dst_le32) {
    for (size_t i = 0; i < n; i++) {
        dst_le32[4 * i] = 0;
        dst_le32[4 * i + 1] = src[3 * i];
        dst_le32[4 * i + 2] = src[3 * i + 1];
        dst_le32[4 * i + 3] = src[3 * i + 2];
    }
}

size_t alk_to_v2_layout(const uint8_t *pkt, uint8_t *dst) {
    alk_hdr_t h;
    memcpy(&h, pkt, sizeof(h));
    if (h.magic != ALK_MAGIC || h.frames == 0 || h.frames > ALK_FRAMES_PER_PKT ||
        (h.version != ALK_VERSION_V2 && h.version != ALK_VERSION_V3)) {
        return 0;
    }
    const size_t samples = (size_t)h.frames * ALK_CHANNELS;
    memcpy(dst, pkt, ALK_HDR_BYTES);
    if (h.version == ALK_VERSION_V3) {
        alk_pcm24_unpack(pkt + ALK_HDR_BYTES, samples, dst + ALK_HDR_BYTES);
    } else {
        memcpy(dst + ALK_HDR_BYTES, pkt + ALK_HDR_BYTES, samples * 4u);
    }
    return ALK_HDR_BYTES + samples * 4u;
}
