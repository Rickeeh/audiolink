#!/usr/bin/env python3
"""
AUDIOLINK Fase 1 - analise de um dump do bench de radio.

Le um ficheiro de log capturado da consola serial do RX (ex.: com
`idf.py monitor > rx_capture.txt` e depois Ctrl-] para sair, ou qualquer
outro terminal serial que grave para ficheiro), extrai o bloco entre
===AUDIOLINK_BENCH_DUMP_BEGIN=== e ===AUDIOLINK_BENCH_DUMP_END=== e
calcula:

  - debito sustentado (bps e pkt/s)
  - perdas (contagem e %) e distribuicao do tamanho das rajadas de perda
  - duplicados e pacotes fora de ordem
  - jitter de chegada: p50/p99/p99.9 (metodo RFC3550, nao precisa de
    relogios sincronizados - ver nota em calc_jitter())
  - RSSI (min/media/p10) quando disponivel (so ESP-NOW)

Uso:
  python3 analyze_bench.py --rx-log captura_rx.txt [--tx-log captura_tx.txt]
  python3 analyze_bench.py --rx-log captura_rx.txt --out relatorio.md

O --tx-log e opcional: se dado, cruza sent_ok/send_fail do TX (que inclui
as falhas de ESP-NOW depois de esgotadas as retransmissoes MAC) com o
que o RX realmente recebeu.
"""
import argparse
import re
import statistics
import sys
from dataclasses import dataclass, field


DUMP_BEGIN = "===AUDIOLINK_BENCH_DUMP_BEGIN==="
DUMP_END = "===AUDIOLINK_BENCH_DUMP_END==="

HEADER_RE = re.compile(
    r"run_id=(?P<run_id>\d+)\s+records=(?P<records>\d+)\s+capacity=(?P<capacity>\d+)\s+"
    r"overflowed=(?P<overflowed>\d+)"
    r"(?:\s+transport=(?P<transport>\S+)\s+channel=(?P<channel>\d+)\s+"
    r"payload_size=(?P<payload_size>\d+)\s+tx_interval_us=(?P<tx_interval_us>\d+))?"
)

TX_START_RE = re.compile(r"TX a arrancar: run_id=(?P<run_id>\d+) pkts=(?P<pkts>\d+)")

TX_SUMMARY_RE = re.compile(
    r"TX TERMINADO run_id=(?P<run_id>\d+) sent_ok=(?P<sent_ok>\d+) send_fail=(?P<send_fail>\d+)"
    r"(?: api_err=(?P<api_err>\d+) last_api_err=(?P<last_api_err>-?\d+))?"
)


@dataclass
class Record:
    seq: int
    tx_time_us: int
    rx_time_us: int
    rssi: int
    phy_rate: int
    mcs: int = 0
    flags: int = 0


@dataclass
class Dump:
    run_id: int
    declared_records: int
    capacity: int
    overflowed: bool
    transport: str = "?"
    channel: int = 0
    payload_size: int = 0
    tx_interval_us: int = 0
    tx_power_qdbm: int = 0
    tx_phymode: int = -1
    tx_rate: int = -1
    step: int = 0
    cycle: int = 0
    planned: int = 0
    proto: int = 0
    records: list = field(default_factory=list)


PHYMODE_NAMES = {0: "LR", 1: "11b", 2: "11g", 3: "11a", 4: "HT20", 5: "HT40", 6: "HE20", 7: "VHT20"}
RATE_NAMES = {0: "1M_L", 1: "2M_L", 2: "5.5M_L", 3: "11M_L", 5: "2M_S", 6: "5.5M_S", 7: "11M_S",
              8: "48M", 9: "24M", 10: "12M", 11: "6M", 12: "54M", 13: "36M", 14: "18M", 15: "9M"}
for _i in range(8):
    RATE_NAMES[0x10 + _i] = f"MCS{_i}_LGI"
    RATE_NAMES[0x18 + _i] = f"MCS{_i}_SGI"


def phy_label(d):
    if d.proto == 1 or d.tx_phymode == 255:
        return "UDP auto"
    if d.tx_phymode < 0:
        return "?"
    return f"{PHYMODE_NAMES.get(d.tx_phymode, d.tx_phymode)} {RATE_NAMES.get(d.tx_rate, d.tx_rate)}"


def parse_rx_log(path):
    with open(path, "r", errors="replace") as f:
        text = f.read()

    dumps = []
    pos = 0
    while True:
        start = text.find(DUMP_BEGIN, pos)
        if start == -1:
            break
        end = text.find(DUMP_END, start)
        if end == -1:
            print(f"AVISO: {DUMP_BEGIN} sem {DUMP_END} correspondente, a ignorar resto do ficheiro",
                  file=sys.stderr)
            break
        block = text[start + len(DUMP_BEGIN):end].strip().splitlines()
        pos = end + len(DUMP_END)

        if not block:
            continue
        kv = dict(re.findall(r"(\w+)=(\S+)", block[0]))
        if "run_id" not in kv:
            print(f"AVISO: cabecalho de dump nao reconhecido: {block[0]!r}", file=sys.stderr)
            continue

        d = Dump(
            run_id=int(kv["run_id"]),
            declared_records=int(kv.get("records", 0)),
            capacity=int(kv.get("capacity", 0)),
            overflowed=kv.get("overflowed") == "1",
            transport=kv.get("transport", "?"),
            channel=int(kv.get("channel", 0)),
            payload_size=int(kv.get("payload_size", 0)),
            tx_interval_us=int(kv.get("tx_interval_us", 0)),
            tx_power_qdbm=int(kv.get("tx_power_qdbm", 0)),
            tx_phymode=int(kv.get("tx_phymode", -1)),
            tx_rate=int(kv.get("tx_rate", -1)),
            step=int(kv.get("step", 0)),
            cycle=int(kv.get("cycle", 0)),
            planned=int(kv.get("planned", 0)),
            proto=int(kv.get("proto", 0)),
        )

        # block[1] deve ser a linha de cabecalho CSV "seq,tx_time_us,..."
        for line in block[2:]:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",")
            if len(parts) not in (5, 7):  # 5 = formato antigo, 7 = com mcs,flags
                continue
            try:
                d.records.append(Record(
                    seq=int(parts[0]), tx_time_us=int(parts[1]), rx_time_us=int(parts[2]),
                    rssi=int(parts[3]), phy_rate=int(parts[4]),
                    mcs=int(parts[5]) if len(parts) == 7 else 0,
                    flags=int(parts[6]) if len(parts) == 7 else 0,
                ))
            except ValueError:
                continue
        dumps.append(d)

    return dumps


def parse_tx_log(path):
    if not path:
        return {}
    with open(path, "r", errors="replace") as f:
        text = f.read()
    out = {}
    planned = {int(m.group("run_id")): int(m.group("pkts")) for m in TX_START_RE.finditer(text)}
    for m in TX_SUMMARY_RE.finditer(text):
        out[int(m.group("run_id"))] = {
            "planned_pkts": planned.get(int(m.group("run_id"))),
            "sent_ok": int(m.group("sent_ok")),
            "send_fail": int(m.group("send_fail")),
            "api_err": int(m.group("api_err") or 0),
            "last_api_err": int(m.group("last_api_err") or 0),
        }
    return out


def burst_histogram(missing_sorted):
    """Agrupa sequencias consecutivas de seq em falta em rajadas e conta
    quantas rajadas de cada comprimento ocorreram."""
    hist = {}
    if not missing_sorted:
        return hist
    run_len = 1
    for i in range(1, len(missing_sorted)):
        if missing_sorted[i] == missing_sorted[i - 1] + 1:
            run_len += 1
        else:
            hist[run_len] = hist.get(run_len, 0) + 1
            run_len = 1
    hist[run_len] = hist.get(run_len, 0) + 1
    return hist


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def calc_jitter(records_by_seq):
    """Jitter de interchegada ao estilo RFC 3550, para pares de seq
    consecutivos (seq, seq+1) ambos recebidos.

    Nao precisamos de relogios sincronizados entre TX e RX: o que
    importa e a VARIACAO do delta (rx[i]-rx[i-1]) - (tx[i]-tx[i-1]).
    Um offset fixo entre relogios cancela nesta subtracao; so entra
    deriva relativa dentro da janela do teste, que para dezenas de
    segundos e desprezavel comparado ao jitter de rede que queremos medir.
    """
    seqs = sorted(records_by_seq.keys())
    deltas = []
    for i in range(1, len(seqs)):
        if seqs[i] != seqs[i - 1] + 1:
            continue  # so pares consecutivos - um buraco de perda não é jitter
        a = records_by_seq[seqs[i - 1]]
        b = records_by_seq[seqs[i]]
        d_tx = b.tx_time_us - a.tx_time_us
        d_rx = b.rx_time_us - a.rx_time_us
        deltas.append(abs(d_rx - d_tx))
    deltas.sort()
    return deltas


ROWS = []


def analyze_dump(d: Dump, tx_stats: dict):
    lines = []
    lines.append(f"## step={d.step} cycle={d.cycle} run_id={d.run_id} ({d.transport}, canal {d.channel}, "
                  f"{phy_label(d)}, TX {d.tx_power_qdbm/4:.1f} dBm, "
                  f"payload={d.payload_size}B, intervalo TX={d.tx_interval_us}us)")

    if not d.records:
        lines.append("(sem registos)")
        return "\n".join(lines)

    records_by_seq = {}
    duplicates = 0
    for r in d.records:
        if r.seq in records_by_seq:
            duplicates += 1
        else:
            records_by_seq[r.seq] = r

    min_seq = min(records_by_seq)
    max_seq = max(records_by_seq)
    received = len(records_by_seq)

    expected_total = max_seq - min_seq + 1
    if d.planned:
        # total planeado vindo do marcador de fim: ve tambem perdas no fim do run
        expected_total = max(expected_total, d.planned - min_seq)
    if d.run_id in tx_stats:
        # se temos o log do TX, a verdade sobre quantos pacotes DEVERIAM
        # ter chegado e o total planeado por ele, nao o maior seq visto no
        # RX (que subestima perdas mesmo no fim do teste).
        # (sent_ok NAO serve: inclui os 10 marcadores de fim e conta callbacks)
        planned = tx_stats[d.run_id].get("planned_pkts")
        if planned:
            expected_total = max(expected_total, planned)

    missing = sorted(set(range(min_seq, min_seq + expected_total)) - set(records_by_seq.keys()))
    loss_pct = 100.0 * len(missing) / expected_total if expected_total else 0.0

    # reordenacao: percorre por ordem de CHEGADA (rx_time_us) e conta
    # quantas vezes o seq desceu face ao anterior.
    by_arrival = sorted(d.records, key=lambda r: r.rx_time_us)
    reorder_events = 0
    last_seq = None
    for r in by_arrival:
        if last_seq is not None and r.seq < last_seq:
            reorder_events += 1
        last_seq = r.seq

    span_us = max(r.rx_time_us for r in d.records) - min(r.rx_time_us for r in d.records)
    span_s = span_us / 1e6 if span_us > 0 else float("nan")
    pkt_per_s = received / span_s if span_s else float("nan")
    bps = (received * d.payload_size * 8) / span_s if span_s and d.payload_size else float("nan")

    jitter = calc_jitter(records_by_seq)

    rssi_vals = sorted(r.rssi for r in d.records if r.rssi != 0)

    lines.append(f"- pacotes recebidos: {received} (seq {min_seq}..{max_seq}, esperados ~{expected_total})")
    lines.append(f"- perdas: {len(missing)} ({loss_pct:.3f}%)")
    lines.append(f"- duplicados: {duplicates}")
    lines.append(f"- fora de ordem (eventos): {reorder_events}")
    lines.append(f"- janela observada: {span_s:.2f}s")
    lines.append(f"- debito sustentado: {pkt_per_s:.1f} pkt/s"
                 + (f" = {bps/1e6:.3f} Mbps" if bps == bps else ""))
    if d.run_id in tx_stats:
        ts = tx_stats[d.run_id]
        tx_loss = ts["send_fail"]
        tx_total = ts["sent_ok"] + ts["send_fail"]
        lines.append(f"- TX (do log local): planeados={ts.get('planned_pkts')} (+10 marcadores de fim) "
                     f"cb_ok={ts['sent_ok']} cb_fail={ts['send_fail']} api_err={ts['api_err']}"
                     + (f" ({100.0*tx_loss/tx_total:.3f}% falhas apos retransmissoes MAC)"
                        if tx_total else ""))

    if jitter:
        lines.append(f"- jitter |Δ| (us): p50={percentile(jitter,50):.1f} "
                     f"p99={percentile(jitter,99):.1f} p99.9={percentile(jitter,99.9):.1f} "
                     f"max={jitter[-1]:.1f}")
    else:
        lines.append("- jitter: sem pares de seq consecutivos suficientes para calcular")

    if rssi_vals:
        lines.append(f"- RSSI (dBm): min={rssi_vals[0]} p10={percentile(rssi_vals,10):.1f} "
                     f"media={statistics.mean(rssi_vals):.1f} max={rssi_vals[-1]}")
    else:
        lines.append("- RSSI: nao disponivel (normal em SoftAP+UDP nesta versao do bench)")

    # PHY realmente observada no RX (prova de que a taxa configurada foi aplicada)
    if any(r.flags or r.mcs or r.phy_rate for r in d.records):
        from collections import Counter
        SIG = {0: "11bg", 1: "HT", 3: "VHT"}
        combos = Counter(
            (SIG.get(r.flags & 3, "?"), r.mcs if (r.flags & 3) else None,
             "40MHz" if (r.flags >> 2) & 1 else "20MHz", "SGI" if (r.flags >> 3) & 1 else "LGI",
             None if (r.flags & 3) else r.phy_rate)
            for r in d.records)
        parts = []
        for (sig, mcs, bw, gi, rate), n in combos.most_common(4):
            desc = f"{sig} MCS{mcs} {bw} {gi}" if sig != "11bg" else f"{sig} rate_idx={rate} {bw}"
            parts.append(f"{desc}: {100.0*n/len(d.records):.1f}%")
        lines.append("- PHY observada no RX: " + "; ".join(parts))

    ROWS.append(dict(
        run_id=d.run_id, step=d.step, cycle=d.cycle, phy=phy_label(d), pwr=d.tx_power_qdbm / 4, payload=d.payload_size,
        target=(8.0 * d.payload_size * 1e6 / d.tx_interval_us / 1e6) if d.tx_interval_us else float("nan"),
        mbps=bps / 1e6 if bps == bps else float("nan"), loss=loss_pct, lost=len(missing),
        jp99=percentile(jitter, 99) if jitter else float("nan"),
        rssi=statistics.mean(rssi_vals) if rssi_vals else None))
    hist = burst_histogram(missing)
    if hist:
        hist_str = ", ".join(f"{length}:{count}" for length, count in sorted(hist.items()))
        lines.append(f"- distribuicao de rajadas de perda (comprimento:contagem): {hist_str}")
    else:
        lines.append("- distribuicao de rajadas de perda: nenhuma perda")

    if d.overflowed:
        lines.append("- **AVISO: o buffer do RX encheu durante este run - aumenta "
                      "AUDIOLINK_RX_RECORD_CAPACITY ou encurta a duracao do teste antes "
                      "de confiar nestes numeros**")

    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rx-log", required=True, help="captura da consola serial do RX")
    ap.add_argument("--tx-log", help="captura da consola serial do TX (opcional)")
    ap.add_argument("--out", help="ficheiro markdown de saida (default: stdout)")
    args = ap.parse_args()

    dumps = parse_rx_log(args.rx_log)
    if not dumps:
        print(f"Nenhum dump encontrado em {args.rx_log} "
              f"(procurei por {DUMP_BEGIN} ... {DUMP_END})", file=sys.stderr)
        sys.exit(1)

    tx_stats = parse_tx_log(args.tx_log)

    out_lines = [f"# AUDIOLINK Fase 1 - relatorio de bench ({args.rx_log})", ""]
    for d in dumps:
        out_lines.append(analyze_dump(d, tx_stats))
        out_lines.append("")

    # separar sessoes: nova sessao sempre que (ciclo, passo) nao avanca (TX reiniciado)
    sess, last = 0, None
    for r in ROWS:
        key = (r["cycle"], r["step"])
        if last is not None and key <= last:
            sess += 1
        r["sess"] = sess
        last = key

    if ROWS:
        pool = ["### Por transporte/PHY (todas as amostras da sessao, passos repetidos juntos)", "",
                "| sessao | PHY | payload B | amostras | perdas % mediana | perdas % media | perdas % min..max | jitter p99 mediano us | Mbps medio | RSSI medio |",
                "|---|---|---|---|---|---|---|---|---|---|"]
        pg = {}
        for r in ROWS:
            pg.setdefault((r["sess"], r["phy"], r["payload"]), []).append(r)
        for (s, phy, pl), rs in sorted(pg.items()):
            ls = sorted(x["loss"] for x in rs)
            rssis = [x["rssi"] for x in rs if x["rssi"] is not None]
            pool.append(f"| {s} | {phy} | {pl} | {len(rs)} | {statistics.median(ls):.2f} | {statistics.mean(ls):.2f} | "
                        f"{ls[0]:.2f}..{ls[-1]:.2f} | {statistics.median(x['jp99'] for x in rs):.0f} | "
                        f"{statistics.mean(x['mbps'] for x in rs):.2f} | {('%.0f' % statistics.mean(rssis)) if rssis else '-'} |")
        pool.append("")
        agg = ["### Agregado por sessao e passo (mediana/pior entre ciclos)", "",
               "| sessao | step | PHY | TX dBm | payload B | alvo Mbps | ciclos | Mbps medio | perdas % mediana | perdas % pior | jitter p99 mediano us | RSSI medio |",
               "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        groups = {}
        for r in ROWS:
            groups.setdefault((r["sess"], r["step"]), []).append(r)
        for (s, st), rs in sorted(groups.items()):
            losses = sorted(x["loss"] for x in rs)
            rssis = [x["rssi"] for x in rs if x["rssi"] is not None]
            agg.append(f"| {s} | {st} | {rs[0]['phy']} | {rs[0]['pwr']:.1f} | {rs[0]['payload']} | {rs[0]['target']:.2f} | "
                       f"{len(rs)} | {statistics.mean(x['mbps'] for x in rs):.2f} | {statistics.median(losses):.2f} | "
                       f"{losses[-1]:.2f} | {statistics.median(x['jp99'] for x in rs):.0f} | "
                       f"{('%.0f' % statistics.mean(rssis)) if rssis else '-'} |")
        agg.append("")
        tbl = ["| sessao | step | ciclo | PHY | TX dBm | payload B | alvo Mbps | obtido Mbps | perdas | jitter p99 us | RSSI |",
               "|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in ROWS:
            tbl.append(f"| {r['sess']} | {r['step']} | {r['cycle']} | {r['phy']} | {r['pwr']:.1f} | {r['payload']} | "
                       f"{r['target']:.2f} | {r['mbps']:.2f} | {r['lost']} ({r['loss']:.2f}%) | "
                       f"{r['jp99']:.0f} | {('%.0f' % r['rssi']) if r['rssi'] is not None else '-'} |")
        out_lines[2:2] = pool + agg + ["### Todos os runs (ordem de chegada)", ""] + tbl + [""]
    report = "\n".join(out_lines)
    if args.out:
        with open(args.out, "w") as f:
            f.write(report)
        print(f"Relatorio escrito em {args.out}", file=sys.stderr)
    else:
        print(report)


if __name__ == "__main__":
    main()
