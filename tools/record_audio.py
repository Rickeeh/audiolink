#!/usr/bin/env python3
"""Grava um par de canais de uma interface de audio via CoreAudio (sounddevice) e REPORTA perdas de amostras.

  record_audio.py saida.wav 10 [canal_esq=1] [canal_dir=1] [nome_dispositivo=Komplete]

Ao contrario do ffmpeg/avfoundation, o callback expoe `status` (input_overflow/underflow) e o numero de frames
realmente recebidos, para distinguir defeitos do DUT de defeitos da propria gravacao.
Grava 24 bit. NOTA macOS: pede permissao de microfone ao processo que o corre.
"""
import sys
import time

import numpy as np
import sounddevice as sd

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import audio_analyze as A


def main():
    out, secs = sys.argv[1], float(sys.argv[2])
    L = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    R = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    name = sys.argv[5] if len(sys.argv) > 5 else "Komplete"
    dev = next((i for i, d in enumerate(sd.query_devices()) if name.lower() in d["name"].lower() and d["max_input_channels"] > 0), None)
    if dev is None:
        sys.exit(f"dispositivo '{name}' nao encontrado")
    info = sd.query_devices(dev)
    fs = 48000
    nch = info["max_input_channels"]
    print(f"a gravar {secs:.0f} s de '{info['name']}' ({nch} canais), canais {L}/{R}, {fs} Hz, latencia 'high'")
    chunks, flags = [], {"overflow": 0, "other": 0, "callbacks": 0}

    def cb(indata, frames, time_info, status):
        flags["callbacks"] += 1
        if status:
            if status.input_overflow:
                flags["overflow"] += 1
            else:
                flags["other"] += 1
        chunks.append(indata[:, [L - 1, R - 1]].copy())

    with sd.InputStream(device=dev, channels=nch, samplerate=fs, dtype="int32", blocksize=4800,
                        latency="high", callback=cb):
        time.sleep(secs + 0.5)
    x = np.concatenate(chunks)
    n_exp = int(secs * fs)
    x = x[:n_exp] if len(x) >= n_exp else x
    print(f"frames recebidos: {len(x)} de {n_exp} esperados ({100.0*len(x)/n_exp:.2f}%); "
          f"callbacks={flags['callbacks']} overflow={flags['overflow']} outros_avisos={flags['other']}")
    A.save_wav(out, (x.astype(np.int64) >> 8).astype(np.int32), fs)  # int32 alinhado a esq -> 24 bit
    print("ok ->", out)


if __name__ == "__main__":
    main()
