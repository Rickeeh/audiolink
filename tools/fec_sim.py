#!/usr/bin/env python3
"""Simulacao offline de FEC + jitter buffer sobre traces reais do bench.

Cada pacote do trace (480 B a cada 2 ms = 1.92 Mbps = 2 ms de audio) e tratado
como um "slot" do canal com o destino que realmente teve. Nao ha pacotes de
paridade no trace, por isso a paridade ocupa slots do proprio trace: mede-se o
que o canal FARIA a um esquema com essa redundancia (aprox.: o debito util
desses esquemas e k/(k+r) do do trace).

Modelo de reproducao: cada pacote tem prazo tx_i + base + D (D = jitter buffer),
com base = menor atraso (rx-tx) observado (cancela o offset entre relogios).
Pacote tardio = perdido para o audio. Metrica = "eventos de silencio": runs
consecutivos de pacotes de audio nao reproduziveis, apos recuperacao por FEC.

Esquemas: none | xorK.M (K dados + 1 paridade XOR, entrelacado M) | rsK+R (ideal MDS:
recupera qualquer <=R perdas em cada bloco de K+R slots).
"""
import argparse
import sys
from collections import namedtuple

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import analyze_bench as ab

INTERVAL_US = 2000


def playable_none(n, ok):
    return list(ok)


def run_events(play):
    ev, cur = [], 0
    for p in play:
        if p:
            if cur:
                ev.append(cur)
            cur = 0
        else:
            cur += 1
    if cur:
        ev.append(cur)
    return ev


def xor_scheme(n, ok, arr, dl, k, m):
    """K dados (entrelacados de M em M) + 1 paridade cujo destino e o do slot
    seguinte ao bloco. Recupera 1 perda por grupo se o resto do grupo+paridade
    chegar antes do prazo do pacote em falta."""
    play = list(ok)
    block = k * m
    for b0 in range(0, n - block - m, block):
        for r in range(m):
            idx = [b0 + r + j * m for j in range(k)]
            par = b0 + block + r  # slot que faz de paridade (destino real do trace)
            miss = [i for i in idx if not ok[i]]
            if len(miss) == 1 and ok[par]:
                last = max(arr[i] for i in idx if ok[i])
                last = max(last, arr[par])
                if last <= dl[miss[0]]:
                    play[miss[0]] = True
    return play


def rs_scheme(n, ok, arr, dl, k, rr):
    """Blocos de K dados + R paridade contiguos; recupera qualquer <=R perdas do bloco
    se o ultimo pacote necessario chegar antes do prazo. Devolve so os slots de DADOS
    (a paridade nao e audio), por ordem, para os eventos de silencio serem contiguos."""
    play = list(ok)
    blk = k + rr
    out = []
    for b0 in range(0, n - blk + 1, blk):
        slots = list(range(b0, b0 + blk))
        lost = [i for i in slots if not ok[i]]
        if 0 < len(lost) <= rr:
            last = max(arr[i] for i in slots if ok[i])
            for i in lost:
                if i < b0 + k and last <= dl[i]:
                    play[i] = True
        out.extend(play[b0:b0 + k])
    return out


def summarize(play, n):
    ev = run_events(play)
    lost = sum(ev)
    return lost, len(ev), (max(ev) * INTERVAL_US / 1000 if ev else 0)


POOL = {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rx_logs", nargs="+")
    ap.add_argument("--jitter-ms", type=float, default=20.0)
    ap.add_argument("--payload", type=int, default=480)
    ap.add_argument("--min-lost", type=int, default=0, help="so runs com >= N perdas brutas")
    ap.add_argument("--pool-only", action="store_true", help="so a tabela agregada")
    ap.add_argument("--phy", nargs="*", help="so PHY cujo rotulo contem um destes textos (ex.: '11g 6M' 'HT20 MCS0')")
    a = ap.parse_args()
    D = a.jitter_ms * 1000

    schemes = [("none", None), ("xor4.1", (4, 1)), ("xor4.3", (4, 3)), ("xor8.2", (8, 2)),
               ("rs8+2", ("rs", 8, 2)), ("rs8+4", ("rs", 8, 4))]
    print(f"jitter buffer D = {a.jitter_ms:.0f} ms. Celula = perdas% / eventos / maior evento (ms). "
          f"'raw' = so perdas+tardios, sem FEC.\n")
    print("| run | step | ciclo | PHY | " + " | ".join(s[0] for s in schemes) + " |")
    print("|---|---|---|---|" + "---|" * len(schemes))
    for path in a.rx_logs:
        tag = path.split("/")[-1].split(".")[0][:24]
        for d in ab.parse_rx_log(path):
            if d.payload_size != a.payload or not d.records or d.tx_phymode < 0:
                continue
            if a.phy and not any(x in ab.phy_label(d) for x in a.phy):
                continue
            by = {}
            for r in d.records:
                by.setdefault(r.seq, r)
            lo = min(by)  # o RX pode ter entrado a meio de um run: indexar a partir do 1o recebido
            hi = max(max(by), (d.planned - 1) if d.planned else 0)
            n = hi - lo + 1
            if lo > 0 and not d.planned:
                pass
            base = min(r.rx_time_us - r.tx_time_us for r in by.values())
            arr = [0] * n
            dl = [0] * n
            ok = [False] * n
            # t0: ajuste linear tx_i ~ t0 + i*INTERVAL (mediana, robusta a atrasos)
            t0 = sorted(r.tx_time_us - (s - lo) * INTERVAL_US for s, r in by.items())[len(by) // 2]
            for i in range(n):
                dl[i] = t0 + i * INTERVAL_US + base + D
                r = by.get(i + lo)
                if r:
                    arr[i] = r.rx_time_us
                    ok[i] = (r.rx_time_us <= dl[i])
            raw_lost = n - sum(ok)
            if raw_lost < a.min_lost:
                continue
            cells = []
            for name, cfg in schemes:
                if cfg is None:
                    play = playable_none(n, ok)
                elif cfg[0] == "rs":
                    play = rs_scheme(n, ok, arr, dl, cfg[1], cfg[2])
                else:
                    play = xor_scheme(n, ok, arr, dl, cfg[0], cfg[1])
                lost, ne, mx = summarize(play, n)
                cells.append(f"{100.0*lost/n:.2f}% / {ne} / {mx:.0f}")
            if not a.pool_only:
                print(f"| {tag} | {d.step} | {d.cycle} | {ab.phy_label(d)} | " + " | ".join(cells) + " |")
            cls = "limpo(<5% brutas)" if 100.0 * raw_lost / n < 5 else "apagao(>=5% brutas)"
            for j, (name, cfg) in enumerate(schemes):
                if cfg is None:
                    play = playable_none(n, ok)
                elif cfg[0] == "rs":
                    play = rs_scheme(n, ok, arr, dl, cfg[1], cfg[2])
                else:
                    play = xor_scheme(n, ok, arr, dl, cfg[0], cfg[1])
                ev = run_events(play)
                P = POOL.setdefault((cls, name), dict(n=0, lost=0, ev=0, mx=0.0))
                P["n"] += n; P["lost"] += sum(ev); P["ev"] += len(ev)
                P["mx"] = max(P["mx"], max(ev) * INTERVAL_US / 1000 if ev else 0)
    print_pool(a.jitter_ms)


def print_pool(D_ms):
    over = {"none": 0, "xor4.1": 25, "xor4.3": 25, "xor8.2": 12.5, "rs8+2": 25, "rs8+4": 50}
    print(f"\n### Agregado (jitter buffer {D_ms:.0f} ms) — todos os runs de 480 B fornecidos\n")
    print("| classe do run | esquema | overhead | minutos de audio | perdas % | eventos de silencio | eventos/min | maior evento ms |")
    print("|---|---|---|---|---|---|---|---|")
    for (cls, name), P in sorted(POOL.items()):
        mins = P["n"] * INTERVAL_US / 60e6
        print(f"| {cls} | {name} | {over[name]}% | {mins:.1f} | {100.0*P['lost']/P['n']:.3f}% | {P['ev']} | {P['ev']/mins:.2f} | {P['mx']:.0f} |")


if __name__ == "__main__":
    main()
