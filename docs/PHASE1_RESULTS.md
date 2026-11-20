# Fase 1 — resultados do benchmark de rádio (em curso)

Estado: **ESP-NOW medido em bancada, a 8 m em linha de vista, e em 3 posições com obstáculos (até ~13 m, RSSI −84 dBm), HT40 e HT20.
SoftAP+UDP ainda não medido. Sem recomendação de transporte fechada.**

Hardware: 2× ESP32-S3 N16R8 (antena PCB), ESP-IDF v5.4.4, canal 6, unicast, `WIFI_PS_NONE`.
Cada run é reproduzível: `configs/<nome>.cfg` (overrides) + `results/<run>.{rx,tx}.sdkconfig`
(config exata compilada) + `results/<run>.md` (análise) + capturas `*.raw.txt` (gitignored).

| Run | Config | Resultado | Notas |
|---|---|---|---|
| 001 | HT40 MCS7 SGI, potência default | **inválido** | falha de rate config (canal secundário em falta) — TX degradava em silêncio para a taxa default. Corrigido: agora aborta. |
| 002 | 11g 6 Mbps, 200 B @ 0,32 Mbps, 10 s | 2000/2000, 0 perdas | PHY observada 11bg 6 Mbps 100% |
| 003 | HT40 MCS7 SGI, potência default, 0,32 Mbps | **falha total** (0 ACKs, RX não recebeu nada) | reprodutível; `api_err` = `ESP_ERR_ESPNOW_NO_MEM` (consequência: fila cheia sem ACKs) |
| 004 | HT40 MCS0 LGI, 0,32 Mbps | 2000/2000, 0 perdas | HT40 funciona; o problema é o MCS alto |
| 005 | HT40 MCS7 SGI, **TX 2 dBm**, 0,32 Mbps | 2000/2000, 0 perdas | mesma geometria que 003, só a potência mudou |
| 006 | HT40 MCS7 SGI, TX 2 dBm, **1470 B @ 7,84 Mbps**, 20 s | 13333/13333, **0 perdas**, 0 falhas pós-retransmissão MAC | ESP-NOW v2 a 2× o alvo (3,84 Mbps) |


## Runs 008/009 — sweep com TX autónomo (perfil 0)

O TX passou a percorrer em ciclo 8 pontos de operação (`AUDIOLINK_TX_SWEEP`), levando os parâmetros
no cabeçalho dos pacotes, para poder ficar longe e alimentado a powerbank (sem log próprio).
`run008` = validação com cabos; `run009` = **TX a 8 m, linha de vista, sem paredes, ~6 ciclos por passo**
(sessão 2 da tabela; as sessões 0/1 são de uma ligação acidental do TX ainda perto).

| PHY | TX dBm | Payload | Alvo Mbps | Perdas mediana | Perdas pior ciclo | Maior rajada (nº pacotes) |
|---|---|---|---|---|---|---|
| 11g 6M | 21 | 200 B | 0,32 | 0,00% | 0,00% | — |
| HT40 MCS0 | 21 | 200 B | 0,32 | 0,00% | 0,10% | ≤1 |
| **HT40 MCS4** | 21 | 1470 B | **4,70** | **0,03%** | 0,10% | **≤1 em 6/6 ciclos** |
| HT40 MCS7 | 21 | 200 B | 0,32 | 0,25% | **25,7%** | 363 no pior ciclo |
| HT40 MCS7 | 10 | 200 B | 0,32 | 0,25% | 3,45% | 25 |
| HT40 MCS7 | **2** | 200 B | 0,32 | **59%** | 93% | 287 |
| HT40 MCS7 | 21 | 1470 B | 7,84 | 0,06% | 2,90% | 48 |
| HT40 MCS7 | 10 | 1470 B | 7,84 | 0,15% | 2,39% | 12 |

Leitura (FORTEMENTE SUPORTADO, um só local/orientação):
- **MCS4 a 4,7 Mbps cumpre o critério do brief (≥ ~3,84 Mbps) com perdas isoladas** (rajada máxima de 1 pacote).
- **MCS7 é instável**: mediana boa mas cauda muito má e rajadas longas — não desenhar em cima dele.
- **2 dBm falha a 8 m** (o run 006 a 2 dBm só valia com as placas coladas).
- No desk o MCS7 a 21 dBm funcionou com RSSI −44 dBm (run 008) e falhou com RSSI +3…+7 dBm (run 003): consistente com saturação do recetor a distância muito curta (geometria e potência variaram em runs diferentes — hipótese, não provada).
- O alvo real é 1,92 Mbps, não 4,7–7,8: a pergunta certa é a **taxa PHY mais baixa** com perdas residuais aceitáveis para tráfego com a forma do áudio → perfil 1 do sweep (`configs/sweep_realistic_ht40_ch6.cfg`, 480 B/2 ms em MCS0…MCS7).

Limitações adicionais: 1 posição, 1 orientação de antena PCB, Wi-Fi doméstico com estado desconhecido, ~1 h de bancada no total.

## Runs 010–015 — tráfego realista (1,92 Mbps) e posições com obstáculos

Perfil 1 (HT40, MCS0–MCS7, 480 B/2 ms, 30 s por passo) e perfil 2 (HT20/11g). Condições de cada run em
`results/run0NN_*.conditions.md`. **Ferramenta para juntar posições: `tools/loss_vs_rssi.py`** (perdas por RSSI/MCS).

| Posição | Descrição | RSSI típico | Resultado (resumo) |
|---|---|---|---|
| 1 (run011 + início do 012) | ~10 m, 1 parede + porta fechada | −72 dBm | HT40 MCS0–3 a 1,92 Mbps: 0,03–0,2% perdas, rajadas ≤ 2 pacotes; MCS4 no limite (1 apagão de 0,44 s); MCS5/7 inutilizáveis |
| 2 (run012 sessão 1) | ~5–6 m, parede grossa + 2 portas, TX pendurado a apontar para cima | −55 dBm | perdas ≈ 0 em tudo, até MCS7. **Não foi o teste extremo** (mais perto e melhor orientado que a posição 1) |
| 3 (run013, HT40) | ~13 m, porta de vidro fina | −84…−89 dBm | 11g 6 Mbps @0,32 Mbps: 0%; HT40 MCS0: ~92%; MCS1: 99%; MCS2+: nada |
| 3 (run015, HT20) | mesma posição, largura de banda 20 MHz | −82…−86 dBm | 11g 6M @1,92 Mbps: mediana 0,01%; 11g 12M: 0,01% (mediana); HT20 MCS0: 0,19%; MCS1: 0,02% (medianas de 3 ciclos) |

Leitura (FORTEMENTE SUPORTADO para HT20 vs HT40; condições não idênticas, ver abaixo):
- **HT20 alcança muito mais que HT40 no mesmo ponto** (MCS0: ~92% → 0,2–5% de perdas). Coerente com ~3 dB de sensibilidade
  (canal com metade do ruído). Para 1,92 Mbps não há razão para usar 40 MHz.
- A −84 dBm, **com o canal limpo, até 11g 12 Mbps e HT20 MCS1 tiveram ~0 perdas**. A fronteira por MCS não é limpa: depende do momento.
- **A perturbação dominante é o corpo humano**: em run015 as 4 janelas de interrupções longas (0,2–3 s, várias seguidas) coincidiram com
  o Psy a passar/ficar à frente do TX (confirmado; hora estimada via `tools/outages.py`). Nenhuma interferência externa identificada.
- ⚠ Correção de uma afirmação minha anterior: **não** está provado que o 11g 6 Mbps "aguenta o corpo à frente" — os seus passos
  correram fora das janelas de perturbação. Testar isto exige perturbação controlada.
- Condições não idênticas entre run013 (sem registo de pessoas a passar) e run015 (pessoas a passar): a melhoria HT20 é, se algo, subestimada.

## Simulação offline de FEC + jitter buffer (`tools/fec_sim.py`, resultados em `results/fec_sim_D20.md` / `_D30.md`)

Sobre os traces reais de 480 B/2 ms (PHY candidatos: 11g 6/12M, HT20 MCS0–2, HT40 MCS0–3; ~19 min de áudio "limpo" e ~4 min de "apagões").
**Aproximações que limitam o valor absoluto** (só o ranking relativo é fiável): a paridade ocupa slots do próprio trace (o trace não tem pacotes de
paridade reais); resultados agrupados entre posições e PHY diferentes; sem retransmissão aplicacional. Um evento = run consecutivo de pacotes
de áudio (2 ms cada) não reproduzíveis.

| Esquema (overhead) | eventos/min, jitter buffer 20 ms | eventos/min, jitter buffer 30 ms |
|---|---|---|
| nenhum | 22,6 | 20,2 |
| XOR 4+1 (25%) | 7,5 | 3,9 |
| RS ideal 8+2 (25%) | 8,4 | **2,95** |
| RS ideal 8+4 (50%) | 10,0 | 3,05 |
| XOR 8+2 entrelaçado (12,5%) | 15,8 | 7,8 |

Leitura (estimativa, com as ressalvas acima):
- Em condições limpas, **a maioria dos eventos são perdas isoladas de 1–2 pacotes (2–4 ms)**, ~20/min sem FEC. FEC de ~25% reduz-os ~5–7× **se o jitter buffer for ≥ 30 ms**; a 20 ms o ganho é bem menor porque o bloco de FEC não cabe no prazo.
- Mais redundância além de 25% quase não ajuda (RS 8+4 ≈ RS 8+2); entrelaçar mais não ajuda (as rajadas são curtas e o atraso extra custa).
- **FEC não resolve os apagões de 0,2–3 s** (maior evento continua ~250–320 ms mesmo em runs "limpos", e 20+ s nos apagões): isso exige ocultação/mute/fade do brief.
- Ficam ~3 eventos/min mesmo com FEC → **a interpolação de perdas curtas é obrigatória**, não opcional.

## Runs 016–018 e scan — ESP-NOW vs SoftAP+UDP no MESMO link (modo dual)

Firmware dual (`configs/dual_ht20_ch6.cfg`, perfil 3): um só TX/RX que **alterna** ESP-NOW (11g 6M, HT20 MCS0, taxas fixas) e UDP sobre SoftAP
(taxa escolhida pelo rate control do driver), 1,92 Mbps (480 B/2 ms), 30 s por passo, 21 dBm, 20 MHz, cada transporte várias vezes por ciclo.
Isto elimina o confundimento "um transporte hoje, outro amanhã": pessoas, hora e interferência afetam-nos por igual.

| Ponto | RSSI | ESP-NOW 11g 6M | ESP-NOW HT20 MCS0 | UDP (auto) | Jitter p99 ESP-NOW / UDP |
|---|---|---|---|---|---|
| Bancada (run016) | −17 | 0% | 0% | 0% | 3,7 / 6,4–7,1 ms |
| **Posição 1** (run018) | −60 | 0,00% (3 amostras) | 0,00% (3) | 0,00–0,01% (7) | **3,0 / 6,5 ms** |
| **Posição 3, limite** (run017) | −85…−89 | 0%, 46%, 51% | 24%, 88% | **78–81% (4/4)** | 13–18 / 26–132 ms |

Sem ninguém à frente do TX nos ciclos medidos (indicação do Psy). Associação Wi-Fi: sem quedas nos ciclos medidos.

Leitura (FORTEMENTE SUPORTADO, não conclusivo em todos os regimes):
- **Com margem de sinal, os dois transportes são equivalentes em perdas (~0%).** O UDP tem **~2× o jitter** (caminho lwip) e depende de uma associação Wi-Fi.
- **No limite, o UDP colapsa de forma consistente (~80%, jitter até 132 ms)**, enquanto o ESP-NOW de taxa fixa é errático (0–88%) mas em média melhor.
  Hipótese (não verificada): rate fallback do UDP para taxas onde 480 B/2 ms não cabem → fila cheia → perdas + atraso. Não foi lido o contador do TX.
- **Nenhuma métrica medida favorece o UDP.** Por isso um teste intermédio (escada de potência) não poderia mudar a decisão e não foi feito.
- **11g 6 Mbps ≥ HT20 MCS0** em todos os pares (run015: 0,01% vs 0,10%; run017: 51/0 vs 88/24). Coerente com a sensibilidade ligeiramente melhor do OFDM legado.

⚠ **A variação de RSSI entre sessões na "mesma" posição foi de 4–14 dB**, sem causa identificada:
posição 1: −72…−74 dBm (runs 011/012, HT40) vs −60 dBm (run 018); posição 3: −82…−84 (run 015) vs −85…−89 (run 017).
A −88 dBm 4 dB separam ~0% de ~50% de perdas. **O produto tem de ser dimensionado com pelo menos ~15 dB de margem sobre o limite (~−88 dBm)** e não sobre um ponto medido.

## Espectro (scan001, RX, 58 scans em 12 min — `results/scan001_rx_location.md`)

- Canal 1: muito ocupado (≈2,7 redes, −44 dBm). Canal 6: quase livre (só redes a −90 dBm) mas a ~2 canais de uma rede a −63 dBm (canal 8). Canais 11–13: os mais limpos por este critério.
- Limitações: só vê APs com beacons; não vê tráfego de clientes nem fontes não-Wi-Fi; só o local do RX. A interferência externa observada (run015, 22:03, ~4 s) **não** tem causa identificada.
- Hipótese por testar: canal 13 (ou 11) melhor que 6. Requisito do brief: o TX escolher o canal no arranque (Fase 5).

## Recomendação de transporte (PROVISÓRIA) e critérios de aceitação da Fase 1

**Recomendação: ESP-NOW v2 unicast, PHY legado 11g 6 Mbps (20 MHz) a 1,92 Mbps, com jitter buffer ≥ 30 ms, FEC leve (~25%) e ocultação obrigatória.**
Razões medidas: perdas iguais com margem, jitter ~2× menor, comportamento no limite ≥ UDP, sem dependência de associação; controlo explícito de taxa/potência.
Não medido: 1 TX → N RX, cifra CCMP, latência absoluta, estabilidade de horas, canais alternativos, uso de CPU.

| Critério do brief | Estado |
|---|---|
| Sustentar ≥ ~3,8 Mbps com margem | **Cumprido em condições boas** (ESP-NOW MCS4 4,7 Mbps: 0,03%; MCS7 7,8 Mbps: 0,06–3%). Ao PHY recomendado (11g 6M) só há prova de 1,92 Mbps. |
| Perdas residuais recuperáveis pelo FEC proposto | **Parcial:** FEC ~25% + buffer 30 ms reduz ~5–7× os eventos curtos (simulação aproximada), mas **não** apagões de 0,2–3 s (corpo humano, interferência) → ocultação obrigatória |
| Distribuição de rajadas, jitter p50/p99/p99,9 | Medido; o jitter inclui filas do stack, não é ar puro |
| Condições: 1–2 paredes, Wi-Fi doméstico ativo | Feito em 3 posições; **sem barras de erro** (1–4 ciclos por ponto), sem testes de horas |
| Uso de CPU / retransmissões por frame | **Não medido** (a API não expõe retransmissões por frame) |

## O que está estabelecido (CONFIRMADO)

- ESP-NOW v2 com payload de 1470 B sustenta 7,84 Mbps sem perdas **nestas condições ideais** (curta distância, 20 s).
- A taxa PHY configurada é a realmente usada: verificado por pacote no RX (`sig_mode/mcs/cwb/sgi`).
- Com potência TX por omissão e placas adjacentes, MCS7 falha por completo; a 2 dBm funciona (003 vs 005).

## Hipóteses (NÃO provadas)

- **Saturação/sobrecarga do recetor** (RSSI observado +3…+7 dBm a potência máxima) como causa da falha do MCS7 (003). FORTEMENTE SUPORTADO por 003 vs 005, mas só testei dois níveis de potência e uma geometria; a alternativa (não-linearidade do PA do TX a MCS altos) dá o mesmo sintoma. Distinguir exigiria variar a distância a potência fixa.
- O RSSI reportado a curta distância pode não ser fiável (valores positivos); tratar como indicativo.

## Limitações conhecidas destas medições

- **Jitter (p99 ≈ 2,7–3,8 ms, p99,9 ≈ 3,5–8,7 ms)** inclui filas do stack ESP-NOW no TX (timestamp tirado antes de `esp_now_send`) e latência de escalonamento da task wifi no RX (timestamp no callback). Não é jitter de ar puro. Serve para dimensionar o jitter buffer (10–30 ms do brief), não para caracterizar o canal.
- 10–20 s por run e 1 repetição: **sem barras de erro**. O brief pede condições adicionais (1–2 paredes, Wi-Fi doméstico ativo, testes mais longos) que ainda não foram feitas.
- Sem medição de CPU nem de retransmissões por frame (a API só expõe o resultado final).
- O `cb_fail=1` dos runs 002/004 (1 callback sem ACK) não foi atribuído a um pacote concreto (provavelmente um marcador de fim, mas não verificado).

## Próximos passos

1. Distância realista + 1–2 paredes + Wi-Fi doméstico ativo (requer mexer nas placas), runs ≥ 60 s, repetidos.
2. SoftAP+UDP nas mesmas condições (comparação lado a lado).
3. Só então: recomendação de transporte fundamentada.
