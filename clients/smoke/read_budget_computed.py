"""Read-budget smoke, computed values (#729); run by run-read-budget.sh.

A read that computes a big value from few or no rows used to pass the per-read budget (#694)
until its result reached the arena: `[0..30000000] as [int]` was refused only after orlyi had
grown by about 1 GB, and a loop with no rows and no memory was never refused at all. The budget
is now charged while the call runs. Against an orlyi with --read_budget_mb=$BUDGET_MB, each call
below must be refused as read_too_large within MAX_SECONDS, and orlyi's peak RSS during the call
must stay within MAX_GROWTH_MB of where it started. Calls of the same shape under the budget
must still answer.

Peak RSS is VmHWM, reset before each call through /proc/<pid>/clear_refs."""
import os
import time

import orly

PID = int(os.environ["ORLYI_PID"])
BUDGET_MB = int(os.environ["BUDGET_MB"])
MAX_SECONDS = float(os.environ.get("MAX_SECONDS", "5"))
MAX_GROWTH_MB = int(os.environ.get("MAX_GROWTH_MB", str(8 * BUDGET_MB)))


failures = []


def fail(msg):
    """Note a failure and go on to the next case, so a run shows every case that fails."""
    print(f"READ BUDGET FAIL (computed): {msg}", flush=True)
    failures.append(msg)


def rss_kb():
    """(peak, current) resident set of orlyi, in KiB."""
    vals = {}
    with open(f"/proc/{PID}/status") as f:
        for line in f:
            key, _, rest = line.partition(":")
            if key in ("VmHWM", "VmRSS"):
                vals[key] = int(rest.split()[0])
    return vals["VmHWM"], vals["VmRSS"]


def reset_peak():
    with open(f"/proc/{PID}/clear_refs", "w") as f:
        f.write("5")


c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=600)
c.new_session()
c.install("read_budget", 1)
pov = c.new_pov()

# Each of these is far over a BUDGET_MB budget: the lists would take 80 MB, the strings 640 MB
# and 1 GiB, and the loops 4 billion steps (about 20 s each before #729). Each takes a different
# path to its refusal, named by the limit its message must give: memory or steps, known up front
# (big_list, spin) or found while it runs (the rest).
MEMORY = "bytes of values while it ran (--read_budget_mb)"
STEPS = "steps (--read_budget_steps)"
OVER = [
    ("big_list", 10_000_000, MEMORY),
    ("big_filtered", 10_000_000, MEMORY),
    ("long_string", 10_000_000, MEMORY),
    ("doubling", 30, MEMORY),
    ("spin", 4_000_000_000, STEPS),
    ("spin_filtered", 4_000_000_000, STEPS),
]
failed_cases = set()
for method, n, limit in OVER:
    before = len(failures)
    reset_peak()
    _, start_kb = rss_kb()
    t = time.monotonic()
    try:
        got = c.call(pov, "read_budget", method, {"n": n})
        msg = f"answered ({str(got)[:40]})"
    except orly.ReadTooLarge as err:
        msg = err.reply["result"]
    except orly.OrlyError as err:
        msg = f"{type(err).__name__}: {err.reply}"
    secs = time.monotonic() - t
    peak_kb, _ = rss_kb()
    growth_mb = (peak_kb - start_kb) / 1024
    print(f"computed: {method}({n}) after {secs:.2f} s, peak RSS +{growth_mb:.0f} MB: {msg[:90]}", flush=True)
    if not msg.startswith("read too large") or limit not in msg:
        fail(f"{method}: expected a refusal naming {limit!r}, got {msg!r}")
    if secs > MAX_SECONDS:
        fail(f"{method}({n}) took {secs:.2f} s to refuse (limit {MAX_SECONDS} s)")
    if growth_mb > MAX_GROWTH_MB:
        fail(f"{method}({n}) grew orlyi by {growth_mb:.0f} MB before it was refused (limit {MAX_GROWTH_MB} MB)")
    if len(failures) > before:
        failed_cases.add(method)

if failures:
    raise SystemExit(f"READ BUDGET FAIL (computed): {len(failed_cases)} of {len(OVER)} cases")

# The same shapes under the budget answer, and correctly.
assert len(c.call(pov, "read_budget", "big_list", {"n": 1000})) == 1001
assert len(c.call(pov, "read_budget", "big_filtered", {"n": 1000})) == 1001
assert len(c.call(pov, "read_budget", "long_string", {"n": 1000})) == 64_000
assert len(c.call(pov, "read_budget", "doubling", {"n": 10})) == 1024
assert c.call(pov, "read_budget", "spin", {"n": 999_999}) == 1_000_000
assert c.call(pov, "read_budget", "spin_filtered", {"n": 999_999}) == []
print(f"READ BUDGET OK (computed, budget {BUDGET_MB} MiB)")
