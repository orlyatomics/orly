"""Desc-key prefix reads before and after the keys reach disk (#792); run by run-desc-prefix.sh.

Writes three families of keys whose patterns have a member with no quick comparison (a desc, a
nested tuple holding a desc) before their free member, with neighbouring values on both sides of
each pattern. Reads every pattern from a fresh POV right after the writes (from memory, or from
disk if the global repo has already merged), waits for the reporting port to show the global repo
as disk files only, and reads them all again. Every read must return exactly the keys written
under its prefix. Before #792's fix a release orlyi returned [] once the keys were on disk, and a
debug orlyi aborted on the disk walker's cross-check."""

import os
import re
import socket
import sys
import time

import orly

URL = os.environ["ORLY_URL"]
REPORT_PORT = int(os.environ["ORLY_REPORT_PORT"])
DISK_WAIT_S = int(os.environ.get("DISK_WAIT_S", "180"))

GS = (2, 3, 4)
ES = (1, 2, 3, 4)
STRS = ("a", "a string well past direct storage in a core")


def report():
    """The reporter keeps the connection open, so read until the layers line is complete."""
    buf = b""
    with socket.create_connection(("127.0.0.1", REPORT_PORT), timeout=5) as sock:
        sock.sendall(b"GET / HTTP/1.0\r\n\r\n")
        try:
            while not re.search(rb"^Global Layers = [^\n]*\n", buf, re.M):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        except TimeoutError:
            pass
    return buf.decode(errors="replace")


def layers():
    for line in report().splitlines():
        if line.startswith("Global Layers = disk "):
            disk, mem = line[len("Global Layers = disk "):].split("; memory ")
            return int(disk), int(mem)
    return None


c = orly.connect(URL, timeout=10, recv_timeout=60)
c.new_session()
c.install("desc_prefix", 1)

writer = c.new_pov()
calls = []
for g in GS:
    for e in ES:
        calls.append(("desc_prefix", "put_d", {"g": g, "e": e}))
        calls.append(("desc_prefix", "put_t", {"g": g, "e": e}))
        for s in STRS:
            calls.append(("desc_prefix", "put_m", {"s": s, "g": g, "e": e}))
for package, method, args in calls:
    c.call(writer, package, method, args)
# The last call writes put_m; make the last point_d write the very last one too.
c.call(writer, "desc_prefix", "put_d", {"g": GS[-1], "e": ES[-1]})
print(f"wrote {len(calls)} keys")

failures = []


def check(phase):
    pov = c.new_pov()
    n = 0
    for g in GS:
        reads = [(f"read_d g={g}", c.call(pov, "desc_prefix", "read_d", {"g": g})),
                 (f"read_t g={g}", c.call(pov, "desc_prefix", "read_t", {"g": g}))]
        for s in STRS:
            reads.append((f"read_m s={s[:8]!r} g={g}", c.call(pov, "desc_prefix", "read_m", {"s": s, "g": g})))
        for label, got in reads:
            n += 1
            tails = sorted(k[-1] for k in got)
            if tails != list(ES):
                failures.append(f"{phase}: {label} returned {got!r}, want keys ending in {list(ES)}")
        for e in ES:
            n += 1
            got = c.call(pov, "desc_prefix", "point_d", {"g": g, "e": e})
            if got != e:
                failures.append(f"{phase}: point_d g={g} e={e} read {got!r}, want {e}")
    print(f"{phase}: {n} reads checked")


# The writer's POV promotes to global asynchronously; wait until a fresh POV sees the last write.
last_g, last_e = GS[-1], ES[-1]
for _ in range(60):
    if c.call(c.new_pov(), "desc_prefix", "point_d", {"g": last_g, "e": last_e}) == last_e:
        break
    time.sleep(0.5)
else:
    sys.exit("DESC PREFIX FAIL: the writes never reached the global repo")

print(f"global layers (disk, memory) before the first read: {layers()}")
check("right after the writes")

deadline = time.time() + DISK_WAIT_S
seen = None
while time.time() < deadline:
    seen = layers()
    if seen is not None and seen[0] >= 1 and seen[1] == 0:
        break
    time.sleep(1)
else:
    sys.exit(f"DESC PREFIX FAIL: global repo never settled on disk only in {DISK_WAIT_S}s (last: {seen})")
print(f"global repo on disk: disk {seen[0]}; memory {seen[1]}")

check("disk files only")

c.close()
if failures:
    print("DESC PREFIX FAIL:\n  " + "\n  ".join(failures[:20]), file=sys.stderr)
    sys.exit(1)
print("desc prefix smoke: ok")
