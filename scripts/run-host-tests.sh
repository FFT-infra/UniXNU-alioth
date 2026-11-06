#!/bin/sh
# Build and run every host test in package/tests. A host test checks the pure logic that a driver
# and a device test cannot check cheaply: register offsets, scaling, header packing, retry rules.
# Each test is a C file that includes its header and asserts against the vendor source by hand.
#
# Usage: sh scripts/run-host-tests.sh [--keep]
#   --keep  leave the binaries in the output directory for inspection.
#
# The Python tests in that directory are not run here: they read device-tree or ADT inputs and are
# invoked by the steps that need them.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/alioth-host-tests
mkdir -p "$out"

cc=${CC:-cc}
fail=0
count=0

for test in "$root"/package/tests/*_host.c; do
  [ -f "$test" ] || continue
  name=$(basename "$test" .c)
  count=$((count + 1))
  printf '%-24s ' "$name"
  if ! "$cc" -std=c11 -Wall -Wextra -Werror -o "$out/$name" "$test" 2>"$out/$name.log"; then
    echo "BUILD FAILED ($out/$name.log)"
    fail=1
    continue
  fi
  if "$out/$name" >"$out/$name.out" 2>&1; then
    echo "ok"
  else
    echo "FAILED ($out/$name.out)"
    fail=1
  fi
  [ "${1:-}" = "--keep" ] || rm -f "$out/$name"
done

if [ "$count" -eq 0 ]; then
  echo "no host tests found in $root/package/tests" >&2
  exit 1
fi

[ "$fail" -eq 0 ] && echo "$count host tests passed"
exit "$fail"
