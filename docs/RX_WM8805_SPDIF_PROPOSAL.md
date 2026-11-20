# RX AudioLink com saída S/PDIF (WM8805 → SMSL SU-2): auditoria e proposta (2026-10-29)

**Estado:** auditoria e proposta para decisão. **Nada foi implementado, gravado ou alterado** (firmware, nós, hardware,
Orange). Só leitura do repositório.

**Etiquetas:** CONFIRMADO / FORTEMENTE SUPORTADO / PROVÁVEL / HIPÓTESE / DESCONHECIDO.

**Limite desta auditoria:** tudo o que aqui está vem do repositório (código, releases, relatórios, docs). O estado
*atual* dos aparelhos (que firmware corre hoje no ESP do AudioCast, se o SU-2 está no coaxial, onde estão fisicamente)
**não foi observado**: é estado documentado, não estado real.

---

## 0. Veredicto curto

1. **Viável: PROVÁVEL.** O RX AudioLink já tem as propriedades certas para alimentar um recetor S/PDIF: é mestre do
   relógio I²S e **nunca para o I²S** (silêncio em apagão). Essa era precisamente a fraqueza do AudioCast.
2. **A hipótese "muda só o backend I²S" é verdadeira para o código, mas falsa para o sistema.** O diff de firmware é
   pequeno (pinos + MCLK). Há, porém, pelo menos cinco mudanças que não são de backend e qualquer uma pode fazer falhar a
   migração sem que o backend tenha culpa:
   - a taxa de 48 076,9 Hz posta num link S/PDIF;
   - um relógio novo de 12,3 MHz a sair do RX;
   - outro cristal (drift face ao TX contra um corretor de 8,3 ppm);
   - o RX longe da consola USB, o que tira a observabilidade;
   - o emparelhamento por MAC fixo no TX.
3. **Recomendação:** fica o **mesmo código de RX da Beta 1 (`link_rx.c` do ramo `audiolink-beta1-led`)**, com um
   **perfil de saída escolhido em compilação** (PCM5102A | WM8805). A validação é por etapas, cada uma a isolar uma
   variável:
   - primeiro o backend sem rádio (tom do `dac_test`);
   - depois o link completo na bancada;
   - por fim a sala.

   Não se junta isto ao RX v2. Não se escreve driver I²C para o WM8805 sem provar que é preciso.
4. **Primeiro passo:** identificar a placa WM8805 e de onde vem o relógio do seu transmissor (§7, W0), sem gravar nada.
   Sem isso não se pode desenhar nada com confiança.

---

## 1. Estado real reconstruído

### 1.1 AudioCast (o nó a reutilizar)

| Facto | Fonte | Etiqueta |
|---|---|---|
| Firmware = fork do snapclient (CarlosDerSeher) em `esp/cumestack-snapclient/snapclient`, IDF 5.4.4 | sdkconfig do release, git | CONFIRMADO (repo) |
| Release de produção documentada: `H1-D1.16B-perbyte`, commit `e32abc7`, app `93c24966…` | `release/H1-D1.16B-perbyte/` | CONFIRMADO (repo); **o que corre hoje no ESP: DESCONHECIDO** |
| I²S0 **mestre**, Philips, 48 kHz, **16 bit**, MCLK 256·fs, `clk_src = DEFAULT` (PLL160 → 48 kHz **fracionário**), `auto_clear = true` | `player.c`, relatório H1-D1.3 | CONFIRMADO |
| Pinos: **MCLK 18, BCK 16, LRCK 8, DOUT 15** | `TESTED-RELEASE-ARTIFACT/sdkconfig` | CONFIRMADO (config); cablagem física: FORTEMENTE SUPORTADO (funcionou) |
| Perfil de placa `DAC_PCM5102A` → **o firmware nunca configurou o WM8805** (I²C 21/22 declarados, sem driver) | sdkconfig | FORTEMENTE SUPORTADO |
| Flash usada como **4 MB**, tabela OTA (nvs 80K, otadata, ota_0/ota_1); PSRAM octal configurada e a funcionar | `partitions.csv`, sdkconfig | CONFIRMADO (config); tamanho físico da flash **DESCONHECIDO** |
| Placa WM8805: modelo, cristal, MCU/jumpers, modo de controlo, origem do relógio do TX S/PDIF | — | **DESCONHECIDO** (não há registo no repo) |
| Problemas resolvidos: janela TCP subdimensionada (32 KB/25), stack do `player_task`, dependência de temporização mitigada com `pd_diag_note_byte_progress()` (mecanismo não caracterizado) | `RELEASE_NOTES.md`, `KNOWN_LIMITATIONS.md` | CONFIRMADO |
| **Ciclo stop/start do I²S**: 2 s sem chunks destrói player/fila/**I²S** e recria-o (2338 stops em ~1,6 h no baseline S2-A) | relatório S2-A | CONFIRMADO |
| **Clicks ocasionais: causa nunca confirmada**; "MCLK/I²S, WM8805, coaxial ou SMSL" explicitamente **não excluídos** | S2-A §1, linha 252 | CONFIRMADO (que ficou em aberto) |
| Dependências no resto da infra: Orange `snapserver` + `audiocast-web :8090`, monitor NanoPi (F1h), `cs-validate-boot.sh` | `ORANGE_RUNTIME_HARDENING.md` | CONFIRMADO (docs) |

### 1.2 AudioLink RX (a base)

| Facto | Fonte | Etiqueta |
|---|---|---|
| Em uso: Beta 1 + LED, RX `86b2f8aaf`, ramo `audiolink-beta1-led` (`d29d57c`); TX v2 `37b588a01` | `release/BETA1-LED/` | CONFIRMADO (repo, 14/10) |
| I²S0 **mestre**, Philips, **slots de 32 bit** (24 bit úteis alinhados à esquerda), BCK = 64·fs, **sem MCLK** (PCM5102A em PLL a partir do BCK) | `link_rx.c:1057-1071` | CONFIRMADO |
| Relógio `INT160_13`: 160 MHz / (256·13) = **48 076,92 Hz**, divisor **inteiro**; TX à mesma taxa nominal | `audio_common.c`, overlays | CONFIRMADO |
| O I²S **nunca para**: rebuffer/arranque/apagão = silêncio no DMA, `expected_ctr` congelado | `link_rx.c` (cabeçalho) | CONFIRMADO |
| DMA 8×128 (~19 ms), alvo 576 tramas, slip ±1 trama, **capacidade do corretor ≈ 8,3 ppm**; par atual ≈ 6 ppm (57–78 % da capacidade) | `ARCH_REVIEW_2026-10-07.md` §9 | CONFIRMADO |
| Dívida aceite: ~0,167 `late_drop`/s (cada um = repetição de bloco de 2,66 ms); dente de serra do slip (artefacto −36…−56 dBFS) | Beta 1, `EXP-OFFLINE-SLIP-QUALITY-01` | CONFIRMADO |
| PSRAM **obrigatória** (bitmap `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` → `abort()`); imagem a 16 MB, single-app | `link_rx.c:1040`, `RX/flash_args` | CONFIRMADO |
| Emparelhamento fixo: RX aceita só `28:84:85:64:1D:EC`; o **TX envia unicast para `28:84:85:58:C7:E4`** | `RX/overlay.ov`, `TX/peer.ov` | CONFIRMADO |
| Observabilidade = consolas USB do TX e do RX lidas no Mac (`alfr`) | `FLIGHT_RECORDER.md` | CONFIRMADO |
| LED WS2812 em GPIO 48 (placas YD-ESP32-S3) | `LED_STATUS.md` | CONFIRMADO para as placas atuais; **DESCONHECIDO** para a placa do AudioCast |

---

## 2. WM8805/S/PDIF face ao PCM5102A: diferenças relevantes

| Aspeto | PCM5102A (hoje) | WM8805 → SU-2 | Consequência |
|---|---|---|---|
| Relógio do conversor | PLL interna a partir do BCK; MCLK desnecessário | **Corrigido pelo datasheet (§9):** com dados do I²S (`TXSRC = 1`), o TX S/PDIF é relogiado **pelo sinal no pino MCLK**. Em escravo esse pino é entrada (o MCLK do ESP). O cristal só serve o recetor/PLL | Um só domínio de relógio, o do ESP: FORTEMENTE SUPORTADO. Só falta confirmar na foto que o GPIO 18 chega ao pino 20 (MCLK) |
| Taxa | Qualquer (o PCM5102A segue o BCK) | O WM8805 não tem problema com 48 076,9 Hz (§9). O recetor do SU-2 (ES9039Q2M; chip recetor S/PDIF não identificado) extrai o relógio do S/PDIF. 48 076,9 Hz = **+1603 ppm**, fora do Nível II da IEC 60958 (±1000 ppm, norma de memória) | **Risco a testar, não problema demonstrado** (§9.3). Se falhar, a correção **não está no RX**: obriga a 48 000 em TX e RX |
| Formato/palavra | Slot 32, 24 bit úteis | Em modo hardware, só 4 formatos fixos por pinos: I²S 16, I²S 24, LJ 24 com flags, RJ 16 (Tabela 16). O AudioCast mandava 16 bit em slots de 16 (BCK 32·fs), o que só cumpre a regra "≥ 2 × palavra BCLKs por LRCLK" com palavra de 16 bit | 64·fs serve qualquer modo (CONFIRMADO, datasheet). Se a placa estiver em I²S 16 (PROVÁVEL, §9.2), há truncatura silenciosa para 16 bit sem dither |
| Mute | XSMT (aqui −1, ponte H3L) | Sem pino de mute; o SU-2 faz mute quando perde o lock | Arranque/reset do RX = queda do S/PDIF → o SU-2 desbloqueia e volta a trancar (centenas de ms, tipicamente sem estalo). Silêncio digital contínuo mantém o SU-2 trancado, o que pode impedir uma seleção automática de entrada, se o SU-2 a tiver (DESCONHECIDO) |
| Jitter | O PLL do PCM5102A filtra o BCK | A PLL do recetor do SU-2 filtra o jitter do S/PDIF. Com `INT160_13` o MCLK é **divisor inteiro** (160/13 = 12,3077 MHz): o caso mais limpo possível no S3. Um 48 000 exato seria fracionário (13 + 1/48) | Mais um argumento para não ir para 48 000 sem necessidade |
| Revelação de artefactos | PCM5102A → KA6 (bancada) | SU-2 → Genelec 8020 (sala; contexto do P23-A) | Os `late_drop` (~0,167/s) e o slip passam a ser muito mais audíveis. Os critérios de aceitação não podem ser só "toca" |

---

## 3. A hipótese "muda essencialmente o backend de saída I²S": análise crítica

**O que é verdade (FORTEMENTE SUPORTADO):**
- No código, a saída está isolada num só bloco (`link_rx.c:1057-1071`, e o mesmo em `dac_test.c`).
- O playout, a fila, o DMA, o slip e a sessão não dependem do conversor.
- O diff mínimo é este:
  - `gpio_cfg.mclk` passa a ser um pino configurável;
  - os pinos passam a 16/8/15 (+18);
  - XSMT fica a −1;
  - o LED fica configurável.

**O que a hipótese esconde.** Cada ponto tem a sua etiqueta e nenhum se resolve no backend:

1. **Domínio de relógio do WM8805: risco BAIXO** (revisto em 29/10 com o datasheet, §9). Com dados do I²S, o TX
   S/PDIF é relogiado pelo MCLK do ESP. Os clicks do AudioCast deixam de ter aqui uma explicação natural.
2. **48 076,9 Hz no S/PDIF: risco a testar só no SU-2** (o WM8805 aceita, §9.3). Se o SU-2 a rejeitar, a mudança deixa
   de ser só do RX: passa a tocar o TX (já validado nos gates E1–E4b) ou a exigir ASRC.
3. **MCLK novo de 12,3 MHz a sair do RX: HIPÓTESE de risco RF.** O NOPINS-01 mostrou, numa sessão, efeito causal forte
   da atividade I²S externa na degradação do rádio **do TX**. O RX nunca emitiu MCLK. Não há evidência de que isto
   aconteça no RX, mas há um precedente que obriga a medir.
4. **Outro cristal: risco médio, mensurável.** A placa do AudioCast tem um desvio de frequência desconhecido face ao TX.
   O corretor atual cobre ~8,3 ppm e o par atual já usa até 78 %. Um par a > 8 ppm satura o corretor: o RX entra em
   rebuffers ou `late_drop` crónicos, e o backend fica com a culpa.
5. **Observabilidade: risco estrutural.** Na sala, o RX deixa de ter consola USB para o Mac e o `alfr` deixa de o ver.
   A razão principal para preferir o AudioLink desaparece exatamente onde ele vai funcionar.
6. **Emparelhamento:** o TX envia unicast para o MAC do RX atual. Um RX noutra placa obriga a **gravar um TX novo**
   (só muda o `peer.ov`), ou o RX atual sai de serviço. Por desenho, só um RX de cada vez (1 TX + 1 RX).
7. **Placa diferente:** o tamanho da flash é desconhecido (a imagem do AudioLink diz 16 MB; se a flash física for de
   4 MB, o arranque falha). A PSRAM octal é obrigatória (FORTEMENTE SUPORTADO que existe). O GPIO 48 tem uso
   desconhecido. A tabela de partições é diferente, logo o rollback não é só gravar a app.
8. **Posição física:** a distância e as paredes entre o TX (junto ao Mac) e a sala são novas. Na Fase 1 mediu-se até
   ~13 m. O orçamento de `late_drop` muda com a posição, não com o backend.
9. **Função:** o AudioCast toca fluxos de rede (Snapcast do Orange). O AudioLink toca o que entra no PCM1808 do TX
   (hoje, os phones da KA6). Isto **troca a fonte de programa do SU-2** e passa-o por um ADC de qualidade modesta
   (piso −78/−87 dBFS). É uma decisão de produto, não técnica, e tem de ser explícita.

**Conclusão:** como hipótese de *trabalho de código*, está certa. Como hipótese de *trabalho de sistema*, está
incompleta. Os pontos 1, 2 e 4 podem fazer falhar a migração mesmo com um backend perfeito.

---

## 4. Alternativas consideradas

| # | Alternativa | Avaliação |
|---|---|---|
| **A** | **RX da Beta 1 + perfil de saída em compilação (PCM5102A \| WM8805), ESP mestre, MCLK 256·fs inteiro, 48 076,9 Hz** | **Recomendada.** Diff mínimo, mesma base medida (25 h de Beta 1), compara 1:1 com o RX atual. O build PCM5102A tem de continuar idêntico em objeto |
| B | A, mas a 48 000 Hz em todo o sistema (TX + RX) | Só se o SU-2 rejeitar 48 076,9 Hz (W1). Toca no TX validado e põe o MCLK fracionário. Plano B |
| C | WM8805 como mestre I²S (cristal próprio), RX escravo | Rejeitada agora: o RX consumiria a 48 000 exatos contra um TX a 48 076,9 (1600 ppm), o que exigiria ASRC. Só faria sentido com B e se um dia o jitter medido o justificar |
| D | ASRC no RX (48 076,9 → 48 000) | Rejeitada: CPU, qualidade e complexidade. Contraria a decisão de 07/10 de não assumir ASRC |
| E | RX v2 (playout novo) já com saída WM8805 | Rejeitada para já: muda playout e saída ao mesmo tempo, contra a regra "uma variável de cada vez". O RX v2 herda o perfil de saída depois |
| F | S/PDIF nativo do S3 (BMC por software no I²S) sem WM8805 | Rejeitada: exige um andar de saída novo (hardware) e CPU no caminho RT, sem ganho demonstrado |
| G | Driver I²C para configurar o WM8805 à partida | Só se o W0/W1 mostrar que a placa não funciona sem ele (não há MCU, ou a palavra está fixa em 16 bit). Não se escreve por antecipação |
| H | Manter o AudioCast e não migrar | Legítima: o AudioCast dá streaming de rede, o AudioLink dá a captura analógica do TX. A decisão é do utilizador (§3.9) |

---

## 5. Solução recomendada (A), em concreto

**Princípio:** o caminho de tempo real não muda. Muda só a configuração do canal I²S e os pinos, escolhidos em
compilação. Não se cria uma camada de abstração em runtime: não há um segundo backend em runtime que a justifique.

1. **Kconfig** (`audio/main/Kconfig.projbuild`):
   - `AUDIOLINK_PIN_MCLK_OUT` (int, default **−1**): pino de saída do MCLK; −1 = não emitido (comportamento atual).
   - Pinos BCK/WS/DATA e XSMT já são Kconfig.
   - LED: o pino do `al_led` passa a configurável, com −1 = desligado. Fica desligado na placa do AudioCast até se
     saber o que está no GPIO 48.
2. **Código:** em `link_rx.c` e `dac_test.c`, `.mclk = (CONFIG_AUDIOLINK_PIN_MCLK_OUT >= 0) ? CONFIG_… : I2S_GPIO_UNUSED`.
   Mais nada.
3. **Overlay da placa** (`build_LINK_RX_WM8805.ov`): MCLK 18, BCK 16, WS 8, DATA 15, XSMT −1, LED −1, `INT160_13`,
   `PEER_MAC` do TX, mais flash/PSRAM conforme o W0.
4. **Prova de não-regressão:** o build PCM5102A (overlay da Beta 1) tem de dar **objetos idênticos** aos da
   `86b2f8aaf`, exceto a versão. É o mesmo método de auditoria de objetos usado nos E2→E4b.
5. **TX:** o binário do BETA1-LED recompilado só com `peer.ov` = MAC STA da placa do AudioCast. O resto fica
   byte-idêntico; auditoria de objetos igual.
6. **WM8805:** sem driver. Se o W1 mostrar que é preciso, vem como passo separado (G), com proposta própria.

---

## 6. Riscos e lições do AudioCast

| Área | Risco | Mitigação / como se mede |
|---|---|---|
| Relógios | Domínio duplo dentro do WM8805 (§3.1): baixo, pelo datasheet (§9) | W0: foto confirma GPIO 18 → pino MCLK e o estado escravo. W1: tom limpo de 10 min |
| Relógios | SU-2 não tranca a 48 076,9 (§3.2) | W1, matriz de 2 taxas com `NOMINAL_48K` como controlo (opção já existente no Kconfig) |
| Relógios | Drift do novo par > capacidade do corretor (§3.4) | W2: slip líquido em regime (tramas/s → ppm). Critério: ≤ 60 % de 8,3 ppm. Acima disso, decidir antes da sala: subir a capacidade (parâmetro de playout, decisão própria) ou RX v2 |
| Continuidade I²S | No AudioCast o I²S era destruído em cada falta de 2 s, e o S/PDIF caía com ele | **Invariante a testar no W2:** TX desligado 60 s → S/PDIF contínuo, SU-2 trancado; TX de volta → áudio sem novo lock |
| DMA | `on_send_q_ovf` com `auto_clear` desligado repete o último descritor (zumbido); é hoje um evento contado (`dac_underrun`) | Manter `dac_underrun` = 0 como critério; não mudar o DMA nesta migração |
| Buffering/sincronização | Dívida de `late_drop` e slip mais audível no SU-2 | Os critérios do W4 usam os números da Beta 1 como linha de base. Não se mexe no playout aqui; se não for aceitável, o caminho é o RX v2 |
| RF | MCLK de 12,3 MHz e posição nova | W2: A/B **só se** as métricas do link piorarem face à linha de base (MCLK ligado vs −1, mesma placa, só métricas do link) |
| Recuperação | Rollback com partições diferentes; NVS do AudioCast (Wi-Fi/definições) | W0: **dump completo da flash** (byte-exato, inclui a NVS) antes de qualquer gravação |
| Observabilidade | RX cego na sala (§3.5) | Decisão antes do W3: telemetria RX→TX por ESP-NOW (proposta já existente, `task_c54cf859`, alteração de protocolo com gate próprio no par atual primeiro) ou um host com USB junto ao RX |
| Temporização (lição do AudioCast) | Mudanças "inócuas" num caminho quente mudaram o comportamento global (`pd_diag_note_byte_progress`) | Nenhuma alteração no loop de playout nem no callback ESP-NOW; prova por auditoria de objetos (§5.4) |

**Lições transferidas:**
- O AudioLink já resolve por desenho as duas classes de falha do AudioCast: transporte TCP e ciclo de vida do I²S.
- O que o AudioCast **não** resolveu (clicks, WM8805/MCLK) passa intacto para a migração. Deve ser medido e não
  herdado.

---

## 7. Plano incremental e rollback

Cada etapa tem uma só variável nova, critério congelado antes de correr, e autorização explícita para tudo o que toca em
aparelhos.

| Etapa | Variável nova | O que se faz | Critério de passagem | Rollback |
|---|---|---|---|---|
| **W0 — Identificação** (sem gravar) | — | Fotos das duas faces da placa WM8805: cristal, MCU, jumpers, ligações ao ESP (o 18 está ligado? e o 21/22?). Modelo e entradas do SU-2. Versão da KA6 (MK1 tem S/PDIF coaxial = captura digital bit-exata, a verificar). Localização e distâncias. No ESP, só leitura: `flash_id`, `read_mac` e **dump completo da flash** (interrompe o AudioCast; precisa de autorização) | Origem do relógio do WM8805 conhecida ou reduzida a 2 hipóteses; flash/PSRAM/MAC conhecidos; dump com SHA256 em `_archive`/disco externo | n/a (nada gravado) |
| **W1 — Backend sem rádio** | Saída WM8805 | `dac_test` com o perfil WM8805: (a) 48 076,9 + MCLK; (c) 48 076,9 **sem** MCLK, reposta para verificar o domínio de relógio na placa atual (§9.8); (b) `NOMINAL_48K` + MCLK como controlo, **só se (a) falhar**. Verificações físicas V1–V6 em §9.8 | Lock do SU-2 < 1 s; 10 min sem artefacto audível (ou 0 erros na captura digital, se houver); taxa reportada pelo SU-2. Se (a) falhar e (b) passar → decisão B | Gravar o dump do W0 (byte-exato) |
| **W2 — Link completo na bancada** | Rádio + novo par de cristais | RX Beta 1 com perfil WM8805 + TX com o peer novo. RX com consola USB no Mac (`alfr`) | Smoke S1–S6 da Beta 1 + 2 h. `late_drop`, `never_recv`, rebuffers e `dac_underrun` comparados com a linha de base da Beta 1. Drift ≤ 60 % da capacidade. Invariante TX desligado/ligado | TX: binário BETA1-LED (RX PCM5102A volta a funcionar sem lhe tocar). RX: dump do W0 |
| **W2b** (condicional) | MCLK | A/B MCLK ligado/−1, só métricas do link | Só corre se o W2 ficar pior que a linha de base | idem |
| **W3 — Observabilidade remota** (decisão) | Telemetria | Validar primeiro no par atual (PCM5102A); depois levar para o WM8805 | Gate próprio | Binários anteriores |
| **W4 — Sala, uso real** | Posição | Instalação junto ao SU-2 | ≥ 24 h com as métricas do W2 dentro da linha de base + escuta do utilizador | idem W2 |
| **W5 — Fecho** | — | Atualizar `PLATFORM_REGISTRY.md`. Decidir o destino do `snapserver`/`audiocast-web` no Orange e das verificações F1h/`cs-validate-boot` (alteração separada) | — | O dump do W0 repõe o AudioCast; o Orange não foi tocado |

**Ordem de rollback (de qualquer etapa):**
1. `alfr stop`;
2. TX → BETA1-LED;
3. ESP do AudioCast → dump do W0;
4. `alfr start`.

O RX PCM5102A atual não é tocado em nenhuma etapa.

---

## 8. Decisões pedidas ao utilizador

1. **Produto:** o SU-2 passa a tocar a captura analógica do TX em vez do streaming Snapcast (§3.9)? Ou os dois coexistem
   (outra placa para o AudioLink)?
2. **Autorizar o W0** (fotos + leitura da flash do ESP do AudioCast, com interrupção do serviço).
3. **Observabilidade na sala:** telemetria RX→TX (recomendado) ou host USB local?
4. **Onde fazer o W1/W2:** levar o SU-2 para a bancada, ou levar o conjunto ESP+WM8805 para junto do Mac? O
   conjunto não se modifica, só se desloca.

---

## 9. Relógios do WM8805: o que diz o datasheet (adenda de 2026-10-29)

**Fonte:** datasheet oficial da Cirrus Logic, *WM8805 Production Data, Rev 4.5, March 2009*, 66 páginas
(`https://www.cirrus.com/docs/ds/wm8805-datasheet.pdf`, SHA256 `a869d8ff…7404`, lido em 29/10). As páginas e tabelas
abaixo são desse documento. O que vem do datasheet está CONFIRMADO quanto ao *chip*. O que diz respeito à *nossa placa*
é dedução e fica etiquetado.

### 9.1 Quem relógia o transmissor S/PDIF (CONFIRMADO, datasheet)

- **Origem do relógio do TX S/PDIF** (p. 27, Tabela 30): "When the transmitter's data source is the digital audio
  interface, the transmitter reference clock source is the MCLK signal at the MCLK input/output pin".
  - Com `TXSRC = 0` (retransmitir o recetor) usa o `CLK2` da PLL.
  - Com `TXSRC = 1` (dados do I²S) usa **o pino MCLK**.
- **MCLK aceite** (p. 27): 128, 256, 384, 512, 768 ou 1152·fs da taxa transmitida.
  - Mestre/escravo do AIF (Tabela 55, `AIF_MS`): "0 = Slave mode – MCLK, LRCLK and BCLK are inputs". Os três sinais
    mudam de sentido juntos; não há um modo com BCLK/LRCLK de entrada e MCLK de saída.
  - Em escravo, o MCLK do TX é o sinal que o ESP mete no pino 20.
- **Função do cristal** (p. 21, p. 30): o oscilador gera o `OSCCLK` para a PLL. A PLL serve o recetor S/PDIF e as saídas
  `MCLK`/`CLKOUT` em mestre. Em modo hardware o cristal tem de ser 12 MHz porque a recuperação de relógio do recetor o
  exige. **Nada disto entra no caminho AIF → TX em escravo.**
- **Conclusão:** a existência de um cristal na placa **não implica** um domínio de relógio independente para o S/PDIF
  transmitido. Com a placa em escravo, o S/PDIF sai exatamente à taxa do MCLK do ESP: um só domínio, o do ESP. Sem
  MCLK no pino, em escravo, o transmissor não tem referência (dedução direta da Tabela 30): não haveria S/PDIF.

### 9.2 Em que modo está a nossa placa (dedução, por confirmar na foto)

Os modos são lidos em pinos no reset (p. 12–19, Tabelas 6, 8, 14, 15, 16):

| Pino | Lido no reset como | 0 | 1 |
|---|---|---|---|
| 7 SDIN/HWMODE | modo de controlo | **hardware** | software |
| 4 SCLK | `AIF_MS` (só em hardware) | **escravo** | mestre |
| 9 CSB | `TXSRC` (só em hardware) | S/PDIF Rx | **AIF** |
| 5 GPO0 + 8 SDOUT | formato do AIF (só em hardware) | 00 = I²S 16 · 01 = I²S 24 | 10 = LJ 24 com flags · 11 = RJ 16 |

Etapas da dedução:
1. **Em software, tudo arranca desligado** (p. 13: "all functions of the device are powered down by default") e o
   AudioCast nunca escreveu registos. Logo, ou a placa está em **modo hardware**, ou tem um **microcontrolador próprio**
   que a configura. FORTEMENTE SUPORTADO.
2. **Em hardware e mestre, o AIF só gera BCLK/LRCLK com S/PDIF à entrada** (p. 46, `SYNC_OFF = 0` por defeito). O ESP
   era mestre de BCK/LRCK e funcionou, logo a placa está em **escravo**. Num mestre haveria contenda no barramento.
   FORTEMENTE SUPORTADO.
3. **Formato:** I²S, LJ e RJ exigem pelo menos 2 × palavra BCLKs por LRCLK (p. 46). O AudioCast mandava slots de 16 bit
   (BCK = 32·fs; `player.c`, stream `48000:16:2`). Isso só cumpre a regra com palavra de 16 bit. A configuração é
   portanto **I²S 16 bit**: PROVÁVEL, com a ressalva de que o AudioCast tocava de facto corretamente.
4. Com I²S 16 e o nosso BCK de 64·fs, o WM8805 lê os 16 bits mais significativos dos 24. Funciona, mas **trunca para 16
   bit sem dither**. Passar a I²S 24 seria mudar um pull-up/down da placa, o que é alteração de hardware: decisão à
   parte e só se a medição o justificar.

**O que a foto tem de mostrar:**
- os pull-ups/downs (ou jumpers) dos pinos 4, 5, 7, 8 e 9;
- se há um microcontrolador;
- o cristal (deve ser 12 MHz se estiver em hardware);
- se o GPIO 18 do ESP chega ao pino 20 (MCLK).

### 9.3 48 076,9 Hz: problema real ou risco a testar?

**No WM8805: sem problema.** FORTEMENTE SUPORTADO pelo datasheet:
- O TX não tranca nem classifica a taxa. Gera tramas ao ritmo de MCLK/256, dentro de "32 to 192k frames/s" (p. 11).
- Os requisitos do MCLK em escravo são ciclo ≥ 27 ns e duty 40:60 a 60:40 (Tabela 1). 160 MHz/13 = 12,31 MHz
  (81 ns). Com divisor ímpar o duty deve ficar em ~46/54 %: PROVÁVEL, não medido.
- No modo hardware, os bits de *channel status* ficam nos valores por omissão (Tabela 37):
  - `FREQ = 0001`: "Sampling Frequency not indicated";
  - `CLKACU = 11`: "Interface frame rate not matched to sampling frequency".
  - O stream **não afirma ser de 48 kHz**, por isso não contradiz a taxa real.
  - `TXWL/MAXWL` indica 24 bit mesmo com o AIF a 16: é só informativo.

**No SU-2: risco a testar, com indícios favoráveis mas sem prova.**
- O SU-2 usa um ES9039Q2M, tem entradas coaxial, ótica e AES/EBU, e mostra a taxa no painel ([Audiophonics](https://www.audiophonics.fr/en/dac-without-volume/smsl-su2-p-21526.html)).
  O chip que recebe o S/PDIF não está identificado. Pode ser um recetor dedicado ou a entrada S/PDIF do próprio ESS:
  DESCONHECIDO.
- A favor:
  - +0,16 % fica muito dentro das janelas de aquisição típicas. O próprio recetor do WM8805 aceita "44.1kHz -1% to
    48kHz +1%" (p. 27). É um exemplo de chip, não o do SU-2.
  - A arquitetura ESS (DPLL contra relógio fixo do DAC) não precisa de taxas nominais exatas. É conhecimento geral,
    não verificado para o SU-2.
- Contra: fica fora do Nível II da IEC 60958 (±1000 ppm, norma de memória, não verificada). O painel pode mostrar uma
  taxa estranha ou a classificação interna pode falhar.
- Já demonstrado: o mesmo conjunto tocou 48 000 Hz fracionário no AudioCast.
- **Decisão:** não é motivo para mudar de taxa à partida. O W1(a) responde diretamente. Se falhar, o W1(b) a 48 000
  separa "taxa" de "outra coisa", e só então se abre a alternativa B.

### 9.4 Efeito no plano

- §3.1 desce de risco alto para baixo. §3.2 fica restrito ao SU-2.
- No W1 sai a variante sem MCLK, e a variante a 48 000 Hz passa a condicional.
- Surge uma pergunta nova para o W0: o formato da placa (I²S 16 vs 24) e se se aceita a truncatura para 16 bit na
  primeira fase.

### 9.5 Estado do W0 (29/10, ~18:50)

- ESP do AudioCast **offline**: o snapserver dá `connected: false`, não responde a ping nem por HTTP em `.123`, e não
  está na tabela ARP.
- Nenhum ESP ligado por USB ao Mac.
- Pronto e testado só nas recusas: `tools/w0_audiocast_dump.sh` (read_mac → guarda de MAC → flash_id → read_flash
  integral → verify_flash → reinício por watchdog; dump em `_archive/`, porque inclui a NVS com credenciais Wi-Fi).
- **Atualização às 19:10:** W0 concluído. Ver §9.7.

### 9.6 W0: identificação por fotografia (IMG_9670–9673)

**As fotos são anteriores ao projeto AudioCast** (correção do utilizador, 29/10). Descrevem a placa tal como veio,
não o estado atual. A ponte IN-OUT, por exemplo, foi fechada depois. Assim:
- tudo o que está abaixo vale como estado **de origem**;
- placa WM8805: o utilizador não se recorda de ter alterado componentes ou pontes (29/10); o histórico só regista
  cablagem e firmware. **Por decisão do utilizador, a configuração física atual fica POR CONFIRMAR e é verificada no
  W1 (§9.8), sem assumir que corresponde às fotos.** As conclusões abaixo sobre a placa WM8805 são hipóteses de
  trabalho para o W1, não factos do estado atual.

**Placa WM8805** ("... I2S TO COAX/OPT"; WM8805 marcado `GADXAK2137 TW`, data provável semana 37/2021):

| Observado | Consequência | Etiqueta |
|---|---|---|
| CN3 "AUDIO INPUT": 1 GND, 2 VDD, **3 MCLK**, 4 LRCK, 5 BCLK, 6 SDATA; "VDD=5V~12V"; CN1 POWER separado | O MCLK é uma **entrada** da placa, vinda do host | CONFIRMADO (serigrafia) |
| **Y2 (oscilador de 4 pinos) não montado. R17 (liga o Y2 à linha do conector) não montado** | **Não há fonte de relógio na placa.** O único relógio é o MCLK do ESP, logo um só domínio | CONFIRMADO (foto). O que está ligado ao XIN do WM8805 não se vê: DESCONHECIDO |
| Nenhum microcontrolador; só o WM8805, o regulador, a saída ótica e a coaxial | Modo **hardware** (straps por resistências) | FORTEMENTE SUPORTADO |
| Tabela "OUTPUT FORMAT OPTION" no verso; montadas **R25 e R2** (103 = 10 kΩ), **R20 e R23 vazias** | Pela própria tabela da placa = **I²S-24BIT**. O nosso formato (Philips, slot 32, 24 bit úteis, BCK 64·fs) cumpre o datasheet **sem truncatura** | CONFIRMADO (foto + tabela da placa) |
| R5, R1, R21 (10 kΩ) montadas | Candidatas aos straps SDIN (hardware), SCLK (escravo) e CSB (`TXSRC = AIF`) | PROVÁVEL (sem traçado das pistas) |
| Coaxial com R8/R4 = 150 Ω (`151`); footprint C15 grande vazio | Saída resistiva, provavelmente **sem transformador**. A massa do ESP liga à do SU-2 pela malha do coaxial | PROVÁVEL |

**Correções ao §9.2:**
- O formato **não** é I²S 16 (a dedução a partir do AudioCast estava errada). É I²S 24.
- O AudioCast, com slots de 16 bit (BCK 32·fs), corria **fora da especificação** do WM8805 (24 bit exigem ≥ 48 BCLK por
  trama). Soava bem, mas o comportamento não está definido. Fica como candidato, não provado, para os clicks nunca
  explicados.
- O AudioLink a 64·fs fica **dentro** da especificação.

**Placa do ESP** (HW678 V0.0, clone do tipo YD-ESP32-S3):
- Módulo marcado **ESP32-S3-N16R8**, que bate com a imagem de 16 MB e PSRAM octal do RX. A confirmar pelo `flash_id`
  do dump.
- Duas USB-C: COM (USB-UART **CH343P**) e USB (USB-OTG nativo).
- LED RGB com ponte "RGB": o estado atual é DESCONHECIDO, porque a foto é de origem. O LED fica desligado no perfil
  WM8805.
- Ponte IN-OUT: aberta na foto de origem, **fechada hoje** (CONFIRMADO pelo utilizador, 29/10). O pino 5V recebe o
  5 V do USB. É coerente com o WM8805 (VDD 5–12 V) alimentado pelo 5V do ESP, que é PROVÁVEL: a cablagem não foi vista.
- Na bancada isto põe a massa do Mac (USB) ligada à massa do SU-2 pela malha do coaxial, que parece não ter
  transformador. É um possível laço de massa. O link é digital, por isso o risco é baixo, mas fica a observar no W1.
- Nas fotos as placas estão separadas, sem cabo. A correspondência GPIO 18/16/8/15 → CN3 3/5/4/6 vem do sdkconfig do
  AudioCast: FORTEMENTE SUPORTADO, não visto.

**Risco novo, baixo:**
- O datasheet indica que a amostragem dos straps no reset é temporizada pelo relógio do cristal (p. 13). Sem Y2, o
  WM8805 depende do que estiver no XIN, possivelmente o MCLK do host. Neste caso o MCLK teria de estar presente
  quando o WM8805 sai do reset. É HIPÓTESE.
- O AudioCast tinha a mesma sequência (o MCLK só arranca com o I²S) e funcionava.
- Validar no W1: ligar a alimentação do WM8805 antes e depois do arranque do ESP.

### 9.7 W0: dump integral e estado real do ESP (29/10, 19:03–19:10)

`tools/w0_audiocast_dump.sh /dev/cu.usbmodem83201`, rc 0.

**Chip**
- ESP32-S3 (QFN56) rev v0.2, cristal de 40 MHz, PSRAM embebida de 8 MB, **flash de 16 MB** (fabricante `0x0b`,
  dispositivo `0x4018`, quad).
- A flash é de outro fabricante que a das placas do AudioLink (MXIC `c2/2018`).
- Ligação pela porta USB nativa (USB-Serial/JTAG).
- MAC `ac:27:6e:a5:4e:10`, igual ao cliente do snapserver: é a placa certa.

**Dump**
- 16 777 216 bytes em 388 s.
- `verify_flash`: "verify OK (digest matched)", MD5 calculado no chip contra o ficheiro.
- SHA256 `b8a8bab16be8be74…41c87`.

**Cópias** (contêm a NVS com as credenciais Wi-Fi; nunca vão para o Git nem para `reports/`):
1. `_archive/audiolink-w0-wm8805/20261029T190338Z/`
2. `/Volumes/Disco QDC/cumestack-backups/audiocast-w0-flash-2026-10-29/`, hashes verificados após a cópia.

**Estado real do firmware** (CONFIRMADO, comparação byte a byte com `release/H1-D1.16B-perbyte`):

| Região | Conteúdo |
|---|---|
| 0x0 bootloader | = release (`TESTED-RELEASE-ARTIFACT/bootloader.bin`) |
| 0x8000 partições | = release (`f8a731f7…abcf`): nvs 80K, otadata, phy_init, ota_0/ota_1 1984K |
| 0x1d000 otadata | seq 1, estado 2 → **ota_0 ativa** |
| 0x20000 ota_0 | = **H1-D1.16B-perbyte** (`snapclient.bin` da release de produção) |
| 0x210000 ota_1 | apagada (0xFF) |

**Rollback do AudioCast:**
- Byte-exato, incluindo a NVS:
  `esptool --chip esp32s3 -p <porta> --before default_reset --after watchdog_reset write_flash 0 flash_full.bin`.
- Alternativa: a release H1-D1.16B (`ROLLBACK_PROCEDURE.md`). Grava a mesma app, mas a NVS fica à mercê do que a
  imagem do AudioLink lá deixar.

**Notas**
- Após o reinício por watchdog, o ESP continua sem responder a ping em `.123`, como já estava antes do W0 (offline no
  snapserver). Não foi investigado.
- A consola não foi aberta, para não reiniciar a placa.

### 9.8 W1: verificação da configuração física atual (decisão do utilizador, 29/10)

**Princípio:** nada do que as fotos de origem mostram se assume como estado atual. Cada hipótese tem uma verificação
funcional, sem desmontar nem medir com instrumentos que não temos (não há osciloscópio nem analisador lógico
documentados). As hipóteses e os critérios ficam congelados antes de correr.

| # | Hipótese (das fotos/datasheet) | Verificação no W1 | Resultado esperado se a hipótese for verdadeira | Se falhar |
|---|---|---|---|---|
| V1 | Cablagem ESP → CN3: GPIO 18 = MCLK, 16 = BCLK, 8 = LRCK, 15 = SDATA | W1(a): tom ALT de 997 Hz | O SU-2 tranca e toca o tom | Parar. Pinos ou cabo por identificar com o utilizador |
| V2 | Sem fonte de relógio na placa (Y2/R17 vazios): o TX S/PDIF depende só do MCLK do ESP | W1(c): mesmo build, só MCLK = −1 | **Sem lock** no SU-2 | Há relógio próprio na placa e reabre-se a questão do domínio (§3.1). Só se continua depois de o perceber |
| V3 | Modo hardware, escravo, `TXSRC = AIF` | Implícito em V1: o ESP é mestre de BCLK/LRCK | Tom limpo, sem contenda | Parar |
| V4 | Formato I²S (não LJ/RJ) | Tom de −20 dBFS gravado à saída do SU-2 (SU-2 → entrada de linha da KA6, `rec/ka6rec.swift`) | Nível −20 dBFS ± 1 dB, THD sem harmónicos de erro de formato | LJ: +6 dB e saturação. RJ: ruído/lixo. Corrigir o formato no firmware, não na placa |
| V5 | **24 bit** (não I²S-16) | Tom de −120 dBFS somado a DC de +½ LSB16 (128 LSB24). Com 24 bit o tom passa. Truncado a 16 bit, todas as amostras ficam no mesmo degrau e o tom desaparece. Deteção por FFT de 60 s (ganho de processamento ~60 dB). Alternativa bit-exata: entrada S/PDIF da KA6, se for MK1 | Tom de 997 Hz presente | I²S-16: aceitar truncatura (decisão) ou alterar straps (hardware, decisão à parte) |
| V6 | Arranque sem cristal: a amostragem dos straps no reset depende do XIN (§9.6) | 5 arranques a frio (WM8805 e ESP ligados juntos) + 3 resets só do ESP (WM8805 sempre alimentado) | S/PDIF e tom corretos em 8/8 | Documentar a sequência necessária antes do W2 |
| V7 | Canais L/R | Sinal ALT do `dac_test` (alterna canais) | L e R na ordem certa | Corrigir no firmware |

**Notas**
- O V5 exige o gerador de tom do `dac_test` com um modo de teste novo (DC + nível muito baixo). É código de teste,
  fora do caminho do RX, e entra na mesma alteração do W1, com o seu próprio teste de host (as amostras geradas têm de
  ficar todas no mesmo degrau de 16 bit).
- Sequência: V1 → V7 → V4 → V5 → V6 → V2. O V2 fica no fim porque deixa o SU-2 sem sinal.
- Uma gravação por variante. Cada mudança de firmware passa por autorização.
- Rollback: o dump do W0.

### 9.9 W1 preparado (29/10), não executado

- **Onde está:** ramo `audiolink-wm8805-w1` (a partir de `d29d57c`), worktree `.claude/worktrees/audiolink-wm8805-w1`,
  árvore de trabalho **não commitada**. Plano, critérios congelados e comandos em
  `esp/AUDIOLINK/experiments/W1-WM8805/PLAN.md` (nessa worktree).
- **Firmware:**
  - variantes A (ALT, 1031 Hz), A5 (LSB24), C (sem MCLK) e B (48 kHz, condicional), em `release/W1-WM8805/`;
  - opção nova `AUDIOLINK_DAC_MCLK_OUT`, `n` por omissão;
  - o RX PCM5102A compilado com esta fonte é idêntico, em todas as secções carregáveis, à referência da Beta 1 + LED.
- **Ferramentas:**
  - `w1_flash.sh`: guarda de MAC, SHA256 e reposição do dump;
  - `w1_run.sh`: gravação da KA6 e reinício do ESP;
  - `w1_rec.py`: `ka6rec` congelado;
  - `w1_analyze.py`: banco sintético 9/9.
- **Correções feitas durante a preparação**, todas apanhadas antes de qualquer medição:
  - tom de teste passou de 997 para **1031 Hz**, por causa da harmónica de rede de 1000 Hz;
  - frequência esperada passou a ser f0 em Hz reais (modelo anterior errado);
  - THD+N passou a ser medido só em janelas interiores (o degrau no início do tom dava falsos FAIL);
  - limiar de deteção fundamentado em 15 dB.
- **Critério do V4 revisto:** o nível absoluto não é calibrado, por isso o V4 usa THD+N, equilíbrio L/R e
  frequência. Um recetor LJ ou RJ que leia I²S gera distorção grosseira, que o banco confirma.

### 9.10 W1 fechado e W2 preparado (29/10)

**W1 fechado** por decisão do utilizador ("Aceitável, seguir W2"):
- W1-D: o S/PDIF sai 1:1 com o relógio do ESP e leva mais de 16 bit.
- W1-S: o SU-2 tranca e mostra "48kHz"; a perceção de mudança de fase no tom foi aceite.
- O caminho KA6/CoreAudio não acompanha 48 076,9 Hz. Isto não prova o comportamento do recetor físico da KA6.

**W2 preparado, não executado** (`experiments/W2-WM8805/PLAN.md` na worktree):
- **RX `7b00821a7`:** face à Beta 1 + LED só muda `link_rx_run`, +4 B (pinos e MCLK). O LED fica no GPIO 48, porque
  desligá-lo mudava 69 funções.
- **TX `c03dbdbe7`:** face ao `37b588a01` só mudam a versão e os 12 B do MAC do peer.
- **Avaliação:** recorder isolado (`--root`); S1–S5 da Beta 1; K1–K4 com K2/K3 em três estados. A própria Beta 1
  mostrou `late_drop` 0,530/s numa janela de 15 min.
- **Factos observados na preparação:**
  - o recorder de produção parou a 18/10 com "No space left on device";
  - o volume do Mac está hoje a 98 % (12 GB livres).

## 10. Estado em 2026-10-30: em utilização real

**Validação funcional no SU-2 (W4): PASS**, por decisão do utilizador ("está a funcionar muito bem").
`experiments/W4-SU2/RESULT_W4.md` (worktree).

**Nós em uso**, compilados a partir da worktree `audiolink-wm8805-w1`, **sem commit**, com cópias em `_archive/`:

| Nó | Placa | Firmware | Notas |
|---|---|---|---|
| RX | Placa do AudioCast (`ac:27:6e:a5:4e:10`) + WM8805 → SU-2 (COAX), na sala | R1 `44e6ac897` (`audiolink-r1-rx-wm8805-reanchor`) | Saída WM8805 com MCLK no GPIO 18, LED no GPIO 48, correção REANCHOR-01 |
| TX | Placa TX da Beta 1 (`28:84:85:64:1d:ec`) no Mac, entrada dos phones da KA6 | W2 `c03dbdbe7` (`audiolink-w2-tx-peer-audiocast`) | Igual à Beta 1 + LED, salvo o peer |

**Fora de serviço:** o RX PCM5102A da Beta 1 (`28:84:85:58:c7:e4`) ficou sem peer. Continua com o firmware `86b2f8aaf`,
que **tem o bug do `implausible_ctr`**. Se voltar a ser usado, deve levar o R1 PCM5102A `7a509f157`.

**Questões em aberto, não bloqueantes:**
1. **Episódios de RF:** ~1/h na bancada, com o RSSI a variar sem manuseamento. Não estão ligados ao WM8805.
   Investigação suspensa por decisão do utilizador; dados em `_archive/audiolink-w2-recorder-20261030T004553Z/`.
2. **Ocultação audível:** durante as perdas, o playout repete o último bloco e produz um "beep" tonal de 375,6 Hz,
   sempre diferente (confirmado numa gravação). É anterior a este trabalho e também acontece na Beta 1. Corresponde ao
   requisito proposto R3 do `RX_PLAYOUT_PROPOSAL.md`.
3. **Duplicados:** a deteção na aplicação está desligada depois de ~532 s de sessão do TX (bitmap absoluto).
4. **Recorder de produção** parado desde 18/10 (disco cheio). Sem consola, o RX na sala só se observa pelo LED; a
   telemetria RX→TX fica para depois.

**Rollback:**
- TX: `w2_flash.sh <porta> rollback-tx` (Beta 1 + LED).
- Placa do AudioCast: `w2_flash.sh <porta> rollback-rx` (AudioCast byte-exato, dump do W0).

O Orange (`snapserver`, `audiocast-web`) não foi tocado.
