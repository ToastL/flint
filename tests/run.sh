#!/bin/sh
# Flint test runner.
#   examples/*.fl      output must match examples/<name>.expected
#   tests/cases/*.fl   output (+ stderr and exit code when non-zero) must match <name>.expected.
#                      A first line `// flint: <flags>` sets the compiler flags; otherwise a case
#                      that exits 0 must also give the same output in ReleaseSafe and ReleaseFast.
#   tests/modules/*/   main.fl (plus the modules it imports): output must match expected
#   tests/packages/*/  `flint run -- x y` and `flint test` in the package match expected-run / expected-test
#   tests/testblocks/*.fl  `flint test` output and exit code must match <name>.expected
#   tests/errors/*.fl  must fail to compile with the message in <name>.expected
cd "$(dirname "$0")/.." || exit 1
FLINT=${FLINT:-./build/flint}   # FLINT=path/to/compiler tests/run.sh tests another build
case $FLINT in /*) ;; *) FLINT=$PWD/$FLINT ;; esac
T=$(mktemp -d) && trap 'rm -rf "$T"' EXIT
fail=0; pass=0
check() { if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "ok   $1"; else fail=$((fail+1)); echo "FAIL $1"; echo "$2" > "$T/expected"; echo "$3" > "$T/got"; diff -u "$T/expected" "$T/got" | sed 's/^/     /'; fi; }
for f in examples/*.fl; do
  n=$(basename "$f" .fl)
  check "$n" "$(cat examples/$n.expected)" "$($FLINT run "$f" 2>&1)"
done
for f in tests/cases/*.fl; do
  n=$(basename "$f" .fl)
  flags=$(head -1 "$f" | sed -n 's|^// flint: ||p')
  out=$($FLINT run $flags "$f" 2>"$T/err"); code=$?
  if [ $code -ne 0 ]; then out=$(printf '%s\n--- stderr\n%s\n--- exit %s' "$out" "$(cat "$T/err")" "$code"); fi
  check "$n" "$(cat tests/cases/$n.expected)" "$out"
  [ $code -ne 0 ] || [ -n "$flags" ] && continue
  for mode in ReleaseSafe ReleaseFast; do   # same program, other build modes
    check "$n -O $mode" "$(cat tests/cases/$n.expected)" "$($FLINT run -O $mode "$f" 2>&1)"
  done
done
for d in tests/modules/*/; do   # multi-file programs: main.fl and the modules it imports
  n=$(basename "$d")
  check "module/$n" "$(cat "$d/expected")" "$($FLINT run "$d/main.fl" 2>&1)"
done
for d in tests/packages/*/; do   # packages (flint.toml): run with arguments, and test
  [ -f "$d/expected-run" ] || continue
  n=$(basename "$d")
  check "package/$n run" "$(cat "$d/expected-run")" "$(cd "$d" && $FLINT run -- x y 2>&1)"
  check "package/$n test" "$(cat "$d/expected-test")" "$(cd "$d" && $FLINT test 2>&1)"
done
for f in tests/testblocks/*.fl; do   # flint test: the runner's report (stdout and stderr) and its exit code
  n=$(basename "$f" .fl)
  out=$($FLINT test "$f" 2>&1); code=$?
  check "test/$n" "$(cat tests/testblocks/$n.expected)" "$(printf '%s\n--- exit %s' "$out" "$code")"
done
for f in tests/errors/*.fl; do
  n=$(basename "$f" .fl)
  got=$($FLINT run "$f" 2>&1 | head -1 | sed 's/^[^ ]* error: //')
  check "error/$n" "$(cat tests/errors/$n.expected)" "$got"
done
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
