#!/usr/bin/env bash
# Flasha bench/build_rx e bench/build_tx, corre o teste e analisa.
#
# Uso: tools/run_bench.sh <nome-do-run> [--no-flash]
#   RX_PORT / TX_PORT por variavel de ambiente (defaults = as portas atuais)
#
# Sequencia (importa): a captura do RX abre primeiro (abrir a porta reinicia
# o S3 e ele fica a ouvir), depois a captura do TX (o reset dela arranca o
# teste). Nunca ter duas ferramentas na mesma porta ao mesmo tempo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NAME="$1"; FLASH=1; [ "${2:-}" = "--no-flash" ] && FLASH=0
RX_PORT="${RX_PORT:-/dev/cu.usbmodem83201}"
TX_PORT="${TX_PORT:-/dev/cu.usbmodem83101}"

# shellcheck disable=SC1091
source "${IDF_PATH:?Define IDF_PATH para o ESP-IDF v5.4.4}/export.sh" >/tmp/idf_export.log 2>&1
cd "$HERE"

if [ "$FLASH" = 1 ]; then
  for pair in "$RX_PORT:build_rx" "$TX_PORT:build_tx"; do
    port="${pair%%:*}"; dir="bench/${pair##*:}"
    echo "=== flash $port ($dir) ==="
    python -m esptool --chip esp32s3 -p "$port" -b 460800 --before default_reset --after no_reset \
      write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
      0x0 "$dir/bootloader/bootloader.bin" 0x8000 "$dir/partition_table/partition-table.bin" \
      0x10000 "$dir/audiolink_bench.bin" 2>&1 | grep -E "Hash of data|Failed|rror" | sort | uniq -c
    python -m esptool --chip esp32s3 -p "$port" --after watchdog_reset read_mac 2>&1 | grep -E "^MAC|Failed" | head -1
  done
fi

OUT="results/$NAME"
mkdir -p results
cp bench/build_rx/sdkconfig "$OUT.rx.sdkconfig"
cp bench/build_tx/sdkconfig "$OUT.tx.sdkconfig"
DUR=$(grep -E '^CONFIG_AUDIOLINK_TEST_DURATION_S=' bench/build_tx/sdkconfig | cut -d= -f2)

python tools/capture_serial.py --port "$RX_PORT" --out "$OUT.rx.raw.txt" \
  --until "===AUDIOLINK_BENCH_DUMP_END===" --timeout $((DUR + 60)) &
RXPID=$!
sleep 6
python tools/capture_serial.py --port "$TX_PORT" --out "$OUT.tx.raw.txt" \
  --until "TX TERMINADO" --timeout $((DUR + 30))
wait $RXPID || true

echo "=== TX ==="; strings "$OUT.tx.raw.txt" | grep -E "TX a arrancar|TERMINADO|E \(|rate_config" || true
echo "=== analise ==="
python tools/analyze_bench.py --rx-log "$OUT.rx.raw.txt" --tx-log "$OUT.tx.raw.txt" --out "$OUT.md" || \
  echo "(sem dump no RX - ver $OUT.rx.raw.txt)"
[ -f "$OUT.md" ] && cat "$OUT.md"
