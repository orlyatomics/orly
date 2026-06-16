// Recursive sum-type storage + client marshaling demo (issue #115), in Go.
//
// Equivalent to demo.py -- same scenario and self-check. Connects to a
// running orlyi over the WebSocket protocol, stores a fixed recursive value
// of each shape, reads it back, and checks the marshaled JSON. A variant
// marshals as {"Tag": <payload>}; a tag-only arm as {"Tag": {}}; numbers come
// back as JSON floats. The Python and Go drivers are paired so reviewers can
// diff them in spirit.
//
// Run via the wrapper:  ./run-go.sh
// Or directly, after starting orlyi separately:  go run demo.go
package main

import (
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"reflect"

	"github.com/gorilla/websocket"
)

const wsURL = "ws://127.0.0.1:8082/"

type reply struct {
	Status string          `json:"status"`
	Result json.RawMessage `json:"result"`
	Pos    string          `json:"pos,omitempty"`
}

func die(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
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
		die(fmt.Sprintf("parse reply to %q: %v\n  raw: %s", stmt, err, msg))
	}
	if r.Status != "ok" {
		die(fmt.Sprintf("%s: %s", stmt, string(msg)))
	}
	return r.Result
}

func sendString(c *websocket.Conn, stmt string) string {
	var s string
	if err := json.Unmarshal(send(c, stmt), &s); err != nil {
		die(fmt.Sprintf("expected string result from %q", stmt))
	}
	return s
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
	u, _ := url.Parse(wsURL)
	c, _, err := websocket.DefaultDialer.Dial(u.String(), nil)
	if err != nil {
		die(fmt.Sprintf("dial %s: %v", wsURL, err))
	}
	defer c.Close()

	send(c, "new session;")
	send(c, "install recursive.0;")
	pov := sendString(c, "new safe shared pov;")
	fmt.Printf("pov: %s\n\n", pov)

	failures := 0
	for k, rc := range cases {
		send(c, fmt.Sprintf("try {%s} recursive %s <{.k: %d}>;", pov, rc.put, k))
		got := send(c, fmt.Sprintf("try {%s} recursive %s <{.k: %d}>;", pov, rc.get, k))
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

	send(c, "exit;")
	if failures > 0 {
		die(fmt.Sprintf("\n%d case(s) failed.", failures))
	}
	fmt.Println("\nAll recursive-variant values round-tripped through storage and marshaled correctly.")
}
