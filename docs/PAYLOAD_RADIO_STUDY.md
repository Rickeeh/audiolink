# Payload e rádio — estudo e plano de decisão (2026-10-08)

**Estado:** estudo e plano. **Nada foi implementado nem alterado.**

**Decisões do utilizador (2026-10-07):**
- PCM de 24 bit empacotado é o candidato principal;
- 16 bit com dither é uma alternativa a avaliar, não uma decisão;
- a taxa PHY decide-se com testes reais, incluindo obstáculos;
- protocolo, transporte e playout não mudam ao mesmo tempo.

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

## 1. Problema demonstrado
- **O payload atual é int32 com 24 bit úteis alinhados à esquerda:** 8 bits de enchimento por amostra, ou seja 25 % do
  pacote (`link_proto.h`). — CONFIRMADO
- **Foi o tamanho que obrigou a sair do ponto robusto da Fase 1:** 3,08 Mb/s de payload contra 1,92 Mb/s em 11g 6M, que
  bate com 16 bit estéreo + 25 % de FEC. Daí o HT20 MCS4, marcado como PROVISÓRIO
  (`audio/main/Kconfig.projbuild:478`). — CONFIRMADO
- **O HT20 MCS4 nunca foi medido com obstáculos.** O MCS4 da Fase 1 era HT40. Na posição 1 (~10 m, 1 parede) teve um
  apagão de 0,44 s; o HT20 alcança muito mais do que o HT40 no mesmo ponto. — CONFIRMADO (`docs/PHASE1_RESULTS.md`)
- **Na bancada (−32 dBm), o tamanho e a taxa não explicam o E6 nem o DEEP-TX.** — PROVÁVEL. A questão aqui é a
  **robustez de alcance**, ainda por medir.

## 2. Formatos candidatos (128 tramas por pacote, ~375,6 pps; cabeçalho v2 de 28 B)

| Formato | Payload | IEs ESP-NOW | Tempo de antena: HT20 MCS4 | 11g 24M | 11g 12M | 11g 6M |
|---|---|---|---|---|---|---|
| int32 (atual) | 1052 B | 5 | 15,5 % | 20,3 % | 34,6 % | 63,3 % |
| **24 bit empacotado** | **796 B** | 4 | **13,6 %** | **17,0 %** | **28,0 %** | 50,1 % |
| 16 bit + dither TPDF | 540 B | 3 | 11,5 % | 13,7 % | 21,4 % | 36,9 % |

Modelo: `experiments/_ARCHIVE_SCRATCHPAD_2026-10-07/files/AUDIT-EXT-ESPNOW-AUDIO-01/airtime.py`, slot de 9 µs, sem
reenvios. Os valores são ESTIMATIVAS.

- **Variante de cadência:** 16 bit com 256 tramas por pacote dá 1052 B a 188 pps.
  - Metade das tramas e das callbacks por segundo, para o mesmo tamanho de trama.
  - Custos: +2,66 ms de enquadramento e perdas em grão de 5,3 ms.
  - Só faz sentido se o custo por trama da pilha (`cb_lat` 1,1–1,9 ms para ~0,4 ms de antena) se revelar limitante.
    HIPÓTESE.

## 3. Qualidade
- **24 bit empacotado:** sem perdas em relação à saída do ADC (os 24 bit úteis). Custo de CPU desprezável: empacotar 3
  bytes por amostra. — CONFIRMADO pela aritmética
- **16 bit com dither TPDF:** ruído de −96,3 dBFS RMS (σ = 2⁻¹⁶ do fundo de escala).
  - Piso medido do ADC, na mesma convenção (RMS relativo ao fundo de escala; `docs/PHASE2_WIRING.md`):
    - L: −86,6 dBFS no total, dos quais **≈ −95 dBFS de banda larga** sem o hum;
    - R: −78,7 dBFS.
  - **O 16 bit somaria ~+2,4 dB ao piso de banda larga do L** (+0,4 dB com o hum; +0,1 dB no R). — PROVÁVEL (depende das
    condições dessa medição)
  - Com um ADC melhor no futuro (o upgrade documentado PCM1863, 110 dB), o 16 bit passaria a ser o limite.
  - **Conclusão:** o 16 bit não é transparente para este ADC no canal L. O 24 bit empacotado é coerente com "estéreo de
    elevada qualidade".
- **Codecs com perdas** (ADPCM, LC3): fora do objetivo de qualidade. O LC3 acrescentaria ainda latência de trama e de
  lookahead. Não se propõem.

## 4. Taxa PHY — plano de testes reais (sem mudar código de produto)
**O firmware de bancada da Fase 1 já cobre isto só com configuração:**
- tamanho, intervalo, PHY HT20/11g, MCS0/MCS4, 6/24 Mb/s e potência;
- o RX regista cada pacote em PSRAM (hora de chegada, RSSI, taxa) e despeja-o no fim.

Isto também dá **chegadas por pacote**, que faltam para dimensionar a margem do RX.

- **Matriz proposta:**
  - payload {1052, 796, 540} × taxa {HT20 MCS4, HT20 MCS0, 11g 24M, 11g 6M}. Como opções isoladas do Kconfig, o bench
    expõe 1M, 11M, 6M, 24M, 54M, MCS0, MCS4 e MCS7. O 11g 12M e o HT20 MCS1–3 só existem nas tabelas de sweep (perfil 2),
    e acrescentá-los como opção isolada seria uma alteração pequena ao bench (ferramenta de teste, não produto);
  - posições: bancada; posição 1 (~10 m, 1 parede, porta); posição 3 (~13 m, porta de vidro);
  - perturbação corporal controlada: pessoa entre os nós, em intervalos marcados;
  - ≥ 10 min por ponto, intercalado (ABBA) para controlar o ambiente.
- **Métricas:**
  - perdas;
  - apagões > 50 ms por hora;
  - cauda do atraso de chegada (p99,9 e máximo por minuto);
  - RSSI.
- **Critério de escolha (proposta para decisão):** a taxa mais robusta com tempo de antena ≤ 30 % que, na posição 1 com
  perturbação corporal, tenha:
  - perdas reais ≤ 0,1 %;
  - apagões > 50 ms ≤ 1 por hora;
  - cauda de atraso compatível com a margem do RX, para cumprir R2 (`CORE_V1_REQUIREMENTS.md`).
  - Margem de RSSI ≥ 15 dB sobre o limite observado (recomendação da Fase 1).
- **Pré-condição:** o resultado do diferencial DEEP-TX. Se o DEEP-TX depender da aplicação, os testes de rádio com o bench
  não são afetados por ele.

## 5. Alterações propostas (modelo de disciplina)

### P1 — Payload de 24 bit empacotado (protocolo v3; TX e RX)
- **Problema demonstrado:** 25 % do pacote é enchimento, e o tamanho prende a taxa PHY (secção 1).
- **Causa:** a escolha provisória de "copiar o buffer do I2S tal como vem".
- **Alternativas:** int32 (atual); 24 bit empacotado; 16 bit com dither; 16 bit com 256 tramas.
- **Experiência mínima:**
  1. teste de ida e volta no host (empacotar/desempacotar bit-exato, com vetores de teste);
  2. cadeia completa na bancada: tom e silêncio, comparando a saída do DAC com o v2 (nulo ≤ ruído) e as perdas/margem
     com o baseline.
- **Critério:**
  - bit-exato nos 24 bit;
  - perdas, margem e latência não piores do que o v2 na mesma taxa;
  - versão do protocolo a 3, com rejeição limpa de v2/v3 cruzados (`pkts_corrupt`).
- **Impacto:**
  - latência ≈ 0;
  - robustez: mesma taxa, −12 % de tempo de antena, e permite descer de taxa;
  - qualidade igual (sem perdas);
  - manutenção: duas funções de (des)empacotamento e a versão.
- **Reversão:** Kconfig de formato no TX e no RX; binários v2 (RC2/RC1) arquivados.
- **Ordem:** depois de os passos de tempo do RX (C1–C3) estarem validados, para não confundir efeitos. Nunca no mesmo
  build que uma mudança de taxa.

### P2 — Taxa PHY escolhida por medição
- **Problema demonstrado:** taxa provisória, sem dados de alcance em HT20 MCS4.
- **Causa:** o adiamento do "passo 5" do plano original.
- **Alternativas:** HT20 MCS4 (atual); MCS0–MCS3; 11g 24M/12M/6M (6M só com 16 bit, e mesmo assim com 37 % de tempo de
  antena).
- **Experiência mínima:** a matriz da secção 4, com o bench (sem código de produto).
- **Critério:** o da secção 4.
- **Impacto:**
  - latência: +0,1–0,5 ms de antena por trama em taxas mais baixas;
  - robustez de alcance: melhor (é o objetivo);
  - qualidade igual;
  - manutenção: uma constante.
- **Reversão:** Kconfig de taxa (já existe).
- **Ordem:** a medição pode correr em paralelo com o trabalho do RX, porque não toca no produto. A mudança no produto
  vem depois do P1, num passo separado.

## 6. Notas
- **SoftAP de bancada:** definir uma palavra-passe própria no menuconfig antes de usar este transporte. As credenciais da bancada original foram removidas da publicação. ESP-NOW não usa esta configuração.
- **A FEC continua fora:** as perdas medidas são atraso, não perda (`never_recv` ~1 %). Reavaliar só se os testes de
  alcance mostrarem perdas reais.
