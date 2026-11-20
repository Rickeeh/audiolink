#!/usr/bin/env bash
# TX v2 - build (NAO grava). Mesma receita dos RC (release/CORE-V1-RC1/artifacts/build.sh.txt): IDF 5.4.4, esp32s3,
# sem ccache, reconfigure + build num diretorio proprio (a sdkconfig fica no diretorio de build, nao no tree).
# Uso: tx_v2/build.sh <dir_de_build> [MAC_do_RX]      (MAC por omissao: o RX da bancada, 28:84:85:58:C7:E4)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
B="${1:?indica o diretorio de build}"
PEER="${2:-28:84:85:58:C7:E4}"
export IDF_PATH="${IDF_PATH:?Define IDF_PATH para a instalacao do ESP-IDF v5.4.4}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1
mkdir -p "$B"
printf 'CONFIG_TXV2_PEER_MAC="%s"\n' "$PEER" > "$B/peer.ov"
idf.py -C "$HERE" -B "$B" -DIDF_TARGET=esp32s3 -DSDKCONFIG="$B/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$HERE/sdkconfig.defaults;$B/peer.ov" -DCCACHE_ENABLE=0 reconfigure build > "$B/build.log" 2>&1
rc=$?
echo "build rc=$rc warnings=$(grep -c 'warning:' "$B/build.log") log=$B/build.log"
exit $rc
