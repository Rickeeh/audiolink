// TX v2 - tarefa de tempo real (tx_audio, core 1, prioridade 20).
//
// Ciclo: bloqueia no DMA do I2S ate ter 128 tramas -> sample_ctr pela timeline -> PCM24 empacotado (alk_pcm24.h) ->
// cabecalho ALK v3 -> esp_now_send.
// O unico ponto de espera e a leitura do DMA, ritmada pelo relogio do ADC. No ciclo de tempo real nao ha consola, nem
// espera ativa, nem reenvios, nem reinicializacoes ou reinicios. Recusa da API = descartar e contar.
// Sem limite de envios em voo (E1b/E1c): cada bloco vai a esp_now_send.
//
// E4b: protocolo v3, com pacotes de 796 B (28 + 128 x 6), sobre a E3b. O RX aceita v2 e v3 (audio/main/link_rx.c).
// Historico, substituido e nao mantido: E1 (ancestral BEN, temporizador com espera ativa, tom digital) e E2/E3 (payload v2
// int32 lido diretamente do DMA para o pacote).
#include "tx_audio.h"

#include <stdint.h>

#include "acq.h"
#include "alk_pcm24.h"
#include "alk_proto.h"
#include "diag.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "espnow_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tx_timeline.h"

static const char *TAG = "tx_audio";

#define TX_SAMPLES (ALK_FRAMES_PER_PKT * ALK_CHANNELS)
_Static_assert(ACQ_BLOCK_BYTES == TX_SAMPLES * 4u, "bloco do DMA = 256 amostras int32");

static int32_t s_block[TX_SAMPLES];
static uint8_t s_pkt[ALK_V3_PKT_BYTES];

void tx_audio_task(void *arg) {
    (void)arg;
    espnow_tx_init();

    uint64_t session_id = 0;
    esp_fill_random(&session_id, sizeof(session_id));  // depois do Wi-Fi ativo (entropia de RF); nunca persistido
    ESP_LOGI(TAG, "sessao: session_id=%016llX", (unsigned long long)session_id);
    ESP_LOGI(TAG, "pacote: v%u, %u tramas PCM24 estereo, %u B cabecalho + %u B audio = %u B; sem limite de envios em voo",
             ALK_VERSION_V3, ALK_FRAMES_PER_PKT, ALK_HDR_BYTES, TX_SAMPLES * 3u, (unsigned)sizeof(s_pkt));

    alk_hdr_t *h = (alk_hdr_t *)s_pkt;
    h->magic = ALK_MAGIC;
    h->version = ALK_VERSION_V3;
    h->flags = 0;
    h->frames = ALK_FRAMES_PER_PKT;
    h->session_id = session_id;

    acq_start();
    ESP_LOGI(TAG, "ciclo de tempo real a arrancar (ritmo: DMA do I2S)");
    diag_start(session_id, esp_timer_get_time());
    tx_timeline_t tl;
    tx_timeline_init(&tl, acq_overflows());

    for (;;) {
        acq_read_block(s_block);
        h->sample_ctr = tx_timeline_stamp(&tl, acq_overflows());
        h->seq = tl.blocks - 1;
        alk_pcm24_pack(s_block, TX_SAMPLES, s_pkt + ALK_HDR_BYTES);
        espnow_tx_send(s_pkt, sizeof(s_pkt));
        diag_publish(tl.blocks, tl.next);
    }
}
