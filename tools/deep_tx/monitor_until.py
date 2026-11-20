#!/usr/bin/env python3
"""So leitura (NUNCA reseta): monitoriza TX+RX ate o TX cumprir a pre-condicao saudavel (mesmos limiares
congelados do sd_matrix.py: 20 amostras consecutivas com cb_lat_avg<=3000 us, pending<=5 e media<=2,
delta accept>=370/s, delta backpressure<=1/s, RX LOW+HIGH inalterados) ou ate ao endpoint.
Uso: monitor_until.py <outdir> <endpoint_epoch> [rotulo]
Opcional: --release-tx-first  liberta o TX do download mode (reset limpo) logo apos abrir as portas
(usado para o ROLLBACK_BOOT: flash com --after no_reset e arranque com a captura ja aberta)."""
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import resetlib  # noqa: E402

out, endpoint = sys.argv[1], float(sys.argv[2])
label = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "monitor"
os.makedirs(out, exist_ok=True)
KV = re.compile(r"\b([a-z_0-9]+)=(-?\d+)")
lock = threading.Lock()
tx_s, rx_s, rom = [], [], []
stop = threading.Event()


def reader(s, path, tag):
    buf = b""
    with open(path, "ab") as f:
        while not stop.is_set():
            c = s.read(4096)
            if not c:
                continue
            buf += c
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                h = time.time()
                f.write(f"H={h:.3f} | ".encode() + raw + b"\n")
                body = raw.decode(errors="replace")
                with lock:
                    if tag == "TX" and "rst:0x" in body:
                        rom.append((h, body.strip()))
                    if tag == "TX" and "LINKTX t=" in body:
                        d = {k: int(v) for k, v in KV.findall(body)}
                        if all(k in d for k in ("uptime_s", "cb_lat_avg_us", "pending", "api_accept", "backpressure_drop")):
                            d["h"] = h
                            tx_s.append(d)
                    if tag == "RX" and "LINKRX t=" in body:
                        d = {k: int(v) for k, v in KV.findall(body)}
                        if "rebuffer_low" in d:
                            d["h"] = h
                            rx_s.append(d)
            f.flush()


def healthy():
    with lock:
        last_rom = rom[-1][0] if rom else 0
        s = [x for x in tx_s if x["h"] > last_rom][-20:]
        r = [y for y in rx_s if s and y["h"] >= s[0]["h"]]
    if len(s) < 20 or time.time() - s[-1]["h"] > 3 or s[-1]["uptime_s"] - s[0]["uptime_s"] != 19:
        return False, f"amostras={len(s)}"
    lat = max(x["cb_lat_avg_us"] for x in s)
    pmax = max(x["pending"] for x in s)
    pavg = sum(x["pending"] for x in s) / 20
    acc = min(b["api_accept"] - a["api_accept"] for a, b in zip(s, s[1:]))
    bp = max(b["backpressure_drop"] - a["backpressure_drop"] for a, b in zip(s, s[1:]))
    info = f"uptime={s[-1]['uptime_s']} lat_max={lat} pend_max={pmax} pend_avg={pavg:.1f} acc_min={acc} bp_max={bp}"
    if lat > 3000 or pmax > 5 or pavg > 2 or acc < 370 or bp > 1:
        return False, info
    if len(r) < 10 or (r[-1]["rebuffer_low"] + r[-1]["rebuffer_high"]) != (r[0]["rebuffer_low"] + r[0]["rebuffer_high"]):
        return False, info + " RX rebuffer LOW/HIGH a crescer/sem dados"
    return True, info


rx = resetlib.open_rx("/dev/tty.usbmodem83101")
tx = resetlib.open_tx("/dev/tty.usbmodem83201")
th = [threading.Thread(target=reader, args=(tx, f"{out}/tx.log", "TX"), daemon=True),
      threading.Thread(target=reader, args=(rx, f"{out}/rx.log", "RX"), daemon=True)]
for t in th:
    t.start()
t_rel = None
if "--release-tx-first" in sys.argv:
    time.sleep(0.5)
    t_rel = resetlib.release(tx)[0]
    print(f"[{time.strftime('%H:%M:%S')}] {label}: TX libertado do download mode (reset limpo)", flush=True)
result, last = "ENDPOINT_DEGRADADO", ""
while time.time() < endpoint:
    ok, info = healthy()
    if ok:
        result, last = "SAUDAVEL", info
        break
    last = info
    if int(time.time()) % 30 == 0:
        print(f"[{time.strftime('%H:%M:%S')}] {label}: ainda nao saudavel: {info}", flush=True)
    time.sleep(1)
if "--wait-dump" in sys.argv:
    # so continua a ler (nenhuma acao): o dump do trace deste boot sai aos ~140 s de uptime
    print(f"[{time.strftime('%H:%M:%S')}] {label}: resultado={result}; a continuar a capturar ate ao dump do trace", flush=True)
    t_w = time.time()
    while time.time() - t_w < 300:
        if "TRACE END" in open(f"{out}/tx.log", errors="replace").read()[-200000:]:
            break
        time.sleep(2)
time.sleep(1)
stop.set()
time.sleep(0.5)
tx.close()
rx.close()
with lock:
    roms = list(rom)
print(f"[{time.strftime('%H:%M:%S')}] {label}: RESULTADO={result} ({last}) banners_ROM={roms}", flush=True)
