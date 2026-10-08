"""Tetris drain smoke (#801); run by run-tetris-drain.sh.

Since #801 a Tetris player plays rounds back to back for a few milliseconds per turn instead of
one round per turn, and tells sessions about promotions once per turn. Each round still takes
its own snapshot of the parent and commits before the next, so:

  drain     eight paused POVs, each holding 1,000 plain writes and 100 guarded `+=` writes (an
            `if` that Tetris replays), are unpaused together. Every write must reach the global
            POV, the POVs' counters exactly, the guarded total exactly (no replay may fail), and
            a pause of one POV in the middle of the drain must hold its backlog still.
  claims    eight paused POVs each claim one shared key, then write 20 times. Exactly one claim
            may be promoted: every other claim's replay sees the owner the first one wrote, in a
            later round of the same turn, so its POV fails and none of its writes are promoted.
            Before rounds were separate, testing them against one snapshot would have let them
            all through.
"""
import os
import sys
import threading
import time

import orly

PKG = "tetris_drain"
URL = os.environ.get("ORLY_URL", "ws://127.0.0.1:19982/")
TIMEOUT_S = float(os.environ.get("DRAIN_TIMEOUT_S", "180"))
POVS = 8
PUTS = 1000
BUMP_EVERY = 10
CLAIM_PUTS = 20


def fail(msg):
    print(f"TETRIS DRAIN FAIL: {msg}", flush=True)
    sys.exit(1)


def connect():
    c = orly.connect(URL, timeout=10, recv_timeout=300)
    c.new_session()
    return c


def read(c, method, args=None):
    """Read through a fresh POV off the global POV, so we see only what Tetris promoted."""
    pov = c.new_pov(safe=False, shared=False)
    return c.call(pov, PKG, method, args or {})


def wait_until(what, pred, timeout=TIMEOUT_S):
    deadline = time.monotonic() + timeout
    while True:
        got = pred()
        if got:
            return got
        if time.monotonic() > deadline:
            fail(f"{what}: not after {timeout:.0f}s")
        time.sleep(0.1)


def fill(povs, base, writes):
    """writes(i) gives POV i's calls, in order: (method, args). Two writers per POV would
    reorder them, so one writer per POV."""
    errors = []

    def one(i):
        try:
            w = connect()
            for method, args in writes(i):
                w.call(povs[i], PKG, method, args)
            w.close()
        except Exception as ex:  # noqa: BLE001
            errors.append(f"pov {base + i}: {ex}")

    threads = [threading.Thread(target=one, args=(i,)) for i in range(len(povs))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        fail("writes: " + "; ".join(errors[:3]))


def drain(c):
    c.call(c.new_pov(safe=True, shared=True), PKG, "seed")
    wait_until("seed promoted", lambda: int(read(c, "bumps")) == 0, 60)
    povs = [c.new_pov(safe=True, shared=True) for _ in range(POVS)]
    for pov in povs:
        c.pause(pov)

    def writes(i):
        for n in range(1, PUTS + 1):
            yield "put", {"p": i, "n": n}
            if n % BUMP_EVERY == 0:
                yield "bump", {}

    fill(povs, 0, writes)
    total = POVS * PUTS
    bumps = POVS * (PUTS // BUMP_EVERY)
    start = time.monotonic()
    for pov in povs:
        c.unpause(pov)
    # Pause POV 0 part way through: its backlog must then hold still.
    paused_at = wait_until("drain under way", lambda: (lambda n: n if 0 < n else None)(int(read(c, "count_of", {"p": 0}))))
    c.pause(povs[0])
    held = int(read(c, "count_of", {"p": 0}))
    time.sleep(1.0)
    after = int(read(c, "count_of", {"p": 0}))
    if after != held:
        fail(f"POV 0 kept promoting while paused: {held} -> {after}")
    c.unpause(povs[0])
    got = wait_until("every write promoted", lambda: (lambda n: n if n >= total else None)(int(read(c, "total"))))
    took = time.monotonic() - start
    if got != total:
        fail(f"total is {got}, expected {total}")
    counts = [int(read(c, "count_of", {"p": i})) for i in range(POVS)]
    if counts != [PUTS] * POVS:
        fail(f"per-POV counts {counts}, expected {PUTS} each")
    got_bumps = int(wait_until("guarded writes promoted", lambda: (lambda n: n if n >= bumps else None)(int(read(c, "bumps"))), 30))
    if got_bumps != bumps:
        fail(f"guarded total is {got_bumps}, expected {bumps}")
    print(f"drain: {total + bumps} updates from {POVS} POVs promoted in {took:.1f}s "
          f"(POV 0 paused at {held} of {PUTS} after reaching {paused_at}); counts and guarded total exact", flush=True)
    print(f"METRIC drain_updates_per_s {(total + bumps) / took:.0f}", flush=True)


def claims(c):
    base = 100
    before = int(read(c, "total"))
    povs = [c.new_pov(safe=True, shared=True) for _ in range(POVS)]
    for pov in povs:
        c.pause(pov)

    def writes(i):
        yield "claim", {"p": base + i}
        for n in range(1, CLAIM_PUTS + 1):
            yield "put", {"p": base + i, "n": n}

    fill(povs, base, writes)
    for pov in povs:
        c.unpause(pov)
    owner = wait_until("a claim promoted", lambda: read(c, "owner"), 60)
    owner = int(owner)
    if not base <= owner < base + POVS:
        fail(f"owner is {owner}, not one of the claimants")
    # The winner's writes follow its claim; the losers fail after ten replays. Wait for both.
    wait_until("the winner's writes promoted", lambda: int(read(c, "count_of", {"p": owner})) >= CLAIM_PUTS, 60)
    time.sleep(3.0)
    counts = {base + i: int(read(c, "count_of", {"p": base + i})) for i in range(POVS)}
    winners = {p: n for p, n in counts.items() if n}
    if winners != {owner: CLAIM_PUTS}:
        fail(f"owner {owner}, but promoted writes per claimant are {counts}: exactly the owner's "
             f"{CLAIM_PUTS} should be")
    if int(read(c, "owner")) != owner:
        fail("the owner changed after the first claim was promoted")
    got = int(read(c, "total")) - before
    if got != CLAIM_PUTS:
        fail(f"total grew by {got}, expected {CLAIM_PUTS}")
    print(f"claims: {POVS} competing claims, one promoted (POV {owner}), the other {POVS - 1} POVs failed", flush=True)


def main():
    c = connect()
    c.install(PKG, 1)
    drain(c)
    claims(c)
    print("TETRIS DRAIN OK", flush=True)


if __name__ == "__main__":
    main()
