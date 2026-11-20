#!/usr/bin/env python3
"""TRACE-TX-BOOT-01: descodifica e analisa os dumps TRACE de um tx.log (pode ter varios boots).
Uso: trace_analyze.py <tx.log> [--raster]
Por dump: integridade (CRC32 por bloco, contagens vs cabecalho, overflow), ancoras de arranque, e
nas duas timelines independentes (sem emparelhar SEND<->CB):
  SEND: delta entre oportunidades, aceites/backpressure/erros por segundo, duracao de esp_now_send()
  CB:   delta entre callbacks, rajadas vs lacunas, callbacks/s, status
  periodicidade: autocorrelacao da contagem de callbacks em bins de 10 ms (lags 20 ms..3 s)
"""
import base64
import re
import struct
import sys
import zlib
import statistics

PERIOD_US = 1e6 * 128 / 48077  # 2662 us
path = sys.argv[1]
raster = "--raster" in sys.argv

dumps, cur = [], None
for line in open(path, errors="replace"):
    line = line.rstrip("\r\n")
    m = re.search(r"TRACE (BEGIN|END|DISABLED|S|C)\b ?(.*)$", line)
    if not m:
        continue
    kind, rest = m.group(1), m.group(2)
    if kind == "BEGIN":
        cur = {"hdr": dict(re.findall(r"(\w+)=(-?\d+)", rest)), "S": {}, "C": {}, "bad": [], "lines": 0}
    elif kind == "DISABLED":
        dumps.append({"disabled": rest})
    elif kind == "END" and cur:
        dumps.append(cur)
        cur = None
    elif kind in "SC" and cur is not None:
        cur["lines"] += 1
        p = rest.split()
        try:
            blk, n, crc, b64 = int(p[0]), int(p[1]), int(p[2], 16), p[3]
            raw = base64.b64decode(b64, validate=True)
            ok = len(raw) == n and (zlib.crc32(raw) & 0xFFFFFFFF) == crc
        except Exception:  # noqa: BLE001
            ok, blk = False, -1
        if ok:
            cur[kind][blk] = raw
        else:
            cur["bad"].append((kind, blk))


def q(v, p):
    v = sorted(v)
    return v[min(int(len(v) * p), len(v) - 1)] if v else float("nan")


def series(raw, kind):
    ev = []
    for i in range(0, len(raw), 8):
        if kind == "S":
            t, outc, pend, call_us = struct.unpack_from("<IBBH", raw, i)
            ev.append((t, outc, pend, call_us))
        else:
            t, st = struct.unpack_from("<IB", raw, i)
            ev.append((t, st))
    return ev


for di, d in enumerate(dumps, 1):
    print(f"\n================ dump {di} ================")
    if "disabled" in d:
        print(f"TRACE DESATIVADO: {d['disabled']}")
        continue
    h = {k: int(v) for k, v in d["hdr"].items()}
    t0 = h["t0_us"]
    S_raw = b"".join(d["S"][k] for k in sorted(d["S"]))
    C_raw = b"".join(d["C"][k] for k in sorted(d["C"]))
    s_blocks_exp = (h["n_send"] * 8 + 127) // 128
    c_blocks_exp = (h["n_cb"] * 8 + 127) // 128
    complete = (not d["bad"] and len(d["S"]) == s_blocks_exp and len(d["C"]) == c_blocks_exp
                and sorted(d["S"]) == list(range(s_blocks_exp)) and sorted(d["C"]) == list(range(c_blocks_exp)))
    print(f"integridade: blocos S {len(d['S'])}/{s_blocks_exp}  C {len(d['C'])}/{c_blocks_exp}  "
          f"CRC/base64 maus={len(d['bad'])}  -> {'INTEGRO' if complete else 'INCOMPLETO ' + str(d['bad'][:5])}")
    print(f"cabecalho: janela={h['window_ms']} ms cap={h['cap']} n_send={h['n_send']} n_cb={h['n_cb']} "
          f"ovf_send={h['ovf_send']} ovf_cb={h['ovf_cb']} max_pending={h['max_pending']}")
    print(f"RAM interna: livre {h['free_int_before']} -> {h['free_int_after']} (maior bloco {h['largest_int_before']} -> "
          f"{h['largest_int_after']}), minimo de sempre {h['free_int_min_ever']}")
    print(f"ancoras (ms desde o boot): esp_wifi_start={h['t_wifi_start_us']/1e3:.1f} esp_now_init={h['t_espnow_init_us']/1e3:.1f} "
          f"peer_pronto={h['t_peer_ready_us']/1e3:.1f} t0(1a oportunidade)={t0/1e3:.1f}")
    S = series(S_raw, "S")
    C = series(C_raw, "C")
    if not S:
        continue
    names = {0: "aceite", 1: "backpressure", 2: "no_mem", 3: "outro_erro"}
    cnt = {k: sum(1 for e in S if e[1] == k) for k in names}
    print(f"SEND: {len(S)} oportunidades em {(S[-1][0]-S[0][0])/1e6:.2f} s -> " +
          " ".join(f"{names[k]}={cnt[k]}" for k in names))
    dts = [(b[0] - a[0]) for a, b in zip(S, S[1:])]
    print(f"  delta oportunidade (us): p50={q(dts,.5)} p95={q(dts,.95)} p99={q(dts,.99)} max={max(dts)} "
          f"| >5,3 ms: {sum(x > 5324 for x in dts)}  >10 ms: {sum(x > 10000 for x in dts)}  >50 ms: {sum(x > 50000 for x in dts)}")
    calls = [e[3] for e in S if e[1] != 1]
    print(f"  duracao esp_now_send (us): p50={q(calls,.5)} p95={q(calls,.95)} p99={q(calls,.99)} max={max(calls) if calls else '-'} "
          f"| >1 ms: {sum(x > 1000 for x in calls)}")
    if C:
        dtc = [(b[0] - a[0]) for a, b in zip(C, C[1:])]
        fails = sum(1 for e in C if e[1] != 0)
        print(f"CB: {len(C)} callbacks (fail={fails})")
        print(f"  delta callback (us): p10={q(dtc,.1)} p50={q(dtc,.5)} p90={q(dtc,.9)} p99={q(dtc,.99)} max={max(dtc)} "
              f"| <300 us (rajada): {sum(x < 300 for x in dtc)}  >10 ms: {sum(x > 10000 for x in dtc)}  "
              f">20 ms: {sum(x > 20000 for x in dtc)}  >50 ms: {sum(x > 50000 for x in dtc)}")
    # por segundo desde t0
    print("  por segundo desde t0:  s | oport acc bp err | cb  | dt_cb p50/p99/max (ms) | gaps_cb>20ms | call_us p99")
    for sec in range(0, (h["window_ms"] + 999) // 1000):
        lo, hi = t0 + sec * 1e6, t0 + (sec + 1) * 1e6
        sw = [e for e in S if lo <= e[0] < hi]
        cw = [e for e in C if lo <= e[0] < hi]
        dd = [(b[0] - a[0]) for a, b in zip(cw, cw[1:])]
        cl = [e[3] for e in sw if e[1] != 1]
        print(f"   {sec:3d} | {len(sw):4d} {sum(e[1]==0 for e in sw):4d} {sum(e[1]==1 for e in sw):4d} "
              f"{sum(e[1]>=2 for e in sw):3d} | {len(cw):4d} | "
              f"{(q(dd,.5)/1e3 if dd else float('nan')):5.1f}/{(q(dd,.99)/1e3 if dd else float('nan')):5.1f}/"
              f"{(max(dd)/1e3 if dd else float('nan')):6.1f} | {sum(x > 20000 for x in dd):3d} | "
              f"{q(cl,.99) if cl else '-'}")
    # periodicidade: contagem de callbacks em bins de 10 ms, autocorrelacao normalizada
    if C:
        nb = int((C[-1][0] - t0) / 10000) + 1
        bins = [0] * nb
        for e in C:
            bins[int((e[0] - t0) / 10000)] += 1
        mu = sum(bins) / nb
        var = sum((x - mu) ** 2 for x in bins) / nb or 1
        ac = []
        for lag in range(2, min(300, nb // 2)):
            c = sum((bins[i] - mu) * (bins[i + lag] - mu) for i in range(nb - lag)) / ((nb - lag) * var)
            ac.append((lag * 10, c))
        top = sorted(ac, key=lambda x: -x[1])[:6]
        print(f"  periodicidade (autocorrelacao da contagem de callbacks/10 ms, lags 20-3000 ms): picos " +
              ", ".join(f"{lag} ms={c:+.2f}" for lag, c in top))
        near1s = [c for lag, c in ac if 900 <= lag <= 1100]
        if near1s:
            print(f"  autocorrelacao maxima perto de 1 s (900-1100 ms): {max(near1s):+.2f}")
    if raster:
        # raster de 300 ms no 2o segundo e no ultimo segundo: 1 caracter = 2 ms
        for label, start in (("inicio (t0+1,0 s)", t0 + 1_000_000), ("fim (t0+janela-1,3 s)", t0 + h["window_ms"] * 1000 - 1_300_000)):
            w = 150
            rows = {"oport": [" "] * w, "aceite": [" "] * w, "bp": [" "] * w, "cb": [" "] * w}
            for e in S:
                k = int((e[0] - start) / 2000)
                if 0 <= k < w:
                    rows["oport"][k] = "|"
                    rows["aceite" if e[1] == 0 else "bp"][k] = "|"
            for e in C:
                k = int((e[0] - start) / 2000)
                if 0 <= k < w:
                    rows["cb"][k] = "|"
            print(f"  raster {label}, 300 ms, 1 caracter = 2 ms:")
            for r in ("oport", "aceite", "bp", "cb"):
                print(f"    {r:6s} {''.join(rows[r])}")
