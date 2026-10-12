/*
   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

package orly

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/gorilla/websocket"
)

func TestKeywordFields(t *testing.T) {
	for _, name := range []string{"id", "to", "from", "start", "after", "if", "true",
		"int", "and", "keys", "try", "safe", "list_packages"} {
		t.Run(name, func(t *testing.T) {
			got, err := Lit(map[string]any{name: 1})
			if err != nil || got != "<{."+name+": 1}>" {
				t.Fatalf("Lit: %q, %v", got, err)
			}
		})
	}
	for _, tc := range []struct {
		value map[string]any
		want  string
	}{
		{map[string]any{"to": map[string]any{"id": 7}}, "<{.to: <{.id: 7}>}>"},
		{map[string]any{}, "<{}>"},
		{map[string]any{"_id2": true}, "<{._id2: true}>"},
	} {
		got, err := Lit(tc.value)
		if err != nil || got != tc.want {
			t.Fatalf("Lit: %q, %v; want %q", got, err, tc.want)
		}
	}
}

func TestInvalidFields(t *testing.T) {
	for _, name := range []string{"", "1st", "first-name", "a.b", "é", "id\n"} {
		t.Run(strconv.Quote(name), func(t *testing.T) {
			_, err := Lit(map[string]any{name: 1})
			if err == nil || !strings.Contains(err.Error(), strconv.Quote(name)) || !strings.Contains(err.Error(), "rename") {
				t.Fatalf("expected field name and rename suggestion, got %v", err)
			}
			_, err = Lit(map[string]any{"to": map[string]any{name: 1}})
			if err == nil || !strings.Contains(err.Error(), "invalid record field name") {
				t.Fatalf("expected nested field error, got %v", err)
			}
		})
	}
}

func TestFormatTryOptions(t *testing.T) {
	if got := formatTryOptions(CallOpts{}); got != "" {
		t.Fatalf("want empty, got %q", got)
	}
	if got := formatTryOptions(CallOpts{Receipt: true}); got != " <{.receipt: true}>" {
		t.Fatalf("want receipt, got %q", got)
	}
	if got := formatTryOptions(CallOpts{WaitDurableMs: 1000}); got != " <{.wait_durable_ms: 1000}>" {
		t.Fatalf("want wait_durable_ms, got %q", got)
	}
	if got := formatTryOptions(CallOpts{Receipt: true, WaitDurableMs: 500}); got != " <{.receipt: true, .wait_durable_ms: 500}>" {
		t.Fatalf("want both, got %q", got)
	}
}

var upgrader = websocket.Upgrader{}

func newTestServer(t *testing.T, handler func(stmt string) string) (*httptest.Server, *Client) {
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("upgrade: %v", err)
			return
		}
		defer conn.Close()
		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				return
			}
			reply := handler(string(msg))
			if err := conn.WriteMessage(websocket.TextMessage, []byte(reply)); err != nil {
				return
			}
		}
	}))
	wsURL := "ws" + strings.TrimPrefix(s.URL, "http")
	client, err := ConnectURL(wsURL)
	if err != nil {
		t.Fatalf("ConnectURL: %v", err)
	}
	return s, client
}

func TestCallWithReceipt(t *testing.T) {
	var lastStmt string
	server, client := newTestServer(t, func(stmt string) string {
		lastStmt = stmt
		if strings.Contains(stmt, "durable_timeout") {
			return `{"status": "durable_timeout", "result": "timeout", "receipt": {"pov": "p1", "version": 5, "durability": "memory"}}`
		}
		if strings.Contains(stmt, "<{.receipt: true") {
			return `{"status": "ok", "result": true, "receipt": {"pov": "p1", "version": 42, "durability": "memory"}}`
		}
		if strings.Contains(stmt, ".wait_durable_ms: 1000") {
			return `{"status": "ok", "result": "done", "receipt": {"pov": "p1", "version": 43, "durability": "durable"}}`
		}
		return `{"status": "ok", "result": "plain"}`
	})
	defer server.Close()
	defer client.Close()

	// 1. Plain Call
	raw, err := client.Call("p1", "pkg", "fn", map[string]any{"k": 1})
	if err != nil || string(raw) != `"plain"` {
		t.Fatalf("Call: %s, %v", raw, err)
	}
	if client.LastReceipt != nil {
		t.Fatalf("expected nil LastReceipt, got %+v", client.LastReceipt)
	}
	if lastStmt != "try {p1} pkg fn <{.k: 1}>;" {
		t.Fatalf("unexpected stmt: %q", lastStmt)
	}

	// 2. CallWithReceipt
	raw, rcpt, err := client.CallWithReceipt("p1", "pkg", "fn", map[string]any{"k": 1}, CallOpts{})
	if err != nil || string(raw) != `true` {
		t.Fatalf("CallWithReceipt: %s, %v", raw, err)
	}
	if rcpt == nil || rcpt.Version != 42 || rcpt.Durability != "memory" {
		t.Fatalf("unexpected receipt: %+v", rcpt)
	}
	if client.LastReceipt != rcpt {
		t.Fatalf("client.LastReceipt mismatch: %+v != %+v", client.LastReceipt, rcpt)
	}
	if lastStmt != "try {p1} pkg fn <{.k: 1}> <{.receipt: true}>;" {
		t.Fatalf("unexpected stmt: %q", lastStmt)
	}

	// 3. CallWithOpts (wait_durable_ms)
	raw, err = client.CallWithOpts("p1", "pkg", "fn", nil, CallOpts{WaitDurableMs: 1000})
	if err != nil || string(raw) != `"done"` {
		t.Fatalf("CallWithOpts: %s, %v", raw, err)
	}
	if client.LastReceipt == nil || client.LastReceipt.Durability != "durable" {
		t.Fatalf("unexpected LastReceipt: %+v", client.LastReceipt)
	}

	// 4. CallBatchWithReceipt
	raw, rcpt, err = client.CallBatchWithReceipt("p1", "pkg", "fn", []map[string]any{{"k": 1}}, CallOpts{})
	if err != nil || string(raw) != `true` {
		t.Fatalf("CallBatchWithReceipt: %s, %v", raw, err)
	}
	if rcpt == nil || rcpt.Version != 42 {
		t.Fatalf("unexpected batch receipt: %+v", rcpt)
	}

	// 5. DurableTimeout
	_, err = client.CallWithOpts("p1", "durable_timeout", "fn", nil, CallOpts{WaitDurableMs: 10})
	if err == nil || !errors.Is(err, ErrDurableTimeout) {
		t.Fatalf("expected ErrDurableTimeout, got %v", err)
	}
	var dte *DurableTimeoutError
	if !errors.As(err, &dte) {
		t.Fatalf("expected *DurableTimeoutError, got %T", err)
	}
	if dte.Receipt == nil || dte.Receipt.Version != 5 {
		t.Fatalf("unexpected receipt on timeout: %+v", dte.Receipt)
	}
}

func TestDurableVersion(t *testing.T) {
	var queriedPov string
	server, client := newTestServer(t, func(stmt string) string {
		if stmt == "durable_version {p1};" {
			queriedPov = "p1"
			return `{"status": "ok", "result": {"pov": "p1", "durable_version": 99}}`
		}
		if stmt == "durable_version {p2};" {
			queriedPov = "p2"
			return `{"status": "ok", "result": {"pov": "p2", "durable_version": null}}`
		}
		return `{"status": "exception", "result": "bad stmt"}`
	})
	defer server.Close()
	defer client.Close()

	v1, err := client.DurableVersion("p1")
	if err != nil || v1 == nil || *v1 != 99 {
		t.Fatalf("DurableVersion(p1): %v, %v", v1, err)
	}
	if queriedPov != "p1" {
		t.Fatalf("queriedPov: %q", queriedPov)
	}

	v2, err := client.DurableVersion("p2")
	if err != nil || v2 != nil {
		t.Fatalf("DurableVersion(p2): %v, %v", v2, err)
	}
	if queriedPov != "p2" {
		t.Fatalf("queriedPov: %q", queriedPov)
	}
}

func TestOpIDOption(t *testing.T) {
	const id = "0194a2b4-7c00-7000-8000-000000000001"
	if got := formatTryOptions(CallOpts{OpID: id}); got != ` <{.op_id: "`+id+`"}>` {
		t.Fatalf("want op_id, got %q", got)
	}
	if got := formatTryOptions(CallOpts{OpID: id, Receipt: true}); got != ` <{.op_id: "`+id+`", .receipt: true}>` {
		t.Fatalf("want op_id then receipt, got %q", got)
	}
	a, b := NewOpID(), NewOpID()
	if a == b || len(a) != 36 || a[14] != '7' || !strings.ContainsRune("89ab", rune(a[19])) {
		t.Fatalf("not a v7 uuid, or not fresh: %q %q", a, b)
	}
}

func TestOperationStatuses(t *testing.T) {
	for status, want := range map[string]error{
		"operation_expired":   ErrOperationExpired,
		"operation_id_reused": ErrOperationIDReused,
		"operation_in_flight": ErrOperationInFlight,
		"outcome_too_large":   ErrOutcomeTooLarge,
	} {
		server, client := newTestServer(t, func(stmt string) string {
			return `{"status": "` + status + `", "result": "x", "version": 7}`
		})
		_, err := client.CallWithOpts("p1", "pkg", "fn", nil, CallOpts{OpID: NewOpID(), OpRetries: -1})
		if !errors.Is(err, want) {
			t.Fatalf("%s: got %v", status, err)
		}
		var otl *OutcomeTooLargeError
		if status == "outcome_too_large" && (!errors.As(err, &otl) || otl.Version != 7) {
			t.Fatalf("no version: %v", err)
		}
		client.Close()
		server.Close()
	}
}

// A connection that drops is redialled and the write resent with the same id, but only
// when there is an id.
func TestOpIDResendsAfterDrop(t *testing.T) {
	var mu sync.Mutex
	var seen []string
	drops := 1
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				return
			}
			mu.Lock()
			seen = append(seen, string(msg))
			drop := strings.HasPrefix(string(msg), "try") && drops > 0
			if drop {
				drops--
			}
			mu.Unlock()
			if drop {
				return // no reply: the connection just closes
			}
			conn.WriteMessage(websocket.TextMessage, []byte(`{"status": "ok", "result": "done"}`))
		}
	}))
	defer s.Close()
	c, err := ConnectURL("ws" + strings.TrimPrefix(s.URL, "http"))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.sessionID = "s1"
	raw, err := c.CallWithOpts("p1", "pkg", "fn", nil, CallOpts{OpID: NewOpID()})
	if err != nil || string(raw) != `"done"` {
		t.Fatalf("with an id: %s, %v", raw, err)
	}
	mu.Lock()
	if len(seen) != 3 || seen[0] != seen[2] || seen[1] != "resume session {s1};" {
		t.Fatalf("seen: %q", seen)
	}
	drops = 1
	mu.Unlock()
	if _, err := c.Call("p1", "pkg", "fn", nil); err == nil {
		t.Fatalf("without an id the drop must surface")
	}
}

func TestReadAtVersion(t *testing.T) {
	var stmts []string
	server, client := newTestServer(t, func(stmt string) string {
		stmts = append(stmts, stmt)
		switch {
		case strings.Contains(stmt, " at 3 "):
			return `{"status": "version_unavailable", "result": "gone", "requested": 3, "oldest": 10, "newest": 20}`
		case strings.Contains(stmt, " at save "):
			return `{"status": "version_read_only", "result": "no"}`
		case strings.Contains(stmt, " at "):
			return `{"status": "ok", "result": 7, "version": 42}`
		}
		return `{"status": "ok", "result": 1}`
	})
	defer server.Close()
	defer client.Close()

	raw, err := client.CallWithOpts("p1", "pkg", "get", map[string]any{"n": 1}, CallOpts{At: AtVersion(42)})
	if err != nil || string(raw) != "7" || client.LastVersion == nil || *client.LastVersion != 42 {
		t.Fatalf("at version: %s, %v, %v", raw, err, client.LastVersion)
	}
	if _, err = client.CallWithOpts("p1", "pkg", "get", nil, CallOpts{At: AtTime("2026-01-01T00:00:00Z")}); err != nil {
		t.Fatal(err)
	}
	if _, err = client.Call("p1", "pkg", "get", nil); err != nil || client.LastVersion != nil {
		t.Fatalf("plain call: %v, %v", err, client.LastVersion)
	}
	want := []string{
		"try {p1} at 42 pkg get <{.n: 1}>;",
		`try {p1} at time "2026-01-01T00:00:00Z" pkg get <{}>;`,
		"try {p1} pkg get <{}>;",
	}
	for i, w := range want {
		if stmts[i] != w {
			t.Fatalf("statement %d: %q, want %q", i, stmts[i], w)
		}
	}

	_, err = client.CallWithOpts("p1", "pkg", "get", nil, CallOpts{At: AtVersion(3)})
	var vue *VersionUnavailableError
	if !errors.Is(err, ErrVersionUnavailable) || !errors.As(err, &vue) ||
		vue.Requested != 3 || vue.Oldest != 10 || vue.Newest != 20 {
		t.Fatalf("version unavailable: %v", err)
	}
	_, err = client.CallWithOpts("p1", "pkg", "put", nil, CallOpts{At: AtSave(`a"b`)})
	if !errors.Is(err, ErrVersionReadOnly) {
		t.Fatalf("read only: %v", err)
	}
	if _, err = client.CallBatchWithOpts("p1", "pkg", "get", []map[string]any{{}}, CallOpts{At: AtVersion(1)}); err == nil {
		t.Fatal("a batch at a version must be refused")
	}
}

func TestVersionStatements(t *testing.T) {
	var stmts []string
	server, client := newTestServer(t, func(stmt string) string {
		stmts = append(stmts, stmt)
		switch {
		case stmt == "version of {g};":
			return `{"status": "ok", "result": {"pov": "g", "version": 1000}}`
		case stmt == "version of {p};":
			return `{"status": "ok", "result": {"pov": "p", "version": "(3, 1000)"}}`
		case strings.HasPrefix(stmt, "hold"):
			return `{"status": "ok", "result": {"lease": "L", "seconds": 60}}`
		case strings.HasPrefix(stmt, "release"):
			return `{"status": "ok", "result": {"lease": "L", "released": true}}`
		case strings.HasPrefix(stmt, "drop"):
			return `{"status": "ok", "result": {"dropped": false}}`
		}
		return `{"status": "ok", "result": {"save_point": "a"}}`
	})
	defer server.Close()
	defer client.Close()

	if v, err := client.VersionOf("g"); err != nil || v != (PovVersion{Number: 1000}) {
		t.Fatalf("VersionOf(g): %v, %v", v, err)
	}
	if v, err := client.VersionOf("p"); err != nil || v != (PovVersion{Vector: "(3, 1000)"}) {
		t.Fatalf("VersionOf(p): %v, %v", v, err)
	}
	if lease, err := client.Hold("g", 5, 60); err != nil || lease != "L" {
		t.Fatalf("Hold: %q, %v", lease, err)
	}
	if ok, err := client.Release("L"); err != nil || !ok {
		t.Fatalf("Release: %v, %v", ok, err)
	}
	if err := client.SavePoint("g", "a"); err != nil {
		t.Fatal(err)
	}
	if err := client.SavePointAt("g", "a", 7); err != nil {
		t.Fatal(err)
	}
	if ok, err := client.DropSavePoint("g", "a"); err != nil || ok {
		t.Fatalf("DropSavePoint: %v, %v", ok, err)
	}
	want := []string{
		"version of {g};", "version of {p};", "hold version {g} 5 for 60;", "release {L};",
		`save point {g} "a";`, `save point {g} "a" at 7;`, `drop save point {g} "a";`,
	}
	if strings.Join(stmts, "\n") != strings.Join(want, "\n") {
		t.Fatalf("statements:\n%s", strings.Join(stmts, "\n"))
	}
}

func TestPagesPinTheVersion(t *testing.T) {
	var stmts []string
	server, client := newTestServer(t, func(stmt string) string {
		stmts = append(stmts, stmt)
		switch {
		case stmt == "version of {g};":
			return `{"status": "ok", "result": {"pov": "g", "version": 77}}`
		case stmt == "version of {p};":
			return `{"status": "ok", "result": {"pov": "p", "version": "(3, 1000)"}}`
		case strings.Contains(stmt, ".last: 0"):
			return `{"status": "ok", "result": {"rows": [1, 2], "last": 2.0}}`
		}
		return `{"status": "ok", "result": {"rows": [], "last": 2.0}}`
	})
	defer server.Close()
	defer client.Close()

	pages := 0
	count := func([]json.RawMessage) error { pages++; return nil }
	args := func() map[string]any { return map[string]any{"last": 0} }
	if err := client.Pages("g", "pkg", "list", args(), PageOptions{}, count); err != nil || pages != 1 {
		t.Fatalf("Pages: %d, %v", pages, err)
	}
	want := []string{
		"version of {g};", "try {g} at 77 pkg list <{.last: 0}>;", "try {g} at 77 pkg list <{.last: 2}>;",
	}
	if strings.Join(stmts, "\n") != strings.Join(want, "\n") {
		t.Fatalf("pinned:\n%s", strings.Join(stmts, "\n"))
	}

	stmts = nil
	client.Pages("p", "pkg", "list", args(), PageOptions{PageSize: 5}, count)
	client.Pages("g", "pkg", "list", args(), PageOptions{PageSize: 5, NoPin: true}, count)
	client.Pages("g", "pkg", "list", args(), PageOptions{PageSize: 5, At: AtSave("s")}, count)
	want = []string{
		"version of {p};", "try {p} pkg list <{.last: 0}>;",
		"try {g} pkg list <{.last: 0}>;",
		`try {g} at save "s" pkg list <{.last: 0}>;`,
	}
	if strings.Join(stmts, "\n") != strings.Join(want, "\n") {
		t.Fatalf("unpinned:\n%s", strings.Join(stmts, "\n"))
	}
}

func TestWholeNumber(t *testing.T) {
	var b struct{ V wholeNumber }
	for _, in := range []string{`{"V":7}`, `{"V":7.0}`, `{"V":4.000000}`} {
		if err := json.Unmarshal([]byte(in), &b); err != nil || b.V != wholeNumber(map[string]uint64{`{"V":7}`: 7, `{"V":7.0}`: 7, `{"V":4.000000}`: 4}[in]) {
			t.Fatalf("%s: %v, %d", in, err, b.V)
		}
	}
	if json.Unmarshal([]byte(`{"V":1.5}`), &b) == nil || json.Unmarshal([]byte(`{"V":-1}`), &b) == nil {
		t.Fatal("a fraction or a negative is not a version")
	}
}
