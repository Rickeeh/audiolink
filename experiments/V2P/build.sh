#!/usr/bin/env bash
# V2P: build do RX (ou do TX de teste) com opcoes de compilacao. O IDF vem do checkout principal. NAO grava.
# Uso: build.sh rx "<CFLAGS>" <dir_de_build>
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
W="${1:?rx}"; X="${2:-}"; B="${3:?dir de build}"
case "$W" in rx) SRC="$HERE/rx" ;; *) echo "Uso: $0 rx \"<CFLAGS>\" <dir_de_build>" >&2; exit 2 ;; esac
export IDF_PATH="${IDF_PATH:?Define IDF_PATH para a instalacao do ESP-IDF v5.4.4}"
. "$IDF_PATH/export.sh" >/dev/null 2>&1
idf.py -C "$SRC" -B "$B" -DIDF_TARGET=esp32s3 -DSDKCONFIG="$B/sdkconfig" -DCCACHE_ENABLE=0 "-DCMAKE_C_FLAGS=$X" build > "$B.log" 2>&1
rc=$?
echo "build $W rc=$rc warnings=$(grep -c 'warning:' "$B.log") extra='$X' log=$B.log"
exit $rc
