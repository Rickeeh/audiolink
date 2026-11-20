#!/usr/bin/env python3
"""Conta linhas de ficheiros C: total e de codigo (sem linhas vazias nem so de comentario). Uso: loc_count.py <ficheiros...>"""
import re, sys
tot_all = tot_code = 0
for p in sys.argv[1:]:
    src = open(p, errors="replace").read()
    nall = src.count("\n")
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    code = sum(1 for ln in s.split("\n") if ln.strip() and not ln.strip().startswith("//"))
    tot_all += nall; tot_code += code
    print(f"{nall:6d} {code:6d}  {p}")
print(f"{tot_all:6d} {tot_code:6d}  TOTAL (linhas, codigo)")
