# RX: "beeps" da ocultação — proposta curta (2026-10-30)

**Estado (30/10, fim do dia):** §1–§5 são o histórico da proposta. **A decisão e a implementação estão em §6**, que
substitui §5.1 (decisão de não repor a sequência) e §5.2 (fade para a frente: tinha um click). Implementado no C1,
**ainda não gravado** (`experiments/C1/PLAN.md`).

**Origem:** `experiments/W4-SU2/RESULT_W4.md`. Numa gravação no SU-2, as rajadas têm os harmónicos de 375,6 Hz
(= 48 076,9/128), a assinatura da repetição de um bloco de 128 tramas.

## 1. O que acontece hoje (CONFIRMADO pelo código, `link_rx.c`, ramo NORMAL do playout)

1. **Falta o bloco esperado:** o RX escreve outra vez o `last_block` a amplitude total (`out_block = last_block`).
   Numa falta contínua isto repete-se a cada 2,66 ms, o que dá um tom de 375,6 Hz com harmónicos. O timbre depende do
   conteúdo do bloco repetido, daí um "beep" sempre diferente.
2. **A repetição só acaba quando dispara o rebuffer LOW:** a EMA rápida da ocupação (τ = 300 ms) tem de descer de
   ~4,5 blocos (alvo de 576 tramas) para 0,5 bloco (`LOW_WATER` = 64 tramas). Isso leva τ·ln(9) ≈ **0,66 s**, o que
   bate com as rajadas medidas (0,56–0,76 s).
3. **Entrada no rebuffer:** passa de imediato do bloco repetido, a amplitude total, para silêncio. É um **degrau**.
4. **Ressincronização:** o primeiro bloco real começa do silêncio sem rampa. Outro **degrau**.
5. **Perda isolada de um bloco** (o `late_drop` comum, ~0,2–0,5/s): uma repetição a amplitude total, com
   descontinuidades nas duas fronteiras do bloco. Ouve-se como um tique curto.

## 2. Alternativas

| # | Alternativa | Avaliação |
|---|---|---|
| A | Repetir 1–2 blocos e cortar para silêncio | Acaba com o tom, mas mantém os degraus no corte e no regresso: troca o beep por clicks |
| B | Encurtar o gatilho do rebuffer (τ ou `LOW_WATER`) | Mexe na dinâmica de jitter/rebuffer, já validada. Mais rebuffers falsos. Reduz o beep mas não o elimina |
| C | PLC "a sério" (pitch/overlap-add, extrapolação) | Melhor qualidade em perdas curtas, mas é complexa e cara em CPU no caminho de tempo real. Fora de âmbito |
| **D** | **Ganho de ocultação com rampas: repetir em fade-out até silêncio em K blocos; fade-in no regresso** | **Recomendada.** Pequena, determinista, sem degraus. Em operação normal não toca em nenhuma amostra |

## 3. A proposta D

- **Estado novo:** um único ganho `g`, de 0 a 1, que só o playout usa. Cada bloco escrito leva uma rampa contínua de
  `g_inicial` para `g_final`, em cosseno elevado com uma tabela de 128 valores, e `g` nunca salta entre blocos.
  - **Bloco real com `g` = 1:** passa **intacto**, sem multiplicação. Em operação normal as amostras ficam
    bit-exatas, igual a hoje.
  - **Ocultação** (falta o bloco): repete o `last_block` numa cópia, com a rampa `g → max(0, g − 1/K)`. Com **K = 2**,
    chega a silêncio em 2 blocos (5,3 ms) e depois escreve zeros.
  - **Bloco real depois de ocultação:** rampa `g → 1` dentro desse bloco (2,66 ms), sem degrau no regresso.
  - **Rebuffer (entrada e saída):** quando o LOW dispara (~0,66 s depois), `g` já está em 0 e não há degrau. Na
    ressincronização, o primeiro bloco faz fade-in a partir de 0. O mesmo vale para o arranque e para a mudança de
    sessão.
- **Perda isolada de um bloco:** uma repetição em fade 1→0,5, seguida do bloco real em fade 0,5→1. Fica um ligeiro
  "afundamento" de ~5 ms em vez de um tique com descontinuidades.
- **Não muda:**
  - a lógica de decisão (`expected_ctr`, `late_drop`, `never_recv`, slip, gatilhos e duração do rebuffer, filas,
    DMA);
  - o protocolo, o TX e a taxa;
  - os contadores, que contam exatamente o mesmo.
  - Só muda o conteúdo das amostras nos blocos ocultados e no bloco a seguir.
- **Custo:** 256 multiplicações por bloco ocultado ou em rampa, nada nos blocos normais. Os blocos com slip de ±1
  trama aplicam a rampa antes do slip.
- **Escolha de K:**
  - K = 1 corta mais depressa, mas a rampa é mais abrupta;
  - K = 2 é um bom compromisso;
  - K maior repete mais (volta a ser um tom curto).
  - Fica como Kconfig, `AUDIOLINK_LINK_CONCEAL_FADE_BLOCKS`, com omissão 2. Com 0 desliga a funcionalidade e o
    comportamento é o atual: é o rollback em compilação.

**Resultado esperado:**
- Uma falta de 0,5–0,8 s passa de "beep de 375 Hz + click" para "**fade de ~5 ms, silêncio, fade-in de ~3 ms**".
- As perdas isoladas ficam menos audíveis.
- Os episódios de RF não desaparecem: ouvem-se como cortes curtos e suaves, não como tons.

## 4. Validação mínima (sem campanha)

1. **Host:** a lógica do ganho e da rampa numa função pura (como o `link_plaus.h`), com teste e mutantes:
   - `g` contínuo entre blocos;
   - bloco normal com `g` = 1 bit-exato;
   - perda isolada, falta longa (zeros depois de K blocos), regresso com fade-in;
   - perda durante o fade-in.
2. **Offline:** aplicar o playout antigo e o novo a um WAV de música com o padrão de perdas observado (uma falta de
   0,66 s mais perdas isoladas). Medir:
   - a energia nos harmónicos de 375,6 Hz, a métrica da gravação do iPhone;
   - o maior salto entre amostras nas transições.
   - Esperado: o tom desaparece e não aparecem saltos novos.
3. **Auditoria de objetos:** só mudam o ciclo de playout e a função nova. Com K = 0 o binário fica igual ao R1, salvo
   a data.
4. **Uma corrida de bancada (~10 min)** com o gancho de surdez do R2, já existente, numa build de teste no **RX
   PCM5102A** (gravável pela entrada analógica da KA6, caminho já validado). Gravar antes/depois (R1 contra fade) e
   aplicar as mesmas duas métricas.
5. **Audição na sala** com a build de produção.

**Rollback:** gravar outra vez o R1 `44e6ac897` (commit `e826ec2`), ou K = 0.

## 5. Revisão de 30/10 (depois do JB40): âmbito final e o esvaziamento no resync

### 5.1 O esvaziamento da fila no resync é um defeito, não uma decisão (CONFIRMADO)

A intenção está escrita em três sítios:
- o comentário do ramo REBUFFERING: "expected_ctr novo é o 1.º frame do bloco mais antigo dessa sequência", ou seja,
  *tocar* a sequência contígua;
- a ajuda de `AUDIOLINK_LINK_DRIFT_TARGET_FRAMES`: "nesse instante a ocupação REAL é sempre BUFFER_BLOCKS";
- a contagem `discarded_total = ... + (items_seen − BUFFER_BLOCKS)`, que assume que os BUFFER blocos ficam na fila.

O código faz outra coisa. Para verificar a contiguidade usa `xQueueReceive`, e uma fila FreeRTOS só deixa espreitar
o primeiro elemento, por isso os pacotes da sequência são **retirados e perdidos**.

**Evidência:**
- `occ_depois` = 0–2 em 15 rebuffers registados (W2/R2);
- no JB40, exatamente 15 ocultações logo a seguir ao resync do arranque.

**Consequência hoje:** depois de cada resync, BUFFER blocos repetem o último bloco antigo: ~11 ms no R1, ~40 ms no
JB40. O atraso alvo fica certo (`expected_ctr` está correto); perde-se apenas o conteúdo desses blocos.

**Decisão proposta:** não repor a sequência.
- Para a tocar seria preciso um buffer de reprodução de BUFFER×1060 B e um segundo caminho de leitura no playout, só
  para recuperar 40 ms de áudio logo a seguir a uma falha.
- Com o ganho de ocultação (5.2), esses BUFFER blocos saem **em silêncio**, o que é inofensivo depois de um rebuffer.
- Na mesma alteração corrigem-se o comentário, a ajuda do Kconfig e a contagem `discarded`, que passa a incluir os
  BUFFER blocos, para a documentação deixar de afirmar o contrário do código.

### 5.2 A alteração mínima: um só ganho, um só caminho para "não há bloco real"

O que muda face ao §3:
- Todos os ticks sem bloco real passam por **uma única função de saída**: a ocultação em NORMAL e **também os ticks
  de REBUFFERING**, que hoje escrevem `silence` diretamente.
- Assim o mesmo mecanismo cobre todos os casos sem regras especiais.

O mecanismo:
- **Estado:** `g` em Q15 (0..32768), só da tarefa de playout. A rampa é **linear dentro do bloco**, em inteiros:
  sem tabela e sem vírgula flutuante.
- **Tick sem bloco real:**
  - se `g` > 0, escreve `last_block` com a rampa `g → max(0, g − 1/K)`;
  - se `g` = 0, escreve silêncio.
  - Com K = 2, a repetição dura no máximo **2 blocos (5,3 ms)**, sempre a descer.
- **Tick com bloco real:**
  - se `g` < 1, rampa `g → 1` nesse bloco (2,66 ms);
  - se `g` = 1, o bloco passa **intacto e bit-exato**, sem multiplicação: em operação normal não muda uma única
    amostra.
- O slip de ±1 trama aplica-se depois, como hoje.

| Situação | Hoje | Com a correção |
|---|---|---|
| Perda isolada | 1 repetição a amplitude total, com descontinuidades | Repetição 1→0,5 e regresso 0,5→1 |
| Falha longa antes do LOW (0,6 s no R1, 1,0 s no JB40) | Beep de 375,6 Hz durante 0,6–1,0 s | 5,3 ms de fade-out, depois silêncio |
| Entrada no rebuffer por LOW | Degrau da repetição para o silêncio | `g` já está em 0, sem degrau |
| Entrada no rebuffer por HIGH (com áudio real) | Degrau do áudio para o silêncio | Fade-out em 2 blocos |
| BUFFER blocos depois do resync (5.1) | 11–40 ms de repetição antiga | Silêncio (`g` = 0) |
| Primeiro bloco real depois do resync, de uma sessão nova do TX ou do arranque | Degrau a partir do silêncio | Fade-in em 2,66 ms |

**Não muda:**
- `expected_ctr`, `late_drop`, `never_recv`, o slip, os gatilhos e a duração do rebuffer, as filas, o DMA, o TX e o
  protocolo;
- todos os contadores, que contam o mesmo.

**Não é PLC:** não se estima conteúdo novo. Repete-se no máximo 5 ms do que já existia, a descer, e depois é silêncio.

**Rollback:**
- em compilação: `AUDIOLINK_LINK_CONCEAL_FADE_BLOCKS` = 0. A auditoria tem de mostrar a app igual ao JB40, exceto a
  versão;
- em gravação: a app do JB40 (`f7ab772e3`) ou a do R1.

**Validação mínima:**
1. **Teste no host:** a função pura com mutantes.
   - `g` contínuo entre blocos;
   - `g` = 1 é bit-exato;
   - silêncio ao fim de K blocos;
   - fade-in;
   - perda durante o fade-in.
2. **Auditoria de objetos:** só mudam o playout e a função nova; com K = 0, igual ao JB40.
3. **Bancada, ~5 min,** com uma build de teste que usa o gancho de surdez já existente (`AUDIOLINK_TEST_RX_DEAF`, 3 s
   de surdez) para forçar falhas longas, um rebuffer e um resync.
   - Confirmar pelos contadores que o comportamento de decisão é idêntico.
   - Confirmar pelo ouvido, no SU-2, que o beep desaparece.
4. **Audição na sala.**

## 6. Decisão final e implementação (C1, 30/10)

**O que mudou face a §5:**
1. **§5.2 tinha um click em cada ocultação.**
   - A repetição para a frente começa em `x[0]` logo a seguir a `x[127]`, com ganho ~1.
   - Offline, com música real: pior transição +18,9 dB de energia de alta frequência face ao original, contra +22,8 dB
     hoje e **+1,4 dB** com a solução escolhida.
2. **§5.1, decisão invertida: a sequência do resync passa a ser tocada.**
   - Custo real: um anel de 15 itens em PSRAM (15,9 KB) e ~40 linhas numa função pura testada (`link_resync.h`), não o
     "segundo caminho de leitura" que eu tinha estimado. Os itens voltam à frente da fila FreeRTOS com
     `xQueueSendToFront`, e o playout normal não muda.
   - Ganha-se:
     - nenhuma ocultação falsa;
     - nenhum gate RF falso;
     - a ocupação depois do resync é verdadeira (a reposição da EMA passa a estar certa);
     - menos 40 ms de silêncio por resync.
3. **Problema novo encontrado na auditoria: cascatas LOW → HIGH.**
   - O resync ancorava no pacote mais antigo da rajada que o TX descarrega depois de uma falha longa, e a fila
     transbordava.
   - Agora o resync usa os 15 **mais recentes** e só acontece num período com chegadas normais.

**Solução escolhida (`link_conceal.h`):** sem bloco real depois de som, o último bloco **ao contrário**, com ganho
(N−1)/N → 0.
- Começa na última amostra escrita: sem degrau.
- Não se repete: sem tom.
- Depois disso, silêncio.
- O 1.º bloco real seguinte entra com fade-in de 1 bloco.
- Os blocos normais saem bit-exatos.
- Um só caminho para todo o período sem bloco: ocultação, rebuffer, sessão nova e arranque.
- Não é PLC.

**Alternativas consideradas e não escolhidas:**
- **Crossfade no regresso** (sem o mergulho de ~5 ms nas perdas isoladas): mais estado. Com o JB40 as perdas
  isoladas quase desapareceram.
- **Encurtar o gatilho LOW:** com o fade, a falha já é silêncio. Ficar em NORMAL mantém a latência e recupera sem
  resync.
- **Buffer > 40 ms:** a cauda que sobra são episódios de centenas de ms.

**Evidência, riscos, validação e rollback:** `experiments/C1/PLAN.md`.

