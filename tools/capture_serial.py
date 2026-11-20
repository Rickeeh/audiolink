#!/usr/bin/env python3
"""Captura a consola serie (USB-Serial/JTAG) para um ficheiro.

Abre a porta com DTR/RTS a False *antes* do open() - no macOS mexer
nessas linhas reinicia o S3 para modo download. Se a porta ainda nao
existir (placa a re-enumerar depois de um reset), tenta de novo.

  capture_serial.py --port /dev/cu.usbmodemXXXX --out rx.txt \
      --until "===AUDIOLINK_BENCH_DUMP_END===" --timeout 120
"""
import argparse
import os
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--until", help="para quando este texto aparecer na captura")
    ap.add_argument("--timeout", type=float, default=120, help="segundos, maximo")
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    s = None
    while time.time() < deadline:
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = args.port, 115200, 0.2
            s.dtr, s.rts = False, False
            s.open()
            break
        except (serial.SerialException, OSError):
            s = None
            time.sleep(0.2)
    if s is None:
        print(f"nao consegui abrir {args.port}", file=sys.stderr)
        sys.exit(2)

    buf = b""
    marker = args.until.encode() if args.until else None
    with open(args.out, "wb") as f:
        while time.time() < deadline:
            try:
                chunk = s.read(65536)
            except (serial.SerialException, OSError):
                # porta desapareceu (reset da placa) - reabre
                s.close()
                while time.time() < deadline:
                    try:
                        s.open()
                        break
                    except (serial.SerialException, OSError):
                        time.sleep(0.2)
                continue
            if chunk:
                f.write(chunk)
                f.flush()
                buf = (buf + chunk)[-4096:]
                if marker and marker in buf:
                    break
    s.close()
    print(f"captura terminada: {os.path.getsize(args.out)} bytes -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
