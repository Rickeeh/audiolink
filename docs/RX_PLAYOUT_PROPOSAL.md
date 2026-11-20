# Playout do RX — revisão profunda e proposta de arquitetura (2026-10-08)

**Estado:** proposta para decisão. **Nada foi implementado nem alterado no firmware.**
- Base: `ARCH_REVIEW_2026-10-07.md`, `experiments/EXP-RX-TARGET-704/offline_sawtooth/` e
  `experiments/EXP-OFFLINE-SLIP-QUALITY-01/`.
- **Não se assume que "DMA curto + PI + ASRC" é a solução.** Cada peça é tratada como hipótese com uma experiência que a
  distingue das alternativas.
- O protocolo e o transporte não mudam neste trabalho; o payload é tratado em separado (`PAYLOAD_RADIO_STUDY.md`).

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

---

## 0. Requisitos
**Congelados pelo utilizador em 2026-10-08** (`CORE_V1_REQUIREMENTS.md`; não se ajustam aos resultados):
- **R1 — latência analógico→analógico:** p95 ≤ 30 ms, máximo ≤ 35 ms, n ≥ 20. Objetivo de desenho: 20–25 ms.
- **R2 — regime estacionário ≥ 2 h:**
  - 0 rebuffers, underruns e overflows;
  - `late_drop`/ocultações com média ≤ 0,05/s e p95 das janelas de 5 min ≤ 0,15/s;
  - `never_recv` ≤ 0,03/s.

**Ainda propostas, não congeladas** (a decidir pelo utilizador):

| | Requisito proposto | Como se mede |
|---|---|---|
| R3 | Nenhum artefacto tonal: uma perda nunca produz repetição sustentada; correção de drift ≤ −90 dBFS com um tom a −20 dBFS | gravação pela KA6 com tons de teste |
| R4 | Drift coberto até ±100 ppm sem perdas (o corretor atual vai até 8,3 ppm) | simulação + par de placas com maior diferença |
| R5 | Arranque e ressincronização sem depender do estado anterior; áudio em ≤ 1 s depois da 1.ª trama | testes de arranque |
| R6 | Observabilidade: margem real por pacote (min, p1, p50 por segundo) e latência fim-de-descritor → escrita | telemetria |

**Consequência direta de R1:** o objetivo de 20–25 ms obriga a recuperar a maior parte dos ~19 ms do anel DMA do RX. Já
não é uma escolha "menos latência ou mais margem".

## 1. Latência real por componente

| Componente | Valor | Fonte e etiqueta |
|---|---|---|
| ADC PCM1808 + DAC PCM5102A (filtros digitais) | ~1 ms | datasheets, valor não verificado aqui — PROVÁVEL |
| Enquadramento de 128 tramas | 2,66 ms, a mesma para todas as amostras | aritmética — CONFIRMADO |
| TX: leitura I2S → `esp_now_send` | ~0 ms (lê o descritor quando fica cheio) | código (`link_tx.c`, IDF) — CONFIRMADO |
| Pilha + ar TX→RX | ~1–2 ms (`cb_lat` envio→ACK 1,1–2,7 ms) | logs — PROVÁVEL |
| Espera na fila ≈ (ocupação − 0,5) × 2,66 ms | 8,2 ms (LD1, 3,57 blocos); 10,9 ms (T704, 4,61); 12–14,6 ms na medição de 2026-09-30 (5–6 blocos) | logs — FORTEMENTE SUPORTADO |
| Anel DMA do RX | 18,7–19,6 ms (7 blocos, mais o deslocamento dos slips) | enchimento medido + código — CONFIRMADO |

- **Medição de referência:** 39,17 ± 0,11 ms (2026-09-30), feita 10–45 s depois do arranque.
  - O modelo dá ~35–38 ms para essa ocupação: **o resíduo é ~1–4 ms**, não 5–8 ms (ver revisão, secção 7). DESCONHECIDO
    dentro dessa margem: a ocupação nessa altura só foi registada como média inteira.
  - **Em regime, hoje:** ~31–32 ms (RC1/576) e ~34 ms (T704), com oscilação de ±1,3 ms (dente de serra), mais o
    deslocamento lento depois de cada arranque.
- **Inevitável com este hardware:** ~1 ms de conversores, 1,3–2,7 ms de enquadramento e ~1 ms de pilha.
- **Necessário mas dimensionável:** a margem contra jitter.
- **Acidental:** os ~19 ms do anel DMA.
- **Para fechar o resíduo (experiência mínima):** repetir a medição física com o script reconstruído e, em simultâneo,
  registar no RX a ocupação/margem, o enchimento do DMA e os slips mod 128.
  - Critério: |medido − modelo| ≤ 1 ms.
  - Sem alterar firmware, se a telemetria de margem (C1) já existir; senão, com a telemetria atual e mais incerteza.

## 2. DMA mínimo seguro e condições para não haver underrun
**Mecanismo (código do IDF 5.4.4, `i2s_common.c`) — CONFIRMADO:**
- O anel tem N descritores de F tramas e a fila de buffers livres tem N − 1 lugares.
- No fim de cada descritor, o ISR devolve o buffer. O escritor bloqueia até haver um livre, enche-o, e esse buffer só
  volta a tocar depois dos outros N − 1.
- Se o escritor não pegar num buffer livre a tempo, o DMA volta a tocar o conteúdo antigo: `auto_clear` está desligado
  por omissão nos dois nós. Isto é um eco de áudio velho, não silêncio. O `on_send_q_ovf` só conta o evento.

**Condição:** não há underrun se o atraso do escritor depois de cada fim-de-descritor (S) cumprir S < (N − 1)·F/fs − t_escrita.

**O que os dados mostram:**
- **CONFIRMADO:** as pausas máximas de 5,3 ms entre escritas são as escritas duplas nas voltas dos 128 slips, não
  atrasos de escalonamento.
- **Pausa máxima por segundo sem esses segundos:** 3374 µs (T704, 2 h), 3381 µs (LD1, 3 h) e 3444 µs (endurance, 12 h),
  contra 2662 µs nominais: **S ≲ 0,8 ms** — FORTEMENTE SUPORTADO. É uma medida indireta: início a início de escrita, por
  segundo, com o playout no core 0, prioridade 1, ao lado da tarefa Wi-Fi.
- **DESCONHECIDO:**
  - a distribuição direta de S (fim-de-descritor → escrita);
  - S sob picos de carga Wi-Fi;
  - o custo da callback de receção no core 0.

**Candidatos** (latência do anel ≈ (N − 1)·F/fs; o grão do prazo é F tramas):

| N × F | Folga (lead) | Grão do prazo | Interrupções/s | Comentário |
|---|---|---|---|---|
| 8 × 128 (hoje) | 18,7 ms | 2,66 ms | 376 | folga de 23× sobre S; quase toda é latência morta |
| 4 × 128 | 8,0 ms | 2,66 ms | 376 | ~10× sobre S; não resolve o grão |
| 6 × 64 | 6,7 ms | 1,33 ms | 751 | |
| 8 × 32 | 4,7 ms | 0,67 ms | 1502 | ~6× sobre S; grão fino |
| 4 × 32 | 2,0 ms | 0,67 ms | 1502 | pouca folga para S desconhecido |

- **Salvaguarda independente do tamanho:** ativar `auto_clear_after_cb`, para que um underrun dê silêncio em vez de áudio
  velho. Mantém-se a contagem com `on_send_q_ovf`.
- **Decisão adiada até C1 medir S diretamente.**
  - Critério proposto: S máximo ≤ 50 % da folga em ≥ 12 h, e 0 `q_ovf`.
  - Mudar a tarefa de playout para o core 1 com prioridade alta é uma alteração à parte (uma de cada vez), só se os dados
    o pedirem.

## 3. Dente de serra: é possível eliminá-lo sem ASRC?
**Causa — CONFIRMADO, 3 corridas:**
- O prazo de consumo está quantizado ao descritor (F = 128 tramas) e cada iteração consome um bloco inteiro.
- O slip corrige o conteúdo trama a trama, mas o prazo só recua em degraus de F, a cada F slips líquidos.
- Daí um dente de serra de F tramas com período F / slip líquido (~7 min), e perdas 1,6–3,2× maiores no fundo.

**O ASRC sozinho não resolve isto:** também consome por descritor.

**Alternativas:**

| | Opção | Efeito no dente de serra | Complexidade | Riscos |
|---|---|---|---|---|
| a | Descritores pequenos (F = 32), mesmo slip | amplitude 2,66 → 0,67 ms | baixa (só configuração do I2S e escrita em blocos de 32) | mais interrupções e iterações; a folga tem de chegar para S |
| b | Saltar/repetir um bloco inteiro com crossfade quando a deriva soma 1 bloco | 1 bloco, raro (~7 min) | baixa | um evento audível de 2,66 ms; não estabiliza a latência |
| c | Escritor que preenche o buffer à medida da necessidade real (enchimento parcial) | quase nula | média/alta (contra o modelo de escrita bloqueante do driver) | frágil; difícil de verificar |
| d | Prazo e margem medidos em µs (secção 5) + controlador proporcional sem banda morta, com (a) | ±0,33 ms | baixa/média | precisa de C1 primeiro |

**Proposta:** (a) + (d).
- Fica a correção trama a trama atual ou a da secção 4, mas o prazo passa a ter grão de 0,67 ms e o controlador
  persegue a margem medida, não a ocupação inteira.
- **Isto é mais simples do que um ASRC** e ataca a causa do problema de *tempo*. O ASRC resolve outro problema: o da
  *qualidade*.

**Controlador:**
- Passa a ser proporcional sobre a margem filtrada (ou PI, se o integral for preciso para o drift), com limite de
  velocidade.
- **Saem:** banda morta (HYST), cooldown e gate RF.
- O ponto de operação passa a ser o alvo, seja qual for o sinal do drift.
- A capacidade passa de 8,3 ppm para ≥ 100 ppm (R4).

**Experiência mínima que distingue as alternativas:** simulação offline com as chegadas medidas por pacote (C1)
reproduzidas contra (atual), (a), (b) e (a+d).
- Métricas: amplitude do dente de serra, margem p1 e perdas.
- Critério para avançar: (a+d) com dente de serra ≤ 0,7 ms pico a pico e perdas ≤ ½ das do atual, com a mesma latência.
- Depois, validação em hardware (12 h).

## 4. O ASRC fracionário justifica-se?
**Evidência:** `EXP-OFFLINE-SLIP-QUALITY-01` (numérico, tom a −20 dBFS).
- **Slip atual:** artefacto de −36 a −56 dBFS, muito acima do piso do ADC (−79/−87 dBFS), ~0,3 vezes/s. — CONFIRMADO no
  modelo; audibilidade PROVÁVEL em material tonal.
- **Transição longa (~170 ms) com interpolação cúbica:** ≤ −100 dBc, ao nível da referência. Mas a cúbica atenua os
  agudos conforme a fração de atraso (−0,5 dB a 10 kHz, −2,5 dB a 15 kHz), o que vira modulação lenta.
- **Interpolador plano** (sinc polifásico de 32–64 taps): sem os dois efeitos. Custo estimado ~6 MMAC/s em estéreo.

**Alternativas, por complexidade:**
1. Slip atual: fora de R3.
2. Slip com rampa longa e cúbica: simples, só ativo durante a rampa, com modulação de agudos residual.
3. Atraso fracionário contínuo com interpolador plano, comandado pelo controlador da secção 3: o "ASRC" mínimo, sem
   estimação de rácio própria, porque o controlador já a dá.

**Leitura:**
- Para "estéreo de elevada qualidade", a opção 3 justifica-se em qualidade. O custo de complexidade é moderado e local
  (uma função de filtro).
- **É um passo separado do de tempo** (C5), feito depois de C3. Não se juntam.

**Experiência mínima:** gravar a saída do DAC real pela KA6 com tons de 1/10/15 kHz e eventos de correção forçados,
nas variantes 1, 2 e 3.
- Critério (R3): artefacto ≤ −90 dBFS na janela do evento e modulação de agudos ≤ 0,1 dB.
- Se a opção 2 cumprir, fica a 2; a 3 é a que deverá cumprir.

## 5. Margem por prazos reais, em vez de ocupação aproximada
**Definição, por bloco k:**
- A_k = instante de chegada (esp_timer, na callback do RX; já é lido hoje).
- D_k = instante em que o playout precisa do bloco (a iteração que o consome ou ocultaria).
- **Margem m_k = D_k − A_k.**
  - Perda por atraso ⟺ m_k < 0. Para um bloco descartado por velho, m_k = −(atraso).
  - Por segundo: mínimo, p1, p50 e número de negativos.

**Porque é melhor do que a ocupação:**
- é contínua (µs);
- não depende do padrão de chegada nem do grão de 128 tramas (o `occ_ema` vê saltos de bloco inteiro, secção 3);
- prevê diretamente as perdas;
- dá a cauda para dimensionar o alvo (R2).

**O que pode remover:** EMA lenta, EMA rápida e EMA de overflow, LOW/HIGH water e a regra de margem. Os seus papéis
passam a ser:
- "margem abaixo do alvo" para o controlador;
- "N blocos seguidos em falta" para o mute/ressincronização;
- "fila cheia" para descartar o mais antigo.

**Primeiro passo:** C1, só observação (sem mudar comportamento).
- Validação do instrumento: o número de margens negativas tem de igualar o `late_drop` (± o resíduo dos descartados
  por velhos).

## 6. Mecanismos que se podem remover sem perder garantias

| Mecanismo | Garantia que dá hoje | Substituto na proposta | Remover quando |
|---|---|---|---|
| Bitmap de duplicados (200 000 bits, desliga-se aos 532 s) | um bloco não toca duas vezes | anel indexado por `sample_ctr`: um duplicado cai na mesma posição | depois de C3 |
| MAX_JUMP (`corrupt_jump`) | um `sample_ctr` implausível não envenena o estado | janela do anel (rejeita k > esperado + capacidade) e `session_id` | depois de C3 |
| `previous_session_id` | uma trama atrasada da sessão antiga não reabre a antiga | **manter** (é barato e cobre um caso real) | — |
| EMA lenta, EMA rápida e EMA de overflow; LOW/HIGH water; OVF_RATE | correção de drift; recuperar de apagões e de fila cheia | margem por prazos (C1/C3), "N em falta → mute e ressincronização", "fila cheia → descartar o mais antigo" | depois de C3, com a mesma cobertura de testes |
| HYST, cooldown e regra de margem | estabilidade da banda morta | controlador sem banda morta (C3) | depois de C3 |
| Gate RF (freeze de 5 s) | não corrigir drift durante rajadas | estimativa de drift robusta (declive de longo prazo da margem) | depois de C3 |
| Repetir o último bloco sem fade | preencher uma perda | PLC: repetir com fade-out 1–2 blocos, mute ao 3.º, crossfade no regresso | C4 |
| Diagnóstico temporário (tabela de MACs, split recebido/enfileirado, timeline do arranque, snapshot de ~60 campos com 1 KB de RSSI copiado sob spinlock) | depuração | telemetria compacta e documentada (margem, perdas, drift, `q_ovf`) | depois de C1 |

## 7. Sequência proposta — cada alteração com o modelo de disciplina
**Regra:** uma alteração de cada vez, cada uma atrás de uma opção de Kconfig (desligada = binário atual). Nunca ao mesmo
tempo que mudanças de protocolo, transporte ou payload.

### C1 — Telemetria de margem por prazos + latência fim-de-descritor → escrita (só observação, RX)
- **Problema demonstrado:** o controlador e as decisões assentam numa ocupação inteira e quantizada (saltos de bloco
  CONFIRMADOS). Não há medida direta da margem nem da latência de escalonamento do playout.
- **Causa:** só existe instrumentação de ocupação, e a de tempo é indireta (pausas por segundo).
- **Alternativas:** inferir a partir da ocupação (atual, enviesado); registo por pacote em PSRAM com despejo posterior
  (mais detalhe, mais memória); estatísticas por segundo (proposto). Os dois últimos podem coexistir.
- **Experiência mínima:** RX instrumentado, sem mudança funcional, 12 h com o TX RC2 e o mesmo método da LD1.
- **Critério de aprovação:**
  - o binário só difere na telemetria (secções carregáveis comparadas, como no RC1);
  - o número de margens negativas é igual ao `late_drop` ± 1 %;
  - a distribuição de S fica medida.
- **Impacto:** latência 0; robustez 0; qualidade 0; manutenção + um bloco de telemetria (substitui o diagnóstico ad hoc
  a seguir).
- **Reversão:** Kconfig desligado, igual ao binário anterior; RX RC1 disponível.

### C2 — Folga de DMA dimensionada e `auto_clear` (RX)
- **Problema demonstrado:** 18,7–19,6 ms de latência que não protege contra jitter (CONFIRMADO).
- **Causa:** N = 8 escolhido sem medida de S.
- **Alternativas:** manter 8 × 128 e aumentar a fila (mais latência); reduzir N com F = 128; ou ir já para F = 32 (C3).
- **Experiência mínima:** com S medido em C1, escolher N (critério da secção 2) e correr 12 h.
- **Critério:**
  - 0 `q_ovf`, 0 underruns e 0 pausas acima de 50 % da folga;
  - latência física medida menor pela diferença prevista (±1 ms);
  - perdas não piores à mesma margem.
- **Impacto:**
  - latência −10 a −14 ms (necessário para R1; o objetivo de desenho de 20–25 ms não deixa a alternativa "mesma
    latência com mais margem");
  - robustez igual ou melhor; qualidade igual (o `auto_clear` só atua em falha);
  - manutenção: uma constante.
- **Reversão:** Kconfig (N = 8).

### C3 — Prazo com grão fino + controlador sem banda morta (RX)
- **Problema demonstrado:**
  - dente de serra de ~1 bloco com perdas concentradas no fundo (CONFIRMADO);
  - ponto de operação no limiar de INSERT e não no TARGET (CONFIRMADO);
  - capacidade de 8,3 ppm.
- **Causa:** grão de 128 tramas; banda morta com cooldown.
- **Alternativas:** as da secção 3 (a, b, c, d).
- **Experiência mínima:** simulação com as chegadas de C1 e depois hardware 12 h, com o mesmo boot do TX e a mesma hora
  quando possível.
- **Critério:**
  - dente de serra ≤ 0,7 ms pico a pico;
  - mediana da margem a ±0,5 ms do alvo;
  - perdas ≤ ½ das do baseline à mesma latência;
  - drift simulado de ±100 ppm sem perdas.
- **Impacto:**
  - latência: mais estável (necessário para R1);
  - robustez: melhor no fundo do ciclo;
  - qualidade: igual (o slip ainda é o atual);
  - manutenção: sai HYST, cooldown, gate RF e as EMAs da decisão.
- **Reversão:** Kconfig, que volta ao controlador atual.

### C4 — PLC com fade e mute (RX)
- **Problema demonstrado:** repetição sem fade até ~0,6 s num apagão. CONFIRMADO no código; audibilidade não medida.
- **Causa:** o "concealment" foi desenhado só como preenchimento.
- **Alternativas:** fade-out + mute; repetição com crossfade; PLC por previsão (mais complexo, só se a escuta o exigir).
- **Experiência mínima:** apagões forçados (TX desligado 0,1/0,5/2 s) com tom e com música, gravados pela KA6.
- **Critério:** nenhuma repetição audível acima de 2 blocos; entrada e saída sem cliques (≤ −60 dBFS); retoma em ≤ 50 ms.
- **Impacto:** latência 0; qualidade melhor; manutenção pequena.
- **Reversão:** Kconfig.

### C5 — Correção de drift de alta qualidade (RX)
- **Problema demonstrado:** artefacto do slip de −36 a −56 dBFS (modelo; secção 4).
- **Causa:** a inserção abrupta de 1 amostra.
- **Alternativas:** rampa longa com cúbica; atraso fracionário contínuo com interpolador plano.
- **Experiência mínima:** a da secção 4, no DAC real.
- **Critério:** R3.
- **Impacto:** latência + o atraso de grupo do filtro (~0,3–0,7 ms, se for simétrico de 32–64 taps); qualidade melhor;
  CPU ~6 MMAC/s; manutenção: uma função.
- **Reversão:** Kconfig.

### C6 — Remoção dos mecanismos substituídos (RX)
- Só depois de C3–C5 aprovados.
- Remove-se o que a secção 6 marca, com a mesma cobertura de testes de arranque, sessão, apagão e endurance.
- **Critério:**
  - a matriz de validação do RC (arranques, sessões, apagões) passa;
  - a endurance (≥ 2 h; 12 h como confirmação) cumpre R2.

## 8. Decisões que ficam com o utilizador
1. R1 e R2 estão congelados (`CORE_V1_REQUIREMENTS.md`). Ficam por decidir os R3–R6, ainda só propostas.
2. O C1 está aprovado em princípio e corre **só depois do EXP-DEEP-TX-DIFF-01**, como build exclusivamente
   observacional e seguro para o tempo real (decisão do utilizador, 2026-10-08).
3. Se o diferencial DEEP-TX der "dependente da aplicação", a ordem relativa entre corrigir o TX e o C1. Recomendação: a
   causa do TX primeiro, porque também pode mudar a cauda de jitter.
