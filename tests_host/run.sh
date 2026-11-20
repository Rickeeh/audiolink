#!/usr/bin/env bash
# Testes de host do AudioLink v2 (sem hardware): compila cada tests_host/test_*.c com o compilador do host, avisos como
# erros, AddressSanitizer + UndefinedBehaviorSanitizer, e corre-o. As fontes extra vem da linha "// deps:" do teste.
# Uso: tests_host/run.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
A="$HERE/.."
CC="${CC:-cc}"
OUT="$(mktemp -d)"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
        -I"$A/components/alk_proto/include" -I"$A/components/al_led/include" -I"$A/tx_v2/main" -I"$HERE")
echo "compilador: $($CC --version | head -1)"
rc=0
for t in "$HERE"/test_*.c; do
  n="$(basename "$t" .c)"
  deps=()
  for d in $(sed -n 's#^// deps: ##p' "$t"); do deps+=("$A/$d"); done
  if "$CC" "${CFLAGS[@]}" "$t" ${deps[@]+"${deps[@]}"} -o "$OUT/$n" 2> "$OUT/$n.cc.txt" && "$OUT/$n"; then
    echo "PASS $n"
  else
    cat "$OUT/$n.cc.txt"
    echo "FAIL $n"
    rc=1
  fi
done
rm -rf "$OUT"
exit $rc
