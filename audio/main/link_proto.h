// AUDIOLINK passo 3 - formato do pacote do link basico:
//   PCM1808 -> TX S3 (I2S) -> ESP-NOW -> RX S3 (I2S) -> PCM5102A
//
// Deliberadamente simples: SEM FEC, SEM PLC, SEM jitter buffer sofisticado
// (isso e so a partir do passo 6, docs/HARDWARE_PLAN.md). NAO e o protocolo
// final do produto - e so o minimo para observar o comportamento real do
// link com audio (perdas, desordem, drift entre os dois relogios).
//
// Payload = hdr.frames frames estereo, cada um 2x int32 (L,R), no MESMO
// formato que i2s_channel_read()/i2s_channel_write() usam com
// I2S_DATA_BIT_WIDTH_32BIT (24 bit de audio alinhados a esquerda nos 32
// bits: amostra = palavra >> 8, tal como em adc_test.c/dac_test.c). Sem
// conversao: o TX copia o buffer do ADC diretamente para o payload do
// pacote, o RX copia o payload diretamente para o buffer do DAC.
//
// PATCH-TX-SESSION (2026-10-01, protocolo v2): AUDIT-TX-SESSION mostrou que o RX nao tinha
// nenhuma forma de saber que o TX tinha reiniciado - sample_ctr sozinho reinicia perto de 0 a
// cada boot do TX, e o RX (que pode continuar ligado indefinidamente) tratava os blocos de
// indice baixo da nova sessao como "ja vistos" (bitmap de dedup nunca limpo) ou deixava
// max_received_ctr/max_enqueued_ctr presos no maximo da sessao anterior - minutos de silencio
// a seguir a um reboot do TX, confirmado em bancada (ver scratchpad/PATCH-OBS-TX-01*/adversarial).
// session_id (novo campo, 64 bit) + sample_ctr juntos identificam a posicao temporal: sample_ctr
// sozinho deixa de ser suficiente. Gerado 1x no boot do TX (esp_fill_random, depois do Wi-Fi
// estar ativo - ver link_tx.c), NUNCA persistido em NVS. O RX trata qualquer session_id novo como
// o inicio de uma timeline nova (reset do estado por sessao - ver link_rx.c).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_FRAMES_PER_PKT ((uint32_t)CONFIG_AUDIOLINK_LINK_FRAMES_PER_PKT)
#define LINK_BYTES_PER_FRAME 8u  // L int32 + R int32, sem compactacao

// "ALK1": identifica um pacote deste protocolo (nao lixo de outra origem no mesmo canal
// ESP-NOW). Mudar o layout do cabecalho = subir LINK_PROTO_VERSION - um RX/TX com versoes
// diferentes rejeita-se mutuamente de forma limpa (pkts_corrupt), nunca interpreta mal um
// cabecalho de tamanho/layout diferente. v2 (2026-10-01): acrescenta session_id.
#define LINK_PROTO_MAGIC   0x414C4B31u
#define LINK_PROTO_VERSION 2u

// Cabecalho explicito e de tamanho fixo: packed (sem padding do compilador) E cada campo tem
// o seu proprio _Static_assert de offsetof, nao so o sizeof total - este e um protocolo
// wire-level entre dois binarios compilados em separado (TX/RX), nao e seguro assumir
// implicitamente o layout so por "parece packed".
typedef struct __attribute__((packed)) {
    uint32_t magic;        // LINK_PROTO_MAGIC
    uint8_t  version;      // LINK_PROTO_VERSION
    uint8_t  flags;        // reservado, 0 por agora
    uint16_t frames;       // n.o de frames estereo neste pacote (pacote auto-descritivo; fixo nesta fase = LINK_FRAMES_PER_PKT)
    uint32_t seq;          // pacotes enviados desde o arranque do stream (monotonico; deteta perdas/desordem ao nivel do transporte)
    uint64_t session_id;   // PATCH-TX-SESSION: identidade do boot do TX (esp_fill_random, nao persistido).
                           // Fica junto a sample_ctr de proposito - session_id+sample_ctr, nao so
                           // sample_ctr, e que identifica a posicao na timeline (ver comentario acima).
    uint64_t sample_ctr;   // indice do 1o frame estereo deste pacote, cumulativo desde o arranque do I2S do TX
                           // NESTA sessao (reinicia a cada boot do TX - ver session_id acima). uint64: a
                           // 48 kHz um uint32 dava a volta as ~24,8 h - aqui nao da a volta em nenhum prazo
                           // razoavel dentro de uma unica sessao.
} link_pkt_hdr_t;

_Static_assert(offsetof(link_pkt_hdr_t, magic) == 0, "layout do cabecalho do link: magic deve estar no offset 0");
_Static_assert(offsetof(link_pkt_hdr_t, version) == 4, "layout do cabecalho do link: version deve estar no offset 4");
_Static_assert(offsetof(link_pkt_hdr_t, flags) == 5, "layout do cabecalho do link: flags deve estar no offset 5");
_Static_assert(offsetof(link_pkt_hdr_t, frames) == 6, "layout do cabecalho do link: frames deve estar no offset 6");
_Static_assert(offsetof(link_pkt_hdr_t, seq) == 8, "layout do cabecalho do link: seq deve estar no offset 8");
_Static_assert(offsetof(link_pkt_hdr_t, session_id) == 12, "layout do cabecalho do link: session_id deve estar no offset 12");
_Static_assert(offsetof(link_pkt_hdr_t, sample_ctr) == 20, "layout do cabecalho do link: sample_ctr deve estar no offset 20");
_Static_assert(sizeof(link_pkt_hdr_t) == 28, "cabecalho do link deve ter 28 bytes (20 + 8 de session_id, protocolo v2)");

#define LINK_PKT_MAX_BYTES (sizeof(link_pkt_hdr_t) + (size_t)LINK_FRAMES_PER_PKT * LINK_BYTES_PER_FRAME)

#ifdef __cplusplus
}
#endif
