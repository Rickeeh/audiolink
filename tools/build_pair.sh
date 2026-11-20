#!/usr/bin/env bash
# Gera e compila RX e TX do bench de raiz (sdkconfig novo) a partir de:
#   sdkconfig.defaults + sdkconfig.defaults.esp32s3 + <overrides comuns> + overrides do papel
#
# Uso: tools/build_pair.sh <ficheiro-overrides-comuns> [MAC_DO_RX]
#   ex.: tools/build_pair.sh configs/baseline_11g_6m.cfg
#
# Saida: bench/build_rx/ e bench/build_tx/ (gitignored). O sdkconfig de cada
# um e guardado em bench/build_<papel>/sdkconfig e serve de prova da config
# exata do run.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
COMMON="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
RX_MAC="${2:-28:84:85:58:C7:E4}"

# shellcheck disable=SC1091
source "${IDF_PATH:?Define IDF_PATH para o ESP-IDF v5.4.4}/export.sh" >/tmp/idf_export.log 2>&1

cd "$HERE/bench"
for role in rx tx; do
  B="build_$role"
  rm -rf "$B"
  OV="$B.overrides"
  {
    cat "$COMMON"
    if [ "$role" = rx ]; then
      echo "CONFIG_AUDIOLINK_ROLE_RX=y"
    else
      echo "CONFIG_AUDIOLINK_ROLE_TX=y"
      echo "CONFIG_AUDIOLINK_ESPNOW_PEER_MAC=\"$RX_MAC\""
    fi
  } > "$OV"
  echo "=== $role: a compilar (overrides: $OV) ==="
  idf.py -B "$B" -D IDF_TARGET=esp32s3 -D SDKCONFIG="$B/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;$OV" build \
    2>&1 | grep -E "warning:|error:|Project build complete|binary size" || true
done
echo "OK: bench/build_rx e bench/build_tx prontos"
