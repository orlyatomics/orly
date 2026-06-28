#!/usr/bin/env python3
"""Concurrent-write throughput benchmark for issue #234.

#231/#232 made *single*-transaction bulk load linear. The remaining write
lever is *concurrent* throughput, and it has a single structural cause: the
global POV's Tetris merge runs on one thread and promotes at most one update
per round, re-snapshotting and re-sorting *every* waiting child each round --
so draining N queued children is O(N^2) in the merge tournament. The textbook
symptom is throughput that flatlines and then *regresses* as writers are added.

This harness makes that cap reproducible (the RFC's original K-writer numbers
came from a throwaway script). It spins up K writer threads, each with its own
session and child POV, all promoting into the one global POV, and reports the
aggregate writes/sec for a sweep of K. A separate same-key correctness oracle
confirms the headline invariant -- concurrent commutative `+=` loses nothing.

Run manually via ./run-concurrent-bench.sh (NOT named run.sh, so the CI
examples job does not pick it up -- this is a manual benchmark, not a gated
demo).

  THROUGHPUT: each writer hammers a disjoint key range with add_mention (one
              commutative `+=` per call). Aggregate w/s is reported per K.
              On baseline this flatlines ~1.4k w/s and degrades past K=4.

  CORRECTNESS: all writers `+=` the SAME hot key concurrently. After draining,
               the merged count read back from a fresh POV must equal the total
               number of writes -- zero lost updates (architecture.md §5).
"""

import os
import sys
import threading
import time

import orly

TOTAL = int(os.environ.get("BENCH_WRITES", "8000"))
KS = [int(k) for k in os.environ.get("BENCH_KS", "1,2,4,8").split(",")]


def p(*a):
    print(*a, flush=True)


def _writer(barrier, results, idx, n, key_for, errbox):
    """One writer: own connection, session and child POV; writes `n` calls."""
    try:
        c = orly.connect()
        c.new_session()
        c.install("graph", 1)  # idempotent install; first wins, rest no-op
        pov = c.new_pov()
        barrier.wait()  # all writers start the timed region together
        for i in range(n):
            c.call(pov, "graph", "add_mention", key_for(idx, i))
        results[idx] = n
        c.exit()
    except Exception as exc:  # noqa: BLE001 -- surface the first failure
        errbox.append(exc)
        try:
            barrier.abort()
        except Exception:
            pass


def run_throughput(k, total, key_for):
    """Run `k` writers splitting `total` writes; return aggregate writes/sec."""
    per = total // k
    actual = per * k
    barrier = threading.Barrier(k)
    results = [0] * k
    errbox = []
    threads = [
        threading.Thread(target=_writer, args=(barrier, results, i, per, key_for, errbox))
        for i in range(k)
    ]
    for t in threads:
        t.start()
    # The barrier inside the workers brackets the timed region; we time from
    # just-after the barrier releases. Approximate it by timing the join window
    # minus connection/setup, which dominates only for tiny runs. For a clean
    # number, time the whole bracket: workers block on the barrier until the
    # last one is connected, so wall time here ~= the write phase.
    t0 = time.monotonic()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    if errbox:
        raise errbox[0]
    return actual / elapsed if elapsed else float("inf")


def disjoint_key(idx, i):
    # Each writer owns a private key namespace -> writers touch disjoint keys,
    # the case the commutative fast-lane batches.
    return {"e": f"w{idx}", "d": i}


def main():
    p(f"=== #234 concurrent write throughput (TOTAL={TOTAL}, K sweep={KS}) ===\n")

    p("A. THROUGHPUT -- disjoint-key add_mention, aggregate writes/sec:")
    p(f"   {'K writers':>10} {'writes/sec':>12} {'vs K=1':>8}")
    base = None
    for k in KS:
        wps = run_throughput(k, TOTAL, disjoint_key)
        if base is None:
            base = wps
        p(f"   {k:>10} {wps:>12.0f} {wps / base:>7.2f}x")
    p("   (baseline signature: flatlines and regresses past K=4 -- one")
    p("    saturated merge thread re-snapshotting all children each round.)\n")

    p("B. CORRECTNESS -- K writers, one hot key, merged count must equal total:")
    k = max(KS)
    per = TOTAL // k
    expect = per * k
    barrier = threading.Barrier(k)
    results = [0] * k
    errbox = []
    threads = [
        threading.Thread(
            target=_writer,
            args=(barrier, results, i, per, lambda idx, i: {"e": "hot", "d": 0}, errbox),
        )
        for i in range(k)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errbox:
        raise errbox[0]

    # Drain: let the merge promote everything to the global POV, then read the
    # merged count from a fresh POV (sees global). Poll until it settles.
    c = orly.connect()
    c.new_session()
    c.install("graph", 1)
    pov = c.new_pov()
    got = None
    for _ in range(120):
        got = c.call(pov, "graph", "mention_count", {"e": "hot", "d": 0})
        if got == expect:
            break
        time.sleep(0.25)
    c.exit()
    ok = got == expect
    p(f"   wrote {expect} concurrent += to one key; read back {got}  => "
      f"{'OK (zero lost)' if ok else 'LOST UPDATES'}")
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
