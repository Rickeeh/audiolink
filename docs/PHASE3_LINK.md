# AUDIOLINK — Fase 3: ESP-NOW básico com áudio real

**Objetivo desta fase:** `PCM1808 → TX S3 → ESP-NOW → RX S3 → PCM5102A` a funcionar, com o mínimo de
instrumentação para depois medir perdas/desordem/drift/underruns. **Sem FEC, sem PLC, sem jitter buffer
sofisticado, sem correção de drift** — isso é dos passos 6/5. Aqui só se observa o comportamento natural.

## Formato do pacote (`audio/main/link_proto.h`)

```c
#define LINK_PROTO_MAGIC   0x414C4B31u  // "ALK1"
#define LINK_PROTO_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;        // LINK_PROTO_MAGIC — deteta lixo/outro protocolo no mesmo canal
    uint8_t  version;      // LINK_PROTO_VERSION — mudar o layout = subir a versão
    uint8_t  flags;        // reservado, 0 por agora
    uint16_t frames;       // n.º de frames estéreo neste pacote (pacote auto-descritivo)
    uint32_t seq;          // pacotes enviados desde o arranque (monotónico; perdas/desordem no transporte)
    uint64_t sample_ctr;   // índice do 1º frame estéreo deste pacote, cumulativo desde o arranque do I2S do TX
                           // (domínio do relógio do ADC; é isto, não o seq, que deteta perdas/desordem do
                           // áudio e serve depois para medir o drift entre os dois relógios independentes;
                           // uint64 para não dar a volta — um uint32 dava a volta às ~24,8 h a 48 kHz)
} link_pkt_hdr_t;         // 20 bytes, tamanho fixo validado por _Static_assert
```

Payload: `frames × 8 bytes` (L,R int32), **sem conversão** — é o mesmo layout que `i2s_channel_read()` entrega
e `i2s_channel_write()` aceita (24 bit alinhados à esquerda em 32, como em `adc_test.c`/`dac_test.c`). O TX
copia o buffer do ADC direto para o payload; o RX copia o payload direto para o buffer do DAC.

| # | Pergunta | Resposta |
|---|---|---|
| 1 | Formato do frame | Ver struct acima; cabeçalho 12 bytes + payload em bruto |
| 2 | Amostras estéreo por pacote | `AUDIOLINK_LINK_FRAMES_PER_PKT`, omissão **128** frames |
| 3 | Tamanho total do pacote | 20 + 128×8 = **1044 bytes** (dentro do limite ESP-NOW v2, 1470 B) |
| 4 | Pacotes por segundo | fs/128 ≈ 48077/128 ≈ **375,6 pkt/s** |
| 5 | Atraso mínimo só do framing | 128/fs ≈ **2,66 ms** para encher um pacote no TX; a isto somam-se tempo de transmissão no ar + reprodução no RX — sem estimar já a latência total, mede-se no passo 5 |
| 6 | RX com pacote em falta | **Repete o último bloco recebido**, um bloco de cada vez (não é PLC: cópia direta, sem crossfade/interpolação); ver `link_rx.c` |

## Decisão de PHY/taxa — sinalizada explicitamente, não escondida no código

Áudio em bruto (32 bit/amostra, sem compactação) a ~48077 Hz estéreo exige **~3,1 Mbps contínuos** só de
payload. Isto é **mais do que os 1,92 Mbps que a Fase 1 validou como ponto robusto** (11g 6 Mbps, testado
com corpo/paredes). A Fase 1 mediu MCS4 LGI a 4,7 Mbps com 0,03% de perdas em condições boas — margem sobre
os ~3,1 Mbps — mas também mostrou degradação com obstrução (`docs/PHASE1_RESULTS.md`, run011: "MCS4 no
limite, 1 apagão de 0,44 s").

**Escolha para este passo 3 (`AUDIOLINK_LINK_RATE_MCS4_LGI`, HT20): provisória**, só para observar o
comportamento com o hardware ligado em bancada. O compromisso taxa/robustez com tráfego de **áudio real**
(não os pacotes sintéticos do bench) fica para o passo 5 — pode implicar aceitar uma taxa mais alta e menos
robusta do que a recomendação final de produto, ou compactar o áudio (24 bit em vez de 32, por exemplo) se
a medição mostrar que é preciso. Não se decide isso agora.

## Contadores (impressos 1×/segundo)

- **TX** (`LINKTX`): `seq`, `sample_ctr`, `sent` (pacotes/s), `send_ok`/`send_fail` (callback ESP-NOW),
  `api_err` (falha na própria chamada), `adc_ovf` (overrun do I²S no ADC — não conseguimos ler a tempo).
- **RX** (`LINKRX`): `recv` (pacotes ESP-NOW recebidos), `ok` (escritos normalmente), `lost_blocks`
  (buracos detetados por `sample_ctr`), `repeated` (blocos repetidos, = `lost_blocks` + `timeouts`),
  `timeouts` (nenhum pacote chegou a tempo — `AUDIOLINK_LINK_RX_WAIT_MS`), `ooo` (atrasado/duplicado,
  largado), `corrupt` (pacote malformado), `queue_ovf` (fila TX↔RX interna cheia — RX não decodificou a
  tempo), `dac_underrun` (evento `on_send_q_ovf` do I²S — a DMA do DAC ficou sem dados).

## Como construir e emparelhar

```bash
# RX primeiro, para ler o MAC no log de arranque ("link_espnow: MAC STA deste no: ...")
tools/build_audio.sh LINK_RX INT160_13
# flash: <comando impresso pelo script, na porta do RX>

# TX, com o MAC do RX lido acima
tools/build_audio.sh LINK_TX INT160_13 AA:BB:CC:DD:EE:FF
# flash: <comando impresso pelo script, na porta do TX>
```

## Diagnóstico de fluxo (2026-09-29/30) e buffer mínimo de 4 blocos

Depois do baseline inicial mostrar `queue_ovf` de 82% (RX) e `api_err` de 24% (TX), dois bugs de
fluxo foram corrigidos (backpressure explícito no TX via fila de pendentes + código exato do
erro; RX deixou de tentar recuperar retroativamente um buraco grande, passando a avançar sempre
exatamente 1 bloco/período). Isso resolveu `queue_ovf`/`api_err`, mas expôs `late`/`reorder`
muito variáveis entre sessões (de 0% a 87% de perda), sem relação com o RSSI (sempre excelente,
−20 a −26 dBm). Uma parte dessa variabilidade era **artefacto do próprio método de teste**:
reabrir a porta série do RX entre corridas por vezes reiniciava a placa sem avisar (confirmado
comparando `uptime_s`, que devia crescer monotonicamente). Corrigido mantendo as portas série
abertas uma única vez durante toda a experiência.

Com o método corrigido, uma corrida contínua de ~185 s sem buffer mostrou um regime estável (não
o pior nem o melhor caso): ~50% dos blocos "atrasados" para o playout sem margem, mas 88,6% desses
atrasos dentro de 1–4 blocos (~2,7–10,6 ms) — evidência de **jitter**, não de bitrate insuficiente
para ESP-NOW nestas condições.

**Buffer mínimo de 4 blocos** (`AUDIOLINK_LINK_RX_BUFFER_BLOCKS`, `link_rx.c`): o RX acumula 4
blocos antes de iniciar o playout (silêncio antes disso, sem contar como perda); depois consome
sempre exatamente 1 bloco/período, nas mesmas regras de sempre (descarta atrasados, concealment
de 1 bloco só, nunca recupera em rajada). Corrida limpa de ~185 s (uptime confirmado monotónico
nas duas placas, sem resets):

| Janela | `ok` | `late` | `buf_underrun`/`concealment` | ocupação (min/avg/max) | TX `backpressure_drop` |
|---|---|---|---|---|---|
| t=60 s | 22343/22349 (99,97%) | 2 | 2 | 2/3/4 | 0 |
| t=120 s | 44857/44885 (99,94%) | 24 | 24 | 2/3/4 | 0 |
| t=180 s | 67384/67421 (99,95%) | 33 | 33 | 1/2/4 | 0 |

`never_recv=0`, `reorder=0`, `dup=0`, `queue_ovf=0` do início ao fim. `late` praticamente deixou
de crescer (33 em 180 s), e mesmo esses ficam quase todos em 1–2 blocos de atraso. TX com
`cb_lat_avg≈1 ms`, sem backpressure. **Critério de sucesso cumprido**: próximo passo é o teste
com tom conhecido de 1 kHz e gravação ponta-a-ponta.

## Validação planeada

1. **Tom conhecido primeiro** (como no passo 1): 1 kHz na entrada do PCM1808, ouvir/gravar a saída do
   PCM5102A. Confirma a cadeia completa antes de música.
2. Só depois música/sinal real, e recolher `LINKTX`/`LINKRX` durante minutos, paradas, movimento, obstrução.
3. **Não** tirar conclusões de robustez desta fase — o objetivo é só confirmar que a cadeia funciona e
   recolher os primeiros números (perdas, `ooo`, underruns/overruns, e mais tarde o drift via `sample_ctr`
   vs. o relógio do RX).
