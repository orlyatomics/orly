#!/bin/bash
# Kill-and-recover campaign (#730): SIGKILL a disk-backed orlyi at random moments under
# concurrent write load, restart it on the same loopback volume, and check every write
# against a client-side ledger. The contract it checks is in docs/durability.md; the
# checks themselves are listed at the top of tests/kill_campaign.py.
#
#   KILLS=40     kills per run (default 40)
#   SEED=<n>     random seed for the kill times (default: the clock; printed)
#   NEGATIVE=rollback
#                negative control: put back an older volume image before some restarts,
#                as a lost data file would look. The run must FAIL.
#   SIGNAL=TERM  stop orlyi gracefully instead of killing it, with the same checks, to compare
#   ORLY_OUT     build tree (default ../out_orly/release); needs orlyi, orlyc, orly_dm
#
# Needs root for losetup and the /proc/partitions device scan, like restart_test.sh: it
# re-runs itself under sudo. Ports 19900-19903. Prints one summary line starting
# "KILL CAMPAIGN:" and exits nonzero on any violation.
set -e
cd "$(dirname "$0")/.."
ORLY_OUT="$(cd "${ORLY_OUT:-$PWD/../out_orly/release}" && pwd)"
for bin in orly/server/orlyi orly/orlyc orly/indy/disk/util/orly_dm; do
  if [ ! -x "$ORLY_OUT/$bin" ]; then
    echo "missing: $ORLY_OUT/$bin (build it first, e.g. make release)"
    exit 1
  fi
done
if [ "$(id -u)" -ne 0 ]; then
  exec sudo --preserve-env=KILLS,SEED,NEGATIVE,SIGNAL ORLY_OUT="$ORLY_OUT" python3 tests/kill_campaign.py "$@"
fi
ORLY_OUT="$ORLY_OUT" exec python3 tests/kill_campaign.py "$@"
