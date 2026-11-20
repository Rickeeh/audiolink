# AudioLink v2 — arquitetura (proposta 2026-10-09; APROVADA com correções em 2026-10-09)

**Estado:** **APROVADA** pelo utilizador em 2026-10-09, com as correções da secção "Decisões aprovadas".
- Implementação de E1–E5 em curso, com commits isolados e sem push.
- Os gates físicos ficam preparados, **não executados**.
- Nós intocados: TX RC2 `b2c7e0d7b` READY, RX RC1 `53952923b`.
- **Fecha a fase DEEP-TX**, por decisão do utilizador: o BEN é robusto; o TX de áudio pode desencadear DEEP; a atividade I2S
  externa participa em algumas condições. Não se procura mais a causa física exata.
- **Bases:**
  - `CORE_V1_REQUIREMENTS.md`, `ARCH_REVIEW_2026-10-07.md`, `RX_PLAYOUT_PROPOSAL.md`, `PAYLOAD_RADIO_STUDY.md`;
  - `PHASE1_RESULTS.md`, `PHASE3_LINK.md`;
  - `experiments/EXP-DEEP-TX-{DIFF,NOPINS,MCLKOFF}-01/RESULT.md`;
  - o código atual: `audio/main/link_{tx,espnow,rx,proto}.*`, `bench/main/*` e as `sdkconfig` dos artefactos.
- **Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO / ESTIMATIVA.

---

## Decisões aprovadas (2026-10-09) — congeladas
- **Base do código:**
  - O ancestral é o BEN da Fase 1, o conhecido-bom.
  - O TX v2 é **código novo e mínimo**, não um refactor do `link_tx.c`.
- **Formato e rádio:**
  - PCM estéreo de 24 bit empacotado, 128 tramas por pacote.
  - ESP-NOW unicast, canal 6, HT20, MCS4, power save desligado.
  - `session_id` uint64 por boot; `sample_ctr` uint64.
- **Ritmo e tarefas:**
  - O ritmo final é dado pelo I2S/DMA.
  - `tx_audio` no core 1, prioridade 20 (o regime do BEN). Mudar só por razão objetiva, documentada.
  - Callback do ESP-NOW mínima: só contadores e o estado indispensável.
  - Diagnóstico fora do caminho de tempo real.
- **Envio:**
  - Sem fila de pendentes, sem requeue.
  - Um `esp_now_send()` recusado é descartado e contado.
  - O ciclo de tempo real **nunca** bloqueia (exceto à espera do DMA), reencaminha, reinicializa nem reinicia.
- **Supervisor:**
  - O supervisor do RC2 **não entra** na v2. O RC2 fica só como reversão histórica.
  - **Se o E3 falhar, nenhum supervisor é introduzido automaticamente.** A decisão volta ao utilizador com os dados.
- **Aquisição:** numa perda ou overflow, o `sample_ctr` avança pelas tramas perdidas, para preservar o tempo lógico.
- **RX nesta implementação:** só o necessário para receber o protocolo v3 (aceitar v2 e v3, desempacotar 24 → 32 bit).
  Nada de playout: TARGET, HIGH/LOW water, slip, ASRC, DMA curto, C1–C6 e ocultação ficam como estão.
- **Gate da E3:** o A legado serve **só** de controlo de presença do fenómeno. Um resultado estatístico favorável demonstra
  melhoria, mas **não transforma o TX em produto**.
- **`MAX_PENDING` = 4.** Decidido a partir da cadência real (secção TX V2, ESP-NOW):
  - no regime limpo há 0–1 pacotes em voo (0,85 ms de envio→callback para 2,66 ms de período);
  - 4 × 2,66 = 10,6 ms ≈ a margem do RX (8,2 ms no LD1 com o TARGET576; ~12 ms no playout v2);
  - **com 2 (5,3 ms)** deitavam-se fora pacotes que um bloqueio curto do canal ainda deixaria chegar a tempo;
  - **acima de 4** só se põe na fila áudio que chega depois do prazo.

### Marcos
- **TRANSPORT CORE:** TX v2 robusto, mais RX a receber v3 corretamente, sem DEEP. Fecha com a E5.
- **AUDIOLINK CORE V1:** TRANSPORT CORE, mais o playout v2 do RX, mais R1 e R2 analógico→analógico (`CORE_V1_REQUIREMENTS.md`).
  - R1 e R2 são requisitos do Core v1 final, **não gates de cada etapa do TX**.
  - O TX v2 sozinho não resolve os ~39 ms históricos (secção ORÇAMENTO).

---

## ARQUITETURA V2 RECOMENDADA

**De ponta a ponta:**

```
PCM1808 (slave) ──I2S 48 076,9 Hz──► TX ESP32-S3                         RX ESP32-S3 ──I2S──► PCM5102A
  MCLK 256·fs, BCLK, LRCK, DIN         core 1: tx_audio (prio 20)           (RC1 + descodificação v3;
                                         DMA 8×128 → 24 bit → v3 →          playout v2 na fase RX)
                                         esp_now_send (unicast)
                                       core 0: pilha Wi-Fi (23), tx_diag (1)
                         ESP-NOW v2 unicast, canal 6, HT20, MCS4 LGI, PS desligado, 796 B a 375,6 pps
```

- **Um TX → um RX**, ESP-NOW v2 unicast com ACK e reenvios MAC. Sem FEC, sem reenvio aplicacional, sem broadcast, sem cifra.
- **Relógios:**
  - o do ADC no TX é o relógio da timeline: `sample_ctr` conta tramas do ADC;
  - o RX corrige a diferença para o relógio do DAC, ~6 ppm medidos (`ARCH_REVIEW`, 1.1).
- **TX v2:**
  - uma única tarefa de tempo real, no core 1, ritmada pelo DMA do I2S;
  - empacota 128 tramas em PCM de 24 bit e envia sem bloquear;
  - tudo o resto (consola, formatação) fica no core 0, fora do caminho de tempo real;
  - **sem supervisor e sem mecanismos de recuperação.**
- **Pacote:** cabeçalho de 28 B, o layout v2 com `version` = 3, mais 128 × 6 B = **796 B a 375,6 pps (2,39 Mb/s)**.
  Tempo de antena: 13,6 % em MCS4.
- **RX:**
  - na fase TX, o RC1 só ganha a descodificação v3;
  - o playout v2 (C1–C3; C4–C5 por qualidade, `RX_PLAYOUT_PROPOSAL.md`) é uma fase separada, e é ela que cumpre R1/R2.

**O que a v2 é, e o que não é:**
- **Não é uma correção do DEEP por construção.** Ler o PCM1808 exige a mesma atividade I2S externa que participa no DEEP.
- **O que garante:**
  - um TX mínimo e determinístico, cuja única diferença face ao BEN limpo é essa atividade;
  - um gate comparativo (etapa E3) que decide, com controlo de presença do fenómeno, se a v2 dispensa o supervisor.
- **Ganhos estimados:**
  - −24 % de bytes por pacote;
  - até −1 ms de pilha e ar, se o envio→ACK ficar no regime limpo de 0,85 ms;
  - código do TX: de 1772 linhas (692 sem as opções desligadas) para ~400 (ESTIMATIVA).

---

## TX V2

### Comparação direta: TX atual contra BEN (código real)

| Aspeto | TX legado (RC2 / A) | BEN (artefacto do DIFF-01) | v2 | Leitura |
|---|---|---|---|---|
| Tarefa, core e prioridade | o ciclo corre na `app_main`: core 0, prio 1, stack 3584, no mesmo core da tarefa Wi-Fi (prio 23); `tx_diag` no core 1 com prio 0 | `bench_tx`: core 1, prio 20, stack 4096 | `tx_audio`: core 1, prio 20; `tx_diag`: core 0, prio 1 | legado: prioridade mínima no core do Wi-Fi, herdada de "correr na app_main" e nunca escolhida |
| Arranque do Wi-Fi/ESP-NOW | na `app_main` (core 0) | dentro da `bench_tx` (core 1) | dentro da `tx_audio`, como o BEN | a pilha Wi-Fi fica no core 0 nos dois (`WIFI_TASK_PINNED_TO_CORE_0`). Se a interrupção do MAC segue o core que inicia: DESCONHECIDO (a escolha está na biblioteca fechada). Mantém-se igual ao BEN para não ser uma variável |
| Ritmo | leitura bloqueante do DMA (relógio do ADC) | `esp_timer` + `vTaskDelay` + espera ativa, a 2662 µs inteiros; WDT do IDLE1 desligado | DMA do I2S a partir da E2. Na E1, temporizador ao ritmo nominal exato (2662,4 µs em média) | o ritmo do BEN não serve áudio: 2662 µs são +150 ppm, mais do que o corretor do RX (8,3 ppm), e a fila do RX enchia |
| I2S/DMA | mestre, Philips, slot de 32 bit, estéreo, `PLL_F160M`/13, MCLK 256·fs; DMA 8 × 128; callback de overflow; descarte de 500 ms | — | igual ao legado; canal criado na `tx_audio`, logo a interrupção fica no core 1 | validado: 0 `adc_ovf` em todas as corridas registadas |
| `esp_now_send` | depois de verificar pending (fila FreeRTOS) e de pôr o timestamp na fila | direto | depois de verificar os pacotes em voo por contadores e de escrever o timestamp num anel | o ponto de envio é o mesmo nos três |
| Pending e contrapressão | fila FreeRTOS de 8 timestamps (`MAX_PENDING` 8); numa recusa da API remove-se o timestamp **mais antigo**, não o acabado de pôr | nenhuma: a fila interna (32 buffers ≈ 85 ms) pode encher de áudio velho | limite de **4 em voo**, contado por contadores; recusa → descartar | **correção:** a remoção errada atribui mal a latência depois de cada `NO_MEM` (enviesada para baixo) |
| Consola | `LINKTX` 1×/s pela `tx_diag`; mas o supervisor e o SELFRESET fazem `ESP_LOGW` **dentro do ciclo de tempo real** | `ESP_LOGI` só no início/fim do run | 1 linha/s pela `tx_diag` no core 0; **nada** no ciclo de tempo real | o padrão do PATCH-OBS-TX-01 fica; as exceções saem |
| Callbacks | callback de envio: `xQueueReceive`, spinlock, anel de 1024 amostras, contadores | contadores | contadores, 1 leitura do anel, soma, máximo e histograma | — |
| Estado global | ~50 variáveis estáticas; 3 cópias de um snapshot de 4,2 KB; 4 KB de amostras de latência; estado RTC do supervisor; buffers de TRACE | 4 contadores + MAC | contadores, anel de 16 timestamps, snapshot de ~100 B, sessão | — |
| Dependências desnecessárias | app multi-papel (ADC/DAC/TX/RX), `esp_driver_gpio`, `esp_psram`, `qsort`, CRC/base64 do TRACE, código do RX no mesmo ficheiro de transporte, ~50 opções de Kconfig | SoftAP/UDP, sweep, registo do RX | `esp_wifi`, `esp_now`, `esp_driver_i2s`, `esp_timer`, `nvs_flash` | — |

**Essencial no TX legado:**
- leitura do I2S ritmada pelo DMA;
- cabeçalho com `session_id` e `sample_ctr`;
- envio unicast com PHY fixo;
- contagem de overflow;
- limite de pacotes em voo;
- telemetria fora do caminho de tempo real.

**Só histórico/depuração:** o resto. A contagem `unifdef` da revisão de 07/10 dá 61 % das linhas do TX/ESP-NOW dedicadas
a sondas, trace e supervisor.

**O BEN não está perfeito:**
- o ritmo de espera ativa queima CPU e obriga a desligar o WDT do IDLE1;
- os 2662 µs inteiros não são o relógio do áudio;
- não tem limite de pacotes em voo;
- não tem sessão.

**O que a v2 tira do BEN:** só o que é demonstradamente limpo — a colocação (core 1, prio 20), o arranque do Wi-Fi/ESP-NOW e
o envio direto.

### Tarefas

| Tarefa | Core | Prio | Faz | Pode bloquear |
|---|---|---|---|---|
| `tx_audio` | 1 | 20 | Arranque: Wi-Fi, ESP-NOW, sessão, I2S (como o BEN). Depois o ciclo: ler 1 descritor → empacotar → cabeçalho → `esp_now_send` | **só** em `i2s_channel_read`, ou seja, à espera do relógio do ADC |
| ISR do I2S | 1 | — | fim de descritor acorda a `tx_audio`; overflow incrementa um contador | nunca |
| Wi-Fi (IDF) | 0 | 23 | pilha 802.11/ESP-NOW; corre a callback de envio | a callback nunca bloqueia: contadores e uma leitura do anel |
| `tx_diag` | 0 | 1 | 1×/s copia o snapshot (spinlock, ~100 B) e imprime uma linha | sim (consola USB, até 50 ms), sem efeito no tempo real |
| `app_main` | 0 | 1 | cria as duas tarefas e termina | — |

**Porquê esta estrutura:**
1. **Prioridade alta no core 1:**
   - o único ponto de espera da `tx_audio` é o DMA;
   - o trabalho por período é de ~0,1 ms mais o `esp_now_send` (≤ 0,83 ms medido no TRACE-TX-BOOT-01);
   - com prio 20 no core 1, a resposta ao fim de descritor é de µs, seja qual for a carga do Wi-Fi no core 0;
   - bloqueia >70 % do tempo, por isso não priva ninguém de CPU.
2. **Sem anel nem fila intermédia:**
   - o anel DMA já desacopla o relógio do ADC do leitor (folga de 7 × 2,66 = 18,6 ms);
   - o `esp_now_send` não bloqueia: copia a trama e devolve;
   - uma segunda tarefa só acrescentaria uma cópia, uma passagem de testemunho e modos de falha, sem resolver nenhum
     problema demonstrado.
3. **Jitter entre aquisição e rádio:** envio imediato a cada fim de descritor, com dezenas de µs de variação do lado do TX.
   O jitter que importa vem da pilha e do ar, e quem o absorve é o RX.
4. **Ressalva honesta:** o NOPINS-01 correu a estrutura legada (core 0, prio 1, ISR do I2S no core 0) e ficou limpo, 0/12.
   **A estrutura não foi a causa demonstrada do DEEP.** Escolhe-se esta por determinismo e por ser a do BEN, não como
   correção.

### Aquisição I2S
- **Configuração:** S3 mestre, PCM1808 slave (MD1/MD0 baixos), Philips, slot de 32 bit (o PCM1808 em slave exige 64 BCK por
  trama), estéreo, `PLL_F160M`/13 → fs = 48 076,9 Hz, MCLK = 12,3077 MHz; GPIO 4/5/6/7. Tudo validado; nada de tuning.
- **DMA de 8 descritores × 128 tramas** (8 KB de RAM interna).
  - **O tamanho natural do bloco é 128 tramas = 1 pacote = 2,66 ms.**
  - A profundidade do anel **não acrescenta latência em regime**: a leitura devolve cada descritor no momento em que
    termina. Só fixa a tolerância a atrasos do leitor, 18,6 ms, cerca de 20× o pior caso do ciclo.
  - Fica igual ao legado, que deu 0 `adc_ovf`.
- **Arranque:** descartar 500 ms, como o legado (o PCM1808 só dá dados válidos ~187 ms depois do relógio, mais o fade-in).
  O `sample_ctr` começa em 0 no primeiro bloco enviado.
- **Overflow** (o descritor mais antigo foi reescrito):
  - contar e **avançar o `sample_ctr` 128 tramas por cada descritor perdido**: o `sample_ctr` é o relógio do ADC;
  - o RX vê então um buraco (ocultação) em vez de uma timeline comprimida;
  - nunca tentar recuperar.
  - **Correção:** o legado não avança (`link_tx.c`: "questão separada").
- **Erro ou timeout de leitura:** contar e continuar; é falha de hardware ou de configuração. Sem reinício nem recuperação.

### Empacotamento
- **Por bloco:** os 24 bits altos de cada palavra I2S, em 3 bytes little-endian por amostra (L, R): 768 B.
  - O byte baixo não é dado do ADC (o `adc_test.c` já faz `>> 8`).
  - Custo estimado: ~0,3 % de CPU (ESTIMATIVA).
- **Cabeçalho:** o v2 sem alterações de layout (`magic` ALK1, `version`, `flags`, `frames` = 128, `seq`, `session_id`,
  `sample_ctr`, 28 B, com os `_Static_assert`). Só muda `version` = 3 ("payload PCM24 empacotado").

### ESP-NOW
- **Unicast para o MAC do RX**, fixo no Kconfig dos dois lados, com abort no arranque se não estiver configurado (como
  hoje).
  - O ACK e os reenvios MAC são a proteção barata contra perdas isoladas.
  - O broadcast perde ambos, e com um só RX não traz nada.
- **Rádio:** HT20, canal 6, **MCS4 LGI** (inicial, igual à produção), `WIFI_PS_NONE`, potência por omissão (20 dBm).
  - Taxa e canal ficam constantes num só header, sem menu de Kconfig.
  - O 24 bit torna viáveis 11g 24M (17,0 %) e 11g 12M (28,0 %). A escolha por alcance (P2) é uma decisão separada.
- **Em voo:** contado como `aceites − (send_ok + send_fail)`. É exato porque há uma callback por envio aceite.
  - **Limite: 4 (≈ 10,6 ms de áudio).**
  - Um pacote atrás de 4 em voo chega com ≥ 10,6 ms de atraso mais o tempo de antena, quase sempre depois do prazo do RX
    (margem de ~12 ms). Gastar antena com ele atrasa os pacotes frescos.
  - Valor inicial. Em regime limpo o número em voo é 0–1 (envio→callback de 0,85 ms).
- **Recusa do `esp_now_send`** (`NO_MEM` ou outro erro), ou limite atingido:
  - descartar o bloco e contar por motivo;
  - nunca reenviar, nunca bloquear, nunca reinicializar.
- **`send_fail`** (sem ACK depois dos reenvios MAC): só contar. Reenviar áudio velho não serve.
- **Latência envio→callback:**
  - timestamp escrito antes do envio num anel de 16 posições, indexado pelo contador de aceites;
  - a callback lê a posição do contador de concluídos;
  - **sem fila FreeRTOS, sem lock e sem rollback**: uma recusa não incrementa o índice e a posição é reutilizada;
  - pressupõe que as callbacks chegam pela ordem dos envios. É PROVÁVEL para um único peer e é o mesmo pressuposto da fila
    legada; o comentário do TRACE avisa que não está provado.
  - Os contadores de em voo não dependem desse pressuposto; só a atribuição da latência depende.
- **Sai a pending queue legada** (e o seu rollback errado).

### Protocolo e sessão
- **Mantém-se sem alterações:**
  - MAC do peer;
  - `session_id` uint64 aleatório por boot, gerado depois do arranque do Wi-Fi e nunca persistido;
  - `sample_ctr` uint64;
  - o lado do RX: reset de sessão (pending → adoção pela tarefa RX), proteção `previous_session_id` e filtro de peer.
- **Simplifica-se:**
  - no TX, só a semântica do `sample_ctr` no overflow (acima);
  - no RX, na fase RX (C3/C6), o bitmap de duplicados e o `MAX_JUMP` passam a anel indexado por `sample_ctr`
    (`RX_PLAYOUT_PROPOSAL.md`, secção 6). Nada disto muda agora.

### Supervisor — decisão: **REMOVER na v2**
- **Porquê:**
  - foi criado para sobreviver a um estado de causa desconhecida do TX legado: reinicia até ter sorte, com 80–90 % de
    reentrada por tentativa;
  - só qualifica o arranque;
  - numa v2 em construção **esconderia precisamente a regressão que cada etapa tem de ver**.
- **Em produção:** o RC2 (com supervisor) fica até a v2 passar a E5.
- **Critério objetivo para o retirar de produção:**
  1. **E3 PASS:** gate comparativo com presença (secção SEQUÊNCIA);
  2. **E5 PASS:** matriz funcional sem nenhum boot degradado.
- **Se o E3 falhar** (a v2 degrada como o legado): **nenhum supervisor é introduzido automaticamente** (decisão do
  utilizador, 2026-10-09). A implementação pára nesse ponto e a decisão volta ao utilizador com os dados.

### Observabilidade mínima (sem consola no caminho de tempo real)
- **TX (aprovado):** uma linha `LINKTX` 1×/s, fora do caminho de tempo real. Mantém os nomes de campo que os avaliadores
  congelados já leem:
  - `t`, `adc_blocks` (blocos I2S lidos), `sample_ctr` (tramas, incluindo as perdidas), `adc_ovf`, `i2s_read_err`;
  - `tx_attempt`, `api_accept`, `api_reject`, `last_err`, `backpressure_drop` (limite atingido);
  - `send_ok`, `send_fail`, `pending` (em voo) e `pending_max` (máximo da janela);
  - `cb_lat_avg_us` e `cb_lat_max_us` (agregados baratos, sem histórico);
  - `heap_min`, `session_id`, `chan`, `uptime_s`.
  - Sem histograma, sem arrays de amostras, sem `qsort`.
- **No arranque, uma vez:** ELF/versão, MACs, PHY/taxa/canal/potência, `session_id`, relógio e DMA do I2S,
  tarefa/core/prio. Fica o log de debug do I2S do IDF no arranque, porque o MC dos avaliadores lê essas linhas.
- **RF:** `chan`, `send_fail`, distribuição envio→callback (TX) e RSSI (RX).
- **RX:** os contadores atuais que R2 usa (`late_drop`, `playout_concealment`, `never_recv`, `rebuffer_*`,
  `dac_underrun`, `queue_ovf`, `iat_jitter_max_us`, RSSI, sessão, uptime). A telemetria de margem por prazos (C1) é da fase
  RX.

---

## FORMATO/PACOTE ESCOLHIDO

**Escolha: PCM de 24 bit empacotado, 128 tramas por pacote: 28 + 768 = 796 B, 375,6 pps, 2,39 Mb/s; tempo de antena
13,6 % (HT20 MCS4), 17,0 % (11g 24M), 28,0 % (11g 12M).**

### Formatos a 128 tramas
Tempo de antena com o modelo do repo (`AUDIT-EXT-ESPNOW-AUDIO-01/airtime.py`, slot de 9 µs, sem reenvios). O cálculo
reproduz os 15,5/13,6/11,5 % do estudo.

| | A. int32 (atual) | **B. 24 bit empacotado** | C. 16 bit + dither TPDF | D. IMA ADPCM (só referência) |
|---|---|---|---|---|
| Payload | 1052 B | **796 B** | 540 B | 164 B (4 bit + estado do preditor) |
| pps | 375,6 | 375,6 | 375,6 | 375,6 |
| Mb/s | 3,16 | 2,39 | 1,62 | 0,49 |
| Antena MCS4 / 24M / 12M | 15,5 / 20,3 / 34,6 % | **13,6 / 17,0 / 28,0 %** | 11,5 / 13,7 / 21,4 % | 8,5 / 8,9 / 11,6 % |
| CPU (ESTIMATIVA) | 0 (cópia) | ~0,3 % | ~0,6 % (gerador do dither) | ~2 % em cada lado |
| Qualidade | 24 bit sem perdas | **24 bit sem perdas** (bit-exato face ao ADC) | +3,5 dB no piso de banda larga do L, ver verificação | com perdas: fora de "estéreo de elevada qualidade" |
| Latência | 2,66 ms | 2,66 ms | 2,66 ms | 2,66 ms (sem lookahead) |
| Complexidade | — | 2 funções + versão | + dither + validação de qualidade | codec nos dois lados, estado por pacote |

**Verificação do "+2,4 dB do 16 bit" contra os dados reais — a afirmação do estudo está ERRADA na convenção:**
- O `tools/audio_analyze.py`, que mediu o piso do ADC, usa **AES17**: referência = potência de um seno de escala completa,
  linhas 123–128.
- O estudo comparou esse piso com o ruído TPDF expresso em relação ao pico (σ = 2⁻¹⁶ → −96,3 dB).
- **Em AES17**, o ruído TPDF de 16 bit é −93,3 dBFS na banda toda e **−94,1 dBFS em 20 Hz–20 kHz**.
- **Efeito no piso:**

  | Canal | Piso | Efeito do 16 bit |
  |---|---|---|
  | L, banda larga | ≈ −95 dBFS (estimativa sem o hum, HIPÓTESE do `PHASE2_WIRING.md`) | **+3,5 dB** |
  | L, medido com hum | −86,4 dBFS | +0,7 dB |
  | R, o pior canal | −78,7 dBFS | **+0,15 dB** |

- A penalização é **maior no melhor canal**, não "no pior".
- **A conclusão mantém-se e fica mais forte:**
  - o 16 bit não é transparente para o melhor caso deste ADC, nem para um ADC melhor (PCM1863);
  - poupa só 2,1 pp de antena em MCS4.
- **"24 bit empacotado sem perda audível" é CONFIRMADO, e é mais do que isso:** é sem perda nenhuma, bit-exato face aos 24
  bits do PCM1808.

### Tamanho do pacote (24 bit)

| Tramas | B/pacote | pps | ms/pacote | IEs ESP-NOW | MCS4 | 24M | 12M | callbacks/s |
|---|---|---|---|---|---|---|---|---|
| 64 | 412 | 751,2 | 1,33 | 2 | 20,8 % | 24,2 % | 36,2 % | 751 |
| 96 | 604 | 500,8 | 2,00 | 3 | 16,1 % | 19,5 % | 30,7 % | 501 |
| **128** | **796** | **375,6** | **2,66** | **4** | **13,6 %** | **17,0 %** | **28,0 %** | **376** |
| 160 | 988 | 300,5 | 3,33 | 4 | 12,1 % | 15,5 % | 26,2 % | 300 |
| 192 | 1180 | 250,4 | 3,99 | 5 | 11,1 % | 14,7 % | 25,2 % | 250 |
| 240 | 1468 | 200,3 | 4,99 | 6 | 10,0 % | 13,7 % | 24,1 % | 200 |
| 256 | 1564 | — | — | — | **impossível:** > 1470 B (máximo do ESP-NOW v2) | | | |

**128 tramas, porque:**
- **64:** +53 % de antena e o dobro das callbacks.
- **96:** compra −0,67 ms com +2,5 pp de antena e +33 % de callbacks, e passa os 30 % em 11g 12M. Com o playout do RX
  corrigido, o orçamento de latência não precisa desses 0,67 ms.
- **192:**
  - compra −2,5 pp de antena com +1,33 ms de latência e unidade de perda de 4 ms;
  - **não abre nenhuma taxa PHY nova** dentro de 30 % de antena (MCS0 e 6M continuam acima de 40 %);
  - muda a grelha de blocos do RX (TARGET, limiares em tramas, slip).
- **240:** fica a 2 B do limite, com perdas em grão de 5 ms.
- **128:**
  - mantém a grelha do RX;
  - cabe nas taxas MCS4, 24M e 12M;
  - a latência de enquadramento cabe no orçamento.

O grão do prazo no RX deixa de depender do tamanho do pacote quando o DMA do RX passar a 32 tramas (C3).

---

## O QUE REUTILIZAMOS

**Do BEN (`bench/main`):**
- **`transport_espnow.c`:**
  - sequência de arranque do Wi-Fi: STA, armazenamento em RAM, HT20, canal fixo, `PS_NONE`, MAC no log;
  - `esp_now_init` + peer + `esp_now_set_peer_rate_config`, com **abort se a taxa não for aplicada**.
- **`bench_main.c`:** tarefa no core 1, prio 20, com o arranque do Wi-Fi/ESP-NOW dentro dela.
- **`sdkconfig` do artefacto BEN do DIFF-01** como base do sistema: CPU a 160 MHz, FreeRTOS a 1000 Hz, consola
  USB-Serial-JTAG, PSRAM ligada (não usada), Wi-Fi por omissão, NVS do Wi-Fi ligado.

**Do TX legado (`audio/main`):**
- `link_proto.h`: layout, `_Static_assert`, ALK1, `session_id`, `sample_ctr`, com `version` → 3.
- **`link_tx.c`:**
  - geração da sessão;
  - configuração do I2S (relógio, slots, pinos, DMA 8 × 128, callback de overflow);
  - descarte de 500 ms;
  - o padrão snapshot → tarefa de diagnóstico noutro core (PATCH-OBS-TX-01), simplificado.
- `audio_common.c`: só o relógio `INT160_13`.

**RX:** RC1 sem alterações até à E4; depois, mais a descodificação v3.

**Ferramentas e processo:**
- `tools/deep_tx/resetlib.py` e os orquestradores derivados do `dtx_block.py`;
- os avaliadores congelados (bandas do `severity.py`, MC1–MC3, regras do DIFF-01);
- os scripts de reposição;
- o modelo de tempo de antena;
- `latency_measure.py` (reconstruído);
- `record_audio.py` / `audio_analyze.py`;
- disciplina de release: artefactos, hashes, reposição.

---

## O QUE APAGAMOS

Nada disto entra na v2. O código antigo fica no histórico e no tree `audio/` até o RC2/RC1 saírem de produção.

| Mecanismo | Onde está hoje | Destino |
|---|---|---|
| Supervisor de arranque (BQ-02, WAIT_PEER/PEER_SEEN, estado RTC, até 20 reinícios) | `link_tx.c` | sai; o RC2 fica como fallback de produção até à E5 |
| SELFRESET_PROBE | `link_tx.c` | sai |
| WIFI_REINIT e as 5 variantes (REINIT, QUAL, PROBE, PAUSE, ESPNOW, ESPNOW_RECOVERY) | `link_espnow.c` (~260 linhas), `link_tx.c` (~350), `link_espnow.h` | sai |
| TRACE_TX_BOOT (buffers, CRC/base64, âncoras) | `link_espnow.c`, `link_tx.c` | sai |
| Fila FreeRTOS de timestamps + rollback + `MAX_PENDING` no Kconfig | `link_espnow.c` | → contadores + anel de 16 + constante 4 |
| 1024 amostras de latência ×2 + `qsort` + p95 por segundo; 3 cópias de um snapshot de 4,2 KB | `link_tx.c`, `link_espnow.c` | → média, máximo e histograma de 5 classes |
| Campos de autodiagnóstico do log (`sent`, `pub_seq`, `pub_cs_max_ever_us`, `log_missed`, `log_lag_us`, `log_prev_print_us`, `log_print_max_ever_us`, `log_stack_free`, `heap_free` 1×/s) | `link_tx.c` | saem (`heap_min` passa a ser registado no arranque e no diagnóstico) |
| `ESP_LOG*` dentro do ciclo de tempo real | supervisor e SELFRESET | sai |
| App multi-papel + menu de Kconfig (papéis ADC/DAC, relógios alternativos, PHY/taxa, sondas) | `main.c`, `Kconfig.projbuild` | o TX v2 é uma app própria; as ferramentas de bring-up ficam no tree antigo |
| Payload int32 | `link_proto.h`, `link_tx.c` | E4 |
| Do BEN: espera ativa + WDT do IDLE1 desligado | `bench_main.c`, `sdkconfig` | só na E1; sai na E2 |
| Do BEN: sweep, dual, SoftAP/UDP, cabeçalho de bench, registo do RX | `bench/main` | nunca portado |

**Tamanho:** o caminho TX/ESP-NOW tem hoje 1772 linhas (692 sem as opções desligadas). A v2 terá ~400 linhas (ESTIMATIVA).

---

## IMPACTO NO RX

**O que muda já (só na E4):**
- aceitar `version` 3 (e v2 durante a transição);
- validar o comprimento por versão: 28 + `frames` × 6 em v3 (com `frames` ≤ 128), × 8 em v2;
- **normalizar à receção:** um pacote v3 é guardado na fila do RX já no layout v2 (cabeçalho + int32 alinhado à esquerda,
  byte baixo 0), pela mesma função de desempacotamento que os testes de host verificam;
  - o playout continua a ler exatamente o que lia;
  - o transporte fica isolado do playout.

Nada mais muda: fila, DMA, slip, sessão e telemetria ficam iguais. Com v2 e v3 aceites, o TX pode ser revertido sozinho.

**Separação dos factos conhecidos do RX:**

| Facto | Muda com o empacotamento? | Dono |
|---|---|---|
| Unidade de ocultação (repete 1 bloco de 2,66 ms) | sim, é o tamanho do pacote; com 128 não muda | RX C4 (PLC com fade) |
| Cópias por pacote na callback do RX (2 × `memcpy` de ~1 KB) e tempo de antena | sim: −25 % de bytes e −12 % de antena | melhora sozinho com a E4 |
| Cauda de chegada (máximo por segundo p99 de 12–16 ms; máximo de 22,6 ms em 12 h) | DESCONHECIDO: a parte que vem do TX (pilha, estado degradado) pode mudar com a v2 | medida na E3/E5 com o `iat_jitter_max_us` do RC1 |
| Anel DMA de 8 × 128 = 18,7 ms de latência morta | não | RX C2 |
| Dente de serra alinhado com os 128 slips (prazo quantizado ao descritor) | não: é o grão do DMA do RX | RX C3 (DMA de 32 + prazo fino) |
| Controlador com inserts contínuos e ponto de operação no limiar de INSERT | não: são dois relógios independentes (~6 ppm) e a banda morta | RX C3 |
| TARGET576 com pouca folga; T704 com −61 % de `late_drop` mas a violar a geometria documentada | não: é dimensionamento da margem. A regra de geometria protege o lado do DROP, que este par nunca usa (`ARCH_REVIEW`, 2.3) | RX C1 + C3: alvo por prazos medidos, não por ocupação |
| Artefactos do sample-slip (−36 a −56 dBFS, modelo) | não | RX C5 (atraso fracionário) |
| ASRC nunca validado no DAC real | não | RX C5, com medição no DAC real |

**Para a fase RX v2:**
- C1 (telemetria por prazos) → C2 (DMA) → C3 (prazo fino + controlador sem banda morta) → C4 → C5 → C6, como em
  `RX_PLAYOUT_PROPOSAL.md`;
- uma alteração de cada vez, depois da E5.

**Correção ao `PAYLOAD_RADIO_STUDY.md`:** a ordem "P1 só depois de C1–C3" é desnecessária. O 24 bit não mexe no tempo, e
um teste nulo bit-exato isola-o. Pode entrar antes (E4).

---

## ORÇAMENTO DE LATÊNCIA

R1 congelado: analógico→analógico p95 ≤ 30 ms, máximo ≤ 35 ms; objetivo de desenho 20–25 ms.

| Componente | Hoje (39,17 ms medidos, 10–45 s depois do arranque) | TX v2 + RX RC1 | **TX v2 + RX v2 (C2 + C3)** | Fonte |
|---|---|---|---|---|
| Conversores (PCM1808 + PCM5102A) | ~1 | ~1 | ~1 | datasheets, não verificado |
| Enquadramento (128 tramas) | 2,66 | 2,66 | 2,66 | aritmética |
| TX: DMA → envio (empacotar + cabeçalho) | ~0 | ~0,1 | ~0,1 | ESTIMATIVA |
| Pilha + ar | 1–2 (envio→callback 2,4–2,7 ms nessa altura) | ~1 (0,85 ms no regime limpo) | ~1 | logs do NOPINS/MCLKOFF |
| Margem contra jitter (espera na fila) | 12–14,6 (ocupação 5–6) | 8–11 (ocupação de regime 3,6–4,6) | **~12, fixa pelo alvo de prazo** | BQ1: 11,6 ms → 0,030/s; T704: 10,9 → 0,097/s; LD1: 8,2 → 0,249/s |
| Anel DMA do RX | 18,7 | 18,7–19,6 | **~4,7** (8 × 32; até 8,0 com 4 × 128) | `RX_PLAYOUT_PROPOSAL.md`, secção 2 |
| Interpolador (C5, opcional) | 0 | 0 | +0,3–0,7 | idem, secção 7 |
| **Total** | **35–38 + 1–4 por explicar** | **~31–34** | **~21,5 (20,5–25)** | |

**Leitura:**
- **Ganhos face aos ~39 ms:**
  - −14 ms do anel DMA do RX (C2/C3);
  - −0 a −2,6 ms por fixar a margem no valor dimensionado em vez do excesso de arranque (C3);
  - até −1 ms de pilha e ar, se o TX v2 ficar no regime limpo de 0,85 ms;
  - **o TX v2 não compra latência de enquadramento**: mantém as 128 tramas.
- **Só com o TX v2, o R1 continua a falhar:** ~31–34 ms em regime. O R1 depende da fase RX.
- **Com o RX v2,** a latência fica constante (±0,33 ms do controlador): p95 ≈ máximo ≈ 21,5–22,5 ms.
- **A margem é a variável:** cada +2,66 ms dividiu as perdas por 2–3 (T704). Até ~14 ms de margem, fica-se dentro dos
  20–25 ms.

---

## RISCOS

1. **A E3 reproduzir o DEEP** — PROVÁVEL que seja possível.
   - A v2 precisa da atividade I2S externa, e o segundo fator físico não é controlado.
   - **Resposta:** parar e devolver a decisão ao utilizador com os dados. **Nenhum supervisor nem mecanismo novo é
     introduzido automaticamente.**
2. **Gates inconclusivos por o fenómeno estar ausente** (precedente: MCLKOFF-01, A com 2/15).
   - **Resposta:** cada gate decisivo leva o A legado intercalado como controlo de presença. Sem presença (A < 50 %) o
     resultado é INCONCLUSIVO, nunca PASS, e repete-se noutra sessão.
   - A montagem dos dias de DEEP alto (ficha dos phones solta na KA6) serve de condição histórica de alta incidência.
     Não foi testada como causa.
3. **R1 e R2 dependem do RX, não do TX.**
   - A v2 do TX não chega ao Core v1 sozinha; atribuir-lhe melhorias de R2 seria um erro de método.
   - **Resposta:** os gates do TX são relativos à linha de base com o mesmo RX RC1. R1/R2 só se avaliam depois da fase RX.
4. **Transição v2 → v3 do protocolo:** compatibilidade e exatidão.
   - **Resposta:** teste de ida e volta no host com vetores (±2²³, 0, aleatório); teste nulo na cadeia; o RX aceita v2 e v3;
     o RC1 continua arquivado.
5. **Alcance em HT20 MCS4 nunca medido com obstáculos** (risco de produto, fora dos requisitos congelados).
   - O 24 bit permite 11g 24M/12M dentro de 30 % de antena.
   - **Resposta:** decisão de taxa (P2) com o bench e obstáculos, separada e depois da E5. Não entra no gate do Core v1 sem
     decisão do utilizador.

**Menor, coberto pelo desenho:**
- operações de flash ou cache durante a execução podiam atrasar a ISR do I2S;
- não há escritas em flash em regime (Wi-Fi com armazenamento em RAM);
- o DMA tolera 18,6 ms;
- o `adc_ovf` = 0 é condição de todos os gates.

---

## SEQUÊNCIA DE IMPLEMENTAÇÃO

**Regras comuns:**
- uma alteração por etapa; cada etapa é um binário próprio da app nova `tx_v2/`, com artefactos e hashes;
- o TX de produção fica no RC2 até à E5;
- **reversão de qualquer etapa:** regravar o RC2 com a reposição existente; o RX fica no RC1 (o RC1 + v3 só a partir da E4).

**Gate de arranque G(N)** (já existe):
- N boots formais por `reset_single`, com o orquestrador e o classificador congelados;
- `entrega15` no RX com as bandas do `severity.py`; degradado = MEDIUM/STRONG;
- MC1–MC3 por boot:
  - cadência 375,6 ± 0,5 %, blocos completos;
  - `adc_ovf` = `i2s_read_err` = 0;
  - canal 6, sem linhas E/W;
- regra `healthy()` em ≥ 20 s seguidos depois de cada boot: `cb_lat_avg` ≤ 3000 µs, `pending` ≤ 5, Δ`api_accept` ≥ 370/s,
  Δ`backpressure_drop` ≤ 1/s.

### E1 — BEN → stream ALK1 (o ancestral)
- **Alteração mínima:** o BEN do DIFF-01, com a mesma app, tarefa, arranque, `sdkconfig` e padrão de ar (1052 B, 375,6 pps,
  HT20 MCS4), mas:
  1. emite pacotes ALK1 v2 compatíveis com o RC1 (cabeçalho com sessão, `seq`, `sample_ctr`; 128 tramas int32 de um tom
     digital de 997 Hz a −20 dBFS);
  2. ritmo nominal exato de 2662,4 µs em média, em vez dos 2662 inteiros, que dariam +150 ppm e encheriam a fila do RX;
  3. caminho de envio final: em voo por contadores, limite 4, recusa → descartar, anel de latência;
  4. telemetria mínima (`LINKTX` + `tx_diag` no core 0).
- **PASS:**
  - G(15) com 0/15 degradados;
  - o RX RC1 adota a sessão e conta os pacotes como ALK1 (`foreign_peer` = `corrupt` = 0);
  - o tom gravado pela KA6 é contínuo;
  - corrida de 30 min com `backpressure_drop` = 0 e `api_err` = 0 em regime.
- **FAIL:** qualquer boot degradado, ou qualquer critério acima falhado.
- **Reversão:** regravar o RC2.
- **Substitui:** o caminho TX do `link_espnow.c` (arranque, envio, contrapressão, estatísticas) e a maquinaria do `tx_diag`.

### E2 — Aquisição I2S/DMA mínima (interna, pinos não usados)
- **Alteração mínima:**
  - sai o temporizador e a espera ativa (volta o WDT do IDLE1);
  - entra o canal I2S criado na `tx_audio` (DMA 8 × 128, relógio `INT160_13`), com os **4 pinos a −1**, como o NOPINS:
    o ritmo passa a ser o relógio do ADC;
  - payload: o que o I2S lê, silêncio.
- **PASS:**
  - G(15) com 0/15;
  - MC1–MC3 em todos os boots;
  - `sample_ctr` = 128 × blocos;
  - 2 h com o RX RC1: 0 `adc_ovf` e 0 rebuffers/underruns/`queue_ovf`;
  - `late_drop` e cauda de chegada (`iat_jitter_max_us`) registados, só como descritivo: dependem da margem do RX, e o R2
    decide-se na fase RX.
- **FAIL:** qualquer boot degradado, ou qualquer critério acima falhado.
- **Reversão:** RC2.
- **Substitui:** a montagem do I2S e o ciclo de leitura do `link_tx_run` (sem o PCM1808).

### E3 — PCM1808 real (o gate decisivo)
- **Alteração mínima:** os 4 pinos ativos (MCLK 4, BCLK 5, WS 6, DIN 7); áudio real; payload ainda v2 int32. O padrão de ar
  fica igual ao do A e do BEN.
- **PASS:**
  - **comparativo ABAB** v2-E3 contra o A legado `2abfa75b5`, 15 + 15 boots, avaliador e regras do DIFF-01;
  - presença: A ≥ 50 % degradado;
  - v2 ≤ 20 % e Fisher unilateral p ≤ 0,01;
  - MC em todos os boots;
  - tom de 1 kHz da KA6 gravado ponta a ponta, contínuo;
  - 2 h com o RC1 com os critérios da E2; a cauda de chegada fica registada ao lado da do A no mesmo dia (descritivo).
- **INCONCLUSIVO** se o A ficar abaixo de 50 %: repetir noutra sessão.
- **FAIL** se a v2 ficar ≥ 50 %, ou parcial: pára; a decisão é do utilizador (risco 1). Um PASS demonstra melhoria
  face ao legado, não faz da v2 um produto.
- **Reversão:** RC2.
- **Substitui:** o TX legado na cadeia completa.

### E4 — Empacotamento final (24 bit, protocolo v3)
- **Alteração mínima:**
  - no TX, empacota em 24 bit e põe `version` = 3;
  - no RX, o RC1 + aceitação v3/v2 + desempacotamento;
  - nada mais.
- **PASS:**
  - ida e volta no host bit-exata;
  - teste nulo na cadeia: E4 contra E3 com o mesmo tom, diferença ≤ piso;
  - G(15) com 0/15 (regressão);
  - envio→callback não pior do que na E3;
  - o RX novo reproduz normalmente com o TX RC2 (v2), o que prova a reversão do TX sozinho;
  - 2 h com os critérios da E2.
- **FAIL:** qualquer critério acima falhado.
- **Reversão:** o TX volta à E3/RC2 e o RX volta ao RC1.
- **Substitui:** o payload int32 e o `link_proto` v2 (o v2 fica aceite no RX durante a transição).

### E5 — Integração e qualificação com o RX; troca de produção
- **Alteração mínima:** nenhuma de código. Passam a produção o TX v2-E4 e o RX RC1 + v3.
- **PASS:**
  - a matriz funcional do RC: 10 sequências de reset do TX, 5 resets do RX com o TX em regime, RX-first × 3, TX-first ×
    1 (5 min), 3 cold boots conjuntos;
  - 0 panics/WDT e **0 boots degradados**;
  - sessão adotada em todos;
  - endurance de 12 h com 0 rebuffers/underruns/overflows e 0 `adc_ovf`;
  - os contadores de R2 ficam registados, só como linha de base da fase RX: não decidem a E5.
- **FAIL:** qualquer critério acima falhado.
- **Reversão:** RC2 + RC1.
- **Substitui:** o RC2 em produção. O supervisor sai de produção (critério cumprido: E3 PASS + E5 PASS) e o TX legado
  passa a arquivo.

Depois da E5 começa a fase RX v2 (C1 → C6), uma alteração de cada vez, que leva ao gate abaixo.

---

## GATE PARA CORE V1 FINAL

Tudo com os **binários finais congelados por hash** (TX v2 + RX v2, sem opções de depuração). Valores de
`CORE_V1_REQUIREMENTS.md`, sem ajuste: um número falhado é FAIL.

1. **R1 — latência analógico→analógico:**
   - **p95 ≤ 30 ms e máximo ≤ 35 ms, com n ≥ 20**;
   - método de 2026-09-30: impulso pela KA6, loopback de referência, correlação cruzada;
   - em regime estacionário, com o estado do RX registado em simultâneo.
2. **R2 — regime estacionário ≥ 2 h** (a janela começa depois do arranque):
   - Δ(`rebuffer_low` + `rebuffer_high` + `rebuffer_session`) = 0;
   - Δ`dac_underrun` = 0;
   - Δ`queue_ovf` (RX) = 0 e Δ`adc_ovf` (TX) = 0;
   - `late_drop` **e** `playout_concealment`, cada um com média ≤ 0,05/s;
   - p95 das janelas fixas de 5 min ≤ 0,15/s;
   - `never_recv` ≤ 0,03/s (média da janela).
3. **Condições de validade** (não são requisitos novos; são o que permite chamar "final" a um sistema sem supervisor):
   - a operacionalização de R1/R2 confirmada pelo utilizador antes da primeira avaliação, como o próprio documento exige;
   - E3 PASS (com presença) e E5 PASS com os binários finais;
   - nenhum boot degradado nas corridas de R1/R2. Se acontecer, a avaliação é FAIL e não se repete por sorte.

Os R3–R6 (artefactos, ±100 ppm, arranque ≤ 1 s, telemetria de margem) continuam **propostas**: só entram no gate se o
utilizador os congelar. A 12 h de endurance e a taxa PHY por alcance também ficam fora, salvo decisão do utilizador.
