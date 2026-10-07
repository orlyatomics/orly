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
	"math"
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

// PovOptions says what NewPovWith makes: a safe or fast, shared or private POV,
// under Parent (a POV id; empty for the global POV), and how it treats
// promotion conflicts (#746): "" or "none", "report" or "refuse".
type PovOptions struct {
	Safe      bool
	Shared    bool
	Parent    string
	Conflicts string
}

// NewPovWith creates a POV as o says and returns its id.
func (c *Client) NewPovWith(o PovOptions) (string, error) {
	parts := []string{"new", "fast", "private", "pov"}
	if o.Safe {
		parts[1] = "safe"
	}
	if o.Shared {
		parts[2] = "shared"
	}
	if o.Parent != "" {
		parts = append(parts, fmt.Sprintf("from {%s}", o.Parent))
	}
	if o.Conflicts != "" && o.Conflicts != "none" {
		parts = append(parts, fmt.Sprintf("<{.conflicts: %s}>", quote(o.Conflicts)))
	}
	return c.SendString(strings.Join(parts, " ") + ";")
}

// Count is a count in a POV review result. The protocol sends every number as
// a JSON float, which Count accepts.
type Count int64

// UnmarshalJSON accepts 3 and 3.0.
func (n *Count) UnmarshalJSON(b []byte) error {
	var f float64
	if err := json.Unmarshal(b, &f); err != nil {
		return err
	}
	*n = Count(f)
	return nil
}

// PovChange is one key a POV changed relative to its parent (#746). Kind is
// "added", "changed", "removed" or "delta"; Before is the parent's value and
// After the POV's (nil when absent); a delta is a run of one commutative
// operator, Op ("add", "or", "union", ...), adding up to Delta. Values are
// decoded JSON.
type PovChange struct {
	Key    []any  `json:"key"`
	Kind   string `json:"kind"`
	Before any    `json:"before"`
	After  any    `json:"after"`
	Op     string `json:"op,omitempty"`
	Delta  any    `json:"delta,omitempty"`
}

// PovDiff is a page of a diff. Next (and its exact orlyscript form,
// NextLiteral) is nil on the last page.
type PovDiff struct {
	Changes     []PovChange `json:"changes"`
	Next        []any       `json:"next"`
	NextLiteral *string     `json:"next_literal"`
	Updates     Count       `json:"updates"`
}

// DiffOptions restrict a diff to keys from Start (inclusive) to Stop
// (exclusive), after After (the previous page's Next, or Raw(NextLiteral)),
// with at most Limit changes a page (0: the server's default, 100). Keys are
// Addr values, or Raw literals.
type DiffOptions struct {
	Start, Stop, After any
	Limit              int
}

// PovConflict is a key a POV's update overwrote ("put") or deleted ("delete")
// after its parent changed it. Number counts them from 1 (0 for a key the POV
// is blocked on); Raced marks one, in refusing mode, that the parent changed
// between Tetris's test and the promotion.
type PovConflict struct {
	Number Count  `json:"number"`
	Key    []any  `json:"key"`
	Op     string `json:"op"`
	Raced  bool   `json:"raced"`
}

// PovDiscard is what Discard threw away.
type PovDiscard struct {
	Updates Count `json:"discarded_updates"`
	Entries Count `json:"discarded_entries"`
}

// PovPromote is what RequestPromotion did: Status "promoting" or "refused"
// (with the would-be Conflicts). Mark is the number of the last conflict
// before it.
type PovPromote struct {
	Status    string        `json:"status"`
	Pending   Count         `json:"pending"`
	Mark      Count         `json:"mark"`
	Conflicts []PovConflict `json:"conflicts"`
}

// PovReview is a POV's promotion progress and conflicts.
type PovReview struct {
	ConflictMode   string        `json:"conflict_mode"`
	Status         string        `json:"status"`
	Pending        Count         `json:"pending"`
	PendingEntries Count         `json:"pending_entries"`
	Blocked        bool          `json:"blocked"`
	BlockedOn      []PovConflict `json:"blocked_on"`
	Conflicts      []PovConflict `json:"conflicts"`
	ConflictCount  Count         `json:"conflict_count"`
	ChangedKeys    Count         `json:"changed_keys"`
	Overflowed     bool          `json:"overflowed"`
}

// PromoteResult is what Promote saw: Status "promoted" (nothing pending),
// "refused", "blocked", "failed", "paused" or "timeout", and the conflicts
// found during the promotion.
type PromoteResult struct {
	Status    string
	Pending   Count
	Conflicts []PovConflict
	BlockedOn []PovConflict
}

func (c *Client) sendInto(stmt string, out any) error {
	raw, err := c.Send(stmt)
	if err != nil {
		return err
	}
	return json.Unmarshal(raw, out)
}

func keyLit(k any) (string, error) {
	switch x := k.(type) {
	case Raw, Addr:
		return Lit(x)
	case []any:
		return Lit(Addr(x))
	default:
		return "", fmt.Errorf("orly: a key must be an Addr, a []any or a Raw literal, not %T", k)
	}
}

// Diff returns a page of what pov changed relative to its parent: its
// unpromoted writes, key by key, in key order (#746).
func (c *Client) Diff(pov string, o DiffOptions) (PovDiff, error) {
	var parts []string
	for _, opt := range []struct {
		name string
		key  any
	}{{"start", o.Start}, {"stop", o.Stop}, {"after", o.After}} {
		if opt.key == nil {
			continue
		}
		l, err := keyLit(opt.key)
		if err != nil {
			return PovDiff{}, err
		}
		parts = append(parts, "."+opt.name+": "+l)
	}
	if o.Limit > 0 {
		parts = append(parts, fmt.Sprintf(".limit: %d", o.Limit))
	}
	suffix := ""
	if len(parts) > 0 {
		suffix = " <{" + strings.Join(parts, ", ") + "}>"
	}
	var d PovDiff
	err := c.sendInto(fmt.Sprintf("diff_pov {%s}%s;", pov, suffix), &d)
	return d, err
}

// DiffAll returns every change of pov's diff, page after page (keyset paging).
func (c *Client) DiffAll(pov string, o DiffOptions) ([]PovChange, error) {
	var all []PovChange
	for {
		page, err := c.Diff(pov, o)
		if err != nil {
			return nil, err
		}
		all = append(all, page.Changes...)
		if page.NextLiteral == nil {
			return all, nil
		}
		o.After = Raw(*page.NextLiteral)
	}
}

// Discard throws away pov's unpromoted changes (a private POV of this
// session's), so it reads as its parent again (#746).
func (c *Client) Discard(pov string) (PovDiscard, error) {
	var d PovDiscard
	err := c.sendInto(fmt.Sprintf("discard_pov {%s};", pov), &d)
	return d, err
}

// RequestPromotion asks for pov's changes to be promoted (unpauses it) and
// returns at once. In refusing mode it is tested first, and stays as it was if
// any change would conflict, unless force.
func (c *Client) RequestPromotion(pov string, force bool) (PovPromote, error) {
	suffix := ""
	if force {
		suffix = " <{.force: true}>"
	}
	var p PovPromote
	err := c.sendInto(fmt.Sprintf("promote_pov {%s}%s;", pov, suffix), &p)
	return p, err
}

// Review returns pov's promotion progress and the conflicts numbered after
// after.
func (c *Client) Review(pov string, after int64) (PovReview, error) {
	suffix := ""
	if after > 0 {
		suffix = fmt.Sprintf(" <{.after: %d}>", after)
	}
	var r PovReview
	err := c.sendInto(fmt.Sprintf("review_pov {%s}%s;", pov, suffix), &r)
	return r, err
}

// Promote promotes pov's changes and waits until nothing is pending, the POV
// is blocked, failed or paused, or timeout passes; it returns the conflicts
// found meanwhile.
func (c *Client) Promote(pov string, force bool, timeout time.Duration) (PromoteResult, error) {
	started, err := c.RequestPromotion(pov, force)
	if err != nil {
		return PromoteResult{}, err
	}
	if started.Status == "refused" {
		return PromoteResult{Status: "refused", Pending: started.Pending, Conflicts: started.Conflicts}, nil
	}
	deadline := time.Now().Add(timeout)
	for {
		r, err := c.Review(pov, int64(started.Mark))
		if err != nil {
			return PromoteResult{}, err
		}
		status := ""
		switch {
		case r.Status == "failed":
			status = "failed"
		case r.Blocked:
			status = "blocked"
		case r.Pending == 0:
			status = "promoted"
		case r.Status == "paused":
			status = "paused"
		case time.Now().After(deadline):
			status = "timeout"
		}
		if status != "" {
			return PromoteResult{Status: status, Pending: r.Pending, Conflicts: r.Conflicts, BlockedOn: r.BlockedOn}, nil
		}
		time.Sleep(50 * time.Millisecond)
	}
}

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

// ErrStopPages, returned by a Pages callback, stops paging; Pages then
// returns nil.
var ErrStopPages = errors.New("orly: stop paging")

// PageOptions tunes Pages. The zero value pages with the cursor argument
// "last" until the first empty page.
type PageOptions struct {
	// Cursor names the argument the method takes the cursor in, and the
	// field of its result that holds the next one. Default "last".
	Cursor string
	// PageSize, when positive, also stops paging after the first page with
	// fewer rows than this.
	PageSize int
	// ToCursor turns the returned cursor (decoded from JSON with UseNumber)
	// into the value Lit encodes for the next call. Default: every integral
	// number, however nested, becomes an int64 and every other number a
	// float64. A cursor holding real members that may be integral needs its
	// own, such as one returning Raw("3.0").
	ToCursor func(json.RawMessage) (any, error)
}

// Pages is keyset paging (#735): it calls pkg method on pov page after page
// and hands each page's rows to fn. The method takes args plus a cursor (the
// argument named by opts.Cursor) and returns <{.rows: [...], .last: ...}>: a
// page of rows and the cursor to pass for the page after it -- typically the
// last row's key, which the method gives to `keys (T) @ <[...]> after <[...]>`.
// args carries the first page's cursor. The server keeps no state between
// pages; each page reads the data as it is when that call runs. Paging stops
// at the first empty page, at a page shorter than opts.PageSize, or when fn
// returns an error (ErrStopPages stops without one).
func (c *Client) Pages(pov, pkg, method string, args map[string]any, opts PageOptions,
	fn func(rows []json.RawMessage) error) error {
	cursor := opts.Cursor
	if cursor == "" {
		cursor = "last"
	}
	if _, ok := args[cursor]; !ok {
		return fmt.Errorf("orly: Pages needs the first page's cursor as args[%q]", cursor)
	}
	toCursor := opts.ToCursor
	if toCursor == nil {
		toCursor = integralNumbersToInts
	}
	next := make(map[string]any, len(args))
	for k, v := range args {
		next[k] = v
	}
	for {
		raw, err := c.Call(pov, pkg, method, next)
		if err != nil {
			return err
		}
		var page map[string]json.RawMessage
		var rows []json.RawMessage
		if json.Unmarshal(raw, &page) != nil || page["rows"] == nil || page[cursor] == nil ||
			json.Unmarshal(page["rows"], &rows) != nil {
			return fmt.Errorf("orly: %s %s must return <{.rows: [...], .%s: ...}> to be paged, got %s",
				pkg, method, cursor, raw)
		}
		if len(rows) == 0 {
			return nil
		}
		if err := fn(rows); err != nil {
			if errors.Is(err, ErrStopPages) {
				return nil
			}
			return err
		}
		if opts.PageSize > 0 && len(rows) < opts.PageSize {
			return nil
		}
		if next[cursor], err = toCursor(page[cursor]); err != nil {
			return err
		}
	}
}

// integralNumbersToInts decodes raw with every integral number in it made an
// int64: the engine sends integers as JSON floats (1.0), and an int cursor
// sent back as a float would not be an int (#735).
func integralNumbersToInts(raw json.RawMessage) (any, error) {
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.UseNumber()
	var v any
	if err := dec.Decode(&v); err != nil {
		return nil, err
	}
	return numbersToLitValues(v), nil
}

func numbersToLitValues(v any) any {
	switch x := v.(type) {
	case json.Number:
		if i, err := x.Int64(); err == nil {
			return i
		}
		f, err := x.Float64()
		if err == nil && f == math.Trunc(f) && math.Abs(f) < 1<<63 {
			return int64(f)
		}
		return f
	case []any:
		for i := range x {
			x[i] = numbersToLitValues(x[i])
		}
		return x
	case map[string]any:
		for k := range x {
			x[k] = numbersToLitValues(x[k])
		}
		return x
	default:
		return v
	}
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

// Addr encodes its elements as an orlyscript address (key) literal <[a, b, ...]>.
type Addr []any

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
//	Addr           -> key <[a, b, ...]>
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
	case Addr:
		return litSeq([]any(x), "<[", "]>")
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
