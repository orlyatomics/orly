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

import websocket  # the `websocket-client` package

__all__ = ["DEFAULT_URL", "DEFAULT_TIMEOUT_S", "OrlyError", "Lit", "lit",
           "Client", "connect"]

DEFAULT_URL = "ws://127.0.0.1:8082/"
DEFAULT_TIMEOUT_S = 30


class OrlyError(RuntimeError):
    """Raised when the server replies with a non-``ok`` status."""

    def __init__(self, statement, reply):
        self.statement = statement
        self.reply = reply
        super().__init__(f"{statement!r}\n  -> {reply}")


class Lit:
    """Wrap a string to inject it into a statement as raw orlyscript.

    Use when a value is already an orlyscript expression that should not be
    re-encoded -- e.g. ``Lit("now()")`` or a pre-built literal.
    """

    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = str(raw)


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
    """
    if isinstance(value, Lit):
        return value.raw
    # bool before int: bool is a subclass of int in Python.
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, str):
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
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
        if reply.get("status") != "ok":
            raise OrlyError(statement, reply)
        return reply.get("result")

    # -- session / package lifecycle ------------------------------------
    def new_session(self):
        self.session_id = self.send("new session;")
        return self.session_id

    def resume_session(self, session_id):
        self.session_id = self.send(f"resume session {lit(session_id)};")
        return self.session_id

    def install(self, package, version):
        self.send(f"install {package}.{int(version)};")

    def uninstall(self, package, version):
        self.send(f"uninstall {package}.{int(version)};")

    def new_pov(self, safe=True, shared=True, parent=None):
        """Create a POV; returns its id. Defaults to ``new safe shared pov;``."""
        parts = ["new"]
        if safe:
            parts.append("safe")
        parts.append("shared" if shared else "private")
        parts.append("pov")
        if parent is not None:
            parts.append(f"parent {lit(parent)}")
        return self.send(" ".join(parts) + ";")

    # -- methods --------------------------------------------------------
    def call(self, pov, package, method, args=None):
        """Call ``package method`` on ``pov`` with a record of ``args``.

        Builds ``try {<pov>} <package> <method> <{.k: v, ...}>;``. ``args`` is a
        dict (or None for no args); values are encoded via :func:`lit`.
        """
        return self.send(f"try {{{pov}}} {package} {method} {lit(args or {})};")

    def pause(self, pov):
        return self.send(f"pause pov {lit(pov)};")

    def unpause(self, pov):
        return self.send(f"unpause pov {lit(pov)};")

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


def connect(url=DEFAULT_URL, timeout=DEFAULT_TIMEOUT_S):
    """Open a WebSocket to a running ``orlyi`` and return a :class:`Client`."""
    return Client(websocket.create_connection(url, timeout=timeout))
