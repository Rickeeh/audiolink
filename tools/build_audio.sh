#!/usr/bin/env bash
# Gera uma variante do firmware audio/ para um papel e um modo de relogio (NAO flasha).
#   tools/build_audio.sh <ADC|DAC|LINK_TX|LINK_RX> <INT160_13|INT240_20|NOMINAL_48K> [ALT|BOTH|SILENCE|<MAC do peer>]
#   3.o argumento: para ADC pode ser MONITOR (estatisticas continuas, sem captura). Para DAC: sinal de teste. ALT = 1 s esq / 1 s dir (verifica L/R, omissao); BOTH = seno continuo nos dois
#   canais (para MEDIR THD+N/ruido); SILENCE = silencio (piso de ruido do DAC). Para LINK_TX: MAC do LINK_RX
#   (formato AA:BB:CC:DD:EE:FF, do log de arranque do LINK_RX) - sem isto o firmware aborta ao arrancar.
# Saida: audio/build_<PAPEL>_<RELOGIO>[_<SIG>]/ (gitignored). Serve para comparar modos de relogio POR MEDICAO
# (o criterio e o desempenho real, nao "divisor inteiro = melhor").
set -euo pipefail
ROLE="$1"; CLK="$2"; SIG="${3:-}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "${IDF_PATH:?Define IDF_PATH para o ESP-IDF v5.4.4}/export.sh" >/tmp/idf_export.log 2>&1
cd "$HERE/audio"
B="build_${ROLE}_${CLK}${SIG:+_$(echo "$SIG" | tr -d ':')}"; rm -rf "$B"
printf "CONFIG_AUDIOLINK_AUDIO_ROLE_%s=y\nCONFIG_AUDIOLINK_CLK_%s=y\n" "$ROLE" "$CLK" > "$B.ov"
if [ -n "$SIG" ]; then
  case "$SIG" in
    MONITOR) echo "CONFIG_AUDIOLINK_ADC_MONITOR=y" >> "$B.ov" ;;
    *:*:*:*:*:*) echo "CONFIG_AUDIOLINK_LINK_PEER_MAC=\"$SIG\"" >> "$B.ov" ;;  # LINK_TX: MAC do peer
    *) echo "CONFIG_AUDIOLINK_DAC_SIGNAL_$SIG=y" >> "$B.ov" ;;
  esac
fi
idf.py -B "$B" -D IDF_TARGET=esp32s3 -D SDKCONFIG="$B/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;$B.ov" build 2>&1 \
  | grep -E "warning:|error:|Project build complete|binary size" || true
echo "flash: python -m esptool --chip esp32s3 -p <porta> -b 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 audio/$B/bootloader/bootloader.bin 0x8000 audio/$B/partition_table/partition-table.bin 0x10000 audio/$B/audiolink_audio.bin"
