// Recursive sum-type storage + client marshaling demo (issue #115), in Go.
//
// Equivalent to demo.py -- same scenario and self-check. Stores a fixed
// recursive value of each shape, reads it back, and checks the marshaled JSON.
// A variant marshals as {"Tag": <payload>}; a tag-only arm as {"Tag": {}};
// numbers come back as JSON floats.
//
// Uses the shared `orly` client (clients/go). Run via the wrapper: ./run-go.sh
// Or directly, after starting orlyi separately:  go run demo.go
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"reflect"

	orly "github.com/orlyatomics/orly/clients/go"
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

// A case: a recursive value stored then read back, with the expected
// round-tripped JSON (as a literal the driver parses for comparison).
type recCase struct {
	label    string
	put, get string
	expected string
}

var cases = []recCase{
	{
		"tree  (self-reference in a record arm, #103)",
		"put_tree", "get_tree",
		`{"Branch": {"l": {"Leaf": 1.0}, "r": {"Branch": {"l": {"Leaf": 2.0}, "r": {"Leaf": 3.0}}}}}`,
	},
	{
		"doc   (self-reference under a list, #120)",
		"put_doc", "get_doc",
		`{"Arr": [{"Num": 1.0}, {"Str": "two"}, {"Arr": [{"Num": 3.0}]}, {"Null": {}}]}`,
	},
	{
		"mtree (mutual recursion, #116)",
		"put_mtree", "get_mtree",
		`{"MNode": {"FCons": {"head": {"MLeaf": 1.0}, "tail": {"FCons": {"head": {"MLeaf": 2.0}, "tail": {"FEmpty": {}}}}}}}`,
	},
	{
		"ntree (nested-variant arm, #125)",
		"put_ntree", "get_ntree",
		`{"NBranch": {"Un": {"NLeaf": 7.0}}}`,
	},
}

// parse turns JSON bytes into a structure DeepEqual can compare (maps/slices/
// float64/string), independent of key order.
func parse(b []byte, what string) interface{} {
	var v interface{}
	if err := json.Unmarshal(b, &v); err != nil {
		die(fmt.Sprintf("parse %s: %v\n  raw: %s", what, err, b))
	}
	return v
}

func main() {
	c, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	defer c.Close()

	must(c.NewSession())
	check(c.Install("recursive", 0))
	pov := must(c.NewPov())
	fmt.Printf("pov: %s\n\n", pov)

	failures := 0
	for k, rc := range cases {
		must(c.Call(pov, "recursive", rc.put, map[string]any{"k": k}))
		got := must(c.Call(pov, "recursive", rc.get, map[string]any{"k": k}))
		ok := reflect.DeepEqual(parse(got, "result"), parse([]byte(rc.expected), "expected"))
		status := "ok"
		if !ok {
			status = "FAIL"
			failures++
		}
		fmt.Printf("  [%s] %s\n", status, rc.label)
		if !ok {
			fmt.Printf("        expected: %s\n        got:      %s\n", rc.expected, got)
		}
	}

	check(c.Exit())
	if failures > 0 {
		die(fmt.Sprintf("\n%d case(s) failed.", failures))
	}
	fmt.Println("\nAll recursive-variant values round-tripped through storage and marshaled correctly.")
}
