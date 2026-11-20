#!/usr/bin/env python3
"""Resume os scans de espectro do modo AUDIOLINK_SCAN_ONLY.

Linhas: `SCAN t=<s> total=<n> ch<C>:<n_aps>,<rssi_max> ...` (sem SSIDs/BSSIDs).

Por canal: fracao de scans em que ha redes, numero medio, RSSI maximo mediano.
"Nivel de interferencia" ~ melhor RSSI de qualquer rede com penalizacao por
distancia de canal (0/3/8/15/25 dB para 0..4 canais de diferenca; canais Wi-Fi 20 MHz
sobrepoem-se ate ~4 canais). E uma APROXIMACAO: so ve APs que emitem beacons, nao ve
trafego de clientes nem fontes nao-Wi-Fi (micro-ondas, Bluetooth, ...).
"""
import re
import statistics
import sys

PEN = {0: 0, 1: 3, 2: 8, 3: 15, 4: 25}


def main(path):
    scans = []
    for line in open(path, errors="replace"):
        m = re.match(r"SCAN t=(\d+) total=(\d+)(.*)", line.strip())
        if not m:
            continue
        chs = {int(c): (int(n), int(r)) for c, n, r in re.findall(r"ch(\d+):(\d+),(-?\d+)", m.group(3))}
        scans.append(chs)
    if not scans:
        print("sem linhas SCAN")
        return
    print(f"{len(scans)} scans\n")
    print("| canal | scans com redes | redes (media) | RSSI max mediano | nivel de interferencia ~ (dBm) |")
    print("|---|---|---|---|---|")
    levels = {}
    for c in range(1, 14):
        present = [s[c] for s in scans if c in s]
        lv = []
        for s in scans:
            best = -127
            for c2, (n, r) in s.items():
                d = abs(c - c2)
                if d in PEN:
                    best = max(best, r - PEN[d])
            lv.append(best)
        levels[c] = statistics.median(lv)
        frac = 100.0 * len(present) / len(scans)
        mean_n = statistics.mean(s.get(c, (0, 0))[0] for s in scans)
        med_r = statistics.median(r for _, r in present) if present else None
        print(f"| {c} | {frac:.0f}% | {mean_n:.1f} | {('%.0f' % med_r) if med_r is not None else '-'} | {levels[c]:.0f} |")
    best = sorted(levels, key=lambda c: levels[c])[:3]
    print(f"\nCanais menos interferidos (aprox.): {', '.join(str(c) for c in best)}")


if __name__ == "__main__":
    main(sys.argv[1])
