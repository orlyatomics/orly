// Package orly is a client for the Orly database over its WebSocket + JSON
// protocol (see docs/PROTOCOL.md in the Orly repo). It owns the connection +
// session lifecycle, builds orlyscript statement strings safely (escaping,
// argument literals, POV threading), and returns the raw JSON result.
//
// It is distinct from the lower-level packed binary protocol in
// orly/protocol.h that the native C++ client (orly/client) speaks.
//
// Typical use:
//
//	c, err := orly.Connect()
//	if err != nil { log.Fatal(err) }
//	defer c.Close()
//	c.NewSession()
//	c.Install("mypkg", 0)
//	pov, _ := c.NewPov()
//	c.Call(pov, "mypkg", "put", map[string]any{"k": 1, "s": "hi"})
//	raw, _ := c.Call(pov, "mypkg", "get", map[string]any{"k": 1})
//
// Result marshaling quirks the engine returns (see docs/PROTOCOL.md): integers
// come back as JSON floats, sets as unordered arrays, variants as
// {"Tag": <payload>}. Call returns the raw JSON; callers decode accordingly.
package orly

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// DefaultURL is the WebSocket endpoint a local orlyi listens on.
const DefaultURL = "ws://127.0.0.1:8082/"

// A just-started or heavily loaded orlyi can briefly refuse the connection or
// time out the WebSocket handshake before it is ready. ConnectURL retries that
// window with exponential backoff: defaultBackoff, doubling each attempt.
const (
	DefaultRetries = 5
	defaultBackoff = 250 * time.Millisecond
)

// ErrInsufficientStorage is wrapped by the error a write gets when the server
// refuses it for lack of disk space ("status": "insufficient_storage"). Nothing
// was written, reads still work, and the write can be retried once space is
// freed. Test with errors.Is.
var ErrInsufficientStorage = errors.New("orly: insufficient storage")

// ErrInsufficientMemory is wrapped by the error a write gets when the server
// refuses it because its update pools are down to the reserve kept for merges
// ("status": "insufficient_memory"). Nothing was written, reads still work, and
// the write can be retried; writes are accepted again once the merges have
// freed the pools, usually within seconds. Test with errors.Is.
var ErrInsufficientMemory = errors.New("orly: insufficient memory")

// ErrWriteTooLarge is wrapped by the error a write gets when it holds more
// entries than half the server's Update Entry pool's merge reserve
// ("status": "write_too_large"), so it could never be promoted. Nothing was
// written. Unlike ErrInsufficientMemory it is NOT retryable: split the batch
// into smaller ones. Test with errors.Is.
var ErrWriteTooLarge = errors.New("orly: write too large")

// ErrReadTooLarge is wrapped by the error a call gets when it walks more rows,
// or builds more result memory, than the server's per-read budget
// ("status": "read_too_large"; --read_budget_rows, --read_budget_mb). Like
// ErrWriteTooLarge it is NOT retryable as sent: read a narrower range. Test with
// errors.Is.
var ErrReadTooLarge = errors.New("orly: read too large")

// ErrRemoteCompileDisabled is wrapped by the error a compile statement gets
// from a server started without --allow_remote_compile ("status":
// "remote_compile_disabled", #705). Compile packages with orlyc and install
// them instead. Test with errors.Is.
var ErrRemoteCompileDisabled = errors.New("orly: remote compile disabled")

// ErrUnauthorized is wrapped by the error ConnectURL returns when the server
// requires a token and this client presented none, or the wrong one
// ("status": "unauthorized", #710). The server has closed the connection; no
// statement ran. Not retryable as sent: fix the token. Test with errors.Is.
var ErrUnauthorized = errors.New("orly: unauthorized")

// Client is a connection to a running orlyi (one WebSocket, one session).
type Client struct {
	conn *websocket.Conn
}

type reply struct {
	Status string          `json:"status"`
	Result json.RawMessage `json:"result"`
}

// Connect dials a running orlyi at DefaultURL.
func Connect() (*Client, error) { return ConnectURL(DefaultURL) }

// ConnectURL dials a running orlyi at the given ws:// URL. It retries a
// failed dial up to DefaultRetries times with exponential backoff, so a
// just-started or loaded orlyi that is not yet accepting connections does not
// flake the caller; the last error is returned once the retries are exhausted.
//
// It presents the token in ORLY_AUTH_TOKEN, or in the file named by
// ORLY_AUTH_TOKEN_FILE, if either is set (#710); see ConnectURLWithToken.
func ConnectURL(url string) (*Client, error) {
	token, err := TokenFromEnv()
	if err != nil {
		return nil, err
	}
	return ConnectURLWithToken(url, token)
}

// TokenFromEnv returns ORLY_AUTH_TOKEN, or the contents of the file named by
// ORLY_AUTH_TOKEN_FILE less a trailing newline, or "" if neither is set.
func TokenFromEnv() (string, error) {
	if path := os.Getenv("ORLY_AUTH_TOKEN_FILE"); path != "" {
		b, err := os.ReadFile(path)
		if err != nil {
			return "", fmt.Errorf("orly: read ORLY_AUTH_TOKEN_FILE: %w", err)
		}
		return strings.TrimSuffix(strings.TrimSuffix(string(b), "\n"), "\r"), nil
	}
	return os.Getenv("ORLY_AUTH_TOKEN"), nil
}

// ConnectURLWithToken is ConnectURL with the server's shared secret (#710),
// presented as the first message, {"auth": "<token>"}, before anything else.
// An empty token presents none. A refused token returns an error wrapping
// ErrUnauthorized, which is not retried. A server with a token answers only
// "ok" or "unauthorized"; one started without a token answers with an error
// status (it tries to parse the message as a statement) and the connection
// carries on, so clients can get the token before the server starts requiring
// it.
func ConnectURLWithToken(url, token string) (*Client, error) {
	delay := defaultBackoff
	var err error
	for attempt := 0; ; attempt++ {
		var conn *websocket.Conn
		conn, _, err = websocket.DefaultDialer.Dial(url, nil)
		if err == nil {
			c := &Client{conn: conn}
			if token == "" {
				return c, nil
			}
			if err = c.authenticate(token); err == nil {
				return c, nil
			}
			c.Close()
			if errors.Is(err, ErrUnauthorized) {
				return nil, err
			}
		}
		if attempt >= DefaultRetries {
			return nil, fmt.Errorf("orly: dial %s (after %d attempts): %w", url, attempt+1, err)
		}
		time.Sleep(delay)
		delay *= 2
	}
}

// authenticate presents the token as the first message. The token never
// appears in an error.
func (c *Client) authenticate(token string) error {
	msg, err := json.Marshal(map[string]string{"auth": token})
	if err != nil {
		return err
	}
	if err := c.conn.WriteMessage(websocket.TextMessage, msg); err != nil {
		return fmt.Errorf("orly: write auth: %w", err)
	}
	_, raw, err := c.conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("orly: read after auth: %w", err)
	}
	var r reply
	if err := json.Unmarshal(raw, &r); err != nil {
		return fmt.Errorf("orly: parse reply to auth: %w", err)
	}
	if r.Status == "unauthorized" {
		return fmt.Errorf("orly: auth -> %s: %w", raw, ErrUnauthorized)
	}
	return nil
}

// Close closes the underlying WebSocket.
func (c *Client) Close() error { return c.conn.Close() }

// Send sends one orlyscript statement and returns its result, or an error if
// the reply status is not "ok".
func (c *Client) Send(stmt string) (json.RawMessage, error) {
	if err := c.conn.WriteMessage(websocket.TextMessage, []byte(stmt)); err != nil {
		return nil, fmt.Errorf("orly: write %q: %w", stmt, err)
	}
	_, msg, err := c.conn.ReadMessage()
	if err != nil {
		return nil, fmt.Errorf("orly: read after %q: %w", stmt, err)
	}
	var r reply
	if err := json.Unmarshal(msg, &r); err != nil {
		return nil, fmt.Errorf("orly: parse reply to %q: %w (raw: %s)", stmt, err, msg)
	}
	if r.Status == "insufficient_storage" {
		return nil, fmt.Errorf("orly: %s -> %s: %w", stmt, msg, ErrInsufficientStorage)
	}
	if r.Status == "insufficient_memory" {
		return nil, fmt.Errorf("orly: %s -> %s: %w", stmt, msg, ErrInsufficientMemory)
	}
	if r.Status == "write_too_large" {
		return nil, fmt.Errorf("orly: %s -> %s: %w", stmt, msg, ErrWriteTooLarge)
	}
	if r.Status == "read_too_large" {
		return nil, fmt.Errorf("orly: %s -> %s: %w", stmt, msg, ErrReadTooLarge)
	}
	if r.Status == "remote_compile_disabled" {
		return nil, fmt.Errorf("orly: %s -> %s: %w", stmt, msg, ErrRemoteCompileDisabled)
	}
	if r.Status == "unauthorized" {
		return nil, fmt.Errorf("orly: %s -> %s: %w", stmt, msg, ErrUnauthorized)
	}
	if r.Status != "ok" {
		return nil, fmt.Errorf("orly: %s -> %s", stmt, msg)
	}
	return r.Result, nil
}

// SendString sends a statement whose result is a JSON string and returns it.
func (c *Client) SendString(stmt string) (string, error) {
	raw, err := c.Send(stmt)
	if err != nil {
		return "", err
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return "", fmt.Errorf("orly: expected string result from %q: %w", stmt, err)
	}
	return s, nil
}

// NewSession opens a session and returns its id.
func (c *Client) NewSession() (string, error) { return c.SendString("new session;") }

// Install installs a package version.
func (c *Client) Install(pkg string, version int) error {
	_, err := c.Send(fmt.Sprintf("install %s.%d;", pkg, version))
	return err
}

// Uninstall uninstalls a package version.
func (c *Client) Uninstall(pkg string, version int) error {
	_, err := c.Send(fmt.Sprintf("uninstall %s.%d;", pkg, version))
	return err
}

// NewPov creates a "new safe shared pov;" and returns its id.
func (c *Client) NewPov() (string, error) { return c.SendString("new safe shared pov;") }

// Call invokes package.method on pov with a record of args, i.e.
// "try {pov} pkg method <{.k: v, ...}>;". Pass nil args for no arguments.
func (c *Client) Call(pov, pkg, method string, args map[string]any) (json.RawMessage, error) {
	lit, err := litRecord(args)
	if err != nil {
		return nil, err
	}
	return c.Send(fmt.Sprintf("try {%s} %s %s %s;", pov, pkg, method, lit))
}

// CallBatch invokes pkg method on pov once per record in argsList, folding all N
// calls into a single transaction (#253). It builds
// `try {pov} pkg method [<{...}>, <{...}>, ...];` and returns a JSON array of the
// N per-call results, in order. The batch is all-or-nothing (one bad record
// rejects the set) and every call runs against the same pre-batch snapshot
// (no read-your-writes within a batch) -- a write-coalescing primitive for
// commutative fan-in / bulk load.
func (c *Client) CallBatch(pov, pkg, method string, argsList []map[string]any) (json.RawMessage, error) {
	if len(argsList) == 0 {
		return nil, fmt.Errorf("CallBatch requires at least one argument record")
	}
	recs := make([]string, 0, len(argsList))
	for _, a := range argsList {
		r, err := litRecord(a)
		if err != nil {
			return nil, err
		}
		recs = append(recs, r)
	}
	return c.Send(fmt.Sprintf("try {%s} %s %s [%s];", pov, pkg, method, strings.Join(recs, ", ")))
}

// Call is one element of a CallMany batch: a method of a package and its
// arguments.
type Call struct {
	Pkg    string
	Method string
	Args   map[string]any
}

// CallMany runs several different methods on pov as one transaction (#255). It
// builds `try {pov} [pkg method <{...}>, ...];` and returns a JSON array with one
// result per call, in order (they may differ in type). Every call reads the same
// pre-batch snapshot (no read-your-writes within a batch), and the batch is
// all-or-nothing: if any call fails, none of the writes land.
func (c *Client) CallMany(pov string, calls []Call) (json.RawMessage, error) {
	if len(calls) == 0 {
		return nil, fmt.Errorf("CallMany requires at least one call")
	}
	parts := make([]string, 0, len(calls))
	for _, call := range calls {
		r, err := litRecord(call.Args)
		if err != nil {
			return nil, err
		}
		parts = append(parts, fmt.Sprintf("%s %s %s", call.Pkg, call.Method, r))
	}
	return c.Send(fmt.Sprintf("try {%s} [%s];", pov, strings.Join(parts, ", ")))
}

// Exit ends the session.
func (c *Client) Exit() error {
	_, err := c.Send("exit;")
	return err
}

// Raw is a value injected into a statement as raw orlyscript, un-encoded
// (e.g. orly.Raw("now()")).
type Raw string

// Set encodes its elements as an orlyscript set literal {a, b, ...}.
type Set []any

// Lit encodes a Go value as an orlyscript literal:
//
//	Raw            -> verbatim
//	bool           -> true / false
//	int*/uint*     -> decimal
//	float*         -> shortest round-trippable form
//	string         -> quoted, escaped
//	map[string]any -> record <{.k: v, ...}> (keys sorted; records are by-name)
//	[]any          -> list [a, b, ...]
//	Set            -> set {a, b, ...}
func Lit(v any) (string, error) {
	switch x := v.(type) {
	case Raw:
		return string(x), nil
	case bool:
		if x {
			return "true", nil
		}
		return "false", nil
	case int:
		return strconv.FormatInt(int64(x), 10), nil
	case int8, int16, int32, int64:
		return fmt.Sprintf("%d", x), nil
	case uint, uint8, uint16, uint32, uint64:
		return fmt.Sprintf("%d", x), nil
	case float32:
		return strconv.FormatFloat(float64(x), 'g', -1, 32), nil
	case float64:
		return strconv.FormatFloat(x, 'g', -1, 64), nil
	case string:
		return quote(x), nil
	case map[string]any:
		return litRecord(x)
	case []any:
		return litSeq(x, "[", "]")
	case Set:
		return litSeq([]any(x), "{", "}")
	default:
		return "", fmt.Errorf("orly: cannot encode %T as an orlyscript literal: %v", v, v)
	}
}

func litRecord(m map[string]any) (string, error) {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(m))
	for _, k := range keys {
		ev, err := Lit(m[k])
		if err != nil {
			return "", err
		}
		parts = append(parts, "."+k+": "+ev)
	}
	return "<{" + strings.Join(parts, ", ") + "}>", nil
}

func litSeq(xs []any, open, close string) (string, error) {
	parts := make([]string, 0, len(xs))
	for _, e := range xs {
		ev, err := Lit(e)
		if err != nil {
			return "", err
		}
		parts = append(parts, ev)
	}
	return open + strings.Join(parts, ", ") + close, nil
}

// quote renders a Go string as an orlyscript string literal, escaping
// backslashes and double quotes, and writing control characters as \n, \r,
// \t or \xNN, since the engine's lexer refuses them raw.
func quote(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		switch c := s[i]; {
		case c == '\\' || c == '"':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '\n':
			b.WriteString(`\n`)
		case c == '\r':
			b.WriteString(`\r`)
		case c == '\t':
			b.WriteString(`\t`)
		case c < 0x20 || c == 0x7f:
			fmt.Fprintf(&b, `\x%02x`, c)
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}
