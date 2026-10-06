"""Client-side checks for the shared-secret smoke (#710), run by run-auth.sh.

    auth_check.py required <port> <ws_port> <token>
        orlyi was started with <token>. Over WebSocket and the binary protocol, a
        connection with no token or a wrong one must be refused before any
        statement runs, with the typed status, and the right token must work.

    auth_check.py open <port> <ws_port> <token>
        orlyi was started with no token. Everything must work as before #710, with
        or without a client token, and the binary protocol must hang up on an auth
        request exactly as on any request kind it doesn't know.

Prints one line per check and exits 1 if any failed. The token itself is never
printed.
"""

import socket
import struct
import sys

import orly

MODE, PORT, WS_PORT, TOKEN = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
WRONG = "0123456789abcdef-not-the-token"
URL = f"ws://127.0.0.1:{WS_PORT}/"
failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


# -- binary protocol (orly/protocol.h) --------------------------------------

MAGIC, VERSION, TTL = 0x53544947, 0x0100, 60
NIL = b"\0" * 16


def header(kind):
    return struct.pack("<IHIc", MAGIC, VERSION, TTL, kind)


def binary():
    return socket.create_connection(("127.0.0.1", PORT), timeout=10)


def read_exactly(sock, n):
    """n bytes, or fewer if the server hangs up (or resets) first."""
    data = b""
    while len(data) < n:
        try:
            chunk = sock.recv(n - len(data))
        except ConnectionResetError:
            break
        if not chunk:
            break
        data += chunk
    return data


def hung_up(sock):
    try:
        return sock.recv(1) == b""
    except ConnectionResetError:
        return True
    except socket.timeout:
        return False


def binary_auth(sock, token):
    raw = token.encode()
    sock.sendall(header(b"A") + struct.pack(">H", len(raw)) + raw)
    return read_exactly(sock, 1)


def binary_new_session(sock):
    # THandshake<TNewSession> is 12 bytes: the header and the (empty) request's one byte.
    sock.sendall(header(b"N") + b"\0")
    return read_exactly(sock, 16)


# -- WebSocket ---------------------------------------------------------------

def ws_connect(token):
    # retries=0: a refusal must come back as itself, not as a retried dial.
    return orly.connect(URL, timeout=10, recv_timeout=30, retries=0, token=token)


def required():
    # WebSocket, no token: the first statement is refused, typed, and nothing runs.
    try:
        c = ws_connect(None)
        try:
            c.send("new session;")
            check(False, "ws: a connection with no token was accepted")
        except orly.Unauthorized as err:
            check(err.reply.get("status") == "unauthorized", "ws: no token -> unauthorized")
    except orly.Unauthorized:
        check(False, "ws: connect() raised before any token was sent")

    # WebSocket, wrong token.
    try:
        ws_connect(WRONG).send("new session;")
        check(False, "ws: a wrong token was accepted")
    except orly.Unauthorized as err:
        check(err.reply.get("status") == "unauthorized", "ws: wrong token -> unauthorized")
        check(TOKEN not in str(err) and WRONG not in str(err), "ws: the refusal does not echo a token")

    # WebSocket, right token.
    try:
        c = ws_connect(TOKEN)
        check(bool(c.new_session()), "ws: right token -> new session")
        check(c.send("echo 'hello';") == "hello", "ws: right token -> statements run")
        c.close()
    except orly.OrlyError as err:
        check(False, f"ws: the right token was refused: {err.reply.get('status')}")

    # Binary, no token: a new-session request gets the nil id, then a hang-up.
    with binary() as s:
        check(binary_new_session(s) == NIL, "binary: no token -> new session refused (nil id)")
        check(hung_up(s), "binary: no token -> server hangs up")

    # Binary, no token, old session: Unauthorized ('U').
    with binary() as s:
        s.sendall(header(b"O") + b"\x11" * 16)
        check(read_exactly(s, 1) == b"U", "binary: no token -> old session refused ('U')")

    # Binary, wrong token: Refused ('R'), then a hang-up.
    with binary() as s:
        check(binary_auth(s, WRONG) == b"R", "binary: wrong token -> refused ('R')")
        check(hung_up(s), "binary: wrong token -> server hangs up")

    # Binary, right token: Accepted ('A'), then a real session.
    with binary() as s:
        check(binary_auth(s, TOKEN) == b"A", "binary: right token -> accepted ('A')")
        sid = binary_new_session(s)
        check(len(sid) == 16 and sid != NIL, "binary: right token -> new session")

    # A WebSocket that never authenticates is closed after the server's 10 s deadline.
    import time
    import websocket
    ws = websocket.create_connection(URL, timeout=30)
    started = time.monotonic()
    try:
        ws.recv()
        closed = False
    except (websocket.WebSocketConnectionClosedException, ConnectionResetError, OSError):
        closed = True
    waited = time.monotonic() - started
    check(closed and 5 < waited < 25, f"ws: an idle unauthenticated connection is closed ({waited:.0f} s)")
    ws.close()

    # Health checks need no token.
    with binary() as s:
        s.sendall(header(b"H") + b"\0")
        check(read_exactly(s, 1) == b"R", "binary: health check needs no token")


def open_():
    c = ws_connect(None)
    check(bool(c.new_session()), "ws: no token configured -> works without one")
    c.close()
    c = ws_connect(TOKEN)
    check(bool(c.new_session()), "ws: no token configured -> a client with a token still works")
    c.close()
    with binary() as s:
        sid = binary_new_session(s)
        check(len(sid) == 16 and sid != NIL, "binary: no token configured -> new session")
    with binary() as s:
        s.sendall(header(b"A"))
        check(hung_up(s), "binary: no token configured -> an auth request is a bad request kind (hang-up)")


if MODE == "required":
    required()
elif MODE == "open":
    open_()
else:
    sys.exit(f"unknown mode {MODE}")
if failures:
    print(f"AUTH CHECK FAILED: {len(failures)} check(s)")
    sys.exit(1)
print("AUTH CHECK OK")
