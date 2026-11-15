#!/bin/sh
# Downloads the Flint release pinned in bootstrap/VERSION, checks its sha256, and prints the path
# of its `flint` binary, for: make FLINT_BOOTSTRAP=$(scripts/bootstrap.sh)
set -e
cd "$(dirname "$0")/.."
. bootstrap/VERSION
if [ -z "$url" ]; then
  echo "bootstrap/VERSION pins no release yet; build from the C compiler instead (plain 'make')" >&2
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
