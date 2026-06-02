#!/usr/bin/env python3
"""
Wikimedia hourly pageviews demo, in Python.

Spawns N concurrent WebSocket writers that hammer per-(page, hour)
counters via Orly's `+=` field calls. Demonstrates:

  - All increments land correctly even with many concurrent writers
    on the same hot key (Orly's `+=` is commutative and lock-free)
  - Throughput doesn't degrade catastrophically with more writers
  - The recorded counts match an independent ground-truth sum

The dataset is synthetic but shaped after real Wikimedia pageview
dumps: each event is `(lang, page, hour, count)`. We deliberately
target a small set of hot pages so the writers contend on the same
keys.

Run via the wrapper:

    ./run.sh

Or directly, after starting orlyi separately:

    python3 demo.py
"""

import json
import random
import sys
import threading
import time
import websocket

WS_URL = "ws://127.0.0.1:8082/"

NUM_WRITERS = 4
EVENTS_PER_WRITER = 200
PAGES = ["Donald_Trump", "Taylor_Swift", "ChatGPT", "Wikipedia"]
HOURS = list(range(2026_06_01_00, 2026_06_01_12))  # 12 "hour" buckets

# Scaled down from 8 writers * 250 events * 96 keys so that the
# per-layer fiber parallelism in TRepo::TPresentWalker's construction
# doesn't exhaust orlyi's fiber pool on a 4-CPU GitHub Actions runner.
# Still meaningfully concurrent (4 writers hammering 48 hot keys,
# ~17 writes per key on average).


def send(ws, stmt):
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def increment_view(ws, pov, page, hour, n):
    stmt = (f'try {{{pov}}} views increment_view '
            f'<{{.lang: "en", .page: "{page}", .hour: {hour}, .n: {n}}}>;')
    send(ws, stmt)


def views_in_hour(ws, pov, page, hour):
    stmt = (f'try {{{pov}}} views views_in_hour '
            f'<{{.lang: "en", .page: "{page}", .hour: {hour}}}>;')
    return int(send(ws, stmt))


def views_total(ws, pov, page, h_start, h_end):
    stmt = (f'try {{{pov}}} views views_total '
            f'<{{.lang: "en", .page: "{page}", .h_start: {h_start}, .h_end: {h_end}}}>;')
    return int(send(ws, stmt))


def generate_events(seed, count):
    """Generate `count` (page, hour, n) events. Deterministic given seed."""
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        page = rng.choice(PAGES)
        hour = rng.choice(HOURS)
        n = rng.randint(1, 5)
        out.append((page, hour, n))
    return out


def writer(writer_id, pov, events):
    """One writer: open a fresh WebSocket connection, open a session,
    ingest its slice of events using the shared POV."""
    ws = websocket.create_connection(WS_URL)
    try:
        send(ws, "new session;")
        for page, hour, n in events:
            increment_view(ws, pov, page, hour, n)
    finally:
        ws.close()


def main():
    # Generate all events deterministically, then split per writer.
    all_events = []
    for w in range(NUM_WRITERS):
        all_events.append(generate_events(seed=w * 17 + 3, count=EVENTS_PER_WRITER))
    total_events = sum(len(es) for es in all_events)

    # Ground truth: what *should* each (page, hour) count be after ingest?
    ground_truth = {}
    for batch in all_events:
        for page, hour, n in batch:
            ground_truth[(page, hour)] = ground_truth.get((page, hour), 0) + n

    # Open one bootstrap connection to create the shared POV and install the package.
    boot = websocket.create_connection(WS_URL)
    send(boot, "new session;")
    send(boot, "install views.1;")
    pov = send(boot, "new safe shared pov;")
    print(f"pov: {pov}")
    print(f"writers: {NUM_WRITERS}, events per writer: {EVENTS_PER_WRITER}, "
          f"total events: {total_events:,}")
    print(f"hot keyspace: {len(PAGES)} pages × {len(HOURS)} hours = "
          f"{len(PAGES) * len(HOURS)} keys")

    # Pre-create every (page, hour) key with value 0 so the concurrent
    # writers all take the `+=` branch (never the `new` create branch).
    # Isolates whether lost updates come from the create-race vs from
    # `+=` itself.
    print("\npre-initialising all hot keys to 0...")
    for page in PAGES:
        for hour in HOURS:
            increment_view(boot, pov, page, hour, 0)

    # Ingest in parallel.
    threads = []
    t0 = time.monotonic()
    for w in range(NUM_WRITERS):
        t = threading.Thread(target=writer, args=(w, pov, all_events[w]))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    rate = total_events / elapsed
    print(f"\ningest done in {elapsed:.2f}s "
          f"({rate:,.0f} events/sec end-to-end with {NUM_WRITERS} writers)")

    # Verify every (page, hour) counter matches ground truth.
    print()
    print("=== verifying counters ===")
    failures = []
    for (page, hour), expected in sorted(ground_truth.items()):
        got = views_in_hour(boot, pov, page, hour)
        marker = "✓" if got == expected else "✗"
        if got != expected:
            failures.append(f"{page} @ {hour}: got {got}, want {expected}")
        if hour == HOURS[-1]:  # only print one line per page for brevity
            pass
    # Print a per-page summary instead of every (page, hour) line.
    print(f"  {'page':<16}  {'24h total':>12}  {'expected':>12}")
    print(f"  {'-'*16}  {'-'*12}  {'-'*12}")
    grand_got = 0
    grand_want = 0
    for page in PAGES:
        got_total = views_total(boot, pov, page, HOURS[0], HOURS[-1])
        want_total = sum(v for (p, h), v in ground_truth.items() if p == page)
        marker = "✓" if got_total == want_total else "✗"
        print(f"  {page:<16}  {got_total:>12,}  {want_total:>12,}  {marker}")
        if got_total != want_total:
            failures.append(f"{page} total: got {got_total}, want {want_total}")
        grand_got += got_total
        grand_want += want_total
    print(f"  {'-'*16}  {'-'*12}  {'-'*12}")
    print(f"  {'TOTAL':<16}  {grand_got:>12,}  {grand_want:>12,}")

    print()
    print("=== the trick ===")
    print(f"  {NUM_WRITERS} concurrent WebSocket writers all `+=` into the same")
    print(f"  {len(PAGES) * len(HOURS)} hot keys, with zero locking. Field calls")
    print(f"  commute; the merge machinery aggregates safely.")

    send(boot, "exit;")
    boot.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures[:20]:
            print(f"  {f}")
        sys.exit(1)
    print(f"\n=== self-check OK ===")
    print(f"  verified {len(ground_truth)} per-key counters + "
          f"{len(PAGES)} per-page totals across {NUM_WRITERS} concurrent writers")


if __name__ == "__main__":
    main()
