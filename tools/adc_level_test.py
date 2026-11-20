#!/usr/bin/env python3
"""Teste de nivel do PCM1808 com sinal real, com guarda de seguranca automatica.

Toca um seno (por omissao 1 kHz a -40 dBFS digitais) pela interface de audio e le, pela consola serie do S3 em
modo MONITOR, as estatisticas por segundo do ADC (ADCSTAT). O UTILIZADOR sobe o botao de volume da saida (phones)
devagar durante o teste; este script:
  - imprime L/R pico e RMS a cada segundo e o tempo, para se ver a resposta ao nivel e o canal correto;
  - PARA O TOM (fade-out) se o pico do ADC chegar a --max-peak (dBFS) ou houver qualquer clipping;
  - no fim resume: piso, maximo atingido, e quanto o canal NAO ligado subiu (separacao de canais).

  adc_level_test.py --port /dev/cu.usbmodemXXXX --seconds 90 [--freq 1000] [--level -40] [--max-peak -12]
                    [--out-channels 1 2] [--device Komplete] [--ramp-step 0 --ramp-hold 8 --max-digital -30]

--ramp-step > 0 sobe o nivel DIGITAL automaticamente (dB por degrau) em vez de contar com o botao.
"""
import argparse
import re
import threading
import time

import numpy as np
import serial
import sounddevice as sd

STAT = re.compile(r"ADCSTAT t=(\d+) fs=([\d.]+) L_peak=(-?[\d.]+)dBFS L_rms=(-?[\d.]+)dBFS L_dc=(-?[\d.]+)%FS L_clip=(\d+) "
                  r"R_peak=(-?[\d.]+)dBFS R_rms=(-?[\d.]+)dBFS R_dc=(-?[\d.]+)%FS R_clip=(\d+)")


class Tone:
    def __init__(self, dev, nch, out_ch, freq, fs=48000):
        self.fs, self.freq, self.out_ch, self.nch = fs, freq, out_ch, nch
        self.phase, self.gain, self.target = 0.0, 0.0, 0.0
        self.stream = sd.OutputStream(device=dev, channels=nch, samplerate=fs, dtype="float32", callback=self.cb)

    def cb(self, out, frames, ti, status):
        step = 1.0 / (0.05 * self.fs)  # rampa de 50 ms entre ganhos (sem cliques)
        ph = self.phase + 2 * np.pi * self.freq / self.fs * np.arange(frames)
        g = np.empty(frames)
        cur = self.gain
        for i in range(frames):
            cur += np.clip(self.target - cur, -step, step)
            g[i] = cur
        self.gain = cur
        self.phase = (ph[-1] + 2 * np.pi * self.freq / self.fs) % (2 * np.pi)
        s = (g * np.sin(ph)).astype(np.float32)
        out[:] = 0
        for c in self.out_ch:
            out[:, c] = s

    def set_db(self, db):
        self.target = 0.0 if db is None else 10 ** (db / 20.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--freq", type=float, default=1000)
    ap.add_argument("--level", type=float, default=-40)
    ap.add_argument("--max-peak", type=float, default=-12)
    ap.add_argument("--out-channels", type=int, nargs="+", default=[1, 2])
    ap.add_argument("--device", default="Komplete")
    ap.add_argument("--ramp-step", type=float, default=0)
    ap.add_argument("--ramp-hold", type=float, default=8)
    ap.add_argument("--max-digital", type=float, default=-30)
    ap.add_argument("--baseline", type=float, default=6, help="segundos de silencio no inicio (piso)")
    a = ap.parse_args()

    dev = next((i for i, d in enumerate(sd.query_devices()) if a.device.lower() in d["name"].lower()
                and d["max_output_channels"] > 0), None)
    if dev is None:
        raise SystemExit("dispositivo de saida nao encontrado")
    nch = sd.query_devices(dev)["max_output_channels"]
    tone = Tone(dev, nch, [c - 1 for c in a.out_channels], a.freq)

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = a.port, 115200, 0.3
    ser.dtr, ser.rts = False, False
    ser.open()

    rows, state = [], {"stop": False, "reason": ""}
    t_start = time.time()
    tone.stream.start()
    level_now = None  # dBFS digital atual (None = silencio)
    next_ramp = None
    print(f"tom {a.freq:.0f} Hz nos canais {a.out_channels} da interface; baseline {a.baseline:.0f} s, depois {a.level:.0f} dBFS digitais; "
          f"guarda: pico ADC >= {a.max_peak:.0f} dBFS ou clip => para o tom. Sobe o botao DEVAGAR.")
    print("  seg | tom_dBFS |  L_pico  L_rms |  R_pico  R_rms | clip")
    buf = b""
    try:
        while time.time() - t_start < a.baseline + a.seconds:
            el = time.time() - t_start
            if not state["stop"]:
                if el >= a.baseline and level_now is None:
                    level_now = a.level
                    tone.set_db(level_now)
                    next_ramp = el + a.ramp_hold
                elif a.ramp_step > 0 and level_now is not None and el >= next_ramp and level_now + a.ramp_step <= a.max_digital:
                    level_now += a.ramp_step
                    tone.set_db(level_now)
                    next_ramp = el + a.ramp_hold
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                m = STAT.search(line.decode("utf-8", "replace"))
                if not m:
                    continue
                t, fs, lp, lr, ld, lc, rp, rr, rd, rc = m.groups()
                lp, lr, rp, rr = float(lp), float(lr), float(rp), float(rr)
                clip = int(lc) + int(rc)
                rows.append((el, level_now, lp, lr, rp, rr, clip))
                tag = "" if level_now is None else f"{level_now:6.1f}"
                print(f" {el:5.1f} | {tag:>8} | {lp:7.1f} {lr:7.1f} | {rp:7.1f} {rr:7.1f} | {clip}")
                if not state["stop"] and level_now is not None and (max(lp, rp) >= a.max_peak or clip > 0):
                    state["stop"], state["reason"] = True, f"pico {max(lp, rp):.1f} dBFS / clip {clip}"
                    tone.set_db(None)
                    print(f"  >>> GUARDA: {state['reason']} -> tom desligado")
    finally:
        tone.set_db(None)
        time.sleep(0.3)
        tone.stream.stop()
        ser.close()

    base = [r for r in rows if r[1] is None and r[0] > 3]
    act = [r for r in rows if r[1] is not None]
    print("\n=== resumo ===")
    if base:
        print(f"piso em silencio (apos assentar): L_rms {np.mean([r[3] for r in base]):.1f}  R_rms {np.mean([r[5] for r in base]):.1f} dBFS")
    if act:
        mx = max(act, key=lambda r: r[2])
        print(f"maximo atingido: L_pico {mx[2]:.1f} dBFS (L_rms {mx[3]:.1f})  com R_pico {mx[4]:.1f} (R_rms {mx[5]:.1f}) ; tom digital {mx[1]:.1f} dBFS")
        print(f"ganho aparente tom->ADC (L_rms - nivel digital): {mx[3]-mx[1]:.1f} dB  | subida de L sobre o piso: "
              f"{mx[3]-(np.mean([r[3] for r in base]) if base else float('nan')):.1f} dB ; subida de R: "
              f"{mx[5]-(np.mean([r[5] for r in base]) if base else float('nan')):.1f} dB")
        print("clipping em algum segundo:", "SIM" if any(r[6] for r in act) else "nao")
    if state["stop"]:
        print("parado pela guarda:", state["reason"])


if __name__ == "__main__":
    main()
