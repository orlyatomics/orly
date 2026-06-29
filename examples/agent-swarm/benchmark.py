#!/usr/bin/env python3
"""Reproduction for issue #227 -- writing N keys to a POV is O(N^2).

This started as a scale benchmark for the #219 graph traversal and instead
surfaced a more fundamental problem: the per-write commit cost grows linearly
with the number of updates already accumulated in the POV's memory layer, so
writing N keys is O(N^2). The mem->disk merge is enqueued only on the
empty->non-empty edge (`orly/indy/repo.cc:301-304`), so under sustained writes
the layer is never re-merged and grows unbounded, and every write pays an
O(layer-size) linear scan over the mem layer's OrderedList. See #227.

It reuses `graph.orly`'s `add_mention` (one `*<['mention',e,d]>::(int) += 1`
per call). NOT a CI demo -- run manually via ./run-bench.sh. It does not assert
a failure (it documents a known bug); it prints the per-write trend and a
verdict that flips from "GROWING" to "FLAT" once #227 is fixed.

  DISTINCT keys: a new key each write. Shows the per-write cost climbing.
  SAME key:      one hot key, repeated +=. Also climbs -- so the cost tracks
                 the accumulated update count, not key cardinality.
  PAUSE:         an idle gap mid-run. The cost does NOT reset, because under
                 the empty->non-empty trigger nothing is queued to merge.
"""

import os
import sys
import time

import orly

TOTAL = int(os.environ.get("BENCH_WRITES", "2000"))
WINDOW = max(TOTAL // 8, 50)
# Batched writes (#253): fold this many add_mention calls into one transaction.
# 1 = unbatched (one `try` per write). Override with `--batch N` or BENCH_BATCH=N.
BATCH = int(os.environ.get("BENCH_BATCH", "1"))


def p(*a):
    print(*a, flush=True)


def run_burst(c, pov, key_for, total, batch=1):
    """Write `total` keys, returning the full per-window (i, ms/write) list.

    With `batch > 1`, coalesce consecutive writes into one `call_batch` so the
    per-round-trip cost (parse, context, commit, I/O) is amortized across the
    batch (#253). ms/write is still reported per logical write, so batched and
    unbatched runs are directly comparable."""
    windows = []
    t0 = time.monotonic()
    last_t, last_i = t0, 0
    i = 0
    while i < total:
        n = min(batch, total - i)
        if n == 1:
            c.call(pov, "graph", "add_mention", key_for(i + 1))
        else:
            c.call_batch(pov, "graph", "add_mention", [key_for(i + k + 1) for k in range(n)])
        i += n
        if i % WINDOW < n or i == total:
            now = time.monotonic()
            ms = (now - last_t) * 1000 / (i - last_i)
            windows.append((i, ms))
            p(f"    {i:>6} writes:  {ms:>7.1f} ms/write")
            last_t, last_i = now, i
    return windows


def verdict(windows, label):
    first = windows[0][1]
    last = windows[-1][1]
    ratio = last / first if first else float("inf")
    # O(N) per write => the last window is many times the first. Bounded
    # (O(1)/O(log N)) per write => roughly flat.
    state = "GROWING  (O(N) per write -- #227 present)" if ratio >= 2.0 else "FLAT  (bounded -- #227 fixed)"
    p(f"  -> {label}: first {first:.1f} ms/write, last {last:.1f} ms/write, {ratio:.1f}x  => {state}")
    return ratio


def main():
    batch = BATCH
    if "--batch" in sys.argv:
        batch = int(sys.argv[sys.argv.index("--batch") + 1])
    mode = f"batch={batch}" if batch > 1 else "unbatched"
    p(f"=== #227 write-scaling reproduction (TOTAL={TOTAL}, window={WINDOW}, {mode}) ===\n")
    if batch > 1:
        p(f"(#253) folding {batch} add_mention calls per transaction\n")
    c = orly.connect()
    c.new_session()
    c.install("graph", 1)

    p("A. DISTINCT keys -- a new key every write:")
    pov = c.new_pov()
    w = run_burst(c, pov, lambda i: {"e": f"e{i}", "d": i}, TOTAL, batch)
    verdict(w, "distinct")

    p("\nB. SAME key -- one hot key, repeated +=:")
    pov = c.new_pov()
    w = run_burst(c, pov, lambda i: {"e": "hot", "d": 0}, TOTAL, batch)
    verdict(w, "same-key")

    p("\nC. PAUSE mid-run -- does an idle gap let the merge catch up?")
    pov = c.new_pov()
    half = TOTAL // 2
    run_burst(c, pov, lambda i: {"e": f"p{i}", "d": i}, half, batch)
    p("    --- idle 8s (background merge would run here) ---")
    time.sleep(8)
    p("    resuming:")
    # continue with fresh key ids so we keep adding distinct keys
    t0 = time.monotonic()
    last_t, last_i = t0, 0
    for j in range(1, half + 1):
        c.call(pov, "graph", "add_mention", {"e": f"p{half + j}", "d": half + j})
        if j % WINDOW == 0:
            now = time.monotonic()
            p(f"    {half + j:>6} writes:  {(now - last_t) * 1000 / (j - last_i):>7.1f} ms/write")
            last_t, last_i = now, j
    p("  -> if the post-pause cost continues the pre-pause trend (no reset),")
    p("     the merge is not bounding the layer (#227).")

    c.exit()
    p("\nWhen #227 is fixed (size/level-triggered merge and/or a seekable mem")
    p("layer), A and B flip to FLAT and a 10k+-key graph becomes loadable.")


if __name__ == "__main__":
    main()
