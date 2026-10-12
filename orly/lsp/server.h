/* <orly/lsp/server.h>

   LSP (Language Server Protocol) server implementation for Orlyscript (#739).

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

#pragma once

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/json.h>
#include <orly/lsp/protocol.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/package.h>

namespace Orly {
  namespace Type {
    class TTypeCzar;
  }
}

namespace Orly::Lsp {

  /* In-memory representation of an open orlyscript document. */
  struct TDocument {
    std::string Uri;
    std::string Text;
    int Version = 0;

    /* Cached parse and semantic analysis results */
    Tools::Nycr::TContextBuilt<Package::Syntax::TPackage> Cst;
    std::unique_ptr<Synth::TPackage> Synth;
    std::vector<TDiagnostic> Diagnostics;
  };

  /* The LSP server handling JSON-RPC requests and notifications over stdio. */
  class TServer {
    public:

    TServer();
    ~TServer();

    /* Runs the stdio message loop until shutdown/exit or stream EOF. */
    void Run(std::istream &in, std::ostream &out);

    /* Process a single parsed JSON-RPC message. Returns false if server should exit. */
    bool HandleMessage(const Base::TJson &msg, std::ostream &out);

    /* Document management */
    void OpenDocument(const std::string &uri, const std::string &text, int version = 0);
    void UpdateDocument(const std::string &uri, const std::string &text, int version = 0);
    void CloseDocument(const std::string &uri);

    [[nodiscard]] const TDocument *GetDocument(const std::string &uri) const;

    /* Language features (available directly for unit testing) */
    [[nodiscard]] std::vector<TDiagnostic> GetDiagnostics(const std::string &uri) const;
    [[nodiscard]] std::optional<THover> GetHover(const std::string &uri, const TPosition &pos) const;
    [[nodiscard]] std::vector<TLocation> GetDefinition(const std::string &uri, const TPosition &pos) const;
    [[nodiscard]] std::vector<TDocumentSymbol> GetDocumentSymbols(const std::string &uri) const;
    [[nodiscard]] std::vector<TCompletionItem> GetCompletions(const std::string &uri, const TPosition &pos) const;

    private:

    void AnalyzeDocument(TDocument &doc);
    void SendNotification(const std::string &method, const Base::TJson &params, std::ostream &out);
    void SendResponse(const Base::TJson &id, const Base::TJson &result, std::ostream &out);
    void SendError(const Base::TJson &id, int code, const std::string &message, std::ostream &out);

    std::unordered_map<std::string, TDocument> Documents;
    std::unique_ptr<Type::TTypeCzar> TypeCzar;
    bool ShutdownRequested = false;

  };  // TServer

}  // namespace Orly::Lsp
