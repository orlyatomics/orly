/* <orly/lsp/server.cc>

   Implements <orly/lsp/server.h> (#739).

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

#include <algorithm>
#include <cctype>
#include <climits>
#include <sstream>

#include <base/as_str.h>
#include <orly/expr/expr.h>
#include <orly/expr/function_app.h>
#include <orly/expr/ref.h>
#include <orly/expr/util.h>
#include <orly/expr/walker.h>
#include <orly/symbol/function.h>
#include <orly/symbol/result_def.h>
#include <orly/synth/context.h>
#include <orly/type/type_czar.h>

namespace Orly::Lsp {

  namespace {

    /* Check whether 1-indexed (line, col) is within Tools::Nycr::TPosRange. */
    bool ContainsPos(const Tools::Nycr::TPosRange &range, uint32_t line, uint32_t col) noexcept {
      const auto &start = range.GetStart();
      const auto &limit = range.GetLimit();
      if (line < static_cast<uint32_t>(start.GetLineNumber()) || line > static_cast<uint32_t>(limit.GetLineNumber())) {
        return false;
      }
      if (line == static_cast<uint32_t>(start.GetLineNumber()) && col < static_cast<uint32_t>(start.GetColumnNumber())) {
        return false;
      }
      if (line == static_cast<uint32_t>(limit.GetLineNumber()) && col > static_cast<uint32_t>(limit.GetColumnNumber())) {
        return false;
      }
      return true;
    }

    /* Extract the word under cursor (0-indexed line, col) from the full document text. */
    std::string GetWordAtPosition(const std::string &text, uint32_t line, uint32_t col) {
      std::istringstream stream(text);
      std::string current_line;
      uint32_t current_line_num = 0;
      while (std::getline(stream, current_line)) {
        if (current_line_num == line) {
          if (col > current_line.size()) {
            return "";
          }
          size_t start = col;
          while (start > 0 && (std::isalnum(current_line[start - 1]) || current_line[start - 1] == '_')) {
            --start;
          }
          size_t end = col;
          while (end < current_line.size() && (std::isalnum(current_line[end]) || current_line[end] == '_')) {
            ++end;
          }
          if (start < end) {
            return current_line.substr(start, end - start);
          }
          return "";
        }
        ++current_line_num;
      }
      return "";
    }

    /* Standard built-in keyword documentation strings. */
    std::string GetKeywordDocumentation(const std::string &word) {
      if (word == "keys") return "**keys** `(addr / dict)`\n\nExtracts all keys from a dictionary, index, or storage address.";
      if (word == "take") return "**take** `n`\n\nTakes the first `n` elements from a sequence, list, or stream.";
      if (word == "skip") return "**skip** `n`\n\nSkips the first `n` elements from a sequence, list, or stream.";
      if (word == "reduce") return "**reduce** `(start: expr) that ...`\n\nFolds elements of a collection into a single value using an accumulator.";
      if (word == "sorted_by") return "**sorted_by** `that ...`\n\nSorts a collection based on a key projection function.";
      if (word == "reversed") return "**reversed**\n\nReverses the ordering of a collection.";
      if (word == "filter") return "**filter** `that ...`\n\nFilters collection elements satisfying a boolean predicate.";
      if (word == "given") return "**given** `{ param is type; ... }`\n\nDeclares required input parameters for a package or function.";
      if (word == "where") return "**where** `{ defs... }`\n\nIntroduces local parameter definitions and helper bindings.";
      if (word == "effecting") return "`(result)` **effecting** `{ mutations... }`\n\nExecutes transactional side-effects and returns a result.";
      if (word == "assert") return "**assert** `{ that condition; }`\n\nEvaluates runtime assertions on an expression.";
      if (word == "empty") return "**empty** `[type]` / `{type}` / `{k: v}`\n\nConstructs an empty list, set, or dictionary.";
      if (word == "opt") return "**opt** `type`\n\nSpecifies an optional (nullable) type.";
      if (word == "package") return "**package** `#version;`\n\nDeclares the package version number and namespace.";
      if (word == "test") return "**test** `{ t1: expr == expected; ... }`\n\nDeclares unit test assertions for the package.";
      if (word == "collated_by") return "**collated_by** `(start: expr) that ...`\n\nGroups and folds values by key.";
      if (word == "split") return "**split** `expr`\n\nSplits a string by a delimiter pattern into a list of strings.";
      if (word == "try") return "**try** `expr else fallback`\n\nAttempts an expression, returning fallback on failure.";
      if (word == "unknown") return "**unknown** `(type)`\n\nRepresents an uninitialized or unknown value of a type.";
      if (word == "mut") return "**mut** `addr`\n\nDeclares a mutable address for write operations.";
      if (word == "read") return "**read** `*<[addr]>::(type)`\n\nReads data stored at the given address.";
      if (word == "time_pnt") return "**time_pnt**\n\nPrimitive timestamp type.";
      if (word == "time_diff") return "**time_diff**\n\nPrimitive duration/interval type.";
      return "";
    }

  }  // namespace

  TServer::TServer()
      : TypeCzar(std::make_unique<Type::TTypeCzar>()) {}

  TServer::~TServer() = default;

  void TServer::OpenDocument(const std::string &uri, const std::string &text, int version) {
    auto &doc = Documents[uri];
    doc.Uri = uri;
    doc.Text = text;
    doc.Version = version;
    AnalyzeDocument(doc);
  }

  void TServer::UpdateDocument(const std::string &uri, const std::string &text, int version) {
    auto &doc = Documents[uri];
    doc.Uri = uri;
    doc.Text = text;
    doc.Version = version;
    AnalyzeDocument(doc);
  }

  void TServer::CloseDocument(const std::string &uri) {
    Documents.erase(uri);
  }

  const TDocument *TServer::GetDocument(const std::string &uri) const {
    const auto it = Documents.find(uri);
    return it != Documents.end() ? &it->second : nullptr;
  }

  std::vector<TDiagnostic> TServer::GetDiagnostics(const std::string &uri) const {
    const auto *doc = GetDocument(uri);
    return doc ? doc->Diagnostics : std::vector<TDiagnostic>{};
  }

  void TServer::AnalyzeDocument(TDocument &doc) {
    doc.Diagnostics.clear();
    doc.Synth.reset();

    Synth::GetContext().ClearErrors();
    doc.Cst = Package::Syntax::TPackage::ParseStr(doc.Text.c_str());

    if (doc.Cst.HasErrors()) {
      doc.Cst.ForEachError([&](const Tools::Nycr::TPosRange &pos, const std::string &msg) {
        doc.Diagnostics.push_back(TDiagnostic{
          .Range = TRange::FromNycr(pos),
          .Severity = TDiagnosticSeverity::Error,
          .Source = "orlyc",
          .Message = msg
        });
        return true;
      });
      return;
    }

    Package::TName pkg_name{{"main"}};
    try {
      doc.Synth = std::make_unique<Synth::TPackage>(pkg_name, &*doc.Cst.Get(), false);
      if (!Synth::GetContext().HasErrors() && doc.Synth->HasSymbol()) {
        doc.Synth->GetSymbol()->TypeCheck();
      }
    } catch (const std::exception &ex) {
      doc.Diagnostics.push_back(TDiagnostic{
        .Range = TRange{TPosition{0, 0}, TPosition{0, 0}},
        .Severity = TDiagnosticSeverity::Error,
        .Source = "orlyc",
        .Message = ex.what()
      });
    }

    Synth::GetContext().ForEachError([&](const Tools::Nycr::TPosRange &pos, const std::string &msg) {
      doc.Diagnostics.push_back(TDiagnostic{
        .Range = TRange::FromNycr(pos),
        .Severity = TDiagnosticSeverity::Error,
        .Source = "orlyc",
        .Message = msg
      });
      return true;
    });
  }

  std::optional<THover> TServer::GetHover(const std::string &uri, const TPosition &pos) const {
    const auto *doc = GetDocument(uri);
    if (!doc) {
      return std::nullopt;
    }

    const uint32_t line_1 = pos.Line + 1;
    const uint32_t col_1 = pos.Character + 1;
    const std::string word = GetWordAtPosition(doc->Text, pos.Line, pos.Character);

    if (doc->Synth && doc->Synth->HasSymbol()) {
      for (const auto &func : doc->Synth->GetSymbol()->GetFunctions()) {
        // 1. Check parameter definitions
        for (const auto &param : func->GetParamDefs()) {
          bool is_param = ContainsPos(param->GetPosRange(), line_1, col_1);
          if (!is_param && !word.empty() && word == param->GetName() &&
              line_1 == static_cast<uint32_t>(param->GetPosRange().GetStart().GetLineNumber())) {
            is_param = true;
          }
          if (is_param) {
            const std::string md = "```orlyscript\n(parameter) " + param->GetName() + ": " + Base::AsStr(param->GetType()) + "\n```";
            return THover{ .Markdown = md, .Range = TRange::FromNycr(param->GetPosRange()) };
          }
        }

        // 2. Check expressions inside function body
        if (func->GetExpr() && ContainsPos(func->GetPosRange(), line_1, col_1)) {
          Expr::TExpr::TPtr best_expr = nullptr;
          size_t best_span = SIZE_MAX;

          Expr::ForEachExpr(func->GetExpr(), [&](const Expr::TExpr::TPtr &expr) {
            const auto &pr = expr->GetPosRange();
            if (ContainsPos(pr, line_1, col_1)) {
              size_t span = static_cast<size_t>(pr.GetLimit().GetLineNumber() - pr.GetStart().GetLineNumber()) * 10000 +
                            static_cast<size_t>(pr.GetLimit().GetColumnNumber() - pr.GetStart().GetColumnNumber());
              if (span <= best_span) {
                best_span = span;
                best_expr = expr;
              }
            }
            return false;
          }, true);

          if (best_expr) {
            // Check if variable reference
            if (const auto *ref = best_expr->TryAs<Expr::TRef>()) {
              if (const auto *def = ref->GetDef()) {
                const std::string md = "```orlyscript\n" + def->GetName() + ": " + Base::AsStr(def->GetType()) + "\n```";
                return THover{ .Markdown = md, .Range = TRange::FromNycr(best_expr->GetPosRange()) };
              }
            }

            // Check if function call
            if (const auto *app = best_expr->TryAs<Expr::TFunctionApp>()) {
              if (const auto any_fn = app->GetFunction()) {
                const std::string md = "```orlyscript\n(function) " + any_fn->GetName() + "(...) -> " + Base::AsStr(any_fn->GetReturnType()) + "\n```";
                return THover{ .Markdown = md, .Range = TRange::FromNycr(best_expr->GetPosRange()) };
              }
            }

            // General expression type
            std::string type_str;
            try {
              type_str = Base::AsStr(best_expr->GetType());
            } catch (...) {
            }
            if (!type_str.empty()) {
              const std::string md = "```orlyscript\ntype: " + type_str + "\n```";
              return THover{ .Markdown = md, .Range = TRange::FromNycr(best_expr->GetPosRange()) };
            }
          }
        }

        // 3. Check function signature hover
        if (ContainsPos(func->GetPosRange(), line_1, col_1)) {
          std::string sig = func->GetName() + "(";
          bool first = true;
          for (const auto &p : func->GetParamDefs()) {
            if (!first) sig += ", ";
            first = false;
            sig += p->GetName() + ": " + Base::AsStr(p->GetType());
          }
          sig += ") -> " + Base::AsStr(func->GetReturnType());
          return THover{ .Markdown = "```orlyscript\n(function) " + sig + "\n```", .Range = TRange::FromNycr(func->GetPosRange()) };
        }
      }
    }

    // Keyword hover fallback
    if (!word.empty()) {
      const std::string doc_str = GetKeywordDocumentation(word);
      if (!doc_str.empty()) {
        return THover{ .Markdown = doc_str, .Range = std::nullopt };
      }
    }

    return std::nullopt;
  }

  std::vector<TLocation> TServer::GetDefinition(const std::string &uri, const TPosition &pos) const {
    const auto *doc = GetDocument(uri);
    if (!doc || !doc->Synth || !doc->Synth->HasSymbol()) {
      return {};
    }

    const uint32_t line_1 = pos.Line + 1;
    const uint32_t col_1 = pos.Character + 1;

    for (const auto &func : doc->Synth->GetSymbol()->GetFunctions()) {
      if (!func->GetExpr() || !ContainsPos(func->GetPosRange(), line_1, col_1)) {
        continue;
      }

      Expr::TExpr::TPtr best_expr = nullptr;
      size_t best_span = SIZE_MAX;

      Expr::ForEachExpr(func->GetExpr(), [&](const Expr::TExpr::TPtr &expr) {
        const auto &pr = expr->GetPosRange();
        if (ContainsPos(pr, line_1, col_1)) {
          size_t span = static_cast<size_t>(pr.GetLimit().GetLineNumber() - pr.GetStart().GetLineNumber()) * 10000 +
                        static_cast<size_t>(pr.GetLimit().GetColumnNumber() - pr.GetStart().GetColumnNumber());
          if (span <= best_span) {
            best_span = span;
            best_expr = expr;
          }
        }
        return false;
      }, true);

      if (best_expr) {
        if (const auto *ref = best_expr->TryAs<Expr::TRef>()) {
          if (const auto *def = ref->GetDef()) {
            return { TLocation{ .Uri = uri, .Range = TRange::FromNycr(def->GetPosRange()) } };
          }
        }
        if (const auto *app = best_expr->TryAs<Expr::TFunctionApp>()) {
          if (const auto any_fn = app->GetFunction()) {
            if (const auto *target_fn = dynamic_cast<const Symbol::TFunction *>(any_fn.get())) {
              return { TLocation{ .Uri = uri, .Range = TRange::FromNycr(target_fn->GetPosRange()) } };
            }
          }
        }
      }
    }

    // Name-based definition lookup fallback
    const std::string word = GetWordAtPosition(doc->Text, pos.Line, pos.Character);
    if (!word.empty()) {
      for (const auto &func : doc->Synth->GetSymbol()->GetFunctions()) {
        if (func->GetName() == word) {
          return { TLocation{ .Uri = uri, .Range = TRange::FromNycr(func->GetPosRange()) } };
        }
      }
    }

    return {};
  }

  std::vector<TDocumentSymbol> TServer::GetDocumentSymbols(const std::string &uri) const {
    const auto *doc = GetDocument(uri);
    if (!doc || !doc->Synth || !doc->Synth->HasSymbol()) {
      return {};
    }

    std::vector<TDocumentSymbol> symbols;

    // Functions
    for (const auto &func : doc->Synth->GetSymbol()->GetFunctions()) {
      TDocumentSymbol sym;
      sym.Name = func->GetName();
      sym.Kind = TSymbolKind::Function;
      sym.Range = TRange::FromNycr(func->GetPosRange());
      sym.SelectionRange = sym.Range;

      std::string sig = "(";
      bool first = true;
      for (const auto &p : func->GetParamDefs()) {
        if (!first) sig += ", ";
        first = false;
        sig += p->GetName() + ": " + Base::AsStr(p->GetType());

        TDocumentSymbol p_sym;
        p_sym.Name = p->GetName();
        p_sym.Detail = Base::AsStr(p->GetType());
        p_sym.Kind = TSymbolKind::Variable;
        p_sym.Range = TRange::FromNycr(p->GetPosRange());
        p_sym.SelectionRange = p_sym.Range;
        sym.Children.push_back(p_sym);
      }
      sig += ") -> " + Base::AsStr(func->GetReturnType());
      sym.Detail = sig;

      symbols.push_back(sym);
    }

    // Tests
    for (size_t i = 0; i < doc->Synth->GetSymbol()->GetTests().size(); ++i) {
      TDocumentSymbol test_sym;
      test_sym.Name = "test";
      test_sym.Detail = "test block";
      test_sym.Kind = TSymbolKind::Method;
      symbols.push_back(test_sym);
    }

    return symbols;
  }

  std::vector<TCompletionItem> TServer::GetCompletions(const std::string &uri, const TPosition &/*pos*/) const {
    std::vector<TCompletionItem> items;

    // 1. Language Keywords
    static const std::vector<std::string> Keywords = {
      "as", "assert", "collated_by", "collected_by", "delete", "effecting", "else", "else_if",
      "empty", "exists", "free", "from", "given", "having", "if", "in", "is", "keys", "known",
      "mut", "mutate", "new", "not", "opt", "package", "read", "reduce", "reversed", "skip",
      "sorted_by", "split", "start", "take", "test", "that", "to", "true", "false", "try",
      "unknown", "where", "while", "xor"
    };

    for (const auto &kw : Keywords) {
      items.push_back(TCompletionItem{
        .Label = kw,
        .Kind = TCompletionItemKind::Keyword,
        .Detail = "keyword",
        .Documentation = GetKeywordDocumentation(kw),
        .InsertText = ""
      });
    }

    // 2. Built-in Types
    static const std::vector<std::string> Types = {
      "bool", "int", "real", "str", "time_pnt", "time_diff", "id"
    };

    for (const auto &t : Types) {
      items.push_back(TCompletionItem{
        .Label = t,
        .Kind = TCompletionItemKind::Class,
        .Detail = "type",
        .Documentation = "",
        .InsertText = ""
      });
    }

    // 3. Document symbols in scope
    if (const auto *doc = GetDocument(uri)) {
      if (doc->Synth && doc->Synth->HasSymbol()) {
        for (const auto &func : doc->Synth->GetSymbol()->GetFunctions()) {
          items.push_back(TCompletionItem{
            .Label = func->GetName(),
            .Kind = TCompletionItemKind::Function,
            .Detail = "(function) -> " + Base::AsStr(func->GetReturnType()),
            .Documentation = "",
            .InsertText = ""
          });
          for (const auto &p : func->GetParamDefs()) {
            items.push_back(TCompletionItem{
              .Label = p->GetName(),
              .Kind = TCompletionItemKind::Variable,
              .Detail = "(parameter) " + Base::AsStr(p->GetType()),
              .Documentation = "",
              .InsertText = ""
            });
          }
        }
      }
    }

    return items;
  }

  void TServer::SendNotification(const std::string &method, const Base::TJson &params, std::ostream &out) {
    Base::TJson notif = Base::TJson::Object;
    notif["jsonrpc"] = "2.0";
    notif["method"] = method;
    notif["params"] = params;

    const std::string payload = Base::AsStr(notif);
    out << "Content-Length: " << payload.size() << "\r\n\r\n" << payload << std::flush;
  }

  void TServer::SendResponse(const Base::TJson &id, const Base::TJson &result, std::ostream &out) {
    Base::TJson resp = Base::TJson::Object;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;

    const std::string payload = Base::AsStr(resp);
    out << "Content-Length: " << payload.size() << "\r\n\r\n" << payload << std::flush;
  }

  void TServer::SendError(const Base::TJson &id, int code, const std::string &message, std::ostream &out) {
    Base::TJson resp = Base::TJson::Object;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;

    Base::TJson err = Base::TJson::Object;
    err["code"] = static_cast<double>(code);
    err["message"] = message;
    resp["error"] = err;

    const std::string payload = Base::AsStr(resp);
    out << "Content-Length: " << payload.size() << "\r\n\r\n" << payload << std::flush;
  }

  bool TServer::HandleMessage(const Base::TJson &msg, std::ostream &out) {
    if (msg.GetKind() != Base::TJson::Object) {
      return true;
    }

    const auto *method_node = msg.TryFind("method");
    const auto *id_node = msg.TryFind("id");
    const auto *params_node = msg.TryFind("params");

    if (!method_node) {
      // Response from client, ignore
      return true;
    }

    const std::string method = method_node->GetString();

    if (method == "initialize") {
      Base::TJson res = Base::TJson::Object;
      Base::TJson caps = Base::TJson::Object;
      caps["textDocumentSync"] = 1.0;  // Full document sync
      caps["hoverProvider"] = true;
      caps["definitionProvider"] = true;
      caps["documentSymbolProvider"] = true;

      Base::TJson comp = Base::TJson::Object;
      Base::TJson::TArray triggers = { ".", "(", ":", " " };
      comp["triggerCharacters"] = Base::TJson(triggers);
      caps["completionProvider"] = comp;

      res["capabilities"] = caps;

      Base::TJson server_info = Base::TJson::Object;
      server_info["name"] = "orly-lsp";
      server_info["version"] = "0.1.0";
      res["serverInfo"] = server_info;

      if (id_node) {
        SendResponse(*id_node, res, out);
      }
      return true;
    }

    if (method == "initialized") {
      return true;
    }

    if (method == "shutdown") {
      ShutdownRequested = true;
      if (id_node) {
        SendResponse(*id_node, Base::TJson(), out);
      }
      return true;
    }

    if (method == "exit") {
      return false;
    }

    if (method == "textDocument/didOpen") {
      if (params_node) {
        if (const auto *td = params_node->TryFind("textDocument")) {
          const std::string uri = (*td)["uri"].GetString();
          const std::string text = (*td)["text"].GetString();
          const int version = static_cast<int>((*td)["version"].GetNumber());
          OpenDocument(uri, text, version);

          Base::TJson p = Base::TJson::Object;
          p["uri"] = uri;
          Base::TJson::TArray diag_arr;
          for (const auto &d : GetDiagnostics(uri)) {
            diag_arr.push_back(d.ToJson());
          }
          p["diagnostics"] = Base::TJson(diag_arr);
          SendNotification("textDocument/publishDiagnostics", p, out);
        }
      }
      return true;
    }

    if (method == "textDocument/didChange") {
      if (params_node) {
        if (const auto *td = params_node->TryFind("textDocument")) {
          const std::string uri = (*td)["uri"].GetString();
          const int version = static_cast<int>((*td)["version"].GetNumber());
          if (const auto *changes = params_node->TryFind("contentChanges")) {
            if (changes->GetSize() > 0) {
              const std::string text = (*changes)[0]["text"].GetString();
              UpdateDocument(uri, text, version);

              Base::TJson p = Base::TJson::Object;
              p["uri"] = uri;
              Base::TJson::TArray diag_arr;
              for (const auto &d : GetDiagnostics(uri)) {
                diag_arr.push_back(d.ToJson());
              }
              p["diagnostics"] = Base::TJson(diag_arr);
              SendNotification("textDocument/publishDiagnostics", p, out);
            }
          }
        }
      }
      return true;
    }

    if (method == "textDocument/didClose") {
      if (params_node) {
        if (const auto *td = params_node->TryFind("textDocument")) {
          CloseDocument((*td)["uri"].GetString());
        }
      }
      return true;
    }

    if (method == "textDocument/hover") {
      if (id_node && params_node) {
        const std::string uri = (*params_node)["textDocument"]["uri"].GetString();
        const auto pos = TPosition::FromJson((*params_node)["position"]);
        const auto hover = GetHover(uri, pos);
        SendResponse(*id_node, hover ? hover->ToJson() : Base::TJson(), out);
      }
      return true;
    }

    if (method == "textDocument/definition") {
      if (id_node && params_node) {
        const std::string uri = (*params_node)["textDocument"]["uri"].GetString();
        const auto pos = TPosition::FromJson((*params_node)["position"]);
        const auto defs = GetDefinition(uri, pos);
        if (!defs.empty()) {
          Base::TJson::TArray arr;
          for (const auto &loc : defs) {
            arr.push_back(loc.ToJson());
          }
          SendResponse(*id_node, Base::TJson(arr), out);
        } else {
          SendResponse(*id_node, Base::TJson(), out);
        }
      }
      return true;
    }

    if (method == "textDocument/documentSymbol") {
      if (id_node && params_node) {
        const std::string uri = (*params_node)["textDocument"]["uri"].GetString();
        const auto syms = GetDocumentSymbols(uri);
        Base::TJson::TArray arr;
        for (const auto &s : syms) {
          arr.push_back(s.ToJson());
        }
        SendResponse(*id_node, Base::TJson(arr), out);
      }
      return true;
    }

    if (method == "textDocument/completion") {
      if (id_node && params_node) {
        const std::string uri = (*params_node)["textDocument"]["uri"].GetString();
        const auto pos = TPosition::FromJson((*params_node)["position"]);
        const auto items = GetCompletions(uri, pos);
        Base::TJson::TArray arr;
        for (const auto &it : items) {
          arr.push_back(it.ToJson());
        }
        SendResponse(*id_node, Base::TJson(arr), out);
      }
      return true;
    }

    // Method not found
    if (id_node) {
      SendError(*id_node, -32601, "Method not found: " + method, out);
    }

    return true;
  }

  void TServer::Run(std::istream &in, std::ostream &out) {
    while (in) {
      size_t content_length = 0;
      std::string line;

      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (line.empty()) {
          break;
        }
        if (line.rfind("Content-Length: ", 0) == 0) {
          content_length = std::stoull(line.substr(16));
        }
      }

      if (!in || content_length == 0) {
        break;
      }

      std::string body(content_length, '\0');
      in.read(&body[0], content_length);

      try {
        const Base::TJson msg = Base::TJson::Parse(body);
        if (!HandleMessage(msg, out)) {
          break;
        }
      } catch (const std::exception &ex) {
        std::cerr << "orly-lsp: error processing message: " << ex.what() << std::endl;
      }
    }
  }

}  // namespace Orly::Lsp
