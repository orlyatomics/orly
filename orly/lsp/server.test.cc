/* <orly/lsp/server.test.cc>

   Unit tests for Orlyscript Language Server (orly-lsp) (#739).

   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <orly/lsp/server.h>

#include <fstream>
#include <sstream>
#include <string>

#include <base/as_str.h>
#include <base/test/kit.h>

using namespace std;
using namespace Orly::Lsp;

/* 1. Test server lifecycle: initialize, shutdown, exit */
FIXTURE(Lifecycle) {
  TServer server;
  ostringstream out;

  // initialize request
  Base::TJson init_req = Base::TJson::Object;
  init_req["jsonrpc"] = "2.0";
  init_req["id"] = 1.0;
  init_req["method"] = "initialize";
  init_req["params"] = Base::TJson::Object;

  EXPECT_TRUE(server.HandleMessage(init_req, out));
  const string out_str = out.str();
  EXPECT_TRUE(out_str.find("capabilities") != string::npos);
  EXPECT_TRUE(out_str.find("hoverProvider") != string::npos);
  EXPECT_TRUE(out_str.find("definitionProvider") != string::npos);

  // shutdown request
  out.str("");
  Base::TJson shut_req = Base::TJson::Object;
  shut_req["jsonrpc"] = "2.0";
  shut_req["id"] = 2.0;
  shut_req["method"] = "shutdown";

  EXPECT_TRUE(server.HandleMessage(shut_req, out));
  EXPECT_TRUE(out.str().find("\"result\":null") != string::npos || out.str().find("\"result\": null") != string::npos);

  // exit request
  out.str("");
  Base::TJson exit_req = Base::TJson::Object;
  exit_req["jsonrpc"] = "2.0";
  exit_req["method"] = "exit";

  EXPECT_FALSE(server.HandleMessage(exit_req, out));
}

/* 2. Valid document has zero diagnostics */
FIXTURE(ValidDocumentDiagnostics) {
  TServer server;
  const string uri = "file:///test/valid.orly";
  const string text = "package #1;\nx = 1 + 2;\n";

  server.OpenDocument(uri, text, 1);
  const auto diags = server.GetDiagnostics(uri);
  EXPECT_EQ(diags.size(), 0UL);
}

/* 3. Syntax errors produce diagnostics with source position */
FIXTURE(SyntaxErrorDiagnostics) {
  TServer server;
  const string uri = "file:///test/syntax_err.orly";
  const string text = "package #1;\nx = 1 + ;\n";

  server.OpenDocument(uri, text, 1);
  const auto diags = server.GetDiagnostics(uri);
  EXPECT_GT(diags.size(), 0UL);
  EXPECT_EQ(diags[0].Severity, TDiagnosticSeverity::Error);
  EXPECT_EQ(diags[0].Source, "orlyc");
}

/* 4. Type errors produce diagnostics */
FIXTURE(TypeErrorDiagnostics) {
  TServer server;
  const string uri = "file:///test/type_err.orly";
  const string text = "package #1;\nx = 1 + \"hello\";\n";

  server.OpenDocument(uri, text, 1);
  const auto diags = server.GetDiagnostics(uri);
  EXPECT_GT(diags.size(), 0UL);
  EXPECT_EQ(diags[0].Severity, TDiagnosticSeverity::Error);
}

/* 5. Hover on functions, parameters, and keywords */
FIXTURE(Hover) {
  TServer server;
  const string uri = "file:///test/hover.orly";
  const string text =
      "package #1;\n"
      "add = ((x + y)) where {\n"
      "  x = given::(int);\n"
      "  y = given::(int);\n"
      "};\n";

  server.OpenDocument(uri, text, 1);
  EXPECT_EQ(server.GetDiagnostics(uri).size(), 0UL);

  // Hover on function name 'add' (line 1, col 1)
  auto h_fn = server.GetHover(uri, TPosition{1, 1});
  EXPECT_TRUE(h_fn.has_value());
  EXPECT_TRUE(h_fn->Markdown.find("add") != string::npos);
  EXPECT_TRUE(h_fn->Markdown.find("int") != string::npos);

  // Hover on parameter 'x' (line 2, col 3)
  auto h_param = server.GetHover(uri, TPosition{2, 3});
  EXPECT_TRUE(h_param.has_value());
  EXPECT_TRUE(h_param->Markdown.find("x") != string::npos);
  EXPECT_TRUE(h_param->Markdown.find("int") != string::npos);

  // Keyword hover for 'keys'
  auto h_kw = server.GetHover(uri, TPosition{0, 2}); // 'package' keyword
  EXPECT_TRUE(h_kw.has_value());
  EXPECT_TRUE(h_kw->Markdown.find("package") != string::npos);
}

/* 6. Go to Definition on parameter reference */
FIXTURE(GoToDefinition) {
  TServer server;
  const string uri = "file:///test/def.orly";
  const string text =
      "package #1;\n"
      "double = ((x * 2)) where {\n"
      "  x = given::(int);\n"
      "};\n";

  server.OpenDocument(uri, text, 1);
  EXPECT_EQ(server.GetDiagnostics(uri).size(), 0UL);

  // Go to definition of 'x' in expression (x * 2) at line 1, col 11
  auto defs = server.GetDefinition(uri, TPosition{1, 11});
  EXPECT_EQ(defs.size(), 1UL);
  EXPECT_EQ(defs[0].Uri, uri);
  // Definition should point to line 2 where 'x = given::(int);' is declared
  EXPECT_EQ(defs[0].Range.Start.Line, 2U);
}

/* 7. Document symbols */
FIXTURE(DocumentSymbols) {
  TServer server;
  const string uri = "file:///test/symbols.orly";
  const string text =
      "package #1;\n"
      "inc = ((x + 1)) where {\n"
      "  x = given::(int);\n"
      "};\n"
      "val = 42;\n"
      "test {\n"
      "  t1: inc(.x: 1) == 2;\n"
      "};\n";

  server.OpenDocument(uri, text, 1);
  EXPECT_EQ(server.GetDiagnostics(uri).size(), 0UL);

  auto syms = server.GetDocumentSymbols(uri);
  EXPECT_GE(syms.size(), 2UL);

  bool found_inc = false;
  bool found_val = false;
  bool found_test = false;

  for (const auto &s : syms) {
    if (s.Name == "inc") {
      found_inc = true;
      EXPECT_EQ(s.Kind, TSymbolKind::Function);
      EXPECT_EQ(s.Children.size(), 1UL);
      EXPECT_EQ(s.Children[0].Name, "x");
    } else if (s.Name == "val") {
      found_val = true;
      EXPECT_EQ(s.Kind, TSymbolKind::Function);
    } else if (s.Name == "test") {
      found_test = true;
      EXPECT_EQ(s.Kind, TSymbolKind::Method);
    }
  }

  EXPECT_TRUE(found_inc);
  EXPECT_TRUE(found_val);
  EXPECT_TRUE(found_test);
}

/* 8. Completions */
FIXTURE(Completions) {
  TServer server;
  const string uri = "file:///test/comp.orly";
  const string text = "package #1;\nfoo = 10;\n";

  server.OpenDocument(uri, text, 1);
  auto items = server.GetCompletions(uri, TPosition{1, 0});
  EXPECT_GT(items.size(), 40UL);

  bool found_given = false;
  bool found_int = false;
  bool found_foo = false;

  for (const auto &item : items) {
    if (item.Label == "given") found_given = true;
    if (item.Label == "int") found_int = true;
    if (item.Label == "foo") found_foo = true;
  }

  EXPECT_TRUE(found_given);
  EXPECT_TRUE(found_int);
  EXPECT_TRUE(found_foo);
}

/* 9. Full JSON-RPC stdio transport loop */
FIXTURE(JsonRpcTransport) {
  TServer server;

  string request_body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
  string input = "Content-Length: " + to_string(request_body.size()) + "\r\n\r\n" + request_body;

  string exit_body = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";
  input += "Content-Length: " + to_string(exit_body.size()) + "\r\n\r\n" + exit_body;

  istringstream in(input);
  ostringstream out;

  server.Run(in, out);

  const string output = out.str();
  EXPECT_TRUE(output.find("Content-Length:") != string::npos);
  EXPECT_TRUE(output.find("capabilities") != string::npos);
}

/* 10. Lang tests corpus: sample files open with no spurious diagnostics, and seeded errors produce diagnostics */
FIXTURE(LangTestsCorpus) {
  TServer server;
  const std::vector<std::string> sample_files = {
    "tests/lang_tests/samples/fizzbuzz.orly",
    "tests/lang_tests/samples/reduce.orly",
    "tests/lang_tests/samples/sort.orly",
    "tests/lang_tests/samples/unittest.orly",
    "tests/lang_tests/samples/collections.orly"
  };

  size_t tested_count = 0;
  for (const auto &rel_path : sample_files) {
    std::ifstream file(rel_path);
    if (!file.is_open()) continue;
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    const std::string uri = "file:///" + rel_path;

    server.OpenDocument(uri, text, 1);
    const auto diags = server.GetDiagnostics(uri);
    EXPECT_EQ(diags.size(), 0UL);
    ++tested_count;
  }
  EXPECT_GT(tested_count, 0UL);

  // Seeded error in fizzbuzz
  std::ifstream fb_file("tests/lang_tests/samples/fizzbuzz.orly");
  if (fb_file.is_open()) {
    std::stringstream buffer;
    buffer << fb_file.rdbuf();
    std::string text = buffer.str();
    // Introduce a type error: replace 'x % 15' with 'x % "error"'
    size_t pos = text.find("x % 15");
    if (pos != std::string::npos) {
      text.replace(pos, 6, "x % \"error\"");
      const std::string uri = "file:///tests/lang_tests/samples/fizzbuzz_err.orly";
      server.OpenDocument(uri, text, 1);
      const auto diags = server.GetDiagnostics(uri);
      EXPECT_GT(diags.size(), 0UL);
      EXPECT_EQ(diags[0].Severity, TDiagnosticSeverity::Error);
      EXPECT_EQ(diags[0].Source, "orlyc");
    }
  }
}
