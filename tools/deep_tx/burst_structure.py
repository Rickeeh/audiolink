#!/usr/bin/env python3
"""Descritivo: estrutura de rajadas/lacunas das callbacks em cada dump TRACE.
Grupo = callbacks consecutivas com delta < 5 ms (cerca de 2 periodos de audio). Reporta tamanho dos
grupos, lacunas entre grupos, histograma de delta_cb, e alinhamento das lacunas a periodos
candidatos (102,4 ms = 100 TU, 409,6 ms = 400 TU, 1000 ms) por teste de Rayleigh (R ~ 0 uniforme,
R ~ 1 alinhado; p ~ exp(-n R^2)). Uso: burst_structure.py <tx.log> [<label>...]"""
import base64
import math
import re
import struct
import sys
import zlib

path = sys.argv[1]
dumps, cur = [], None
for line in open(path, errors="replace"):
    m = re.search(r"TRACE (BEGIN|END|S|C)\b ?(.*)$", line.rstrip("\r\n"))
    if not m:
        continue
    k, rest = m.group(1), m.group(2)
    if k == "BEGIN":
        cur = {"hdr": {a: int(b) for a, b in re.findall(r"(\w+)=(-?\d+)", rest)}, "C": {}}
    elif k == "END" and cur:
        dumps.append(cur)
        cur = None
    elif k == "C" and cur is not None:
        p = rest.split()
        raw = base64.b64decode(p[3])
        if (zlib.crc32(raw) & 0xFFFFFFFF) == int(p[2], 16):
            cur["C"][int(p[0])] = raw


def rayleigh(times, period):
    if len(times) < 3:
        return float("nan"), float("nan")
    c = sum(math.cos(2 * math.pi * (t % period) / period) for t in times)
    s = sum(math.sin(2 * math.pi * (t % period) / period) for t in times)
    R = math.hypot(c, s) / len(times)
    return R, math.exp(-len(times) * R * R)


for i, d in enumerate(dumps, 1):
    raw = b"".join(d["C"][k] for k in sorted(d["C"]))
    t = [struct.unpack_from("<I", raw, j)[0] for j in range(0, len(raw), 8)]
    dt = [b - a for a, b in zip(t, t[1:])]
    edges = [0, 1000, 2000, 3000, 5000, 10000, 20000, 50000, 90000, 115000, 200000, 10**9]
    hist = [sum(1 for x in dt if lo <= x < hi) for lo, hi in zip(edges, edges[1:])]
    groups, gaps, starts = [], [], [t[0]]
    size = 1
    for a, b in zip(t, t[1:]):
        if b - a < 5000:
            size += 1
        else:
            groups.append(size)
            gaps.append(b - a)
            starts.append(b)
            size = 1
    groups.append(size)
    gs = sorted(groups)
    gp = sorted(gaps)
    q = lambda v, p: v[min(int(len(v) * p), len(v) - 1)] if v else float("nan")  # noqa: E731
    print(f"\n== dump {i}: {len(t)} callbacks ==")
    print("  hist delta_cb: " + "  ".join(f"[{lo/1000:g}-{hi/1000:g} ms)={n}" for lo, hi, n in
                                        zip(edges, edges[1:], hist) if n) .replace("1e+06", "inf"))
    print(f"  grupos (delta<5 ms): {len(groups)}  tamanho p50={q(gs,.5)} p90={q(gs,.9)} max={gs[-1]}  "
          f"| lacunas entre grupos (ms): p10={q(gp,.1)/1e3:.1f} p50={q(gp,.5)/1e3:.1f} p90={q(gp,.9)/1e3:.1f} "
          f"max={(gp[-1]/1e3 if gp else float('nan')):.1f}")
    long_gap_ends = [b for a, b in zip(t, t[1:]) if b - a > 20000]
    for per in (102400, 409600, 1000000):
        R, p = rayleigh(long_gap_ends, per)
        print(f"  alinhamento dos fins de lacuna >20 ms a {per/1000:g} ms: n={len(long_gap_ends)} R={R:.2f} p~{p:.1e}")
