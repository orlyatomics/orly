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

	orly "github.com/orlyatomics/orly/clients/go"
)


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

func die(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}

func must[T any](v T, err error) T {
	if err != nil {
		die(err.Error())
	}
	return v
}

func check(err error) {
	if err != nil {
		die(err.Error())
	}
}

func asString(raw json.RawMessage) string {
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		die(fmt.Sprintf("expected string, got %s", raw))
	}
	return s
}

func asInt(raw json.RawMessage) int {
	var n float64
	if err := json.Unmarshal(raw, &n); err != nil {
		die(fmt.Sprintf("expected number, got %s", raw))
	}
	return int(n)
}

func connect() *orly.Client {
	c, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	must(c.NewSession())
	return c
}

// posLit renders a dense int position as the engine's [str] form, as an
// []any orly.Lit encodes to a list literal ["000NN", ...].
func posLit(ints []int) []any {
	enced := enc(ints)
	out := make([]any, len(enced))
	for i, s := range enced {
		out[i] = s
	}
	return out
}

// ---------------------------------------------------------------------
// Engine ops.
// ---------------------------------------------------------------------
const pkg = "crdt_text"

var povGlobal, docGlobal string

func opInsert(c *orly.Client, pos []int, ch, site string, clock int64) {
	must(c.Call(povGlobal, pkg, "insert", map[string]any{
		"doc": docGlobal, "pos": posLit(pos), "ch": ch, "site": site, "clock": clock}))
}

func opRemove(c *orly.Client, pos []int, clock int64) {
	must(c.Call(povGlobal, pkg, "remove", map[string]any{
		"doc": docGlobal, "pos": posLit(pos), "clock": clock}))
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

func visibleAsOf(c *orly.Client, asOf int64) []vchar {
	raw := must(c.Call(povGlobal, pkg, "visible_as_of",
		map[string]any{"doc": docGlobal, "as_of": asOf}))
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

func renderAsOf(c *orly.Client, asOf int64) string {
	return asString(must(c.Call(povGlobal, pkg, "render_as_of",
		map[string]any{"doc": docGlobal, "as_of": asOf})))
}

func charCount(c *orly.Client) int {
	return asInt(must(c.Call(povGlobal, pkg, "char_count",
		map[string]any{"doc": docGlobal})))
}

// ---------------------------------------------------------------------
// Editor-side ops, built on between().
// ---------------------------------------------------------------------
func insertText(c *orly.Client, index int, text, site string, rng *rand.Rand) {
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

func deleteRange(c *orly.Client, start, count int) {
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

func show(c *orly.Client, label string) string {
	text := renderAsOf(c, forever)
	fmt.Printf("  %-34s %q\n", label, text)
	return text
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
func main() {
	boot := connect()
	check(boot.Install("crdt_text", 1))
	povGlobal = must(boot.NewPov())
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

	check(boot.Exit())

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
