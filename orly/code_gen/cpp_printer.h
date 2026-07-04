
/* <orly/code_gen/cpp_printer.h>

   The C++ source-writing stream used by every code-gen node.
   `TCppPrinter` buffers output in memory with indent tracking and
   a tiny vocabulary (`TEol`, `TIndent`, `TNamespacePrinter`,
   `TOrlyNamespacePrinter`). Construct with a target filename and
   description; each `Write` call honours the current indent level
   (set up by `TIndent` RAII guards).

   The buffer reaches disk in `Commit()` (run by the destructor on
   non-exception paths): if the target file already holds exactly this
   content it is left untouched -- preserving its mtime for anything
   keyed on it downstream (jhm, ccache, the compile driver's staleness
   check, #312) -- otherwise the content lands via write-to-temp +
   rename. The rename doubles as crash atomicity (a Ctrl-C never leaves
   a half-written file) and closes a real race: parallel orlyc
   processes all emit the shared object headers under orly/rt/objects,
   and an in-place rewrite could be seen half-done by another process's
   gcc step.

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

#include <cstdio>
#include <exception>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <base/split.h>
#include <base/path.h>
#include <orly/code_gen/error.h>
#include <orly/package/name.h>

namespace Orly {

  namespace CodeGen {

    //A simple token class for printing the end of a line
    class TEol {};


    static const TEol Eol{};

    class TCppPrinter {
      NO_COPY(TCppPrinter);
      public:

      TCppPrinter(const std::string &filename, const std::string &file_desc = "IR")
          : Indent(0), Filename(filename), FileDesc(file_desc), StartOfLine(true) {}

      /* Commits on the way out; a commit failure must fail the compile, hence
         noexcept(false). Mid-unwind the output is abandoned instead -- the
         compile already failed and a partial buffer must not reach disk. */
      ~TCppPrinter() noexcept(false) {
        if (!Committed && std::uncaught_exceptions() == 0) {
          Commit();
        }
      }

      /* Write the buffer to Filename, unless the file already holds exactly
         this content (then leave it untouched, preserving its mtime). New
         content goes to a pid-suffixed temp file in the same directory and is
         rename()d into place. */
      void Commit() {
        Committed = true;
        const std::string content = Out.str();
        /* already up to date? */ {
          std::ifstream in(Filename, std::ios::binary);
          if (in) {
            std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in && existing == content) {
              return;
            }
          }
        }
        const std::string tmp_path = Filename + ".tmp." + std::to_string(getpid());
        /* write the temp file */ {
          std::ofstream tmp(tmp_path, std::ios::binary | std::ios::trunc);
          if (tmp) {
            tmp << content;
            tmp.close();
          }
          if (!tmp) {
            unlink(tmp_path.c_str());
            throw TCgError(HERE, ("Unable to open file '" + tmp_path + "' for writing " + FileDesc + " to.").c_str());
          }
        }
        if (rename(tmp_path.c_str(), Filename.c_str()) != 0) {
          unlink(tmp_path.c_str());
          throw TCgError(HERE, ("Unable to move '" + tmp_path + "' into place as '" + Filename + "'.").c_str());
        }
      }

      std::ostream &GetOstream() {

        return Out;
      }

      template <typename TVal>
      void Write(const TVal &val) {
        if(StartOfLine) {
          for(auto i = 0u; i < Indent; ++i) {
            Out << "  ";
          }
          StartOfLine = false;
        }
        Out << val;
      }

      private:
      unsigned int Indent;
      std::string Filename;
      std::string FileDesc;
      std::ostringstream Out;
      bool StartOfLine;
      bool Committed = false;

      friend class TIndent;
      friend class TNamespacePrinter;
      friend class TOrlyNamespacePrinter;
    }; // TCppPrinter

    class TIndent {
      NO_COPY(TIndent);
      public:
      TIndent(TCppPrinter &out) : Out(out) {
        ++Out.Indent;
      }

      ~TIndent() {
        --Out.Indent;
      }

      private:
      TCppPrinter &Out;
    }; // TIndent

    template <>
    inline void TCppPrinter::Write(const TEol &) {
      Out << '\n';
      StartOfLine = true;
    }

    template <typename TContainer, typename TDelimiter, typename TFormat>
    TCppPrinter &operator<<(TCppPrinter &printer,
                            const Base::TJoin<TContainer, TDelimiter, TFormat> &that) {
      return Base::WriteJoin(printer, that);
    }

    template <typename TVal>
    TCppPrinter &operator<<(TCppPrinter &printer, const TVal &val) {
      printer.Write(val);

      return printer;
    }

    class TNamespacePrinter {
      public:
      TNamespacePrinter(const Base::TNamespace &ns_, TCppPrinter &out) : Namespace(ns_), Out(out) {
        for (const auto &ns: Namespace) {
          Start(ns);
        }
      }

      void Start(const std::string &str) const {
        Out << "namespace " << str << " {" << Eol << Eol;
        ++Out.Indent;
      }

      void End(const std::string &str) const {
        --Out.Indent;
        Out << Eol << "} // " << str << Eol;
      }

      ~TNamespacePrinter() {
        for (const auto &ns: Namespace) {
          End(ns);
        }
      }

      private:
      const Base::TNamespace Namespace;
      TCppPrinter &Out;
    }; // TNamespacePrinter

    class TOrlyNamespacePrinter {
      public:
      TOrlyNamespacePrinter(const Package::TName &name_, TCppPrinter &out) : Name(name_), Out(out) {
        for(const auto &ns: Name.Name) {
          Start(ns);
        }
      }

      void Start(const std::string &str) const {
        Out << "namespace NS" << str << " {" << Eol << Eol;
        ++Out.Indent;
      }

      void End(const std::string &str) const {
        --Out.Indent;
        Out << Eol << "} // NS" << str << Eol;
      }

      ~TOrlyNamespacePrinter() {
        for(const auto &ns: Name.Name) {
          End(ns);
        }
      }

      private:
      const Package::TName Name;
      TCppPrinter &Out;
    }; // TOrlyNamespacePrinter

    template <>
    inline void TCppPrinter::Write(const Package::TName &sns) {
      *this << Base::Join(sns.Name,
                          "::",
                          [](TCppPrinter &out, const std::string &str) {
                            out << "NS" << str;
                          });
    }

  } // Codegen

} // Orly
