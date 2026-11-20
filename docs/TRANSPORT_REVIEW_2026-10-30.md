# Avaliação do transporte AudioLink — origem das interrupções audíveis (2026-10-30)

**Estado:** avaliação e recomendação. **Nada implementado nem gravado.**
**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

**Dados usados** (sem testes novos, salvo 61 s de leitura passiva da consola do TX):
- Recorder do W2 arquivado (`_archive/audiolink-w2-recorder-20261030T004553Z`):
  - janela limpa do RX W2 de 39 min (23:25–00:04);
  - janela limpa do R1 de 14 min (00:23–00:37).
- RX da Beta 1 de 14/10 (21:00–21:28, só RX, recorder de produção).
- Consola do TX com o **RX na sala**, em uso real: 61 s às ~02:1x de 30/10.
- `PHASE1_RESULTS.md`, `PAYLOAD_RADIO_STUDY.md`, `RX_PLAYOUT_PROPOSAL.md`, gravação do iPhone (W4).

## 1. Configuração real (CONFIRMADO, código e sdkconfig)

- **Rádio:** ESP-NOW v2 **unicast** (ACK e retransmissões MAC), canal 6 fixo, **HT20 MCS4 LGI (39 Mbit/s)**, 20 dBm,
  power save desligado.
- **Pacote:** 796 B (28 de cabeçalho + 128 tramas estéreo PCM24 empacotado), um a cada 2,66 ms, **375,6 pps**,
  2,39 Mbit/s de payload.
- **TX:** sem limite de envios em voo. Se a fila interna do ESP-NOW estiver cheia (`api_reject`), o bloco é descartado
  na origem.
- **RX:** a fila arranca com 4 blocos e o alvo de ocupação é 576 tramas (~4,5 blocos ≈ **12 ms de margem para
  jitter**). O anel DMA tem 8×128 tramas (~21 ms de latência fixa). Um bloco em falta dá lugar a uma repetição do
  último bloco.
- **Tempo de antena** (modelo, ESTIMATIVA): ~346 µs por pacote, ou seja **~13 % de ocupação**. Bate com o estudo de
  08/10 (13,6 %).

## 2. O que causa as interrupções

| Classe | Evidência | Etiqueta |
|---|---|---|
| **Pacotes que chegam tarde** (depois do prazo de ~12 ms de margem) | Em todas as janelas limpas, **praticamente toda a ocultação é por fila vazia, por pacotes atrasados**: W2 0,87/s, R1 0,58/s, Beta 1 0,68/s. Os segundos com `late_drop` coincidem com o TX a demorar mais a entregar (`cb_lat_max` 11–13 ms contra 5 ms) e com a fila do RX a zero | **CONFIRMADO (mecanismo dominante)** |
| Perdas reais no rádio (nunca chegam) | Limpo: W2 0,004/s, R1 0,08/s; **na sala, 0 `send_fail` em 61 s**. Beta 1 14/10: 0,23/s, com 6 s de buracos > 100 ms | CONFIRMADO: raras em condições normais; relevantes só em episódios |
| Limites de processamento ou fila no RX | `queue_ovf` 0, `dac_underrun` 0 e rebuffers 0 em todas as janelas limpas | CONFIRMADO: **não são a causa** |
| Fila do TX cheia (`api_reject`) | Só nos episódios graves, quando o rádio abranda (`pending` 32, `cb_lat` de 300 a 1200 ms). É consequência, não causa | CONFIRMADO |
| Artefactos do playout | Beeps de 375,6 Hz na ocultação (gravação W4); dente de serra do slip, −36…−56 dBFS (`EXP-OFFLINE-SLIP-QUALITY-01`); silêncio nos rebuffers | CONFIRMADO. **São o som das perdas acima**, não uma fonte independente, exceto o slip |

**Quanto se atrasam os pacotes** (atraso dos `late_drop`, em blocos de 2,66 ms: 1 / 2 / 3–4 / 5–8 / >8):
- W2: 44 / 21 / 17 / 12 / 7 %;
- R1: 39 / 23 / 20 / 13 / 6 %;
- Beta 1: 28 / 12 / 12 / 12 / 36 %.

**Maior falha de chegada por segundo, em % dos segundos:**

| Janela | > 12 ms | > 20 ms | > 30 ms | > 50 ms |
|---|---|---|---|---|
| W2 | 3,4 | 0,5 | 0,13 | 0,04 |
| R1 | 4,1 | 0,8 | 0,12 | 0 |
| Beta 1 | 1,7 | 0,65 | 0,47 | 0,36 |

**Na sala, em uso real (TX):** latência máxima de entrega por segundo com p50 **9,6 ms**, p90 **24,5 ms** e máximo
**39,7 ms**, com 0 perdas. Pior do que na bancada (5,6 / 11,3 / 63 ms). Com 12 ms de margem, **dezenas de segundos por
minuto excedem o prazo**. Isto basta para explicar interrupções regulares em uso normal: FORTEMENTE SUPORTADO (falta o
lado do RX na sala, que não tem consola).

**Origem dos atrasos:**
- FORTEMENTE SUPORTADO que estão **entre a pilha do TX e o ar**: retransmissões MAC com recuo exponencial por
  colisão ou interferência, e espera por canal livre.
- A nossa carga é ~13 %. Os atrasos de 10–40 ms equivalem a 30–110 tempos de pacote.
- Há redes vizinhas fortes em canais sobrepostos (canal 8 a −48 dBm no Mac, e às vezes no 11; Fase 1: "canal 6 a
  ~2 canais de uma rede a −63 dBm").
- Separar a pilha do TX do ar exigiria um sniffer: DESCONHECIDO.

## 3. Estamos a exigir demasiado do rádio?

- **Não em débito:** ~13 % de antena com MCS4.
- **Sim em margem de tempo:** o RX só tolera ~12 ms de atraso. Num canal partilhado de 2,4 GHz com ACK e
  retransmissões, atrasos de 10–40 ms são normais. Os sistemas sem fios dedicados usam tipicamente dezenas a centenas
  de ms de buffer.
- O buffer curto vem do requisito R1 (latência p95 ≤ 30 ms), congelado em 08/10. O sistema atual já mede ~39 ms, com
  21 ms no anel DMA.
- **A Fase 1 tinha recomendado jitter buffer ≥ 30 ms** (`PHASE1_RESULTS.md` §Recomendação).

## 4. Alternativas avaliadas

| Alternativa | Efeito sobre a causa dominante (atraso) | Custo / risco | Avaliação |
|---|---|---|---|
| **A. Mais jitter buffer no RX** (~12 → ~40 ms) | **Direto**: absorve os atrasos sem tocar no rádio. Pelos histogramas, uma margem de ~10 blocos a mais apanharia ~93 % dos `late_drop` da bancada e todos os atrasos medidos na sala (máx. 39,7 ms) | Só configuração do RX (Kconfig), sem protocolo nem TX. +~27 ms de latência (total ~65 ms). Choca com o R1 congelado | **Recomendada em 1.º lugar** |
| B. Canal com menos vizinhos (ex.: 13) e/ou PHY mais robusta (MCS2) | Reduz retransmissões, logo a cauda de atrasos | Configuração do TX e do RX. Os vizinhos mudam de canal (o do canal 8 andou pelo 11). O MCS2 sobe a ocupação para ~19 %. Mede-se depressa pela `cb_lat` do TX (5 min por braço) | 2.º, só se A não chegar |
| C. PCM16 + dither TPDF, 256 tramas por pacote | −43 % de antena e −50 % de pacotes e callbacks; menos colisões | Protocolo (TX+RX). O L ganha ~+2,4 dB de piso face ao ADC (estudo de 08/10). Grão de perda de 5,3 ms | 3.º, se B não chegar |
| D. FEC ou redundância (bloco anterior no pacote seguinte) | **Não ajuda nos atrasos:** os pacotes saem em ordem, por isso um atrasado atrasa os seguintes e a redundância chega tarde na mesma. Só cobre perdas reais, raras em condições normais | Protocolo, +25–100 % de débito | **Não agora** |
| E. Compressão sem perdas (estilo FLAC) | Menos antena (~−50 %), mas o débito não é o gargalo | Tamanho variável, CPU, complexidade no tempo real | Não agora |
| F. Broadcast sem ACK + FEC | Elimina o bloqueio por retransmissões, mas as perdas passam a reais e exigem FEC forte | Reescrita do transporte | Não agora |
| G. 5 GHz (ex.: ESP32-C5) | Foge à congestão dos 2,4 GHz | Hardware novo (compras congeladas) | Só se A–C falharem |

**Interação com o fade (`RX_CONCEAL_FADE_PROPOSAL.md`):** com A, os eventos de ocultação devem cair uma ordem de
grandeza. O fade continua útil para os que restarem, como os episódios graves e os rebuffers, mas passa a ser um
complemento.

## 5. Recomendação

**Passo único primeiro (A):** aumentar a margem de jitter do RX para ~40 ms, só por configuração.
- `AUDIOLINK_LINK_RX_BUFFER_BLOCKS` 4 → 14;
- `AUDIOLINK_LINK_DRIFT_TARGET_FRAMES` 576 → ~1984 (15,5 blocos);
- `REBUFFER_HIGH_WATER` ajustado à nova fila (`QUEUE_DEPTH` = BUFFER + 4 = 18).
- Os valores exatos validam-se offline contra a dinâmica do controlador antes de compilar.
- **Custo:** +~27 ms de latência. **Decisão do utilizador:** aceitar um orçamento de latência "de música" (~65 ms)
  acima do R1 de 30 ms. Se a latência vier a importar, reduzir o anel DMA (21 ms fixos, proposta C2 do playout)
  compensa parte.
- **Medição curta, sem campanha:**
  - bancada, RX junto ao Mac: 15 min com a configuração atual contra 15 min com a nova, mesmo dia e mesma posição;
  - métrica: `late_drop`/s (atual 0,58–0,87/s). **Critério proposto:** ≤ 0,05/s (redução ≥ 90 %);
  - depois, audição na sala.
- **Rollback:** voltar a gravar o R1 `44e6ac897`.
- **Só se a medição não chegar:** B (canal ou PHY, medido pela `cb_lat` do TX), depois C.
