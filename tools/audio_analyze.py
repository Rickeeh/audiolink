#!/usr/bin/env python3
"""Analisa um dump ADCDUMP (firmware audio/, papel ADC) ou um WAV.

  audio_analyze.py captura.raw.txt [--wav saida.wav] [--band 20 20000]
  audio_analyze.py ficheiro.wav

Convencao AES17: 0 dBFS = seno de amplitude full-scale (o RMS de um seno le o mesmo valor que o seu pico).
Metricas por canal: DC (%FS), pico, RMS sem DC, e do espetro (Welch, janela Blackman-Harris):
frequencia/nivel do tom dominante (se >-60 dBFS), ruido+distorcao na banda (excluindo o tom), THD+N
relativo ao tom, e os maiores picos espurios. Sem tom -> so o ruido de fundo na banda.

NOTA: o piso medido e o da cadeia inteira (fonte + ADC/DAC + interface de captura). Nao atribuir o
ruido a um so componente sem medir o piso do proprio instrumento.
"""
import argparse
import re
import struct
import sys
import wave

import numpy as np

FS24 = 8388608.0  # 2^23


def load_dump(path):
    text = open(path, errors="replace").read()
    m = re.search(r"ADCDUMP_BEGIN (.*)\n", text)
    if not m:
        sys.exit("sem ADCDUMP_BEGIN em " + path)
    kv = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', m.group(1)))
    body = text[m.end():text.index("ADCDUMP_END", m.end())]
    hexs = "".join(body.split())
    raw = np.frombuffer(bytes.fromhex(hexs), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = (raw[:, 0] << 16) | (raw[:, 1] << 8) | raw[:, 2]
    v = np.where(v >= 0x800000, v - 0x1000000, v).astype(np.float64)
    x = v.reshape(-1, 2)
    fs = float(kv.get("fs_meas", kv.get("fs_req", 48000)))
    return x, fs, kv


def load_wav(path):
    """Leitor RIFF proprio: o modulo `wave` nao le WAVE_FORMAT_EXTENSIBLE (ffmpeg grava 24 bit assim)."""
    b = open(path, "rb").read()
    if b[:4] not in (b"RIFF", b"RF64") or b[8:12] != b"WAVE":
        sys.exit("nao e um WAV RIFF: " + path)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, fs, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]  # subformato real (1 = PCM, 3 = float)
            fmt = (tag, ch, fs, bits)
        elif cid == b"data":
            data = body if size != 0xFFFFFFFF else b[pos + 8:]
            break
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit("WAV sem fmt/data")
    tag, ch, fs, bits = fmt
    if tag == 1 and bits == 24:
        a = np.frombuffer(data[: len(data) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        v = np.where(v >= 0x800000, v - 0x1000000, v).astype(np.float64)
    elif tag == 1 and bits == 16:
        v = np.frombuffer(data[: len(data) // 2 * 2], dtype="<i2").astype(np.float64) * 256.0
    elif tag == 1 and bits == 32:
        v = np.frombuffer(data[: len(data) // 4 * 4], dtype="<i4").astype(np.float64) / 256.0
    elif tag == 3 and bits == 32:
        v = np.frombuffer(data[: len(data) // 4 * 4], dtype="<f4").astype(np.float64) * FS24
    else:
        sys.exit(f"formato WAV nao suportado: tag={tag} bits={bits}")
    n = len(v) // ch
    return v[: n * ch].reshape(-1, ch), float(fs), {}


def save_wav(path, x, fs):
    w = wave.open(path, "wb")
    w.setnchannels(x.shape[1]); w.setsampwidth(3); w.setframerate(int(round(fs)))
    v = x.astype(np.int64) & 0xFFFFFF
    out = bytearray()
    for a, b in v:
        out += struct.pack("<I", a)[:3] + struct.pack("<I", b)[:3]
    w.writeframes(bytes(out)); w.close()


def bh(n):
    k = np.arange(n)
    return (0.35875 - 0.48829 * np.cos(2 * np.pi * k / (n - 1)) + 0.14128 * np.cos(4 * np.pi * k / (n - 1))
            - 0.01168 * np.cos(6 * np.pi * k / (n - 1)))


def psd_welch(x, fs, seg=65536):
    seg = min(seg, 1 << int(np.floor(np.log2(len(x)))))
    w = bh(seg); step = seg // 2
    acc = None; cnt = 0
    for i in range(0, len(x) - seg + 1, step):
        f = np.fft.rfft(x[i:i + seg] * w)
        p = (np.abs(f) ** 2)
        acc = p if acc is None else acc + p; cnt += 1
    acc /= cnt
    # potencia por bin (para somar bandas): |X|^2 / (N * sum(w^2)) * 2  -> potencia de sinal (RMS^2) por bin
    pw = acc * 2.0 / (seg * np.sum(w ** 2))
    pw[0] /= 2.0
    return pw, fs / seg


def db(x):
    return 10 * np.log10(np.maximum(x, 1e-30))


def analyze(x, fs, band):
    lo, hi = band
    x = x / FS24  # +-1
    mean = x.mean(); xc = x - mean
    peak = np.max(np.abs(x))
    rms = np.sqrt(np.mean(xc ** 2))
    pw, df = psd_welch(xc, fs)
    fr = np.arange(len(pw)) * df
    inband = (fr >= lo) & (fr <= hi)
    half = np.sqrt(0.5)  # AES17: RMS de seno full-scale = 1/sqrt2 -> 0 dBFS
    ref = 0.5  # potencia de um seno full-scale (amplitude 1): 0.5
    kmax = np.argmax(np.where(fr >= 40, pw, 0))
    tone_p = pw[max(kmax - 4, 0):kmax + 5].sum()
    tone_db = db(tone_p / ref)
    res = dict(dc=100 * mean, peak=db(peak ** 2), rms=db(rms ** 2 / ref))
    mask = inband.copy()
    if tone_db > -60:
        mask[max(kmax - 4, 0):kmax + 5] = False
        res["tone_hz"] = fr[kmax]; res["tone_dbfs"] = tone_db
    nd_p = pw[mask].sum()
    res["noise_dbfs"] = db(nd_p / ref)
    if tone_db > -60:
        res["thdn_db"] = res["noise_dbfs"] - tone_db
    # picos espurios: bins locais > 12 dB acima da mediana do piso, fora do tom
    floor = np.median(pw[mask])
    spurs = []
    for k in range(5, len(pw) - 5):
        if not mask[k] or pw[k] < 16 * floor or pw[k] < pw[k - 1] or pw[k] < pw[k + 1]:
            continue
        if 'tone_hz' in res and abs(k - kmax) <= 16:  # lobulos laterais da janela, nao espurios
            continue
        spurs.append((db(pw[k - 2:k + 3].sum() / ref), fr[k]))
    spurs.sort(reverse=True)
    res["spurs"] = spurs[:6]
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--wav")
    ap.add_argument("--band", type=float, nargs=2, default=[20, 20000])
    ap.add_argument("--skip", type=float, default=0.0, help="saltar os primeiros N segundos (assentamento do HPF/DC do ADC)")
    a = ap.parse_args()
    x, fs, kv = load_wav(a.input) if a.input.endswith(".wav") else load_dump(a.input)
    if a.skip > 0:
        x = x[int(a.skip * fs):]
    if a.wav:
        save_wav(a.wav, x, fs)
    print(f"{len(x)} frames, fs = {fs:.3f} Hz ({len(x)/fs:.1f} s), banda {a.band[0]:.0f}-{a.band[1]:.0f} Hz")
    if kv:
        print("firmware:", " ".join(f"{k}={v}" for k, v in kv.items()))
    for c, name in enumerate("LR"[:x.shape[1]]):
        r = analyze(x[:, c], fs, a.band)
        line = (f"[{name}] DC {r['dc']:+.3f}%FS | pico {r['peak']:.1f} dBFS | RMS(sem DC) {r['rms']:.1f} dBFS")
        if "tone_hz" in r:
            line += f" | tom {r['tone_hz']:.1f} Hz a {r['tone_dbfs']:.1f} dBFS"
        print(line)
        print(f"      ruido{'+distorcao' if 'tone_hz' in r else ''} na banda: {r['noise_dbfs']:.1f} dBFS"
              + (f" | THD+N {r['thdn_db']:.1f} dB re tom" if 'thdn_db' in r else ""))
        if r["spurs"]:
            print("      espurios: " + ", ".join(f"{f:.0f} Hz {l:.1f} dBFS" for l, f in r["spurs"]))


if __name__ == "__main__":
    main()
