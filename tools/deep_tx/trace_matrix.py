#!/usr/bin/env python3
"""DEEP-TX-DEGRADATION-02: matriz intercalada S D S D ... (S = 1 reset limpo, D = reset limpo +
2o reset 300 ms depois). Um so processo abre TX e RX UMA vez (metodo validado nos probes) e fica a
ler continuamente; o reset e feito no mesmo handle (so RTS). Retomavel: se markers.jsonl ja tiver
ciclos, continua do seguinte. Uso: sd_matrix.py <outdir> [n_cada=10]

Pre-condicao (congelada, medida em 943 amostras saudaveis) antes de CADA reset: as ultimas 20
amostras LINKTX do boot atual (uptime consecutivo, ultima com < 3 s) com cb_lat_avg <= 3000 us,
pending <= 5 e media <= 2, delta api_accept >= 370/s, delta backpressure_drop <= 1/s, e no RX
rebuffer_low+rebuffer_high inalterados nesse intervalo. Se nao for cumprida em 600 s: o ciclo
anterior fica marcado como nao recuperado e e feito um reset de recuperacao (S, NAO contado,
registado), ate 3 vezes; depois disso aborta.
"""
import json
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import resetlib  # noqa: E402

TXP, RXP = "/dev/tty.usbmodem83201", "/dev/tty.usbmodem83101"
OUT = sys.argv[1]
N_EACH = int(sys.argv[2]) if len(sys.argv) > 2 else 10
SEQ = ["S"] * N_EACH  # TRACE-TX-BOOT-01: so resets unicos
OBS_S, HEALTHY_N, PRE_CAP_S, MAX_REC = 90, 20, 600, 3
KV = re.compile(r"\b([a-z_0-9]+)=(-?\d+)")
os.makedirs(OUT, exist_ok=True)

lock = threading.Lock()
tx_samples, rx_samples = [], []
last_rom_h = [0.0]
trace_end_h = [0.0]  # TRACE-TX-BOOT-01: hora do ultimo "TRACE END"/"TRACE DISABLED"
ports = {}
stop = threading.Event()


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(f"{OUT}/orchestrator.log", "a") as f:
        f.write(line + "\n")


def marker(obj):
    obj["h"] = time.time()
    with open(f"{OUT}/markers.jsonl", "a") as f:
        f.write(json.dumps(obj) + "\n")


def reader(tag, opener, port, path):
    buf = b""
    with open(path, "ab") as f:
        while not stop.is_set():
            try:
                chunk = ports[tag].read(4096)
            except Exception as e:  # noqa: BLE001
                log(f"ERRO leitura {tag}: {type(e).__name__}: {e} - a reabrir (metodo seguro)")
                time.sleep(0.5)
                try:
                    ports[tag].close()
                except Exception:  # noqa: BLE001
                    pass
                try:
                    ports[tag] = opener(port)
                except Exception as e2:  # noqa: BLE001
                    log(f"ERRO reabrir {tag}: {e2}")
                    time.sleep(1)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                h = time.time()
                f.write(f"H={h:.3f} | ".encode() + raw + b"\n")
                body = raw.decode(errors="replace")
                if tag == "TX":
                    if "TRACE END" in body or "TRACE DISABLED" in body:
                        with lock:
                            trace_end_h[0] = h
                    if "ESP-ROM:" in body:
                        with lock:
                            last_rom_h[0] = h
                    elif "LINKTX t=" in body:
                        d = {k: int(v) for k, v in KV.findall(body)}
                        if all(k in d for k in ("uptime_s", "cb_lat_avg_us", "pending", "api_accept", "backpressure_drop")):
                            d["h"] = h
                            with lock:
                                tx_samples.append(d)
                elif "LINKRX t=" in body:
                    d = {k: int(v) for k, v in KV.findall(body)}
                    if "rebuffer_low" in d and "rebuffer_high" in d:
                        d["h"] = h
                        with lock:
                            rx_samples.append(d)
            f.flush()


def healthy():
    now = time.time()
    with lock:
        s = [x for x in tx_samples if x["h"] > last_rom_h[0]][-HEALTHY_N:]
        r = [y for y in rx_samples if s and y["h"] >= s[0]["h"]]
    if len(s) < HEALTHY_N:
        return False, f"so {len(s)} amostras do boot atual"
    if now - s[-1]["h"] > 3:
        return False, "TX sem amostras recentes"
    if s[-1]["uptime_s"] - s[0]["uptime_s"] != HEALTHY_N - 1:
        return False, "uptime nao consecutivo"
    lat = max(x["cb_lat_avg_us"] for x in s)
    pmax = max(x["pending"] for x in s)
    pavg = sum(x["pending"] for x in s) / len(s)
    acc = min(b["api_accept"] - a["api_accept"] for a, b in zip(s, s[1:]))
    bp = max(b["backpressure_drop"] - a["backpressure_drop"] for a, b in zip(s, s[1:]))
    info = f"lat_max={lat} pend_max={pmax} pend_avg={pavg:.1f} acc_min={acc} bp_max={bp}"
    if lat > 3000 or pmax > 5 or pavg > 2 or acc < 370 or bp > 1:
        return False, info
    if len(r) < HEALTHY_N // 2:
        return False, info + " RX sem dados suficientes"
    rb0, rb1 = r[0]["rebuffer_low"] + r[0]["rebuffer_high"], r[-1]["rebuffer_low"] + r[-1]["rebuffer_high"]
    if rb1 != rb0:
        return False, info + f" RX rebuffer LOW/HIGH a crescer ({rb0}->{rb1})"
    return True, info + f" RX lo+hi={rb1}"


def wait_healthy(label):
    t0 = time.time()
    rec = 0
    t_att = t0
    last_info = ""
    while True:
        ok, info = healthy()
        if ok:
            return time.time() - t0, rec, info
        if info != last_info and int(time.time() - t0) % 15 == 0:
            log(f"  {label}: a espera de pre-condicao ({time.time() - t0:.0f}s): {info}")
            last_info = info
        if time.time() - t_att > PRE_CAP_S:
            rec += 1
            marker({"kind": "not_recovered", "before": label, "waited_s": time.time() - t0, "info": info})
            if rec > MAX_REC:
                log(f"ABORTAR: {MAX_REC} resets de recuperacao sem TX saudavel")
                marker({"kind": "abort", "before": label})
                stop.set()
                sys.exit(2)
            log(f"  {label}: NAO recuperou em {PRE_CAP_S}s -> reset de recuperacao #{rec} (nao contado)")
            p = resetlib.reset_single(ports["TX"])
            marker({"kind": "recovery", "before": label, "n": rec, "pulses": p})
            t_att = time.time()
        time.sleep(1)


done = 0
if os.path.exists(f"{OUT}/markers.jsonl"):
    done = sum(1 for l in open(f"{OUT}/markers.jsonl") if '"kind": "cycle"' in l)
ports["RX"] = resetlib.open_rx(RXP)
ports["TX"] = resetlib.open_tx(TXP)
threads = [threading.Thread(target=reader, args=("TX", resetlib.open_tx, TXP, f"{OUT}/tx.log"), daemon=True),
           threading.Thread(target=reader, args=("RX", resetlib.open_rx, RXP, f"{OUT}/rx.log"), daemon=True)]
for t in threads:
    t.start()
log(f"inicio: {len(SEQ)} ciclos {''.join(SEQ)}, ja feitos={done}")
marker({"kind": "start", "seq": "".join(SEQ), "done": done})

for n in range(done + 1, len(SEQ) + 1):
    typ = SEQ[n - 1]
    label = f"ciclo {n} ({typ})"
    waited, rec, info = wait_healthy(label)
    log(f"{label}: pre-condicao OK apos {waited:.0f}s ({info}) -> reset")
    pulses = resetlib.reset_single(ports["TX"]) if typ == "S" else resetlib.reset_double(ports["TX"], 0.30)
    marker({"kind": "cycle", "n": n, "type": typ, "pulses": pulses, "pre_wait_s": waited,
            "recovery_resets_before": rec, "pre_health": info})
    time.sleep(max(0.0, pulses[-1][1] + OBS_S + 2 - time.time()))
    # TRACE-TX-BOOT-01: nao resetar antes de o dump deste boot sair (aos ~140 s de uptime)
    t_d0 = time.time()
    while time.time() - t_d0 < 240:
        with lock:
            dumped = trace_end_h[0] > pulses[-1][1]
        if dumped:
            break
        time.sleep(1)
    log(f"{label}: dump do trace {'recebido' if dumped else 'NAO RECEBIDO em 240 s'}")
    marker({"kind": "trace_dump", "n": n, "received": dumped})
    with lock:
        s = [x for x in tx_samples if x["h"] > pulses[-1][1]]
    if s:
        log(f"{label}: fim da observacao, ultimo uptime={s[-1]['uptime_s']} cb_lat_avg={s[-1]['cb_lat_avg_us']} "
            f"pending={s[-1]['pending']}")

waited, rec, info = wait_healthy("final")
marker({"kind": "final_health", "waited_s": waited, "recovery_resets": rec, "info": info})
log(f"MATRIZ COMPLETA (recuperacao do ultimo ciclo: {waited:.0f}s)")
stop.set()
time.sleep(0.5)
