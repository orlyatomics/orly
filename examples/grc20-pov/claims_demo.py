#!/usr/bin/env python3
"""
Claims, not triples: provenance, evidence, disagreement and honest answers
on Orly. The driver for `claims.orly`.

Four sources describe a handful of Greek philosophers:

  wiki  -- a general encyclopedia
  sep   -- a scholarly encyclopedia
  dnb   -- a library authority file that names people in German
  blog  -- a hobbyist blog, wrong about a few things

Every statement is stored as a claim: the source is part of its key, so
when wiki and sep both say Socrates was born in 470 BC that is two claims,
each with its own confidence, time qualifier and citations. A reader picks
the sources it trusts and gets an honest answer: the agreed value, the
conflicting positions side by side, "unknown", "no claim", or "only sources
you didn't select say anything".

The driver loads the corpus (in batches, from two concurrent sessions),
then walks through the read shapes and checks every result. Any mismatch
exits non-zero, so this is also the example's smoke test.

Run via the wrapper:

    ./run-claims.sh

Or directly, against a running orlyi with claims.1 in its package dir:

    python3 claims_demo.py
"""

import os
import sys
import threading
import time

import orly

PKG = "claims"
URL = os.environ.get("ORLY_WS_URL", orly.DEFAULT_URL)

# ---------------------------------------------------------------------
# Values and time qualifiers. A client can't send a variant argument yet
# (#816), so the package's client-facing writes take flat arguments: one
# method per kind of value, and the period as two optional years.
# ---------------------------------------------------------------------
def text(s):
    return ("assert_text", {"text": s})


def number(n):
    return ("assert_number", {"n": int(n)})


UNKNOWN = ("assert_unknown", {})
ALWAYS = (None, None)


def between(since, until):
    return (since, until)


def opt_int(v):
    return orly.Lit("unknown int") if v is None else orly.Lit(f"{int(v)}?")


# ---------------------------------------------------------------------
# Corpus. Entity ids are the identity; names are just claims.
# (source, entity, property, value, confidence, period)
# ---------------------------------------------------------------------
CLAIMS = [
    # Socrates: wiki and sep agree on 470 BC, the blog says 469.
    ("wiki", "socrates", "name", text("Socrates"), 99, ALWAYS),
    ("sep",  "socrates", "name", text("Socrates"), 99, ALWAYS),
    ("dnb",  "socrates", "name", text("Sokrates"), 99, ALWAYS),
    ("wiki", "socrates", "born", number(-470), 80, ALWAYS),
    ("sep",  "socrates", "born", number(-470), 85, ALWAYS),
    ("blog", "socrates", "born", number(-469), 30, ALWAYS),
    ("wiki", "socrates", "died", number(-399), 99, ALWAYS),
    ("sep",  "socrates", "died", number(-399), 99, ALWAYS),
    ("sep",  "socrates", "school", text("Classical"), 70, ALWAYS),
    ("blog", "socrates", "school", text("Stoicism"), 10, ALWAYS),
    # Plato: the scholarly sources genuinely disagree, 428 vs 427 BC.
    ("wiki", "plato", "name", text("Plato"), 99, ALWAYS),
    ("sep",  "plato", "name", text("Plato"), 99, ALWAYS),
    ("dnb",  "plato", "name", text("Platon"), 99, ALWAYS),
    ("wiki", "plato", "born", number(-428), 70, ALWAYS),
    ("sep",  "plato", "born", number(-427), 70, ALWAYS),
    ("dnb",  "plato", "born", number(-428), 60, ALWAYS),
    ("wiki", "plato", "died", number(-348), 90, ALWAYS),
    # Aristotle, and a different person who shares the name.
    ("wiki", "aristotle", "name", text("Aristotle"), 99, ALWAYS),
    ("sep",  "aristotle", "name", text("Aristotle"), 99, ALWAYS),
    ("wiki", "aristotle", "born", number(-384), 95, ALWAYS),
    ("sep",  "aristotle", "born", number(-384), 95, ALWAYS),
    ("wiki", "aristotle-of-cyrene", "name", text("Aristotle"), 80, ALWAYS),
    # A value known to exist but not known: sep says he was born, not when.
    ("sep",  "aristotle-of-cyrene", "born", UNKNOWN, 50, ALWAYS),
    # The Academy, with time-qualified claims about who led it.
    ("wiki", "academy", "name", text("Platonic Academy"), 99, ALWAYS),
    ("wiki", "academy", "scholarch", text("Plato"), 90, between(-387, -348)),
    ("sep",  "academy", "scholarch", text("Speusippus"), 90, between(-347, -339)),
    ("wiki", "speusippus", "name", text("Speusippus"), 99, ALWAYS),
    ("wiki", "speusippus", "born", number(-408), 60, ALWAYS),
]

# (source, entity, property, target, confidence, period)
LINKS = [
    ("wiki", "plato", "student_of", "socrates", 95, ALWAYS),
    ("sep",  "plato", "student_of", "socrates", 95, ALWAYS),
    ("wiki", "aristotle", "student_of", "plato", 95, ALWAYS),
    ("sep",  "aristotle", "student_of", "plato", 95, ALWAYS),
    ("blog", "aristotle", "student_of", "socrates", 20, ALWAYS),
    ("wiki", "plato", "head_of", "academy", 90, between(-387, -348)),
    ("wiki", "speusippus", "head_of", "academy", 90, between(-347, -339)),
    ("sep",  "speusippus", "nephew_of", "plato", 80, ALWAYS),
]

# (source, entity, property, work, locator)
CITES = [
    ("sep", "socrates", "born", "Diogenes Laertius, Lives of Eminent Philosophers", "II.44"),
    ("sep", "plato", "born", "Diogenes Laertius, Lives of Eminent Philosophers", "III.2"),
    ("wiki", "plato", "born", "Diogenes Laertius, Lives of Eminent Philosophers", "III.2"),
    ("wiki", "plato", "born", "Apollodorus, Chronicle (as quoted by Diogenes)", "III.2"),
]

SCHOLARS = {"wiki", "sep"}
EVERYONE = {"wiki", "sep", "dnb", "blog"}

failures = []


def check(label, ok, got=None):
    if ok:
        print(f"  ok    {label}")
    else:
        print(f"  FAIL  {label}: got {got!r}")
        failures.append(label)


# ---------------------------------------------------------------------
# Small helpers over the engine's JSON (ints come back as floats,
# variants as {"Tag": payload}, sets as unordered arrays).
# ---------------------------------------------------------------------
def tag(v):
    (k,) = v.keys()
    return k


def show_value(v):
    t = tag(v)
    if t == "Number":
        n = int(v["Number"])
        return f"{-n} BC" if n < 0 else f"AD {n}"
    if t == "Text":
        return repr(v["Text"])
    return "(unknown value)"


def show_answer(a):
    t = tag(a)
    if t == "NoClaim":
        return "no claim"
    if t == "NotSelected":
        return f"not selected (only {sorted(a[t])} claim it)"
    if t == "UnknownValue":
        return f"unknown value (per {sorted(a[t])})"
    if t == "Agreed":
        return f"{show_value(a[t]['value'])} (agreed by {sorted(a[t]['sources'])})"
    return "CONFLICT: " + " vs ".join(
        f"{show_value(p['value'])} {sorted(p['sources'])}" for p in a[t])


def show_period(p):
    t = tag(p)
    if t == "Always":
        return ""
    if t == "Between":
        return f" [{int(p[t]['since'])}..{int(p[t]['until'])}]"
    return f" [{t.lower()} {int(p[t])}]"


def positions(a):
    return {(show_value(p["value"]), frozenset(p["sources"])) for p in a["Conflict"]}


# ---------------------------------------------------------------------
# Load: two sessions write concurrently, each one mixed batch. The
# writes go through `assert_claim`, whose effect sits under an `if`, and a
# batch replays each call at promotion (#751/#757), so none is lost.
# ---------------------------------------------------------------------
def claim_call(row):
    """(method, args) for one corpus claim."""
    src, e, p, (method, value_args), conf, (since, until) = row
    args = {"source": src, "entity": e, "property": p, "confidence": conf,
            "since": opt_int(since), "until": opt_int(until)}
    args.update(value_args)
    return method, args


def link_args(row):
    src, e, p, t, conf, (since, until) = row
    return {"source": src, "entity": e, "property": p, "target": t, "confidence": conf,
            "since": opt_int(since), "until": opt_int(until)}


def load(pov):
    halves = [CLAIMS[0::2], CLAIMS[1::2]]
    errors = []

    def writer(rows):
        try:
            with orly.connect(URL) as c:
                c.new_session()
                # One mixed batch: every call a different method, one transaction.
                c.call_many(pov, [(PKG, *claim_call(r)) for r in rows])
        except Exception as ex:  # surfaced below
            errors.append(ex)

    threads = [threading.Thread(target=writer, args=(h,)) for h in halves]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise errors[0]


def wait_for(c, pov, method, args, pred, what, timeout_s=60):
    """Writes reach other sessions' reads once Tetris promotes them, so a
    read can lag a just-acknowledged write briefly. Poll until `pred`."""
    deadline = time.time() + timeout_s
    while True:
        got = c.call(pov, PKG, method, args)
        if pred(got):
            return got
        if time.time() > deadline:
            check(f"{what} (waited {timeout_s}s)", False, got)
            return got
        time.sleep(0.2)


def main():
    with orly.connect(URL) as c:
        c.new_session()
        c.install(PKG, 1)
        pov = c.new_pov()

        print("[load] 2 concurrent sessions batch-assert %d claims" % len(CLAIMS))
        load(pov)
        c.call_batch(pov, PKG, "link", [link_args(r) for r in LINKS])
        print(f"[load] {len(LINKS)} link claims")
        # Evidence is a set unioned with `|=`, which is commutative, so two
        # citations of the same claim in one batch both land.
        cited = c.call_batch(pov, PKG, "cite",
                             [{"source": s, "entity": e, "property": p, "work": w, "locator": l}
                              for s, e, p, w, l in CITES])
        check("every citation attached to an existing claim", all(cited), cited)
        # Evidence for a claim nobody made is refused, not stored.
        orphan = c.call(pov, PKG, "cite", {"source": "blog", "entity": "plato", "property": "born",
                                           "work": "x", "locator": "y"})
        check("citing a claim that doesn't exist returns false", orphan is False, orphan)
        wait_for(c, pov, "catalogue", {"trusted": EVERYONE},
                 lambda rows: len(rows) == 6 and all(
                     len(c.call(pov, PKG, "claims", {"entity": s, "property": p})) ==
                     sum(1 for r in CLAIMS if r[1] == s and r[2] == p)
                     for s, p in {(r[1], r[2]) for r in CLAIMS}),
                 "all claims visible")

        print("\n=== 1. provenance: every source's claim, side by side ===")
        rows = c.call(pov, PKG, "claims", {"entity": "socrates", "property": "born"})
        for r in rows:
            ev = "; ".join(f"{x['work']} {x['locator']}" for x in r["evidence"]) or "-"
            print(f"  socrates.born  {r['source']:5} {show_value(r['value']):8} "
                  f"confidence {int(r['confidence'])}%  evidence: {ev}")
        check("two sources with the same value are two claims",
              [r["source"] for r in rows] == ["blog", "sep", "wiki"], rows)
        check("evidence belongs to the sep claim only",
              [len(r["evidence"]) for r in rows] == [0, 1, 0], rows)

        print("\n=== 2. honest answers, by who you trust ===")
        cases = [
            ("socrates", "born", SCHOLARS, "Agreed"),
            ("socrates", "born", EVERYONE, "Conflict"),
            ("plato", "born", SCHOLARS, "Conflict"),
            ("plato", "born", {"wiki", "dnb"}, "Agreed"),
            ("socrates", "school", {"wiki"}, "NotSelected"),
            ("aristotle-of-cyrene", "born", SCHOLARS, "UnknownValue"),
            ("aristotle-of-cyrene", "died", EVERYONE, "NoClaim"),
        ]
        for e, p, trusted, want in cases:
            a = c.call(pov, PKG, "answer", {"entity": e, "property": p, "trusted": trusted})
            print(f"  {e}.{p} trusting {sorted(trusted)}: {show_answer(a)}")
            check(f"{e}.{p} for {sorted(trusted)} is {want}", tag(a) == want, a)
        a = c.call(pov, PKG, "answer", {"entity": "plato", "property": "born", "trusted": SCHOLARS})
        check("plato.born conflict shows both positions with their sources",
              positions(a) == {("428 BC", frozenset({"wiki"})), ("427 BC", frozenset({"sep"}))}, a)

        print("\n=== 3. time qualifiers: who led the Academy? ===")
        a = c.call(pov, PKG, "answer", {"entity": "academy", "property": "scholarch", "trusted": SCHOLARS})
        print(f"  ever:        {show_answer(a)}")
        check("without a year the two scholarchs look like a conflict", tag(a) == "Conflict", a)
        for year, want in ((-350, "'Plato'"), (-340, "'Speusippus'")):
            a = c.call(pov, PKG, "answer_at", {"entity": "academy", "property": "scholarch",
                                               "trusted": SCHOLARS, "year": year})
            print(f"  in {-year} BC: {show_answer(a)}")
            check(f"in {-year} BC the answer is {want}",
                  tag(a) == "Agreed" and show_value(a["Agreed"]["value"]) == want, a)

        print("\n=== 4. identity is the id, never the name ===")
        named = c.call(pov, PKG, "named", {"text": "Aristotle"})
        for n in named:
            print(f"  'Aristotle' -> {n['entity']} (named so by {sorted(n['sources'])})")
        check("two entities named Aristotle stay two entities",
              [n["entity"] for n in named] == ["aristotle", "aristotle-of-cyrene"], named)
        for trusted in (SCHOLARS, {"dnb"}):
            lbl = c.call(pov, PKG, "label", {"entity": "plato", "trusted": trusted})
            print(f"  plato's label for {sorted(trusted)}: {lbl}")
        a = c.call(pov, PKG, "answer", {"entity": "plato", "property": "born", "trusted": {"dnb"}})
        check("dnb's 'Platon' claims join on the id: dnb's birth year is plato's",
              tag(a) == "Agreed" and a["Agreed"]["sources"] == ["dnb"], a)

        print("\n=== 5. point read by id ===")
        d = c.call(pov, PKG, "describe", {"entity": "socrates", "trusted": SCHOLARS})
        print(f"  {d['entity']} described by {sorted(d['described_by'])}")
        for f in d["facts"]:
            print(f"    {f['property']:8} {show_answer(f['answer'])}")
        check("describe lists every property any source claims",
              [f["property"] for f in d["facts"]] == ["born", "died", "name", "school"], d)
        empty = c.call(pov, PKG, "describe", {"entity": "nobody", "trusted": EVERYONE})
        check("an unknown id reads as empty, not an error", empty["facts"] == [], empty)

        print("\n=== 6. one-hop neighbours ===")
        n = c.call(pov, PKG, "neighbours", {"entity": "plato", "trusted": SCHOLARS})
        for l in n["links"]:
            arrow = "->" if l["direction"] == "out" else "<-"
            print(f"  plato {arrow} {l['property']:10} {l['other']:11} {sorted(l['sources'])}")
        print(f"  ({int(n['unselected'])} link claims from unselected sources left out)")
        got = [(l["direction"], l["property"], l["other"]) for l in n["links"]]
        check("plato's trusted links, both directions",
              got == [("out", "head_of", "academy"), ("out", "student_of", "socrates"),
                      ("in", "nephew_of", "speusippus"), ("in", "student_of", "aristotle")], got)
        n = c.call(pov, PKG, "neighbours", {"entity": "socrates", "trusted": SCHOLARS})
        check("the blog's aristotle->socrates link is hidden and counted",
              int(n["unselected"]) == 1 and len(n["links"]) == 1, n)
        n = c.call(pov, PKG, "neighbours_at", {"entity": "academy", "trusted": SCHOLARS, "year": -340})
        check("the Academy's head in 340 BC is Speusippus",
              [l["other"] for l in n["links"]] == ["speusippus"], n)

        print("\n=== 7. range page over a property, keyset cursor ===")
        args = {"property": "born", "lo": -500, "hi": -400, "trusted": EVERYONE, "limit": 3}
        cursor = (-500, "", "")
        pages = []
        while True:
            page = c.call(pov, PKG, "range_page", dict(args, after_n=cursor[0],
                                                       after_entity=cursor[1], after_source=cursor[2]))
            if not page:
                break
            pages.append(page)
            last = page[-1]
            cursor = (int(last["n"]), last["entity"], last["source"])
        for i, page in enumerate(pages, 1):
            print(f"  page {i}: " + ", ".join(
                f"{r['entity']}={int(r['n'])} ({r['source']})" for r in page))
        flat = [(int(r["n"]), r["entity"], r["source"]) for p in pages for r in p]
        whole = c.call(pov, PKG, "range_page", dict(args, limit=100, after_n=-500,
                                                    after_entity="", after_source=""))
        check("pages of 3 concatenate to the one big page",
              flat == [(int(r["n"]), r["entity"], r["source"]) for r in whole], flat)
        check("rows are claims in (value, entity, source) order",
              flat == sorted(flat) and len(flat) == 7, flat)

        print("\n=== 8. cross-source listings ===")
        for row in c.call(pov, PKG, "catalogue", {"trusted": SCHOLARS}):
            print(f"  {row['entity']:20} {row['label']:18} described by {sorted(row['sources'])}")
        disputes = c.call(pov, PKG, "disputes", {"property": "born", "trusted": SCHOLARS})
        print("  disputed 'born' among scholars: " +
              ", ".join(f"{d['entity']}: {show_answer(d['answer'])}" for d in disputes))
        check("among the scholars only plato's birth year is disputed",
              [d["entity"] for d in disputes] == ["plato"], disputes)

        print("\n=== 9. a source revises its claim ===")
        replaced = c.call(pov, PKG, *claim_call(("blog", "socrates", "born", number(-470), 40, ALWAYS)))
        check("the write reports it replaced the blog's earlier claim", replaced is True, replaced)
        a = wait_for(c, pov, "answer", {"entity": "socrates", "property": "born", "trusted": EVERYONE},
                     lambda a: tag(a) == "Agreed", "the revision is visible")
        print(f"  socrates.born trusting everyone: {show_answer(a)}")
        check("now every source agrees, still three claims",
              tag(a) == "Agreed" and sorted(a["Agreed"]["sources"]) == ["blog", "sep", "wiki"], a)
        page = c.call(pov, PKG, "range_page", dict(args, limit=100, hi=-469, after_n=-500,
                                                   after_entity="", after_source=""))
        check("the revised claim's old index row is skipped",
              [(int(r["n"]), r["source"]) for r in page if r["entity"] == "socrates"]
              == [(-470, "blog"), (-470, "sep"), (-470, "wiki")], page)

        c.exit()

    print()
    if failures:
        print(f"FAILED: {len(failures)} check(s): {failures}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
