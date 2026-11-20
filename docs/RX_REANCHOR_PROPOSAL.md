# RX: bloqueio permanente por `implausible_ctr` depois de uma falha de rádio > 2 s — proposta de correção (2026-10-29)

**Estado:** proposta para decisão. **Nada foi implementado nem gravado.**

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

**Origem:** W2 do RX WM8805 (`experiments/W2-WM8805/RESULT_W2.md`, na worktree `audiolink-wm8805-w1`). Incidente
marcado no recorder do W2 às 23:11:26 UTC.

---

## 1. O defeito (CONFIRMADO)

`audio/main/link_rx.c`, `link_rx_on_packet`, limite de plausibilidade (2026-09-30):

```c
if (s_have_max_ctr) {
    int64_t jump = (int64_t)hdr->sample_ctr - (int64_t)s_max_received_ctr;
    int64_t max_jump_frames = (int64_t)CONFIG_AUDIOLINK_LINK_RX_MAX_JUMP_S * (int64_t)s_fs_hz;   // 2 s
    if (jump > max_jump_frames) { s_corrupt_jump++; return; }
}
```

- `s_max_received_ctr` só sobe com pacotes **aceites**.
- O `sample_ctr` do TX avança com o tempo real, haja ou não receção.
- Se o RX não aceitar nada durante mais de 2 s com o TX a correr, o primeiro pacote que volta já está mais de 2 s à
  frente e é rejeitado. Os seguintes também.
- A referência nunca mais se move. **O RX fica mudo para sempre.** Só sai do estado com uma sessão nova (reinício do
  TX) ou com o reinício do RX.

**Evidência do W2:**
- 23:11:22: `rx_peer_lost`. 23:11:26: os pacotes voltam, todos `implausible_ctr` (+368, +376, …/s).
- `max_received_ctr` congelado em 78 285 184 e `received_packets` em 610 611.
- `implausible_ctr` chegou a 37 720 em 15 min. O RX continuava a medir RSSI e a enviar ACK, ou seja, ouvia os
  pacotes mas rejeitava-os.
- O desligar/ligar do TX (sessão nova) desbloqueou-o: O1–O3 PASS.

**Âmbito:**
- O mesmo código está no **RX PCM5102A em produção** (`86b2f8aaf`, Beta 1 + LED).
- Não depende do WM8805.
- Nunca apareceu em 25 h de Beta 1 porque nunca houve uma falha de rádio > 2 s com o TX a correr. Na sala, a uma
  distância maior, é PROVÁVEL que aconteça.

## 2. Por que o resto do RX já sabe recuperar (CONFIRMADO pelo código)

- Numa falta de pacotes, a fila esvazia e o playout entra em **REBUFFERING** (motivo LOW), com silêncio contínuo e o
  I²S sempre a correr.
- Em REBUFFERING, o RX ressincroniza assim que vê `BUFFER_BLOCKS` (4) pacotes com `sample_ctr` contíguos,
  **independentemente do salto**: `expected_ctr = rebuf_run_start`.
- O `never_recv` conta como perda definitiva os blocos que nunca chegaram (`max_seen − first − unique`). Uma falha
  de 4 s soma ~1500, que é a semântica correta.
- O único obstáculo é o filtro de plausibilidade, que impede os pacotes de chegarem à fila. **Corrigir só o filtro
  chega.**

## 3. Alternativas

| # | Alternativa | Avaliação |
|---|---|---|
| A | Subir `MAX_JUMP_S` (ex.: 60 s) | Rejeitada: só muda o limiar. Uma falha de 61 s volta a bloquear para sempre |
| **B** | **Limite proporcional ao tempo decorrido:** salto plausível = `MAX_JUMP_S·fs` + `fs·(agora − hora em que o máximo foi aceite)` | **Recomendada.** O contador do TX avança com o tempo real, por isso um fluxo legítimo **nunca** excede este limite, seja qual for a duração da falha (provas em §4). Um valor envenenado (o caso de 30/09: ~562 320 000, horas à frente) continua rejeitado, salvo se a falha tiver durado essas horas. Não há estado novo: usa `s_max_received_at_us`, que já existe e já é escrito no mesmo sítio |
| C | Re-ancoragem por consistência: K pacotes rejeitados seguidos e coerentes entre si passam a ser a nova base | Funciona, mas acrescenta estado (candidato, contagem) e um parâmetro K a validar. Mais código no callback do Wi-Fi para o mesmo efeito de B |
| D | Remover o filtro | Rejeitada: reabre o envenenamento de `s_max_received_ctr` que motivou o filtro |

## 4. A correção recomendada (B)

```c
// limite = margem fixa + o que o TX avancou em tempo real desde que o maximo atual foi aceite
int64_t elapsed_us = now - s_max_received_at_us;               // >= 0: ambos de esp_timer no mesmo core
int64_t max_jump_frames = (int64_t)CONFIG_AUDIOLINK_LINK_RX_MAX_JUMP_S * fs + elapsed_us * fs / 1000000;
```

**Por que um fluxo legítimo nunca é rejeitado (FORTEMENTE SUPORTADO, a provar em teste de host):**
- `ctr_k − ctr_max ≈ fs_tx·(t_k − t_max) + atraso_no_TX`.
- A diferença entre os relógios TX/RX é de ppm: 0,58 ppm no par do W2, 3,19 no da Beta 1. Numa falha de 1 h,
  isso dá ~10–40 ms.
- Com `d` = atraso entre a captura no TX e a chegada ao RX, `salto − fs·decorrido = fs·(d_referência − d_atual)`.
  O salto só pode exceder o tempo decorrido se o pacote de referência tiver chegado **mais atrasado** do que o
  atual, e no máximo por `fs·d_referência`.
- O pior atraso medido no TX foi a latência de callback de 1,2 s na congestão do W2; a fila de envio vale até 32
  pacotes ≈ 85 ms. **A margem fixa de 2 s cobre esse pior caso** (folga de ~0,8 s).
- **Risco residual:** se o pacote de referência tiver chegado com mais de 2 s de atraso **a mais** do que os
  seguintes, estes são todos rejeitados e a referência não muda. Seria o mesmo bloqueio, por outro caminho.
  - Exige que o atraso caia mais de 2 s entre dois pacotes aceites consecutivos.
  - Com a entrega do ESP-NOW em ordem (FIFO) e a fila do TX limitada a 32 pacotes, o atraso evolui gradualmente.
    Por isso é PROVÁVEL que não aconteça, mas não está provado.
- **Defesa proposta, mínima:** contar no snapshot o maior número de rejeições seguidas (`implausible_run_max`). Se
  alguma vez passar de ~375, que é 1 s de pacotes, a condição tornou-se visível no recorder em vez de silenciosa.
- O R0 inclui a fronteira: referência com 1,9 s de atraso a mais → aceite; com 2,5 s → rejeitada, o caso residual,
  documentado como limite conhecido.

**O que continua protegido:**
- Um valor isolado muito à frente do real continua a ser rejeitado.
- Um valor envenenado que caia dentro da janela permitida empurra `s_max_received_ctr` para a frente. Os pacotes reais
  seguintes ficam **atrás** do máximo e são aceites, porque o filtro só rejeita saltos para a frente. O máximo serve
  só de diagnóstico e de referência para este filtro; o controlador usa a ocupação real. **Não volta a haver bloqueio.**

**Mudança de código:** uma expressão em `link_rx_on_packet`, extraída para uma função pura com teste de host
(`link_plaus.h`, como o `dac_signal.h` do W1). Em operação normal (sem falhas > 2 s), as decisões são iguais.

## 5. Validação proposta (incremental, um fator de cada vez)

| Passo | O quê | Critério |
|---|---|---|
| R0 | Função pura + testes de host + mutantes | Normal (+128) aceite; jitter aceite; falhas de 4 s, 60 s e 1 h aceites; ±100 ppm ao fim de 1 h aceite; 562 320 000 ao fim de 1 s rejeitado; +3 h ao fim de 1 s rejeitado; salto para trás nunca rejeitado; sem overflow |
| R1 | Auditoria de objetos | Face ao RX do W2 (`7b00821a7`) e ao Beta 1 + LED: só `link_rx_on_packet` muda |
| R2 | **Reprodução causal na bancada, com injeção de falha.** Build **só de teste**, com um Kconfig que faz o RX ignorar todos os pacotes em janelas programadas: 4 s aos 60 s de uptime, 30 s aos 120 s, 300 s aos 240 s. É o equivalente aplicacional de uma falha de rádio com o TX a correr. **A** = código atual + injeção; **B** = correção + injeção | **A reproduz o bloqueio** depois da 1.ª janela: `implausible_ctr` sobe e `received_packets` fica congelado ≥ 60 s. Isto valida o teste e o mecanismo. **B** recupera depois de cada janela: rebuffer +1, ≥ 370 recv/s em ≤ 5 s, `implausible_ctr` 0, `dac_underrun` 0, sem reinícios |
| R3 | Build de produção do RX com a correção (sem injeção) | Smoke de 15 min S1–S5 na bancada |
| R4 | Decisão do utilizador: aplicar também ao RX PCM5102A em produção (Beta 1.1), que tem o mesmo defeito | — |

**Notas sobre o R2:**
- A injeção não reproduz a física de uma falha de rádio: o MAC continua a fazer ACK e o TX não congestiona. Mas
  reproduz **exatamente** a condição do bloqueio (nenhum pacote processado durante mais de 2 s com o TX a correr),
  de forma determinista e sem mexer nas placas.
- A falha física fica para validação oportunista na sala.
- As builds de teste nunca são de produção. A de produção não tem a injeção (`#if`), e o R1 audita isso.

**Rollback:** RX de volta ao `7b00821a7` (W2) ou ao `86b2f8aaf` (Beta 1 + LED), só a app.

## 6. Achado secundário, fora desta correção (CONFIRMADO pelo código)

- O bitmap de duplicados é indexado pelo índice **absoluto** do bloco (`sample_ctr / frames`), com capacidade de
  200 000 blocos (~532 s desde o arranque do TX).
- Daí em diante, `bitmap_test_and_set` devolve sempre "novo": a deteção de duplicados ao nível da aplicação está
  **desligada em todo o uso real da Beta 1** (uptimes de dias).
- **Impacto: PROVÁVEL que seja pequeno.**
  - O 802.11 já descarta retransmissões duplicadas ao nível MAC.
  - Um duplicado que passe é descartado como atrasado (`sample_ctr < expected`) depois de o original ser tocado.
  - Pode inflacionar a ocupação, `late_drop` e `queue_ovf`.
- **Proposta:** não misturar com esta correção (um fator de cada vez). Fica registado para o RX v2: uma janela
  deslizante relativa a `expected_ctr`.

---

## 7. R0–R1 executados (2026-10-29), nada gravado

**Código** (worktree `audiolink-wm8805-w1`, não commitado):
- `audio/main/link_plaus.h`: função pura `link_ctr_implausible()`.
- `link_rx.c`: chama a função e acrescenta o contador `implausible_run_max` ao `LINKRX`.
- `tests_host/test_link_plaus.c`.

**Correção feita à proposta durante o R0:** o teste apanhou uma lacuna do limite da §4. Com o TX 100 ppm mais rápido
e uma falha de 24 h, o TX avança 8,6 s a mais do que o tempo medido pelo RX, acima da margem de 2 s: seria de novo
um bloqueio. O tempo decorrido passou a levar **+1000 ppm de tolerância** (`elapsed + elapsed/1000`). Cobre qualquer
cristal; o valor envenenado de 30/09 e um salto de +3 h em 1 s continuam rejeitados.

**R0 (testes de host):** `test_link_plaus` com 569 verificações e 0 falhas; restantes testes 6/6 PASS. Casos cobertos:
- fluxo normal durante 1 h; falhas de 1 s a 24 h a 0, ±100 e +900 ppm; referência atrasada 85 ms;
- regressão do W2 (regra antiga bloqueia, nova aceita);
- envenenamentos rejeitados; saltos para trás nunca rejeitados;
- fronteira exata (= aceite, +1 rejeitado); fronteira do atraso residual (1,9 s aceite, 2,5 s rejeitado);
- ausência de overflow a 1 ano.

Mutantes, todos mortos:

| Mutante | Falhas |
|---|---|
| Regra antiga | 35 |
| Sem tolerância de ppm | 5 |
| `>=` em vez de `>` | 1 |
| Sem margem fixa | 527 |
| µs tratados como ms | 3 |

**R1 (auditoria de objetos), por símbolo, igual nos dois perfis:**

| Build | Face a | Diferenças |
|---|---|---|
| RX WM8805 `44e6ac897` | W2 `7b00821a7` | `link_rx_on_packet` +136 B, `link_rx_run` +12 B, `rx_diag_task` −12 B, 2 variáveis novas (`s_corrupt_jump_run`, `s_corrupt_jump_run_max`). Nada mais |
| RX PCM5102A `7a509f157` | Beta 1 + LED (`d29d57c` = `86b2f8aaf`) | As mesmas |

- O campo novo do snapshot foi posto **no fim** da estrutura, onde ocupa o enchimento que já existia. O snapshot não
  muda de tamanho e os campos que o playout escreve não se deslocam. Na 1.ª tentativa, a meio da estrutura,
  `link_rx_run` crescia 48 B.
- Na desmontagem de `link_rx_run` aparecem a cópia do contador (`l32i`/`memw`/`s32i`) e duas trocas de codificação
  vizinhas (`l32i`→`l32i.n`, `movi`→`movi.n`). O resto é ruído do desmontador nas *literal pools*.

**Artefactos:** `release/R1-REANCHOR/RX_WM8805/` e `RX_PCM5102A/` (SHA256SUMS), não gravados.

**Próximo:** R2, a reprodução causal com injeção de falha: build de teste A (regra antiga) contra B (correção). Exige
preparar o Kconfig de teste e autorização para gravar o RX da bancada.

## 8. R2 preparado (2026-10-29), não executado

`experiments/R2-REANCHOR/PLAN.md` (worktree).
- **Builds de teste com surdez programada:** A (regra antiga) `639b46a5b`, B (correção) `7a70aad96`.
- **Ganchos inertes em produção:** o R1 recompilado a partir da fonte final é idêntico ao auditado.
- **Avaliador validado com os eventos reais do W2:** bloqueio, período normal e recuperação.
- **Sequência:** A (tem de reproduzir o bloqueio) → B (tem de recuperar nas 3 janelas) → R1 → R3 smoke. Cada
  gravação com autorização.
