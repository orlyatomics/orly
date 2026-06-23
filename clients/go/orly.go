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
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/gorilla/websocket"
)

// DefaultURL is the WebSocket endpoint a local orlyi listens on.
const DefaultURL = "ws://127.0.0.1:8082/"

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

// ConnectURL dials a running orlyi at the given ws:// URL.
func ConnectURL(url string) (*Client, error) {
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		return nil, fmt.Errorf("orly: dial %s: %w", url, err)
	}
	return &Client{conn: conn}, nil
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
// backslashes and double quotes (matching the engine's lexer).
func quote(s string) string {
	s = strings.ReplaceAll(s, "\\", "\\\\")
	s = strings.ReplaceAll(s, "\"", "\\\"")
	return "\"" + s + "\""
}
