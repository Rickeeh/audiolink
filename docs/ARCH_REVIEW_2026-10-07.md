# AudioLink — revisão de arquitetura de primeiros princípios (2026-10-07)

**Estado:** revisão aceite pelo utilizador como base para reabrir decisões, separando investigação, validação e
implementação.
- Nada foi implementado. Nenhum firmware de produção foi alterado.
- A secção 7 corrige afirmações da versão apresentada em conversa.
- As propostas detalhadas estão em `RX_PLAYOUT_PROPOSAL.md` e `PAYLOAD_RADIO_STUDY.md`.
- O diferencial DEEP-TX está em `experiments/EXP-DEEP-TX-DIFF-01/`.

**Etiquetas:**
- **CONFIRMADO:** medido ou lido no código/binário, reprodutível a partir dos ficheiros indicados.
- **FORTEMENTE SUPORTADO:** várias evidências convergentes, sem prova direta.
- **PROVÁVEL / HIPÓTESE:** plausível; ainda não testado.
- **DESCONHECIDO.**

**Pergunta:** estamos a resolver o problema de transporte/playout ou a contornar sintomas de uma base mal escolhida?
**Resposta curta:** em boa parte, a contornar sintomas, mas não do transporte em si.
- A cadeia de base (ESP-NOW unicast, dois relógios, jitter buffer, compensação de drift) é defensável.
- **Três escolhas provisórias nunca foram revistas:** o payload int32, o anel DMA de 8 blocos no RX e o slip com banda
  morta e prazo quantizado ao bloco. São elas que consomem a margem que depois se tentou recuperar com TARGET.
- **O DEEP-TX foi investigado só no software do rádio,** numa única placa e sempre com I2S ativo. Nunca foi comparado com a
  configuração da Fase 1, que não o mostrava.

---

## 1. Factos estabelecidos

### 1.1 Playout do RX
1. **A única proteção contra jitter é a fila. O anel DMA de saída é latência pura.** — CONFIRMADO
   - O anel tem 8 descritores × 128 tramas (`audio/main/link_rx.c:157`). Como a escrita I2S é bloqueante, ficam sempre
     ~7 blocos à frente do DAC.
   - Enchimento medido no snapshot: p50 935–943 tramas, ≈ 19,4–19,6 ms (T704, LD1, endurance;
     `experiments/EXP-RX-TARGET-704/offline_sawtooth/sawtooth.txt`).
   - A decisão "o bloco chegou?" é tomada quando o bloco entra no DMA, não quando toca. Esses ~19 ms não protegem contra
     jitter de rede.
2. **A ocupação de regime fica no limiar de INSERT, não no TARGET.** — CONFIRMADO
   - O DAC do RX é mais rápido que o ADC do TX. O slip líquido observado é +0,29 a +0,31 tramas/s, ≈ 6 ppm, e há 0 drops
     em regime.
   - O controlador de banda morta deixa a ocupação descer até TARGET − HYST (480 tramas = 3,75 blocos com o 576; 608 =
     4,75 com o 704) e só atua daí para baixo.
3. **O slip trama a trama só move o prazo em degraus de um bloco, a cada 128 slips líquidos.** — CONFIRMADO
   - A escrita de 129 tramas (1032 B) desloca a posição no descritor de 1024 B. A iteração que espera dois
     fins-de-descritor só acontece quando slips líquidos ≡ 0 mod 128.
   - Daí um **dente de serra de ~0,9 bloco com período 128/0,3 ≈ 7 min.**
   - Em 3 corridas, as subidas bruscas do `occ_ema` e as pausas de ~5,3 ms do loop caem todas nesse ponto: 16/16 (T704),
     25/25 (LD1) e 102/102 (endurance de 12 h).
4. **As perdas concentram-se no fundo do dente de serra.** `late_drop`/s no terço inferior do `occ_ema` contra o
   terço superior: 0,141 vs 0,087 (T704), 0,404 vs 0,127 (LD1), 2,095 vs 0,776 (endurance). — CONFIRMADO (associação
   dentro de cada corrida)
5. **As perdas são atraso, não perda:** `late_drop` ≈ `queue_empty` ≈ ocultações, e o `never_recv` é ~1 %. — CONFIRMADO
   (`release/CORE-V1-RC2/endurance/offline/OFFLINE_ANALYSIS.md`)
6. **Com a mesma cauda de jitter, a margem manda.** — FORTEMENTE SUPORTADO (as janelas do BQ1 são logo a seguir ao READY)
   - O BQ1 e o LD1 têm caudas iguais: máximo por segundo p50/p95/p99 de 5,6/8,9/11,8 ms contra 5,5/8,9/12,0 ms, e
     `cb_lat` 1258 contra 1259 µs.
   - As perdas diferem 8× (0,030 contra 0,249/s), com ocupação 4,85 contra 3,57
     (`experiments/EXP-LATENCY-DRIFT-01/offline_tail/tail_compare.txt`).
7. **A cauda de jitter é própria deste link.** — FORTEMENTE SUPORTADO
   - O máximo por segundo do desvio entre chegadas teve p99 de 12–16 ms. O máximo em 12 h foi 22,6 ms.
   - Uma margem de 8–11 ms produz, por construção, perdas da ordem de 0,1–1/s.
8. **A ocultação repete o mesmo bloco de 2,66 ms, sem fade** (`link_rx.c:1390`). — CONFIRMADO no código; a audibilidade
   não foi medida.
   - Num apagão, o rebuffer LOW (EMA rápida com τ = 300 ms, limiar de 0,5 bloco) só dispara ao fim de ~0,6 s.
   - Até lá ouve-se a repetição do bloco, ~375 Hz.
9. **Capacidade do corretor:** 1 trama / 2,5 s = 0,4 tramas/s ≈ 8,3 ppm. Este par de placas usa 57–78 % dessa
   capacidade. — CONFIRMADO

### 1.2 Latência
- **Medida:** 39,17 ± 0,11 ms (n = 20, 2026-09-30). O script foi reconstruído em
  `experiments/_ARCHIVE_SCRATCHPAD_2026-10-07/reconstructed/latency_measure.py`.
- **Condições da medição — CONFIRMADO pela transcrição:**
  - 10–45 s depois de regravar e arrancar os dois nós;
  - ocupação logada `occ_now` 5–6, `occ_avg` 5;
  - TX com `cb_lat_avg` 2,4–2,7 ms;
  - RSSI −21 dBm.
- **Decomposição — PROVÁVEL, ver secção 7:**

| Componente | Na medição de 2026-09-30 | Em regime (LD1/RC1) | Natureza |
|---|---|---|---|
| Conversores (PCM1808 + PCM5102A) | ~1 ms (datasheets, não verificado) | ~1 ms | inevitável com este hardware |
| Enquadramento (128 tramas) | 2,66 ms (bloco inteiro para cada amostra) | 2,66 ms | escolha |
| Pilha + ar TX→RX | ~1–2 ms (`cb_lat` 2,4–2,7 ms nessa altura) | ~1 ms | quase inevitável |
| Espera na fila ≈ (ocupação − 0,5) × 2,66 ms | ~12–14,6 ms (ocupação 5–6) | ~8,2 ms (3,57) | **a única que compra robustez** |
| Anel DMA do RX | 18,7 ms (sem slips, deslocamento 0) | ~19,4 ms | **acidental** |
| Soma | ~35–38 ms | ~31–32 ms | |

- **Consequências:**
  - O resíduo face aos 39,17 ms fica entre ~1 e ~4 ms. A ocupação nessa altura só está registada como média inteira.
  - **A latência não é constante:** desce nos primeiros minutos depois de cada arranque e oscila ~1 bloco (2,66 ms) com o
    dente de serra.
  - Só ~25 % da latência de regime protege contra jitter.

### 1.3 Transporte
- **O payload int32 leva 8 bits de enchimento por amostra (25 %).** — CONFIRMADO (`link_proto.h`)
  - Foi isto que obrigou a usar HT20 MCS4: 3,08 Mb/s, acima dos 1,92 Mb/s que a Fase 1 validou como robustos em
    11g 6M. Esses 1,92 Mb/s batem exatamente com 16 bit estéreo + 25 % de FEC.
  - A escolha está marcada como PROVISÓRIA (`audio/main/Kconfig.projbuild:478`) e nunca foi revista. O HT20 MCS4 nunca foi
    medido com obstáculos.
- **O tempo de antena hoje é 15,5 %** (modelo em `AUDIT-EXT-ESPNOW-AUDIO-01/airtime.py`).
  - O tamanho do pacote não explica o E6 nem o DEEP-TX na bancada (−32 dBm, `send_fail` ≈ 0, DEEP-TX também sem peer).
    — PROVÁVEL
  - Limita, isso sim, a robustez de alcance. — FORTEMENTE SUPORTADO

### 1.4 DEEP-TX
- **Os 8 boots válidos do TX da Fase 1 não mostram DEEP-TX.** — CONFIRMADO (dados)
  - Condições: placa `…64:04:2C`, firmware de bench sem I2S. São os mesmos de hoje: IDF 5.4.4, blob PHY 711, a
    `sdkconfig` de Wi-Fi/PHY/PSRAM/CPU/FreeRTOS/consola (a única diferença é `ESP_WIFI_NVS_ENABLED`) e o RX `…58:C7:E4`.
  - Enviaram desde ~1,2–4 s de uptime, a 200–667 pps e até 1470 B, com `api_err` = 0 (`results/run0*.tx.raw.txt`).
  - Num DEEP-TX, os 32 buffers TX encheriam em milissegundos e dariam NO_MEM.
  - Com a taxa de ~80–90 % do TX de áudio, ver 0/8 tem probabilidade < 10⁻⁵.
- **Inferência — FORTEMENTE SUPORTADO:** o DEEP-TX não é inerente ao S3 + IDF + ESP-NOW com este padrão de tráfego.
  Depende da placa, da aplicação de áudio (I2S/MCLK/PCM1808) ou do ambiente. Também PROVÁVEL: 6 desses 8 boots fizeram
  calibração parcial, como o TX de áudio, por isso a calibração não é o fator.
- **Toda a investigação causal variou só software do rádio, sempre na placa `…1D:EC` com I2S ativo.** Isto inclui PHYCAL,
  PM, FORCE-WAKE, INFLIGHT, REINIT, ESPNOW, PAUSE, PEER-RETURN e a frente temporal. Não há registo de nenhuma corrida com
  outra placa nem sem I2S. — FORTEMENTE SUPORTADO (pesquisa no repo, no scratchpad e na memória)
- **O primeiro baseline do link (2026-09-29/30) tinha 24 % de `api_err` no TX** e foi tratado como "bug de fluxo"
  (`docs/PHASE3_LINK.md`). Desde aí, o `MAX_PENDING` passou a esconder esse sintoma. HIPÓTESE forte: já era o DEEP-TX.

### 1.5 Complexidade
- **61 % do código de TX/ESP-NOW existe só para sondas, trace e supervisor:** 1772 → 692 linhas, contado com `unifdef`.
  — CONFIRMADO
- O tamanho não é o objetivo. Indica, no entanto, quanto mecanismo foi acumulado à volta de um problema por compreender.

## 2. Premissas que devem ser reabertas
1. **"O DEEP-TX é um estado interno do S3; só se contorna"** (secção 1.4).
   - Candidatos nunca testados:
     - a placa;
     - o MCLK de 12,3077 MHz no GPIO4, levado por fios até ao PCM1808. A harmónica 198 cai a 2436,92 MHz, a 77 kHz do
       centro do canal 6, e o BCLK põe ~6 riscas dentro do canal. A aritmética está CONFIRMADA; o efeito é HIPÓTESE e não
       explica sozinho a dependência do tempo desde o boot;
     - o PHY USB ativo;
     - o ambiente.
2. **"O supervisor resolve o arranque."** É reiniciar até ter sorte contra um estado de causa desconhecida.
   - Cada reinício volta a cair no mesmo estado com ~80–90 % de probabilidade (BQ-02: mediana de 85 s, pior caso 214 s).
   - Só qualifica o arranque (no BQ1, um boot passou de 1265 para ~1900 µs dentro do mesmo boot).
   - O READY aceita 2,9 ms de serviço por trama com um período de 2,66 ms.
   - **Fica como guarda temporária, sem ser removido nem estendido,** até ao diferencial.
3. **"TARGET é a ocupação alvo" e a regra `HIGH_WATER − (TARGET + HYST) ≥ 2·HYST`.**
   - O ponto de operação é o limiar de INSERT, menos o dente de serra.
   - A regra protege o lado do DROP, que este par nunca usa. Decorre da banda morta; não é uma propriedade física.
   - A restrição física real é `QUEUE_DEPTH` = 8 blocos.
4. **"4 blocos chegam"** (2026-09-30).
   - Saiu de uma corrida de 185 s, com 0,18 ocultações/s aceites como sucesso. Contrariava os ≥ 30 ms da Fase 1.
   - O E6 da endurance foi essa cauda a aparecer.
5. **"`cb_lat` do TX explica o E6"** (limiar de 1500 µs, "fixa por boot").
   - Era uma correlação entre 3 regimes. As 205 janelas não são independentes.
   - O BQ1 contradisse-a, e no LD1 não houve variação.
6. **Parâmetros de controlo validados só em simulação** (EMA_TAU, COOLDOWN, gate RF, HIGH_WATER, OVF_RATE) continuam no RC.
7. **Método:**
   - Spearman de "fração de segundos com fila vazia" contra `late_drop` é quase tautológico.
   - Janelas de 5 min da mesma sessão foram tratadas como independentes.
   - Baselines de outra hora do dia.
   - O limiar do E6 saiu de 20 min de um único boot e foi aplicado a 12 h.
   - **O TUNE-TARGET-01 já mostrava o ciclo:** episódios de inserts a 0,40/s com período de ~456 s. Concluiu "sem hunting"
     porque a métrica gross/|net| só deteta alternância entre insert e drop.

## 3. O que está overengineered
- **Fisicamente necessário:** `sample_ctr`, identidade de sessão, buffer com prazo, compensação de drift, PLC com fade,
  enchimento inicial, mute em apagão, filtro de peer e telemetria mínima.
- **Acumulado por debugging:**
  - supervisor, SELFRESET, WIFI_REINIT (5 variantes) e TRACE (depende do diferencial DEEP-TX);
  - gate RF;
  - três estimadores da mesma margem (EMA lenta, EMA rápida, EMA de overflow), mais LOW/HIGH water e OVF_RATE;
  - HYST, cooldown e regra de margem;
  - bitmap de duplicados, que se desliga aos 532 s de sessão (`link_rx.c:162`; duplicados = 0 em todas as corridas);
  - diagnósticos temporários.
- A análise mecanismo a mecanismo, com a garantia que cada um dá, está em `RX_PLAYOUT_PROPOSAL.md`, secção 6.

## 4. Arquitetura que faria hoje de raiz (mesmo hardware, 1 TX → 1 RX)
- **Payload:** PCM 24 bit empacotado como candidato principal; 16 bit com dither como alternativa a medir (decisão do
  utilizador). Ver `PAYLOAD_RADIO_STUDY.md`.
- **Rádio:** ESP-NOW unicast, com a taxa PHY escolhida por testes reais com obstáculos.
- **TX:** captura e envio numa tarefa de prioridade alta. Sem supervisor, se o diferencial mostrar uma causa corrigível.
- **RX:**
  - margem medida por prazos reais em vez de ocupação;
  - prazo de consumo com resolução fina;
  - controlo de drift sem banda morta;
  - DMA curto, dimensionado pela latência de escalonamento medida;
  - PLC com fade e mute.
  - **As opções (incluindo alternativas mais simples do que ASRC) e as experiências que as distinguem estão em
    `RX_PLAYOUT_PROPOSAL.md`.** Nenhuma está decidida.

## 5. Alternativas (estimativas)

| | A. Corrigir só a margem do RX (TARGET) | B. Manter PCM, redesenhar o playout | C. Payload compacto + taxa robusta | D. Redesign "à externo" |
|---|---|---|---|---|
| Robustez ao jitter | perdas ÷2–3 por bloco extra (T704) | ~2× a margem à mesma latência | igual | pior (buffer de 1–2 tramas) |
| Robustez ao alcance | igual (MCS4) | igual | melhor (por medir) | 1 Mb/s, mas ~91 % de tempo de antena |
| Latência | +2,7 ms por bloco | ~28–32 ms (estimativa) | igual | ~12–20 ms |
| Carga RF | 15,5 % | 15,5 % | 13,6 % (24 bit, MCS4) a 28 % (24 bit, 11g 12M) | ~91 % |
| Risco | baixo técnico, alto de produto | médio (local, validável por partes) | baixo | alto (não é audiófilo) |
| Reaproveitamento | ~100 % | ~70 % | ~100 % | < 20 % |

O "~12 ms" do externo não é comparável com os nossos 39 ms: é um alvo relativo a uma média do desvio de relógio, exclui
as partes fixas e aceita PLC frequente. Ideias a levar de lá: DMA mínimo, timestamp por pacote, callback mínimo e
crossfade.

## 6. Recomendação e decisões do utilizador (2026-10-07)
**Recomendação:** redesenhar uma parte fundamental, o playout do RX, com o payload compacto tratado como trabalho
separado. O transporte fica.

**Decisões do utilizador:**
1. Preservar a evidência. Feito no commit `e68346e`.
2. **Prioridade: diferencial DEEP-TX ABAB.**
   - Mesma placa TX, firmware de bench contra TX de áudio sem supervisor.
   - Protocolo e critérios congelados antes de executar.
   - Firmware de produção inalterado e supervisor mantido.
3. Arquitetura do RX: revisão profunda, **sem implementação**. Não se assume que DMA curto + PI + ASRC é a solução.
4. Payload: 24 bit empacotado como candidato principal, 16 bit com dither como alternativa. A taxa PHY decide-se com
   testes reais, incluindo obstáculos.
5. Não alterar ao mesmo tempo protocolo, transporte e playout.
6. Cada alteração segue o modelo: problema demonstrado → causa → alternativas → experiência mínima que as distingue →
   critério → impacto → reversão.

## 7. Correções à versão apresentada em conversa
1. **"5–8 ms de latência por explicar" — ERRADO na forma em que foi dito.**
   - Usei a ocupação de regime (~3,6). A transcrição mostra que a medição foi feita 10–45 s depois do arranque, com
     ocupação 5–6.
   - Com ela, o modelo dá ~35–38 ms e o resíduo fica em ~1–4 ms.
   - A decomposição de 2026-09-30 tinha dois erros:
     - contava o enquadramento como 1,3 ms (é um bloco inteiro, 2,66 ms, para cada amostra);
     - não contava o anel DMA de ~18,7 ms, que estava dentro do "resto de 22–25 ms".
2. **"Pausa máxima do loop de 5,3 ms em 2 h" — mal interpretada.**
   - Essas pausas são a escrita dupla na volta dos 128 slips (secção 1.1, ponto 3), não atrasos de escalonamento.
   - Sem elas, a pausa máxima por segundo em 2–12 h é 3,37–3,44 ms: ≤ ~0,8 ms acima do período nominal.
3. **"Resíduo de fase diferente por boot (0–1 no T704, 5–6 no LD1)" — ERRADO.**
   - O contador certo é o de slips líquidos (inserts − drops). Com ele, o resíduo é 0→1 nas 3 corridas, como o
     mecanismo prevê.

## 8. Registo de evidência
- **Evidência que só existia em `/tmp`:** `experiments/_ARCHIVE_SCRATCHPAD_2026-10-07/` (README, MANIFEST, SHA256SUMS).
  34 diretórios de brutos já se tinham perdido antes da preservação (`EMPTY_DIRS.txt`).
- **T704:** `experiments/EXP-RX-TARGET-704/RESULT.md`, `run1/` e `offline_sawtooth/`.
- **Bench da Fase 1:** `results/run002..run016*.tx.raw.txt`, versionados à força em 2026-10-07 apesar do `*.raw.txt` no
  `.gitignore` (hashes em `results/PHASE1_TX_RAW.sha256`), mais as `*.sdkconfig` já versionadas.
- **Auditorias anteriores:** `_ARCHIVE_SCRATCHPAD_2026-10-07/files/AUDIT-EXT-ESPNOW-AUDIO-01/AUDIT.md`,
  `AUDIT-IDF-TXPATH-01/REPORT.md` e `AUDIOLINK-RELIABILITY-ARCH-01/REVIEW.md`.
