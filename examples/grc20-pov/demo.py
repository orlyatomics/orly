#!/usr/bin/env python3
"""
GRC-20-shaped knowledge graph on Orly, in Python.

GRC-20 (https://github.com/geobrowser/grc-20) is a property-graph
standard from Geo / The Graph. Its events are append-only ops --
CreateEntity, SetProperty, CreateRelation, DeleteEntity -- and the
current state of an entity is the *replay* of its event log: latest
write per property wins, a tombstone deletes. That's exactly Orly's
storage model: commutative set union (`|=`) into a per-(entity,
property) history set, with the replay folded inside the engine.

What's new in this version (the #95 / #96 capstone): the op is a typed
tagged union, not a string-packed (kind, val) pair --

    op_t  is  <| Text(str) | Number(int) | Relation(str) | Deleted |>

-- the history is a *set of variant-bearing records* (storable thanks to
#96), and the replay -- latest-write-wins, tombstones, value formatting,
time-travel -- runs *in orlyscript* via `reduce` + the exhaustive `when`
match, not in this driver. Compare this file's `reconstruct` to the old
one: there is no `event_key`, no latest-wins loop, no `format_value`. The
driver just enumerates entities and asks the engine `display_as_of`.

The demo runs in three phases:

  Phase 1 (wiki editor):     biographical facts on 6 Greek philosophers
                             (name, born, died).
  Phase 2 (stanford editor): school of thought + student_of relations.
  Phase 3 (concurrent):      both editors race to "correct" the same
                             attribute on the same entity -- multi-editor
                             `|=` never drops events; latest .ts wins.

Snapshots are printed after each phase (each a time-travel read at that
phase's cutoff), plus a final editorial-diff summary.

Run via the wrapper:

    ./run.sh

Or directly, after starting orlyi separately:

    python3 demo.py
"""

import json
import sys
import threading
import time
import websocket

WS_URL = "ws://127.0.0.1:8082/"
WS_TIMEOUT_S = 30

# A .ts beyond any real event -- "now / all events" (mirrors grc20.orly's
# `forever`). int64 max.
FOREVER = 9223372036854775807


# ---------------------------------------------------------------------
# Corpus: six Greek philosophers. Editor 1 ("wiki") provides
# biographical facts; editor 2 ("stanford") provides schools + the
# student_of relation chain.
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
# "current" value (the engine's `reduce` picks it); the loser's event
# stays in history as an alternative-source claim.
RACE_FACT = ("pythagoras", "born", -570, -575)


# ---------------------------------------------------------------------
# WS plumbing.
# ---------------------------------------------------------------------
def now_ms():
    return int(time.time() * 1000)


def send(ws, stmt):
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def orly_str(s):
    """Escape a Python string for an Orlyscript string literal."""
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


# ---------------------------------------------------------------------
# Writes. The engine owns the op vocabulary now: the driver hands it
# typed scalars and orlyscript builds the `op_t` variant. ts is supplied
# by the driver (now_ms) so concurrent writers get a real wall-clock
# order; the engine resolves latest-.ts-wins.
# ---------------------------------------------------------------------
def register_entity(ws, pov, entity):
    send(ws, (f'try {{{pov}}} grc20 register_entity '
              f'<{{.entity: {orly_str(entity)}}}>;'))


def register_prop(ws, pov, entity, prop):
    send(ws, (f'try {{{pov}}} grc20 register_prop '
              f'<{{.entity: {orly_str(entity)}, '
              f'.property: {orly_str(prop)}}}>;'))


def create_entity(ws, pov, entity, ts, editor, kind):
    send(ws, (f'try {{{pov}}} grc20 create_entity '
              f'<{{.entity: {orly_str(entity)}, .ts: {int(ts)}, '
              f'.editor: {orly_str(editor)}, .kind: {orly_str(kind)}}}>;'))


def delete_entity(ws, pov, entity, ts, editor):
    send(ws, (f'try {{{pov}}} grc20 delete_entity '
              f'<{{.entity: {orly_str(entity)}, .ts: {int(ts)}, '
              f'.editor: {orly_str(editor)}}}>;'))


def set_text(ws, pov, entity, prop, ts, editor, text):
    send(ws, (f'try {{{pov}}} grc20 set_text '
              f'<{{.entity: {orly_str(entity)}, .property: {orly_str(prop)}, '
              f'.ts: {int(ts)}, .editor: {orly_str(editor)}, '
              f'.text: {orly_str(text)}}}>;'))


def set_number(ws, pov, entity, prop, ts, editor, n):
    send(ws, (f'try {{{pov}}} grc20 set_number '
              f'<{{.entity: {orly_str(entity)}, .property: {orly_str(prop)}, '
              f'.ts: {int(ts)}, .editor: {orly_str(editor)}, .n: {int(n)}}}>;'))


def set_relation(ws, pov, entity, prop, ts, editor, target):
    send(ws, (f'try {{{pov}}} grc20 set_relation '
              f'<{{.entity: {orly_str(entity)}, .property: {orly_str(prop)}, '
              f'.ts: {int(ts)}, .editor: {orly_str(editor)}, '
              f'.target: {orly_str(target)}}}>;'))


# ---------------------------------------------------------------------
# GRC-20-style ops. Thin: pick the typed setter, register the property
# in the catalogue so reads can enumerate it. CreateEntity registers the
# id + stamps a typed marker on the reserved "__entity" property (which
# the engine's `entity_live` reads back); it is deliberately NOT listed
# as a normal property.
# ---------------------------------------------------------------------
def op_create_entity(ws, pov, editor, entity, type_name):
    create_entity(ws, pov, entity, now_ms(), editor, type_name)


def op_set_text(ws, pov, editor, entity, prop, text):
    register_prop(ws, pov, entity, prop)
    set_text(ws, pov, entity, prop, now_ms(), editor, text)


def op_set_int(ws, pov, editor, entity, prop, n):
    register_prop(ws, pov, entity, prop)
    set_number(ws, pov, entity, prop, now_ms(), editor, n)


def op_create_relation(ws, pov, editor, entity, prop, target):
    register_entity(ws, pov, target)
    register_prop(ws, pov, entity, prop)
    set_relation(ws, pov, entity, prop, now_ms(), editor, target)


def op_delete_entity(ws, pov, editor, entity):
    delete_entity(ws, pov, entity, now_ms(), editor)


# ---------------------------------------------------------------------
# Reads. The replay -- latest-wins, tombstone, formatting, time-travel --
# is the engine's job. `display_as_of` returns the already-formatted
# current value (or "(absent)" for an unset/deleted property);
# `entity_live_as_of` replays the entity tombstone.
# ---------------------------------------------------------------------
def display_as_of(ws, pov, entity, prop, as_of):
    return send(ws, (f'try {{{pov}}} grc20 display_as_of '
                     f'<{{.entity: {orly_str(entity)}, '
                     f'.property: {orly_str(prop)}, .as_of: {int(as_of)}}}>;'))


def entity_live_as_of(ws, pov, entity, as_of):
    return bool(send(ws, (f'try {{{pov}}} grc20 entity_live_as_of '
                          f'<{{.entity: {orly_str(entity)}, '
                          f'.as_of: {int(as_of)}}}>;')))


def all_entities(ws, pov):
    r = send(ws, f'try {{{pov}}} grc20 all_entities <{{}}>;')
    return list(r or [])


def props_of(ws, pov, entity):
    r = send(ws, (f'try {{{pov}}} grc20 props_of '
                  f'<{{.entity: {orly_str(entity)}}}>;'))
    return list(r or [])


def entity_count(ws, pov):
    return int(send(ws, f'try {{{pov}}} grc20 entity_count <{{}}>;'))


def hist_meta(ws, pov, entity, prop):
    """Raw event log for one (entity, property), but only the provenance
    fields (.ts, .editor) -- used for per-editor analytics, never for
    replay. The typed .op is resolved by the engine, so the driver does
    not parse it."""
    r = send(ws, (f'try {{{pov}}} grc20 hist_for '
                  f'<{{.entity: {orly_str(entity)}, '
                  f'.property: {orly_str(prop)}}}>;'))
    return [{"ts": int(ev["ts"]), "editor": ev["editor"]} for ev in (r or [])]


# ---------------------------------------------------------------------
# Reconstruct: enumerate the graph at `as_of`, asking the engine for each
# value. No latest-wins loop, no tombstone bookkeeping, no formatting --
# that all moved into grc20.orly. Live entities only (entity tombstone is
# an engine replay), present properties only ("(absent)" filtered out).
# ---------------------------------------------------------------------
def reconstruct(ws, pov, as_of=FOREVER):
    """Returns {entity_id: {"__type": type_str, property: display_str}}."""
    out = {}
    for eid in sorted(all_entities(ws, pov)):
        if not entity_live_as_of(ws, pov, eid, as_of):
            continue
        e_state = {"__type": display_as_of(ws, pov, eid, "__entity", as_of)}
        for prop in sorted(props_of(ws, pov, eid)):
            value = display_as_of(ws, pov, eid, prop, as_of)
            if value != "(absent)":
                e_state[prop] = value
        out[eid] = e_state
    return out


def print_snapshot(label, state):
    print(f"\n=== {label} ===")
    if not state:
        print("  (empty graph)")
        return
    for eid in sorted(state):
        attrs = dict(state[eid])
        type_name = attrs.pop("__type", "?")
        bits = [f"{prop}={attrs[prop]}" for prop in sorted(attrs)]
        body = ', '.join(bits) if bits else '(no props)'
        print(f"  {eid} ({type_name}): {body}")


# ---------------------------------------------------------------------
# Phases. Each opens its own WebSocket -- separate sessions are what make
# the multi-editor story testable. Phase 3 spins two threads so a true
# interleaving happens on the wire.
# ---------------------------------------------------------------------
def phase1_wiki(pov):
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
    """Per-editor event count + overlap analysis, over the raw log."""
    by_editor = {}                # editor -> int (event count)
    entities_by_editor = {}       # editor -> set(entity ids)
    for eid in sorted(all_entities(ws, pov)):
        for prop in sorted(props_of(ws, pov, eid)) + ["__entity"]:
            for ev in hist_meta(ws, pov, eid, prop):
                editor = ev["editor"]
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
                   reconstruct(boot, pov, as_of=ts1))

    # Tiny gap so phase-2 timestamps are strictly later than phase-1's --
    # otherwise the ms-resolution clock can blur the phase boundary.
    time.sleep(0.05)

    # --- Phase 2: stanford streams schools + relations ---
    print("\n[phase 2] stanford -> school + student_of relations")
    t0 = time.monotonic()
    phase2_stanford(pov)
    ts2 = now_ms()
    print(f"  done in {time.monotonic() - t0:.2f}s")
    print_snapshot("snapshot 2: end of phase 2 (wiki + stanford)",
                   reconstruct(boot, pov, as_of=ts2))

    time.sleep(0.05)

    # --- Phase 3: concurrent race on the same attribute ---
    entity, prop, wiki_value, stanford_value = RACE_FACT
    print(f"\n[phase 3] wiki + stanford concurrently overwrite "
          f"{entity}.{prop}")
    print(f"  wiki     -> {wiki_value}")
    print(f"  stanford -> {stanford_value}")
    phase3_race(pov)
    ts3 = now_ms()
    race_events = sorted(hist_meta(boot, pov, entity, prop),
                         key=lambda ev: (ev["ts"], ev["editor"]))
    print(f"  history for {entity}.{prop} after race "
          f"({len(race_events)} events, ts-sorted):")
    for ev in race_events:
        print(f"    {ev['ts']}  {ev['editor']}")
    winning = display_as_of(boot, pov, entity, prop, ts3)
    print(f"  -> engine-resolved current value: {entity}.{prop} = {winning}")

    # --- Final state + editorial diff ---
    print_snapshot("snapshot 3: final state", reconstruct(boot, pov, as_of=ts3))
    editorial_diff(boot, pov)

    # --- Self-check ---
    print("\n=== the trick ===")
    print("  Each op is a typed variant")
    print("    op_t is <| Text(str) | Number(int) | Relation(str) | Deleted |>")
    print("  unioned into a per-(entity, prop) set of <{.ts,.editor,.op}>")
    print("  records (storable variants, #96). Multi-editor `|=` never")
    print("  drops events (post-#50 fold). The REPLAY -- sort by .ts,")
    print("  `reduce` to the latest, `when` over the op to format/tombstone,")
    print("  filtered by an `as_of` cutoff for time-travel -- runs in the")
    print("  engine. This driver just enumerates and reads.")

    failures = []
    # Phase-1 invariant: every philosopher has name + born; no school yet.
    s1 = reconstruct(boot, pov, as_of=ts1)
    for eid, name, born, died in PHILOSOPHERS:
        e = s1.get(eid, {})
        if e.get("name") != name:
            failures.append(f"snapshot 1: {eid}.name = {e.get('name')!r}")
        if e.get("born") != str(born):
            failures.append(f"snapshot 1: {eid}.born = {e.get('born')!r}")
        if "school" in e:
            failures.append(f"snapshot 1: {eid}.school leaked from phase 2")
    # Phase-2 invariant: every philosopher now has a school.
    s2 = reconstruct(boot, pov, as_of=ts2)
    for eid, school in SCHOOLS.items():
        e = s2.get(eid, {})
        if e.get("school") != school:
            failures.append(f"snapshot 2: {eid}.school = {e.get('school')!r}")
    # Phase-2 invariant: the student_of relations are formatted "-> target".
    for subject, prop, target in RELATIONS:
        e = s2.get(subject, {})
        if e.get(prop) != f"-> {target}":
            failures.append(f"snapshot 2: {subject}.{prop} = {e.get(prop)!r}")
    # Phase-3 invariant: both race events survive in history.
    if len(race_events) < 2:
        failures.append(f"phase 3: only {len(race_events)} events in race "
                        f"history -- expected both writers to land")

    send(boot, "exit;")
    boot.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures:
            print(f"  {f}")
        sys.exit(1)
    print("\n=== self-check OK ===")
    print("  verified 3 time-travel snapshot invariants + multi-editor race")


if __name__ == "__main__":
    main()
