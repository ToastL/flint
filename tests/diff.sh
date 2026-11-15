#!/bin/sh
# Compares the self-hosted compiler with the C one: for every test program, the output of
# `flint <what> <file>` (IR, or the compile error) must be byte-identical.
#   tests/diff.sh [what]      what: ir (default, unoptimized), ir-safe, ir-fast, asm, asm-safe,
#                             asm-fast, tokens, ast
#   NEW=build/flint1 OLD=build/flint
cd "$(dirname "$0")/.." || exit 1
WHAT=${1:-ir}
OLD=${OLD:-./build/flint}
NEW=${NEW:-./build/flint1}
T=$(mktemp -d)
pass=0; fail=0
for f in examples/*.fl tests/cases/*.fl tests/errors/*.fl tests/tests/*.fl tests/modules/*/main.fl; do
  case $WHAT in
    ir|asm) M=Debug ;; ir-safe|asm-safe) M=ReleaseSafe ;; ir-fast|asm-fast) M=ReleaseFast ;; *) M= ;;
  esac
  if [ -n "$M" ]; then
    C=${WHAT%%-*}
    $OLD $C -O $M "$f" > "$T/old" 2>&1
    $NEW $C -O $M "$f" > "$T/new" 2>&1
  else
    $OLD "$WHAT" "$f" > "$T/old" 2>&1
    $NEW "$WHAT" "$f" > "$T/new" 2>&1
  fi
  if cmp -s "$T/old" "$T/new"; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF $f"; fi
done
rm -rf "$T"
echo "$pass same, $fail different"
[ $fail -eq 0 ]
