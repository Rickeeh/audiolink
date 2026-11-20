#!/usr/bin/env python3
"""DEEP-TX-DEGRADATION-02, matriz S/D - classificacao FIXA (congelada antes de correr; ver
frozen_criteria.sha256). Uso: classify_sd.py <dir>

Fronteiras por ciclo vem de markers.jsonl (host time). Boot sob teste = linhas LINKTX com hora do
host > ultimo pulso do ciclo e < primeiro pulso do ciclo seguinte.

VALIDACAO por ciclo: banners "ESP-ROM:" entre (1o pulso - 0.05 s) e (ultimo pulso + 1.5 s):
S tem de ter exatamente 1, D exatamente 2, e todos com boot:0x8 (arranque normal da flash).
Caso contrario o ciclo e INVALIDO (nao entra na comparacao).

DEGRADACAO (sistemica, nao picos isolados): janela de 5 amostras LINKTX consecutivas (1/s):
  degradada = mediana(cb_lat_avg_us) > 5000  E  soma(delta backpressure_drop) >= 25
  (baseline saudavel medida em 943 amostras: cb_lat_avg <= 1.8 ms, delta bp <= 1/s)
t_rec = uptime da ultima amostra da ultima janela degradada (0 se nenhuma).
  CLEAN     : t_rec <= 10 s (nunca degradou materialmente, ou normalizou ate 10 s)
  SUSTAINED : a ultima janela que termina em uptime <= 90 s esta degradada
  TRANSIENT : restantes (degradou, recuperou entre 10 e 90 s)
t_rec_total = mesmo calculo sobre TODO o periodo monitorizado desse boot (inclui a espera pela
pre-condicao); 'nao recuperou' se a ultima janela monitorizada estiver degradada.
"""
import json
import re
import statistics
import sys

D = sys.argv[1]
H = re.compile(r"^H=([\d.]+) \| (.*)$")
KV = re.compile(r"\b([a-z_0-9]+)=(-?\d+)")
TXK = ("uptime_s", "cb_lat_avg_us", "cb_lat_p95_us", "cb_lat_max_us", "pending", "api_accept",
       "send_ok", "send_fail", "backpressure_drop", "adc_ovf", "heap_min", "heap_free")
RXK = ("rebuffer_low", "rebuffer_high", "rebuffer_session", "conceal_queue_empty", "queue_ovf")

markers = [json.loads(l) for l in open(f"{D}/markers.jsonl") if l.strip()]
cycles = [m for m in markers if m.get("kind") == "cycle"]

tx, rom = [], []
for line in open(f"{D}/tx.log", errors="replace"):
    m = H.match(line.rstrip("\n"))
    if not m:
        continue
    h, body = float(m.group(1)), m.group(2)
    if "ESP-ROM:" in body:
        rom.append([h, None])
    elif "rst:0x" in body and rom and rom[-1][1] is None:
        bm = re.search(r"boot:(0x[0-9a-fA-F]+)", body)
        rom[-1][1] = bm.group(1) if bm else "?"
    elif "LINKTX t=" in body:
        d = {k: int(v) for k, v in KV.findall(body)}
        if all(k in d for k in TXK):
            d["h"] = h
            tx.append(d)
    elif "sessao: session_id=" in body:
        sm = re.search(r"session_id=([0-9A-F]{16})", body)
        if sm:
            tx.append({"h": h, "session": sm.group(1)})

rx = []
for line in open(f"{D}/rx.log", errors="replace"):
    m = H.match(line.rstrip("\n"))
    if not m or "LINKRX t=" not in m.group(2):
        continue
    body = m.group(2)
    d = {k: int(v) for k, v in KV.findall(body)}
    sm = re.search(r"session_id=([0-9A-F]{16})", body)
    rs = re.search(r"rssi_avg=(-?\d+)", body)
    if all(k in d for k in RXK) and sm:
        d["h"], d["sid"], d["rssi"] = float(m.group(1)), sm.group(1), int(rs.group(1)) if rs else None
        rx.append(d)


def windows(samples):
    """devolve lista de (uptime_fim, degradada) para janelas de 5 amostras consecutivas."""
    out = []
    for i in range(4, len(samples)):
        w = samples[i - 4:i + 1]
        if w[-1]["uptime_s"] - w[0]["uptime_s"] != 4:
            continue
        med = statistics.median(s["cb_lat_avg_us"] for s in w)
        prev = samples[i - 5]["backpressure_drop"] if i >= 5 and samples[i - 5]["uptime_s"] == w[0]["uptime_s"] - 1 else w[0]["backpressure_drop"]
        dbp = w[-1]["backpressure_drop"] - prev
        out.append((w[-1]["uptime_s"], med > 5000 and dbp >= 25))
    return out


results = []
for idx, c in enumerate(cycles):
    t_first = c["pulses"][0][0]
    t_last = c["pulses"][-1][1]
    t_next = cycles[idx + 1]["pulses"][0][0] if idx + 1 < len(cycles) else float("inf")
    boots = [r for r in rom if t_first - 0.05 <= r[0] <= t_last + 1.5]
    expected = 1 if c["type"] == "S" else 2
    valid = len(boots) == expected and all(b[1] == "0x8" for b in boots)
    s_all = [s for s in tx if "uptime_s" in s and t_last < s["h"] < t_next]
    # so o boot sob teste: a partir do 1o uptime baixo, sem voltar a descer
    start = next((i for i, s in enumerate(s_all) if s["uptime_s"] <= 3), None)
    samples = []
    if start is not None:
        for s in s_all[start:]:
            if samples and s["uptime_s"] < samples[-1]["uptime_s"]:
                break
            samples.append(s)
    sess = next((s["session"] for s in tx if "session" in s and t_last < s["h"] < t_next), None)
    w_all = windows(samples)
    w90 = [w for w in w_all if w[0] <= 90]
    deg90 = [u for u, d in w90 if d]
    t_rec = deg90[-1] if deg90 else 0
    if not samples or not w90:
        cls = "SEM_DADOS"
    elif w90[-1][1]:
        cls = "SUSTAINED"
    elif t_rec <= 10:
        cls = "CLEAN"
    else:
        cls = "TRANSIENT"
    deg_all = [u for u, d in w_all if d]
    if w_all and w_all[-1][1]:
        t_rec_total = f"nao recuperou em {samples[-1]['uptime_s']} s"
    else:
        t_rec_total = f"{deg_all[-1] if deg_all else 0} s"
    s90 = [s for s in samples if s["uptime_s"] <= 90]
    r = {"cycle": c["n"], "type": c["type"], "valid": valid, "rom_boots": len(boots),
         "boot_modes": [b[1] for b in boots], "session": sess, "class": cls, "t_rec": t_rec,
         "t_rec_total": t_rec_total, "pre_wait_s": c.get("pre_wait_s"), "recovery_resets_before": c.get("recovery_resets_before", 0)}
    if s90:
        dur = s90[-1]["uptime_s"] - s90[0]["uptime_s"]
        acc5 = [(s90[i]["api_accept"] - s90[i - 5]["api_accept"]) / 5 for i in range(5, len(s90))]
        r.update({
            "cb_lat_avg_peak_ms": max(s["cb_lat_avg_us"] for s in s90) / 1000,
            "cb_lat_p95_peak_ms": max(s["cb_lat_p95_us"] for s in s90) / 1000,
            "cb_lat_max_peak_ms": max(s["cb_lat_max_us"] for s in s90) / 1000,
            "accept_per_s_min5s": min(acc5) if acc5 else None,
            "accept_per_s_avg": (s90[-1]["api_accept"] - s90[0]["api_accept"]) / dur if dur else None,
            "send_ok_per_s_avg": (s90[-1]["send_ok"] - s90[0]["send_ok"]) / dur if dur else None,
            "send_fail_90s": s90[-1]["send_fail"] - s90[0]["send_fail"],
            "bp_drop_90s": s90[-1]["backpressure_drop"] - s90[0]["backpressure_drop"],
            "pending_avg": sum(s["pending"] for s in s90) / len(s90),
            "heap_min": min(s["heap_min"] for s in s90), "adc_ovf": s90[-1]["adc_ovf"],
        })
    if sess:
        rxs = [x for x in rx if x["sid"] == sess]
        t0 = s90[0]["h"] if s90 else None
        rx90 = [x for x in rxs if t0 is None or x["h"] <= t0 + 90]
        if len(rx90) >= 2:
            a, b = rx90[0], rx90[-1]
            rr = [x["rssi"] for x in rx90 if x["rssi"] is not None]
            r.update({"rx_rebuf_low": b["rebuffer_low"] - a["rebuffer_low"],
                      "rx_rebuf_high": b["rebuffer_high"] - a["rebuffer_high"],
                      "rx_rebuf_session_total": b["rebuffer_session"],
                      "rx_queue_empty": b["conceal_queue_empty"] - a["conceal_queue_empty"],
                      "rx_queue_ovf": b["queue_ovf"] - a["queue_ovf"],
                      "rx_rssi_avg": sum(rr) / len(rr) if rr else None})
    results.append(r)

with open(f"{D}/classification.json", "w") as f:
    json.dump(results, f, indent=1)

print(f"{'n':>3} {'tipo':4} {'valido':6} {'rom':3} {'classe':9} {'t_rec':>5} {'t_rec_total':>22} "
      f"{'pico_avg':>8} {'acc_min5':>8} {'bp90':>6} {'RX lo/hi':>8} {'RX qe':>6} {'RSSI':>5} {'espera_pre':>10}")
for r in results:
    print(f"{r['cycle']:>3} {r['type']:4} {str(r['valid']):6} {r['rom_boots']:>3} {r['class']:9} {r['t_rec']:>5} "
          f"{r['t_rec_total']:>22} {r.get('cb_lat_avg_peak_ms', 0):>8.1f} {r.get('accept_per_s_min5s') or 0:>8.0f} "
          f"{r.get('bp_drop_90s', 0):>6} {str(r.get('rx_rebuf_low', '-')) + '/' + str(r.get('rx_rebuf_high', '-')):>8} "
          f"{r.get('rx_queue_empty', '-'):>6} {r.get('rx_rssi_avg') or 0:>5.1f} {r.get('pre_wait_s') or 0:>10.0f}")
print()
for t in ("S", "D"):
    v = [r for r in results if r["type"] == t and r["valid"]]
    n = len(v)
    cnt = {k: sum(1 for r in v if r["class"] == k) for k in ("CLEAN", "TRANSIENT", "SUSTAINED", "SEM_DADOS")}
    trs = sorted(r["t_rec"] for r in v if r["class"] == "TRANSIENT")
    print(f"{t}: validos={n} CLEAN={cnt['CLEAN']} TRANSIENT={cnt['TRANSIENT']} {trs} SUSTAINED={cnt['SUSTAINED']} "
          f"SEM_DADOS={cnt['SEM_DADOS']} | invalidos={sum(1 for r in results if r['type'] == t and not r['valid'])}")
