# AudioLink L2-12 vs WillyBilly06/esp-now-audio-{source,sink}: comparação técnica (2026-10-31)

**Estado:** análise. **Nada implementado nem gravado.** Referência: L2-12 (`39b5b9a6f`), aprovado na audição no SU-2.
Rollback: L1 (`0e60ad7`).

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

**Fontes:**
- Ramo `main` (ADPCM) do source e do sink. O código atual no GitHub é **idêntico byte a byte** à cópia arquivada a
  03/10 (hashes de conteúdo iguais), embora o histórico tenha sido reescrito com outros hashes.
- O ramo `lc3` (OpenALS) evoluiu depois e não é o que está em causa.
- Auditoria anterior: `experiments/_ARCHIVE_SCRATCHPAD_2026-10-07/files/AUDIT-EXT-ESPNOW-AUDIO-01/AUDIT.md`, focada no
  DEEP-TX e não no playout.

## 1. Como o projeto externo controla o atraso

**Mecanismo:**
- Cada pacote leva `src_t_us`, o relógio do ESP da fonte no fim da leitura do bloco I²S.
- O sink mantém uma média móvel (α = 1/16) do desvio (chegada − `src_t_us`).
- Na descodificação calcula "atraso de reprodução" = agora − (`src_t_us` + desvio médio).

**O que essa métrica mede (CONFIRMADO, código + modelo):**
- o **tempo que o pacote esperou na fila do sink, mais o seu desvio face ao atraso médio**;
- o **atraso médio de transporte é absorvido pelo desvio médio** e não aparece: com 1, 5 ou 20 ms de transporte, o
  modelo dá sempre ~1,9 ms;
- também não entram a captura (bloco de 2 ms), o DMA de saída nem os conversores.

**Os "12 ms" são um alvo dessa métrica**, não latência de ponta a ponta.
- Não há nenhuma medição física no repositório.
- Vários números do README e dos comentários são de uma configuração anterior:
  - `FRAME_TIME_US` "~5333us" quando são 2000;
  - DMA "~5333us" quando são 1333;
  - `pps=188` quando são 500.

**Controlo em si:**
- fila alvo de 1 trama de 2 ms (2 sob jitter);
- até 1–2 ms de espera por um pacote antes de ocultar;
- se a métrica passa de 12 ms (teto 17 ms), descarta tramas inteiras da fila;
- **a deriva de relógio corrige-se descartando ou repetindo tramas inteiras de 96 tramas (2 ms), sem slip fino.**

**Latência real estimada do externo, num canal limpo:**

| Componente | Valor |
|---|---|
| ADC | ~0,4 ms |
| Bloco | 2 ms |
| Ar a 1 Mb/s | ~1,5 ms |
| Fila | 2–4 ms |
| Espera | 0–2 ms |
| DMA | 1,3 ms |
| DAC | ~0,5 ms |
| **Total** | **~8–12 ms** (PROVÁVEL, não medido) |

## 2. Jitter, perdas e timing

| Aspeto | Externo | AudioLink L2-12 |
|---|---|---|
| Transporte | Broadcast sem ACK, **taxa por omissão de 1 Mb/s** (não configurada), 500 pps: **~91 % do canal** | Unicast com ACK, MCS4, 375,6 pps: ~13 % do canal |
| Áudio | IMA ADPCM 4:1, com perdas | PCM24 sem perdas |
| Tolerância a atrasos | ~2–6 ms | 29,3 ms de margem mínima real |
| Ocultação | Repete a última trama e faz crossfade de 24 amostras no regresso. **Sem fade-out:** numa falha repete a trama, um tom de 500 Hz, o "beep" que o C1 eliminou | Fade-out espelhado, silêncio, fade-in (C1) |
| Adaptação | Fila alvo 1 → 2 tramas se houver ≥ 2 ocultações em 5 s; volta ao fim de 3 janelas calmas | Fixa (margem por ocupação mínima) |
| Deriva | Descarte ou repetição de 2 ms | Slip de 1 trama, ≤ 8,3 ppm |
| DMA de saída | 2 × 32 tramas (1,3 ms) com escritas de 96: **folga negativa**. Não há deteção de underrun de DMA nem `auto_clear` | 4 × 128 (10,6 ms), folga de 8 ms medida (`dma_fill_min`), underrun = silêncio |
| Resync e sessões | Corta ao fim de 120 ms sem pacotes; "reboot" da fonte por salto > 200 ms | Sequência contígua tocada, sessão, plausibilidade (C1, REANCHOR) |

**No nosso ambiente** (atrasos de acesso ao canal de 10–25 ms medidos, e comboios do AP vizinho):
- a tolerância de 2–6 ms do externo daria ocultações em **10–56 % dos segundos** (`txlat.py`: segundos com atraso
  > 5–8 ms);
- os 91 % de ocupação a 1 Mb/s competiriam com os vizinhos, e o CCA atrasa um broadcast tanto como um unicast.

## 3. A compressão é determinante para a baixa latência?

**Não** (FORTEMENTE SUPORTADO).
- O ADPCM serve para caber em 1 Mb/s. Não encurta nenhum buffer.
- A latência baixa do externo vem de três escolhas:
  - **fila mínima** (1–2 tramas);
  - **DMA mínimo**;
  - **aceitar ocultar com frequência**.
- A MCS4, o nosso pacote PCM24 ocupa o ar ~0,3 ms. O pacote ADPCM do externo a 1 Mb/s ocupa ~1,5 ms: **o nosso PCM
  sem perdas está menos tempo exposto por pacote.**
- A margem de que precisamos é ditada pela cauda de espera pelo canal, não pelo tamanho dos dados.

## 4. Separação

| Categoria | Princípio |
|---|---|
| **Já implementado (às vezes melhor)** | Escrita I²S bloqueante como relógio mestre. Callback de receção mínima. Carimbo de tempo: o `sample_ctr` é um carimbo no relógio do áudio da fonte, melhor do que relógios de CPU com desvio estimado. Ocultação sem artefactos (C1). Deriva por slip de 1 trama. Resync robusto. Telemetria de folga do DMA e de underruns |
| **Realmente novo e aproveitável** | (a) **DMA de saída mínimo**, mas dimensionado pela folga medida, o que o externo não faz. (b) **Controlo do atraso em µs a partir do carimbo**, em vez da ocupação em blocos inteiros: eliminaria o dente de serra de 1 bloco (proposta C3 de 08/10). Ganho ~1,3–2,7 ms, complexidade média. (c) **Adaptação da margem ao canal** |
| **Não vale a pena replicar** | ADPCM (perde qualidade sem ganho de latência). Broadcast a 1 Mb/s (91 % do canal; o CCA não desaparece). Repetir a última trama como ocultação. Correção de deriva por tramas inteiras. DMA com folga negativa e sem deteção de underrun |

**A adaptação da margem, simulada com os atrasos já gravados** (`experiments/LAT-01/adaptive_sim.py`):
- **Política:** cresce no próprio mergulho (sem novo artefacto) e só encolhe em silêncios (pausas entre faixas).
- **Resultado:** média ~37–41 ms contra 46 ms do L2-12 fixo, com **2–3× mais mergulhos** (0,006–0,011/s contra
  0,003/s). Só nos períodos calmos desce para ~31–33 ms.
- **Condição:** depende de haver silêncios a cada 1–3 min. Com música contínua não encolhe, porque encolher sem
  silêncio seria audível.
- **Leitura:** ganho condicional, mais mergulhos e um laço de controlo novo. **Hoje não compensa.**

## 5. Oportunidade mais promissora: DMA de saída mínimo e seguro (8 × 32, escritas de 32 tramas)

**O princípio vem do externo, que o leva ao extremo sem rede.** Nós podemos fazê-lo com a folga medida.

| Anel (N × F, escrita) | Latência do anel | Folga do escritor | Folga ÷ pior S (0,92 ms) | Ganho |
|---|---|---|---|---|
| 4 × 128, 128 (L2-12) | 10,6 ms | 8,0 ms | 8,7× | — |
| 3 × 128, 128 | 8,0 ms | 5,3 ms | 5,8× | −2,7 ms |
| 4 × 64, 64 | 5,3 ms | 4,0 ms | 4,3× | −5,3 ms |
| **8 × 32, 32** | **5,3 ms** | **4,7 ms** | **5,1×** | **−5,3 ms** |
| 2 × 32, 32 (≈ externo) | 1,3 ms | 0,7 ms | 0,7×: underrun | −9,3 ms |

**A alteração** (só no RX; sem margem, TX, protocolo, PHY nem canal):
- `I2S_DMA_DESC` 4 → 8;
- descritor de 128 → 32 tramas;
- o playout continua a decidir por bloco de 128 (ocultação, slip e resync iguais), mas **escreve cada bloco em 4
  partes de 32**;
- `deadline_miss` e `dma_fill_min` continuam válidos.

**Ganho:** **−5,3 ms determinísticos.** Mede-se diretamente: chegada → saída e enchimento do anel de 10,6 → 5,3 ms.
- Ponta a ponta: L2-12 ~47 → **~42 ms**.
- Não é "substancial" sozinho (~11 %), mas é o maior ganho que se pode demonstrar sem tocar na margem nem no rádio.

**Riscos:**
- Folga de 4,7 ms em vez de 8,0 (ainda 5× o pior atraso medido do escritor).
- 1502 interrupções de DMA/s em vez de 376, no núcleo do Wi-Fi e do playout. Custo estimado < 1 % de CPU, a
  confirmar.
- 4 chamadas de escrita por bloco.
- O slip (escritas de 127/129 tramas) fica repartido em partes de 32/33. Testar.

**Experiência mínima** (o mesmo procedimento do L1):
1. **Offline:** auditoria de objetos. Só o playout e as constantes do canal I²S mudam.
2. **Build de teste com surdez,** ~4,5 min. **PASS:** K1–K5, `dma_fill_min` nunca abaixo de 2 descritores (64
   tramas), 0 underruns, atraso do escritor medido.
3. **Produção,** ~5 min. **PASS:**
   - anel medido em 5,3 ms;
   - chegada → saída 5 ± 1 ms abaixo do L2-12 na mesma fase;
   - 0 underruns;
   - ocultações como no L2-12.
4. **Audição no SU-2.**

**Critérios de abandono:**
- algum underrun;
- `dma_fill_min` a descer até 1 descritor;
- atraso do escritor > 2 ms;
- qualquer artefacto na audição.

Nesse caso volta-se ao L2-12, que é a versão em uso, sem alterações.

## 6. O que fica por resolver

**A parte grande da latência que sobra** (~29 ms de margem) é ditada pela **cauda de espera pelo canal no nosso
ambiente**: comboios de respostas de sonda do AP vizinho, até ~25 ms.
- **O projeto externo não a resolve: esconde-a**, aceitando ocultações frequentes e sem qualidade.
- **Uma redução substancial (> 15 ms) sem perder estabilidade nem qualidade** exige encurtar essa cauda:
  - canal ou ambiente; o vizinho muda de canal sozinho;
  - ou outro rádio (5 GHz; compras congeladas).
- Exige isso, e não importar a arquitetura externa.
