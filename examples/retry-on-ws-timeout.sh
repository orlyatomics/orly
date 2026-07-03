#!/bin/bash
# CI helper: run a demo command and retry it ONCE if (and only if) it failed
# with a WebSocket timeout.
#
# The examples job used to intermittently hit an orlyi read hang under the
# loaded 2-core CI runner: a read call blocked ~120s and the client raised
# `WebSocketTimeoutException: Connection timed out` (rare, ~2/25 runs,
# load-sensitive, not a regression). #247 tracked it and was closed 2026-07-03
# after 43 consecutive quiet runs following the fiber-handshake and teardown
# fixes (#407, #386). This wrapper stays armed as the tripwire: the warning
# annotation below names #247, so if it ever fires again, reopen #247 with
# that run attached.
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
