#!/bin/sh
# ./x <command> - builds, tests and packages Flint. The work is done by dev/ (written in Flint);
# all this script does is find a Flint compiler to run it with, stage 0:
#   - FLINT_BOOTSTRAP=path/to/flint, if given
#   - otherwise the release pinned in bootstrap/release, downloaded once into build/stage0 and
#     checked against its sha256
# `./x help` lists the commands.
set -e
case $FLINT_BOOTSTRAP in ''|/*) ;; *) FLINT_BOOTSTRAP=$PWD/$FLINT_BOOTSTRAP ;; esac
cd "$(dirname "$0")"
if [ "$1" = clean ]; then   # needs no compiler (and would delete the one it ran with)
  rm -rf build
  exit 0
fi
if [ -n "$FLINT_BOOTSTRAP" ]; then
  stage0=$FLINT_BOOTSTRAP
else
  . bootstrap/release
  if [ -z "$url" ]; then
    echo "bootstrap/release pins no release; give a compiler: FLINT_BOOTSTRAP=path/to/flint ./x" >&2
    exit 1
  fi
  dir=build/stage0/$version
  if [ ! -x "$dir/flint" ]; then
    mkdir -p "$dir"
    curl -fsSL "$url" -o "$dir/flint.tar.gz"
    echo "$sha256  $dir/flint.tar.gz" | shasum -a 256 -c - >&2
    tar -xzf "$dir/flint.tar.gz" -C "$dir" --strip-components 1
  fi
  stage0=$PWD/$dir/flint
fi
FLINT_STAGE0=$stage0 exec "$stage0" run dev/main.fl "$@"
