# Latência do AudioLink: medição, decomposição e proposta (2026-10-30)

**Estado atual: §9 (31/10).** Histórico: **(30/10, 21:10)** **L1 implementado e PASS na bancada.**
- Ganho medido: −10,7 ms no anel DMA; chegada → saída 60,7 → 46,4 ms.
- Detalhes em `experiments/LAT-01/RESULT_L1.md`.
- O RX tem o L1 (`b75e5c7ec`); falta a audição no SU-2.
- L2 parado por decisão do utilizador.
- O C1 (`7ba85c35c`, commit `d3a7962`) é a referência estável e o rollback.

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

**Dados usados.** Só instrumentos e gravações existentes, mais 10 min de gravação passiva do TX com o RX na sala:
- RX: PLAYOUT do C1 (bancada), do JB40 e do R1 (arquivo do W2/R1);
- TX: LINKTX da sala de hoje (20:08–20:18, 627 s), da bancada (JB40, C1, W2, R1) e do RX da Beta 1 na sala (14/10);
- código do TX v2 e do RX, e o driver I²S do IDF 5.4.4.

Ferramentas em `experiments/LAT-01/` (`rxlat.py`, `txlat.py`).

## 1. Onde está a latência hoje (C1)

**Medido no RX, só com o relógio do RX:**
- Instante de chegada do pacote mais recente (`rx_max_at_us`).
- Instante em que esse pacote sai do I²S: `t_us + (enchimento do DMA + rx_max_ctr − exp_ctr)/fs`.

| Componente | C1 | R1 | Natureza | Fonte |
|---|---|---|---|---|
| Filtro do ADC PCM1808 | ~0,4 ms | ~0,4 ms | inevitável | datasheet; PROVÁVEL |
| Enquadramento de 128 tramas no TX | 2,66 ms | 2,66 ms | escolha do pacote | código: a leitura do DMA devolve o bloco quando fica completo; CONFIRMADO |
| TX: leitura → `esp_now_send` | ~0 | ~0 | — | código; CONFIRMADO |
| Transporte (`esp_now_send` → ACK, média) | 0,6–0,9 ms | igual | rádio | LINKTX; CONFIRMADO |
| **Fila do RX (a margem contra o jitter)** | **~39 ms** | ~7–8 ms | a única parte que compra robustez | PLAYOUT; CONFIRMADO |
| **Anel DMA do I²S no RX (8×128)** | **21,3 ms** | 21,3 ms | **acidental: não protege contra o rádio** | PLAYOUT + driver (`desc_num − 1` buffers livres); CONFIRMADO |
| WM8805 + receção S/PDIF e filtro do SU-2 | ~1 ms | ~1 ms | equipamento | HIPÓTESE, não medido |
| **Total da entrada analógica do TX à saída do SU-2** | **~65 ms** | **~33 ms** | | FORTEMENTE SUPORTADO |

- **Chegada → saída I²S, medido:**
  - C1: p50 60,7 ms (p5 55,9; p95 61,1);
  - R1: p50 28,7 ms.
- **O modelo foi validado fisicamente a 30/09:** medição de 39,17 ms contra 35–38 ms previstos
  (`docs/ARCH_REVIEW_2026-10-07.md` §1.2).
- **A tua estimativa de ouvido (~30 ms) está abaixo do valor objetivo.**
- **Fora do AudioLink:** a saída do Mac (CoreAudio) e da KA6 também conta, se comparares com o que sai do Mac. Não
  está medida.

**Leitura:** 61 % da latência do C1 é margem contra atrasos e 33 % é o anel DMA, que não serve para nada contra o rádio.
O resto (~6 %) é fixo.

## 2. O que a margem tem de cobrir: atraso real de entrega

**% de segundos em que pelo menos um pacote demorou mais do que X ms** (`cb_lat_max`, TX; só segundos sem falhas de
envio):

| Janela | > 8 | > 10 | > 13 | > 16 | > 20 | > 25 | > 30 | > 40 |
|---|---|---|---|---|---|---|---|---|
| **Sala, hoje 20:08 (C1 em uso, 627 s)** | 3,0 | 1,4 | 0,5 | 0,3 | 0,16 | 0 | 0 | 0 |
| Bancada JB40, 14:00 (663 s) | 14,2 | 4,7 | 1,2 | 0,45 | 0,15 | 0,15 | 0 | 0 |
| Bancada C1, 19:20–19:45 (503 s) | 56,5 | 45,9 | 22,5 | 1,6 | 0,2 | 0,2 | 0,2 | 0 |
| Bancada W2, 29/10 à noite (2335 s) | 23,6 | 13,5 | 4,0 | 2,5 | 1,1 | 0,56 | 0,43 | 0,21 |
| Bancada R1, 30/10 00:23 (839 s) | 27,7 | 17,5 | 6,4 | 4,4 | 1,6 | 1,2 | 0,7 | 0,36 |

**Outras janelas, que não entram na tabela:**
- **Sala, 30/10 às 02h** (61 s, `TRANSPORT_REVIEW`): p90 do máximo por segundo 24,5 ms, máximo 39,7 ms. É a pior
  amostra.
- **Sala, 14/10, lado do RX da Beta 1:** desvio de chegada > 16 ms em 0,9 % dos segundos. Há ainda episódios de rádio
  > 100 ms em 0,36 % dos segundos, que nenhum buffer cobre.

**Calibração: o modelo prevê as perdas reais (CONFIRMADO).**
- Segundos com `late_drop`: W2 19,0 % (o modelo dá 13,5–23,6 %); R1 11,9 % (margem efetiva ~11 ms).
- Um segundo afetado tem em média ~4,7 blocos perdidos, porque um pacote atrasado atrasa os que vêm atrás (fila FIFO
  com retransmissões).
- **Regra prática:** o R2 congelado (`late_drop` ≤ 0,05/s) equivale a ≤ ~1 % de segundos com atraso acima da margem.

**Margem necessária para o R2, por janela:**

| Janela | Margem |
|---|---|
| Sala hoje | ≥ ~11 ms |
| JB40 | ≥ ~14 ms |
| C1, ao fim da tarde | ≥ ~16 ms |
| W2 | ≥ ~22 ms |
| R1 | ≥ ~26 ms |
| Sala às 02h | ≥ ~35–40 ms |

**Conclusão (FORTEMENTE SUPORTADO):** a cauda dos atrasos muda 3–4× com a hora do dia (tráfego dos vizinhos no canal
6). Uma margem fixa tem de ser dimensionada para os maus períodos.

## 3. Alavancas avaliadas

Latência = ~5 ms fixos + fila (margem + ~1 ms de fase) + anel DMA. A margem de cada configuração é a que a ocupação dá
no limite inferior da banda: (BUFFER − 0,25) × 2,66 ms. Confere com os ~39 ms medidos no C1.

| # | Alavanca | Ganho | Custo para a estabilidade | Avaliação |
|---|---|---|---|---|
| **L1** | **Anel DMA 8 → 4 descritores** (+ `auto_clear_after_cb`) | **−10,7 ms** | Nenhum contra o rádio. Risco: atrasos de escalonamento do playout. S ≲ 0,8 ms medido em 3–12 h (`RX_PLAYOUT_PROPOSAL.md` §2), contra 8 ms de folga = 10×. Um underrun passa a dar silêncio em vez de áudio velho | **Recomendada em 1.º lugar** |
| L1b | Anel de 3 descritores | −13,3 ms | Folga de 5,3 ms (6× S); a escrita dupla dos slips fica mais justa | Só depois de L1 medida |
| **L2** | **Margem 15 → 10 blocos** (BUFFER 10, TARGET 1344, HIGH 1664, fila 14; mesmas relações do JB40) | **−13,3 ms** | Margem ~26 ms: R2 em todas as janelas medidas, salvo o R1 (~1,1 %, no limite) e a amostra das 02h. O que sobrar são mergulhos suaves de 5 ms (C1), não beeps | **Recomendada depois de L1** |
| L2b | Margem 8 blocos | −18,6 ms | Margem ~21 ms: R2 no limite no W2/R1 (~1–1,5 % de segundos) | Possível, com a sala a confirmar |
| L2c | Margem 6 blocos (≈ 30 ms totais) | −23,9 ms | Margem ~15 ms: falha o R2 nos maus períodos (2,8–6 % de segundos, ~0,13–0,28/s); bem nos bons | Só com perfil ou margem adaptativa |
| L3 | Reduzir a cauda do transporte: canal menos usado (A/B da `cb_lat`, 5 min por canal, TX+RX), limite de pacotes em voo no TX | Indireto: permite L2b/L2c | Mexe no TX e no RX | A medir depois de L1/L2, se for preciso |
| L4 | **Margem adaptativa:** cresce numa perda (prolonga o mergulho que já existe) e encolhe devagar (salta 1 bloco com fade quando a margem medida sobra durante minutos) | Ficaria em ~25–30 ms nos bons períodos | Novo laço de controlo; precisa de telemetria de margem por pacote. O corretor por slip (8,3 ppm ≈ 1 bloco a cada 5 min) não serve para isto | A melhor resposta à variabilidade medida, mas **não agora**. Candidata se L1+L2 não chegarem |
| L5 | Perfis fixos (música / baixa latência) | — | Só se trocam regravando, ou com uma flag no protocolo | Pouco valor |
| L6 | Pacotes de 64 tramas | −1,3 ms | Dobra os pacotes/s e ~+35 % de antena: mais exposição à contenção | Não |
| L7 | ESP-NOW sem ACK + redundância, ou 5 GHz (outro hardware) | Seriam os únicos caminhos para ~20 ms | Reescrita do transporte ou compra | Não agora |

## 4. Os objetivos são possíveis?

Mínimo fixo com o anel reduzido: ~5 ms (conversores, enquadramento, transporte, SU-2) + 8–11 ms de anel ≈ 13–16 ms,
antes de qualquer margem.

| Total | Margem que sobra | Resultado medido | Etiqueta |
|---|---|---|---|
| **20 ms** | ~3–6 ms | Mesmo no melhor período medido (sala hoje), 10–27 % dos segundos com perdas, ~0,5–1 mergulho/s; nos maus, até 70 %. É o regime do R1 ou pior | **Não é possível** com ESP-NOW unicast neste canal de 2,4 GHz. FORTEMENTE SUPORTADO |
| **30 ms** | ~15 ms | Cumpre o R2 nos bons períodos (sala hoje 0,35 %, JB40 ~0,6 %); falha nos maus (W2/R1/C1 ao fim da tarde: 2,8–6 %) | **Possível só com compromisso:** mergulhos suaves nos maus períodos, ou margem adaptativa (L4). PROVÁVEL |
| **~36 ms** (L1 + L2b) | ~21 ms | R2 em quase todas as janelas medidas | FORTEMENTE SUPORTADO (modelo calibrado) |
| **~42 ms** (L1 + L2) | ~26 ms | R2 em todas, salvo a amostra das 02h | FORTEMENTE SUPORTADO |
| ~55 ms (só L1) | ~39 ms | Igual ao C1 | CONFIRMADO pelo C1 + mecanismo |

**Requisitos congelados (`CORE_V1_REQUIREMENTS.md`, 08/10):** R1 pede p95 ≤ 30 ms e R2 pede ≤ 0,05 perdas/s. **Com
ESP-NOW neste ambiente, os dois juntos não se cumprem nos maus períodos.** É preciso escolher entre:
- latência fixa ~36–42 ms com o R2; ou
- ~30 ms com mergulhos suaves ocasionais; ou
- investir na margem adaptativa (L4).

## 5. Proposta

1. **L1: anel DMA de 4 descritores + `auto_clear`**, numa variante isolada do C1.
   - Opção de Kconfig; a métrica `deadline_miss` passa a usar a folga do anel novo.
   - Testes: auditoria de objetos e 10 min na bancada.
   - Critérios:
     - `dac_underrun` +0 com áudio;
     - pausas do loop < 50 % da folga;
     - chegada → saída I²S ≈ 50 ms (hoje 60,7);
     - depois, audição.
   - **−10,7 ms sem custo de robustez.**
2. **L2 (margem 10 blocos), medida.** Bancada, mais 10 min de gravação do TX num período mau (fim da tarde), mais a
   sala. **−13,3 ms. Total ~42 ms.**
3. **Medição física de ponta a ponta,** para fixar o valor absoluto antes e depois.
   - `latency_measure.py` (impulso pela KA6 + loopback de referência), na placa RX PCM5102A, que tem o mesmo playout.
   - Exige ligar a saída analógica dessa placa à KA6 e gravar nela a build equivalente (não é produção).
4. **Só depois, e se ainda for preciso:** L2b ou L2c, L3 (canal) ou L4 (margem adaptativa). Cada um com a sua medição.

**Rollback:** o C1 (`release/C1/RX_WM8805`, commit `d3a7962`) com `c1_flash.sh <porta> c1`.

## 6. Decisões do utilizador

- **Para que é a latência?**
  - Sincronização com vídeo: até ~40–45 ms costuma ser imperceptível.
  - Ouvir ao mesmo tempo o som do Mac/KA6 e o do SU-2 (DJ, monitorização): aí até 30 ms se ouve como eco. A solução
    pode passar por atrasar a outra saída em vez de baixar esta.
- **Se o R1 (≤ 30 ms) se mantém,** perante o conflito com o R2 demonstrado na §4.

## 7. Depois do L1: o que sobra e o que ainda se pode tirar sem mexer na margem (30/10, 21:40)

**Latência do L1 em regime:** ~50 ms ponta a ponta (chegada → saída ~45 ms, mais ~5 ms fixos).

| Fonte | Valor | Removível sem perder robustez? | Evidência |
|---|---|---|---|
| **Margem da fila** (contra atrasos do rádio) | ~35–40 ms (14–15 blocos) | **Não** sem decidir a robustez (L2) | Medido |
| Dente de serra do prazo (ocupação em blocos inteiros) | A margem oscila **1 bloco (2,66 ms)** em ciclos de ~10–20 min, ~1,3 ms em média | Só com prazo de grão fino e controlador novo (C3 de 08/10): ~1,3 ms por muita complexidade. **Não** | **Medido agora:** C1 aos 51 min, ocupação 15 → 14 e `occ_ema` 13,98 abaixo da banda; slips ao ritmo máximo (0,4 tramas/s), ~11 min para recuperar |
| Anel DMA | 10,6 ms (4 descritores) | **−2,66 ms com 3 descritores.** Folga 5,3 ms = 7× o pior atraso medido do escritor; o L1 mostrou a folga nunca gasta | Medido |
| Transitório depois do arranque ou resync | +~2,6 ms nos primeiros ~4–6 min | Não vale a pena: é transitório e o controlador leva-o ao ponto de regime | Medido (JB40 e L1: `occ_ema` 15,9 → 14,9) |
| Enquadramento no TX | 2,66 ms | −1,33 ms com pacotes de 64 tramas, mas muda TX e protocolo e dobra os pacotes/s (mais exposição ao rádio). **Não** | Aritmética; o efeito no rádio é especulativo |
| Transporte (média) | 0,6–0,9 ms | Não: é física | Medido |
| ADC, WM8805 e SU-2 | ~1–2 ms | Talvez o filtro do SU-2, se for selecionável | Não medido |
| Fora do AudioLink: Mac/CoreAudio → KA6 → entrada do TX | Desconhecido | Configuração do Mac/KA6, não firmware | Não medido |

**Conclusões:**
- Fora da margem, o único ganho demonstrável e barato que resta é o **anel com 3 descritores (−2,66 ms)**.
- O resto é físico, de protocolo, ou complexo para ~1 ms.
- **~75 % da latência que sobra é margem contra o rádio.** O próximo ganho grande só pode vir do L2.
- Como a cauda varia com a hora, o L2 deve ser dimensionado com dados dos maus períodos da sala. O instrumento já
  existe e não muda nada: o recorder só do TX, no Mac, durante o uso normal (`txlat.py`).
- **Para transformar a soma em medição física de ponta a ponta:** `latency_measure.py` com a placa RX PCM5102A (mesmo
  playout) ligada à KA6.

## 8. Decisões de 30/10 (noite) e preparação do L2

**Decisões do utilizador:**
- **Fica o anel de 4 descritores** do L1; não se passa a 3.
- O L2 não avança antes de a audição do L1 estar validada e de haver dados dos maus períodos.
- Nada de margem adaptativa nem de mexer no rádio.

### 8.1 A oscilação da ocupação reduz a margem real (CONFIRMADO)

**O mecanismo:**
- O controlador de deriva vê a ocupação em blocos inteiros e corrige 1 trama a cada 2,5 s (0,4 tramas/s).
- Quando a fase de chegada atravessa a fronteira de um período, a ocupação cai um bloco inteiro e fica lá até o
  controlador repor 128 tramas.

**No C1 aos 51 min:**
- a ocupação passou de 15 para 14;
- `occ_ema` ficou em 13,98, abaixo do limite da banda (14,75);
- slips ao ritmo máximo;
- ~11 min até repor.

**Consequência:** durante esse tempo, a margem real é **1 bloco (2,66 ms) menor** do que a configuração indica.

**Regra para o L2:** dimensionar sempre pela **ocupação mínima observada (BUFFER − 1 blocos)**, nunca pelo alvo.

### 8.2 Leitura preliminar do L2 (`experiments/LAT-01/l2_eval.py`, dados existentes)

Perdas/s estimadas com a margem mínima. R2 ≤ 0,05/s; * = falha.

| Janela | BUFFER 8 (~31 ms) | 10 (~37 ms) | 11 (~39 ms) | 12 (~42 ms) | 15 (~50 ms, L1) |
|---|---|---|---|---|---|
| Sala, 30/10 20:08 | 0,007 | 0 | 0 | 0 | 0 |
| Bancada 14:00 | 0,007 | 0,007 | 0,007 | 0 | 0 |
| Bancada 19:20 | 0,019 | 0,009 | 0,009 | 0,009 | 0 |
| Bancada W2 (noite de 29/10) | 0,068* | 0,030 | 0,024 | 0,020 | 0,010 |
| Bancada R1 (00:23) | 0,112* | 0,062* | 0,050 | 0,039 | 0,022 |

**Leitura:**
- BUFFER 12 cumpre o R2 em todas as janelas medidas: −8 ms, ~42 ms.
- BUFFER 10 só falha na pior janela de bancada: −13 ms, ~37 ms.
- Abaixo de 10, falha nos maus períodos.
- **Ainda não há dados dos maus períodos na sala.** A única amostra má da sala (02h de 30/10, 61 s, só o resumo)
  sugere que podem ser piores do que os da bancada.
- **O L2 compensa (8–13 ms, 16–26 % do que resta) se os maus períodos da sala não forem piores do que os da
  bancada.**

### 8.3 Recolha dos dados (passiva, limitada)

**`experiments/LAT-01/tx_session_rec.sh [minutos] [max_mb]`:**
- grava só a consola do TX, durante uma sessão normal com o RX na sala;
- omissão: 120 min e 50 MB;
- no fim imprime o resumo e a tabela do L2.

**Garantias:**
- para no tempo, no limite de MB ou com < 5 GB livres no disco;
- o recorder é filho do script, e Ctrl-C, HUP ou TERM param-no e confirmam que morreu;
- um temporizador independente mata-o no prazo + 2 min, mesmo que o script seja morto à força;
- recusa arrancar se já houver um recorder;
- nunca abre a porta do RX.

**Testado a 30/10 21:50 (só o TX, com o RX na bancada):**
- fim normal ao fim de 1 min, sem restos;
- TERM ao script: parou aos 20 s, com o recorder e o temporizador mortos;
- KILL ao script: o recorder ficou órfão só até ao prazo e o temporizador matou-o; 0 processos no fim.

As sessões de teste foram apagadas.

**Armadilha encontrada:** com uma config só do TX, o `alfr` junta a porta de RX por omissão (`/dev/tty.usbmodem83101`),
que hoje é a do TX. A 30/10 20:08 só o trinco exclusivo evitou que o leitor "rx" abrisse o TX. O script desliga
explicitamente a porta de RX.

**Critério de paragem da recolha:** 2–3 sessões ao fim da tarde ou à noite, ou uma sessão que mostre um mau período.
Basta para escolher BUFFER entre 10 e 12, ou para concluir que o L2 não compensa. Não é uma campanha.

## 9. Estado da fase (31/10, 01:15 UTC)

### 9.1 Versões

| Versão | Papel | Estado |
|---|---|---|
| C1 `7ba85c35c` (commit `d3a7962`) | Rollback de segunda linha | Estável na sala |
| L1 `b75e5c7ec` (commit `0e60ad7`) | Rollback do L2-12 | Estável na sala (audição PASS); anel DMA com 4 descritores; margem de 15 blocos |
| **L2-12 `39b5b9a6f`** | **Referência funcional desde 31/10** | PASS na bancada (`experiments/LAT-01/RESULT_L2-12.md`) e **PASS na audição no SU-2**. A margem RF nas condições mais difíceis da sala continua por medir. Fixado em Git a 31/10 |

**Rollback:** `experiments/LAT-01/l2_flash.sh /dev/cu.usbmodem83201 l1` (ou `c1`).

### 9.2 Latência medida (chegada → saída I²S, p50; ponta a ponta ≈ +5 ms)

| Versão | Chegada → saída |
|---|---|
| C1 | 60,7 ms (logo após o arranque), 55,6 ms (regime) |
| L1 | 46,4 ms (logo após o arranque), 43,6 ms (regime) |
| L2-12 | 41,7 ms (logo após o arranque) |

- Ganho do L2-12 sobre o L1: 3 blocos estruturais (8 ms em média); 4,7 ms numa comparação curta.
- Cada medição curta tem ±2,7 ms de fase (ciclo de ~10 min).

### 9.3 O que se sabe da cauda do transporte

- **A cauda vem sobretudo da espera pelo canal livre e da acumulação atrás de um pacote atrasado,** não das nossas
  retransmissões. Amostra de 10 s com o RX na bancada; **não verificado na sala**: a captura de 120 s ficou por fazer
  quando o E-ATTR foi suspenso.
- **Mecanismo identificado** (PROVÁVEL): o AP vizinho V muda de canal sozinho (6–11). Emite comboios
  de respostas de sonda a 1 Mb/s, retransmitidas, de ~3,5 ms cada, até ~25 ms por comboio.
- Broadcast, pacotes de 64 tramas ou N+N−1 não encurtariam esta espera (`BROADCAST_REVIEW_2026-10-30.md`).

### 9.4 De onde pode vir o próximo ganho significativo

Avaliação pelas evidências, sem nada implementado:
- **Reduzir mais o buffer:** não. Cada bloco a menos expõe diretamente a margem aos comboios do vizinho.
- **Previsibilidade do transporte:**
  - o único controlo do nosso lado é o canal, e o vizinho muda de canal sozinho, por isso um canal fixo só ajuda parte
    do tempo;
  - avalia-se de forma passiva (captura E-ATTR por canal, suspensa).
- **Arquitetura:**
  - **margem adaptativa:** baixa latência nos períodos calmos, mais margem durante os comboios. A variabilidade medida
    justifica-a, mas é um laço de controlo novo;
  - **5 GHz** (outro hardware): as tramas de gestão a 1 Mb/s dos 2,4 GHz desaparecem. Mas as compras estão
    congeladas.

### 9.5 Trabalho por commitar (preservado na cópia de trabalho, por decisão do utilizador)

- `release/L2-12/`, `experiments/LAT-01/{PLAN,RESULT}_L2-12.md`, `l2_flash.sh`;
- ferramentas E-ATTR (`eattr_capture.sh`, `eattr_analyze.py`) e §8–§12 da `BROADCAST_REVIEW`;
- correção de `tx_session_rec.sh` (espaço em disco sob `sudo -u`);
- `tests_host/test_link_resync.c` tornado genérico (`-DB`/`-DDEPTH`).
