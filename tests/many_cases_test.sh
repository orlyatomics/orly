#!/usr/bin/env bash
# Gate for #683: orlyc's embedded test server must run any number of test
# cases, bounded by the work in flight rather than by how many cases came
# before. It used to hold every case's pov (and repo) open until the run
# ended, and ran out of repo data layers after about 300 cases.
#
# Generates a package of BLOCKS `with {...} test {...}` sections of five cases
# each (a read, a write with a read under it, and a nested write with a read
# under that), compiles it with orlyc -v, and requires every case to pass.
#
# Usage: tests/many_cases_test.sh [path-to-orlyc] [blocks]
#        (defaults: the debug build, 120 blocks = 600 cases)
set -eu

ORLYC="${1:-${ORLYC:-$PWD/../out_orly/debug/orly/orlyc}}"
BLOCKS="${2:-120}"
[ -x "$ORLYC" ] || { echo "FAIL: orlyc not found at $ORLYC" >&2; exit 1; }

work_dir="$(mktemp -d /tmp/orly_many_cases.XXXXXX)"
trap 'rm -rf "$work_dir"' EXIT

{
  echo 'package #1;'
  echo 'read_int = (*<[n]>::(int)) where { n = given::(int); };'
  echo 'write_int = ((true) effecting { *<[n]>::(int) <- x; }) where { n = given::(int); x = given::(int); };'
  for ((i = 0; i < BLOCKS; ++i)); do
    cat <<EOT
with { <[$i]> <- $i; } test {
  a$i: read_int(.n: $i) == $i;
  w$i: write_int(.n: $i, .x: 42) {
    r$i: read_int(.n: $i) == 42;
    v$i: write_int(.n: $i, .x: 7) { s$i: read_int(.n: $i) == 7; };
  };
};
EOT
  done
} > "$work_dir/many.orly"

expected=$((BLOCKS * 5))
status=0
(cd "$work_dir" && "$ORLYC" -v -o "$work_dir" many.orly) > "$work_dir/out" 2>&1 || status=$?
passed="$(grep -c ' PASSED$' "$work_dir/out" || true)"
if [ "$status" -ne 0 ] || [ "$passed" -ne "$expected" ]; then
  grep -v ' PASSED$' "$work_dir/out" | tail -20 >&2
  echo "FAIL: orlyc exited $status with $passed of $expected cases passed" >&2
  exit 1
fi
echo "PASS: all $expected cases passed"
