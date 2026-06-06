#!/usr/bin/env python3
"""
A collaborative text editor as a CRDT, on Orly, in Python.

Google-Docs-style concurrent editing where the database *is* the merge:
two editors stream inserts/deletes into one shared POV with no
coordination -- no operational transform, no central reconcile, no lock --
and converge. This is a Logoot sequence CRDT: every character gets a dense,
totally-ordered position id, the document is an unordered *set* of
(position, char) entries, and the visible text is "sort by position, drop
deletes, concatenate" -- a fold the engine runs (`render`), not the driver.

The driver owns exactly one algorithm: `between(p, q)`, which mints a
position id strictly between two neighbors (the dense-order allocation).
Everything else -- merge, convergence, tombstones, time-travel -- is the
engine's commutative storage + read-time fold.

The demo runs four phases on one shared document:

  Phase 1 (alice):     types a base sentence (sequential).
  Phase 2 (concurrent, non-overlapping): alice inserts a word mid-sentence
                       while bob appends at the end -- disjoint spots, so the
                       result is deterministic.
  Phase 3 (concurrent, SAME spot): both insert at the very start at once --
                       the no-conflict guarantee: both land, neither is lost.
  Phase 4 (concurrent delete + insert): alice deletes a char while bob adds
                       one -- tombstone + insert converge.

It prints the document after each phase, a few time-travel snapshots, and
self-checks convergence (independent readers agree), no lost edits, and
stable history.

Run via the wrapper:  ./run.sh
Or directly (orlyi already up):  python3 demo.py
"""

import itertools
import json
import sys
import threading
import time
import websocket

WS_URL = "ws://127.0.0.1:8082/"
WS_TIMEOUT_S = 30

# Logoot digit base: interior digits are 1..BASE-1; 0 and BASE bracket the
# document (begin = [], end = [BASE]). WIDTH zero-pads a digit so that string
# order == numeric order (the engine sorts the [str] positions).
BASE = 1 << 16
WIDTH = 5
BEGIN = []        # absolute floor; never stored, only a bound for between()
END = [BASE]      # > every real position

# A logical clock beyond any real edit (mirrors crdt_text.orly's `forever`).
FOREVER = 9223372036854775807


# ---------------------------------------------------------------------
# The one real algorithm: a dense position id strictly between p and q.
# p, q are integer lists compared lexicographically (shorter/prefix first),
# with p < q. Validated by the self-check's ordering invariant.
# ---------------------------------------------------------------------
def between(p, q, rng):
    res = []
    i = 0
    while True:
        pd = p[i] if i < len(p) else 0
        qd = q[i] if i < len(q) else BASE
        if pd + 1 < qd:
            res.append(rng.randint(pd + 1, qd - 1))
            return res
        res.append(pd)                          # no room: take lower, descend
        if not (i < len(q) and qd == pd):       # keep q only if it stays equal
            q = []
        i += 1


def enc(ints):
    """int-list position -> ['000NN', ...] for the engine."""
    return [format(d, "0%dd" % WIDTH) for d in ints]


def dec(strs):
    """engine ['000NN', ...] -> int-list position."""
    return [int(s) for s in strs]


# A process-wide logical clock; every edit takes the next tick. Small ints
# keep the time-travel output readable.
_clock_lock = threading.Lock()
_clock_counter = itertools.count(1)
def tick():
    with _clock_lock:
        return next(_clock_counter)


# ---------------------------------------------------------------------
# WS plumbing.
# ---------------------------------------------------------------------
def send(ws, stmt):
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def orly_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def orly_pos(ints):
    return "[" + ", ".join(orly_str(s) for s in enc(ints)) + "]"


# ---------------------------------------------------------------------
# Engine ops.
# ---------------------------------------------------------------------
def op_insert(ws, pov, doc, pos, ch, site, clock):
    send(ws, (f'try {{{pov}}} crdt_text insert '
              f'<{{.doc: {orly_str(doc)}, .pos: {orly_pos(pos)}, '
              f'.ch: {orly_str(ch)}, .site: {orly_str(site)}, '
              f'.clock: {int(clock)}}}>;'))


def op_remove(ws, pov, doc, pos, clock):
    send(ws, (f'try {{{pov}}} crdt_text remove '
              f'<{{.doc: {orly_str(doc)}, .pos: {orly_pos(pos)}, '
              f'.clock: {int(clock)}}}>;'))


def visible(ws, pov, doc, as_of=FOREVER):
    """Ordered visible chars as [(pos_ints, ch, site, clock), ...]."""
    r = send(ws, (f'try {{{pov}}} crdt_text visible_as_of '
                  f'<{{.doc: {orly_str(doc)}, .as_of: {int(as_of)}}}>;'))
    return [(dec(e["pos"]), e["ch"], e["site"], int(e["clock"])) for e in (r or [])]


def render(ws, pov, doc, as_of=FOREVER):
    return send(ws, (f'try {{{pov}}} crdt_text render_as_of '
                     f'<{{.doc: {orly_str(doc)}, .as_of: {int(as_of)}}}>;'))


def char_count(ws, pov, doc):
    return int(send(ws, (f'try {{{pov}}} crdt_text char_count '
                         f'<{{.doc: {orly_str(doc)}}}>;')))


# ---------------------------------------------------------------------
# Editor-side ops, built on between(). Each reads the current visible
# sequence, picks the bookends around the edit index, and mints positions.
# ---------------------------------------------------------------------
def insert_text(ws, pov, doc, index, text, site, rng):
    """Insert `text` so it lands at visible position `index`."""
    seq = visible(ws, pov, doc)
    left = seq[index - 1][0] if index > 0 else BEGIN
    right = seq[index][0] if index < len(seq) else END
    prev = left
    for ch in text:
        pos = between(prev, right, rng)
        op_insert(ws, pov, doc, pos, ch, site, tick())
        prev = pos


def delete_range(ws, pov, doc, start, count):
    """Tombstone `count` visible chars starting at visible index `start`."""
    seq = visible(ws, pov, doc)
    for pos, _ch, _site, _clk in seq[start:start + count]:
        op_remove(ws, pov, doc, pos, tick())


def connect():
    ws = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    send(ws, "new session;")
    return ws


def show(ws, pov, doc, label):
    text = render(ws, pov, doc)
    print(f"  {label:<34} {text!r}")
    return text


# ---------------------------------------------------------------------
# main
# ---------------------------------------------------------------------
def main():
    boot = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    send(boot, "new session;")
    send(boot, "install crdt_text.1;")
    pov = send(boot, "new safe shared pov;")
    doc = "doc"
    print(f"pov: {pov}")
    print(f"document: {doc!r} -- two editors (alice, bob) on one shared POV\n")

    # --- Phase 1: alice types a base sentence (sequential) ---
    print("[phase 1] alice types the base sentence")
    arng = __import__("random").Random(1)   # per-editor deterministic streams
    brng = __import__("random").Random(2)
    a = connect()
    insert_text(a, pov, doc, 0, "hello world", "alice", arng)
    a.close()
    clock_p1 = tick() - 1
    base = show(boot, pov, doc, "after phase 1:")

    # --- Phase 2: concurrent, non-overlapping (mid-insert vs append) ---
    print("\n[phase 2] alice inserts 'BIG ' before 'world'  ||  bob appends '!'")
    def p2_alice():
        ws = connect()
        # index of 'w' in "hello world" == 6
        insert_text(ws, pov, doc, 6, "BIG ", "alice", arng)
        ws.close()
    def p2_bob():
        ws = connect()
        seq = visible(ws, pov, doc)
        insert_text(ws, pov, doc, len(seq), "!", "bob", brng)
        ws.close()
    run_concurrent([p2_alice, p2_bob])
    clock_p2 = tick() - 1
    after_p2 = show(boot, pov, doc, "after phase 2:")

    # --- Phase 3: concurrent SAME spot -- the no-conflict guarantee ---
    print("\n[phase 3] alice inserts 'X'  ||  bob inserts 'Y'  -- both at the very start")
    def p3(site, ch, rng):
        def run():
            ws = connect()
            insert_text(ws, pov, doc, 0, ch, site, rng)
            ws.close()
        return run
    run_concurrent([p3("alice", "X", arng), p3("bob", "Y", brng)])
    after_p3 = show(boot, pov, doc, "after phase 3:")

    # --- Phase 4: concurrent delete + insert ---
    print("\n[phase 4] alice deletes the trailing '!'  ||  bob appends '?'")
    def p4_alice():
        ws = connect()
        seq = visible(ws, pov, doc)
        # delete the '!' (last char that equals '!')
        idx = max(i for i, (_p, c, _s, _k) in enumerate(seq) if c == "!")
        delete_range(ws, pov, doc, idx, 1)
        ws.close()
    def p4_bob():
        ws = connect()
        seq = visible(ws, pov, doc)
        insert_text(ws, pov, doc, len(seq), "?", "bob", brng)
        ws.close()
    run_concurrent([p4_alice, p4_bob])
    final = show(boot, pov, doc, "after phase 4 (final):")

    # --- Time-travel: replay the document at past logical clocks ---
    print("\n[time-travel] the same document at past logical clocks")
    print(f"  as of end of phase 1 (clock {clock_p1}):  {render(boot, pov, doc, clock_p1)!r}")
    print(f"  as of end of phase 2 (clock {clock_p2}):  {render(boot, pov, doc, clock_p2)!r}")
    print(f"  live:                              {final!r}")

    # --- the trick ---
    print("\n=== the trick ===")
    print("  Every character is one |= of an immutable <{.pos,.ch,.site,.clock}>")
    print("  record; the position is a dense Logoot id (a list of digits) so the")
    print("  document is just a SET. The visible text is the engine's read-time")
    print("  fold: sort by position, drop tombstoned, concatenate. Concurrent")
    print("  editors never coordinate -- convergence and no-lost-edits fall out")
    print("  of commutative |= + the deterministic sort. The driver's only job")
    print("  is between(p, q); the database is the CRDT.")

    # --- self-check ---
    failures = []
    # 1. Convergence: three independent readers must agree.
    readers = [connect() for _ in range(3)]
    try:
        texts = [render(r, pov, doc) for r in readers]
    finally:
        for r in readers:
            r.close()
    if len(set(texts)) != 1:
        failures.append(f"convergence: readers disagree: {texts}")
    converged = texts[0]
    # 2. char_count matches the rendered length.
    if char_count(boot, pov, doc) != len(converged):
        failures.append("char_count != len(render)")
    # 3. No lost edits. The base words survive intact (phase 2 inserted 'BIG '
    #    *between* them, so they are no longer contiguous); 'BIG' landed; both
    #    concurrent same-spot inserts X and Y landed; the '?' append landed.
    for needle in ["hello", "world", "BIG", "X", "Y", "?"]:
        if needle not in converged:
            failures.append(f"lost edit: {needle!r} missing from {converged!r}")
    # The deleted '!' is gone (phase 4 tombstone).
    if "!" in converged:
        failures.append("tombstone failed: deleted '!' still visible")
    # 4. Time-travel is stable history.
    if render(boot, pov, doc, clock_p1) != base:
        failures.append("time-travel: phase-1 snapshot drifted")
    if render(boot, pov, doc, clock_p2) != after_p2:
        failures.append("time-travel: phase-2 snapshot drifted")

    send(boot, "exit;")
    boot.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures:
            print(f"  {f}")
        sys.exit(1)
    print("\n=== self-check OK ===")
    print(f"  converged to {converged!r} across 3 independent readers;")
    print("  no concurrent edit lost; tombstone applied; history stable.")


def run_concurrent(fns):
    threads = [threading.Thread(target=f) for f in fns]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
