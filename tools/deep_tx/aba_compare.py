#!/usr/bin/env python3
"""ABA: por boot (fronteira = banner ESP-ROM no tx.log), tempo real de recuperacao + metricas do trace.
Recuperacao: mesma janela degradada do classify_sd.py (5 amostras LINKTX consecutivas, mediana cb_lat_avg
> 5 ms E soma delta backpressure >= 25); t_rec = uptime do fim da ultima janela degradada; se a ultima
janela observada ainda estiver degradada -> 'nao recuperou (>= uptime observado)'.
Uso: aba_compare.py <rotulo>=<dir> ..."""
import base64
import math
import re
import statistics
import struct
import sys
import zlib

KV = re.compile(r"\b([a-z_0-9]+)=(-?\d+)")
H = re.compile(r"^H=([\d.]+) \| (.*)$")


def q(v, p):
    v = sorted(v)
    return v[min(int(len(v) * p), len(v) - 1)] if v else float("nan")


def boots_of(d):
    boots, cur, dump = [], None, None
    for line in open(f"{d}/tx.log", errors="replace"):
        m = H.match(line.rstrip("\r\n"))
        if not m:
            continue
        body = m.group(2)
        if "ESP-ROM:" in body:
            cur = {"samples": [], "S": {}, "C": {}, "hdr": None, "bad": 0}
            boots.append(cur)
            continue
        if cur is None:
            continue
        if "LINKTX t=" in body:
            x = {k: int(v) for k, v in KV.findall(body)}
            if "cb_lat_avg_us" in x and "backpressure_drop" in x and "api_accept" in x:
                cur["samples"].append(x)
        tm = re.search(r"TRACE (BEGIN|S|C)\b ?(.*)$", body)
        if tm:
            k, rest = tm.group(1), tm.group(2)
            if k == "BEGIN":
                cur["hdr"] = {a: int(b) for a, b in re.findall(r"(\w+)=(-?\d+)", rest)}
            else:
                p = rest.split()
                try:
                    raw = base64.b64decode(p[3])
                    if (zlib.crc32(raw) & 0xFFFFFFFF) == int(p[2], 16):
                        cur[k][int(p[0])] = raw
                    else:
                        cur["bad"] += 1
                except Exception:  # noqa: BLE001
                    cur["bad"] += 1
    return boots


def recovery(s):
    s = sorted({x["uptime_s"]: x for x in s}.values(), key=lambda x: x["uptime_s"])
    deg_end, last_deg = None, False
    for i in range(5, len(s)):
        w = s[i - 4:i + 1]
        if w[-1]["uptime_s"] - w[0]["uptime_s"] != 4 or s[i - 5]["uptime_s"] != w[0]["uptime_s"] - 1:
            continue
        d = statistics.median(x["cb_lat_avg_us"] for x in w) > 5000 and w[-1]["backpressure_drop"] - s[i - 5]["backpressure_drop"] >= 25
        if d:
            deg_end = w[-1]["uptime_s"]
        last_deg = d
    if not s:
        return "sem dados"
    if last_deg:
        return f"NAO (>= {s[-1]['uptime_s']} s)"
    return f"{deg_end or 0} s"


def trace(b):
    if not b["hdr"]:
        return None
    S_raw = b"".join(b["S"][k] for k in sorted(b["S"]))
    C_raw = b"".join(b["C"][k] for k in sorted(b["C"]))
    S = [struct.unpack_from("<IBBH", S_raw, i) for i in range(0, len(S_raw), 8)]
    C = [struct.unpack_from("<I", C_raw, i)[0] for i in range(0, len(C_raw), 8)]
    dts = [y[0] - x[0] for x, y in zip(S, S[1:])]
    calls = [e[3] for e in S if e[1] != 1]
    dtc = [y - x for x, y in zip(C, C[1:])]
    groups, size = [], 1
    for x, y in zip(C, C[1:]):
        if y - x < 5000:
            size += 1
        else:
            groups.append(size)
            size = 1
    groups.append(size)
    ends = [y for x, y in zip(C, C[1:]) if y - x > 20000]
    R = p = float("nan")
    if len(ends) >= 3:
        cs = sum(math.cos(2 * math.pi * (t % 102400) / 102400) for t in ends)
        sn = sum(math.sin(2 * math.pi * (t % 102400) / 102400) for t in ends)
        R = math.hypot(cs, sn) / len(ends)
        p = math.exp(-len(ends) * R * R)
    return {"ok": b["bad"] == 0, "dS_max": max(dts), "call_max": max(calls), "dC_p50": q(dtc, .5), "dC_p90": q(dtc, .9),
            "dC_p99": q(dtc, .99), "dC_max": max(dtc), "g20": sum(x > 20000 for x in dtc), "g50": sum(x > 50000 for x in dtc),
            "g100": sum(x > 100000 for x in dtc), "grp50": q(groups, .5), "grp90": q(groups, .9),
            "acc": sum(1 for e in S if e[1] == 0), "bp": sum(1 for e in S if e[1] == 1), "R": R, "p": p}


print(f"{'fase':22s} {'boot':>4} {'recuperacao':>18} {'int':>3} {'dS_max':>6} {'call':>5} {'dC_p50':>6} {'dC_p90':>6} "
      f"{'dC_p99':>7} {'dC_max':>7} {'g>20':>5} {'g>50':>5} {'g>100':>5} {'grp50/90':>8} {'aceites':>7} {'bp':>5} {'R102':>5} {'p102':>8}")
for arg in sys.argv[1:]:
    label, d = arg.rsplit("=", 1)
    for i, b in enumerate(boots_of(d), 1):
        t = trace(b)
        rec = recovery(b["samples"])
        if not t:
            print(f"{label:22s} {i:>4} {rec:>18}   (sem dump)")
            continue
        print(f"{label:22s} {i:>4} {rec:>18} {'ok' if t['ok'] else 'MAU':>3} {t['dS_max']:>6} {t['call_max']:>5} {t['dC_p50']:>6} "
              f"{t['dC_p90']:>6} {t['dC_p99']:>7} {t['dC_max']:>7} {t['g20']:>5} {t['g50']:>5} {t['g100']:>5} "
              f"{str(t['grp50']) + '/' + str(t['grp90']):>8} {t['acc']:>7} {t['bp']:>5} {t['R']:>5.2f} {t['p']:>8.1e}")
