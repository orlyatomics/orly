"""Orly client for the WebSocket + JSON protocol.

A thin client for talking to a running ``orlyi`` over WebSocket. It owns the
connection + session lifecycle, builds orlyscript statement strings safely
(escaping, argument literals, POV threading), and hands back the parsed JSON
result. See ``docs/PROTOCOL.md`` in the Orly repo for the protocol itself.

Typical use::

    import orly

    with orly.connect() as c:           # opens a WebSocket
        c.new_session()
        c.install("mypkg", 0)
        pov = c.new_pov()               # "new safe shared pov;"
        c.call(pov, "mypkg", "put", {"k": 1, "s": "hi"})
        print(c.call(pov, "mypkg", "get", {"k": 1}))

Note the JSON marshaling quirks the engine returns (``docs/PROTOCOL.md``):
integers come back as floats (``1`` -> ``1.0``), sets as unordered arrays, and
variants as ``{"Tag": <payload>}``. This client returns the parsed value as-is;
callers compare numerically / as sets accordingly.
"""

import json as _json
import os as _os
import time as _time

import websocket  # the `websocket-client` package

__all__ = ["DEFAULT_URL", "DEFAULT_TIMEOUT_S", "DEFAULT_RECV_TIMEOUT_S",
           "DEFAULT_RETRIES", "DEFAULT_BACKOFF_S", "OrlyError", "InsufficientStorage",
           "InsufficientMemory", "WriteTooLarge", "ReadTooLarge", "RemoteCompileDisabled",
           "Unauthorized", "Lit", "lit", "Addr",
           "Client", "connect"]

DEFAULT_URL = "ws://127.0.0.1:8082/"
# Connect-handshake timeout. Kept short so a not-yet-ready orlyi fails fast and
# the retry below covers the startup window.
DEFAULT_TIMEOUT_S = 30
# Per-call recv() timeout, set on the socket AFTER connecting. Decoupled from
# the connect timeout: a method reply should arrive in milliseconds, so a recv
# that takes this long means a severely starved server (e.g. a CI runner under
# the load of every demo at once), not a hung one -- giving it generous headroom
# stops a transient latency spike from killing an otherwise-fine call (issue
# #224). A genuinely hung orlyi still surfaces, just later; the CI job timeout is
# the backstop. Retrying the call instead is unsafe -- a resent write could
# double-apply -- so we wait rather than retry.
DEFAULT_RECV_TIMEOUT_S = 120
# A freshly started or heavily loaded orlyi can briefly refuse the connection
# or time out the WebSocket handshake before it is ready. connect() retries
# that window with exponential backoff: DEFAULT_BACKOFF_S, doubling each time.
DEFAULT_RETRIES = 5
DEFAULT_BACKOFF_S = 0.25


class OrlyError(RuntimeError):
    """Raised when the server replies with a non-``ok`` status."""

    def __init__(self, statement, reply):
        self.statement = statement
        self.reply = reply
        super().__init__(f"{statement!r}\n  -> {reply}")


class InsufficientStorage(OrlyError):
    """Raised when the server refuses a write because it is low on disk space
    (``"status": "insufficient_storage"``). Nothing was written, reads still
    work, and the write can be retried once space is freed."""


class InsufficientMemory(OrlyError):
    """Raised when the server refuses a write because its update pools are
    down to the reserve kept for merges (``"status": "insufficient_memory"``).
    Nothing was written, reads still work, and the write can be retried;
    writes are accepted again once the merges have freed the pools, usually
    within seconds."""


class WriteTooLarge(OrlyError):
    """Raised when a single write holds more entries than half the server's
    Update Entry pool's merge reserve (``"status": "write_too_large"``), so it
    could never be promoted. Nothing was written. Unlike ``InsufficientMemory``
    it is NOT retryable: split the batch into smaller ones."""


class ReadTooLarge(OrlyError):
    """Raised when a call walks more rows, or builds more result memory, than
    the server's per-read budget (``"status": "read_too_large"``;
    ``--read_budget_rows``, ``--read_budget_mb``). Like ``WriteTooLarge`` it is
    NOT retryable as sent: read a narrower range."""


class RemoteCompileDisabled(OrlyError):
    """Raised when a ``compile`` statement reaches a server started without
    ``--allow_remote_compile`` (``"status": "remote_compile_disabled"``,
    #705). Compile packages with ``orlyc`` and install them instead."""


class Unauthorized(OrlyError):
    """Raised by :func:`connect` when the server requires a token and this
    client presented none, or the wrong one (``"status": "unauthorized"``,
    #710). The server has closed the connection; no statement ran. Not
    retryable as sent: fix the token."""


class Lit:
    """Wrap a string to inject it into a statement as raw orlyscript.

    Use when a value is already an orlyscript expression that should not be
    re-encoded -- e.g. ``Lit("now()")`` or a pre-built literal.
    """

    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = str(raw)


class Addr:
    """Wrap a sequence to encode it as an orlyscript address (key) literal
    ``<[a, b, ...]>``: ``Addr(["edge", 1])`` -> ``<["edge", 1]>``."""

    __slots__ = ("items",)

    def __init__(self, items):
        self.items = list(items)


def _key_lit(key):
    """A key for the POV review statements: a Lit or Addr as is, a list or tuple as an address."""
    if isinstance(key, (Lit, Addr)):
        return lit(key)
    return lit(Addr(key))


_SHORT_ESCAPES = {"\n": "\\n", "\r": "\\r", "\t": "\\t"}


def _quote(s):
    """A string as an orlyscript literal. The lexer refuses raw control characters, so those
    are written as \\n, \\r, \\t or \\xNN."""
    out = []
    for ch in s:
        if ch == "\\" or ch == '"':
            out.append("\\" + ch)
        elif ch in _SHORT_ESCAPES:
            out.append(_SHORT_ESCAPES[ch])
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def lit(value):
    """Encode a Python value as an orlyscript literal.

    - ``Lit`` -> its raw text, verbatim
    - ``bool`` -> ``true`` / ``false``
    - ``int`` -> decimal
    - ``float`` -> ``repr``
    - ``str`` -> a quoted, escaped string literal
    - ``dict`` -> a record ``<{.k: v, ...}>`` (empty: ``<{}>``)
    - ``list`` / ``tuple`` -> ``[a, b, ...]``
    - ``set`` / ``frozenset`` -> ``{a, b, ...}``
    - ``Addr`` -> ``<[a, b, ...]>``
    """
    if isinstance(value, Lit):
        return value.raw
    if isinstance(value, Addr):
        return "<[" + ", ".join(lit(v) for v in value.items) + "]>"
    # bool before int: bool is a subclass of int in Python.
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, dict):
        return "<{" + ", ".join(f".{k}: {lit(v)}" for k, v in value.items()) + "}>"
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(lit(v) for v in value) + "]"
    if isinstance(value, (set, frozenset)):
        return "{" + ", ".join(lit(v) for v in value) + "}"
    raise TypeError(f"cannot encode {type(value).__name__} as an orlyscript literal: {value!r}")


class Client:
    """A connection to a running ``orlyi`` (one WebSocket, one session)."""

    def __init__(self, ws):
        self.ws = ws
        self.session_id = None

    # -- core ------------------------------------------------------------
    def send(self, statement):
        """Send one statement; return its ``result``, or raise ``OrlyError``."""
        self.ws.send(statement)
        reply = _json.loads(self.ws.recv())
        status = reply.get("status")
        if status == "insufficient_storage":
            raise InsufficientStorage(statement, reply)
        if status == "insufficient_memory":
            raise InsufficientMemory(statement, reply)
        if status == "write_too_large":
            raise WriteTooLarge(statement, reply)
        if status == "read_too_large":
            raise ReadTooLarge(statement, reply)
        if status == "remote_compile_disabled":
            raise RemoteCompileDisabled(statement, reply)
        if status == "unauthorized":
            raise Unauthorized(statement, reply)
        if status != "ok":
            raise OrlyError(statement, reply)
        return reply.get("result")

    def authenticate(self, token):
        """Present the server's shared secret (#710): the first message,
        ``{"auth": "<token>"}``. :func:`connect` calls this when it has a token;
        the token never appears in an error. A server with a token answers only
        ``ok`` or ``unauthorized``. One started without a token answers with an
        error status (it tries to parse the message as a statement) and the
        connection carries on unauthenticated, so clients can get the token
        before the server starts requiring it."""
        try:
            self._send_raw("<auth>", _json.dumps({"auth": token}))
        except Unauthorized:
            raise
        except OrlyError:
            pass

    def _send_raw(self, label, message):
        """Send ``message`` as-is, reporting errors against ``label``."""
        self.ws.send(message)
        reply = _json.loads(self.ws.recv())
        status = reply.get("status")
        if status == "unauthorized":
            raise Unauthorized(label, reply)
        if status != "ok":
            raise OrlyError(label, reply)
        return reply.get("result")

    # -- session / package lifecycle ------------------------------------
    def new_session(self):
        self.session_id = self.send("new session;")
        return self.session_id

    def resume_session(self, session_id):
        # The console grammar takes the session id as a braced IdExpr
        # (``resume session {uuid};``), not a quoted string.
        self.session_id = self.send(f"resume session {{{session_id}}};")
        return self.session_id

    def install(self, package, version):
        self.send(f"install {package}.{int(version)};")

    def uninstall(self, package, version):
        self.send(f"uninstall {package}.{int(version)};")

    def new_pov(self, safe=True, shared=True, parent=None, conflicts=None):
        """Create a POV; returns its id. Defaults to ``new safe shared pov;``.

        ``safe=False`` makes a ``fast`` POV; ``parent`` is a POV id, and the
        new POV is created ``from`` it. The grammar has no default guarantee,
        so one of ``safe``/``fast`` is always spelled out (#580).

        ``conflicts`` (``"report"`` or ``"refuse"``) tracks promotion conflicts
        from this fork on (#746): see :meth:`promote`.
        """
        parts = ["new", "safe" if safe else "fast", "shared" if shared else "private", "pov"]
        if parent is not None:
            parts.append(f"from {{{parent}}}")
        if conflicts not in (None, "none"):
            parts.append(lit({"conflicts": conflicts}))
        return self.send(" ".join(parts) + ";")

    # -- POV review (#746) ------------------------------------------------
    def diff(self, pov, start=None, stop=None, after=None, limit=None):
        """A page of what ``pov`` changed relative to its parent: its
        unpromoted writes, key by key, in key order.

        Returns ``{"changes": [...], "next": key or None, "next_literal": str
        or None, "updates": n}``. Each change has ``key``, ``kind`` (``added``,
        ``changed``, ``removed`` or ``delta``), ``before`` (the parent's value)
        and ``after`` (the POV's), and for a ``delta``, ``op`` and ``delta``.
        ``start`` (inclusive) and ``stop`` (exclusive) restrict the keys;
        ``after`` is the previous page's ``next``; ``limit`` is the page size
        (default 100). Keys are lists or tuples, sent as key literals.
        """
        options = []
        if start is not None:
            options.append(f".start: {_key_lit(start)}")
        if stop is not None:
            options.append(f".stop: {_key_lit(stop)}")
        if after is not None:
            options.append(f".after: {_key_lit(after)}")
        if limit is not None:
            options.append(f".limit: {int(limit)}")
        suffix = f" <{{{', '.join(options)}}}>" if options else ""
        return self.send(f"diff_pov {{{pov}}}{suffix};")

    def diff_pages(self, pov, start=None, stop=None, limit=None):
        """Every page of ``pov``'s diff, as lists of changes (keyset paging)."""
        page = self.diff(pov, start=start, stop=stop, limit=limit)
        while True:
            if page["changes"]:
                yield page["changes"]
            if page["next_literal"] is None:
                return
            page = self.diff(pov, start=start, stop=stop, after=Lit(page["next_literal"]), limit=limit)

    def discard(self, pov):
        """Throw away ``pov``'s unpromoted changes (a private POV of this
        session's), so it reads as its parent again. Returns
        ``{"discarded_updates": n, "discarded_entries": n}``."""
        return self.send(f"discard_pov {{{pov}}};")

    def request_promotion(self, pov, force=False):
        """Ask for ``pov``'s changes to be promoted (unpause it) and return at
        once. In refusing mode it is tested first and stays as it was if any
        change would conflict (``status`` ``"refused"``), unless ``force``."""
        suffix = " <{.force: true}>" if force else ""
        return self.send(f"promote_pov {{{pov}}}{suffix};")

    def review(self, pov, after=0):
        """``pov``'s promotion progress and the conflicts numbered after
        ``after``."""
        suffix = f" <{{.after: {int(after)}}}>" if after else ""
        return self.send(f"review_pov {{{pov}}}{suffix};")

    def promote(self, pov, force=False, timeout=30.0, poll=0.05):
        """Promote ``pov``'s changes and wait until nothing is pending, or the
        POV is blocked, failed or paused, or ``timeout`` seconds pass. Returns
        ``{"status": ..., "pending": n, "conflicts": [...], "blocked_on":
        [...]}`` with ``status`` one of ``promoted``, ``refused``, ``blocked``,
        ``failed``, ``paused`` or ``timeout``, and the conflicts found during
        this promotion."""
        started = self.request_promotion(pov, force=force)
        if started["status"] == "refused":
            return {"status": "refused", "pending": started["pending"],
                    "conflicts": started["conflicts"], "blocked_on": []}
        deadline = _time.monotonic() + timeout
        while True:
            review = self.review(pov, after=int(started["mark"]))
            if review["status"] == "failed":
                status = "failed"
            elif review["blocked"]:
                status = "blocked"
            elif review["pending"] == 0:
                status = "promoted"
            elif review["status"] == "paused":
                status = "paused"
            elif _time.monotonic() > deadline:
                status = "timeout"
            else:
                _time.sleep(poll)
                continue
            return {"status": status, "pending": review["pending"],
                    "conflicts": review["conflicts"], "blocked_on": review["blocked_on"]}

    # -- methods --------------------------------------------------------
    def call(self, pov, package, method, args=None):
        """Call ``package method`` on ``pov`` with a record of ``args``.

        Builds ``try {<pov>} <package> <method> <{.k: v, ...}>;``. ``args`` is a
        dict (or None for no args); values are encoded via :func:`lit`.
        """
        return self.send(f"try {{{pov}}} {package} {method} {lit(args or {})};")

    def call_batch(self, pov, package, method, args_list):
        """Call ``package method`` on ``pov`` once per record in ``args_list``,
        folding all N calls into a **single transaction** (#253).

        Builds ``try {<pov>} <package> <method> [<{...}>, <{...}>, ...];`` and
        returns a JSON **array** of the N per-call results, in order. This is a
        write-coalescing primitive for commutative fan-in / bulk load: every call
        runs against the same pre-batch snapshot (no read-your-writes within a
        batch) and the batch is all-or-nothing (one bad record rejects the set).
        """
        if not args_list:
            raise ValueError("call_batch requires at least one argument record")
        records = lit([dict(a or {}) for a in args_list])
        return self.send(f"try {{{pov}}} {package} {method} {records};")

    def call_many(self, pov, calls):
        """Run several different methods on ``pov`` as **one transaction** (#255).

        ``calls`` is a list of ``(package, method, args)`` tuples. Builds
        ``try {<pov>} [<package> <method> <{...}>, ...];`` and returns a list with
        one result per call, in order (they may differ in type). Like
        ``call_batch``, every call reads the same pre-batch snapshot (no
        read-your-writes within a batch) and the batch is all-or-nothing: if any
        call fails, none of the writes land.
        """
        if not calls:
            raise ValueError("call_many requires at least one call")
        parts = [f"{package} {method} {lit(dict(args or {}))}" for package, method, args in calls]
        return self.send(f"try {{{pov}}} [{', '.join(parts)}];")

    def pause(self, pov):
        return self.send(f"pause {{{pov}}};")

    def unpause(self, pov):
        return self.send(f"unpause {{{pov}}};")

    # -- teardown -------------------------------------------------------
    def exit(self):
        try:
            self.send("exit;")
        finally:
            self.close()

    def close(self):
        self.ws.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def _token_from_env():
    """``ORLY_AUTH_TOKEN``, or the contents of the file named by
    ``ORLY_AUTH_TOKEN_FILE`` less a trailing newline, or None."""
    path = _os.environ.get("ORLY_AUTH_TOKEN_FILE")
    if path:
        with open(path) as f:
            token = f.read()
        return token[:-1].rstrip("\r") if token.endswith("\n") else token
    return _os.environ.get("ORLY_AUTH_TOKEN") or None


def connect(url=DEFAULT_URL, timeout=DEFAULT_TIMEOUT_S,
            recv_timeout=DEFAULT_RECV_TIMEOUT_S,
            retries=DEFAULT_RETRIES, backoff=DEFAULT_BACKOFF_S, token=None):
    """Open a WebSocket to a running ``orlyi`` and return a :class:`Client`.

    A just-started or heavily loaded ``orlyi`` can refuse the connection or time
    out the WebSocket handshake briefly before it is ready to serve. To keep
    demos and tests from flaking on that window, the connection is retried up to
    ``retries`` times with exponential backoff (``backoff`` seconds, doubling
    each attempt). The last error is re-raised once the retries are exhausted;
    pass ``retries=0`` to fail fast on the first attempt.

    ``timeout`` bounds the connect handshake (kept short so the retry covers
    startup). ``recv_timeout`` bounds each subsequent ``recv()`` and is set
    generously: a method reply is normally milliseconds, so a long recv means a
    starved server, not a hung one, and we would rather wait it out than fail an
    otherwise-fine call (issue #224).

    ``token`` is the server's shared secret (#710), presented before anything
    else; it defaults to ``ORLY_AUTH_TOKEN`` or the file named by
    ``ORLY_AUTH_TOKEN_FILE``. A refused token raises :class:`Unauthorized`,
    which is not retried.
    """
    if token is None:
        token = _token_from_env()
    delay = backoff
    for attempt in range(retries + 1):
        try:
            ws = websocket.create_connection(url, timeout=timeout)
            # Decouple the per-call recv timeout from the connect timeout.
            ws.settimeout(recv_timeout)
            client = Client(ws)
            if token is not None:
                try:
                    client.authenticate(token)
                except BaseException:
                    client.close()
                    raise
            return client
        except (websocket.WebSocketException, OSError):
            if attempt == retries:
                raise
            _time.sleep(delay)
            delay *= 2
