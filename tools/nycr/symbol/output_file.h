/* <tools/nycr/symbol/output_file.h>

   Handles the creation of output files.

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

#include <cassert>
#include <exception>
#include <fstream>
#include <string>

#include <tools/nycr/symbol/language.h>

namespace Tools {

  namespace Nycr {

    namespace Symbol {

      /* A stream insertion helper for writing language paths. */
      class TPath {
        public:

        /* Do-little. */
        TPath(const char *branch, const char *atom, const TLanguage *language)
            : Atom(atom), Branch(branch), Language(language)  {
          assert(language);
        }

        /* Write the path, like 'here/is/my/language'. */
        void Write(std::ostream &strm) const;

        private:

        //The atom which the includes will live
        const char *Atom;

        //The JHM File Branch in which the includes will live
        const char *Branch;

        /* The language on which we operate. */
        const TLanguage *Language;

      };  // TPath

      /* A stream insertion helper for writing language paths. */
      class TScope {
        public:

        /* Do-little. */
        TScope(const TLanguage *language)
            : Language(language) {
          assert(language);
        }

        /* Write the path, like 'here/is/my/language'. */
        void Write(std::ostream &strm) const;

        private:

        /* The language on which we operate. */
        const TLanguage *Language;

      };  // TScope

      /* A stream insertion helper for writing 'using namespace' clauses. */
      class TUsingNamespace {
        public:

        /* Do-little. */
        TUsingNamespace(const TLanguage *language)
            : Language(language) {
          assert(language);
        }

        /* Write the clause. */
        void Write(std::ostream &strm) const;

        private:

        /* The language on which we operate. */
        const TLanguage *Language;

      };  // TUsingNamespace

      /* A stream insertion helper for writing c prefixes based on language name. */
      class TUnderscore {
        public:

        /* Do-little. */
        TUnderscore(const TLanguage *language)
            : Language(language) {
          assert(language);
        }

        /* Write the clause. */
        void Write(std::ostream &strm) const;

        private:

        /* The language on which we operate. */
        const TLanguage *Language;

      };  // TUnderscore


    enum TCommentStyle { CStyle, XmlStyle };

    /* An output stream that writes to a '.tmp'-suffixed sibling of the target
       file and atomically rename()s it into place on Commit().  A reader
       racing the generator (such as a jhm dep scan, #406) can therefore never
       observe a partially written file: the target path either does not exist
       yet, still holds its old content, or holds the complete new content.
       Destruction without Commit() -- say, because an exception unwound the
       writer -- discards the temporary and leaves the target untouched. */
    class TOutputFile final : public std::ofstream {
      public:

      TOutputFile() = default;

      TOutputFile(const TOutputFile &) = delete;
      TOutputFile &operator=(const TOutputFile &) = delete;

      /* Discards the temporary if Commit() was never reached.  (The base
         destructor is noexcept, so we cannot throw here; a writer that
         forgets to commit loses its output, which jhm's output-existence
         check reports loudly.) */
      ~TOutputFile();

      /* Opens the '.tmp' sibling of 'path' for writing. */
      void Open(std::string &&path);

      /* Closes the stream and renames the temporary to the target path.
         Throws if the stream went bad or the rename fails. */
      void Commit();

      private:

      /* The path we ultimately produce and the temporary we write to.
         TmpPath is cleared once the temporary is renamed away. */
      std::string FinalPath, TmpPath;

    };  // TOutputFile

    /* Creates an output file and writes its header comment. Returns an open stream to the file. */
    void CreateOutputFile(const char *root, const char *branch, const char *atom, const TLanguage *language, const char *ext, TOutputFile &strm, TCommentStyle comment_style = CStyle);

/* Standard inserter for Tools::Nycr::Symbol::TPath. */
inline std::ostream &operator<<(std::ostream &strm, const Tools::Nycr::Symbol::TPath &that) {
  that.Write(strm);
  return strm;
}

/* Standard inserter for Tools::Nycr::Symbol::TScope. */
inline std::ostream &operator<<(std::ostream &strm, const Tools::Nycr::Symbol::TScope &that) {
  that.Write(strm);
  return strm;
}

/* Standard inserter for Tools::Nycr::Symbol::TUsingNamespace. */
inline std::ostream &operator<<(std::ostream &strm, const Tools::Nycr::Symbol::TUsingNamespace &that) {
  that.Write(strm);
  return strm;
}


/* Standard inserter for Tools::Nycr::Symbol::TUnderscore. */
inline std::ostream &operator<<(std::ostream &strm, const Tools::Nycr::Symbol::TUnderscore &that) {
  that.Write(strm);
  return strm;
}

    }  // Symbol

  }  // Nycr

}  // Tools