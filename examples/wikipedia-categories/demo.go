// Wikipedia-style category-membership demo, in Go.
//
// Mirrors demo.py: same scenario, same expected output, same self-check.
// Provided so readers can pick whichever language matches their environment.
//
// Run via the wrapper:
//
//	./run-go.sh
//
// Or directly, after starting orlyi separately:
//
//	go run demo.go
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"sort"
	"strings"

	orly "github.com/orlyatomics/orly/clients/go"
)

type growth struct {
	cat     string
	year    int
	members []string
}

var events = []growth{
	// --- Programming languages ---
	{"languages", 1957, []string{"FORTRAN"}},
	{"languages", 1958, []string{"Lisp", "ALGOL"}},
	{"languages", 1959, []string{"COBOL"}},
	{"languages", 1964, []string{"BASIC"}},
	{"languages", 1970, []string{"Pascal"}},
	{"languages", 1972, []string{"C", "Smalltalk"}},
	{"languages", 1980, []string{"Ada"}},
	{"languages", 1983, []string{"C++", "Objective-C"}},
	{"languages", 1986, []string{"Erlang"}},
	{"languages", 1987, []string{"Perl"}},
	{"languages", 1990, []string{"Haskell"}},
	{"languages", 1991, []string{"Python", "Visual Basic"}},
	{"languages", 1993, []string{"Lua"}},
	{"languages", 1995, []string{"Java", "JavaScript", "PHP", "Ruby"}},
	{"languages", 2000, []string{"C#"}},
	{"languages", 2003, []string{"Scala", "Groovy"}},
	{"languages", 2007, []string{"Clojure"}},
	{"languages", 2009, []string{"Go"}},
	{"languages", 2010, []string{"Rust"}},
	{"languages", 2011, []string{"Dart", "Elixir", "Kotlin"}},
	{"languages", 2012, []string{"Julia", "TypeScript"}},
	{"languages", 2014, []string{"Swift"}},
	{"languages", 2016, []string{"Zig"}},
	{"languages", 2023, []string{"Mojo"}},

	// --- Cryptocurrencies ---
	{"crypto", 2009, []string{"Bitcoin"}},
	{"crypto", 2011, []string{"Litecoin", "Namecoin"}},
	{"crypto", 2012, []string{"Ripple"}},
	{"crypto", 2013, []string{"Dogecoin"}},
	{"crypto", 2014, []string{"Monero"}},
	{"crypto", 2015, []string{"Ethereum"}},
	{"crypto", 2017, []string{"Bitcoin Cash", "Cardano"}},
	{"crypto", 2020, []string{"Solana"}},
}

var snapshots = []struct {
	cat   string
	years []int
}{
	{"languages", []int{1957, 1965, 1975, 1985, 1995, 2005, 2015, 2024}},
	{"crypto", []int{2008, 2010, 2012, 2014, 2016, 2018, 2020, 2024}},
}

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

// orlyi serialises sets as JSON arrays; decode + sort for stable comparison.
func asStringSet(raw json.RawMessage) []string {
	var out []string
	if string(raw) == "null" {
		return out
	}
	if err := json.Unmarshal(raw, &out); err != nil {
		die(fmt.Sprintf("expected array result, got %s", raw))
	}
	sort.Strings(out)
	return out
}

// strSet turns a []string into an orly.Set (encoded as {"a", "b"}).
func strSet(members []string) orly.Set {
	s := make(orly.Set, len(members))
	for i, m := range members {
		s[i] = m
	}
	return s
}

func addGrowth(c *orly.Client, pov, cat string, year int, members []string) {
	must(c.Call(pov, "wiki", "add_growth",
		map[string]any{"cat": cat, "year": year, "members": strSet(members)}))
}

func membersAt(c *orly.Client, pov, cat string, since, target int) []string {
	return asStringSet(must(c.Call(pov, "wiki", "members_at",
		map[string]any{"cat": cat, "since": since, "target": target})))
}

func main() {
	c, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	defer c.Close()

	must(c.NewSession())
	check(c.Install("wiki", 1))
	pov := must(c.NewPov())
	fmt.Printf("pov: %s\n\n", pov)

	catSet := map[string]struct{}{}
	for _, e := range events {
		catSet[e.cat] = struct{}{}
	}
	fmt.Printf("ingesting %d growth events across %d categories...\n", len(events), len(catSet))
	for _, e := range events {
		addGrowth(c, pov, e.cat, e.year, e.members)
	}

	pretty := map[string]string{
		"languages": "Programming languages",
		"crypto":    "Cryptocurrencies",
	}
	for _, snap := range snapshots {
		fmt.Println()
		fmt.Printf("=== %s ===\n", pretty[snap.cat])
		for _, y := range snap.years {
			ms := membersAt(c, pov, snap.cat, 0, y)
			body := strings.Join(ms, ", ")
			if body == "" {
				body = "(empty)"
			}
			fmt.Printf("  %d  (%2d): %s\n", y, len(ms), body)
		}
	}

	// Cross-category sanity.
	fmt.Println()
	fmt.Println("=== cross-category isolation ===")
	lang2024 := membersAt(c, pov, "languages", 0, 2024)
	crypto2024 := membersAt(c, pov, "crypto", 0, 2024)
	overlap := []string{}
	cryptoSet := map[string]struct{}{}
	for _, x := range crypto2024 {
		cryptoSet[x] = struct{}{}
	}
	for _, x := range lang2024 {
		if _, ok := cryptoSet[x]; ok {
			overlap = append(overlap, x)
		}
	}
	overlapStr := strings.Join(overlap, ", ")
	if overlapStr == "" {
		overlapStr = "empty (good — different keyspaces)"
	}
	fmt.Printf("  |languages 2024| = %d\n", len(lang2024))
	fmt.Printf("  |crypto 2024|    = %d\n", len(crypto2024))
	fmt.Printf("  overlap          = %s\n", overlapStr)

	fmt.Println()
	fmt.Println("=== the trick ===")
	fmt.Println("  bitcoin demo:  [0..h] reduce start 0 + delta_at(addr, that)")
	fmt.Println("  this demo:     [since..target] reduce start (empty {str}) | growth_in(cat, that)")
	fmt.Println("  same pattern, different commutative monoid.")

	// Self-check (mirrors demo.py).
	type want struct {
		cat      string
		year     int
		expected []string
	}
	wants := []want{
		{"languages", 1957, []string{"FORTRAN"}},
		{"languages", 1958, []string{"ALGOL", "FORTRAN", "Lisp"}},
		{"languages", 1995, []string{"ALGOL", "Ada", "BASIC", "C", "C++", "COBOL", "Erlang",
			"FORTRAN", "Haskell", "Java", "JavaScript", "Lisp", "Lua",
			"Objective-C", "PHP", "Pascal", "Perl", "Python", "Ruby",
			"Smalltalk", "Visual Basic"}},
		{"crypto", 2009, []string{"Bitcoin"}},
		{"crypto", 2024, []string{"Bitcoin", "Bitcoin Cash", "Cardano", "Dogecoin", "Ethereum",
			"Litecoin", "Monero", "Namecoin", "Ripple", "Solana"}},
	}
	var failures []string
	for _, w := range wants {
		got := membersAt(c, pov, w.cat, 0, w.year)
		if !reflect.DeepEqual(got, w.expected) {
			failures = append(failures, fmt.Sprintf("%s at %d: got %v, want %v", w.cat, w.year, got, w.expected))
		}
	}

	check(c.Exit())

	if len(failures) > 0 {
		fmt.Println("\n=== self-check FAILED ===")
		for _, f := range failures {
			fmt.Println("  " + f)
		}
		os.Exit(1)
	}
	fmt.Printf("\n=== self-check OK ===\n  verified %d snapshot queries\n", len(wants))
}
