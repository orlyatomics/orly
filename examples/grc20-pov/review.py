#!/usr/bin/env python3
"""Reviewing a GRC-20 edit before it lands (#746): a draft POV, its diff, a refused promotion,
a discard, and a clean promotion.

A `wiki` source has published Plato and Socrates to the global POV.  A `stanford` editor drafts
changes in a private POV made with conflicts="refuse", paused so nothing reaches the graph until
it's reviewed: a new name event, Plato's school, and a student_of relation.  Meanwhile `wiki`
publishes Plato's school itself.

Every GRC-20 op is a set-union into a property's history, so edits to a property that already
has history compose with anyone else's and never conflict.  But the first event of a property
*creates* its history (`new <['hist', e, p]> <- {ev}`), and stanford's draft created
plato.school while wiki was creating it too: promoting the draft would overwrite wiki's
history.  The promotion is refused and names that key.  The editor discards the draft (it reads
as the published graph again), re-applies the edits on top, and this time every change is a
union delta, so it promotes cleanly and both sources' school events are in the graph.

Run by run-review.sh; self-checks and exits nonzero on a mismatch."""

import os
import sys
import time

import orly

PKG = "grc20"
failures = 0


def check(what, got, want):
    global failures
    ok = got == want
    if not ok:
        failures += 1
        print(f"REVIEW FAIL: {what}: got {got!r}, want {want!r}")
    return ok


def wait_for(what, read, want, timeout=30.0):
    deadline = time.monotonic() + timeout
    while (got := read()) != want:
        if time.monotonic() > deadline:
            return check(f"{what} ({timeout:.0f} s)", got, want)
        time.sleep(0.1)
    return True


def show_diff(title, diff):
    print(f"--- {title}: {len(diff['changes'])} change(s) over {int(diff['updates'])} unpromoted update(s)")
    for ch in diff["changes"]:
        key = "/".join(str(k) for k in ch["key"])
        if ch["kind"] == "delta":
            n = len(ch["delta"]) if isinstance(ch["delta"], list) else ch["delta"]
            print(f"  {ch['kind']:8} {key}  ({ch['op']} of {n} item(s))")
        else:
            count = lambda v: "absent" if v is None else f"{len(v)} event(s)"
            print(f"  {ch['kind']:8} {key}  ({count(ch['before'])} -> {count(ch['after'])})")


def kinds(diff):
    return sorted(("/".join(str(k) for k in ch["key"]), ch["kind"]) for ch in diff["changes"])


with orly.connect(os.environ.get("ORLY_URL", orly.DEFAULT_URL)) as c:
    c.new_session()
    c.install(PKG, 1)
    wiki = c.new_pov(safe=False, shared=False)       # its writes promote to the global POV
    reader = c.new_pov(safe=False, shared=False)     # reads the global POV through
    display = lambda pov, e, p: c.call(pov, PKG, "display", {"entity": e, "property": p})
    ts = iter(range(1000, 100000))

    def edit(pov, editor, method, entity, prop, **extra):
        c.call(pov, PKG, method, {"entity": entity, "property": prop, "ts": next(ts), "editor": editor, **extra})
        c.call(pov, PKG, "register_prop", {"entity": entity, "property": prop})

    for entity, name in (("plato", "Plato"), ("socrates", "Socrates")):
        c.call(wiki, PKG, "create_entity", {"entity": entity, "ts": next(ts), "editor": "wiki", "kind": "Person"})
        edit(wiki, "wiki", "set_text", entity, "name", text=name)
    wait_for("wiki's graph is published", lambda: display(reader, "plato", "name"), "Plato")

    # The draft: forked from the published graph, conflicts refused, paused for review.
    draft = c.new_pov(safe=False, shared=False, conflicts="refuse")
    c.pause(draft)

    def stanford_edits():
        edit(draft, "stanford", "set_text", "plato", "name", text="Plato of Athens")
        edit(draft, "stanford", "set_text", "plato", "school", text="Platonism")
        edit(draft, "stanford", "set_relation", "plato", "student_of", target="socrates")

    stanford_edits()
    # Meanwhile wiki publishes Plato's school too.
    edit(wiki, "wiki", "set_text", "plato", "school", text="The Academy")
    wait_for("wiki's school is published", lambda: display(reader, "plato", "school"), "The Academy")

    diff = c.diff(draft)
    show_diff("the draft, against the published graph", diff)
    check("the draft's changes", kinds(diff), [
        ("hist/plato/name", "delta"),            # a union into existing history
        ("hist/plato/school", "changed"),        # created in the draft, but wiki created it too
        ("hist/plato/student_of", "added"),      # created in the draft
        ("props/plato", "delta"),
    ])
    check("the published graph is untouched", display(reader, "plato", "name"), "Plato")

    result = c.promote(draft)
    print(f"--- promote: {result['status']}; would conflict on "
          + ", ".join("/".join(str(k) for k in x["key"]) for x in result["conflicts"]))
    check("promotion refused", [result["status"], [x["key"] for x in result["conflicts"]]],
          ["refused", [["hist", "plato", "school"]]])
    check("the draft is still paused", c.review(draft)["status"], "paused")

    # Throw the draft away: it reads as the published graph again.
    discarded = c.discard(draft)
    print(f"--- discard: {int(discarded['discarded_updates'])} update(s), "
          f"{int(discarded['discarded_entries'])} entries thrown away")
    check("the draft reads as published", display(draft, "plato", "school"), "The Academy")
    check("nothing left to diff", c.diff(draft)["changes"], [])

    # Re-apply on top of the published graph: now every change is a union delta.
    stanford_edits()
    diff = c.diff(draft)
    show_diff("the redone draft", diff)
    check("only deltas now", sorted({ch["kind"] for ch in diff["changes"]}), ["added", "delta"])
    result = c.promote(draft)
    print(f"--- promote: {result['status']}, {len(result['conflicts'])} conflict(s)")
    check("promoted cleanly", [result["status"], result["conflicts"]], ["promoted", []])
    wait_for("stanford's edits are published", lambda: display(reader, "plato", "student_of"), "-> socrates")
    school_events = int(c.call(reader, PKG, "event_count", {"entity": "plato", "property": "school"}))
    print(f"--- published: plato.name = {display(reader, 'plato', 'name')!r}, "
          f"plato.school = {display(reader, 'plato', 'school')!r} ({school_events} events, both sources)")
    check("both sources' school events", school_events, 2)
    check("the latest name wins", display(reader, "plato", "name"), "Plato of Athens")

if failures:
    print(f"REVIEW FAIL: {failures} check(s) failed")
    sys.exit(1)
print("REVIEW OK")
