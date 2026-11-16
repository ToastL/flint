#!/bin/sh
# Downloads the Flint release pinned in bootstrap/release, checks its sha256, and prints the path
# of its `flint` binary. The Makefile uses it as stage 0 unless FLINT_BOOTSTRAP is given.
set -e
cd "$(dirname "$0")/.."
. bootstrap/release
if [ -z "$url" ]; then
  echo "bootstrap/release pins no release; give a compiler: make FLINT_BOOTSTRAP=path/to/flint" >&2
  exit 1
fi
dir=build/bootstrap/$version
if [ ! -x "$dir/flint" ]; then
  mkdir -p "$dir"
  curl -fsSL "$url" -o "$dir/flint.tar.gz"
  echo "$sha256  $dir/flint.tar.gz" | shasum -a 256 -c - >&2
  tar -xzf "$dir/flint.tar.gz" -C "$dir" --strip-components 1
fi
echo "$dir/flint"
