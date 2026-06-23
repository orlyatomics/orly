// Time-travel + multiverse demo against a running orlyi, in Go.
//
// Equivalent to demo.py -- same scenario (8 mainnet blocks, fork at h=6,
// 3 fork blocks), same expected output, same self-check. The Python and
// Go drivers are paired so reviewers can diff them in spirit.
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

	orly "github.com/orlyatomics/orly/clients/go"
)

const (
	satPerBTC  = 100_000_000
	forkHeight = 6
)

// One operation inside a block: a credit (or debit, if amount < 0) to addr.
type op struct {
	addr   string
	amount int64
}

var (
	mainnetBlocks = [][]op{
		{{"alice", 50_00000000}},                                                   // h=1 coinbase
		{{"alice", -10_00000000}, {"bob", 10_00000000}},                            // h=2
		{{"carol", 50_00000000}},                                                   // h=3 coinbase
		{{"alice", -5_00000000}, {"bob", 5_00000000}},                              // h=4
		{{"bob", -8_00000000}, {"dave", 8_00000000}},                               // h=5 (last common)
		{{"alice", 50_00000000}},                                                   // h=6 mainnet coinbase to alice
		{{"alice", -2_00000000}, {"carol", 2_00000000}},                            // h=7
		{{"alice", -1_00000000}, {"dave", 1_00000000}},                             // h=8
	}

	forkBlocks = [][]op{
		{{"bob", 50_00000000}},                                                     // h=6 fork: coinbase to bob
		{{"bob", -20_00000000}, {"alice", 20_00000000}},                            // h=7
		{{"alice", -5_00000000}, {"carol", 5_00000000}},                            // h=8
	}

	watch = []string{"alice", "bob", "carol", "dave"}

	// Expected balances in BTC for [alice, bob, carol, dave] at each height.
	expectMainnet = []struct {
		h    int
		bals [4]int64
	}{
		{0, [4]int64{0, 0, 0, 0}},
		{1, [4]int64{50, 0, 0, 0}},
		{2, [4]int64{40, 10, 0, 0}},
		{3, [4]int64{40, 10, 50, 0}},
		{4, [4]int64{35, 15, 50, 0}},
		{5, [4]int64{35, 7, 50, 8}},
		{6, [4]int64{85, 7, 50, 8}},
		{7, [4]int64{83, 7, 52, 8}},
		{8, [4]int64{82, 7, 52, 9}},
	}
	expectFork = []struct {
		h    int
		bals [4]int64
	}{
		{0, [4]int64{0, 0, 0, 0}},
		{1, [4]int64{50, 0, 0, 0}},
		{2, [4]int64{40, 10, 0, 0}},
		{3, [4]int64{40, 10, 50, 0}},
		{4, [4]int64{35, 15, 50, 0}},
		{5, [4]int64{35, 7, 50, 8}},
		{6, [4]int64{35, 57, 50, 8}},
		{7, [4]int64{55, 37, 50, 8}},
		{8, [4]int64{50, 37, 55, 8}},
	}
)

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

// asInt decodes a result that orlyi serialises as a JSON float (e.g.
// `5000000000.0`) into int64. Integer satoshi stay inside float64's exact
// range (2^53), so there's no precision loss.
func asInt(raw json.RawMessage) int64 {
	var n float64
	if err := json.Unmarshal(raw, &n); err != nil {
		die(fmt.Sprintf("expected number result, got %s", raw))
	}
	return int64(n)
}

// Convenience wrappers over the shared client.
func creditAt(c *orly.Client, pov, branch, addr string, amount int64, h int) {
	must(c.Call(pov, "bitcoin", "credit_at",
		map[string]any{"branch": branch, "addr": addr, "amount": amount, "h": h}))
}

func balanceAt(c *orly.Client, pov, branch, addr string, h int) int64 {
	return asInt(must(c.Call(pov, "bitcoin", "balance_at",
		map[string]any{"branch": branch, "addr": addr, "h": h})))
}

func forkFrom(c *orly.Client, pov, branch, parent string, h int) {
	must(c.Call(pov, "bitcoin", "fork_from",
		map[string]any{"branch": branch, "parent": parent, "fork_h": h}))
}

func fmtBTC(satoshi int64) string {
	return fmt.Sprintf("%7.2f", float64(satoshi)/satPerBTC)
}

func printChain(c *orly.Client, pov, branch string, maxH int) {
	fmt.Printf("  %-3s  ", "h")
	for _, a := range watch {
		fmt.Printf("%7s  ", a)
	}
	fmt.Println()
	fmt.Println("  " + dashes(8+9*len(watch)))
	for h := 0; h <= maxH; h++ {
		fmt.Printf("  %2d   ", h)
		for _, addr := range watch {
			fmt.Printf("%s  ", fmtBTC(balanceAt(c, pov, branch, addr, h)))
		}
		fmt.Println()
	}
}

func dashes(n int) string {
	out := make([]byte, n)
	for i := range out {
		out[i] = '-'
	}
	return string(out)
}

func main() {
	c, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	defer c.Close()

	must(c.NewSession())
	check(c.Install("bitcoin", 1))
	pov := must(c.NewPov())
	fmt.Printf("pov: %s\n\n", pov)

	fmt.Printf("applying mainnet (%d blocks)...\n", len(mainnetBlocks))
	for hIdx, ops := range mainnetBlocks {
		h := hIdx + 1
		for _, o := range ops {
			creditAt(c, pov, "mainnet", o.addr, o.amount, h)
		}
	}

	fmt.Printf("forking at h=%d, applying %d fork blocks...\n", forkHeight, len(forkBlocks))
	forkFrom(c, pov, "fork", "mainnet", forkHeight)
	for i, ops := range forkBlocks {
		h := forkHeight + i
		for _, o := range ops {
			creditAt(c, pov, "fork", o.addr, o.amount, h)
		}
	}

	totalH := len(mainnetBlocks)

	fmt.Println("\n=== MAINNET ===")
	printChain(c, pov, "mainnet", totalH)

	fmt.Printf("\n=== FORK (forked from mainnet at h=%d) ===\n", forkHeight)
	printChain(c, pov, "fork", totalH)

	fmt.Println("\n=== mainnet vs fork, per height (* = post-fork) ===")
	fmt.Println("  h     mainnet alice  fork alice    mainnet bob   fork bob")
	fmt.Println("  " + dashes(58))
	for h := 0; h <= totalH; h++ {
		ma := balanceAt(c, pov, "mainnet", "alice", h)
		fa := balanceAt(c, pov, "fork", "alice", h)
		mb := balanceAt(c, pov, "mainnet", "bob", h)
		fb := balanceAt(c, pov, "fork", "bob", h)
		marker := "  "
		if h >= forkHeight {
			marker = " *"
		}
		fmt.Printf(" %s%2d   %10s     %10s     %10s     %10s\n",
			marker, h, fmtBTC(ma), fmtBTC(fa), fmtBTC(mb), fmtBTC(fb))
	}
	fmt.Printf("\n  rows 0..%d are identical: reads on `fork` recurse through `<['parent', 'fork']>` into mainnet's keyspace\n", forkHeight-1)

	// Self-check, same as Python.
	var failures []string
	for _, want := range expectMainnet {
		for i, addr := range watch {
			got := balanceAt(c, pov, "mainnet", addr, want.h)
			expected := want.bals[i] * satPerBTC
			if got != expected {
				failures = append(failures, fmt.Sprintf("mainnet h=%d %s: got %d, want %d", want.h, addr, got, expected))
			}
		}
	}
	for _, want := range expectFork {
		for i, addr := range watch {
			got := balanceAt(c, pov, "fork", addr, want.h)
			expected := want.bals[i] * satPerBTC
			if got != expected {
				failures = append(failures, fmt.Sprintf("fork h=%d %s: got %d, want %d", want.h, addr, got, expected))
			}
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
	fmt.Println("\n=== self-check OK ===")
	checked := (len(expectMainnet) + len(expectFork)) * len(watch)
	fmt.Printf("  verified %d balance values across both branches\n", checked)
}
