/* <test/expect.h>

   An expectation in a unit test fixture.

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
#include <cstdint>
#include <ios>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>

#include <base/class_traits.h>
#include <base/code_location.h>
#include <base/demangle.h>
#include <base/no_default_case.h>
#include <base/test/runner.h>

/* Exepects lhs == rhs. */
#define EXPECT_EQ(lhs, rhs) \
    ::Test::TExpect(HERE, #lhs, lhs, ::Test::TExpect::Eq, #rhs, rhs)

/* Exepects lhs != rhs. */
#define EXPECT_NE(lhs, rhs) \
    ::Test::TExpect(HERE, #lhs, lhs, ::Test::TExpect::Ne, #rhs, rhs)

/* Exepects lhs < rhs. */
#define EXPECT_LT(lhs, rhs) \
    ::Test::TExpect(HERE, #lhs, lhs, ::Test::TExpect::Lt, #rhs, rhs)

/* Exepects lhs <= rhs. */
#define EXPECT_LE(lhs, rhs) \
    ::Test::TExpect(HERE, #lhs, lhs, ::Test::TExpect::Le, #rhs, rhs)

/* Exepects lhs > rhs. */
#define EXPECT_GT(lhs, rhs) \
    ::Test::TExpect(HERE, #lhs, lhs, ::Test::TExpect::Gt, #rhs, rhs)

/* Exepects lhs >= rhs. */
#define EXPECT_GE(lhs, rhs) \
    ::Test::TExpect(HERE, #lhs, lhs, ::Test::TExpect::Ge, #rhs, rhs)

/* Exepects arg == true. */
#define EXPECT_TRUE(arg) \
    ::Test::TExpect(HERE, ::Test::TExpect::IsTrue, #arg, arg)

/* Exepects arg == false. */
#define EXPECT_FALSE(arg) \
    ::Test::TExpect(HERE, ::Test::TExpect::IsFalse, #arg, arg)

/* Exepects lhs == rhs. */
#define EXPECT_EQ_AT(here, lhs, rhs) \
    ::Test::TExpect(here, #lhs, lhs, ::Test::TExpect::Eq, #rhs, rhs)

/* Exepects lhs != rhs. */
#define EXPECT_NE_AT(here, lhs, rhs) \
    ::Test::TExpect(here, #lhs, lhs, ::Test::TExpect::Ne, #rhs, rhs)

/* Exepects lhs < rhs. */
#define EXPECT_LT_AT(here, lhs, rhs) \
    ::Test::TExpect(here, #lhs, lhs, ::Test::TExpect::Lt, #rhs, rhs)

/* Exepects lhs <= rhs. */
#define EXPECT_LE_AT(here, lhs, rhs) \
    ::Test::TExpect(here, #lhs, lhs, ::Test::TExpect::Le, #rhs, rhs)

/* Exepects lhs > rhs. */
#define EXPECT_GT_AT(here, lhs, rhs) \
    ::Test::TExpect(here, #lhs, lhs, ::Test::TExpect::Gt, #rhs, rhs)

/* Exepects lhs >= rhs. */
#define EXPECT_GE_AT(here, lhs, rhs) \
    ::Test::TExpect(here, #lhs, lhs, ::Test::TExpect::Ge, #rhs, rhs)

/* Exepects arg == true. */
#define EXPECT_TRUE_AT(here, arg) \
    ::Test::TExpect(here, ::Test::TExpect::IsTrue, #arg, arg)

/* Exepects arg == false. */
#define EXPECT_FALSE_AT(here, arg) \
    ::Test::TExpect(here, ::Test::TExpect::IsFalse, #arg, arg)

/* Exepects that the given exception class will be thrown during evaluation of the function. */
#define EXPECT_THROW(exception, func) \
    ::Test::TExpect(HERE, #exception, \
                    [](bool &pass) { try { func(); pass = false; } catch (const exception&) { pass = true; } })

/* Like EXPECT_THROW, but captures the callable function. */
#define EXPECT_THROW_FUNC(exception, func) \
    ::Test::TExpect(HERE, #exception, \
                    [&func](bool &pass) { try { func(); pass = false; } catch (const exception&) { pass = true; } })


/* Exepects that the given exception class will be thrown during evaluation of the function. */
#define EXPECT_THROW_AT(here, exception, func) \
    ::Test::TExpect(here, #exception, \
                    [](bool &pass) { try { func(); pass = false; } catch (const exception&) { pass = true; }})


namespace Test {

  class TExpect : public TRunner::TExpect {
    NO_COPY(TExpect);
    public:

    enum TInfixOp { Lt, Le, Gt, Ge };

    enum TEqOp {Eq, Ne};

    template <typename TLhs, typename TRhs>
    TExpect(
        const Base::TCodeLocation &code_location,
        const char *lhs_str, const TLhs &lhs,
        TEqOp op,
        const char *rhs_str, const TRhs &rhs) : CodeLocation(code_location) {
      assert(lhs_str);
      assert(rhs_str);

      const char *op_str = "ERROR";
      switch(op) {
        case Eq: {
          op_str = "==";
          Pass = (lhs == rhs);
          break;
        }
        case Ne: {
          op_str = "!=";
          Pass = (lhs != rhs);
          break;
        }
      }

      WriteInfixOp(lhs_str, lhs, op_str, rhs_str, rhs);
    }

    enum TPrefixOp { IsTrue, IsFalse };

    template <typename TLhs, typename TRhs>
    TExpect(
        const Base::TCodeLocation &code_location,
        const char *lhs_str, const TLhs &lhs,
        TInfixOp op,
        const char *rhs_str, const TRhs &rhs)
        : CodeLocation(code_location) {
      assert(lhs_str);
      assert(rhs_str);

      const char *op_str;
      switch (op) {
        case Lt: {
          op_str = "<";
          Pass = (lhs < rhs);
          break;
        }
        case Le: {
          op_str = "<=";
          Pass = (lhs <= rhs);
          break;
        }
        case Gt: {
          op_str = ">";
          Pass = (lhs > rhs);
          break;
        }
        case Ge: {
          op_str = ">=";
          Pass = (lhs >= rhs);
          break;
        }
      }
      WriteInfixOp(lhs_str, lhs, op_str, rhs_str, rhs);
    }

    template <typename TArg>
    TExpect(
        const Base::TCodeLocation &code_location,
        TPrefixOp op, const char *arg_str, const TArg &arg)
        : CodeLocation(code_location) {
      assert(arg_str);
      const char *op_str;
      std::ostringstream strm;
      switch (op) {
        case IsTrue: {
          op_str = "";
          Pass = bool(arg);
          break;
        }
        case IsFalse: {
          op_str = "!";
          Pass = !bool(arg);
          break;
        }
        NO_DEFAULT_CASE;
      }
      WriteType<TArg>(strm);
      strm << op_str << arg_str;
      Source = strm.str();
      strm.str("");
      strm << op_str << bool(arg);
      Expression = strm.str();
    }

    TExpect(
      const Base::TCodeLocation &code_location,
      const char *exc_str,
      const std::function<void (bool &pass)> &func) : CodeLocation(code_location) {
      assert(exc_str);
      assert(func);

      std::ostringstream strm;
      strm << "throw " << exc_str;
      Source = strm.str();

      func(Pass);

      Expression = Pass ? "throw" : "nothrow";

    }


    virtual ~TExpect();

    operator bool() const {
      return Pass;
    }

    template <typename TVal>
    const TExpect &Write(const TVal &val) const {
      Explanation << val;
      return *this;
    }

    private:
    /* C++20 deleted operator<<(ostream&, char16_t/char32_t/char8_t) since
       there's no agreed encoding for writing a single unicode code unit to
       a byte stream. We just want a readable diagnostic, so print the code
       point as U+XXXX hex. */
    template <typename TVal>
    static void StreamValue(std::ostream &strm, const TVal &val) {
      using TBare = std::decay_t<TVal>;
      if constexpr (std::is_same_v<TBare, char16_t> ||
                    std::is_same_v<TBare, char32_t> ||
                    std::is_same_v<TBare, char8_t>) {
        std::ios::fmtflags saved(strm.flags());
        strm << "U+" << std::hex << static_cast<std::uint32_t>(val);
        strm.flags(saved);
      } else {
        strm << val;
      }
    }

    template <typename TLhs, typename TRhs>
    void WriteInfixOp(const char *lhs_str, const TLhs &lhs, const char *op_str, const char *rhs_str, const TRhs &rhs) {
      assert(lhs_str);
      assert(op_str);
      assert(rhs_str);

      std::ostringstream strm;
      WriteType<TLhs>(strm);
      strm << lhs_str << ' ' << op_str << ' ';
      WriteType<TRhs>(strm);
      strm << rhs_str;
      Source = strm.str();
      strm.str("");
      StreamValue(strm, lhs);
      strm << ' ' << op_str << ' ';
      StreamValue(strm, rhs);
      Expression = strm.str();
    }

    template <typename TArg>
    static void WriteType(std::ostream &strm) {
      strm << '(' << Base::Demangle<TArg>() << ')';
    }

    Base::TCodeLocation CodeLocation;

    bool Pass;

    std::string Source, Expression;

    mutable std::ostringstream Explanation;
  };

}

/* A stream inserter for Test::TExpect targets. */
template <typename TVal>
const Test::TExpect &operator<<(const Test::TExpect &expect, const TVal &val) {
  return expect.Write(val);
}
