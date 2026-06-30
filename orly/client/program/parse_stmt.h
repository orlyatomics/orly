/* <orly/client/program/parse_stmt.h>

   Parser for orly client-program statements. `ParseStmtFile` /
   `ParseStmtStr` invokes `TForStmt` per parsed `TStmt` CST node.

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

#include <functional>

#include <orly/client/program/program.program.cst.h>

namespace Orly {

  namespace Client {

    namespace Program {

      using TForStmt = std::function<void (const TStmt *)>;

      void ParseStmtFile(const char *path, const TForStmt &cb);

      void ParseStmtStr(const char *str, const TForStmt &cb);

    }  // Program

  }  // Client

}  // Orly
