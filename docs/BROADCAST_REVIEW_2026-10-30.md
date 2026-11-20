# Avaliação crítica das propostas Grok/Gemini: broadcast sem ACK + redundância temporal (2026-10-30)

**Estado:** avaliação e recomendação. **Nada implementado nem gravado.** O L1 (`b75e5c7ec`) está gravado no RX à espera
da audição; o C1 é o rollback.

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

**Nota:** o RX WM8805 tem agora uma antena externa de 2,4 GHz (só ele). Nada no código depende disso. As medições do
lado do RX feitas a partir de agora não se comparam diretamente com as anteriores.

**As propostas:**
- **Grok:** ESP-NOW broadcast sem ACK + redundância. Prevê 25–35 ms.
- **Gemini:** blocos de 64 tramas, cada pacote com os blocos N e N−1, 3 blocos de jitter buffer. Prevê 14–16,5 ms.
- **Hipótese comum:** a cauda dos atrasos vem das retransmissões MAC do unicast com ACK.

## 1. A cauda vem das retransmissões? Não se sabe, e há pelo menos três causas plausíveis

**O que a `cb_lat` mede** (envio → callback de envio, no TX):
- espera na fila do TX;
- espera pelo canal livre (CCA, DIFS e backoff);
- transmissão e ACK;
- **cada retransmissão** (com backoff a dobrar);
- o despacho da callback pela tarefa Wi-Fi.

**Os dados existentes não separam estas parcelas** (DESCONHECIDO):
- `send_fail` = 0 nas janelas limpas: nenhum pacote esgotou as retransmissões. Não diz quantas houve.
- `pending_max` é quase sempre 2: a fila do TX não é a origem principal (PROVÁVEL).

**Há indícios fortes de CCA, que o broadcast não elimina** (FORTEMENTE SUPORTADO, varredura de 30/10 00:43):
- dois APs vizinhos no próprio canal 6, em 40 MHz (−72 a −77 dBm, acima do limiar de deteção de preâmbulo);
- um AP vizinho forte ("V") no canal 8, que se sobrepõe ao 6, a −48 dBm. Às vezes salta para o 11, o que pode
  explicar a variação da cauda ao longo do dia;
- beacons a 1 Mb/s duram ~2 ms cada;
- ao fim da tarde a cauda teve um modo concentrado em 10–16 ms (46 % dos segundos > 10 ms, só 1,6 % > 16 ms). Isso
  parece ocupação periódica do canal, não a cauda geométrica típica de retransmissões (HIPÓTESE).

**Conclusão:** as retransmissões são uma causa possível, mas não estão demonstradas como a dominante. **Um broadcast
continua a esperar pelo canal livre.** Se a cauda for sobretudo CCA, o broadcast não a encurta.

## 2. O ESP-IDF 5.4.4 deixa controlar as retransmissões?

| Via | Estado |
|---|---|
| API pública para limitar retransmissões no unicast | **Não existe** (CONFIRMADO nos headers). `esp_now_rate_config_t` só tem `phymode`, `rate`, `ersu`, `dcm` |
| `esp_wifi_internal_set_retry_counter` | **Existe na biblioteca fechada** (`libnet80211.a`, esp32s3) mas **não está declarado** em nenhum header. Assinatura, efeito no ESP-NOW e estabilidade entre versões: DESCONHECIDOS. Não é uma via suportada |
| Broadcast (`FF:FF:FF:FF:FF:FF` como par) | Suportado e documentado. Não há ACK nem retransmissão. O RX já aceita: filtra pelo MAC de origem |
| **Taxa do broadcast** | **Ponto crítico.** A taxa por omissão do ESP-NOW é 1 Mb/s, o que daria 263 % do canal (§4). `esp_now_set_peer_rate_config` é por par; que aceite o par de broadcast em MCS4 está implícito na doc, mas **não está confirmado** (DESCONHECIDO até um teste) |
| Estatísticas Wi-Fi (`esp_wifi_statis_dump`) | Pública, mas imprime na consola. Não serve no caminho de tempo real, e o conteúdo exato das contagens de retransmissão é DESCONHECIDO |

## 3. A redundância N+N−1 funciona?

**O mecanismo:** o pacote k leva os blocos k e k−1.
- **Recupera uma perda isolada,** mas só se o pacote k+1 chegar antes do prazo do bloco k. Isso **acrescenta um
  período de pacote ao prazo**: o buffer tem de esperar mais um bloco.
- **Não recupera duas perdas seguidas** (k e k+1).

**Quanto atrasos, não ajuda:** se o canal estiver ocupado 10 ms (CCA), os pacotes k, k+1, … chegam todos tarde. A cópia
de k vem num pacote ainda mais tardio. A redundância só converte **perdas**, não atrasos.

**Viabilidade:** depende da distribuição de rajadas de perdas no broadcast, que não temos (DESCONHECIDO). Uma aproximação
mede-se no unicast atual: uma 1.ª tentativa sem ACK equivale, no broadcast, a uma perda. É exatamente o que a
experiência da §6 dá.

## 4. Custo de dobrar a cadência

Modelo de tempo de antena (DIFS + backoff médio + preâmbulo HT + dados; mais SIFS + ACK no unicast):

| Esquema | µs/pacote | Ocupação do canal |
|---|---|---|
| Hoje: unicast MCS4, 128 tramas, 375,6/s | 350 | 13,1 % |
| Broadcast MCS4, 128 tramas | 308 | 11,5 % |
| **Broadcast MCS4, 64 tramas N+N−1** (pacote do mesmo tamanho), 751/s | 308 | **23,1 %** |
| Broadcast à taxa por omissão (1 Mb/s) | 7000 | **263 %, impossível** |
| Broadcast legacy 6 Mb/s | 1240 | 46,6 % |

**O custo real:**
- **Antena:** o tempo de antena duplica e o número de acessos ao canal também. Cada acesso é uma nova exposição à
  espera por CCA, portanto mais oportunidades por segundo de um pacote se atrasar.
- **CPU e callbacks:**
  - TX: 751 envios e callbacks/s.
  - RX: 751 callbacks de receção/s na tarefa Wi-Fi, que corre no **mesmo núcleo do playout**.
  - Playout com grão de 64 tramas: 751 iterações e 751 interrupções de DMA/s.
- **O S3 aguenta,** mas o atraso do escritor (hoje ≤ 1,35 ms) vai crescer. Com um anel DMA de poucos blocos de 64
  tramas, a folga encolhe ao mesmo tempo. O TX tem historial de sensibilidade à carga da aplicação (DEEP-TX).

## 5. Os orçamentos são consistentes?

**Orçamento do Gemini, com os componentes que medimos:**
- ADC: ~0,4 ms;
- enquadramento de 64 tramas: 1,33 ms;
- ar: ~0,3 ms;
- jitter de 3 blocos: 4,0 ms;
- anel DMA de 64 tramas com folga mínima: ~4–5 ms;
- WM8805, SU-2 e filtros: ~1–1,5 ms.

**Soma ≈ 12,5–14 ms. A aritmética fecha. O problema é o que os 4 ms de jitter têm de cobrir:**
- 1 bloco (1,33 ms) só para esperar pelo pacote N+1 que traz a cópia;
- 1 bloco (1,33 ms) de oscilação da ocupação. Medida no C1: a margem real cai um bloco durante ~11 min, e isto não
  desaparece com blocos menores;
- **sobra ~1,3 ms para o atraso de entrega.**
- Medido (unicast): o máximo por segundo passa 4 ms em 27–70 % dos segundos. Um único beacon a 1 Mb/s de um vizinho no
  canal 6 já são ~2 ms de espera por CCA.

**→ Os 14–16,5 ms são incompatíveis com o canal medido:** dariam atrasos várias vezes por segundo. FORTEMENTE
SUPORTADO.

**Grok (25–35 ms):** plausível **só** se o broadcast reduzir a cauda para ~10–15 ms (p99 por segundo). É precisamente o
que não sabemos.

**Comparação justa:** o L1 com o L2 já dá ~37–42 ms com ACK (§8 de `LATENCY_REVIEW`). O ganho realista da arquitetura
nova sobre isso é **~5–15 ms**, se a hipótese das retransmissões se confirmar, e custa reescrever o transporte e dobrar
o tráfego.

## 6. Recomendação: uma experiência passiva, decisiva e sem firmware

**E-ATTR: captura do canal 6 no Mac**, com o Wi-Fi `en1` em modo monitor.
- **Condições:**
  - o Mac está ligado à rede por Ethernet (`en0`), e o `en1` está ligado ao router próprio a 5 GHz; a captura **não corta a rede**;
  - precisa de `sudo`, corrido por ti;
  - o RX deve estar **no sítio real** (sala), num período mau (fim da tarde ou noite);
  - 15 min, em simultâneo com `tx_session_rec.sh` (telemetria do TX).
- **Comando previsto,** a confirmar num ensaio de 10 s antes de valer:
  `airport -z; airport -c6; tcpdump -I -i en1 -y IEEE802_11_RADIO -s 200 -G 900 -W 1 -w ch6.pcap`
  - 200 B por trama: chegam o cabeçalho radiotap, o MAC, o ESP-NOW e o `sample_ctr` do ALK; ~0,2–0,4 GB no total;
  - para sozinho ao fim de 900 s.
- **Análise** (script a preparar antes da captura), para cada pacote do TX (MAC de origem, número de sequência,
  `sample_ctr`):
  1. **Atraso de acesso** = 1.ª tentativa − instante ideal (dado pelo `sample_ctr`): fila + CCA + backoff + pilha;
  2. **Atraso por retransmissões** = tentativa com ACK − 1.ª tentativa, mais o número de tentativas;
  3. **Ocupação do canal por vizinhos** antes de cada 1.ª tentativa atrasada (CCA) e sinal de canal livre (que
     apontaria para atraso interno do TX);
  4. **Rajadas de 1.as tentativas sem ACK**, isto é, as perdas que o broadcast teria e que a N+N−1 teria de cobrir.

**Critério de validade:** ≥ 95 % das tramas do TX capturadas (continuidade da sequência) e ≥ 20 segundos com atraso
> 10 ms na janela. Senão, repete-se uma vez noutro período mau, e fica por aí.

**Critérios de decisão:**

| Resultado | Decisão |
|---|---|
| **Retransmissões ≥ 60 %** do atraso nos pacotes com mais de 8 ms, **e** perdas de 1.ª tentativa em rajadas de ≥ 2 em ≤ 10 % dos casos, **e** atraso de acesso com p99 por segundo ≤ 8 ms | **Avançar** para um protótipo mínimo de broadcast: só TX, 128 tramas, sem mudar o tamanho do bloco. Primeiro confirmar a taxa MCS4 no par de broadcast; A/B de 10 min por braço. Só depois se pensa em redundância |
| **Atraso de acesso (CCA ou fila) ≥ 50 %** da cauda, **ou** p99 por segundo do atraso de acesso ≥ 10 ms | **Abandonar o broadcast:** não encurtaria a cauda. As alavancas passam a ser o L2 (já preparado) e, se o utilizador o permitir, o **canal**. A mesma captura nos canais 1, 11 e 13 mede a ocupação em minutos |
| 1.ª tentativa atrasada **com o canal livre** | Atraso **interno ao TX** (tarefa Wi-Fi ou pilha). Investiga-se o TX, coisa barata, antes de qualquer arquitetura |

**Não recomendo** adotar agora a proposta do Gemini (64 tramas, N+N−1, 3 blocos):
- o orçamento contradiz o canal medido;
- dobra o tempo de antena e a carga de CPU;
- a redundância não trata atrasos.

## 7. Como medir a latência e a variabilidade sem enganos

- **Os eventos DMA não são saída de amostras.**
  - O `on_sent` marca o fim de um descritor: as amostras passaram para o FIFO do I²S, ainda não saíram do SU-2.
  - O enchimento contado pelos fins de DMA tem uma ambiguidade de ±1 descritor (o que está a tocar conta inteiro).
  - Depois do DMA ainda há o FIFO do I²S, o WM8805, a receção S/PDIF e o filtro do DAC.
  - **Usa-se a telemetria de DMA só para comparações relativas** (por exemplo, o ganho do L1) e para a folga.
- **Latência absoluta:** só fisicamente.
  - Impulso na entrada analógica do TX e captura da saída analógica no **mesmo relógio**: KA6 com loopback de
    referência e correlação cruzada (`latency_measure.py`), n ≥ 20.
  - Para incluir o SU-2, a saída analógica dele tem de voltar à KA6. Sem isso, usa-se a placa PCM5102A (mesmo playout)
    e soma-se o SU-2 à parte.
- **Variabilidade:** as medições têm de cobrir **mais de um ciclo do dente de serra** (≥ 15–20 min), com a ocupação do
  RX registada ao mesmo tempo. Uma rajada de 20 medições em 1 min só vê um ponto do ciclo.
- **Atraso de entrega por pacote:** a medida limpa é a da captura E-ATTR (instante ideal vs 1.ª tentativa vs ACK). A
  `cb_lat` por segundo é um proxy conservador: inclui retransmissões em que o RX já tinha recebido e só o ACK se
  perdeu.

## 8. Instrumento E-ATTR preparado (30/10, noite)

**Ferramentas:**
- `experiments/LAT-01/eattr_capture.sh <s>`, corrido com `sudo` pelo utilizador:
  - TX-only recorder ao mesmo tempo;
  - `en1` desligado do router próprio e posto no canal 6, com a rede do Mac pela Ethernet;
  - `tcpdump` em modo monitor (radiotap, 256 B por trama);
  - o Wi-Fi é sempre reposto no fim;
  - analisa logo a seguir.
- `experiments/LAT-01/eattr_analyze.py`: só biblioteca padrão.

**Separação explícita** (pedido do utilizador):

| Natureza | Conteúdo |
|---|---|
| Observado | Tentativas do TX (instante TSFT, bit R, MCS, sequência 802.11, `sample_ctr`), ACK para o TX, tramas de outros emissores (duração e NAV) |
| Inferido | Instante em que cada bloco ficou pronto no TX: envelope inferior de 1.a tentativa contra `sample_ctr` |
| Não observável | Fila interna do ESP32 e energia não descodificada |

**Regras de atribuição:**
- Um buraco na sequência 802.11 é falha do sniffer, não ausência de transmissão.
- Um pacote cuja 1.a tentativa captada já tem o bit R fica com o acesso "por medir".
- Um atraso de acesso só conta como canal ocupado se as tramas captadas de terceiros o cobrirem; senão fica **não
  explicado**.

**Validado numa captura sintética** com causas conhecidas: retransmissões, vizinho a ocupar 8 ms, 6 ms com o canal
livre, pacote não captado e 1.a tentativa não captada. Todas foram classificadas corretamente; a correlação com a
telemetria do TX encontra o desfasamento de +1 s.

**Critérios do ensaio de 10 s (o instrumento serve?):**
- V0: TSFT em ≥ 95 % das tramas;
- V1/V3: ≥ 95 % dos pacotes do TX captados;
- V2: canal 2437 MHz e MCS 4;
- V4: `sample_ctr` lido em ≥ 95 %;
- V5 (ACK visíveis) e V6 (terceiros visíveis): informativos.

Se falharem, o método abandona-se (ver a §6).

## 9. Ensaio de 10 s (30/10 23:05): o instrumento serve para a hipótese principal

**Montagem:**
- RX na bancada, com antena (sinal −13 dBm no Mac);
- TX intocado;
- telemetria do TX gravada ao mesmo tempo;
- Wi-Fi reposto no fim, sem processos restantes.

**Validação do instrumento:**

| Critério | Resultado | Estado |
|---|---|---|
| V0 TSFT | 100 % | ok |
| V2 canal e taxa | 2437 MHz, MCS 4 em 100 %, 860 B | ok |
| V4 `sample_ctr` | 100 % | ok |
| V5 ACK visíveis | 99,3 % | ok |
| V1/V3 cobertura | 94,8 % (175 pacotes não captados) | no limite |

**Notas sobre a validação:**
- Os pacotes não captados estão **espalhados**: 145 buracos, 128 de 1 pacote, nenhum maior que 5. Não há períodos de
  surdez do sniffer.
- O intervalo ACK − tentativa é de 226 µs (duração da trama + SIFS): o TSFT marca o início da trama, coerente com o
  modelo.
- O relógio do ADC visto pelo Mac está a +1 ppm do nominal.
- A correlação com a `cb_lat` do TX é positiva (ρ 0,50 em 10 s); medianas 3,9 ms (captura) e 4,8 ms (TX).

**Limitação confirmada:** o Mac a 20 MHz no canal 6 **não descodifica**:
- energia de canais sobrepostos (o AP vizinho V no 8, a −48 dBm);
- tramas de 40 MHz.

O CCA do TX deteta-as. Por isso o "não explicado" junta essa energia ao atraso interno do TX. **Não afeta a hipótese
principal:** as retransmissões veem-se diretamente pelo bit R.

**Primeira amostra** (máximo por segundo até 15 ms; no TX, p90 17 ms nesse minuto):
- 20 pacotes com mais de 5 ms;
- **retransmissões: 0 % do atraso**;
- fila atrás do pacote anterior: 60 %;
- atraso de acesso da 1.a tentativa: 38 %. Destes, 59 % com o canal visivelmente ocupado e o resto não explicado.
- Só 4 de 3213 pacotes tiveram a 1.a tentativa falhada, todos isolados.

**Leitura provisória:** neste minuto, a cauda não veio dos ACK, mas do acesso ao canal e da fila atrás de um pacote
atrasado. A hipótese Grok/Gemini fica **por confirmar e com sinal contra**.

**Sessão decisiva:**
- RX no sítio real (sala), num período mau;
- capturas de 120 s (`sudo .../eattr_capture.sh 120`, ~15 MB);
- **para quando houver ≥ 20 segundos com o máximo acima de 10 ms**, no máximo 3 capturas.

Decisão pelos critérios da §6.

## 10. Mecanismo provável da cauda (ensaio de 10 s; a confirmar na sala)

**O que se vê** (`eattr_analyze.py`, secção de episódios):
- As 6 cabeças de episódio esperaram **5–15 ms antes da 1.a tentativa** (mediana 10,1 ms). Retransmissões nossas: 0.
- Em 3 delas havia no ar um **comboio de respostas a pedidos de sonda do AP vizinho V**:
  - **agora no canal 6** (antes no 8 ou no 11), −48 dBm no Mac;
  - 2 a 4 tramas seguidas a **1 Mb/s**, de ~3,5 ms cada;
  - **89 % com o bit de retransmissão**;
  - disparadas por pedidos de sonda de telemóveis.

**Interpretação** (PROVÁVEL):
- O vizinho repete as respostas porque o telemóvel já mudou de canal e não confirma.
- O nosso TX espera pelo comboio inteiro (CCA) e os pacotes seguintes acumulam-se atrás.
- As outras 3 esperas ficam **não explicadas**: o Mac falha uma parte das tramas a 1 Mb/s (captou ~2/3 dos beacons
  esperados do AP V), e há energia de canais sobrepostos.
- Comboios > 5 ms: 0,33/s, mediana 10,9 ms, máximo 14,5 ms.

**Consequências:**
- **Um broadcast nosso esperaria exatamente o mesmo:** o ACK não está no caminho.
- O comprimento máximo de um comboio destes é físico: limite de retransmissões do vizinho × ~3,5 ms, até ~25 ms.
  Ajuda a dimensionar a margem.

## 11. Disco do Mac (só registo, sem ação)

- **30/10 23:10:** volume de dados a **98 %** (460 GB, **11 GB livres**).
- É o mesmo tipo de risco que parou o recorder de produção a 18/10.
- As ferramentas desta fase têm limites próprios:
  - `tx_session_rec.sh` recusa arrancar com < 5 GB livres e para nessa condição;
  - as capturas E-ATTR têm ~15 MB por 120 s.
- **Não se fez nenhuma limpeza nem alteração** (decisão do utilizador).

## 12. Redes de 2,4 GHz às 23:50 (cache do macOS, só leitura; lista possivelmente incompleta)

| Canal | Rede | Largura | Sinal |
|---|---|---|---|
| 1 | Router próprio (2,4 GHz) | 20 MHz | −42 dBm |
| 1 | Relógio próprio com ESP em modo de configuração (AP de setup), junto ao Mac | 20 MHz | **−18 dBm** |
| 7 | AP vizinho V | 20 MHz | −51 dBm |

- **O AP vizinho V muda de canal sozinho:** 8 e 11 a 29–30/10, recebido no 6 às 23:05, no 7 às 23:50.
- Qualquer canal fixo entre o 4 e o 13 fica sobreposto a ele em parte do tempo.
- A avaliação de canais tem de considerar este salto, e não só uma fotografia.
