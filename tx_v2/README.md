# AudioLink TX v2

> O código publicado já inclui aquisição PCM1808, PCM24/ALK v3 e LED de estado.
> A descrição abaixo documenta a etapa histórica E1; consultar o README da raiz para o build atual.

TX novo e mínimo, construído a partir do BEN da Fase 1 (o ancestral conhecido-bom), segundo `docs/ARCH_V2_PROPOSAL.md`
(aprovada em 2026-10-09). **Não é um refactor do `audio/main/link_tx.c`.**

**Estado:** etapa **E1** — BEN → AudioLink mínimo, sem PCM1808.
- Build e testes de host feitos.
- **Gate físico por executar.**

## Fluxo

```
[E1] temporizador (ritmo nominal exato) → bloco de 128 tramas (tom digital) → cabeçalho ALK v2 → esp_now_send → callback mínima
```

## Módulos (`main/`)

| Ficheiro | Responsabilidade | Origem |
|---|---|---|
| `tx_main.c` | ciclo de vida: NVS, cria `tx_audio` (core 1, prio 20) e `tx_diag` (core 0, prio 1) | BEN (`bench_main.c`: `app_main`, `tx_role_main`) |
| `tx_audio.c` | tarefa de tempo real: ritmo → bloco → cabeçalho → envio | BEN (`run_step`, `sleep_until`) |
| `tx_pace.h` | ritmo nominal exato, 2662,4 µs (só E1) | novo |
| `espnow_tx.c` | Wi-Fi + ESP-NOW (unicast, canal 6, HT20, MCS4 LGI, PS desligado); envio sem bloqueio | BEN (`transport_espnow.c`) |
| `tx_ledger.c` | contabilidade do envio: em voo, limite 4, recusas, latência envio→callback; sem fila | novo, testado no host |
| `diag.c` | snapshot da tarefa RT e uma linha `LINKTX` por segundo, fora do tempo real | novo |
| `../components/alk_proto` | formato ALK no fio, partilhado com o RX | layout do v2 (`audio/main/link_proto.h`) |

## Build (não grava)
```
tx_v2/build.sh <dir_de_build> [MAC_do_RX]
tx_v2/stage_artifacts.sh <etapa> <dir_de_build>
tests_host/run.sh
```

## Etapas

| Etapa | Commit | ELF | Gate físico |
|---|---|---|---|
| E1 | este | `artifacts/E1/ELF_PREFIX` | por executar |
