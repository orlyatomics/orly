/* <orly/lsp/protocol.h>

   LSP (Language Server Protocol) data types and JSON serialization (#739).

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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <base/json.h>
#include <tools/nycr/pos_range.h>

namespace Orly::Lsp {

  /* 0-indexed position in a text document. */
  struct TPosition {
    uint32_t Line = 0;
    uint32_t Character = 0;

    [[nodiscard]] bool operator==(const TPosition &other) const noexcept {
      return Line == other.Line && Character == other.Character;
    }

    [[nodiscard]] bool operator!=(const TPosition &other) const noexcept {
      return !(*this == other);
    }

    [[nodiscard]] bool operator<=(const TPosition &other) const noexcept {
      return Line < other.Line || (Line == other.Line && Character <= other.Character);
    }

    [[nodiscard]] bool operator<(const TPosition &other) const noexcept {
      return Line < other.Line || (Line == other.Line && Character < other.Character);
    }

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      j["line"] = static_cast<double>(Line);
      j["character"] = static_cast<double>(Character);
      return j;
    }

    static TPosition FromJson(const Base::TJson &j) {
      TPosition p;
      if (const auto *l = j.TryFind("line")) {
        p.Line = static_cast<uint32_t>(l->GetNumber());
      }
      if (const auto *c = j.TryFind("character")) {
        p.Character = static_cast<uint32_t>(c->GetNumber());
      }
      return p;
    }
  };

  /* A range in a text document. */
  struct TRange {
    TPosition Start;
    TPosition End;

    [[nodiscard]] bool operator==(const TRange &other) const noexcept {
      return Start == other.Start && End == other.End;
    }

    [[nodiscard]] bool operator!=(const TRange &other) const noexcept {
      return !(*this == other);
    }

    [[nodiscard]] bool Contains(const TPosition &pos) const noexcept {
      return Start <= pos && pos <= End;
    }

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      j["start"] = Start.ToJson();
      j["end"] = End.ToJson();
      return j;
    }

    static TRange FromJson(const Base::TJson &j) {
      TRange r;
      if (const auto *s = j.TryFind("start")) {
        r.Start = TPosition::FromJson(*s);
      }
      if (const auto *e = j.TryFind("end")) {
        r.End = TPosition::FromJson(*e);
      }
      return r;
    }

    /* Convert 1-indexed Tools::Nycr::TPosRange to 0-indexed LSP TRange. */
    static TRange FromNycr(const Tools::Nycr::TPosRange &pos) {
      const uint32_t start_line = pos.GetStart().GetLineNumber() > 0 ? pos.GetStart().GetLineNumber() - 1 : 0;
      const uint32_t start_col = pos.GetStart().GetColumnNumber() > 0 ? pos.GetStart().GetColumnNumber() - 1 : 0;
      const uint32_t end_line = pos.GetLimit().GetLineNumber() > 0 ? pos.GetLimit().GetLineNumber() - 1 : 0;
      const uint32_t end_col = pos.GetLimit().GetColumnNumber() > 0 ? pos.GetLimit().GetColumnNumber() - 1 : 0;
      return TRange{TPosition{start_line, start_col}, TPosition{end_line, end_col}};
    }
  };

  /* Represents a location inside a resource. */
  struct TLocation {
    std::string Uri;
    TRange Range;

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      j["uri"] = Uri;
      j["range"] = Range.ToJson();
      return j;
    }
  };

  enum class TDiagnosticSeverity : int {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4
  };

  struct TDiagnostic {
    TRange Range;
    TDiagnosticSeverity Severity = TDiagnosticSeverity::Error;
    std::string Source = "orlyc";
    std::string Message;

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      j["range"] = Range.ToJson();
      j["severity"] = static_cast<double>(static_cast<int>(Severity));
      j["source"] = Source;
      j["message"] = Message;
      return j;
    }
  };

  /* LSP SymbolKind enum values. */
  namespace TSymbolKind {
    constexpr int File = 1;
    constexpr int Module = 2;
    constexpr int Namespace = 3;
    constexpr int Package = 4;
    constexpr int Class = 5;
    constexpr int Method = 6;
    constexpr int Property = 7;
    constexpr int Field = 8;
    constexpr int Constructor = 9;
    constexpr int Enum = 10;
    constexpr int Interface = 11;
    constexpr int Function = 12;
    constexpr int Variable = 13;
    constexpr int Constant = 14;
    constexpr int String = 15;
    constexpr int Number = 16;
    constexpr int Boolean = 17;
    constexpr int Array = 18;
    constexpr int Object = 19;
    constexpr int Key = 20;
    constexpr int Null = 21;
    constexpr int Struct = 23;
  }  // TSymbolKind

  struct TDocumentSymbol {
    std::string Name;
    std::string Detail;
    int Kind = TSymbolKind::Function;
    TRange Range;
    TRange SelectionRange;
    std::vector<TDocumentSymbol> Children;

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      j["name"] = Name;
      if (!Detail.empty()) {
        j["detail"] = Detail;
      }
      j["kind"] = static_cast<double>(Kind);
      j["range"] = Range.ToJson();
      j["selectionRange"] = SelectionRange.ToJson();
      if (!Children.empty()) {
        Base::TJson::TArray ch;
        for (const auto &c : Children) {
          ch.push_back(c.ToJson());
        }
        j["children"] = Base::TJson(std::move(ch));
      }
      return j;
    }
  };

  /* LSP CompletionItemKind enum values. */
  namespace TCompletionItemKind {
    constexpr int Text = 1;
    constexpr int Method = 2;
    constexpr int Function = 3;
    constexpr int Constructor = 4;
    constexpr int Field = 5;
    constexpr int Variable = 6;
    constexpr int Class = 7;
    constexpr int Interface = 8;
    constexpr int Module = 9;
    constexpr int Property = 10;
    constexpr int Unit = 11;
    constexpr int Value = 12;
    constexpr int Enum = 13;
    constexpr int Keyword = 14;
    constexpr int Snippet = 15;
    constexpr int Color = 16;
    constexpr int File = 17;
    constexpr int Reference = 18;
    constexpr int Folder = 19;
    constexpr int EnumMember = 20;
    constexpr int Constant = 21;
    constexpr int Struct = 22;
    constexpr int Event = 23;
    constexpr int Operator = 24;
    constexpr int TypeParameter = 25;
  }  // TCompletionItemKind

  struct TCompletionItem {
    std::string Label;
    int Kind = TCompletionItemKind::Text;
    std::string Detail;
    std::string Documentation;
    std::string InsertText;

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      j["label"] = Label;
      j["kind"] = static_cast<double>(Kind);
      if (!Detail.empty()) {
        j["detail"] = Detail;
      }
      if (!Documentation.empty()) {
        Base::TJson doc = Base::TJson::Object;
        doc["kind"] = "markdown";
        doc["value"] = Documentation;
        j["documentation"] = doc;
      }
      if (!InsertText.empty()) {
        j["insertText"] = InsertText;
      }
      return j;
    }
  };

  struct THover {
    std::string Markdown;
    std::optional<TRange> Range;

    [[nodiscard]] Base::TJson ToJson() const {
      Base::TJson j = Base::TJson::Object;
      Base::TJson contents = Base::TJson::Object;
      contents["kind"] = "markdown";
      contents["value"] = Markdown;
      j["contents"] = contents;
      if (Range) {
        j["range"] = Range->ToJson();
      }
      return j;
    }
  };

  inline std::ostream &operator<<(std::ostream &strm, const TPosition &p) {
    return strm << p.Line << ':' << p.Character;
  }

  inline std::ostream &operator<<(std::ostream &strm, const TRange &r) {
    return strm << r.Start << '-' << r.End;
  }

  inline std::ostream &operator<<(std::ostream &strm, TDiagnosticSeverity s) {
    switch (s) {
      case TDiagnosticSeverity::Error: return strm << "Error";
      case TDiagnosticSeverity::Warning: return strm << "Warning";
      case TDiagnosticSeverity::Information: return strm << "Information";
      case TDiagnosticSeverity::Hint: return strm << "Hint";
    }
    return strm << "Unknown";
  }

}  // namespace Orly::Lsp
