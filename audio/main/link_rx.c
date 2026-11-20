// Passo 3 (RX): ESP-NOW -> I2S -> PCM5102A, em bruto, sem FEC/PLC.
//
// CORRIGIDO (2026-09-29): a 1a versao tentava recuperar retroativamente todos
// os blocos em falta quando detetava um buraco grande (repetia o ultimo bloco
// em cadeia, de forma sincrona, ate apanhar o pacote atual) - isso bloqueava a
// tarefa durante segundos, a fila enchia (queue_ovf) e o atraso so crescia
// (espiral sem recuperacao). Desde ai o DAC avanca SEMPRE exatamente 1 bloco
// de LINK_FRAMES_PER_PKT frames por periodo, ritmado pelo proprio
// i2s_channel_write (bloqueia ate a DMA precisar de mais dados = tempo real).
//
// BUFFER MINIMO (2026-09-30): antes de comecar a tocar, acumula
// AUDIOLINK_LINK_RX_BUFFER_BLOCKS blocos na fila (silencio entretanto, sem
// contar como perda). So depois disso arranca o playout, um bloco por
// periodo, com as MESMAS regras de sempre - nao mudou nenhuma delas:
//   1. descarta (sem escrever) qualquer pacote na fila com sample_ctr < esperado
//      - atrasado/duplicado/fora de ordem; um buraco antigo NUNCA bloqueia isto;
//   2. se o pacote com sample_ctr == esperado ja estiver na fila, usa-o;
//   3. senao (ainda nao chegou OU perdido - nao se distingue aqui), faz
//      concealment de UM bloco (repete o ultimo; desde CONCEAL-FADE-01, fade-out e silencio) e avanca na mesma.
// Nunca ha um ciclo de recuperacao nem reenchimento a meio do stream: cada
// volta do loop escreve exatamente um bloco, sempre. O buffer so amortece a
// chegada tardia de 1-4 blocos (a maioria medida antes de o introduzir); nao
// e PLC nem corrige drift.
//
// I2S identico ao validado no passo 2 (dac_test.c): S3 mestre, PCM5102A em
// modo PLL a partir do BCK (SCK do modulo a GND), GPIO5/6/7 = BCK/LRC/DIN,
// XSMT (se comandado) solto ~0,5 s depois do BCK/LRCK estabilizarem.
//
// INSTRUMENTACAO (2026-09-29): separa as causas de perda em vez de as misturar
// num "ooo" generico. No RECEBIMENTO (link_rx_on_packet, tarefa Wi-Fi), cada
// pacote e classificado por um bitmap de blocos ja vistos - duplicado (bit ja
// ligado, NUNCA entra na fila) ou unico -, e por comparacao com o maior
// sample_ctr ja aceite - "reordenado na chegada" (chegou depois de um bloco
// mais recente) ou nao. Tambem mede o intervalo entre chegadas e o desvio
// (jitter) face aos ~2,66 ms nominais. Na REPRODUCAO (link_rx_run), quando um
// pacote e descartado por chegar tarde (sample_ctr < esperado), usa essa
// classificacao para contar separadamente "atrasado" vs "reordenado", e mede
// o atraso em blocos (histograma). O playout em si NAO mudou desde a correcao
// anterior (continua 1 bloco por periodo, sem recuperacao sincrona).
//
// CORRECAO DE DRIFT POR SAMPLE-SLIP (v1 experimental, 2026-09-30; v1b
// 2026-09-30 - split de contadores + controlador por ocupacao): mecanismo
// separado do concealment acima, so ativo com AUDIOLINK_LINK_DRIFT_CORRECTION.
//
// SPLIT DE CONTADORES (v1b, pedido do Psy apos observar phase_err na casa dos
// milhares de frames com queue_ovf alto): o antigo s_max_enqueued_ctr avancava
// ANTES de se saber se xQueueSend() tinha sucesso, e nunca recuava em
// queue_ovf - media "maior sample_ctr recebido", nao "maior que entrou na
// fila". Agora sao dois contadores SEPARADOS (s_max_received_ctr sobe sempre
// que um pacote valido do peer passa a validacao/jump-check, independente do
// enqueue; s_max_enqueued_ctr so sobe se xQueueSend() devolver sucesso) e
// NENHUM dos dois alimenta o controlador - ficam so como diagnostico nos logs
// (phase_err_received/phase_err_enqueued). O controlador usa a OCUPACAO REAL
// da fila (uxQueueMessagesWaiting), que nao tem este problema por construcao.
//
// Tres mecanismos independentes, cada um com contadores e caminho proprios:
//   1. concealment - RF normal, 1 bloco por periodo (o que se escreve: ver CONCEAL-FADE-01 abaixo).
//   2. drift correction - insere/remove exatamente 1 frame estereo (nunca um
//      bloco) via interpolacao linear (slip_insert/slip_drop), decidido por
//      uma EMA lenta (tau=AUDIOLINK_LINK_DRIFT_EMA_TAU_MS) da OCUPACAO REAL da
//      fila (occ_ema, expressa em frames-equivalentes = occ_ema*128) contra
//      TARGET = AUDIOLINK_LINK_RX_BUFFER_BLOCKS*128, com histerese e cooldown;
//      NUNCA toca em expected_ctr. Suspenso durante o freeze do gate RF.
//   3. rebuffer/recovery - separado do sample-slip, e agora com DUAS condicoes
//      de gatilho independentes sobre uma EMA rapida da ocupacao
//      (tau=AUDIOLINK_LINK_REBUFFER_EMA_TAU_MS), mais robusta a oscilacao do
//      jitter normal do que um check de "sustentado sem interrupcao" (que na
//      simulacao nunca completava por o jitter tocar de volta para dentro da
//      banda): LOW (ocupacao cai abaixo de AUDIOLINK_LINK_REBUFFER_LOW_WATER_FRAMES)
//      ou HIGH (ocupacao fica perto do topo da fila, >=
//      AUDIOLINK_LINK_REBUFFER_HIGH_WATER_FRAMES, e/ou a taxa de queue_ovf
//      excede AUDIOLINK_LINK_REBUFFER_OVF_RATE_HZ - caso observado em
//      bancada: TX bem a frente do que expected_ctr esperava, fila sempre
//      cheia, sem que o LOW water alguma vez dispare). O STARTUP (arranque ou
//      reinicio do RX enquanto o TX ja vai a meio) usa o MESMO mecanismo,
//      unificado - nao ha mais um bootstrap separado que aceite blocos nao
//      contiguos. Em qualquer dos 3 motivos: expected_ctr fica CONGELADO; o
//      I2S/DAC continua a correr com silencio (nunca para/reinicia); so
//      resincroniza quando encontra AUDIOLINK_LINK_RX_BUFFER_BLOCKS blocos com
//      sample_ctr CONTIGUOS (nao "quaisquer N pacotes"). Ao sair do rebuffer,
//      reinicia as duas EMAs de ocupacao, a EMA da taxa de queue_ovf, a
//      janela do gate RF e o cooldown - evita reagir ao salto artificial
//      causado pela propria resincronizacao.
// gate_freeze: deteta rajada (>=AUDIOLINK_LINK_RF_TRIGGER_COUNT concealments
// future_waiting em AUDIOLINK_LINK_RF_TRIGGER_WINDOW_MS) e congela so a correcao
// de drift (nao o rebuffer) por uma duracao FIXA e NAO renovavel
// (AUDIOLINK_LINK_RF_FREEZE_MS) - um freeze que se renovasse a cada evento
// durante ele proprio ficava preso indefinidamente perante lag cronico (bug
// encontrado e corrigido em simulacao de 4h antes de codificar).
// PATCH-DRIFT-01 (2026-10-01): concealments queue_empty deixaram de armar o
// gate. FAIL-DRIFT-01 mostrou o ciclo: slip insuficiente -> occupancy desce ->
// queue_empty sobe (>=6/s em 67,7% dos segundos) -> gate_freeze quase
// permanente -> slip limitado a 1 por RF_FREEZE_MS -> occupancy desce mais.
// Era a consequencia do erro de relogio a desligar o mecanismo que o corrige.
// PATCH-OBS-01 (2026-10-01): nenhuma operacao de logging/consola corre no loop de
// playout. INSTR-PLAYOUT-01 mostrou um stall de 55 ms (+13 on_send_q_ovf) no proprio
// loop: a consola USB-Serial-JTAG sem driver faz busy-wait ate 50 ms quando o FIFO nao
// esvazia (esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c). O loop so preenche um
// snapshot (rx_diag_t) 1x/s e publica-o com uma copia curta sob spinlock; formatar e
// imprimir e trabalho de rx_diag_task (prioridade abaixo do playout, no outro core), que
// pode bloquear ou perder linhas sem atrasar o I2S. Nenhuma regra de playout mudou.
// Parametros (EMA_TAU, HYST, COOLDOWN, RF_TRIGGER_COUNT, RF_FREEZE,
// REBUFFER_EMA_TAU, REBUFFER_LOW/HIGH_WATER, OVF_RATE) validados so por
// simulacao ou reexpressos de valores ja aprovados (LOW_WATER) - HIGH_WATER e
// OVF_RATE sao NOVOS e NAO validados (primeira estimativa). Ver
// Kconfig.projbuild para a justificacao de cada um. Nao ajustar sem validacao
// em hardware real.
// CONCEAL-FADE-01 e RESYNC-KEEP-RUN (2026-10-30, docs/RX_CONCEAL_FADE_PROPOSAL.md §6), cada um com a sua opcao
// Kconfig (desligadas = comportamento do JB40):
//   - CONCEAL-FADE-01: todo o periodo sem bloco real (ocultacao em NORMAL e REBUFFERING) sai por link_conceal.h:
//     1 fade-out com o ultimo bloco ao contrario, depois silencio; o 1.o bloco real a seguir entra com fade-in. Acaba
//     com o tom de 375,6 Hz das falhas longas e com os degraus nas transicoes. Os blocos normais saem bit-exatos e
//     nenhuma decisao ou contador muda.
//   - RESYNC-KEEP-RUN: no fim do rebuffer, a sequencia contigua que prova a ressincronizacao volta a frente da fila
//     e e tocada, em vez de ser perdida (ver o ramo REBUFFERING).
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "al_led.h"
#include "alk_pcm24.h"
#include "alk_proto.h"
#include "audio_common.h"
#include "link_conceal.h"
#include "link_plaus.h"
#include "link_resync.h"
#include "link_proto.h"
#include "link_espnow.h"

static const char *TAG = "link_rx";

// TX v2 (E4, 2026-10-09): o RX aceita o protocolo v2 (int32) e o v3 (PCM24 empacotado, components/alk_proto). Um pacote v3
// e normalizado na rececao para o layout v2 (cabecalho + int32 alinhado a esquerda, byte baixo 0) e o playout le
// exatamente o que lia. Nada do playout muda (TARGET, water, slip, DMA, ocultacao).
_Static_assert(sizeof(link_pkt_hdr_t) == ALK_HDR_BYTES && LINK_PROTO_MAGIC == ALK_MAGIC &&
                   LINK_PROTO_VERSION == ALK_VERSION_V2 && LINK_FRAMES_PER_PKT == ALK_FRAMES_PER_PKT &&
                   LINK_PKT_MAX_BYTES == ALK_V2_PKT_BYTES,
               "link_proto.h (v2 legado) e alk_proto.h tem de descrever o mesmo cabecalho e o mesmo pacote v2");

static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}

#if CONFIG_AUDIOLINK_LINK_RX_LOG_CONCEALMENTS
// Pico do canal R (dBFS) de um bloco int32 estereo (formato I2S 24 bit alinhados a
// esquerda em 32, igual ao adc_test.c/dac_test.c). So para diagnostico (2026-09-30,
// investigacao do "pii" no fim do tom): dá o conteudo do bloco repetido em cada
// concealment, para correlacionar com a transicao tom->silencio.
static float block_peak_dbfs_r(const int32_t *block, uint32_t frames) {
    int32_t peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t v = block[2 * i + 1] >> 8;  // R, 24 bit
        int32_t a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    const double FS = 8388608.0;  // 2^23
    return (float)(20.0 * log10(peak > 0 ? (double)peak / FS : 1e-9));
}
#endif

// Buffer minimo (blocos que se tenta ter sempre esperando a jogar) + folga para rajadas
// momentaneas de chegada. Como o loop descarta ativamente tudo o que fica velho
// (sample_ctr < esperado), a fila nao pode encher por causa de um atraso antigo.
#define BUFFER_BLOCKS ((uint32_t)CONFIG_AUDIOLINK_LINK_RX_BUFFER_BLOCKS)
#define QUEUE_DEPTH (BUFFER_BLOCKS + 4)
// RESYNC-KEEP-RUN: o resync so acontece num periodo em que se retiraram no maximo isto da fila (chegadas normais: 1 por
// periodo, 2 com jitter). Depois de uma falha longa o TX descarrega ate 32 pacotes atrasados numa rajada; ancorar no
// mais antigo deixava a fila acima do HIGH_WATER e o rebuffer HIGH disparava logo a seguir (LOW -> HIGH -> HIGH, ate
// 3 em 2 s, no R1/W2 a 30/10).
#define REBUF_STEADY_POPS 2u
// descriptors DMA do canal I2S TX (cc.dma_desc_num). L1 (2026-10-30, docs/LATENCY_REVIEW_2026-10-30.md): o anel esta
// sempre cheio (o loop so e ritmado pela escrita bloqueante), por isso cada descriptor a mais e um bloco (2,66 ms) de
// latencia que nao protege contra o radio; so da folga ao escritor, que precisa de ~1 ms (S medido).
#define I2S_DMA_DESC_NUM ((uint32_t)CONFIG_AUDIOLINK_LINK_RX_I2S_DMA_DESC)
#if CONFIG_AUDIOLINK_LINK_RX_I2S_AUTO_CLEAR
#define I2S_AUTO_CLEAR 1
#else
#define I2S_AUTO_CLEAR 0
#endif

// Cobre ate ~530 s de stream a ~375 pacotes/s (1 bit por bloco); mais do que chega para
// esta fase de testes de bancada de ~60 s. Alem da capacidade, a deteccao de duplicados
// fica desativada para esses blocos (tratados como unicos) em vez de indexar fora do buffer.
#define BITMAP_CAP_BLOCKS 200000u

typedef struct {
    uint8_t buf[LINK_PKT_MAX_BYTES];
    size_t len;
    bool reordered_on_arrival;  // decidido no recebimento (link_rx_on_packet), usado so na reproducao
} rx_item_t;

static QueueHandle_t s_queue;  // xQueueCreate: sempre em RAM interna (pvPortMalloc do IDF = MALLOC_CAP_INTERNAL)
#if CONFIG_AUDIOLINK_LINK_RX_RESYNC_KEEP_RUN
// RESYNC-KEEP-RUN (2026-10-30): a sequencia contigua candidata do REBUFFERING, retirada da fila para ser verificada
// e devolvida a frente dela no resync. BUFFER_BLOCKS itens em PSRAM: so a procura do resync lhe toca.
static rx_item_t *s_rebuf_stage;
#endif
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
// CONCEAL-FADE-01: transicoes do ganho de ocultacao (link_conceal.h), so a tarefa de playout escreve
static uint32_t s_fade_out = 0;  // fades-out espelhados (som -> silencio)
static uint32_t s_fade_in = 0;   // fades-in (silencio -> som), incluindo o do arranque
#endif
#define CONCEAL_FRAMES_LOG2 7
_Static_assert((1u << CONCEAL_FRAMES_LOG2) == LINK_FRAMES_PER_PKT, "link_conceal.h trabalha com blocos de 2^n tramas");

// --- pipeline no recebimento (tarefa Wi-Fi, link_rx_on_packet) ---
static volatile uint32_t s_pkts_recv = 0;     // toda a chamada valida do callback ESP-NOW, incl. duplicados
static volatile uint32_t s_pkts_corrupt = 0;
static volatile uint32_t s_dup = 0;           // sample_ctr ja visto antes (bitmap) - nunca entra na fila
static volatile uint32_t s_unique_recv = 0;   // 1a vez que se ve este sample_ctr ("received_packets")
static volatile uint32_t s_enqueued_count = 0;  // xQueueSend() com sucesso ("enqueued_packets")
static volatile uint32_t s_queue_ovf = 0;     // unico, nao duplicado, mas a fila estava cheia
static uint8_t *s_seen_bitmap;                // 1 bit por indice de bloco (sample_ctr / frames)
// Split (2026-09-30, pedido do Psy): o antigo s_max_enqueued_ctr avancava ANTES de se saber
// se xQueueSend() tinha sucesso, e nunca recuava em queue_ovf - ou seja, media na verdade
// "maior sample_ctr recebido", nao "maior que entrou na fila". Com queue_ovf a dezenas por
// segundo isso inflacionava phase_err para milhares de frames sem relacao com o que estava
// mesmo disponivel para tocar (fila real, max 8 blocos). Agora sao dois contadores
// SEPARADOS, so diagnostico (o controlador usa occupancy real - ver occ_ema em link_rx_run):
static uint64_t s_max_received_ctr = 0;       // maior sample_ctr valido recebido do peer (pos filtro/jump),
                                               // atualizado INDEPENDENTEMENTE do enqueue ter sucesso
static uint64_t s_max_enqueued_ctr = 0;       // maior sample_ctr que REALMENTE entrou na fila (so em
                                               // xQueueSend()==pdTRUE; nao avanca em queue_ovf)
static bool s_have_max_ctr = false;           // true assim que s_max_received_ctr teve o 1o valor real
static uint64_t s_max_seen_block_plus1 = 0;   // (maior indice de bloco visto) + 1, relativo ao bloco 0
static uint64_t s_first_block_idx = 0;        // indice do 1o bloco alguma vez recebido (referencia p/ "nunca recebido")
static bool s_have_ref = false;
static int64_t s_last_recv_us = 0;
static uint32_t s_fs_hz = 0;

// --- PATCH-TX-SESSION (2026-10-01): identidade de sessao do TX (ver link_proto.h) ---
// AUDIT-TX-SESSION encontrou TRES variaveis que assumiam, sem o dizer, que o sample_ctr do TX
// era uma timeline continua desde sempre: s_seen_bitmap (nunca limpo), s_max_received_ctr/
// s_max_enqueued_ctr ("so sobem"), e s_max_seen_block_plus1/s_first_block_idx/s_have_ref (idem,
// usados no calculo de never_recv - o underflow de uint64 que vimos num log, never_recv~=2^64, e
// exatamente este bug: max_seen_block_plus1 ficava preso no pico da sessao antiga enquanto
// s_unique_recv (contador de vida inteira do RX, nao reiniciado por sessao) continuava a crescer).
//
// Ownership/concorrencia (pedido explicito - responde isto por variavel, nao so em prosa):
//   s_have_current_session, s_current_session_id, s_previous_session_id,
//   s_session_pending, s_pending_session_id, s_pending_session_sample_ctr
//     -> SO estas seis vivem sob s_session_lock. E so "handshake": compara-las/mudar-las e um
//        punhado de copias de palavra, nunca um memset nem I/O. link_rx_on_packet (tarefa Wi-Fi)
//        LE as 3 primeiras e ESCREVE as 3 ultimas (so quando deteta uma sessao desconhecida,
//        quando s_session_pending ainda esta false); link_rx_run (tarefa RX) LE as 3 ultimas e
//        ESCREVE as 3 primeiras, uma unica vez por transicao, DEPOIS de todo o reset pesado abaixo
//        estar feito - nunca durante o memset do bitmap, como pedido.
//   s_seen_bitmap, s_max_seen_block_plus1, s_first_block_idx, s_have_ref,
//   s_max_received_ctr, s_max_received_at_us, s_max_enqueued_ctr, s_have_max_ctr
//     -> SEM lock proprio para a transicao de sessao. Protegidos ESTRUTURALMENTE, nao por
//        spinlock: enquanto s_session_pending estiver true, link_rx_on_packet descarta QUALQUER
//        pacote (da sessao antiga, da nova candidata, ou de outra coisa qualquer) logo no topo,
//        ANTES de tocar em qualquer uma destas variaveis (ver a vedacao no inicio da funcao). A
//        tarefa Wi-Fi processa pacotes em serie (uma invocacao do callback termina antes da
//        seguinte comecar) - logo, no instante em que a tarefa RX ve s_session_pending==true e
//        comeca a repor este estado, NENHUMA escrita do lado do callback a estas variaveis pode
//        estar "a meio": ou ja terminou (pacote anterior, sessao ainda valida) ou nunca vai
//        acontecer (pacotes seguintes ja veem pending==true e saem antes de lhes chegar a vez).
//        s_max_received_ctr/at_us continuam, alem disso, sob s_rx_max_lock - mas so pelo motivo
//        ORIGINAL (anterior a este patch): proteger contra a LEITURA concorrente de rx_diag_task/
//        snapshot 1x/s, nao contra o reset da tarefa RX (esse ja esta coberto pela vedacao acima).
//        TENTATIVA ANTERIOR (revertida): meter TUDO (incl. s_max_enqueued_ctr/s_have_max_ctr, so
//        escrita) sob lock "por consistencia" tornava o caminho do pacote mais lento o suficiente
//        para perturbar o timing de ACK do Wi-Fi (send_fail/backpressure/cb_lat subiram muito
//        numa corrida de validacao) - revertido para o minimo estritamente necessario.
//   s_session_unique_recv_base
//     -> SO a tarefa RX toca (escrita na transicao, leitura no preenchimento do snapshot 1x/s).
//        Sem lock - mesmo padrao de outros contadores de uma so tarefa neste ficheiro (s_pl_*).
static portMUX_TYPE s_session_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_have_current_session = false;        // false ate ao 1o pacote valido de sempre
static uint64_t s_current_session_id = 0;          // so significa algo quando s_have_current_session
static uint64_t s_previous_session_id = 0;         // sessao imediatamente anterior - protege contra um
                                                    // pacote atrasado de A reabrir A depois de B ja ter
                                                    // sido aceite (ver link_rx_on_packet)
static bool s_session_pending = false;              // true desde que se deteta uma sessao desconhecida
                                                     // ate a tarefa RX terminar de a adotar
static uint64_t s_pending_session_id = 0;
static uint64_t s_pending_session_sample_ctr = 0;   // sample_ctr do pacote que revelou a nova sessao -
                                                     // usado so para popular s_mac_stats[peer].first_sample_ctr
static volatile uint32_t s_stale_session_drop = 0;  // pacote com session_id == anterior (atrasado) - rejeitado
static volatile uint32_t s_session_pending_drop = 0;  // pacotes descartados enquanto uma transicao esperava
                                                       // pela tarefa RX (inclui o 1o da sessao nova)
static uint64_t s_session_unique_recv_base = 0;     // s_unique_recv no momento da ultima transicao - so a
                                                     // tarefa RX toca; ver never_recv no rx_diag_task

// filtro de peer + limite de plausibilidade (2026-09-30, ver comentarios em
// link_rx_on_packet): pacotes ALK1 bem formados mas de outro MAC, ou com um salto de
// sample_ctr implausivel vindo do MAC certo, sao descartados ANTES de afetar RSSI/
// jitter/dedup/s_max_received_ctr/fila.
static uint8_t s_peer_mac[6];                 // parseado 1x no arranque de link_rx_run()
static volatile uint32_t s_foreign_peer = 0;  // magic/version/tamanho validos, MAC != peer configurado
static volatile uint32_t s_corrupt_jump = 0;  // do peer certo, mas salto de sample_ctr implausivel
static uint32_t s_corrupt_jump_run = 0;       // rejeicoes implausiveis seguidas (so a tarefa Wi-Fi escreve)
static volatile uint32_t s_corrupt_jump_run_max = 0;  // pior sequencia; > ~375 (1 s) = bloqueio a investigar

// jitter/intervalo entre chegadas: janela desde a ultima leitura (reinicia no print)
static volatile uint64_t s_iat_sum_us = 0;
static volatile uint64_t s_iat_jitter_sum_us = 0;
static volatile uint32_t s_iat_count = 0;
static volatile uint32_t s_iat_jitter_max_us = 0;

// --- reproducao (tarefa principal, link_rx_run): descartados por chegarem tarde ---
static volatile uint32_t s_late = 0;     // atrasado, mas chegou na ordem (nao passou nenhum bloco mais recente)
static volatile uint32_t s_reorder = 0;  // genuinamente fora de ordem (chegou depois de um bloco mais recente)
static volatile uint32_t s_hist_1 = 0, s_hist_2 = 0, s_hist_3_4 = 0, s_hist_5_8 = 0, s_hist_gt8 = 0;

static volatile uint32_t s_dac_underrun = 0;  // evento de hardware (on_send_q_ovf): a DMA do DAC ficou sem dados

// STARTUP-QOVF-01 (2026-10-02): timeline do arranque I2S/DMA vs Wi-Fi/ESP-NOW, 1x por boot.
// So timestamps/contadores (0 = ainda nao aconteceu) - nenhum logging dentro de callbacks ou
// do caminho real-time; publicados no snapshot existente e impressos 1x por rx_diag_task.
static int64_t s_t_espnow_ready_us = 0;
static int64_t s_t_i2s_enable_us = 0;
static int64_t s_t_loop_entry_us = 0;
static int64_t s_t_first_i2s_write_begin_us = 0;
static int64_t s_t_first_i2s_write_end_us = 0;
static int64_t s_t_first_on_sent_us = 0;          // escrito sob s_i2s_cb_lock, junto com on_sent
static volatile int64_t s_t_first_q_ovf_us = 0;   // sem lock, mesmo criterio que s_dac_underrun
static uint32_t s_loop_entry_enqueued = 0, s_loop_entry_occ = 0, s_loop_entry_queue_ovf = 0;

// buffer minimo: ocupacao (itens na fila a espera de tocar) amostrada em cada periodo,
// janela desde a ultima leitura (reinicia no print).
// RENOMEADO (2026-09-30, pedido do Psy): "concealment" NAO representa especificamente
// perda de RF - e qualquer vez que o playout repete o ultimo bloco por o bloco esperado
// nao estar disponivel NESTE instante, por DUAS causas distintas, agora separadas:
//   1. s_conceal_queue_empty (fila mesmo vazia - pode ser perda real, ou so o pacote
//      ainda estar a caminho e a chegar no proximo tick; nao se distingue so por isto);
//   2. s_conceal_future_waiting (a fila TEM algo, mas e um bloco FUTURO que ainda nao e a
//      vez dele - o bloco devido nao chegou a tempo, mas outro mais recente ja chegou;
//      isto e mais um sintoma de jitter/reordenacao do que de perda de RF).
// s_playout_concealment = soma das duas (acao real tomada: repetir o ultimo bloco).
static volatile uint32_t s_occ_min = UINT32_MAX, s_occ_max = 0;
static volatile uint64_t s_occ_sum = 0;
static volatile uint32_t s_occ_n = 0;
static volatile uint32_t s_conceal_queue_empty = 0;     // antigo s_buf_underrun
static volatile uint32_t s_conceal_future_waiting = 0;  // NOVO: fila nao-vazia, mas nada devido agora
static volatile uint32_t s_playout_concealment = 0;      // antigo s_concealment; = soma das duas acima

// --- correcao de drift por sample-slip (v1 experimental, 2026-09-30) ---
// Completamente separado do playout_concealment acima: le a ocupacao REAL da fila (ver
// occ_ema/occ_ema_fast em link_rx_run), nunca escreve em expected_ctr. Ver
// docs/PHASE3_LINK.md para o desenho e a simulacao que validou os parametros.
static volatile uint32_t s_drift_insert_frames = 0;
static volatile uint32_t s_drift_drop_frames = 0;
// rebuffer_events = soma dos 4 motivos, separados (pedido do Psy, 2026-09-30/2026-10-01) para se
// poder distinguir arranque, recuperacao normal e mudanca de sessao do TX:
static volatile uint32_t s_rebuffer_events = 0;
static volatile uint32_t s_rebuffer_startup = 0;
static volatile uint32_t s_rebuffer_low = 0;
static volatile uint32_t s_rebuffer_high = 0;
static volatile uint32_t s_rebuffer_session = 0;  // PATCH-TX-SESSION: so a 1a sessao conta como STARTUP;
                                                   // qualquer session_id seguinte conta aqui, nunca nos dois
static volatile uint32_t s_rebuffer_last_duration_us = 0;

// RSSI por pacote (wifi_pkt_rx_ctrl_t), janela desde a ultima leitura - instrumentacao
// passiva para caracterizar condicoes de RF (Psy, 2026-09-30), sem tocar no playout.
#define RSSI_SAMPLE_CAP 1024
static int8_t s_rssi_samples[RSSI_SAMPLE_CAP];
static volatile uint32_t s_rssi_n = 0;

// DIAGNOSTICO TEMPORARIO (Psy, 2026-09-30): um DRIFT com tx_ctr congelado num valor
// absurdo (~562320000, muito acima do sample_ctr real do TX, uptime~3,2h) revelou que
// on_recv_cb() nao filtra por MAC de origem - aceita qualquer pacote ALK1 bem formado,
// de qualquer emissor no canal. Isto so agrega por MAC (tabela pequena, sem log por
// pacote) para identificar se ha mais de um emissor ALK1 antes de decidir o que fazer
// fisicamente. Ainda NAO filtra nada - so observa. Ver link_rx_on_packet().
#define MAC_TRACK_CAP 8
typedef struct {
    uint8_t mac[6];
    volatile uint32_t count;
    volatile uint64_t last_sample_ctr;
    volatile uint64_t first_sample_ctr;  // PATCH-OBS-01: para o log "novo MAC", agora diferido
} mac_stat_t;
static mac_stat_t s_mac_stats[MAC_TRACK_CAP];
static volatile uint32_t s_mac_stats_n = 0;

static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_dac_underrun++;
    if (s_t_first_q_ovf_us == 0) {
        s_t_first_q_ovf_us = esp_timer_get_time();
    }
    return false;
}

// --- INSTR-PLAYOUT-01 (2026-10-01): instrumentacao do caminho playout -> I2S, SEM alteracao
// funcional. Todos os instantes em esp_timer (systimer contador 0) - nunca no timestamp do log,
// que e o tick do FreeRTOS (systimer contador 1, ver esp-idf components/log/src/os/log_timestamp.c).
// Identidades que estes contadores permitem testar (janelas sem rebuffer):
//   out_frames_req   = 128*(normal_blocks + silence_blocks) + inserts - drops
//   out_frames_written ~= out_frames_req (short write => diferenca)
//   d(expected_ctr)  = src_frames_adv + resync_delta,  src_blocks_adv = normal_blocks
//   fill_DMA (frames) = preload_frames + out_frames_written - i2s_bytes_sent/8
// i2s_bytes_sent vem do on_sent do driver: 1 evento por descriptor DMA efetivamente enviado
// para o I2S (esp_driver_i2s/i2s_common.c, i2s_dma_tx_callback) - verdade de hardware.
static uint32_t s_pl_normal_blocks = 0;    // iteracoes no ramo NORMAL (1 bloco logico jogado ou ocultado)
static uint32_t s_pl_silence_blocks = 0;   // iteracoes a escrever silencio (REBUFFERING / pre-buffer)
static uint32_t s_pl_src_blocks_adv = 0;   // vezes que expected_ctr avancou no ramo NORMAL (match ou concealment)
static uint64_t s_pl_src_frames_adv = 0;   // soma dos frames somados a expected_ctr nesse ramo
static uint32_t s_pl_resync_events = 0;    // vezes que expected_ctr foi REDEFINIDO (fim de rebuffer / 1o bloco)
static int64_t s_pl_resync_delta = 0;      // soma de (novo - antigo) expected_ctr nessas redefinicoes
static uint64_t s_out_frames_req = 0;      // frames pedidos a i2s_channel_write
static uint64_t s_out_frames_written = 0;  // frames que i2s_channel_write reportou escritos (bytes_written/8)
static uint32_t s_i2s_short_writes = 0;    // bytes_written < pedido (com portMAX_DELAY so se o canal parar)
// intervalo entre INICIOS de duas chamadas i2s_channel_write consecutivas (cadencia do loop)
static uint32_t s_loop_gap_max_us = 0;          // maximo na janela de stats (1 s), reinicia a cada linha
static uint32_t s_loop_gap_max_ever_us = 0;     // maximo desde o arranque
static int64_t s_loop_gap_max_ever_at_us = 0;   // esp_timer do inicio da escrita que fechou esse pior intervalo
static uint32_t s_loop_gap_over_1p5_tick = 0;   // intervalos > 1,5 periodo de bloco
// L1 (2026-10-30): folga REAL do DMA quando o escritor volta = tramas ainda por tocar no instante em que comeca cada
// escrita (pre-carga + escritas - enviadas pelos fins de descriptor, verdade de hardware). Em regime, (N-1)*128; 0 =
// underrun. Substitui a medida antiga por intervalos, que com um anel pequeno contaria as escritas duplas dos slips.
// So depois do 1.o segundo do loop (o arranque comeca com 1 bloco pre-carregado).
static int32_t s_dma_fill_min = INT32_MAX;       // minimo na janela de stats (1 s)
static int32_t s_dma_fill_min_ever = INT32_MAX;  // minimo desde o arranque
static uint32_t s_loop_deadline_miss = 0;       // escritas que comecaram com < 1 descriptor (128 tramas) por tocar:
                                                // o escritor chegou a menos de 2,66 ms do underrun
// duracao de cada chamada i2s_channel_write (bloqueia enquanto espera descriptor livre)
static uint32_t s_i2s_write_max_us = 0;
static uint32_t s_i2s_write_max_ever_us = 0;
static int64_t s_i2s_write_max_ever_at_us = 0;
static uint32_t s_i2s_write_over_1p5_tick = 0;  // escritas que esperaram mais de 1,5 periodo de bloco
// duracao do bloco de estatisticas/logs (corre no proprio loop de playout)
static uint32_t s_stats_max_us = 0;
static uint32_t s_stats_max_ever_us = 0;

static portMUX_TYPE s_i2s_cb_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_i2s_bytes_sent = 0;   // bytes efetivamente enviados pelo DMA para o I2S (soma de on_sent)
static uint32_t s_i2s_desc_sent = 0;    // n. de eventos on_sent (descriptors concluidos)
static int64_t s_i2s_last_sent_us = 0;  // esp_timer do ultimo on_sent

static bool IRAM_ATTR on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)user_ctx;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_i2s_cb_lock);
    s_i2s_bytes_sent += event->size;
    s_i2s_desc_sent++;
    s_i2s_last_sent_us = now;
    if (s_t_first_on_sent_us == 0) {
        s_t_first_on_sent_us = now;
    }
    portEXIT_CRITICAL_ISR(&s_i2s_cb_lock);
    return false;
}

// par (maior sample_ctr recebido, instante esp_timer da sua chegada), escrito pela tarefa Wi-Fi
// e lido pelo loop de playout - mede o ritmo do relogio de amostragem do TX no esp_timer do RX.
static portMUX_TYPE s_rx_max_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_max_received_at_us = 0;

// --- PATCH-OBS-01 (2026-10-01): snapshot de diagnostico, produtor unico = loop de playout ---
// O loop preenche s_diag_work (so dele) 1x/s e copia-o para s_diag_pub dentro de uma critical
// section curta (sem printf, sem malloc, sem chamadas que esperem). rx_diag_task copia
// s_diag_pub para s_diag_log dentro da mesma critical section e so depois formata e imprime,
// ja sem lock nenhum: se a consola bloquear, bloqueia so o logger.
typedef struct {
    uint32_t reason;  // rebuffer_reason_t (STARTUP/LOW/HIGH/SESSION)
    int64_t dur_us;
    int64_t phase_err_received_trigger;
    double occ_ema_fast_trigger;
    uint32_t occ_before;
    uint32_t occ_after;
    uint32_t discarded;
    uint64_t new_expected_ctr;
} rebuffer_evt_t;

typedef struct {
    uint8_t mac[6];
    uint32_t count;
    uint64_t last_sample_ctr;
    uint64_t first_sample_ctr;
} diag_mac_t;

typedef struct {
    uint32_t seq;      // 1, 2, 3... por publicacao (o logger deteta snapshots que nao chegou a ler)
    int64_t now_us;    // esp_timer no inicio do bloco de stats (LINKRX t/uptime_s, DRIFT uptime_s)
    int64_t t0_us;     // esp_timer no arranque do loop de playout
    // recebimento (tarefa Wi-Fi), lidos como antes (sem lock)
    uint32_t pkts_recv, unique_recv, dup, enqueued, pkts_corrupt, foreign_peer, corrupt_jump, queue_ovf;
    uint64_t max_seen_block_plus1, first_block_idx, max_enqueued_ctr;
    // PATCH-TX-SESSION: ver comentario junto a s_session_lock. session_unique_recv_base e so para o
    // calculo de never_recv (ver rx_diag_task) nao voltar a underflowar depois de uma mudanca de sessao.
    uint64_t session_unique_recv_base;
    uint64_t current_session_id, previous_session_id;
    bool have_current_session;
    uint32_t stale_session_drop, session_pending_drop;
    // janelas desde a publicacao anterior (reiniciadas ao publicar, como antes no print)
    uint64_t iat_sum_us, iat_jitter_sum_us;
    uint32_t iat_count, iat_jitter_max_us;
    uint32_t occ_min, occ_max, occ_n;
    uint64_t occ_sum;
    uint32_t rssi_n;
    int8_t rssi[RSSI_SAMPLE_CAP];
    uint32_t mac_n;
    diag_mac_t mac[MAC_TRACK_CAP];
    // playout
    uint32_t late, reorder, hist_1, hist_2, hist_3_4, hist_5_8, hist_gt8;
    uint32_t playout_concealment, conceal_queue_empty, conceal_future_waiting;
    uint32_t occ_now, dac_underrun, pkts_ok;
    uint64_t expected_ctr;
    bool unmuted;
    // correcao de drift (zeros sem AUDIOLINK_LINK_DRIFT_CORRECTION)
    double occ_ema, occ_ema_fast, ovf_rate_ema;
    uint32_t drift_insert, drift_drop;
    uint32_t rebuffer_events, rebuffer_startup, rebuffer_low, rebuffer_high, rebuffer_session, rebuffer_last_duration_us;
    rebuffer_evt_t rb_last;  // detalhes do ultimo rebuffer concluido (antes: log "REBUFFER fim" no loop)
    // PLAYOUT (INSTR-PLAYOUT-01, mesmos campos)
    int64_t t_us;
    uint64_t max_received_ctr;  // lido sob s_rx_max_lock, junto com max_received_at_us
    int64_t max_received_at_us;
    uint64_t i2s_bytes_sent;
    uint32_t i2s_desc_sent;
    int64_t i2s_last_sent_us;
    uint32_t preload_frames, normal_blocks, silence_blocks, src_blocks_adv, resync_events, short_writes;
    uint64_t src_frames_adv, out_req, out_written;
    int64_t resync_delta;
    uint32_t gap_max_us, gap_max_ever_us, gap_gt1p5tick, deadline_miss;
    int64_t gap_max_ever_at_us;
    uint32_t wr_max_us, wr_max_ever_us, wr_gt1p5tick;
    int64_t wr_max_ever_at_us;
    uint32_t stats_max_us, stats_max_ever_us, pub_cs_max_ever_us, tick_us;
    // STARTUP-QOVF-01: timeline do arranque, 1x por boot (0 = ainda nao aconteceu)
    int64_t t_espnow_ready_us, t_i2s_enable_us, t_loop_entry_us;
    int64_t t_first_i2s_write_begin_us, t_first_i2s_write_end_us;
    int64_t t_first_on_sent_us, t_first_q_ovf_us;
    uint32_t loop_entry_enqueued, loop_entry_occ, loop_entry_queue_ovf;
    // REANCHOR-01: no fim de proposito (ocupa o enchimento final; nao desloca os campos que o playout escreve)
    uint32_t corrupt_jump_run_max;
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
    uint32_t fade_out, fade_in;  // CONCEAL-FADE-01
#endif
    int32_t dma_fill_min, dma_fill_min_ever;  // L1: folga real do DMA (tramas por tocar quando o escritor volta)
} rx_diag_t;

static rx_diag_t s_diag_work;  // so o loop de playout
static rx_diag_t s_diag_pub;   // partilhado, so sob s_diag_lock
static rx_diag_t s_diag_log;   // so rx_diag_task
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_diag_seq = 0;             // escrito so pelo playout
static uint32_t s_pub_cs_max_ever_us = 0;   // pior duracao da publicacao (inclui espera pelo spinlock)
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
static rebuffer_evt_t s_rb_last;            // escrito so pelo playout, no fim de cada rebuffer
static const char *const REBUFFER_REASON_STR[] = {"STARTUP", "LOW", "HIGH", "SESSION"};
#endif

#define RX_DIAG_STACK 6144
#define RX_DIAG_PRIO tskIDLE_PRIORITY  // abaixo do playout (main task, prioridade 1)
#define RX_DIAG_CORE 1                 // playout e tarefa Wi-Fi estao no core 0
#define RX_DIAG_POLL_MS 100

// true se o bloco ja tinha sido visto (bit ja ligado); liga o bit em qualquer caso.
// Fora da capacidade do bitmap: trata sempre como "novo" (sem dedup possivel).
static inline bool bitmap_test_and_set(uint64_t blk) {
    if (blk >= BITMAP_CAP_BLOCKS) {
        return false;
    }
    uint8_t mask = (uint8_t)(1u << (blk & 7));
    uint8_t *byte = &s_seen_bitmap[blk >> 3];
    bool was_set = (*byte & mask) != 0;
    *byte |= mask;
    return was_set;
}

#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
// Sample-slip: insere/remove exatamente 1 frame estereo por interpolacao linear entre
// os 2 frames centrais do bloco - L e R tratados independentemente. Aritmetica em
// int64_t (a soma de dois int32_t perto do full-scale positivo excederia int32_t).
// Chamado "sample-slip"/interpolacao de proposito - NAO e um ASRC completo.
static void slip_insert(const int32_t *in, int32_t *out) {
    const uint32_t MID = LINK_FRAMES_PER_PKT / 2;  // 64: frames 0..63 ficam, 64 e novo
    memcpy(out, in, (size_t)MID * 2 * sizeof(int32_t));
    int64_t l = (int64_t)in[2 * (MID - 1)] + (int64_t)in[2 * MID];      // media(frame63, frame64), L
    int64_t r = (int64_t)in[2 * (MID - 1) + 1] + (int64_t)in[2 * MID + 1];  // idem, R
    out[2 * MID] = (int32_t)(l / 2);
    out[2 * MID + 1] = (int32_t)(r / 2);
    memcpy(out + (MID + 1) * 2, in + MID * 2, (size_t)(LINK_FRAMES_PER_PKT - MID) * 2 * sizeof(int32_t));
}

static void slip_drop(const int32_t *in, int32_t *out) {
    const uint32_t MID = LINK_FRAMES_PER_PKT / 2 - 1;  // 63: frames 0..62 ficam, 63 funde 63+64
    memcpy(out, in, (size_t)MID * 2 * sizeof(int32_t));
    int64_t l = (int64_t)in[2 * MID] + (int64_t)in[2 * (MID + 1)];
    int64_t r = (int64_t)in[2 * MID + 1] + (int64_t)in[2 * (MID + 1) + 1];
    out[2 * MID] = (int32_t)(l / 2);
    out[2 * MID + 1] = (int32_t)(r / 2);
    memcpy(out + (MID + 1) * 2, in + (MID + 2) * 2,
           (size_t)(LINK_FRAMES_PER_PKT - MID - 2) * 2 * sizeof(int32_t));
}
#endif

#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
// CONCEAL-FADE-01: link_conceal_block + contagem das transicoes do ganho (so a tarefa de playout chama isto)
static int32_t *conceal_out(link_conceal_t *st, bool real, int32_t *last_block, int32_t *buf, int32_t *silence) {
    const bool was_audible = st->audible;
    int32_t *o = link_conceal_block(st, real, last_block, buf, silence, CONCEAL_FRAMES_LOG2);
    if (st->audible != was_audible) {
        if (st->audible) {
            s_fade_in++;
        } else {
            s_fade_out++;
        }
    }
    return o;
}
#endif

#if CONFIG_AUDIOLINK_LINK_RX_RESYNC_KEEP_RUN
// RESYNC-KEEP-RUN: a fila do RX vista por link_resync.h (so o ramo REBUFFERING do playout usa isto)
static bool rebuf_q_recv(void *q, void *item) {
    return xQueueReceive((QueueHandle_t)q, item, 0) == pdTRUE;
}
static bool rebuf_q_send_front(void *q, const void *item) {
    return xQueueSendToFront((QueueHandle_t)q, item, 0) == pdTRUE;
}
static uint64_t rebuf_item_ctr(const void *item) {
    return ((const link_pkt_hdr_t *)((const rx_item_t *)item)->buf)->sample_ctr;
}
#endif

// Chamado pela tarefa Wi-Fi (via link_espnow.c) por cada pacote ESP-NOW recebido.
// Curto de proposito: valida, classifica (duplicado/reordenado, jitter) e entrega a
// fila; nunca bloqueia. Duplicados sao filtrados AQUI, nunca ocupam uma posicao da fila.
void link_rx_on_packet(const uint8_t *data, size_t len, int8_t rssi, const uint8_t src_mac[6]) {  LINK_RX_TEST_DEAF_HOOK();  // R2: inerte em producao (link_plaus.h)
    s_pkts_recv++;
    int64_t now = esp_timer_get_time();

    if (len < sizeof(link_pkt_hdr_t) || len > LINK_PKT_MAX_BYTES) {
        s_pkts_corrupt++;
        return;
    }
    const link_pkt_hdr_t *hdr = (const link_pkt_hdr_t *)data;
    if (hdr->magic != LINK_PROTO_MAGIC || (hdr->version != LINK_PROTO_VERSION && hdr->version != ALK_VERSION_V3)) {
        s_pkts_corrupt++;  // nao e um pacote deste protocolo (v2 ou v3): lixo no canal, ou TX desatualizado
        return;
    }
    if (hdr->frames == 0 || hdr->frames > LINK_FRAMES_PER_PKT ||
        sizeof(link_pkt_hdr_t) + alk_payload_bytes(hdr->version, hdr->frames) != len) {
        s_pkts_corrupt++;
        return;
    }

    // Diagnostico temporario de MAC de origem (ver comentario junto a s_mac_stats acima).
    // So agrega/alerta - ainda nao rejeita nada aqui.
    if (src_mac != NULL) {
        int idx = -1;
        for (uint32_t i = 0; i < s_mac_stats_n; i++) {
            if (memcmp(s_mac_stats[i].mac, src_mac, 6) == 0) {
                idx = (int)i;
                break;
            }
        }
        if (idx < 0 && s_mac_stats_n < MAC_TRACK_CAP) {
            idx = (int)s_mac_stats_n;
            memcpy(s_mac_stats[idx].mac, src_mac, 6);
            s_mac_stats[idx].count = 0;
            s_mac_stats[idx].first_sample_ctr = hdr->sample_ctr;
            s_mac_stats_n = s_mac_stats_n + 1;
            // PATCH-OBS-01: o aviso "novo MAC" e impresso por rx_diag_task (via snapshot), nao aqui.
        }
        if (idx >= 0) {
            s_mac_stats[idx].count++;
            s_mac_stats[idx].last_sample_ctr = hdr->sample_ctr;
        }
    }

    // Filtro de peer (2026-09-30): so aceita audio do MAC TX configurado
    // (AUDIOLINK_LINK_PEER_MAC). Sem isto, qualquer emissor ALK1 bem formado no canal
    // conseguia entrar na fila e mexer em s_max_enqueued_ctr - descoberto por um
    // tx_ctr congelado num valor absurdo. Conta e descarta ANTES de RSSI/jitter/
    // dedup/s_max_enqueued_ctr/fila - nao deixa rasto nenhum no resto do pipeline.
    if (src_mac == NULL || memcmp(src_mac, s_peer_mac, 6) != 0) {
        s_foreign_peer++;
        return;
    }

    // PATCH-TX-SESSION (2026-10-01): classifica a sessao ANTES de tocar em qualquer estado scoped a
    // sessao (RSSI exceptua-se - ver abaixo). So DETETA e SINALIZA uma sessao desconhecida; nunca faz
    // o memset/flush/reset aqui - isso e trabalho exclusivo da tarefa RX (inicio do loop em
    // link_rx_run). Ver o comentario de ownership junto a s_session_lock para o porque disto ser
    // seguro sem lock nenhum sobre o bitmap/contadores: enquanto s_session_pending estiver true,
    // QUALQUER pacote (da sessao antiga incluida) e descartado aqui, antes de chegar ao bitmap.
    {
        bool accept_as_current = false;
        bool is_stale = false;
        portENTER_CRITICAL(&s_session_lock);
        if (s_session_pending) {
            // ja ha uma transicao a espera da tarefa RX - nao tocar em mais nada (nem bitmap, nem
            // max_received/enqueued, nem have_ref) ate ela publicar a nova sessao como "current".
        } else if (s_have_current_session && hdr->session_id == s_current_session_id) {
            accept_as_current = true;
        } else if (s_have_current_session && hdr->session_id == s_previous_session_id) {
            is_stale = true;
        } else {
            // sessao desconhecida (1a de sempre, ou um reboot do TX) - fica pendente; a tarefa RX
            // trata disto no topo do proprio loop. Perder este pacote (e os poucos que cheguem
            // antes da tarefa RX reagir) e aceitavel - o rebuffer existe precisamente para isto.
            s_session_pending = true;
            s_pending_session_id = hdr->session_id;
            s_pending_session_sample_ctr = hdr->sample_ctr;
        }
        portEXIT_CRITICAL(&s_session_lock);
        if (is_stale) {
            s_stale_session_drop++;  // pacote atrasado de uma sessao ja abandonada - nunca reabre A depois de B
            return;
        }
        if (!accept_as_current) {
            s_session_pending_drop++;
            return;
        }
    }

    // RSSI e independente de dedup/reordenacao/sessao - e uma propriedade fisica deste frame.
    {
        uint32_t n = s_rssi_n;
        if (n < RSSI_SAMPLE_CAP) {
            s_rssi_samples[n] = rssi;
            s_rssi_n = n + 1;
        }
    }

    if (!s_have_ref) {
        s_fs_hz = audio_clk_get().sample_rate_hz;
        s_first_block_idx = hdr->sample_ctr / hdr->frames;
        s_last_recv_us = now;
        s_have_ref = true;
    } else {
        int64_t iat = now - s_last_recv_us;
        s_last_recv_us = now;
        int64_t nominal = (int64_t)((uint64_t)hdr->frames * 1000000ull / s_fs_hz);
        int64_t jit = iat - nominal;
        if (jit < 0) {
            jit = -jit;
        }
        s_iat_sum_us += (uint64_t)iat;
        s_iat_jitter_sum_us += (uint64_t)jit;
        s_iat_count++;
        if ((uint32_t)jit > s_iat_jitter_max_us) {
            s_iat_jitter_max_us = (uint32_t)jit;
        }
    }

    // Limite de plausibilidade (2026-09-30): um evento real mostrou este contador
    // congelado em ~562320000 (muito acima do sample_ctr real do TX). A causa exata NAO
    // esta determinada (802.11/ESP-NOW ja tem FCS ao nivel MAC - um frame fisicamente
    // corrompido normalmente nem chega a esta camada; nao assumir "bit-flip no ar" sem
    // evidencia). Isto e uma defesa independente da causa: qualquer salto implausivel
    // para a frente e rejeitado antes de poder envenenar s_max_received_ctr (que so sobe).
    // So se aplica DEPOIS de ja haver uma base real (senao o 1o pacote legitimo de um TX
    // que ja estava a correr ha muito tempo seria injustamente rejeitado).
    // REANCHOR-01 (2026-10-29, docs/RX_REANCHOR_PROPOSAL.md): o limite passou a somar o tempo real decorrido desde
    // que s_max_received_ctr foi aceite. Com o limite fixo, uma falha de radio > MAX_JUMP_S com o TX a correr
    // deixava todos os pacotes seguintes implausiveis para sempre (RX mudo ate um reinicio; W2 23:11:26).
    if (s_have_max_ctr) {
        if (link_ctr_implausible(hdr->sample_ctr, s_max_received_ctr, now, LINK_RX_TEST_REF_AT(now, s_max_received_at_us), s_fs_hz,
                                 CONFIG_AUDIOLINK_LINK_RX_MAX_JUMP_S)) {
            s_corrupt_jump++;
            if (++s_corrupt_jump_run > s_corrupt_jump_run_max) {
                s_corrupt_jump_run_max = s_corrupt_jump_run;  // visivel no LINKRX: rejeicoes seguidas, pior caso
            }
            return;  // nao entra na fila, nao mexe em nenhum dos dois contadores
        }
        s_corrupt_jump_run = 0;
    }

    uint64_t blk = hdr->sample_ctr / hdr->frames;  // grelha fixa nesta fase: sample_ctr e sempre multiplo de frames
    if (bitmap_test_and_set(blk)) {
        s_dup++;
        return;  // duplicado: nunca entra na fila
    }
    s_unique_recv++;
    if (blk + 1 > s_max_seen_block_plus1) {
        s_max_seen_block_plus1 = blk + 1;
    }

    static rx_item_t item;  // static: ~1 KB, evitar na stack da tarefa Wi-Fi; xQueueSend copia o conteudo
    item.reordered_on_arrival = (hdr->sample_ctr < s_max_received_ctr);
    // PATCH-TX-SESSION: nao e preciso lock extra aqui para proteger contra o reset da tarefa RX -
    // a mesma vedacao do pending (ver comentario de ownership junto a s_session_lock) ja garante
    // que esta funcao so chega aqui (accept_as_current) quando a tarefa RX NAO esta a meio de um
    // reset. s_rx_max_lock continua a existir (inalterado) so pelo motivo original: proteger
    // max_received_ctr/at_us contra a LEITURA concorrente de rx_diag_task/snapshot, 1x/s.
    if (hdr->sample_ctr > s_max_received_ctr) {
        portENTER_CRITICAL(&s_rx_max_lock);
        s_max_received_ctr = hdr->sample_ctr;  // diagnostico: recebido, independente do enqueue ter sucesso
        s_max_received_at_us = now;            // INSTR-PLAYOUT-01: chegada em esp_timer (now = topo da funcao)
        portEXIT_CRITICAL(&s_rx_max_lock);
    }
    s_have_max_ctr = true;
    item.len = alk_to_v2_layout(data, item.buf);  // v3 -> layout v2 (int32); v2 copiado tal como chegou
    if (xQueueSend(s_queue, &item, 0) == pdTRUE) {
        s_enqueued_count++;
        if (hdr->sample_ctr > s_max_enqueued_ctr) {
            s_max_enqueued_ctr = hdr->sample_ctr;  // diagnostico: so avanca em enqueue com sucesso
        }
    } else {
        s_queue_ovf++;  // s_max_enqueued_ctr NAO avanca aqui - e exatamente o ponto do split
    }
}

// PATCH-OBS-01: unico sitio que formata/imprime o diagnostico periodico do RX. Le so o snapshot
// publicado pelo playout (+ canal Wi-Fi e heap, consultados aqui). Pode bloquear na consola o
// tempo que for preciso; o playout nunca espera por ele. Linhas LINKRX/ALK_SRC_SUMMARY/PLAYOUT/
// DRIFT com o mesmo formato de INSTR-PLAYOUT-01; PLAYOUT ganha campos log_* no fim.
static void rx_diag_task(void *arg) {
    (void)arg;
    rx_diag_t *d = &s_diag_log;
    uint32_t last_seq = 0;
    uint32_t log_missed = 0;          // snapshots publicados que o logger nunca chegou a copiar
    uint32_t mac_logged_n = 0;
    uint32_t prev_print_us = 0;       // duracao do ciclo de impressao anterior (o logger e que bloqueia)
    uint32_t print_max_ever_us = 0;
    int64_t next_drift_us = 0;
    int64_t next_mem_us = 0;
    uint64_t session_logged = 0;      // PATCH-TX-SESSION: ultimo current_session_id ja impresso
    bool have_session_logged = false;
    bool startup_timeline_logged = false;  // STARTUP-QOVF-01: timeline do arranque, 1x por boot
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
    uint32_t rb_logged = 0;
#else
    bool prebuffer_logged = false;
#endif
#if CONFIG_AUDIOLINK_PIN_XSMT >= 0
    bool unmute_logged = false;
#endif
    // LED de estado: criado aqui (core 1, prioridade 0, fora do playout que corre no core 0) para a interrupcao do
    // RMT ficar neste core. Atualizado 1x por snapshot (~1 s): erro real = pacote corrompido, sample_ctr
    // implausivel ou underrun do DAC nesta janela; peer = chegaram pacotes novos nesta janela.
    al_led_init(AL_LED_ROLE_RX);
    uint32_t led_prev_recv = 0, led_prev_err = 0;
    bool led_first = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RX_DIAG_POLL_MS));
        bool fresh = false;
        portENTER_CRITICAL(&s_diag_lock);
        if (s_diag_pub.seq != last_seq) {
            *d = s_diag_pub;
            fresh = true;
        }
        portEXIT_CRITICAL(&s_diag_lock);
        if (!fresh) {
            continue;
        }
        const int64_t t_copy = esp_timer_get_time();
        if (last_seq != 0 && d->seq - last_seq > 1) {
            log_missed += d->seq - last_seq - 1;
        }
        last_seq = d->seq;
        {
            const uint32_t led_err = d->pkts_corrupt + d->corrupt_jump + d->dac_underrun;
            al_led_update((al_led_obs_t){.booting = led_first, .error_now = !led_first && led_err != led_prev_err,
                                         .peer_ok = !led_first && d->unique_recv != led_prev_recv});
            led_prev_recv = d->unique_recv;
            led_prev_err = led_err;
            led_first = false;
        }
        if (next_drift_us == 0) {
            next_drift_us = d->t0_us + (int64_t)CONFIG_AUDIOLINK_LINK_RX_DRIFT_LOG_S * 1000000;
        }

        // --- eventos (antes impressos no instante, pela tarefa que os detetava) ---
        // STARTUP-QOVF-01: timeline do arranque, 1x por boot, so depois de termos a 1a escrita
        // concluida (nessa altura, 1s+ ja passou desde loop_entry - t_first_on_sent_us e
        // t_first_q_ovf_us, se iam acontecer, ja aconteceram).
        if (!startup_timeline_logged && d->t_first_i2s_write_end_us != 0) {
            char on_sent_buf[24], q_ovf_buf[24];
            if (d->t_first_on_sent_us != 0) {
                snprintf(on_sent_buf, sizeof(on_sent_buf), "+%lldus",
                         (long long)(d->t_first_on_sent_us - d->t_i2s_enable_us));
            } else {
                snprintf(on_sent_buf, sizeof(on_sent_buf), "N/D");
            }
            if (d->t_first_q_ovf_us != 0) {
                snprintf(q_ovf_buf, sizeof(q_ovf_buf), "+%lldus",
                         (long long)(d->t_first_q_ovf_us - d->t_i2s_enable_us));
            } else {
                snprintf(q_ovf_buf, sizeof(q_ovf_buf), "nenhum");
            }
            ESP_LOGW(TAG, "STARTUP timeline (ref=i2s_enable): espnow_ready=%lldus "
                          "espnow_ready->loop_entry=%lldus loop_entry=+%lldus "
                          "1a_write_begin=+%lldus 1a_write_end=+%lldus "
                          "1o_on_sent=%s 1o_q_ovf=%s dac_underrun_total=%lu | "
                          "loop_entry: rx_enqueued=%lu rx_queue_occ=%lu rx_queue_ovf=%lu",
                     (long long)(d->t_espnow_ready_us - d->t_i2s_enable_us),
                     (long long)(d->t_loop_entry_us - d->t_espnow_ready_us),
                     (long long)(d->t_loop_entry_us - d->t_i2s_enable_us),
                     (long long)(d->t_first_i2s_write_begin_us - d->t_i2s_enable_us),
                     (long long)(d->t_first_i2s_write_end_us - d->t_i2s_enable_us),
                     on_sent_buf, q_ovf_buf, (unsigned long)d->dac_underrun,
                     (unsigned long)d->loop_entry_enqueued, (unsigned long)d->loop_entry_occ,
                     (unsigned long)d->loop_entry_queue_ovf);
            startup_timeline_logged = true;
        }
        for (; mac_logged_n < d->mac_n; mac_logged_n++) {
            const diag_mac_t *m = &d->mac[mac_logged_n];
            ESP_LOGW(TAG, "ALK_SRC novo MAC de origem visto: %02X:%02X:%02X:%02X:%02X:%02X (sample_ctr=%llu)",
                     m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5],
                     (unsigned long long)m->first_sample_ctr);
        }
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
        if (d->rebuffer_events != rb_logged) {
            const rebuffer_evt_t *e = &d->rb_last;
            ESP_LOGI(TAG, "REBUFFER fim: motivo=%s duracao_us=%lld phase_err_recebido_gatilho=%lld "
                          "occ_ema_fast_gatilho=%.2f occ_antes=%lu occ_depois=%lu "
                          "blocos_descartados=%lu novo_expected_ctr=%llu eventos=%lu",
                     e->reason < 4 ? REBUFFER_REASON_STR[e->reason] : "?", (long long)e->dur_us,
                     (long long)e->phase_err_received_trigger, e->occ_ema_fast_trigger,
                     (unsigned long)e->occ_before, (unsigned long)e->occ_after, (unsigned long)e->discarded,
                     (unsigned long long)e->new_expected_ctr, (unsigned long)(d->rebuffer_events - rb_logged));
            rb_logged = d->rebuffer_events;
        }
#else
        if (!prebuffer_logged && d->resync_events > 0) {
            ESP_LOGI(TAG, "buffer inicial cheio (%lu blocos): playout arrancou", (unsigned long)BUFFER_BLOCKS);
            prebuffer_logged = true;
        }
#endif
#if CONFIG_AUDIOLINK_PIN_XSMT >= 0
        if (!unmute_logged && d->unmuted) {
            ESP_LOGI(TAG, "XSMT alto: un-mute");
            unmute_logged = true;
        }
#endif
        // PATCH-TX-SESSION: evento diferido de mudanca de sessao (antes impresso no instante seria
        // na tarefa Wi-Fi ou na tarefa RX - nenhuma das duas deve imprimir, ver ownership acima).
        if (d->have_current_session && (!have_session_logged || d->current_session_id != session_logged)) {
            ESP_LOGW(TAG, "SESSAO TX %s: session_id=%016llX (anterior=%016llX)",
                     have_session_logged ? "mudou" : "adquirida",
                     (unsigned long long)d->current_session_id, (unsigned long long)d->previous_session_id);
            session_logged = d->current_session_id;
            have_session_logged = true;
        }

        // "nunca recebido ate agora, NESTA sessao": blocos entre o 1o alguma vez visto e o mais
        // recente visto, NA SESSAO ATUAL, que ainda nao apareceram nem uma vez (podem chegar mais
        // tarde - convergem para "genuinamente nunca" perto do fim da sessao, nao antes).
        // PATCH-TX-SESSION: max_seen_block_plus1/first_block_idx sao reiniciados a cada sessao, mas
        // unique_recv (d->unique_recv) e de vida inteira do RX - sem o session_unique_recv_base a
        // subtracao desalinhava-se (session pequena - lifetime grande) e dava o MESMO underflow de
        // uint64 que o bug original, so que por um caminho diferente.
        uint64_t session_unique_recv = d->unique_recv - d->session_unique_recv_base;
        uint64_t never_recv = d->max_seen_block_plus1 > d->first_block_idx
                                   ? (d->max_seen_block_plus1 - d->first_block_idx) - session_unique_recv
                                   : 0;
        uint32_t iat_avg = d->iat_count ? (uint32_t)(d->iat_sum_us / d->iat_count) : 0;
        uint32_t iat_jitter_avg = d->iat_count ? (uint32_t)(d->iat_jitter_sum_us / d->iat_count) : 0;

        // RSSI: ordena a copia do snapshot, tira avg/p5/min/p95 - janela desde a publicacao anterior
        uint32_t rn = d->rssi_n;
        int rssi_avg = 0, rssi_min = 0, rssi_p5 = 0, rssi_p95 = 0;
        if (rn > 0) {
            qsort(d->rssi, rn, sizeof(int8_t), cmp_i8);
            int32_t sum = 0;
            for (uint32_t i = 0; i < rn; i++) {
                sum += d->rssi[i];
            }
            rssi_avg = sum / (int32_t)rn;
            rssi_min = d->rssi[0];
            rssi_p5 = d->rssi[(rn * 5) / 100];
            rssi_p95 = d->rssi[(rn * 95) / 100 < rn ? (rn * 95) / 100 : rn - 1];
        }

        uint8_t chan = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&chan, &sec);

        uint32_t occ_min = d->occ_n ? d->occ_min : 0;
        uint32_t occ_avg = d->occ_n ? (uint32_t)(d->occ_sum / d->occ_n) : 0;

        ESP_LOGI(TAG, "LINKRX t=%d recv_total=%lu received_packets=%lu duplicate=%lu enqueued_packets=%lu never_recv=%llu ok=%lu "
                      "late_drop=%lu reorder=%lu hist[1,2,3-4,5-8,>8]=%lu,%lu,%lu,%lu,%lu "
                      "corrupt=%lu foreign_peer=%lu implausible_ctr=%lu implausible_run_max=%lu queue_ovf=%lu "
                      "playout_concealment=%lu conceal_queue_empty=%lu conceal_future_waiting=%lu "
                      "occ_now=%lu occ_min=%lu occ_avg=%lu occ_max=%lu dac_underrun=%lu "
                      "expected_ctr=%llu max_received_ctr=%llu max_enqueued_ctr=%llu "
                      "iat_avg_us=%lu iat_jitter_avg_us=%lu iat_jitter_max_us=%lu "
                      "rssi_min=%d rssi_p5=%d rssi_avg=%d rssi_p95=%d chan=%u "
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
                      "occ_ema=%.2f occ_ema_fast=%.2f drift_insert_frames=%lu drift_drop_frames=%lu "
                      "rebuffer_events=%lu rebuffer_startup=%lu rebuffer_low=%lu rebuffer_high=%lu "
                      "rebuffer_session=%lu rebuffer_last_duration_us=%lu "
#endif
                      "session_id=%016llX stale_session_drop=%lu session_pending_drop=%lu "
                      "heap_free=%lu heap_min=%lu uptime_s=%lld",
                 (int)((d->now_us - d->t0_us) / 1000000), (unsigned long)d->pkts_recv,
                 (unsigned long)d->unique_recv, (unsigned long)d->dup, (unsigned long)d->enqueued,
                 (unsigned long long)never_recv, (unsigned long)d->pkts_ok,
                 (unsigned long)d->late, (unsigned long)d->reorder, (unsigned long)d->hist_1,
                 (unsigned long)d->hist_2, (unsigned long)d->hist_3_4, (unsigned long)d->hist_5_8,
                 (unsigned long)d->hist_gt8, (unsigned long)d->pkts_corrupt, (unsigned long)d->foreign_peer,
                 (unsigned long)d->corrupt_jump, (unsigned long)d->corrupt_jump_run_max, (unsigned long)d->queue_ovf,
                 (unsigned long)d->playout_concealment, (unsigned long)d->conceal_queue_empty,
                 (unsigned long)d->conceal_future_waiting, (unsigned long)d->occ_now,
                 (unsigned long)occ_min, (unsigned long)occ_avg, (unsigned long)d->occ_max,
                 (unsigned long)d->dac_underrun, (unsigned long long)d->expected_ctr,
                 (unsigned long long)d->max_received_ctr, (unsigned long long)d->max_enqueued_ctr,
                 (unsigned long)iat_avg,
                 (unsigned long)iat_jitter_avg, (unsigned long)d->iat_jitter_max_us, rssi_min, rssi_p5, rssi_avg,
                 rssi_p95, (unsigned)chan,
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
                 d->occ_ema, d->occ_ema_fast,
                 (unsigned long)d->drift_insert, (unsigned long)d->drift_drop,
                 (unsigned long)d->rebuffer_events, (unsigned long)d->rebuffer_startup,
                 (unsigned long)d->rebuffer_low, (unsigned long)d->rebuffer_high,
                 (unsigned long)d->rebuffer_session, (unsigned long)d->rebuffer_last_duration_us,
#endif
                 (unsigned long long)d->current_session_id, (unsigned long)d->stale_session_drop,
                 (unsigned long)d->session_pending_drop,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size(), (long long)(d->now_us / 1000000));

        // ALK_SRC: resumo agregado por MAC de origem (diagnostico temporario, ver
        // comentario junto a s_mac_stats) - uma linha por MAC conhecido, 1x/s, nunca
        // por pacote.
        for (uint32_t i = 0; i < d->mac_n; i++) {
            const diag_mac_t *m = &d->mac[i];
            ESP_LOGI(TAG, "ALK_SRC_SUMMARY mac=%02X:%02X:%02X:%02X:%02X:%02X count=%lu last_sample_ctr=%llu",
                     m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5], (unsigned long)m->count,
                     (unsigned long long)m->last_sample_ctr);
        }

        // PLAYOUT (INSTR-PLAYOUT-01): contadores monotonicos do caminho playout -> I2S, todos com
        // instantes em esp_timer. stats_max_us = pior bloco de stats/publicacao da janela anterior
        // (desde PATCH-OBS-01 ja nao inclui impressao). Campos log_*: estado do proprio logger.
        ESP_LOGI(TAG, "PLAYOUT t_us=%lld exp_ctr=%llu rx_max_ctr=%llu rx_max_at_us=%lld "
                      "i2s_bytes_sent=%llu i2s_desc_sent=%lu i2s_last_sent_us=%lld preload_frames=%lu "
                      "normal_blocks=%lu silence_blocks=%lu src_blocks_adv=%lu src_frames_adv=%llu "
                      "resync_events=%lu resync_delta=%lld out_req=%llu out_written=%llu "
                      "short_writes=%lu ins=%lu drp=%lu gap_max_us=%lu gap_max_ever_us=%lu "
                      "gap_max_ever_at_us=%lld gap_gt1p5tick=%lu deadline_miss=%lu wr_max_us=%lu "
                      "wr_max_ever_us=%lu wr_max_ever_at_us=%lld wr_gt1p5tick=%lu stats_max_us=%lu "
                      "stats_max_ever_us=%lu q_ovf=%lu tick_us=%lu "
                      "pub_seq=%lu pub_cs_max_ever_us=%lu log_missed=%lu log_lag_us=%lld "
                      "log_prev_print_us=%lu log_print_max_ever_us=%lu log_stack_free=%lu fade_out=%lu fade_in=%lu "
                      "dma_desc=%lu auto_clear=%d dma_fill_min=%ld dma_fill_min_ever=%ld",
                 (long long)d->t_us, (unsigned long long)d->expected_ctr, (unsigned long long)d->max_received_ctr,
                 (long long)d->max_received_at_us, (unsigned long long)d->i2s_bytes_sent,
                 (unsigned long)d->i2s_desc_sent, (long long)d->i2s_last_sent_us, (unsigned long)d->preload_frames,
                 (unsigned long)d->normal_blocks, (unsigned long)d->silence_blocks,
                 (unsigned long)d->src_blocks_adv, (unsigned long long)d->src_frames_adv,
                 (unsigned long)d->resync_events, (long long)d->resync_delta,
                 (unsigned long long)d->out_req, (unsigned long long)d->out_written,
                 (unsigned long)d->short_writes, (unsigned long)d->drift_insert, (unsigned long)d->drift_drop,
                 (unsigned long)d->gap_max_us, (unsigned long)d->gap_max_ever_us,
                 (long long)d->gap_max_ever_at_us, (unsigned long)d->gap_gt1p5tick,
                 (unsigned long)d->deadline_miss, (unsigned long)d->wr_max_us,
                 (unsigned long)d->wr_max_ever_us, (long long)d->wr_max_ever_at_us,
                 (unsigned long)d->wr_gt1p5tick, (unsigned long)d->stats_max_us,
                 (unsigned long)d->stats_max_ever_us, (unsigned long)d->dac_underrun,
                 (unsigned long)d->tick_us, (unsigned long)d->seq, (unsigned long)d->pub_cs_max_ever_us,
                 (unsigned long)log_missed, (long long)(t_copy - d->t_us), (unsigned long)prev_print_us,
                 (unsigned long)print_max_ever_us, (unsigned long)uxTaskGetStackHighWaterMark(NULL),
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
                 (unsigned long)d->fade_out, (unsigned long)d->fade_in,
#else
                 0ul, 0ul,
#endif
                 (unsigned long)I2S_DMA_DESC_NUM, I2S_AUTO_CLEAR,
                 (long)d->dma_fill_min, (long)d->dma_fill_min_ever);

        // MEM (2026-10-30): onde estao a fila e a sequencia do resync, e a folga de RAM interna (o Wi-Fi precisa dela).
        // 1x por minuto, so aqui no logger.
        if (d->now_us >= next_mem_us) {
            ESP_LOGI(TAG, "MEM queue=%s queue_bytes=%lu stage=%s int_free=%lu int_min=%lu int_largest=%lu psram_free=%lu",
                     esp_ptr_external_ram(s_queue) ? "psram" : "interna",
                     (unsigned long)(QUEUE_DEPTH * sizeof(rx_item_t)),
#if CONFIG_AUDIOLINK_LINK_RX_RESYNC_KEEP_RUN
                     esp_ptr_external_ram(s_rebuf_stage) ? "psram" : "interna",
#else
                     "n/a",
#endif
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            next_mem_us = d->now_us + 60 * 1000000LL;
        }

        // DRIFT: mesma cadencia de antes (CONFIG_AUDIOLINK_LINK_RX_DRIFT_LOG_S, ancorada no arranque
        // do loop), agora a partir do snapshot de 1 s mais recente. phase_err_* continuam SO
        // diagnostico - nenhum alimenta o controlador.
        if (d->now_us >= next_drift_us) {
            int64_t phase_err_received = (int64_t)d->max_received_ctr - (int64_t)d->expected_ctr;
            int64_t phase_err_enqueued = (int64_t)d->max_enqueued_ctr - (int64_t)d->expected_ctr;
            ESP_LOGI(TAG, "DRIFT uptime_s=%lld max_received_ctr=%llu max_enqueued_ctr=%llu "
                          "expected_ctr=%llu phase_err_received=%lld phase_err_enqueued=%lld "
                          "occ=%lu "
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
                          "occ_ema=%.2f occ_ema_fast=%.2f ovf_rate_ema=%.1f "
#endif
                          "playout_concealment=%lu never_recv=%llu queue_ovf=%lu late_drop=%lu",
                     (long long)(d->now_us / 1000000), (unsigned long long)d->max_received_ctr,
                     (unsigned long long)d->max_enqueued_ctr, (unsigned long long)d->expected_ctr,
                     (long long)phase_err_received, (long long)phase_err_enqueued,
                     (unsigned long)d->occ_now,
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
                     d->occ_ema, d->occ_ema_fast, d->ovf_rate_ema,
#endif
                     (unsigned long)d->playout_concealment,
                     (unsigned long long)never_recv, (unsigned long)d->queue_ovf,
                     (unsigned long)d->late);
            do {
                next_drift_us += (int64_t)CONFIG_AUDIOLINK_LINK_RX_DRIFT_LOG_S * 1000000;
            } while (next_drift_us <= d->now_us);
        }

        prev_print_us = (uint32_t)(esp_timer_get_time() - t_copy);
        if (prev_print_us > print_max_ever_us) {
            print_max_ever_us = prev_print_us;
        }
    }
}

void link_rx_run(void) {
    audio_clk_t clk = audio_clk_get();

    // Peer MAC (2026-09-30): parseado 1x, antes de o callback de recebimento poder ser
    // chamado (link_espnow_rx_init(), mais abaixo, e' o que o regista). Aborta se nao
    // estiver configurado - falhar alto e bom som, como ja fazia o LINK_TX para o MAC do
    // RX, em vez de um RX a "aceitar de ninguem" sem se perceber porque.
    if (!link_espnow_mac_from_str(CONFIG_AUDIOLINK_LINK_PEER_MAC, s_peer_mac)) {
        ESP_LOGE(TAG, "MAC do peer invalido em Kconfig: '%s'", CONFIG_AUDIOLINK_LINK_PEER_MAC);
        abort();
    }
    static const uint8_t zero_mac[6] = {0};
    if (memcmp(s_peer_mac, zero_mac, 6) == 0) {
        ESP_LOGE(TAG, "MAC do peer (TX) nao configurado (00:00:00:00:00:00). Le o MAC no log "
                      "de arranque do LINK_TX (link_espnow: MAC STA deste no) e configura em "
                      "AUDIOLINK_LINK_PEER_MAC.");
        abort();
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(rx_item_t));
    if (!s_queue) {
        abort();
    }
#if CONFIG_AUDIOLINK_LINK_RX_RESYNC_KEEP_RUN
    s_rebuf_stage = heap_caps_malloc(BUFFER_BLOCKS * sizeof(rx_item_t), MALLOC_CAP_SPIRAM);
    if (!s_rebuf_stage) {
        abort();
    }
#endif
    s_seen_bitmap = heap_caps_calloc(BITMAP_CAP_BLOCKS / 8, 1, MALLOC_CAP_SPIRAM);
    if (!s_seen_bitmap) {
        abort();
    }

    // STARTUP-QOVF-01 (2026-10-02): o canal fica MONTADO e PRE-CARREGADO mas DESATIVADO (sem
    // i2s_channel_enable) ate estarmos a poucas instrucoes de entrar no loop que o alimenta.
    // Antes, i2s_channel_enable() corria aqui e o DMA comecava a consumir ~100 ms antes de
    // link_espnow_rx_init() (que inclui esp_wifi_start(), tipicamente dezenas de ms de
    // calibracao de RF) sequer comecar - gerando ~31 eventos on_send_q_ovf deterministicos por
    // arranque. Auditoria confirmou (fonte do driver, esp_driver_i2s/i2s_common.c): os buffers
    // DMA sao calloc'd (silencio valido desde a criacao do canal, independente do preload); o
    // evento so dispara quando um descriptor termina e a fila de buffers livres ja esta cheia,
    // ou seja, quando ninguem chamou i2s_channel_write ha varios periodos de bloco. Adiar o
    // enable() para o fim do bring-up resolve a causa (responsabilidade temporal da tarefa),
    // nao so o sintoma (o contador).
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = I2S_DMA_DESC_NUM;
    cc.dma_frame_num = LINK_FRAMES_PER_PKT;
    cc.auto_clear_after_cb = I2S_AUTO_CLEAR;  // L1: underrun toca silencio, nao audio velho
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx, NULL));

    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = clk.sample_rate_hz, .clk_src = clk.clk_src,
                    .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = AUDIOLINK_DAC_GPIO_MCLK, .bclk = CONFIG_AUDIOLINK_PIN_BCLK,
                     .ws = CONFIG_AUDIOLINK_PIN_WS, .dout = CONFIG_AUDIOLINK_PIN_DATA,
                     .din = I2S_GPIO_UNUSED},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));

    i2s_event_callbacks_t cbs = {.on_sent = on_sent, .on_send_q_ovf = on_send_q_ovf};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx, &cbs, NULL));

#if CONFIG_AUDIOLINK_PIN_XSMT < 0
    ESP_LOGW(TAG, "XSMT NAO comandado pelo S3: mute/un-mute depende da ponte H3L da placa");
#else
    gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_AUDIOLINK_PIN_XSMT, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(CONFIG_AUDIOLINK_PIN_XSMT, 0);
#endif

    static int32_t silence[LINK_FRAMES_PER_PKT * 2];
    memset(silence, 0, sizeof(silence));
    size_t w = 0;
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx, silence, sizeof(silence), &w));
    const uint32_t preload_frames = (uint32_t)(w / LINK_BYTES_PER_FRAME);  // INSTR-PLAYOUT-01: real, nao assumido
    // I2S preparado (canal + clock + callbacks + XSMT baixo + 1 bloco pre-carregado), mas AINDA
    // desativado - ver nota STARTUP-QOVF-01 acima. O DMA so arranca a consumir mais abaixo.

    link_espnow_rx_init();
    s_t_espnow_ready_us = esp_timer_get_time();
    ESP_LOGI(TAG, "a espera do 1o pacote...");

    // PATCH-OBS-01: logger de diagnostico fora do caminho real-time. Se nao for possivel cria-lo,
    // o audio segue sem diagnostico periodico (perder logs e aceitavel, perder audio nao).
    if (xTaskCreatePinnedToCore(rx_diag_task, "rx_diag", RX_DIAG_STACK, NULL, RX_DIAG_PRIO, NULL,
                                RX_DIAG_CORE) == pdPASS) {
        ESP_LOGI(TAG, "rx_diag: tarefa de diagnostico criada (snapshot=%u B, prio=%d, core=%d, stack=%d)",
                 (unsigned)sizeof(rx_diag_t), (int)RX_DIAG_PRIO, RX_DIAG_CORE, RX_DIAG_STACK);
    } else {
        ESP_LOGE(TAG, "rx_diag: falha a criar a tarefa de diagnostico - audio continua sem logs periodicos");
    }

    // STARTUP-QOVF-01: so agora o DMA arranca a consumir. A janela entre link_espnow_rx_init()
    // (acima) ter terminado e o 1o i2s_channel_write() (for(;;), poucas linhas abaixo) e que
    // importa manter minima do lado da RECECAO (risco de queue_ovf em s_queue se o TX ja
    // estiver quente) - por isso o enable() fica o mais perto possivel do loop, nao logo a
    // seguir ao ESP-NOW.
    s_t_i2s_enable_us = esp_timer_get_time();
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    static int32_t last_block[LINK_FRAMES_PER_PKT * 2];
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
    // CONCEAL-FADE-01: todo o periodo, com ou sem bloco real, sai por conceal_out() (link_conceal.h). Arranca em
    // silencio: o 1.o bloco real entra com fade-in.
    static int32_t conceal_buf[LINK_FRAMES_PER_PKT * 2];
    link_conceal_t conceal = {.audible = false};
#endif
#if !CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
    bool have_last = false;  // com drift-correction ativo, isto fica unificado em rx_mode (ver abaixo)
#endif
    uint64_t expected_ctr = 0;
    uint64_t frame_idx = 0;
    bool unmuted = false;

    uint32_t pkts_ok = 0;
    int64_t t0 = esp_timer_get_time();
    int64_t next_stat = t0 + 1000000;
    // INSTR-PLAYOUT-01: periodo nominal de um bloco (us) e instante do inicio da escrita anterior
    const uint32_t tick_us = (uint32_t)(1000000.0 * LINK_FRAMES_PER_PKT / clk.sample_rate_hz);
    int64_t prev_wr0_us = 0;

#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
    // --- estado do corretor de drift (v1b, 2026-09-30: baseado em ocupacao REAL da fila,
    // nao em phase_err derivado de s_max_enqueued_ctr - ver split received/enqueued no topo
    // do ficheiro) + gate RF + rebuffer (LOW/HIGH/STARTUP unificados) ---
    typedef enum { RX_MODE_NORMAL, RX_MODE_REBUFFERING } rx_mode_t;
    typedef enum { REBUFFER_REASON_STARTUP, REBUFFER_REASON_LOW, REBUFFER_REASON_HIGH,
                   REBUFFER_REASON_SESSION } rebuffer_reason_t;  // SESSION: PATCH-TX-SESSION, ver abaixo
    // Arranca diretamente em REBUFFERING/STARTUP: unifica o bootstrap com o rebuffer
    // (pedido explicito do Psy - "especialmente importante quando o RX arranca... enquanto
    // o TX ja em andamento"). O antigo bootstrap simples (!have_last) so pegava no mais
    // antigo dos BUFFER_BLOCKS acumulados, sem exigir contiguidade - podia ancorar
    // expected_ctr num ponto ja stale se a fila tivesse buracos no arranque.
    rx_mode_t rx_mode = RX_MODE_REBUFFERING;
    rebuffer_reason_t rebuffer_reason = REBUFFER_REASON_STARTUP;
    static int32_t slip_buf[(LINK_FRAMES_PER_PKT + 1) * 2];
    // TUNE-TARGET-01 (2026-10-02): TARGET separado de BUFFER_BLOCKS (ver Kconfig). BUFFER_BLOCKS
    // continua a mandar sozinho em QUEUE_DEPTH, na profundidade de contiguidade do bootstrap/
    // rebuffer, e no valor da EMA logo apos um resync (linhas abaixo, inalteradas) - nenhum deles
    // mudou. So o ALVO de regime do controlador passa a ser independente.
    const double target_occ_frames = (double)CONFIG_AUDIOLINK_LINK_DRIFT_TARGET_FRAMES;
    const double tick_period_s = (double)LINK_FRAMES_PER_PKT / clk.sample_rate_hz;
    double occ_ema = (double)BUFFER_BLOCKS;       // EMA lenta da OCUPACAO REAL (blocos) - decisao insert/drop
    double occ_ema_fast = (double)BUFFER_BLOCKS;  // EMA rapida da ocupacao real (blocos) - gatilho LOW/HIGH
    double ovf_rate_ema = 0.0;                    // EMA rapida da taxa de queue_ovf (/s) - gatilho HIGH
    uint32_t last_ovf_seen = s_queue_ovf;
    int64_t last_corr_us = 0;
    bool gate_freeze = false;
    int64_t gate_freeze_until_us = 0;
    // janela deslizante do detetor de rajada do gate RF (timestamps, us) - inalterado,
    // continua ligado a concealment (RF), nao a ocupacao.
    #define RF_WINDOW_CAP 32
    static int64_t rf_window_us[RF_WINDOW_CAP];
    uint32_t rf_window_head = 0, rf_window_count = 0;
    // rebuffer: procura de sequencia CONTIGUA de sample_ctr (nao "4 pacotes quaisquer") -
    // usado tanto no arranque (STARTUP) como em LOW/HIGH, mesmo codigo.
    uint64_t rebuf_run_start = 0;
    uint32_t rebuf_run_count = 0;
    uint32_t rebuf_items_seen = 0;
    uint32_t rebuf_discarded_at_entry = 0;
    int64_t rebuf_start_us = 0;
    int64_t rebuf_phase_err_received_trigger = 0;  // diagnostico (phase_err_received), nao usado p/ decisao
    double rebuf_occ_ema_fast_trigger = 0.0;
    uint32_t rebuf_occ_before = 0;
#endif

    // STARTUP-QOVF-01: estado do lado da RECECAO no instante exato em que o loop de playout
    // comeca a drenar s_queue - responde "o ESP-NOW ja encheu a fila antes de alguem a ler?"
    s_loop_entry_enqueued = s_enqueued_count;
    s_loop_entry_occ = (uint32_t)uxQueueMessagesWaiting(s_queue);
    s_loop_entry_queue_ovf = s_queue_ovf;
    s_t_loop_entry_us = esp_timer_get_time();

    for (;;) {
        static rx_item_t peeked;  // static: ~1 KB, evitar na stack da tarefa
        int32_t *out_block;
        uint32_t frames_this_tick = LINK_FRAMES_PER_PKT;

        // PATCH-TX-SESSION (2026-10-01): processa uma transicao de sessao pendente, se houver -
        // ANTES de tudo o resto do loop, todas as iteracoes (mesmo a meio de um REBUFFERING ja em
        // curso: um 2o reboot do TX antes de terminar de resincronizar com o 1o reinicia tudo outra
        // vez, de proposito). Unico sitio que faz o trabalho pesado (memset do bitmap, reset dos
        // contadores scoped a sessao, flush da fila) - nunca a tarefa Wi-Fi. Ver o comentario de
        // ownership junto a s_session_lock para a prova de que isto e seguro sem lock sobre o
        // bitmap: a tarefa Wi-Fi nao toca em nada disto enquanto s_session_pending for true, e so
        // ela pode tornar pending=true (nunca esta tarefa).
        {
            bool do_transition = false;
            uint64_t new_session_id = 0, new_session_sample_ctr = 0;
            portENTER_CRITICAL(&s_session_lock);
            if (s_session_pending) {
                do_transition = true;
                new_session_id = s_pending_session_id;
                new_session_sample_ctr = s_pending_session_sample_ctr;
            }
            portEXIT_CRITICAL(&s_session_lock);
            if (do_transition) {
                bool was_first_session = !s_have_current_session;  // so esta tarefa escreve isto
                const int64_t t_memset0 = esp_timer_get_time();
                memset(s_seen_bitmap, 0, BITMAP_CAP_BLOCKS / 8);
                portENTER_CRITICAL(&s_rx_max_lock);
                s_max_received_ctr = 0;
                s_max_received_at_us = 0;
                s_max_enqueued_ctr = 0;
                s_have_max_ctr = false;
                portEXIT_CRITICAL(&s_rx_max_lock);
                s_max_seen_block_plus1 = 0;
                s_first_block_idx = 0;
                s_have_ref = false;
                s_session_unique_recv_base = s_unique_recv;  // ver never_recv em rx_diag_task
                for (uint32_t i = 0; i < s_mac_stats_n; i++) {
                    if (memcmp(s_mac_stats[i].mac, s_peer_mac, 6) == 0) {
                        s_mac_stats[i].first_sample_ctr = new_session_sample_ctr;
                        s_mac_stats[i].last_sample_ctr = new_session_sample_ctr;
                        break;
                    }
                }
                uint32_t flushed = 0;
                while (xQueueReceive(s_queue, &peeked, 0) == pdTRUE) {
                    flushed++;
                }
                const uint32_t memset_us = (uint32_t)(esp_timer_get_time() - t_memset0);
                (void)memset_us;  // nao exposto no snapshot (evento raro); medido so para auditoria

#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
                rebuffer_reason = was_first_session ? REBUFFER_REASON_STARTUP : REBUFFER_REASON_SESSION;
                rebuf_discarded_at_entry = flushed;
                rebuf_occ_before = flushed;          // nao ha gatilho LOW/HIGH aqui - usa o que foi descartado
                rebuf_phase_err_received_trigger = 0;  // n/a para este motivo (evita mostrar valor obsoleto)
                rebuf_occ_ema_fast_trigger = 0.0;      // idem
                rebuf_run_count = 0;
                rebuf_items_seen = 0;
                rebuf_start_us = esp_timer_get_time();
                rx_mode = RX_MODE_REBUFFERING;
#else
                (void)flushed;
                (void)was_first_session;  // so usado para escolher STARTUP vs SESSION (so com drift-correction)
                have_last = false;  // mesmo bootstrap simples que o arranque usa sem drift-correction
#endif
                // so agora, com o reset TODO feito, e que a sessao nova passa a "current" - e so a
                // partir daqui que a tarefa Wi-Fi volta a aceitar pacotes (ver link_rx_on_packet).
                portENTER_CRITICAL(&s_session_lock);
                s_previous_session_id = s_current_session_id;
                s_current_session_id = new_session_id;
                s_have_current_session = true;
                s_session_pending = false;
                portEXIT_CRITICAL(&s_session_lock);
            }
        }

#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
        if (rx_mode == RX_MODE_REBUFFERING) {
            // Usado tanto no arranque (motivo STARTUP, unificado aqui) como em LOW/HIGH:
            // silencio continuo (I2S/DAC nunca para) ate encontrar BUFFER_BLOCKS pacotes
            // com sample_ctr CONTIGUOS (nao "quaisquer N pacotes") - pedido explicito do
            // Psy: pacotes soltos nao dao uma base solida para (re)sincronizar, e isso e
            // especialmente importante quando o RX arranca enquanto o TX ja vai a meio.
            // expected_ctr novo e o 1o frame do bloco mais antigo dessa sequencia.
            // RESYNC-KEEP-RUN (2026-10-30): uma fila FreeRTOS so deixa ver o 1.o item, por isso verificar a
            // contiguidade obriga a retirar os itens. Ate aqui eram retirados para `peeked` e perdidos: o RX tocava
            // depois BUFFER_BLOCKS ocultacoes (repeticao do ultimo bloco antigo), a ocupacao real ficava 0-2 e nao
            // BUFFER_BLOCKS, e as ocultacoes future_waiting armavam o gate RF. Agora cada item vai para s_rebuf_stage
            // (anel de BUFFER_BLOCKS: ficam os mais recentes da sequencia) e, no resync, os BUFFER_BLOCKS mais
            // recentes voltam a frente da fila: sao tocados e a ocupacao e mesmo BUFFER_BLOCKS. O resync espera por
            // um periodo com chegadas normais (REBUF_STEADY_POPS): depois de uma falha longa o TX descarrega numa
            // rajada os pacotes que tinha em fila, e ancorar no mais antigo enchia a fila acima do HIGH_WATER.
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
            out_block = conceal_out(&conceal, false, last_block, conceal_buf, silence);  // fade-out, se vinha de som
#else
            out_block = silence;
#endif
            s_pl_silence_blocks++;
            bool rebuf_resync = false;
#if CONFIG_AUDIOLINK_LINK_RX_RESYNC_KEEP_RUN
            const link_resync_q_t ops = {rebuf_q_recv, rebuf_q_send_front, rebuf_item_ctr, s_queue};
            uint32_t rebuf_back = 0;
            uint64_t rebuf_head_ctr = 0;
            rebuf_resync = link_resync_period(&rebuf_run_start, &rebuf_run_count, &rebuf_items_seen,
                                              (uint8_t *)s_rebuf_stage, sizeof(rx_item_t), BUFFER_BLOCKS,
                                              LINK_FRAMES_PER_PKT, REBUF_STEADY_POPS, &ops, &rebuf_back,
                                              &rebuf_head_ctr);
            if (rebuf_resync) {
                rebuf_run_start = rebuf_head_ctr;  // o 1.o bloco que voltou a fila: o novo expected_ctr
            }
#else
            while (xQueueReceive(s_queue, &peeked, 0) == pdTRUE) {
                const link_pkt_hdr_t *phdr = (const link_pkt_hdr_t *)peeked.buf;
                rebuf_items_seen++;
                if (rebuf_run_count > 0 &&
                    phdr->sample_ctr == rebuf_run_start + (uint64_t)rebuf_run_count * LINK_FRAMES_PER_PKT) {
                    rebuf_run_count++;
                } else {
                    rebuf_run_start = phdr->sample_ctr;
                    rebuf_run_count = 1;
                }
                if (rebuf_run_count >= BUFFER_BLOCKS) {
                    rebuf_resync = true;
                    break;
                }
            }
#endif
            if (rebuf_resync) {
                s_pl_resync_events++;
                s_pl_resync_delta += (int64_t)rebuf_run_start - (int64_t)expected_ctr;
                expected_ctr = rebuf_run_start;
                rx_mode = RX_MODE_NORMAL;
                s_rebuffer_events++;
                switch (rebuffer_reason) {
                    case REBUFFER_REASON_STARTUP: s_rebuffer_startup++; break;
                    case REBUFFER_REASON_LOW: s_rebuffer_low++; break;
                    case REBUFFER_REASON_HIGH: s_rebuffer_high++; break;
                    case REBUFFER_REASON_SESSION: s_rebuffer_session++; break;
                }
                int64_t now_evt = esp_timer_get_time();
                int64_t dur_us = now_evt - rebuf_start_us;
                s_rebuffer_last_duration_us = (uint32_t)dur_us;
                uint32_t occ_after = (uint32_t)uxQueueMessagesWaiting(s_queue);
#if CONFIG_AUDIOLINK_LINK_RX_RESYNC_KEEP_RUN
                // retirados e nao devolvidos: os que nao faziam parte da sequencia + os que nao couberam
                uint32_t discarded_total = rebuf_discarded_at_entry + (rebuf_items_seen - rebuf_back);
#else
                uint32_t discarded_total = rebuf_discarded_at_entry +
                                            (rebuf_items_seen > BUFFER_BLOCKS ? rebuf_items_seen - BUFFER_BLOCKS : 0);
#endif
                // PATCH-OBS-01: so regista; a linha "REBUFFER fim" e impressa por rx_diag_task.
                s_rb_last = (rebuffer_evt_t){
                    .reason = (uint32_t)rebuffer_reason,
                    .dur_us = dur_us,
                    .phase_err_received_trigger = rebuf_phase_err_received_trigger,
                    .occ_ema_fast_trigger = rebuf_occ_ema_fast_trigger,
                    .occ_before = rebuf_occ_before,
                    .occ_after = occ_after,
                    .discarded = discarded_total,
                    .new_expected_ctr = expected_ctr,
                };
                // Reset do controlador (pedido explicito): a EMA antiga esta contaminada
                // pelo desvio que causou o rebuffer (ou nem existe ainda, no STARTUP), e o
                // cooldown/janela do gate tambem nao devem transportar estado de antes.
                occ_ema = (double)BUFFER_BLOCKS;
                occ_ema_fast = (double)BUFFER_BLOCKS;
                ovf_rate_ema = 0.0;
                last_ovf_seen = s_queue_ovf;
                last_corr_us = now_evt;
                rf_window_count = 0;
                gate_freeze = false;
            }
        } else
#else
        if (!have_last) {
            // Pre-buffer (comportamento antigo, sem drift-correction ativo): silencio ate
            // ter BUFFER_BLOCKS na fila; arranca do mais antigo, SEM exigir contiguidade.
            if ((uint32_t)uxQueueMessagesWaiting(s_queue) >= BUFFER_BLOCKS &&
                xQueueReceive(s_queue, &peeked, 0) == pdTRUE) {
                const link_pkt_hdr_t *hdr = (const link_pkt_hdr_t *)peeked.buf;
                memcpy(last_block, peeked.buf + sizeof(link_pkt_hdr_t), (size_t)hdr->frames * LINK_BYTES_PER_FRAME);
                s_pl_resync_events++;
                s_pl_resync_delta += (int64_t)hdr->sample_ctr - (int64_t)expected_ctr;
                expected_ctr = hdr->sample_ctr + hdr->frames;
                s_pl_normal_blocks++;
                s_pl_src_blocks_adv++;
                s_pl_src_frames_adv += hdr->frames;
                have_last = true;
                out_block = last_block;
                pkts_ok++;  // "buffer inicial cheio": impresso por rx_diag_task (resync_events > 0)
            } else {
                out_block = silence;
                s_pl_silence_blocks++;
            }
        } else
#endif
        {
            s_pl_normal_blocks++;
            // 1. descarta tudo o que ja ficou velho - nunca deixa um buraco antigo bloquear o atual.
            // Classifica cada um (atrasado-mas-em-ordem vs genuinamente reordenado, decidido no
            // recebimento) e mede o atraso em blocos (histograma).
            while (xQueuePeek(s_queue, &peeked, 0) == pdTRUE) {
                const link_pkt_hdr_t *phdr = (const link_pkt_hdr_t *)peeked.buf;
                if (phdr->sample_ctr >= expected_ctr) {
                    break;  // nao esta atrasado; decide-se a seguir
                }
                xQueueReceive(s_queue, &peeked, 0);  // remove mesmo (xQueuePeek nao tira da fila)
                if (peeked.reordered_on_arrival) {
                    s_reorder++;
                } else {
                    s_late++;
                }
                uint32_t delay_blocks = (uint32_t)((expected_ctr - phdr->sample_ctr) / LINK_FRAMES_PER_PKT);
                if (delay_blocks <= 1) {
                    s_hist_1++;
                } else if (delay_blocks == 2) {
                    s_hist_2++;
                } else if (delay_blocks <= 4) {
                    s_hist_3_4++;
                } else if (delay_blocks <= 8) {
                    s_hist_5_8++;
                } else {
                    s_hist_gt8++;
                }
            }

            // ocupacao (itens validos, ja sem os velhos): amostra deste periodo para
            // min/media/max e para distinguir underrun (fila vazia) de "so nao e a vez".
            uint32_t occ = (uint32_t)uxQueueMessagesWaiting(s_queue);
            s_occ_sum += occ;
            s_occ_n++;
            if (occ < s_occ_min) {
                s_occ_min = occ;
            }
            if (occ > s_occ_max) {
                s_occ_max = occ;
            }

            // 2. o bloco esperado AGORA ja chegou?
            out_block = NULL;
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
            bool real_block = false;
#endif
            if (xQueuePeek(s_queue, &peeked, 0) == pdTRUE) {
                const link_pkt_hdr_t *phdr = (const link_pkt_hdr_t *)peeked.buf;
                if (phdr->sample_ctr == expected_ctr) {
                    xQueueReceive(s_queue, &peeked, 0);
                    memcpy(last_block, peeked.buf + sizeof(link_pkt_hdr_t), (size_t)phdr->frames * LINK_BYTES_PER_FRAME);
                    expected_ctr += phdr->frames;
                    s_pl_src_blocks_adv++;
                    s_pl_src_frames_adv += phdr->frames;
                    out_block = last_block;
                    pkts_ok++;
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
                    real_block = true;
#endif
                }
                // sample_ctr > expected_ctr: um bloco FUTURO ja chegou, mas ainda nao e a vez
                // dele - fica na fila (nao se consome), sem bloquear nada; sera servido num
                // periodo seguinte quando expected_ctr o alcancar, ou descartado mais tarde
                // como "velho" se entretanto deixar de fazer sentido.
            }

            // 3. nada disponivel para AGORA: falha esta unica vez e avanca - nunca em rajada.
            // Duas causas distintas, contadas em separado (nao assumir que isto e "perda de
            // RF" sem mais - ver comentario junto a s_playout_concealment no topo):
            //   underrun/queue_empty: a fila estava mesmo vazia neste instante;
            //   future_waiting: a fila tinha algo, mas so um bloco FUTURO (ja chegado fora
            //     de ordem), nao o que era devido agora - sintoma de jitter/reordenacao.
            // PATCH-DRIFT-01 (2026-10-01): so future_waiting alimenta o gate_freeze. queue_empty
            // pode ser produzido pelo proprio defice de sample-slip (FAIL-DRIFT-01: occupancy a
            // descer -> queue_empty a subir -> gate_freeze quase permanente -> slip bloqueado).
            bool had_future_waiting_this_tick = false;
            if (!out_block) {
                out_block = last_block;
                s_playout_concealment++;
                bool underrun = (occ == 0);
                if (underrun) {
                    s_conceal_queue_empty++;
                } else {
                    s_conceal_future_waiting++;
                    had_future_waiting_this_tick = true;
                }
#if CONFIG_AUDIOLINK_LINK_RX_LOG_CONCEALMENTS
                // Diagnostico (investigacao do "pii" no fim do tom, 2026-09-30): o instante
                // (timestamp do proprio ESP_LOG) e o conteudo do bloco repetido, para
                // correlacionar com a transicao tom->silencio feita no lado do Mac.
                // DESLIGADO POR OMISSAO (2026-09-30): uma linha por concealment satura a serie
                // em condicoes de rede ma's (>100/s ja visto), o que atrasa de forma variavel a
                // chegada das linhas periodicas ao host e contamina qualquer medicao de tempo
                // que dependa do instante de chegada a serie (ver DRIFT abaixo, que por isso
                // nao depende disso). Só ligar para investigacao pontual de concealments.
                // PATCH-OBS-01: e a UNICA excecao que fica - um log por concealment no proprio loop
                // de playout. Ligado, reintroduz o risco de stall da consola; nunca em corridas de
                // validacao.
                ESP_LOGI(TAG, "CONCEAL expected_ctr=%llu occ=%lu underrun=%d last_R_peak_dbfs=%.1f",
                         (unsigned long long)expected_ctr, (unsigned long)occ, underrun ? 1 : 0,
                         block_peak_dbfs_r(last_block, LINK_FRAMES_PER_PKT));
#endif
                expected_ctr += LINK_FRAMES_PER_PKT;
                s_pl_src_blocks_adv++;
                s_pl_src_frames_adv += LINK_FRAMES_PER_PKT;
            }
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
            // CONCEAL-FADE-01: o que se escreve muda, a decisao e os contadores acima nao. Bloco real depois de som:
            // last_block intacto. Sem bloco: 1 fade-out espelhado e depois silencio, nunca a repeticao do bloco.
            // Antes do slip, que atua no meio do bloco e mantem as pontas.
            out_block = conceal_out(&conceal, real_block, last_block, conceal_buf, silence);
#endif
#if !CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
            (void)had_future_waiting_this_tick;  // so lido pelo gate_freeze, que so existe com drift-correction
#endif

#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
            // --- corretor de drift (sample-slip) + gate RF + gatilho de rebuffer (LOW/HIGH) ---
            // CORRIGIDO (2026-09-30): o sinal de controlo passou a ser a OCUPACAO REAL da
            // fila ("occ", ja amostrado acima no passo 1/ocupacao), nao mais phase_err
            // derivado de s_max_enqueued_ctr. Esse contador tinha side-effect: avancava
            // mesmo quando xQueueSend() falhava (queue_ovf), inflacionando o sinal para
            // milhares de frames sem relacao com o que estava mesmo disponivel para tocar.
            // s_max_received_ctr/s_max_enqueued_ctr continuam a existir, so como
            // diagnostico nos logs (phase_err_received/phase_err_enqueued) - NUNCA como
            // feedback aqui. expected_ctr nunca e tocado por este bloco (so o ramo
            // REBUFFERING, acima no loop, o faz, na resincronizacao).
            int64_t now_us = esp_timer_get_time();

            // gate_freeze: congela a correcao de drift perante rajadas de "bloco devido em
            // falta com audio mais recente ja disponivel" (future_waiting) - descontinuidade no
            // fluxo recebido (gap/atraso/reorder; causa nao determinada, nao e perda RF
            // confirmada). queue_empty NAO alimenta isto (PATCH-DRIFT-01, ver acima). Uma
            // rajada (>=N na janela) arma um freeze de duracao FIXA e NAO renovavel - um freeze
            // renovado a cada evento durante ele proprio ficava preso perante lag cronico.
            if (had_future_waiting_this_tick) {
                rf_window_us[rf_window_head] = now_us;
                rf_window_head = (rf_window_head + 1) % RF_WINDOW_CAP;
                if (rf_window_count < RF_WINDOW_CAP) {
                    rf_window_count++;
                }
            }
            uint32_t in_window = 0;
            for (uint32_t i = 0; i < rf_window_count; i++) {
                uint32_t idx = (rf_window_head + RF_WINDOW_CAP - 1 - i) % RF_WINDOW_CAP;
                if (now_us - rf_window_us[idx] <= (int64_t)CONFIG_AUDIOLINK_LINK_RF_TRIGGER_WINDOW_MS * 1000) {
                    in_window++;
                } else {
                    break;  // janela em ordem temporal: o resto e ainda mais antigo
                }
            }
            if (!gate_freeze && in_window >= (uint32_t)CONFIG_AUDIOLINK_LINK_RF_TRIGGER_COUNT) {
                gate_freeze = true;
                gate_freeze_until_us = now_us + (int64_t)CONFIG_AUDIOLINK_LINK_RF_FREEZE_MS * 1000;
            }
            if (gate_freeze && now_us >= gate_freeze_until_us) {
                gate_freeze = false;
            }

            // EMA rapida da ocupacao real (gatilho do rebuffer): robusta a oscilacao normal
            // do jitter, ao contrario de um check "sempre fora da banda durante X s" (nunca
            // completava, porque o jitter normal toca de volta para dentro da banda antes de
            // somar X s de desvio sustentado - bug encontrado em simulacao longa, com o sinal
            // antigo, mas o argumento e o mesmo para ocupacao).
            double alpha_fast = tick_period_s / ((double)CONFIG_AUDIOLINK_LINK_REBUFFER_EMA_TAU_MS / 1000.0);
            occ_ema_fast += alpha_fast * ((double)occ - occ_ema_fast);

            // taxa de queue_ovf (/s), EMA rapida com a mesma constante de tempo - 2a condicao
            // independente para o gatilho HIGH (fila cronicamente a transbordar mesmo que a
            // ocupacao amostrada no instante do tick nao pareca extrema).
            uint32_t ovf_now = s_queue_ovf;
            uint32_t ovf_delta = ovf_now - last_ovf_seen;
            last_ovf_seen = ovf_now;
            double ovf_rate_now = (double)ovf_delta / tick_period_s;
            ovf_rate_ema += alpha_fast * (ovf_rate_now - ovf_rate_ema);

            double occ_ema_fast_frames = occ_ema_fast * LINK_FRAMES_PER_PKT;
            bool low_trigger = occ_ema_fast_frames < (double)CONFIG_AUDIOLINK_LINK_REBUFFER_LOW_WATER_FRAMES;
            bool high_trigger = occ_ema_fast_frames >= (double)CONFIG_AUDIOLINK_LINK_REBUFFER_HIGH_WATER_FRAMES ||
                                 ovf_rate_ema >= (double)CONFIG_AUDIOLINK_LINK_REBUFFER_OVF_RATE_HZ;

            if (low_trigger || high_trigger) {
                // dispara rebuffer: regista o estado no instante do gatilho (para o log final,
                // phase_err_received e so diagnostico - NAO foi o que disparou isto), esvazia
                // a fila contando o descartado, e muda de modo. expected_ctr fica CONGELADO
                // ate a resincronizacao (feita no ramo REBUFFERING, acima no loop).
                rebuffer_reason = low_trigger ? REBUFFER_REASON_LOW : REBUFFER_REASON_HIGH;
                rebuf_phase_err_received_trigger = (int64_t)s_max_received_ctr - (int64_t)expected_ctr;
                rebuf_occ_ema_fast_trigger = occ_ema_fast;
                rebuf_occ_before = occ;
                rebuf_discarded_at_entry = 0;
                while (xQueueReceive(s_queue, &peeked, 0) == pdTRUE) {
                    rebuf_discarded_at_entry++;
                }
                rebuf_run_count = 0;
                rebuf_items_seen = 0;
                rebuf_start_us = now_us;
                rx_mode = RX_MODE_REBUFFERING;
            } else {
                // EMA lenta da ocupacao real (decisao de correcao) + histerese + cooldown, so
                // aplicada fora do freeze do gate RF. Unidade minima de correcao: 1 frame
                // estereo via sample-slip - nunca um bloco inteiro.
                occ_ema += (tick_period_s / ((double)CONFIG_AUDIOLINK_LINK_DRIFT_EMA_TAU_MS / 1000.0)) *
                           ((double)occ - occ_ema);
                double occ_ema_frames = occ_ema * LINK_FRAMES_PER_PKT;
                double hyst = (double)CONFIG_AUDIOLINK_LINK_DRIFT_HYST_FRAMES;
                bool cooldown_elapsed =
                    (now_us - last_corr_us) >= (int64_t)CONFIG_AUDIOLINK_LINK_DRIFT_COOLDOWN_MS * 1000;
                if (!gate_freeze && cooldown_elapsed) {
                    if (occ_ema_frames < target_occ_frames - hyst) {
                        // ocupacao abaixo do alvo: abranda o consumo (129 frames esta volta)
                        // para dar tempo ao buffer de recuperar.
                        slip_insert(out_block, slip_buf);
                        out_block = slip_buf;
                        frames_this_tick = LINK_FRAMES_PER_PKT + 1;
                        s_drift_insert_frames++;
                        last_corr_us = now_us;
                    } else if (occ_ema_frames > target_occ_frames + hyst) {
                        // ocupacao acima do alvo: acelera o consumo (127 frames esta volta)
                        // para nao deixar o buffer crescer sem controlo.
                        slip_drop(out_block, slip_buf);
                        out_block = slip_buf;
                        frames_this_tick = LINK_FRAMES_PER_PKT - 1;
                        s_drift_drop_frames++;
                        last_corr_us = now_us;
                    }
                }
            }
#endif
        }

        const size_t req_bytes = (size_t)frames_this_tick * LINK_BYTES_PER_FRAME;
        const int64_t t_wr0 = esp_timer_get_time();
        if (s_t_first_i2s_write_begin_us == 0) {
            s_t_first_i2s_write_begin_us = t_wr0;  // STARTUP-QOVF-01
        }
        if (prev_wr0_us != 0) {
            uint32_t gap = (uint32_t)(t_wr0 - prev_wr0_us);
            if (gap > s_loop_gap_max_us) {
                s_loop_gap_max_us = gap;
            }
            if (gap > s_loop_gap_max_ever_us) {
                s_loop_gap_max_ever_us = gap;
                s_loop_gap_max_ever_at_us = t_wr0;
            }
            if (gap * 2 > tick_us * 3) {
                s_loop_gap_over_1p5_tick++;
            }
        }
        prev_wr0_us = t_wr0;
        if (t_wr0 - t0 > 1000000) {
            portENTER_CRITICAL(&s_i2s_cb_lock);
            const uint64_t sent_bytes = s_i2s_bytes_sent;
            portEXIT_CRITICAL(&s_i2s_cb_lock);
            const int32_t fill = (int32_t)((int64_t)preload_frames + (int64_t)s_out_frames_written -
                                           (int64_t)(sent_bytes / LINK_BYTES_PER_FRAME));
            if (fill < s_dma_fill_min) {
                s_dma_fill_min = fill;
            }
            if (fill < s_dma_fill_min_ever) {
                s_dma_fill_min_ever = fill;
            }
            if (fill < (int32_t)LINK_FRAMES_PER_PKT) {
                s_loop_deadline_miss++;
            }
        }
        ESP_ERROR_CHECK(i2s_channel_write(tx, out_block, req_bytes, &w, portMAX_DELAY));
        const int64_t t_wr1 = esp_timer_get_time();
        if (s_t_first_i2s_write_end_us == 0) {
            s_t_first_i2s_write_end_us = t_wr1;  // STARTUP-QOVF-01
        }
        {
            uint32_t dur = (uint32_t)(t_wr1 - t_wr0);
            if (dur > s_i2s_write_max_us) {
                s_i2s_write_max_us = dur;
            }
            if (dur > s_i2s_write_max_ever_us) {
                s_i2s_write_max_ever_us = dur;
                s_i2s_write_max_ever_at_us = t_wr0;
            }
            if (dur * 2 > tick_us * 3) {
                s_i2s_write_over_1p5_tick++;
            }
            s_out_frames_req += frames_this_tick;
            s_out_frames_written += w / LINK_BYTES_PER_FRAME;
            if (w < req_bytes) {
                s_i2s_short_writes++;
            }
        }
        frame_idx += frames_this_tick;

        if (!unmuted && frame_idx > (uint64_t)clk.sample_rate_hz / 2) {
#if CONFIG_AUDIOLINK_PIN_XSMT >= 0
            gpio_set_level(CONFIG_AUDIOLINK_PIN_XSMT, 1);  // "XSMT alto": impresso por rx_diag_task
#endif
            unmuted = true;
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_stat) {
            // PATCH-OBS-01: so preenche o snapshot e publica-o. Nada aqui pode esperar - sem
            // ESP_LOG/printf, sem qsort, sem chamadas ao driver Wi-Fi, sem malloc. Mesmas leituras e
            // mesmos reinicios de janela de antes; formatar e imprimir e com rx_diag_task.
            rx_diag_t *d = &s_diag_work;
            d->now_us = now;
            d->t0_us = t0;
            d->pkts_recv = s_pkts_recv;
            d->unique_recv = s_unique_recv;
            d->dup = s_dup;
            d->enqueued = s_enqueued_count;
            d->pkts_corrupt = s_pkts_corrupt;
            d->foreign_peer = s_foreign_peer;
            d->corrupt_jump = s_corrupt_jump;
            d->corrupt_jump_run_max = s_corrupt_jump_run_max;
#if CONFIG_AUDIOLINK_LINK_RX_CONCEAL_FADE
            d->fade_out = s_fade_out;
            d->fade_in = s_fade_in;
#endif
            d->queue_ovf = s_queue_ovf;
            d->max_seen_block_plus1 = s_max_seen_block_plus1;
            d->first_block_idx = s_first_block_idx;
            d->session_unique_recv_base = s_session_unique_recv_base;
            portENTER_CRITICAL(&s_rx_max_lock);
            d->max_enqueued_ctr = s_max_enqueued_ctr;
            portEXIT_CRITICAL(&s_rx_max_lock);
            portENTER_CRITICAL(&s_session_lock);
            d->have_current_session = s_have_current_session;
            d->current_session_id = s_current_session_id;
            d->previous_session_id = s_previous_session_id;
            portEXIT_CRITICAL(&s_session_lock);
            d->stale_session_drop = s_stale_session_drop;
            d->session_pending_drop = s_session_pending_drop;

            d->iat_sum_us = s_iat_sum_us;
            d->iat_jitter_sum_us = s_iat_jitter_sum_us;
            d->iat_count = s_iat_count;
            d->iat_jitter_max_us = s_iat_jitter_max_us;
            s_iat_sum_us = 0;
            s_iat_jitter_sum_us = 0;
            s_iat_count = 0;
            s_iat_jitter_max_us = 0;

            uint32_t rn = s_rssi_n;
            s_rssi_n = 0;
            memcpy(d->rssi, s_rssi_samples, (size_t)rn);
            d->rssi_n = rn;

            d->occ_now = (uint32_t)uxQueueMessagesWaiting(s_queue);
            d->occ_min = s_occ_min;
            d->occ_max = s_occ_max;
            d->occ_sum = s_occ_sum;
            d->occ_n = s_occ_n;
            s_occ_min = UINT32_MAX;
            s_occ_max = 0;
            s_occ_sum = 0;
            s_occ_n = 0;

            uint32_t n_mac = s_mac_stats_n;
            for (uint32_t i = 0; i < n_mac; i++) {
                memcpy(d->mac[i].mac, s_mac_stats[i].mac, 6);
                d->mac[i].count = s_mac_stats[i].count;
                d->mac[i].last_sample_ctr = s_mac_stats[i].last_sample_ctr;
                d->mac[i].first_sample_ctr = s_mac_stats[i].first_sample_ctr;
            }
            d->mac_n = n_mac;

            d->late = s_late;
            d->reorder = s_reorder;
            d->hist_1 = s_hist_1;
            d->hist_2 = s_hist_2;
            d->hist_3_4 = s_hist_3_4;
            d->hist_5_8 = s_hist_5_8;
            d->hist_gt8 = s_hist_gt8;
            d->playout_concealment = s_playout_concealment;
            d->conceal_queue_empty = s_conceal_queue_empty;
            d->conceal_future_waiting = s_conceal_future_waiting;
            d->dac_underrun = s_dac_underrun;
            d->pkts_ok = pkts_ok;
            d->expected_ctr = expected_ctr;
            d->unmuted = unmuted;
#if CONFIG_AUDIOLINK_LINK_DRIFT_CORRECTION
            d->occ_ema = occ_ema;
            d->occ_ema_fast = occ_ema_fast;
            d->ovf_rate_ema = ovf_rate_ema;
            d->drift_insert = s_drift_insert_frames;
            d->drift_drop = s_drift_drop_frames;
            d->rebuffer_events = s_rebuffer_events;
            d->rebuffer_startup = s_rebuffer_startup;
            d->rebuffer_low = s_rebuffer_low;
            d->rebuffer_high = s_rebuffer_high;
            d->rebuffer_session = s_rebuffer_session;
            d->rebuffer_last_duration_us = s_rebuffer_last_duration_us;
            d->rb_last = s_rb_last;
#endif

            // PLAYOUT (INSTR-PLAYOUT-01): os s_pl_*/s_out_* so sao escritos por esta tarefa; os do
            // ISR (on_sent) e da tarefa Wi-Fi sao lidos sob o respetivo spinlock, como antes.
            d->t_us = esp_timer_get_time();
            portENTER_CRITICAL(&s_i2s_cb_lock);
            d->i2s_bytes_sent = s_i2s_bytes_sent;
            d->i2s_desc_sent = s_i2s_desc_sent;
            d->i2s_last_sent_us = s_i2s_last_sent_us;
            portEXIT_CRITICAL(&s_i2s_cb_lock);
            portENTER_CRITICAL(&s_rx_max_lock);
            d->max_received_ctr = s_max_received_ctr;
            d->max_received_at_us = s_max_received_at_us;
            portEXIT_CRITICAL(&s_rx_max_lock);
            d->preload_frames = preload_frames;
            d->normal_blocks = s_pl_normal_blocks;
            d->silence_blocks = s_pl_silence_blocks;
            d->src_blocks_adv = s_pl_src_blocks_adv;
            d->src_frames_adv = s_pl_src_frames_adv;
            d->resync_events = s_pl_resync_events;
            d->resync_delta = s_pl_resync_delta;
            d->out_req = s_out_frames_req;
            d->out_written = s_out_frames_written;
            d->short_writes = s_i2s_short_writes;
            d->gap_max_us = s_loop_gap_max_us;
            d->gap_max_ever_us = s_loop_gap_max_ever_us;
            d->gap_max_ever_at_us = s_loop_gap_max_ever_at_us;
            d->gap_gt1p5tick = s_loop_gap_over_1p5_tick;
            d->deadline_miss = s_loop_deadline_miss;
            d->dma_fill_min = s_dma_fill_min;
            d->dma_fill_min_ever = s_dma_fill_min_ever;
            d->wr_max_us = s_i2s_write_max_us;
            d->wr_max_ever_us = s_i2s_write_max_ever_us;
            d->wr_max_ever_at_us = s_i2s_write_max_ever_at_us;
            d->wr_gt1p5tick = s_i2s_write_over_1p5_tick;
            d->stats_max_us = s_stats_max_us;  // pior bloco de stats/publicacao da janela anterior
            d->stats_max_ever_us = s_stats_max_ever_us;
            d->pub_cs_max_ever_us = s_pub_cs_max_ever_us;
            d->tick_us = tick_us;

            // STARTUP-QOVF-01: so leituras (escritos 1x no arranque, ou pelas callbacks sob
            // s_i2s_cb_lock / sem lock, mesmo criterio ja usado para s_dac_underrun).
            d->t_espnow_ready_us = s_t_espnow_ready_us;
            d->t_i2s_enable_us = s_t_i2s_enable_us;
            d->t_loop_entry_us = s_t_loop_entry_us;
            d->t_first_i2s_write_begin_us = s_t_first_i2s_write_begin_us;
            d->t_first_i2s_write_end_us = s_t_first_i2s_write_end_us;
            portENTER_CRITICAL(&s_i2s_cb_lock);
            d->t_first_on_sent_us = s_t_first_on_sent_us;
            portEXIT_CRITICAL(&s_i2s_cb_lock);
            d->t_first_q_ovf_us = s_t_first_q_ovf_us;
            d->loop_entry_enqueued = s_loop_entry_enqueued;
            d->loop_entry_occ = s_loop_entry_occ;
            d->loop_entry_queue_ovf = s_loop_entry_queue_ovf;

            d->seq = ++s_diag_seq;

            // publicacao: unica secao partilhada com o logger, so uma copia de struct
            const int64_t t_cs0 = esp_timer_get_time();
            portENTER_CRITICAL(&s_diag_lock);
            s_diag_pub = *d;
            portEXIT_CRITICAL(&s_diag_lock);
            const uint32_t cs_us = (uint32_t)(esp_timer_get_time() - t_cs0);
            if (cs_us > s_pub_cs_max_ever_us) {
                s_pub_cs_max_ever_us = cs_us;
            }

            s_loop_gap_max_us = 0;
            s_dma_fill_min = INT32_MAX;
            s_i2s_write_max_us = 0;
            s_stats_max_us = 0;
            next_stat += 1000000;
        }

        // INSTR-PLAYOUT-01: duracao do bloco de estatisticas desta iteracao (corre dentro do loop de
        // playout). Desde PATCH-OBS-01 e so preenchimento + publicacao do snapshot, sem impressao.
        {
            uint32_t sd = (uint32_t)(esp_timer_get_time() - now);
            if (sd > s_stats_max_us) {
                s_stats_max_us = sd;
            }
            if (sd > s_stats_max_ever_us) {
                s_stats_max_ever_us = sd;
            }
        }
    }
}
