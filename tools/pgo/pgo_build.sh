#!/bin/bash
# Profile-guided release build of orlyi and orlyc.
#
#   tools/pgo/pgo_build.sh [--march=x86-64-v3] [--profile-dir DIR] [--jobs N] [--skip-train]
#
# A release build, then the same build again guided by a profile of the server doing real work:
#   1. build orlyi and orlyc release with -fprofile-generate (counters in every function);
#   2. run the training workload below against that build, which writes the profile;
#   3. rebuild with -fprofile-use. The result is in ../out_orly/release, exactly where `make release` puts it.
#
# The training workload is the repository's own smokes (clients/smoke), so anyone can regenerate the profile
# from a checkout: reads at many disk layers under ingest, paged reads, point and prefix reads at 1 to 16
# sessions, and write-pool pressure. It runs against the mem-sim orlyi each smoke starts. See
# docs/pgo.md for what to change when the workload or the code moves.
#
# --march=CPU compiles for that CPU (-march=CPU) as well. x86-64-v3 is Haswell and later (AVX2, BMI2); the
# binaries fault on older CPUs, so it is off unless asked for. --profile-dir keeps the profile (default
# ../out_orly/pgo-profile), and --skip-train reuses the one already there. The profile is keyed by the
# object files' paths, so it matches only a rebuild in this same checkout. Needs what `make release` needs,
# plus node and npm (the smokes' client). Takes a few times as long as a plain release build. It does not
# run `make version`; run that first for a stamped build, as `make release-pgo` does.
#
# Only release.jhm is touched while it runs, and it is put back on exit.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/../out_orly"
PROFILE="$OUT/pgo-profile"
MARCH=""
JOBS="$(nproc)"
TRAIN=1
while [ $# -gt 0 ]; do
  case "$1" in
    --march=*) MARCH="${1#*=}" ;;
    --profile-dir) PROFILE="$2"; shift ;;
    --jobs) JOBS="$2"; shift ;;
    --skip-train) TRAIN=0 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
if [ "$TRAIN" = 1 ]; then
  for tool in node npm; do
    command -v "$tool" >/dev/null || { echo "$tool is needed to run the training smokes (their client is TypeScript)" >&2; exit 1; }
  done
fi
mkdir -p "$PROFILE"
PROFILE="$(cd "$PROFILE" && pwd)"

CONFIG="$ROOT/release.jhm"
SAVED="$CONFIG.pgo-saved"
cp "$CONFIG" "$SAVED"
trap 'mv -f "$SAVED" "$CONFIG"' EXIT

# release.jhm with extra flags on every compile and link line.
with_flags() {
  python3 - "$SAVED" "$CONFIG" "$@" <<'PY'
import json, sys
src, dst, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
cfg = json.load(open(src))
cmd = cfg["cmd"]
cmd["+g++"] += extra
cmd["+gcc"] += extra
cmd["flex"]["+g++"] += extra
cmd["bison"]["+g++"] += extra
cmd["ld"]["+flags"] += extra
json.dump(cfg, open(dst, "w"), indent=2)
PY
}

build() {
  rm -rf "$OUT/release"
  # ccache would hand back objects built without (or with another) profile.
  (cd "$ROOT" && CCACHE_DISABLE=1 PATH="$ROOT/tools:$PATH" tools/jhm -c release --worker-count "$JOBS" \
     orly/server/orlyi orly/orlyc)
}

MARCH_FLAG=()
[ -z "$MARCH" ] || MARCH_FLAG=("-march=$MARCH")

(cd "$ROOT" && make bootstrap)

if [ "$TRAIN" = 1 ]; then
  rm -rf "$PROFILE" && mkdir -p "$PROFILE"
  echo "== 1/3 instrumented build"
  # Counters are not atomic, which the many threads of a server make a little inexact; -fprofile-correction
  # repairs the counts it can below.
  with_flags "${MARCH_FLAG[@]}" "-fprofile-generate=$PROFILE" "-fprofile-update=single"
  build

  echo "== 2/3 training run"
  export ORLY_OUT="$OUT/release" SKIP_TS_BUILD=1
  (cd "$ROOT/clients/ts" && npm install --silent && npx tsc)
  smoke() {
    local name="$1"; shift
    echo "-- $name"
    env "$@" bash "$ROOT/clients/smoke/run-$name.sh" || echo "-- $name exited $? (the profile keeps what ran)"
  }
  smoke ws-scale LEVELS=1,4,16 KINDS=point,prefix SECS=10 GROUPS=2000 WORKERS=4
  smoke read-fold NKEYS=200 ROUNDS=40 PAUSE_MS=250 R=4 SECS=20
  smoke layer-count K=8 SECS=60
  smoke read-fanout R=4
  smoke keyset-paging
  smoke pool-pressure K=8 SECS=30 MAX_WRITES=60000
  [ "$(find "$PROFILE" -name '*.gcda' | wc -l)" -gt 50 ] || { echo "too few profile files in $PROFILE" >&2; exit 1; }
fi

echo "== 3/3 profile-guided build"
with_flags "${MARCH_FLAG[@]}" "-fprofile-use=$PROFILE" "-fprofile-correction" "-fprofile-partial-training" \
  "-Wno-missing-profile" "-Wno-coverage-mismatch"
build
echo "built: $OUT/release/orly/server/orlyi  $OUT/release/orly/orlyc"
