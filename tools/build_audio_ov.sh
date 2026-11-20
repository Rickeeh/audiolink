#!/usr/bin/env bash
# Build do firmware audio/ (RX ou TX legado) com um overlay, pela receita dos RC
# (release/CORE-V1-RC1/artifacts/build.sh.txt). NAO grava.
# Uso: tools/build_audio_ov.sh <overlay.ov> <dir_de_build>
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OV="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
B="${2:?dir de build}"
export IDF_PATH="${IDF_PATH:?Define IDF_PATH para a instalacao do ESP-IDF v5.4.4}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1
mkdir -p "$B"
idf.py -C "$HERE/audio" -B "$B" -DSDKCONFIG="$B/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$HERE/audio/sdkconfig.defaults;$HERE/audio/sdkconfig.defaults.esp32s3;$OV" \
  -DIDF_TARGET=esp32s3 -DCCACHE_ENABLE=0 reconfigure build > "$B/build.log" 2>&1
rc=$?
echo "build rc=$rc warnings=$(grep -c 'warning:' "$B/build.log") log=$B/build.log"
exit $rc
