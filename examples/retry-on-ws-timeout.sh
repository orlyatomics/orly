#!/bin/bash
# CI helper: run a demo command and retry it ONCE if (and only if) it failed
# with a WebSocket timeout.
#
# The examples job intermittently hits an orlyi read hang under the loaded
# 2-core CI runner: a read call blocks ~120s and the client raises
# `WebSocketTimeoutException: Connection timed out`. It is rare (~2/25 runs),
# load-sensitive, and NOT a regression -- it first appeared on a docs-only PR.
# Tracked as a real bug in #247; this wrapper is only a band-aid so the flake
# does not red the build.
#
# Crucially, ANY OTHER failure (a correctness mismatch, a crash, a non-WS
# timeout, "orlyi failed to come up", ...) is propagated immediately and is NOT
# retried, so genuine regressions still fail on the first attempt.
#
# Usage:  retry-on-ws-timeout.sh ./run.sh      (from a demo's working dir)
set -uo pipefail

out=$("$@" 2>&1); rc=$?
printf '%s\n' "$out"

if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qiE 'WebSocketTimeout|Connection timed out'; then
  echo "::warning title=examples flake (#247)::demo hit a WebSocketTimeout -- retrying once"
  sleep 3   # let the previous orlyi fully exit and release port 8082
  "$@"
  exit $?
fi

exit "$rc"
