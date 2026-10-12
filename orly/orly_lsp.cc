/* <orly/orly_lsp.cc>

   Main entry point for the Orlyscript Language Server (orly-lsp) (#739).

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

#include <iostream>
#include <string>

#include <orly/lsp/server.h>

#if __has_include(<orly/build_version.h>)
#include <orly/build_version.h>
#endif
#ifndef ORLY_BUILD_VERSION
#define ORLY_BUILD_VERSION "unknown"
#endif

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version" || arg == "-v") {
      std::cout << "orly-lsp " << ORLY_BUILD_VERSION << std::endl;
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << "Orlyscript Language Server (orly-lsp)\n\n"
                << "Usage: orly-lsp [options]\n\n"
                << "Options:\n"
                << "  -v, --version    Show version\n"
                << "  -h, --help       Show help\n\n"
                << "Speaks Language Server Protocol (LSP) JSON-RPC over stdio.\n";
      return 0;
    }
  }

  Orly::Lsp::TServer server;
  server.Run(std::cin, std::cout);
  return 0;
}
