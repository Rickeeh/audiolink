# AudioLink Core v1 — requisitos congelados (2026-10-08)

Congelados pelo utilizador em 2026-10-08. **Não se ajustam aos resultados seguintes.**
- Qualquer alteração exige uma decisão explícita do utilizador, registada aqui com data.
- Um resultado que falhe estes números é FAIL, não motivo para mudar os números.

## R1 — Latência analógico → analógico
- **p95 ≤ 30 ms**, **máximo ≤ 35 ms**, com **n ≥ 20** medições.
- **Objetivo de desenho:** 20–25 ms.

## R2 — Regime estacionário (≥ 2 h)
- **0** rebuffers, **0** underruns, **0** overflows.
- `late_drop` / ocultações: **média ≤ 0,05/s**.
- **p95 das janelas de 5 min ≤ 0,15/s**.
- `never_recv` **≤ 0,03/s**.

## Operacionalização proposta (por confirmar pelo utilizador antes da primeira avaliação; não muda os números)
- **R1:**
  - método da medição de 2026-09-30: impulso pela KA6 com loopback de referência e correlação cruzada
    (`experiments/_ARCHIVE_SCRATCHPAD_2026-10-07/reconstructed/latency_measure.py`, reconstruído);
  - n ≥ 20 em regime estacionário;
  - registo simultâneo do estado do RX.
- **R2:**
  - a janela começa depois do arranque, pelo que o rebuffer STARTUP fica fora;
  - Δ(`rebuffer_low` + `rebuffer_high` + `rebuffer_session`) = 0;
  - Δ`dac_underrun` = 0;
  - Δ`queue_ovf` (RX) = 0 e Δ`adc_ovf` (TX) = 0;
  - "late_drop/concealment ≤ 0,05/s" aplica-se a `late_drop` e a `playout_concealment`, cada um;
  - p95 sobre janelas fixas de 5 min;
  - `never_recv` como média da janela.

## Situação atual face aos requisitos (dados existentes; nada ajustado)

| | Valor medido | Requisito | Situação |
|---|---|---|---|
| R1, latência (39,17 ms, n = 20, 2026-09-30, 10–45 s depois do arranque) | máximo 39,31 ms | máximo ≤ 35 ms | **FALHA** |
| R1 em regime (estimativa pelo modelo, não medido) | ~31–34 ms | p95 ≤ 30 ms | provável FALHA |
| R2, `late_drop` médio: LD1 (3 h) | 0,249/s | ≤ 0,05/s | **FALHA** |
| R2, `late_drop` médio: T704 formal (90 min; probe, não é Core) | 0,097/s | ≤ 0,05/s | **FALHA** |
| R2, `late_drop` médio: endurance RC2 (12 h) | 1,41/s | ≤ 0,05/s | **FALHA** |

O sistema atual não cumpre R1 nem R2. O objetivo de desenho de 20–25 ms obriga a recuperar a maior parte da latência
morta do anel DMA do RX (`RX_PLAYOUT_PROPOSAL.md`, secções 1–2).
