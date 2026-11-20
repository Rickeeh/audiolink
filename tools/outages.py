#!/usr/bin/env python3
"""Lista interrupcoes longas (rajadas de perda) por run, com hora aproximada.

O rx_time_us de cada registo e o esp_timer do RX (tempo desde o boot do RX,
que reinicia ao abrir a captura). A hora de relogio e estimada como
<hora de criacao do ficheiro> + tempo de boot: erro tipico de 1-3 s.

  outages.py results/run015*.rx.raw.txt [--min-packets 50]
"""
import argparse
import os
import sys
from datetime import datetime, timedelta

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import analyze_bench as ab


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rx_log")
    ap.add_argument("--min-packets", type=int, default=50)
    a = ap.parse_args()

    boot_wall = datetime.fromtimestamp(os.stat(a.rx_log).st_birthtime) - timedelta(seconds=1)
    print(f"(hora de referencia: boot do RX ~ {boot_wall:%H:%M:%S}; erro tipico 1-3 s)\n")
    print("| hora ~ | step | ciclo | PHY | interrupcao (pacotes) | duracao ~ s | perdas no run |")
    print("|---|---|---|---|---|---|---|")
    for d in ab.parse_rx_log(a.rx_log):
        if not d.records or d.tx_phymode < 0:
            continue
        by = {}
        for r in d.records:
            by.setdefault(r.seq, r)
        seqs = sorted(by)
        interval_s = d.tx_interval_us / 1e6
        total_lost = (max(seqs) - min(seqs) + 1) - len(seqs)
        for prev, cur in zip(seqs, seqs[1:]):
            gap = cur - prev - 1
            if gap >= a.min_packets:
                t_us = by[prev].rx_time_us
                wall = boot_wall + timedelta(microseconds=t_us)
                print(f"| {wall:%H:%M:%S} | {d.step} | {d.cycle} | {ab.phy_label(d)} | {gap} | "
                      f"{gap * interval_s:.1f} | {total_lost} |")


if __name__ == "__main__":
    main()
