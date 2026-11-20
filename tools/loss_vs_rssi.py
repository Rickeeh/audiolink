#!/usr/bin/env python3
"""Perdas em funcao do RSSI por (PHY, payload), agregando varios rx.raw.txt.

O RSSI e o eixo que realmente controla a fiabilidade; a geometria (distancia,
paredes, orientacao) so o influencia indiretamente. Agrupar por RSSI permite
juntar runs de posicoes diferentes e ler a *margem de ligacao* de cada MCS.

  loss_vs_rssi.py results/run009*.rx.raw.txt results/run011*.rx.raw.txt ...
"""
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import analyze_bench as ab

BINS = [(-30, 99), (-50, -30), (-60, -50), (-65, -60), (-70, -65), (-75, -70), (-99, -75)]


def label(lo, hi):
    return f">{lo}" if hi == 99 else (f"<={hi}" if lo == -99 else f"{lo}..{hi}")


def main(paths):
    cells = defaultdict(list)  # (phy, payload, tx_dbm, bin) -> [loss%]
    for p in paths:
        for d in ab.parse_rx_log(p):
            if not d.records or d.tx_phymode < 0:
                continue
            by = {}
            for r in d.records:
                by.setdefault(r.seq, r)
            lo_seq = min(by)
            exp = max(max(by) - lo_seq + 1, (d.planned - lo_seq) if d.planned else 0)
            loss = 100.0 * (exp - len(by)) / exp
            rs = [r.rssi for r in d.records if r.rssi]
            if not rs:
                continue
            rssi = statistics.mean(rs)
            for b in BINS:
                if b[0] < rssi <= b[1]:
                    cells[(ab.phy_label(d), d.payload_size, d.tx_power_qdbm / 4, b)].append(loss)
                    break
    keys = sorted({(k[0], k[1], k[2]) for k in cells}, key=lambda k: (k[1], k[0], k[2]))
    print("| PHY | payload B | TX dBm | " + " | ".join(label(*b) for b in BINS) + " |")
    print("|---|---|---|" + "---|" * len(BINS))
    for phy, pl, pw in keys:
        row = []
        for b in BINS:
            v = cells.get((phy, pl, pw, b))
            row.append(f"{statistics.median(v):.2f}% (n={len(v)})" if v else "")
        print(f"| {phy} | {pl} | {pw:.0f} | " + " | ".join(row) + " |")
    print("\n(RSSI medio por run em dBm; celula = mediana das perdas dos runs nessa faixa, n = numero de runs)")


if __name__ == "__main__":
    main(sys.argv[1:])
