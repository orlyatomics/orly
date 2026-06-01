#!/usr/bin/env python3
"""
Wikipedia-style category-membership demo, in Python.

Same fold-over-keyed-versions pattern as the bitcoin time-travel demo,
but with set union instead of integer addition. Proves the polymorphic-
monoid claim from the bitcoin README: swap the operator (`|` for sets,
`+` for ints), keep the structure, get historical queries for free.

The dataset is a hand-curated timeline of when various programming
languages and cryptocurrencies came into being, by year of first
release. Real Wikipedia categorization is richer (full hour-resolution
revision history, removals, renames), but the demo's point is the
pattern, not the data.

Run via the wrapper:

    ./run.sh

Or directly, after starting orlyi separately:

    python3 demo.py
"""

import json
import sys
import websocket

WS_URL = "ws://127.0.0.1:8082/"


# Each entry: (category, year, [articles added that year])
# Sourced from Wikipedia's "first appeared" fields, rounded to year.
# This is illustrative; treat the data as approximate.
EVENTS = [
    # --- Programming languages ---
    ("languages", 1957, ["FORTRAN"]),
    ("languages", 1958, ["Lisp", "ALGOL"]),
    ("languages", 1959, ["COBOL"]),
    ("languages", 1964, ["BASIC"]),
    ("languages", 1970, ["Pascal"]),
    ("languages", 1972, ["C", "Smalltalk"]),
    ("languages", 1980, ["Ada"]),
    ("languages", 1983, ["C++", "Objective-C"]),
    ("languages", 1986, ["Erlang"]),
    ("languages", 1987, ["Perl"]),
    ("languages", 1990, ["Haskell"]),
    ("languages", 1991, ["Python", "Visual Basic"]),
    ("languages", 1993, ["Lua"]),
    ("languages", 1995, ["Java", "JavaScript", "PHP", "Ruby"]),
    ("languages", 2000, ["C#"]),
    ("languages", 2003, ["Scala", "Groovy"]),
    ("languages", 2007, ["Clojure"]),
    ("languages", 2009, ["Go"]),
    ("languages", 2010, ["Rust"]),
    ("languages", 2011, ["Dart", "Elixir", "Kotlin"]),
    ("languages", 2012, ["Julia", "TypeScript"]),
    ("languages", 2014, ["Swift"]),
    ("languages", 2016, ["Zig"]),
    ("languages", 2023, ["Mojo"]),

    # --- Cryptocurrencies ---
    ("crypto", 2009, ["Bitcoin"]),
    ("crypto", 2011, ["Litecoin", "Namecoin"]),
    ("crypto", 2012, ["Ripple"]),
    ("crypto", 2013, ["Dogecoin"]),
    ("crypto", 2014, ["Monero"]),
    ("crypto", 2015, ["Ethereum"]),
    ("crypto", 2017, ["Bitcoin Cash", "Cardano"]),
    ("crypto", 2020, ["Solana"]),
]

# Snapshot years to display, per category.
SNAPSHOTS = {
    "languages": [1957, 1965, 1975, 1985, 1995, 2005, 2015, 2024],
    "crypto":    [2008, 2010, 2012, 2014, 2016, 2018, 2020, 2024],
}


def send(ws, stmt):
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def set_literal(members):
    """Format a Python iterable as an Orlyscript string-set literal."""
    return "{" + ", ".join(f'"{m}"' for m in members) + "}"


def add_growth(ws, pov, cat, year, members):
    stmt = (f'try {{{pov}}} wiki add_growth '
            f'<{{.cat: "{cat}", .year: {year}, .members: {set_literal(members)}}}>;')
    send(ws, stmt)


def members_at(ws, pov, cat, since, target):
    """Returns a sorted list of articles in `cat` from `since` through `target`."""
    stmt = (f'try {{{pov}}} wiki members_at '
            f'<{{.cat: "{cat}", .since: {since}, .target: {target}}}>;')
    result = send(ws, stmt)
    # Orlyi serialises sets as JSON arrays.
    return sorted(result) if result else []


def count_at(ws, pov, cat, since, target):
    stmt = (f'try {{{pov}}} wiki count_at '
            f'<{{.cat: "{cat}", .since: {since}, .target: {target}}}>;')
    result = send(ws, stmt)
    return int(result) if result is not None else 0


def main():
    ws = websocket.create_connection(WS_URL)
    send(ws, "new session;")
    send(ws, "install wiki.1;")
    pov = send(ws, "new safe shared pov;")
    print(f"pov: {pov}\n")

    # Ingest the timeline.
    print(f"ingesting {len(EVENTS)} growth events across "
          f"{len({e[0] for e in EVENTS})} categories...")
    for cat, year, members in EVENTS:
        add_growth(ws, pov, cat, year, members)

    # The headline output: snapshots of each category at chosen years.
    cat_pretty = {"languages": "Programming languages", "crypto": "Cryptocurrencies"}
    for cat, years in SNAPSHOTS.items():
        print()
        print(f"=== {cat_pretty[cat]} ===")
        for y in years:
            members = members_at(ws, pov, cat, 0, y)
            count = len(members)
            print(f"  {y}  ({count:>2}): {', '.join(members) if members else '(empty)'}")

    # Cross-category sanity: at the same year, the two sets are disjoint
    # and querying one doesn't bleed into the other.
    print()
    print("=== cross-category isolation ===")
    lang_2024 = set(members_at(ws, pov, "languages", 0, 2024))
    crypto_2024 = set(members_at(ws, pov, "crypto", 0, 2024))
    overlap = lang_2024 & crypto_2024
    print(f"  |languages 2024| = {len(lang_2024)}")
    print(f"  |crypto 2024|    = {len(crypto_2024)}")
    print(f"  overlap          = {overlap or 'empty (good — different keyspaces)'}")

    # The polymorphic-monoid demo: same fold pattern, different operator
    # than the bitcoin demo.
    print()
    print("=== the trick ===")
    print("  bitcoin demo:  [0..h] reduce start 0 + delta_at(addr, that)")
    print("  this demo:     [since..target] reduce start (empty {str}) | growth_in(cat, that)")
    print("  same pattern, different commutative monoid.")

    # Self-check.
    failures = []
    expectations = [
        ("languages", 1957, ["FORTRAN"]),
        ("languages", 1958, ["ALGOL", "FORTRAN", "Lisp"]),
        ("languages", 1995, ["ALGOL", "Ada", "BASIC", "C", "C++", "COBOL", "Erlang",
                             "FORTRAN", "Haskell", "Java", "JavaScript", "Lisp", "Lua",
                             "Objective-C", "PHP", "Pascal", "Perl", "Python", "Ruby",
                             "Smalltalk", "Visual Basic"]),
        ("crypto", 2009, ["Bitcoin"]),
        ("crypto", 2024, ["Bitcoin", "Bitcoin Cash", "Cardano", "Dogecoin", "Ethereum",
                          "Litecoin", "Monero", "Namecoin", "Ripple", "Solana"]),
    ]
    for cat, year, expected in expectations:
        got = members_at(ws, pov, cat, 0, year)
        if got != expected:
            failures.append(f"{cat} at {year}: got {got!r}, want {expected!r}")

    send(ws, "exit;")
    ws.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures:
            print(f"  {f}")
        sys.exit(1)
    print(f"\n=== self-check OK ===\n  verified {len(expectations)} snapshot queries")


if __name__ == "__main__":
    main()
