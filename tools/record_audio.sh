#!/usr/bin/env bash
# Grava N segundos de um par de canais de uma interface de audio (por omissao a Komplete Audio 6) para WAV 24 bit.
#   tools/record_audio.sh <saida.wav> <segundos> [canal_esq=1] [canal_dir=2] [nome_dispositivo=Komplete]
# Depois: .venv/bin/python tools/audio_analyze.py <saida.wav>
# ATENCAO: NAO usar para MEDIR. O ffmpeg/avfoundation perde amostras sem avisar (medido: 10,7 s gravados de 12 s pedidos e ~85
# descontinuidades num seno limpo). Usar tools/record_audio.py (CoreAudio, reporta overflow e frames recebidos).
# NOTA macOS: a primeira gravacao pede permissao de MICROFONE ao terminal/app que corre este script
# (Definicoes do Sistema > Privacidade e Seguranca > Microfone). Sem ela o ffmpeg fica pendurado.
set -euo pipefail
OUT="$1"; SECS="$2"; L="${3:-1}"; R="${4:-2}"; NAME="${5:-Komplete}"
# `ffmpeg -list_devices` sai SEMPRE com erro (nao ha ficheiro de entrada): com pipefail isso mataria o script em silencio.
LIST=$(ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 || true)
IDX=$(printf '%s\n' "$LIST" | sed -n '/AVFoundation audio devices/,$p' | grep -i "$NAME" | grep -o '\[[0-9]*\]' | head -1 | tr -d '[]' || true)
[ -n "$IDX" ] || { echo "dispositivo '$NAME' nao encontrado"; exit 1; }
echo "a gravar $SECS s do dispositivo [$IDX] canais $L/$R -> $OUT"
timeout $((SECS + 20)) ffmpeg -hide_banner -loglevel error -y -f avfoundation -i ":$IDX" -t "$SECS" \
  -af "pan=stereo|c0=c$((L-1))|c1=c$((R-1))" -ar 48000 -c:a pcm_s24le "$OUT"
echo "ok: $(ls -la "$OUT" | awk '{print $5}') bytes"
