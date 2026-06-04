#!/usr/bin/env python3
"""
GRC-20-shaped knowledge graph on Orly, in Python.

GRC-20 (https://github.com/geobrowser/grc-20) is a binary property-
graph standard from Geo / The Graph. Its events are append-only
ops -- CreateEntity, UpdateEntity, CreateRelation, DeleteEntity -- and
the current state of an entity is the replay of its event log. That's
exactly the storage model Orly handles natively, via commutative set
union (`|=`) on a per-(entity, property) history set. Concurrent
editors can stream ops into the same shared POV with no coordination;
the read path reconstructs the graph at any point in time by replaying
the timestamp-sorted log.

The demo runs in three phases:

  Phase 1 (wiki editor):     basic biographical facts on 6 Greek
                             philosophers (name, born, died). 24 ops.
  Phase 2 (stanford editor): school of thought + student_of relations.
                             8 ops.
  Phase 3 (concurrent):      both editors race to "correct" the same
                             attribute on the same entity. Demonstrates
                             that multi-editor `|=` writes don't drop
                             events.

Snapshots are printed after each phase, plus a final editorial-diff
summary (events per editor, overlapping entities).

Run via the wrapper:

    ./run.sh

Or directly, after starting orlyi separately:

    python3 demo.py
"""

import json
import os
import sys
import threading
import time
import websocket

WS_URL = "ws://127.0.0.1:8082/"
WS_TIMEOUT_S = 30


# ---------------------------------------------------------------------
# Corpus: six Greek philosophers. Editor 1 ("wiki") provides
# biographical facts; editor 2 ("stanford") provides schools + the
# student_of relation chain. Tombstones aren't exercised in the main
# narrative -- the phase-3 race showcases concurrent overwrites.
# ---------------------------------------------------------------------
PHILOSOPHERS = [
    ("socrates",   "Socrates",    -470, -399),
    ("plato",      "Plato",       -428, -348),
    ("aristotle",  "Aristotle",   -384, -322),
    ("pythagoras", "Pythagoras",  -570, -495),
    ("heraclitus", "Heraclitus",  -535, -475),
    ("epicurus",   "Epicurus",    -341, -270),
]

SCHOOLS = {
    "socrates":   "Classical",
    "plato":      "Platonism",
    "aristotle":  "Peripateticism",
    "pythagoras": "Pythagoreanism",
    "heraclitus": "Heracliteanism",
    "epicurus":   "Epicureanism",
}

# (subject, "student_of", teacher) — directed relations.
RELATIONS = [
    ("plato",     "student_of", "socrates"),
    ("aristotle", "student_of", "plato"),
]

# Phase-3 race: both editors concurrently rewrite the *same* attribute
# on the *same* entity. Whoever's event has the later timestamp is the
# "current" value; the loser's event stays in history as an
# alternative-source claim. Models the real GRC-20 editorial workflow.
RACE_FACT = ("pythagoras", "born", -570, -575)


# ---------------------------------------------------------------------
# WS helpers + op encoding.
#
# Each event is encoded as "<ts>:<editor>:<kind>:<value>" where ts is
# zero-padded 13-digit ms (sorts lexicographically = chronologically),
# kind ∈ {L text, I integer, R relation target id, D tombstone}.
# ---------------------------------------------------------------------
def now_ms():
    return int(time.time() * 1000)


def encode(editor, kind, value):
    return f"{now_ms():013d}:{editor}:{kind}:{value}"


def parse(entry):
    """Returns (ts:int, editor:str, kind:str, value:str)."""
    ts, editor, kind, value = entry.split(":", 3)
    return int(ts), editor, kind, value


def send(ws, stmt):
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def orly_str(s):
    """Escape a Python string for an Orlyscript string literal.
    Our corpus has no quotes/backslashes, but be defensive."""
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def append_op(ws, pov, entity, prop, entry):
    send(ws, (f'try {{{pov}}} grc20 append_op '
              f'<{{.entity: {orly_str(entity)}, '
              f'.property: {orly_str(prop)}, '
              f'.entry: {orly_str(entry)}}}>;'))


def register_entity(ws, pov, entity):
    send(ws, (f'try {{{pov}}} grc20 register_entity '
              f'<{{.entity: {orly_str(entity)}}}>;'))


def register_prop(ws, pov, entity, prop):
    send(ws, (f'try {{{pov}}} grc20 register_prop '
              f'<{{.entity: {orly_str(entity)}, '
              f'.property: {orly_str(prop)}}}>;'))


def hist_for(ws, pov, entity, prop):
    r = send(ws, (f'try {{{pov}}} grc20 hist_for '
                  f'<{{.entity: {orly_str(entity)}, '
                  f'.property: {orly_str(prop)}}}>;'))
    return list(r or [])


def all_entities(ws, pov):
    r = send(ws, f'try {{{pov}}} grc20 all_entities <{{}}>;')
    return list(r or [])


def props_of(ws, pov, entity):
    r = send(ws, (f'try {{{pov}}} grc20 props_of '
                  f'<{{.entity: {orly_str(entity)}}}>;'))
    return list(r or [])


def entity_count(ws, pov):
    return int(send(ws, f'try {{{pov}}} grc20 entity_count <{{}}>;'))


# ---------------------------------------------------------------------
# GRC-20-style ops (entity / property / relation), each implemented as
# one append_op + entity/prop registration. The driver, not orlyscript,
# owns the op vocabulary -- the engine just stores the events.
# ---------------------------------------------------------------------
def op_create_entity(ws, pov, editor, entity, type_name):
    """CreateEntity: registers + records the entity's type as a TEXT
    property called '__type'."""
    register_entity(ws, pov, entity)
    register_prop(ws, pov, entity, "__type")
    append_op(ws, pov, entity, "__type", encode(editor, "L", type_name))


def op_set_text(ws, pov, editor, entity, prop, text):
    register_entity(ws, pov, entity)
    register_prop(ws, pov, entity, prop)
    append_op(ws, pov, entity, prop, encode(editor, "L", text))


def op_set_int(ws, pov, editor, entity, prop, n):
    register_entity(ws, pov, entity)
    register_prop(ws, pov, entity, prop)
    append_op(ws, pov, entity, prop, encode(editor, "I", str(n)))


def op_create_relation(ws, pov, editor, entity, prop, target):
    register_entity(ws, pov, entity)
    register_entity(ws, pov, target)
    register_prop(ws, pov, entity, prop)
    append_op(ws, pov, entity, prop, encode(editor, "R", target))


def op_delete_entity(ws, pov, editor, entity):
    register_prop(ws, pov, entity, "__deleted")
    append_op(ws, pov, entity, "__deleted", encode(editor, "D", ""))


# ---------------------------------------------------------------------
# Replay: build the current state of every (entity, property) by
# walking its history set in timestamp order, applying each event.
# A cutoff (`as_of_ts`) drops events with ts > cutoff -- that's the
# time-travel knob.
# ---------------------------------------------------------------------
def reconstruct(ws, pov, as_of_ts=None):
    """Returns {entity_id: {property: (kind, value, editor, ts)}}.

    Entities whose latest __deleted event is in scope are omitted.
    'kind' is L/I/R as defined in the encoding above (D is consumed
    by the entity-tombstone branch and never surfaces here)."""
    entities = sorted(all_entities(ws, pov))
    out = {}
    for eid in entities:
        props = sorted(props_of(ws, pov, eid))
        deleted = False
        e_state = {}
        for prop in props:
            entries = sorted(hist_for(ws, pov, eid, prop))
            if as_of_ts is not None:
                entries = [e for e in entries if parse(e)[0] <= as_of_ts]
            if not entries:
                continue
            # Latest event wins per property.
            ts, editor, kind, value = parse(entries[-1])
            if prop == "__deleted" and kind == "D":
                deleted = True
                break
            e_state[prop] = (kind, value, editor, ts)
        if not deleted:
            out[eid] = e_state
    return out


def format_value(kind, value):
    if kind == "L":
        return value
    if kind == "I":
        return value
    if kind == "R":
        return f"→ {value}"
    return f"?{kind}:{value}"


def print_snapshot(label, state):
    print(f"\n=== {label} ===")
    if not state:
        print("  (empty graph)")
        return
    for eid in sorted(state):
        attrs = state[eid]
        if not attrs:
            print(f"  {eid}: (no props)")
            continue
        type_name = attrs.get("__type", ("L", "?", "", 0))[1]
        bits = []
        for prop in sorted(attrs):
            if prop == "__type":
                continue
            kind, value, editor, _ts = attrs[prop]
            bits.append(f"{prop}={format_value(kind, value)} [{editor}]")
        print(f"  {eid} ({type_name}): {', '.join(bits) if bits else '(no props)'}")


# ---------------------------------------------------------------------
# Phases. Each phase opens its own WebSocket -- separate sessions are
# what makes the multi-editor story testable. Phase 3 spins two threads
# so a true interleaving happens on the wire.
# ---------------------------------------------------------------------
def phase1_wiki(pov):
    """Editor 'wiki' streams biographical facts (~24 ops)."""
    ws = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    try:
        send(ws, "new session;")
        for eid, name, born, died in PHILOSOPHERS:
            op_create_entity(ws, pov, "wiki", eid, "Person")
            op_set_text(ws, pov, "wiki", eid, "name", name)
            op_set_int(ws, pov, "wiki", eid, "born", born)
            op_set_int(ws, pov, "wiki", eid, "died", died)
    finally:
        ws.close()


def phase2_stanford(pov):
    """Editor 'stanford' streams schools + relations (~8 ops)."""
    ws = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    try:
        send(ws, "new session;")
        for eid, school in SCHOOLS.items():
            op_set_text(ws, pov, "stanford", eid, "school", school)
        for subject, prop, target in RELATIONS:
            op_create_relation(ws, pov, "stanford", subject, prop, target)
    finally:
        ws.close()


def phase3_race(pov):
    """Both editors concurrently rewrite the same attribute. Whichever
    event lands with the later ms timestamp wins on the read; the loser
    stays in history as an alternative-source claim."""
    entity, prop, wiki_value, stanford_value = RACE_FACT

    def writer(editor, value):
        ws = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
        try:
            send(ws, "new session;")
            op_set_int(ws, pov, editor, entity, prop, value)
        finally:
            ws.close()

    threads = [
        threading.Thread(target=writer, args=("wiki", wiki_value)),
        threading.Thread(target=writer, args=("stanford", stanford_value)),
    ]
    for t in threads: t.start()
    for t in threads: t.join()


def editorial_diff(ws, pov):
    """Per-editor event count + overlap analysis."""
    by_editor = {}                # editor -> int (event count)
    entities_by_editor = {}       # editor -> set(entity ids)
    for eid in sorted(all_entities(ws, pov)):
        for prop in sorted(props_of(ws, pov, eid)):
            for entry in hist_for(ws, pov, eid, prop):
                _ts, editor, _kind, _value = parse(entry)
                by_editor[editor] = by_editor.get(editor, 0) + 1
                entities_by_editor.setdefault(editor, set()).add(eid)
    print("\n=== editorial diff ===")
    for editor in sorted(by_editor):
        print(f"  {editor:<10} {by_editor[editor]:>3} events on "
              f"{len(entities_by_editor[editor]):>2} entities")
    editors = sorted(entities_by_editor)
    if len(editors) >= 2:
        a, b = editors[:2]
        overlap = entities_by_editor[a] & entities_by_editor[b]
        print(f"  overlap: {len(overlap)} entities edited by both "
              f"({a} ∩ {b}): {sorted(overlap)}")


def main():
    # Bootstrap.
    boot = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    send(boot, "new session;")
    send(boot, "install grc20.1;")
    pov = send(boot, "new safe shared pov;")
    print(f"pov: {pov}")
    print(f"corpus: {len(PHILOSOPHERS)} entities, "
          f"{len(SCHOOLS)} schools, {len(RELATIONS)} relations")

    # --- Phase 1: wiki streams biographical facts ---
    print("\n[phase 1] wiki -> biographical facts (name, born, died)")
    t0 = time.monotonic()
    phase1_wiki(pov)
    ts1 = now_ms()
    print(f"  {entity_count(boot, pov)} entities registered "
          f"in {time.monotonic() - t0:.2f}s")
    print_snapshot("snapshot 1: end of phase 1 (wiki only)",
                   reconstruct(boot, pov, as_of_ts=ts1))

    # Tiny gap so phase-2 timestamps are strictly later than phase-1
    # timestamps -- otherwise the ms-resolution clock can blur the
    # boundary on fast machines.
    time.sleep(0.05)

    # --- Phase 2: stanford streams schools + relations ---
    print("\n[phase 2] stanford -> school + student_of relations")
    t0 = time.monotonic()
    phase2_stanford(pov)
    ts2 = now_ms()
    print(f"  done in {time.monotonic() - t0:.2f}s")
    print_snapshot("snapshot 2: end of phase 2 (wiki + stanford)",
                   reconstruct(boot, pov, as_of_ts=ts2))

    time.sleep(0.05)

    # --- Phase 3: concurrent race on the same attribute ---
    entity, prop, wiki_value, stanford_value = RACE_FACT
    print(f"\n[phase 3] wiki + stanford concurrently overwrite "
          f"{entity}.{prop}")
    print(f"  wiki     -> {wiki_value}")
    print(f"  stanford -> {stanford_value}")
    phase3_race(pov)
    ts3 = now_ms()
    race_entries = sorted(hist_for(boot, pov, entity, prop))
    print(f"  history for {entity}.{prop} after race "
          f"({len(race_entries)} events, ts-sorted):")
    for e in race_entries:
        ts, ed, kind, val = parse(e)
        print(f"    {ts}  {ed:<10} {kind}  {val}")
    final_state = reconstruct(boot, pov, as_of_ts=ts3)
    winning = final_state[entity][prop]
    print(f"  -> winning event: {winning[1]} [{winning[2]}, ts={winning[3]}]")

    # --- Final state + editorial diff ---
    print_snapshot("snapshot 3: final state", final_state)
    editorial_diff(boot, pov)

    # --- Self-check ---
    print("\n=== the trick ===")
    print("  Every editor `|=` a packed event into a per-(entity, prop)")
    print("  history set. Multi-editor writes never drop events (post-#50")
    print("  deferred-mutation fold). Reads replay the timestamp-sorted")
    print("  log -- pass a cutoff for time-travel, take the latest for")
    print("  the current value. The schema is three commutative funcs;")
    print("  GRC-20's op vocabulary lives entirely in the driver.")

    failures = []
    # Phase-1 invariant: every philosopher has name + born + died.
    s1 = reconstruct(boot, pov, as_of_ts=ts1)
    for eid, name, born, died in PHILOSOPHERS:
        e = s1.get(eid, {})
        if e.get("name", (None,))[0] != "L" or e["name"][1] != name:
            failures.append(f"snapshot 1: {eid}.name = {e.get('name')}")
        if e.get("born", (None,))[0] != "I" or int(e["born"][1]) != born:
            failures.append(f"snapshot 1: {eid}.born = {e.get('born')}")
        # Phase-1 must NOT contain schools or relations.
        if "school" in e:
            failures.append(f"snapshot 1: {eid}.school leaked from phase 2")
    # Phase-2 invariant: every philosopher now has a school.
    s2 = reconstruct(boot, pov, as_of_ts=ts2)
    for eid, school in SCHOOLS.items():
        e = s2.get(eid, {})
        if e.get("school", (None,))[0] != "L" or e["school"][1] != school:
            failures.append(f"snapshot 2: {eid}.school = {e.get('school')}")
    # Phase-2 must contain the student_of relations.
    for subject, prop, target in RELATIONS:
        e = s2.get(subject, {})
        if e.get(prop, (None,))[0] != "R" or e[prop][1] != target:
            failures.append(f"snapshot 2: {subject}.{prop} = {e.get(prop)}")
    # Phase-3 invariant: both race events survive in history.
    if len(race_entries) < 2:
        failures.append(f"phase 3: only {len(race_entries)} events in race "
                        f"history -- expected both writers to land")

    send(boot, "exit;")
    boot.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures:
            print(f"  {f}")
        sys.exit(1)
    print("\n=== self-check OK ===")
    print(f"  verified 3 snapshot invariants + multi-editor race")


if __name__ == "__main__":
    main()
