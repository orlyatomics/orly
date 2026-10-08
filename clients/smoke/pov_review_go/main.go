// POV review smoke, Go client (#746); run by run-pov-review.sh after pov_review.mjs.
//
// A refuse-mode private POV under the global POV: paused, it overwrites a key the parent
// changed after the fork; its diff must list it and a counter delta; a promotion must be
// refused, then forced through with the conflict reported. Then a POV discards its paused
// writes.
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"time"

	orly "github.com/orlyatomics/orly/clients/go"
)

const pkg = "pov_review"
const g = 200

var failures int

func check(what string, got, want any) {
	if !reflect.DeepEqual(got, want) {
		failures++
		fmt.Printf("POV REVIEW GO FAIL: %s: got %#v, want %#v\n", what, got, want)
	}
}

func must[T any](v T, err error) T {
	if err != nil {
		fmt.Printf("POV REVIEW GO FAIL: %v\n", err)
		os.Exit(1)
	}
	return v
}

func get(c *orly.Client, pov string, e int) int {
	raw := must(c.Call(pov, pkg, "get", map[string]any{"g": g, "e": e}))
	var f float64
	if err := json.Unmarshal(raw, &f); err != nil {
		fmt.Printf("POV REVIEW GO FAIL: %v\n", err)
		os.Exit(1)
	}
	return int(f)
}

func waitFor(what string, read func() int, want int) {
	deadline := time.Now().Add(30 * time.Second)
	for got := read(); got != want; got = read() {
		if time.Now().After(deadline) {
			check(what+" (30 s)", got, want)
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func keyOf(k []any) string { return fmt.Sprint(k...) }

func main() {
	c := must(orly.ConnectURL(os.Getenv("ORLY_URL")))
	defer c.Close()
	must(c.NewSession())
	gw := must(c.NewPovWith(orly.PovOptions{}))
	gr := must(c.NewPovWith(orly.PovOptions{}))
	must(c.Call(gw, pkg, "put", map[string]any{"g": g, "e": 1, "w": 10}))
	must(c.Call(gw, pkg, "set_count", map[string]any{"g": g, "k": 0, "n": 5}))
	waitFor("base", func() int { return get(c, gr, 1) }, 10)

	p := must(c.NewPovWith(orly.PovOptions{Conflicts: "refuse"}))
	must(c.Send(fmt.Sprintf("pause {%s};", p)))
	must(c.Call(gw, pkg, "put", map[string]any{"g": g, "e": 1, "w": 1000}))
	waitFor("the parent's change", func() int { return get(c, gr, 1) }, 1000)
	must(c.Call(p, pkg, "put", map[string]any{"g": g, "e": 1, "w": 111}))
	must(c.Call(p, pkg, "bump", map[string]any{"g": g, "k": 0, "n": 3}))

	all := must(c.DiffAll(p, orly.DiffOptions{Limit: 1}))
	var kinds []string
	for _, ch := range all {
		kinds = append(kinds, keyOf(ch.Key)+" "+ch.Kind)
	}
	check("diff", kinds, []string{fmt.Sprint("count", g, 0) + " delta", fmt.Sprint("edge", g, 1) + " changed"})
	edges := must(c.Diff(p, orly.DiffOptions{Start: orly.Addr{"edge"}, Stop: orly.Addr{"tags"}}))
	check("ranged diff", len(edges.Changes), 1)

	refused := must(c.Promote(p, false, 30*time.Second))
	check("refused", [2]any{refused.Status, len(refused.Conflicts)}, [2]any{"refused", 1})
	forced := must(c.Promote(p, true, 30*time.Second))
	check("forced", [3]any{forced.Status, len(forced.Conflicts), forced.Conflicts[0].Number}, [3]any{"promoted", 1, orly.Count(1)})
	waitFor("forced writes land", func() int { return get(c, gr, 1) }, 111)

	d := must(c.NewPovWith(orly.PovOptions{Safe: true}))
	must(c.Send(fmt.Sprintf("pause {%s};", d)))
	must(c.Call(d, pkg, "put", map[string]any{"g": g, "e": 7, "w": 7}))
	discarded := must(c.Discard(d))
	check("discard", [2]orly.Count{discarded.Updates, discarded.Entries}, [2]orly.Count{1, 1})
	check("discarded", get(c, d, 7), -1)

	if failures > 0 {
		fmt.Printf("POV REVIEW GO FAIL: %d checks failed\n", failures)
		os.Exit(1)
	}
	fmt.Println("POV REVIEW GO OK")
}
