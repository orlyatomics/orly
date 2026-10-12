#!/usr/bin/env python3
"""
Native versioned reads on Orly: "what did this say last week, and what
changed since". The driver for `versioned.orly`.

`grc20.orly` keeps an event log so history is a fold over events. This
example keeps only the current value of each property and asks the engine
for the past instead: every write to the global POV has a version, and a
read can run as of any retained version, wall-clock time or named save
point (docs/PROTOCOL.md, "Reads at a version"). Nothing is folded in user
space and nothing is stored twice.

Needs a disk-backed orlyi started with a retention flag
(--retention_seconds or --retention_versions) and a fresh store; see the
README for the steps.

    python3 versioned_demo.py
"""

import datetime
import os
import sys
import time

import orly

PKG = "versioned"
URL = os.environ.get("ORLY_WS_URL", orly.DEFAULT_URL)
GLOBAL = "C4EF7C46-28C5-4000-8CCD-C8E799E2C3F3"

failures = []


def check(what, ok, got=None):
    print(f"  {'ok  ' if ok else 'FAIL'} {what}" + ("" if ok else f"  (got {got!r})"))
    if not ok:
        failures.append(what)


def main():
    with orly.connect(URL) as c:
        c.new_session()
        c.install(PKG, 1)
        pov = c.new_pov()

        def write(method, **args):
            """Write through a session POV and wait until the write is in the
            global POV (promotion is asynchronous); return the global version."""
            before = c.version_of(GLOBAL)
            c.call(pov, PKG, method, args)
            deadline = time.time() + 60
            while True:
                v = c.version_of(GLOBAL)
                if v > before:
                    return v
                if time.time() > deadline:
                    check(f"{method} reached the global POV", False, v)
                    return v
                time.sleep(0.1)

        def snapshot(entity, at=None):
            # Now is read on the session POV; the past is read on the global POV.
            rows = c.call(pov if at is None else GLOBAL, PKG, "snapshot", {"entity": entity}, at=at)
            return {r["property"]: r["value"] for r in rows}

        def changed(old, new):
            out = {}
            for k in sorted(set(old) | set(new)):
                if old.get(k) != new.get(k):
                    out[k] = (old.get(k), new.get(k))
            return out

        print("=== 1. build a small graph ===")
        write("set_text", entity="plato", property="name", text="Plato")
        write("set_number", entity="plato", property="born", n=-428)
        v_early = write("set_relation", entity="plato", property="student_of", target="socrates")
        print(f"  plato as first recorded (version {v_early}): {snapshot('plato')}")

        # A named save point survives restarts and is the stable way to say
        # "as it stood last week" without remembering a number.
        # Save points need a persisted store; the in-memory simulator CI runs
        # on refuses them, and the demo falls back to the version it noted.
        try:
            c.save_point(GLOBAL, "last-week")
            have_save = True
        except orly.OrlyError as e:
            print(f"  (no save point here: {e.reply.get('result')})")
            have_save = False
        t_before = datetime.datetime.now(datetime.timezone.utc)
        time.sleep(1.1)

        print("\n=== 2. a week later: edits, a correction and a delete ===")
        write("set_text", entity="plato", property="name", text="Plato of Athens")
        write("set_number", entity="plato", property="born", n=-427)
        write("clear_prop", entity="plato", property="student_of")
        v_late = write("set_relation", entity="plato", property="founded", target="academy")

        now = snapshot("plato")
        print(f"  now: {now}")
        check("the current snapshot shows the edits",
              now == {"name": "Plato of Athens", "born": "-427", "founded": "-> academy"}, now)

        print("\n=== 3. what did this say last week? ===")
        by_save = snapshot("plato", at={"save": "last-week"}) if have_save else snapshot("plato", at=v_early)
        by_version = snapshot("plato", at=v_early)
        # A time resolves through the (time, version) samples the retention
        # window keeps at memory merges, so a moment before the first sample
        # is a typed error, not a guess.
        try:
            by_time = snapshot("plato", at={"time": t_before.isoformat()})
        except orly.VersionUnavailable as e:
            print(f"  (no sample that old yet: {e.reply.get('result')})")
            by_time = None
        print(f"  last week: {by_save}")
        expect = {"name": "Plato", "born": "-428", "student_of": "-> socrates"}
        check("as of the save point", by_save == expect, by_save)
        check("as of a version number", by_version == expect, by_version)
        if by_time is not None:
            check("as of a wall-clock time", by_time == expect, by_time)
        check("the deleted property is still there in the past",
              "student_of" in by_save and "student_of" not in now)

        print("\n=== 4. what changed since? ===")
        diff = changed(by_save, now)
        for k, (old, new) in diff.items():
            print(f"  {k}: {old!r} -> {new!r}")
        check("the diff names every change",
              diff == {"name": ("Plato", "Plato of Athens"), "born": ("-428", "-427"),
                       "student_of": ("-> socrates", None), "founded": (None, "-> academy")}, diff)

        print("\n=== 5. the version a reply was computed at ===")
        got = c.call(GLOBAL, PKG, "display", {"entity": "plato", "property": "name"}, at=v_early)
        check("the reply reports the version it read", c.last_version == v_early, c.last_version)
        check("a read as of v_early sees the old name", got == "Plato", got)
        newest = c.version_of(GLOBAL)
        check("the newest version is no older than the last write", newest >= v_late, newest)

        print("\n=== 6. a version that is gone is a typed error ===")
        try:
            c.call(GLOBAL, PKG, "display", {"entity": "plato", "property": "name"},
                   at=newest + 1000)
            check("a future version is refused", False)
        except orly.VersionUnavailable as e:
            check("a future version raises VersionUnavailable", e.requested == newest + 1000, e)

        if have_save:
            c.drop_save_point(GLOBAL, "last-week")
        c.exit()

    print()
    if failures:
        print(f"FAILED: {len(failures)} check(s): {failures}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
