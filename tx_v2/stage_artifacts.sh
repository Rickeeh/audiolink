#!/usr/bin/env bash
# TX v2 - guarda os artefactos de uma etapa (so copia o que o build produziu; NAO grava em nenhum no).
# Uso: tx_v2/stage_artifacts.sh <E1|E2|E3|E4|...> <dir_de_build>
# Resultado: tx_v2/artifacts/<etapa>/ com app (.bin), ELF, bootloader, tabela de particoes, flash_args, sdkconfig,
# overlay do peer, build.log, ELF_PREFIX (9 hex) e SHA256SUMS.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
S="${1:?etapa}"
B="${2:?dir de build}"
D="$HERE/artifacts/$S"
rm -rf "$D"
mkdir -p "$D"
cp "$B/audiolink_tx_v2.bin" "$B/audiolink_tx_v2.elf" "$B/bootloader/bootloader.bin" \
   "$B/partition_table/partition-table.bin" "$B/flash_args" "$B/sdkconfig" "$B/peer.ov" "$B/build.log" "$D/"
shasum -a 256 "$D/audiolink_tx_v2.elf" | cut -c1-9 > "$D/ELF_PREFIX"
( cd "$D" && shasum -a 256 audiolink_tx_v2.bin audiolink_tx_v2.elf bootloader.bin partition-table.bin flash_args \
  sdkconfig peer.ov > SHA256SUMS )
echo "$S: ELF $(cat "$D/ELF_PREFIX") app $(cut -c1-16 < <(shasum -a 256 "$D/audiolink_tx_v2.bin")) ($(stat -f %z "$D/audiolink_tx_v2.bin") B)"
