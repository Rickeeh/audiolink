# AUDIOLINK — plano de hardware

**Direção (Psy, 2026-09-25):** prioridade = **1 TX + 1 RX a funcionar bem, simples e mensurável com o hardware atual.**
**Compras congeladas.** Nenhum upgrade é requisito: só se considera um upgrade depois de uma medição mostrar uma limitação real.
Multiroom / 1 TX → N RX **fora de âmbito** por agora.

## Sistema base
| Nó | Hardware |
|---|---|
| **TX** | ESP32-S3 N16R8 + módulo PCM1808 (ADC) |
| **RX** | ESP32-S3 N16R8 + módulo PCM5102A (DAC) |

Restantes módulos do inventário (2.º PCM5102A, MAX98357A, 3.º e 4.º S3): **só entram com utilidade concreta de teste, diagnóstico ou comparação**
(ex.: 2.º PCM5102A / 4.º S3 como referência A/B ou banco de bancada; MAX98357A como saída "para ouvir" rápida durante depuração). Não fazem parte do desenho.

## Ordem de trabalho (definida pelo Psy)
1. Validar ADC/I²S no TX.
2. Validar DAC/I²S no RX.
3. ESP-NOW básico entre os dois S3.
4. Cadeia completa: analógico → ADC → TX → ESP-NOW → RX → DAC → analógico.
5. Medir perda de pacotes, estabilidade, **drift**, **buffer necessário** e **latência real**.
6. Só depois: FEC / PLC / otimizações.
7. Por fim: medir qualidade analógica e decidir se algum upgrade é justificável.

**Latência e buffer: sem valores assumidos.** Os números da Fase 1 (jitter buffer ≥ 30 ms com FEC ⇒ ~45–60 ms) vêm de simulação sobre traces
e **não são arquitetura final**. O buffer mínimo estável mede-se no sistema real (passo 5) e o compromisso latência/robustez decide-se depois.

## Inventário (declarado pelo Psy)
4× ESP32-S3 N16R8 (2 verificados por USB) · 1× PCM1808 · 2× PCM5102A · 1× MAX98357A. Alimentação: USB de adaptador de parede.
Sem Si5351, sem oscilador de cristal.

## Factos verificados (datasheets/IDF — não de memória)
| Componente | Facto | Fonte |
|---|---|---|
| PCM1808 | VCC analógico **5 V**, VDD digital **3,3 V**; SNR 99 dB típ.; entrada single-ended **fundo de escala 0,6·VCC = 3 Vp-p (≈1,06 Vrms)**, impedância de entrada 60 kΩ; filtro anti-alias interno −3 dB a 1,3 MHz | [TI PCM1808](https://www.ti.com/lit/ds/symlink/pcm1808.pdf) |
| PCM1808 | **MD1/MD0 = Low/Low → slave** (deteta 256/384/512·fs); High/High → master 256 fs; **MD têm de estar definidos antes de ligar a alimentação**; FMT baixo = I²S 24 bit, alto = left-justified; MD/FMT/SCKI toleram 5 V; pull-downs internos de 50 kΩ | idem |
| PCM1808 | Slave: **64 BCK/frame** (ou 48 só com 384 fs); LRCK e SCKI têm de estar **sincronizados** — se a relação variar > ±6 BCK por jitter, o ADC pára e envia zeros até ressincronizar; dados válidos só **8960/fs (~187 ms a 48 kHz)** após o reset + fade-in; mudar SCKI/fs em execução exige forçar o reset parando SCKI ≥ 4 µs | idem |
| PCM5102(A) | Pinos de controlo: **FMT** baixo = I²S; **FLT** baixo = latência normal; **DEMP** baixo = off; **XSMT** alto = sem mute (soft-mute/un-mute em 104 amostras; tr/tf < 20 ns) | [TI PCM510x (SLAS764B)](https://www.ti.com/lit/gpn/PCM5102) — *família; confirmar que a variante "A" não altera estes pontos* |
| PCM5102(A) | **Modo PLL sem SCK:** SCK a 0 V + BCK/LRCK válidos durante 16 períodos de LRCK → a PLL interna gera SCK a partir de BCK; **BCK = 32·fs ou 64·fs**; se a relação LRCK/BCK for inválida > 4 períodos, saída forçada a zero | idem |
| Si5351A | jitter de fase RMS típ. **3,5 ps** (máx. 11 ps) | [Silicon Labs](https://cdn-shop.adafruit.com/datasheets/Si5351.pdf) |
| ESP32-S3 | sem APLL; fontes I²S: PLL_F160M, PLL_D2 (240 MHz), XTAL, MCLK externo (pino MCLK vira entrada; `ext_clk_freq_hz`). O driver escolhe divisor **inteiro + fração a/b**; fração ⇒ o relógio alterna entre duas divisões inteiras | código ESP-IDF v5.4.4 |

## Módulos concretos (confirmado por fotos/serigrafia do Psy — detalhe em `PHASE2_WIRING.md`)
- **PCM1808:** topo BCK/OUT/LRC/SCK/GND/3.3V; baixo FMT/MD1/MD0/GND/3.3V/+5V; entrada LIN/GND/RIN com **10 µF** em série (só bloqueio DC). **Divisor/atenuação de entrada NÃO confirmado.**
- **PCM5102A:** SCK/BCK/DIN/LCK/GND/VIN + fila LROUT/AGND/ROUT/AGND/A3V3/FMT/XSMT/DEMP/FLT com pontes H1L–H4L. **Lado de cada ponte não confirmado** (medir continuidade); **XSMT não é comandado pelo S3** até se saber o estado da ponte H3L.

## Pontos de atenção de projeto
1. **Nível de entrada do PCM1808:** fundo de escala ≈ 1,06 Vrms; uma saída de linha de consumo (~2 Vrms) **clipa**. **Primeiro verificar se o módulo já tem atenuação/condicionamento** (foto/serigrafia); só alterar hardware se não tiver. No primeiro teste, usar fonte a volume baixo e ler o pico (dBFS) no firmware antes de subir.
2. **Relógio do TX sem XO:** o PCM1808 em **slave**, com o S3 como mestre I²S a gerar SCKI (MCLK), BCK e LRCK da mesma cadeia de divisores (sincronia garantida). Preferir divisores **inteiros** ao fracionário (menos jitter por construção) — a escolha de taxa é uma opção de firmware (ver `PHASE2_WIRING.md`); **o critério é desempenho real medido** (48000 Hz fracionário vs 48076,9 Hz inteiro vs, se útil, 46875 Hz) e **não** "inteiro = melhor".
3. **Arranque do ADC:** descartar os primeiros ~250 ms; mudar de taxa exige reiniciar o relógio.
4. **Alimentação:** adaptador USB para começar; comparar com powerbank/portátil na medição de ruído (passo 7), só depois considerar LDOs dedicados.
5. **Limite da cadeia:** o ADC (99 dB) limita o sistema mais do que o DAC (112 dB). O **instrumento de medida** (interface de áudio + Mac) tem piso de ruído próprio: medi-lo primeiro (loopback interface→interface) antes de tirar conclusões sobre os conversores.

## Upgrades possíveis (DOCUMENTADOS — não requisitos, sem compras nesta fase)
Só considerar se uma medição mostrar uma limitação real:
- **Si5351A** (jitter 3,5 ps típ.) no RX para seguir o relógio do TX em passos finos, sem reamostragem — relevante se a deriva medida no passo 5 for problemática.
- **XO de baixo jitter** (12,288 MHz / 24,576 MHz) no SCKI do PCM1808, com o ADC em master — relevante se o piso/espetro do ADC for limitado pelo relógio do S3.
- **ADC melhor** (ex.: PCM1863, 110 dB, entrada até 2,1 Vrms single-ended, controlo I²C — [TI](https://www.ti.com/product/PCM1863)) — relevante se o PCM1808 for o limite medido.
- **LDOs de baixo ruído** para rails analógicos — relevante se o ruído variar com a fonte de alimentação.
- Qualquer compra futura: verificar autenticidade/qualidade do módulo concreto antes.
