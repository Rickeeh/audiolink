# AudioLink

Link de áudio sem fios ponto a ponto com ESP32-S3 N16R8 e ESP-NOW. Firmware de desenvolvimento, ferramentas de bancada e testes executáveis no computador.

## Hardware e código

- **TX:** entrada I2S pelo PCM1808, áudio estéreo PCM24 e pacotes ALK v3 (`tx_v2/`).
- **RX:** receção ESP-NOW e saída I2S, com configurações para PCM5102A ou WM8805/S/PDIF (`audio/`).
- **Componentes partilhados:** protocolo, conversão PCM24 e LED de estado (`components/`).
- **Bancada:** benchmark de rádio, configurações e ferramentas de análise (`bench/`, `configs/`, `tools/`).
- **Protótipo V2P:** recetor passivo e associação com botões (`experiments/V2P/`). O suporte de botões está desativado por omissão (`V2_BTN=0`); o ramo de botões ainda não tinha sido gravado em hardware no commit de origem.

Esta publicação parte do commit de desenvolvimento `871f2c91f47c7d27eb66eee1a64e174ecf9b516d` (16 de novembro de 2026). A referência RX WM8805 L2-12 e os respetivos overlays estão incluídos. Os relatórios em `docs/` descrevem etapas históricas e propostas, e devem ser lidos pelas respetivas datas. Valores MAC nos exemplos são endereços de bancada e devem ser adaptados ao hardware de quem compila.

## Compilar

Instalar o **ESP-IDF v5.4.4** com suporte para `esp32s3`, seguindo a [documentação oficial do ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/index.html). O SDK e a toolchain são dependências externas.

```bash
export IDF_PATH="$HOME/esp/esp-idf"
source "$IDF_PATH/export.sh"

# TX: substituir pelo MAC do recetor
bash tx_v2/build.sh "$PWD/build/tx" AA:BB:CC:DD:EE:FF

# RX WM8805: ajustar antes o peer MAC e a pinagem no overlay
bash tools/build_audio_ov.sh release/L2-12/RX_WM8805/overlay.ov "$PWD/build/rx"
```

Os scripts acima apenas compilam. As ligações e os GPIO estão documentados em `docs/PHASE2_WIRING.md` e `docs/RX_WM8805_SPDIF_PROPOSAL.md`; verificar sempre o overlay utilizado. A gravação é feita separadamente pelo ESP-IDF/esptool.

Para o benchmark SoftAP, definir uma palavra-passe própria em `idf.py menuconfig`; esta publicação não inclui uma palavra-passe por omissão.

## Testes no computador

Requer Bash e um compilador C compatível com AddressSanitizer e UndefinedBehaviorSanitizer (Clang ou GCC).

```bash
bash tests_host/run.sh
```

Os testes cobrem protocolo, PCM24, temporização, ocultação, ressincronização, LED e lógica V2P. Estes testes não validam alcance rádio, qualidade de áudio ou o comportamento físico das placas.

## Conteúdo da publicação

Snapshot independente dos fontes AudioLink, documentação técnica, overlays e testes. Foram excluídos o histórico do monorepositório CumeStack, builds, toolchains, capturas de áudio, logs de ensaios e artefactos binários. Os scripts de build foram adaptados para usar `IDF_PATH`, sem depender do caminho local do autor. O firmware C/H foi preservado sem alterações.
