#!/usr/bin/env bash
# Gate for the #312 incremental-compilation contract:
#
#   1. A second compile of an unchanged package does no codegen and no gcc:
#      every generated file (including the .so) keeps its exact mtime.
#   2. Editing the source invalidates the staleness key: the .so is relinked.
#   3. --syntax-only accepts a program with type errors (it stops before
#      type-check) but still rejects a parse error; neither produces output.
#   4. --semantic-only rejects the type error and emits no generated C++.
#   5. --transient-cc leaves the linked .so but no generated C++ behind.
#
# The staleness cache keys on the compiler build stamp, so this needs an
# orlyc built after `make version` (make debug does this); with an
# "unknown" stamp the cache is disabled and step 1 would fail honestly.
#
# Usage: tests/incremental_test.sh [path-to-orlyc]   (default: the debug build)
set -eu

ORLYC="${1:-${ORLYC:-$PWD/../out_orly/debug/orly/orlyc}}"
[ -x "$ORLYC" ] || { echo "FAIL: orlyc not found at $ORLYC" >&2; exit 1; }

work_dir="$(mktemp -d /tmp/orly_incremental.XXXXXX)"
trap 'rm -rf "$work_dir"' EXIT
out_dir="$work_dir/out"
mkdir -p "$out_dir"

fail() { echo "FAIL: $*" >&2; exit 1; }

cat > "$work_dir/sample.orly" <<'EOF'
package #1;

double = (x * 2) where {
  x = given::(int);
};

test {
  t1: double(.x: 21) == 42;
};
EOF

# mtime snapshot of every file under the out dir, sorted for stable diffing.
snapshot() { find "$out_dir" -type f -printf '%T@ %p\n' | sort -k2; }

echo "--- 1: rebuild of an unchanged package is a no-op ---"
(cd "$work_dir" && "$ORLYC" --skip-tests -o "$out_dir" sample.orly) || fail "first compile"
[ -e "$out_dir/sample.1.so" ] || fail "first compile produced no .so"
first="$(snapshot)"
sleep 1.1   # ensure a rewrite would be visible even on coarse (1s) mtimes
(cd "$work_dir" && "$ORLYC" --skip-tests -o "$out_dir" sample.orly) || fail "second compile"
second="$(snapshot)"
[ "$first" = "$second" ] && echo "PASS: no file changed" || {
  diff <(echo "$first") <(echo "$second") >&2 || true
  fail "second compile of unchanged source rewrote output"
}

echo "--- 2: an edit invalidates the key ---"
sleep 1.1
sed -i 's/x \* 2/x + x/' "$work_dir/sample.orly"   # same behavior, new bytes
(cd "$work_dir" && "$ORLYC" --skip-tests -o "$out_dir" sample.orly) || fail "recompile after edit"
third="$(snapshot)"
[ "$first" = "$third" ] && fail "edit did not invalidate the staleness key"
echo "PASS: outputs regenerated"

echo "--- 3: --syntax-only ---"
cat > "$work_dir/typebad.orly" <<'EOF'
package #1;

bad = true + 1;
EOF
(cd "$work_dir" && "$ORLYC" --syntax-only -o "$out_dir" typebad.orly) \
  || fail "--syntax-only rejected a syntactically valid (if ill-typed) program"
[ -e "$out_dir/typebad.cc" ] && fail "--syntax-only emitted C++"
cat > "$work_dir/parsebad.orly" <<'EOF'
package #1;

bad = ;
EOF
if (cd "$work_dir" && "$ORLYC" --syntax-only -o "$out_dir" parsebad.orly) 2>/dev/null; then
  fail "--syntax-only accepted a parse error"
fi
echo "PASS: parse checked, types not"

echo "--- 4: --semantic-only ---"
if (cd "$work_dir" && "$ORLYC" --semantic-only -o "$out_dir" typebad.orly) 2>/dev/null; then
  fail "--semantic-only accepted a type error"
fi
rm -f "$out_dir"/sample.* # so the transient check below sees only its own output
cat > "$work_dir/clean.orly" <<'EOF'
package #1;

same = (x) where {
  x = given::(int);
};
EOF
(cd "$work_dir" && "$ORLYC" --semantic-only -o "$out_dir" clean.orly) || fail "--semantic-only rejected a clean program"
[ -e "$out_dir/clean.cc" ] && fail "--semantic-only emitted C++"
echo "PASS: type checked, nothing emitted"

echo "--- 5: --transient-cc ---"
(cd "$work_dir" && "$ORLYC" --skip-tests --transient-cc -o "$out_dir" clean.orly) || fail "--transient-cc compile"
[ -e "$out_dir/clean.1.so" ] || fail "--transient-cc produced no .so"
leftovers="$(find "$out_dir" -name 'clean.*' ! -name 'clean.1.so' -type f)"
[ -z "$leftovers" ] || fail "--transient-cc left intermediates: $leftovers"
echo "PASS: only the .so remains"

echo "OK: incremental compilation contract holds"
