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
	"errors"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
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
