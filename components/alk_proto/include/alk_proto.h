// AudioLink - formato do pacote ALK no fio (TX -> RX), partilhado pelo TX v2 (tx_v2/) e pelo RX (audio/).
//
// O cabecalho e o do protocolo v2 em producao (audio/main/link_proto.h, link_pkt_hdr_t): 28 B, packed, sem padding.
// Campos multibyte em little-endian (ordem nativa do ESP32-S3; os testes de host verificam que o host tambem e LE).
//
//   offset  bytes  campo
//   0       4      magic       0x414C4B31 ("ALK1")
//   4       1      version     2 = payload int32 por amostra (24 bit alinhados a esquerda)
//                              3 = payload PCM de 24 bit empacotado, 3 B por amostra (alk_pcm24.h)
//   5       1      flags       0 (reservado)
//   6       2      frames      tramas estereo no pacote (fixo: 128)
//   8       4      seq         pacotes desde o arranque desta sessao
//   12      8      session_id  identidade do boot do TX (aleatoria, nunca persistida)
//   20      8      sample_ctr  indice da 1.a trama do pacote no relogio do ADC (conta tambem as tramas perdidas)
//   28      ...    payload     frames x (L, R): v2 = 128 x 8 B = 1024 B (pacote de 1052 B);
//                              v3 = 128 x 6 B = 768 B (pacote de 796 B)
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALK_MAGIC 0x414C4B31u
#define ALK_VERSION_V2 2u
#define ALK_VERSION_V3 3u
#define ALK_FRAMES_PER_PKT 128u
#define ALK_CHANNELS 2u
#define ALK_HDR_BYTES 28u
#define ALK_V2_BYTES_PER_FRAME 8u  // L int32 + R int32
#define ALK_V2_PKT_BYTES (ALK_HDR_BYTES + ALK_FRAMES_PER_PKT * ALK_V2_BYTES_PER_FRAME)
#define ALK_V3_BYTES_PER_FRAME 6u  // L 24 bit + R 24 bit, empacotados
#define ALK_V3_PKT_BYTES (ALK_HDR_BYTES + ALK_FRAMES_PER_PKT * ALK_V3_BYTES_PER_FRAME)
#define ALK_ESPNOW_MAX_BYTES 1470u  // ESP_NOW_MAX_DATA_LEN_V2 (IDF 5.4.4, esp_now.h)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t frames;
    uint32_t seq;
    uint64_t session_id;
    uint64_t sample_ctr;
} alk_hdr_t;

_Static_assert(offsetof(alk_hdr_t, magic) == 0, "alk_hdr_t: magic no offset 0");
_Static_assert(offsetof(alk_hdr_t, version) == 4, "alk_hdr_t: version no offset 4");
_Static_assert(offsetof(alk_hdr_t, flags) == 5, "alk_hdr_t: flags no offset 5");
_Static_assert(offsetof(alk_hdr_t, frames) == 6, "alk_hdr_t: frames no offset 6");
_Static_assert(offsetof(alk_hdr_t, seq) == 8, "alk_hdr_t: seq no offset 8");
_Static_assert(offsetof(alk_hdr_t, session_id) == 12, "alk_hdr_t: session_id no offset 12");
_Static_assert(offsetof(alk_hdr_t, sample_ctr) == 20, "alk_hdr_t: sample_ctr no offset 20");
_Static_assert(sizeof(alk_hdr_t) == ALK_HDR_BYTES, "alk_hdr_t: 28 B, sem padding");
_Static_assert(ALK_V2_PKT_BYTES == 1052u, "pacote v2: 28 + 128 x 8 = 1052 B");
_Static_assert(ALK_V2_PKT_BYTES <= ALK_ESPNOW_MAX_BYTES, "pacote v2 cabe numa trama ESP-NOW v2");
_Static_assert(ALK_V3_PKT_BYTES == 796u, "pacote v3: 28 + 128 x 6 = 796 B");
_Static_assert(ALK_V3_PKT_BYTES <= ALK_ESPNOW_MAX_BYTES, "pacote v3 cabe numa trama ESP-NOW v2");

// Bytes de payload de um pacote desta versao com `frames` tramas; 0 se a versao for desconhecida.
static inline size_t alk_payload_bytes(uint8_t version, uint16_t frames) {
    return version == ALK_VERSION_V2   ? (size_t)frames * ALK_V2_BYTES_PER_FRAME
           : version == ALK_VERSION_V3 ? (size_t)frames * ALK_V3_BYTES_PER_FRAME
                                       : 0;
}
