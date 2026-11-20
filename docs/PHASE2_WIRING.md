# Passos 1 e 2 — ligação física e validação de ADC (TX) e DAC (RX)

Sistema base: **TX = S3 + PCM1808**, **RX = S3 + PCM5102A**. Sem hardware novo. Firmware: `audio/` (papéis `ADC_TEST` e `DAC_TEST`).
**Ordem atual (Psy): 1.º o DAC no RX (wiring mínimo + seno), 2.º o ADC no TX, 3.º ESP-NOW.**
Wiring mínimo do DAC: **VIN, GND, BCK, DIN, LCK e SCK→GND** (PLL interna). Pinos de controlo (FLT/DEMP/FMT/XSMT) **não ligam ao S3**: as pontes H1–H4 estão soldadas
(H1 FLT=L, H2 DEMP=L, H3 XSMT=H, H4 FMT=L, lidas de fotografia nítida) e a verificação **P2** com multímetro confirma-as antes de alimentar.
**Sem commit** até haver um estado funcional conhecido (decisão do Psy).

## Estado (2026-09-26)
- **DAC (RX) validado AUDITIVAMENTE:** com VIN a ~4,8 V (ponte IN-OUT fechada), o Psy ouve nos auscultadores (jack do módulo) o seno de ~1 kHz a passar de L para R. Isto valida: I²S do S3, PLL do PCM5102A a partir de BCK
  (SCK a GND), XSMT em "sem mute" (ponte H3), e ordem L/R correta. Relógio usado: `INT160_13` (13 0/2, MCLK 12,307692 MHz, fs ≈ 48076,9 Hz).
- **DAC medido pela Komplete Audio 6, entrada 1** (cabo TRS→TS, só o canal L; DAC com seno contínuo −20 dBFS, `INT160_13`), gravação CoreAudio sem perdas (100% dos frames, 0 overflows):
  **seno limpo, sem descontinuidades** (passo máx. entre amostras 0,054 vs 0,052 esperado); tom 996,8 Hz; **THD+N −56,7 dB re tom** com ruído+distorção −64,7 dBFS na banda 20 Hz–20 kHz;
  espúrios: 100/201/300 Hz (−70/−72/−76 dBFS, harmónicas de 50 Hz → **hum**) e 8000/16000 Hz (−78 dBFS).
  **Este valor NÃO caracteriza o DAC**: o ganho da entrada da interface não está registado, o piso da própria cadeia (interface + cabo TS não balanceado + massas partilhadas via USB do Mac) não foi medido, e o hum sugere **laço de massa** (HIPÓTESE, não verificada).
  Para atribuir ruído ao DAC falta: piso da interface com a entrada em curto e o DAC em silêncio (`tools/build_audio.sh DAC INT160_13 SILENCE`), com o mesmo ganho.
- **Lição de método (medida):** o `ffmpeg`/avfoundation **perde amostras sem avisar** (10,7 s gravados de 12 s pedidos; ~85 descontinuidades num seno limpo, que pareciam defeito do DAC). Com o mesmo DAC e cabo, a gravação por CoreAudio
  (`tools/record_audio.py`, que reporta overflow e frames recebidos) não tem nenhuma. **Medir só com `record_audio.py`.**
- Leituras A3V3/XSMT/BCK não reportadas (a função prova o essencial).
- **ADC (TX) validado — captura I²S com entrada em curto (LIN/RIN a GND), 2026-09-26** (S3 …64:1D:EC N16R8 + PCM1808; firmware `ADC_TEST`, `INT160_13`):
  dados reais (não zeros), **fs = 48076,92 Hz** em todas as leituras por segundo (= divisor inteiro `13 0/2`), **clip = 0**, L e R simétricos.
  **Assentamento:** RMS −56 → −86 dBFS e DC 0,128% → 0,001% FS ao longo de **~6 s** (filtro DC do ADC + condensadores de entrada) — **analisar só a partir dos 6 s** (`audio_analyze.py --skip 6`); os primeiros 500 ms descartados pelo firmware não chegam.
  **Estabilizado (4 s, entrada em curto):** DC +0,001% FS; RMS sem DC **−85,9 dBFS**; ruído+espúrios na banda 20 Hz–20 kHz **−86,4 dBFS**; espúrios **100 Hz −87,9 dBFS, 200 Hz −94,9, 301 Hz −100,9, 50 Hz −107,8** (L e R quase idênticos).
  **Leitura (HIPÓTESE, não isolada):** a potência dos espúrios (100+200+301 Hz ≈ −86,9 dBFS) explica quase todo o ruído medido; descontando-os o piso de banda larga fica ≈ **−95 dBFS**, coerente com um ADC de 99 dB SNR (ponderado). O **hum de 100 Hz** (2× rede) sugere ondulação na alimentação ou acoplamento — a fonte (USB do Mac via pino 5V do S3, massas partilhadas) **não foi isolada**; testar com +5V do módulo a partir de outra fonte é a experiência que separa.
- **Ligação final do TX (verificada):** SCK←GPIO4, BCK←GPIO5, LRC←GPIO6, OUT→GPIO7 (22–47 Ω em SCK e BCK); MD0/MD1/FMT e LIN/RIN a GND; GND comum; **+5V do pino 5V do S3 (4,8 V, ponte IN-OUT fechada)**;
  **P1: os pinos 3.3V do módulo dão 0,09 V sem alimentação ⇒ o módulo NÃO tem regulador de 3,3 V e é o S3 que fornece o VDD** (3V3 do S3 → 3.3V do módulo; ambos os pinos medem 3,3 V depois).
- Firmware compila (ADC e DAC × 3 modos de relógio), sem warnings.
- Gravação pela interface: **usar `tools/record_audio.py`** (CoreAudio, com deteção de perdas); `record_audio.sh` (ffmpeg) fica só para verificações rápidas, **não para medir**. Permissão de microfone concedida.

## ADC — teste de sinal real: LIN e RIN validados (RIN após reparação)
Fonte: saída de phones da Komplete Audio 6 (botão fixo), tom de 1 kHz por `tools/adc_level_test.py` (firmware `ADC_TEST` em modo MONITOR), com guarda automática (para o tom se o pico do ADC ≥ −12 dBFS ou clip).
- **LIN (canal L) — VALIDADO:** resposta **linear, inclinação 1,000 dB/dB, desvio máx. 0,07 dB** em 8 pontos de −35 a 0 dBFS digitais; ganho digital→ADC constante **−31,6 dB** (inclui botão, interface e módulo); seno puro (crista ≈ 0,1 dB a níveis altos);
  **sem clipping** (pico máx. do ADC ≈ −31,6 dBFS, > 19 dB abaixo do limite da guarda); canal R sem sinal permaneceu no piso (−85,9…−85,7 dBFS) ⇒ **canal correto**. Repetido com ponte LIN↔RIN: L outra vez −31,4 dBFS a 0 dBFS digitais.
- **Nota de método:** numa rampa anterior o nível saltou 30 dB entre dois degraus; a causa mais provável (não confirmada) é o **botão da KA6 a mexer** durante a corrida. O potenciómetro dos phones da KA6 está **estragado** (estoura e liga/desliga as vias R/L, segundo o Psy): **não é uma fonte fiável**. Não comparar runs onde o botão possa ter mexido.
- **RIN — dano original:** sem resposta ao tom em 3 corridas (incl. com ponte LIN↔RIN, R plano em −85,9 dBFS). Continuidades medidas pelo Psy (sem alimentação): **LIN ↔ C1-A apita; RIN ↔ C2-A e C2-B NÃO apitam** ⇒ o furo RIN não estava ligado ao condensador de entrada do R. A pastilha do furo RIN escurecida/estragada e sem aceitar solda. Lado digital do R sempre bom (ruído limpo, igual ao L em curto). **Não foi defeito de projeto.**
- **Reparação (2026-09-29):** o Psy identificou por continuidade o terminal do C2 do lado da entrada (o que ia para a pastilha RIN queimada) e soldou aí diretamente o fio do sinal (ponta do jack; a blindagem a GND), contornando a pastilha danificada. Módulo sem alimentação durante a soldadura.
- **RIN — VALIDADO (2026-09-29), após reparação:** 1.ª corrida (botão dos phones mexido pouco antes) deu uma subida lenta e não-linear ao longo de vários segundos com o nível digital parado — **não é comportamento de canal saudável**, consistente com o potenciómetro da KA6 (já conhecido como defeituoso, ver nota acima) ainda a assentar, não com a solda. Repetido com o **botão fixo, sem tocar durante a corrida**: resposta **imediata e linear, declive ≈1,00 dB/dB** (desvios 3,8–4,0 dB por degraus de 4 dB, entre −40 e −20 dBFS digitais), **sem clipping em nenhum segundo**. Critério igual ao do L, cumprido.
  **Ressalva em aberto:** piso do R em silêncio ≈ −78,7 dBFS RMS, ~8 dB acima do piso do L (−86,6 dBFS). Canal funcional e linear, mas mais ruidoso — fica registado para a fase de qualidade analógica (passo 7), não bloqueia os passos seguintes.
- **Decisão:** seguir com este módulo reparado (LIN e RIN estéreo). Um 2.º módulo PCM1808 fica como sobressalente a encomendar, não como bloqueio.
- **Estado do ADC + I²S:** **captura I²S validada; canal L (LIN) validado com sinal real; R pendente por dano físico do módulo.**

## Decisão de bancada (Psy, 2026-09-26): TX e RX eletricamente independentes
- A S3 **…64:04:2C** (antiga TX) **fica como spare** para testes que não usem o pino 5V: arranca normal, 3V3 = 3,3 V, mas o pino **5V lê ~2 V** (ponte IN-OUT danificada/não conduz). **Não voltar a soldar nessa ponte; também não soldar já na nova S3.**
- **A TX passa a ser outra S3 (ainda por identificar).** **RX = …58:C7:E4** (DAC; ponte IN-OUT fechada, pino 5V ≈ 4,8 V).
- **PCM1808 com alimentação independente da ponte IN-OUT:** +5V de uma **fonte USB/powerbank separada**; **GND da fonte = GND do PCM1808 = GND do S3** (massa comum); a S3 continua alimentada por USB normal.
  Sem alimentação cruzada com a RX. Cuidados: (a) com um cabo USB cortado/breakout, **medir a polaridade com o multímetro antes de ligar ao módulo** (polaridade invertida destrói-o); (b) preferir **carregador de parede** a powerbank (algumas powerbanks desligam com consumos pequenos);
  (c) **ligar primeiro o USB da S3 e só depois a fonte do módulo** (evita o DOUT do módulo a alimentar o S3 pelos díodos do GPIO).
- Sequência acordada: (1) enumeração USB da nova S3 · (2) 3V3 = 3,3 V · (3) firmware ADC (com os fios de sinal **ainda desligados**) · (4) PCM1808 alimentado só com +5V/GND (MD0/MD1/FMT e LIN/RIN a GND, sinais desligados) ·
  (5) **P1**: medir os pinos 3.3V (≈3,3 V → tem regulador, não ligar o 3V3 do S3; ≈0 V → avaliar) · (6) **desligar tudo**, ligar SCK/BCK/LRC/OUT (22–47 Ω em SCK e BCK) e só então clocks e captura I²S.

## Particularidade da placa S3 do Psy (medida em 2026-09-26): pino 5V
Placa com dois USB-C (COM e USB) e uma ponte de solda **IN-OUT**. Com a ponte **aberta**, o pino **5V lê ≈0,19 V** (só USB nativo), **1,8 V** com COM+USB ligados; o pino 3V3 lê 3,3 V.
Com a ponte **IN-OUT fechada** o pino **5V lê 4,8 V** (VBUS através do díodo em curto). Sem esta ponte o VIN do PCM5102A ficava a ≈0,09 V (módulo sem alimentação) — foi esta a causa de "sem som".
Consequência: **nunca alimentar o pino 5V com uma fonte externa enquanto o USB estiver ligado e a ponte fechada** (retroalimentação). Fonte: comportamento descrito para clones da DevKitC-1 em fóruns
([HA Community](https://community.home-assistant.io/t/how-to-enable-output-on-5v-pin-on-knockoff-esp32-s3-devkitc-1-boards/734727), [Arduino Forum](https://forum.arduino.cc/t/chinese-esp32-s3-5v-pin-warning/1192758)); o esquema exato da placa do Psy não foi consultado.
Leituras já feitas no DAC antes da ponte: **LCK = 1,65 V** (relógio a chegar), **DIN = 0,65 V** (dados presentes), VIN e XSMT = 0,09 V (módulo sem alimentação, não diz nada sobre H3). BCK ainda por medir.

## Módulos (serigrafia confirmada pelo Psy/fotos)

### PCM1808 (ADC) — serigrafia indicada pelo Psy
Topo: **BCK, OUT, LRC, SCK, GND, 3.3V** · Baixo: **FMT, MD1, MD0, GND, 3.3V, +5V** · Analógico: **LIN, GND, RIN** (3 furos; **condensadores de 10 µF nas entradas**).
- Mapeamento para o chip: SCK = **SCKI**, OUT = **DOUT**, LRC = **LRCK**. VCC analógico = **+5V**; VDD digital = **3.3V**.
- **Divisor/atenuação na entrada: NÃO confirmado.** Os 10 µF só bloqueiam DC (com 60 kΩ de entrada ⇒ corte ≈ 0,27 Hz). Não assumir que aguenta 2 Vrms: **começar com nível reduzido e medir** (fundo de escala = 3 Vp-p ≈ 1,06 Vrms).
- **Incerteza a resolver antes de alimentar:** os pinos "3.3V" (há dois) são **entrada ou saída de um regulador do módulo?** Ver verificação P1.

### PCM5102A (DAC) — das fotos
Cabeçalho digital (6 pinos): **SCK, BCK, DIN, LCK (=LRCK), GND, VIN**.
Fila de controlo/analógico (9 furos, no verso): **LROUT, AGND, ROUT, AGND, A3V3, FMT, XSMT, DEMP, FLT**; quatro pontes de solda **H1L, H2L, H3L, H4L** (pelos números 1–4 na frente: **H1 = FLT, H2 = DEMP, H3 = XSMT, H4 = FMT**).
Duas pastilhas junto a SCK na frente (provável ponte SCK→GND).
- **Não se vê nas fotos em que lado cada ponte está soldada.** Determina-se por continuidade (verificação P2). Configuração pretendida: **FLT=L, DEMP=L, FMT=L (I²S), XSMT=H (sem mute)**, SCK=GND (modo PLL a partir de BCK).
- Duas placas de regulador visíveis ⇒ **VIN provavelmente aceita 5 V**; não confirmado → verificação P2 (medir A3V3).
- Os 4 furos superiores da coluna esquerda na frente (L, G, R, G) são as saídas analógicas (= LROUT, AGND, ROUT, AGND do verso) e há uma ficha de 3,5 mm.

## Verificado nos datasheets (TI) — o que a ligação tem de respeitar
**PCM1808** ([datasheet](https://www.ti.com/lit/ds/symlink/pcm1808.pdf)): **MD1=MD0=GND → slave**; **MD têm de estar definidos antes de ligar a alimentação**; **FMT=GND → I²S 24 bit**; slave exige **64 BCK/frame** e LRCK/SCKI sincronizados (o S3 gera os três da mesma cadeia); MD/FMT/SCKI toleram 5 V e têm pull-down interno de 50 kΩ; dados válidos só ~187 ms após o arranque do relógio (o firmware descarta 500 ms).
**PCM5102A** ([família PCM510x](https://www.ti.com/lit/gpn/PCM5102) — confirmar variante "A"): **SCK=GND → PLL interna a partir de BCK** (BCK = 32 ou 64·fs); FMT baixo = I²S; FLT baixo = latência normal; DEMP baixo = off; **XSMT alto = sem mute** (rampa de 104 amostras; tr/tf < 20 ns).

## Pinout do S3 (igual nas duas placas)
Evitados: 0, 3, 45, 46 (strapping); 19/20 (USB); 26–37 (flash/PSRAM octal); 43/44 (UART0); 48 (LED).

| Sinal | GPIO | TX → PCM1808 | RX → PCM5102A |
|---|---|---|---|
| MCLK | **4** | → **SCK** | não usado (SCK do DAC a GND) |
| BCLK | **5** | → **BCK** | → **BCK** |
| WS/LRCK | **6** | → **LRC** | → **LCK** |
| DADOS | **7** | ← **OUT** | → **DIN** |
| XSMT | — | — | **NÃO ligar ao S3** (ver abaixo) |

Fios curtos (< 10 cm); **resistência série 22–47 Ω** em MCLK e BCLK junto ao S3; massa comum de baixa impedância.

## TX — ligações PCM1808 (com o módulo **sem alimentação**)
| Pino do módulo | Liga a |
|---|---|
| SCK | S3 GPIO 4 (via 22–47 Ω) |
| BCK | S3 GPIO 5 (via 22–47 Ω) |
| LRC | S3 GPIO 6 |
| OUT | S3 GPIO 7 |
| MD0, MD1, FMT | **GND** (fio a cada um, ao pino GND adjacente) — **antes de alimentar** |
| GND (qualquer) | GND do S3 |
| +5V | pino 5V do S3 (VBUS do USB) |
| 3.3V | **depende de P1** |
| LIN / GND / RIN | por agora **em curto a GND** (LIN e RIN ligados ao GND) |

**P1 — verificação dos pinos 3.3V (antes de ligar os sinais):** ligar **só** GND e +5V, sem mais nada. Com multímetro medir a tensão nos pinos **3.3V**.
- Se ≈ **3,3 V** ⇒ o módulo tem regulador e o pino é **saída**: **não** ligar o 3V3 do S3 lá (dois reguladores em conflito).
- Se ≈ **0 V** ⇒ é **entrada**: ligar ao pino 3V3 do S3.
Só depois ligar SCK/BCK/LRC/OUT.

## RX — ligações PCM5102A (com o módulo **sem alimentação**)
| Pino do módulo | Liga a |
|---|---|
| BCK | S3 GPIO 5 (via 22–47 Ω) |
| LCK | S3 GPIO 6 |
| DIN | S3 GPIO 7 |
| GND | GND do S3 |
| VIN | pino 5V do S3 (**só depois de P2**) |
| SCK | **GND**, se a ponte junto a SCK não o fizer (P2) |
| XSMT | **não ligar a nenhum GPIO** (estado dado pela ponte H3L) |
| LROUT / ROUT / AGND | → entrada de linha da interface de áudio / amplificador (nunca diretamente a colunas) |

**P2 — verificações de continuidade (módulo SEM alimentação), com multímetro em continuidade/ohms:**
1. Entre o pino **SCK** e **GND**: se houver continuidade a ponte SCK→GND está feita (nada a fazer); se não, ligar SCK a GND com um fio.
2. Entre cada pino **FLT, DEMP, FMT, XSMT** (fila de 9 furos) e **AGND**, e entre cada um e **A3V3**. Configuração pretendida:
   **FLT→GND ou 3,3 V (qualquer)**, **DEMP→GND**, **FMT→GND**, **XSMT→3,3 V (A3V3)**.
   **Se XSMT estiver ligado a GND (H3L soldado a L), o DAC fica em mute permanente** e é preciso mudar a ponte H3 para o lado H.
   Se algum pino ainda mostrar o lado errado, corrigir a ponte de solda correspondente antes de continuar.
3. Alimentar **VIN com 5 V** (só GND e VIN) e medir **A3V3 ≈ 3,3 V** antes de ligar BCK/LCK/DIN. Se não der ~3,3 V, desligar.

## Procedimento — passo 2 (validar captura no TX)
1. Ligações do TX acima, entradas em curto. **MD0/MD1/FMT a GND antes de alimentar.** Fazer P1.
2. Flashar o ADC: `tools/build_audio.sh ADC INT160_13` e o comando `flash:` que ele imprime (na porta da placa TX).
3. Esperado nos `ADCSTAT` (1/s): `fs` ≈ 48076,9; `dc` pequeno; `rms` (ruído) baixo; **`clip=0`**. O dump (`ADCDUMP_BEGIN…END`) analisa-se com
   `.venv/bin/python tools/audio_analyze.py captura.raw.txt --wav saida.wav` → **piso de ruído do ADC com a entrada em curto**.
4. Só depois ligar uma fonte: **volume da fonte baixo**, ler `L_peak/R_peak` e subir devagar até pico ≈ −12…−6 dBFS. Nunca ultrapassar 3 Vp-p. **Não assumir que 2 Vrms cabem.**
5. Diagnóstico: **tudo zero** → MD/SCK/BCK/LRC (MD definido antes do arranque; frequências); **dados ruidosos/deslocados** → FMT/64 BCK; **L/R trocados** → ordem no módulo.

## Procedimento — passo 4 (validar reprodução no RX)
1. Ligações do RX acima; P2 concluída; saída → entrada de linha da interface.
2. Flashar o DAC: `tools/build_audio.sh DAC INT160_13`. Emite seno 997 Hz a −20 dBFS, **1 s no esquerdo, 1 s no direito**.
3. Se silêncio: PLL do DAC não bloqueou (SCK a GND? BCK a chegar? modo de relógio) **ou XSMT em mute** (P2). Gravar na interface e correr `audio_analyze.py` (aceita WAV): tom, THD+N, espúrios.
   **Medir primeiro o piso da própria interface** (loopback interface→interface).

## Relógio: comparar POR MEDIÇÃO (critério = desempenho real, não "divisor inteiro = melhor")
Candidatos (o divisor real vem do log de debug do driver, não é suposição):

| Modo | Divisor real | MCLK | fs |
|---|---|---|---|
| `NOMINAL_48K` | `13 1/48` (fracionário) | 12,288 MHz (média) | 48000 Hz |
| `INT160_13` | `13 0/2` (inteiro) | 12,307692 MHz | 48076,9 Hz |
| `INT240_20` (só se tiver utilidade) | `20 0/2` (inteiro) | 12,000000 MHz | 46875 Hz |

`INT160_13` é **só o modo de arranque dos testes**, não uma decisão. Protocolo (mesmas condições nos 3): (a) ADC com entrada em curto → piso e espetro; (b) ADC com o mesmo tom → THD+N, espúrios e bandas laterais;
(c) DAC a emitir o mesmo tom → gravação na interface; (d) repetir ≥ 3 vezes para ver a variação entre capturas. Sem mais otimização de relógio por agora. Variantes: `tools/build_audio.sh <ADC|DAC> <MODO>`.

## Módulos extra
Só entram se ajudarem num teste concreto (ex.: 2.º PCM5102A como referência). Neste momento **não** são usados.

## Dependências do analisador
`python3.11 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt` (numpy). Convenção AES17: 0 dBFS = seno de amplitude full-scale.

## Gravar a saída do DAC no Mac (Komplete Audio 6)
`.venv/bin/python tools/record_audio.py saida.wav 12 [canal_esq=1] [canal_dir=1]` → grava por CoreAudio, imprime `frames recebidos … overflow=…` (**rejeitar qualquer gravação com overflow ou frames em falta**) e escreve WAV 24 bit.
Depois `.venv/bin/python tools/audio_analyze.py saida.wav`. A primeira gravação exige permissão de MICROFONE para a app que corre o comando.
- Para **medir** usar o DAC com **seno contínuo nos dois canais**: `tools/build_audio.sh DAC INT160_13 BOTH`; o modo ALT (1 s esq / 1 s dir) serve só para verificar L/R.
- O DAC emite a ~48077 Hz e a interface grava a 48000 Hz (relógios independentes): o tom aparece ~0,16% mais grave; não afeta ruído/espúrios.
- Entrada 1 da Komplete Audio 6 em LINE, ganho mínimo, phantom desligado; **registar o ganho de cada medição** (muda o piso). Cabo TRS→TS **só leva L** (não soma canais).
