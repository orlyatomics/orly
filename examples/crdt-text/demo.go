// A collaborative text editor as a CRDT, on Orly, in Go.
//
// Mirrors demo.py: a Logoot sequence CRDT where the database is the merge.
// Every character is one commutative |= of an immutable
// <{.pos,.ch,.site,.clock}> record; the position is a dense Logoot id (a
// list of digits) so the document is a SET, and the visible text is the
// engine's read-time fold (sort by position, drop tombstoned, concatenate).
// The driver owns one algorithm -- between(p, q) -- and nothing else;
// convergence, tombstones, and time-travel are the engine's.
//
// Run via the wrapper:  ./run-go.sh
// Or directly (orlyi already up):  go run demo.go
package main

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/gorilla/websocket"
)

const wsURL = "ws://127.0.0.1:8082/"

// Logoot digit base: interior digits 1..BASE-1; 0 and BASE bracket the doc
// (begin = [], end = [BASE]). width zero-pads a digit so string order ==
// numeric order (the engine sorts the [str] positions).
const base = 1 << 16
const width = 5

var begin = []int{}   // floor; never stored, only a bound for between()
var end = []int{base} // > every real position

// A logical clock beyond any real edit (mirrors crdt_text.orly's forever).
const forever int64 = 9223372036854775807

// ---------------------------------------------------------------------
// The one real algorithm: a dense position id strictly between p and q
// (integer lists, p < q lexicographically with shorter/prefix first).
// ---------------------------------------------------------------------
func between(p, q []int, rng *rand.Rand) []int {
	res := []int{}
	i := 0
	for {
		pd := 0
		if i < len(p) {
			pd = p[i]
		}
		qd := base
		if i < len(q) {
			qd = q[i]
		}
		if pd+1 < qd {
			res = append(res, pd+1+rng.Intn(qd-pd-1))
			return res
		}
		res = append(res, pd) // no room: take lower, descend
		if !(i < len(q) && qd == pd) {
			q = []int{} // keep q only if it stays equal here
		}
		i++
	}
}

func enc(ints []int) []string {
	out := make([]string, len(ints))
	for i, d := range ints {
		out[i] = fmt.Sprintf("%0*d", width, d)
	}
	return out
}

func dec(strs []string) []int {
	out := make([]int, len(strs))
	for i, s := range strs {
		var d int
		fmt.Sscanf(s, "%d", &d)
		out[i] = d
	}
	return out
}

// Process-wide logical clock; every edit takes the next tick.
var clockCounter int64

func tick() int64 { return atomic.AddInt64(&clockCounter, 1) }

// ---------------------------------------------------------------------
// WS plumbing.
// ---------------------------------------------------------------------
type reply struct {
	Status string          `json:"status"`
	Result json.RawMessage `json:"result"`
}

func die(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}

func dial() *websocket.Conn {
	c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		die(fmt.Sprintf("dial %s: %v", wsURL, err))
	}
	return c
}

func send(c *websocket.Conn, stmt string) json.RawMessage {
	if err := c.WriteMessage(websocket.TextMessage, []byte(stmt)); err != nil {
		die(fmt.Sprintf("write %q: %v", stmt, err))
	}
	_, msg, err := c.ReadMessage()
	if err != nil {
		die(fmt.Sprintf("read after %q: %v", stmt, err))
	}
	var r reply
	if err := json.Unmarshal(msg, &r); err != nil {
		die(fmt.Sprintf("parse reply: %v\n  raw: %s", err, msg))
	}
	if r.Status != "ok" {
		die(fmt.Sprintf("%s: %s", stmt, string(msg)))
	}
	return r.Result
}

func sendString(c *websocket.Conn, stmt string) string {
	var s string
	if err := json.Unmarshal(send(c, stmt), &s); err != nil {
		die(fmt.Sprintf("expected string, got error %v", err))
	}
	return s
}

func sendInt(c *websocket.Conn, stmt string) int {
	var n float64
	if err := json.Unmarshal(send(c, stmt), &n); err != nil {
		die(fmt.Sprintf("expected number, got error %v", err))
	}
	return int(n)
}

func connect() *websocket.Conn {
	c := dial()
	send(c, "new session;")
	return c
}

func orlyStr(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}

func orlyPos(ints []int) string {
	parts := make([]string, 0, len(ints))
	for _, s := range enc(ints) {
		parts = append(parts, orlyStr(s))
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

// ---------------------------------------------------------------------
// Engine ops.
// ---------------------------------------------------------------------
const pkg = "crdt_text"

var povGlobal, docGlobal string

func opInsert(c *websocket.Conn, pos []int, ch, site string, clock int64) {
	send(c, fmt.Sprintf(`try {%s} %s insert <{.doc: %s, .pos: %s, .ch: %s, .site: %s, .clock: %d}>;`,
		povGlobal, pkg, orlyStr(docGlobal), orlyPos(pos), orlyStr(ch), orlyStr(site), clock))
}

func opRemove(c *websocket.Conn, pos []int, clock int64) {
	send(c, fmt.Sprintf(`try {%s} %s remove <{.doc: %s, .pos: %s, .clock: %d}>;`,
		povGlobal, pkg, orlyStr(docGlobal), orlyPos(pos), clock))
}

type vchar struct {
	pos      []int
	ch, site string
	clock    int
}

type vwire struct {
	Pos   []string `json:"pos"`
	Ch    string   `json:"ch"`
	Site  string   `json:"site"`
	Clock float64  `json:"clock"`
}

func visibleAsOf(c *websocket.Conn, asOf int64) []vchar {
	raw := send(c, fmt.Sprintf(`try {%s} %s visible_as_of <{.doc: %s, .as_of: %d}>;`,
		povGlobal, pkg, orlyStr(docGlobal), asOf))
	if string(raw) == "null" {
		return nil
	}
	var wire []vwire
	if err := json.Unmarshal(raw, &wire); err != nil {
		die(fmt.Sprintf("expected array-of-objects, got %s", raw))
	}
	out := make([]vchar, 0, len(wire))
	for _, w := range wire {
		out = append(out, vchar{pos: dec(w.Pos), ch: w.Ch, site: w.Site, clock: int(w.Clock)})
	}
	return out
}

func renderAsOf(c *websocket.Conn, asOf int64) string {
	return sendString(c, fmt.Sprintf(`try {%s} %s render_as_of <{.doc: %s, .as_of: %d}>;`,
		povGlobal, pkg, orlyStr(docGlobal), asOf))
}

func charCount(c *websocket.Conn) int {
	return sendInt(c, fmt.Sprintf(`try {%s} %s char_count <{.doc: %s}>;`, povGlobal, pkg, orlyStr(docGlobal)))
}

// ---------------------------------------------------------------------
// Editor-side ops, built on between().
// ---------------------------------------------------------------------
func insertText(c *websocket.Conn, index int, text, site string, rng *rand.Rand) {
	seq := visibleAsOf(c, forever)
	var left, right []int
	if index > 0 {
		left = seq[index-1].pos
	} else {
		left = begin
	}
	if index < len(seq) {
		right = seq[index].pos
	} else {
		right = end
	}
	prev := left
	for _, ch := range text {
		pos := between(prev, right, rng)
		opInsert(c, pos, string(ch), site, tick())
		prev = pos
	}
}

func deleteRange(c *websocket.Conn, start, count int) {
	seq := visibleAsOf(c, forever)
	for _, v := range seq[start : start+count] {
		opRemove(c, v.pos, tick())
	}
}

func runConcurrent(fns ...func()) {
	var wg sync.WaitGroup
	for _, f := range fns {
		wg.Add(1)
		f := f
		go func() { defer wg.Done(); f() }()
	}
	wg.Wait()
}

func show(c *websocket.Conn, label string) string {
	text := renderAsOf(c, forever)
	fmt.Printf("  %-34s %q\n", label, text)
	return text
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
func main() {
	boot := dial()
	send(boot, "new session;")
	send(boot, "install crdt_text.1;")
	povGlobal = sendString(boot, "new safe shared pov;")
	docGlobal = "doc"
	fmt.Printf("pov: %s\n", povGlobal)
	fmt.Printf("document: %q -- two editors (alice, bob) on one shared POV\n\n", docGlobal)

	arng := rand.New(rand.NewSource(1)) // per-editor deterministic streams
	brng := rand.New(rand.NewSource(2))

	// --- Phase 1: alice types the base sentence (sequential) ---
	fmt.Println("[phase 1] alice types the base sentence")
	a := connect()
	insertText(a, 0, "hello world", "alice", arng)
	a.Close()
	clockP1 := tick() - 1
	base1 := show(boot, "after phase 1:")

	// --- Phase 2: concurrent, non-overlapping ---
	fmt.Println("\n[phase 2] alice inserts 'BIG ' before 'world'  ||  bob appends '!'")
	runConcurrent(
		func() { ws := connect(); insertText(ws, 6, "BIG ", "alice", arng); ws.Close() },
		func() {
			ws := connect()
			insertText(ws, len(visibleAsOf(ws, forever)), "!", "bob", brng)
			ws.Close()
		},
	)
	clockP2 := tick() - 1
	afterP2 := show(boot, "after phase 2:")

	// --- Phase 3: concurrent SAME spot -- the no-conflict guarantee ---
	fmt.Println("\n[phase 3] alice inserts 'X'  ||  bob inserts 'Y'  -- both at the very start")
	runConcurrent(
		func() { ws := connect(); insertText(ws, 0, "X", "alice", arng); ws.Close() },
		func() { ws := connect(); insertText(ws, 0, "Y", "bob", brng); ws.Close() },
	)
	show(boot, "after phase 3:")

	// --- Phase 4: concurrent delete + insert ---
	fmt.Println("\n[phase 4] alice deletes the trailing '!'  ||  bob appends '?'")
	runConcurrent(
		func() {
			ws := connect()
			seq := visibleAsOf(ws, forever)
			idx := -1
			for i, v := range seq {
				if v.ch == "!" {
					idx = i
				}
			}
			if idx >= 0 {
				deleteRange(ws, idx, 1)
			}
			ws.Close()
		},
		func() {
			ws := connect()
			insertText(ws, len(visibleAsOf(ws, forever)), "?", "bob", brng)
			ws.Close()
		},
	)
	final := show(boot, "after phase 4 (final):")

	// --- Time-travel ---
	fmt.Println("\n[time-travel] the same document at past logical clocks")
	fmt.Printf("  as of end of phase 1 (clock %d):  %q\n", clockP1, renderAsOf(boot, clockP1))
	fmt.Printf("  as of end of phase 2 (clock %d):  %q\n", clockP2, renderAsOf(boot, clockP2))
	fmt.Printf("  live:                              %q\n", final)

	fmt.Println("\n=== the trick ===")
	fmt.Println("  Every character is one |= of an immutable <{.pos,.ch,.site,.clock}>")
	fmt.Println("  record; the position is a dense Logoot id (a list of digits) so the")
	fmt.Println("  document is just a SET. The visible text is the engine's read-time")
	fmt.Println("  fold: sort by position, drop tombstoned, concatenate. Concurrent")
	fmt.Println("  editors never coordinate -- convergence and no-lost-edits fall out")
	fmt.Println("  of commutative |= + the deterministic sort. The driver's only job")
	fmt.Println("  is between(p, q); the database is the CRDT.")

	// --- self-check ---
	var failures []string
	// 1. Convergence: three independent readers must agree.
	var texts []string
	for i := 0; i < 3; i++ {
		r := connect()
		texts = append(texts, renderAsOf(r, forever))
		r.Close()
	}
	if texts[0] != texts[1] || texts[1] != texts[2] {
		failures = append(failures, fmt.Sprintf("convergence: readers disagree: %q", texts))
	}
	converged := texts[0]
	// 2. char_count matches rendered length.
	if charCount(boot) != len([]rune(converged)) {
		failures = append(failures, "char_count != len(render)")
	}
	// 3. No lost edits.
	for _, needle := range []string{"hello", "world", "BIG", "X", "Y", "?"} {
		if !strings.Contains(converged, needle) {
			failures = append(failures, fmt.Sprintf("lost edit: %q missing from %q", needle, converged))
		}
	}
	if strings.Contains(converged, "!") {
		failures = append(failures, "tombstone failed: deleted '!' still visible")
	}
	// 4. Time-travel is stable history.
	if renderAsOf(boot, clockP1) != base1 {
		failures = append(failures, "time-travel: phase-1 snapshot drifted")
	}
	if renderAsOf(boot, clockP2) != afterP2 {
		failures = append(failures, "time-travel: phase-2 snapshot drifted")
	}

	send(boot, "exit;")

	if len(failures) > 0 {
		fmt.Println("\n=== self-check FAILED ===")
		for _, f := range failures {
			fmt.Println("  " + f)
		}
		os.Exit(1)
	}
	fmt.Println("\n=== self-check OK ===")
	fmt.Printf("  converged to %q across 3 independent readers;\n", converged)
	fmt.Println("  no concurrent edit lost; tombstone applied; history stable.")
}
