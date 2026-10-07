"""Batch-promotion smoke, Python driver (#751); run by run-batch-promotion.sh.

Batched calls (call_batch and call_many, sizes 1, 2 and 8) to methods with a conditional effect,
a conditional value, an assert-only read and a plain write, on all four POV flavours. Every write
is checked after promotion, through a fresh POV that reads the global POV, and the POV must still
take writes afterwards. Before #751 was fixed, any batched call with an `if` replied ok, failed
Tetris's assertion replay, and was never promoted; the POV was then failed.
"""
import os
import sys
import time

import orly

PKG = "batch_promotion"
TAG = os.environ.get("BATCH_TAG", "py")
PROMOTION_TIMEOUT_S = float(os.environ.get("PROMOTION_TIMEOUT_S", "60"))
SIZES = (1, 2, 8)
FLAVOURS = [(True, True), (True, False), (False, True), (False, False)]
GETTER = {"put_cond": "get_k", "put_ci": "get_c", "guard": "get_g", "put_plain": "get_p"}


def fail(msg):
    print(f"BATCH PROMOTION FAIL ({TAG}): {msg}", flush=True)
    sys.exit(1)


def args_for(method, key, n):
    """The args for one call writing `key`, and the value its getter reads back."""
    if method == "put_cond":
        return {"k": key, "v": f"v{n}"}, f"v{n}"
    if method == "put_ci":
        return {"k": key, "x": n}, 1 if n > 5 else 0
    if method == "guard":
        return {"k": key}, True
    return {"k": key, "v": f"p{n}"}, f"p{n}"


def read_global(reader, expected):
    """Read every expected key through a fresh POV off global, in one read-only batch."""
    pov = reader.new_pov(safe=False, shared=False)
    items = sorted(expected.items())
    got = reader.call_many(pov, [(PKG, getter, {"k": key}) for (getter, key), _ in items])
    return {k: g for (k, _), g in zip(items, got)}


def wait_promoted(reader, expected, what):
    deadline = time.monotonic() + PROMOTION_TIMEOUT_S
    while True:
        got = read_global(reader, expected)
        missing = {k: v for k, v in expected.items() if got[k] != v}
        if not missing:
            print(f"{TAG}: {what}: {len(expected)} writes promoted", flush=True)
            return
        if time.monotonic() > deadline:
            sample = sorted(missing.items())[:5]
            fail(f"{what}: {len(missing)} of {len(expected)} acknowledged writes never reached global "
                 f"after {PROMOTION_TIMEOUT_S:.0f}s, e.g. {[(k, v, got[k]) for k, v in sample]}")
        time.sleep(0.5)


def get_t(reader):
    return reader.call(reader.new_pov(safe=False, shared=False), PKG, "get_t")


def main():
    c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=120)
    c.new_session()
    c.install(PKG, 1)
    reader = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=120)
    reader.new_session()
    for fi, (safe, shared) in enumerate(FLAVOURS):
        flav = f"{'safe' if safe else 'fast'}-{'shared' if shared else 'private'}"
        pov = c.new_pov(safe=safe, shared=shared)
        expected = {}
        n = 0
        # Same-method batches, one per method and size.
        for size in SIZES:
            for method in GETTER:
                calls = []
                for i in range(size):
                    n += 1
                    key = f"{TAG}-{flav}-{method}-{size}-{i}"
                    args, want = args_for(method, key, n)
                    calls.append(args)
                    expected[(GETTER[method], key)] = want
                try:
                    results = c.call_batch(pov, PKG, method, calls)
                except orly.OrlyError as ex:
                    fail(f"{flav}: call_batch {method} x{size} refused: {ex}")
                if len(results) != size:
                    fail(f"{flav}: call_batch {method} x{size} returned {results}")
        # Mixed batches cycling through the methods.
        for size in SIZES:
            calls = []
            for i in range(size):
                n += 1
                method = list(GETTER)[i % len(GETTER)]
                key = f"{TAG}-{flav}-mixed-{size}-{i}"
                args, want = args_for(method, key, n)
                calls.append((PKG, method, args))
                expected[(GETTER[method], key)] = want
            try:
                results = c.call_many(pov, calls)
            except orly.OrlyError as ex:
                fail(f"{flav}: call_many x{size} refused: {ex}")
            if len(results) != size:
                fail(f"{flav}: call_many x{size} returned {results}")
        wait_promoted(reader, expected, f"{flav} batches")

        # The conditional effect's other branch: the keys exist now, so put_cond updates them.
        update = {k: v for k, v in expected.items() if k[0] == "get_k"}
        keys = sorted(key for _, key in update)
        for start in range(0, len(keys), 8):
            chunk = keys[start:start + 8]
            c.call_batch(pov, PKG, "put_cond", [{"k": key, "v": f"u-{key}"} for key in chunk])
            for key in chunk:
                update[("get_k", key)] = f"u-{key}"
        wait_promoted(reader, update, f"{flav} put_cond updates")

        # A no-argument conditional call in a mixed batch (one flavour only: `t` is one key).
        if fi == 0:
            before = get_t(reader)
            key = f"{TAG}-{flav}-tick"
            c.call_many(pov, [(PKG, "tick", {}), (PKG, "put_plain", {"k": key, "v": "t"})])
            want_t = (before + 2) if before is not None else 1
            wait_promoted(reader, {("get_p", key): "t"}, f"{flav} tick batch")
            if get_t(reader) != want_t:
                fail(f"{flav}: tick: t is {get_t(reader)}, expected {want_t}")

        # The POV still takes writes, batched and not, and they promote.
        tail = {}
        for i, batched in enumerate((False, True)):
            key = f"{TAG}-{flav}-after-{i}"
            try:
                if batched:
                    c.call_batch(pov, PKG, "put_cond", [{"k": key, "v": "after"}])
                else:
                    c.call(pov, PKG, "put_cond", {"k": key, "v": "after"})
            except orly.OrlyError as ex:
                fail(f"{flav}: the POV refused a write after its batches promoted: {ex}")
            tail[("get_k", key)] = "after"
        wait_promoted(reader, tail, f"{flav} later writes")
    print(f"{TAG}: batch promotion OK on all four POV flavours", flush=True)
    c.close()
    reader.close()


main()
