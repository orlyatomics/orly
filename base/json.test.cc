/* <base/json.test.cc>

   Unit test for <base/json.h>.

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

#include <base/json.h>

#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <base/as_str.h>
#include <base/test/kit.h>

using namespace std;
using namespace Base;

/* Convert a c-string to a JSON string. */
static TJson ToString(const char *text) {
  string temp;
  /* extra */ {
    ostringstream strm;
    strm << '"' << text << '"';
    temp = strm.str();
  }
  TJson json;
  /* extra */ {
    istringstream strm(temp);
    strm >> json;
  }
  return json;
}

FIXTURE(HexCode) {
  EXPECT_EQ(ToString("2 \\u002A 2 \\u003d 4"), "2 * 2 = 4");
  EXPECT_EQ(ToString("$\\u0024$"), "$$$");
  EXPECT_EQ(ToString("$\\u00a2$"), "$¢$");
  EXPECT_EQ(ToString("$\\u20AC$"), "$€$");
}

template <typename TTpl, size_t... Idx>
vector<TJson::TKind> GetKindsImpl(const index_sequence<Idx...> &) {
  return vector<TJson::TKind> { TJson(typename tuple_element<Idx, TTpl>::type()).GetKind()... };
}

template <typename TTpl>
vector<TJson::TKind> GetKinds() {
  return GetKindsImpl<TTpl>(make_index_sequence<tuple_size<TTpl>::value>());
}

template <typename TTpl>
bool AllKindsEq(TJson::TKind expected_kind) {
  static const size_t expected_size = tuple_size<TTpl>::value;
  auto kinds = GetKinds<TTpl>();
  bool success = (kinds.size() == expected_size);
  for (size_t i = 0; success && i < expected_size; ++i) {
    success = (kinds[i] == expected_kind);
  }
  return success;
}

FIXTURE(ConstructedKinds) {
  EXPECT_TRUE(TJson().GetKind() == TJson::Null);
  EXPECT_TRUE((AllKindsEq<tuple<int8_t, int16_t, int32_t, int64_t, uint8_t, uint16_t, uint32_t, uint64_t, float, double>>(TJson::Number)));
  EXPECT_TRUE((AllKindsEq<tuple<char, const char *, string>>(TJson::String)));
  EXPECT_TRUE((AllKindsEq<tuple<TJson::TObject>>(TJson::Object)));
  EXPECT_TRUE((AllKindsEq<tuple<TJson::TArray>>(TJson::Array)));
}

static const TJson::TArray  Array  = { 1, 2, 3 };
static const TJson::TObject Object = { { "a", 1 }, { "b", 2 }, { "c", 3 } };

FIXTURE(Format) {
  EXPECT_EQ(AsStr(TJson()), "null");
  EXPECT_EQ(AsStr(TJson(true)), "true");
  EXPECT_EQ(AsStr(TJson(false)), "false");
  EXPECT_EQ(AsStr(TJson("")), R"("")");
  EXPECT_EQ(AsStr(TJson('x')), R"("x")");
  EXPECT_EQ(AsStr(TJson("foo")), R"("foo")");
  EXPECT_EQ(AsStr(TJson(string("bar"))), R"("bar")");
}

FIXTURE(Defaults) {
  EXPECT_EQ(TJson(TJson::Null), TJson());
  EXPECT_EQ(TJson(TJson::Bool), TJson(false));
  EXPECT_EQ(TJson(TJson::Number), TJson(0));
  EXPECT_EQ(TJson(TJson::Array), TJson(TJson::TArray()));
  EXPECT_EQ(TJson(TJson::Object), TJson(TJson::TObject()));
  EXPECT_EQ(TJson(TJson::String), TJson(""));
}

template <typename TArg>
void TestCopyAndMove(TArg &&arg, TJson::TKind kind) {
  const TJson expected = forward<TArg>(arg);
  EXPECT_EQ(expected.GetKind(), kind);
  TJson a = expected;
  EXPECT_EQ(a, expected);
  TJson b = move(a);
  EXPECT_EQ(b, expected);
  EXPECT_TRUE(a.GetKind() == TJson::Null);
  a = expected;
  EXPECT_EQ(a, expected);
  b.Reset();
  EXPECT_TRUE(b.GetKind() == TJson::Null);
  b = move(a);
  EXPECT_EQ(b, expected);
  EXPECT_TRUE(a.GetKind() == TJson::Null);
}

FIXTURE(CopyAndMove) {
  TestCopyAndMove(TJson(), TJson::Null);
  TestCopyAndMove(true, TJson::Bool);
  TestCopyAndMove(101, TJson::Number);
  TestCopyAndMove(Array, TJson::Array);
  TestCopyAndMove(Object, TJson::Object);
  TestCopyAndMove("hello", TJson::String);
}

FIXTURE(ArraySubscripting) {
  TJson j(3, TJson::String);
  EXPECT_EQ(j.GetSize(), 3U);
  EXPECT_EQ(j[0], "");
  EXPECT_EQ(j[1], "");
  EXPECT_EQ(j[2], "");
  j[0] = "hello";
  j[1] = "doctor";
  j[2] = "name";
  EXPECT_EQ(j[0], "hello");
  EXPECT_EQ(j[1], "doctor");
  EXPECT_EQ(j[2], "name");
}

FIXTURE(ObjectSubscripting) {
  TJson j = TJson::Object;
  EXPECT_EQ(j.GetSize(), 0U);
  j["a"] = 1;
  EXPECT_EQ(j.GetSize(), 1U);
  j["b"] = 2;
  EXPECT_EQ(j.GetSize(), 2U);
  j["c"] = 3;
  EXPECT_EQ(j.GetSize(), 3U);
  EXPECT_EQ(j, Object);
  const TJson k = j;
  EXPECT_EQ(k["a"], 1);
  EXPECT_EQ(k["b"], 2);
  EXPECT_EQ(k["c"], 3);
}

FIXTURE(Parse) {
  EXPECT_EQ(TJson::Parse("null"), TJson());
  EXPECT_EQ(TJson::Parse("true"), true);
  EXPECT_EQ(TJson::Parse("false"), false);
  EXPECT_EQ(TJson::Parse("0"), 0);
  EXPECT_EQ(TJson::Parse("+101"), 101);
  EXPECT_EQ(TJson::Parse("-101"), -101);
  EXPECT_EQ(TJson::Parse("98.6"), 98.6);
  EXPECT_EQ(TJson::Parse("1e10"), 1e10);
  EXPECT_EQ(TJson::Parse("[]"), TJson::TArray());
  EXPECT_EQ(TJson::Parse("[ 1, 2, 3 ]"), Array);
  EXPECT_EQ(TJson::Parse("{}"), TJson::TObject());
  EXPECT_EQ(TJson::Parse(R"({ "a": 1, "b": 2, "c": 3} )"), Object);
  EXPECT_EQ(TJson::Parse(R"("")"), "");
  EXPECT_EQ(TJson::Parse(R"("hello")"), "hello");
  EXPECT_EQ(TJson::Parse(R"(hello)"), "hello");
  EXPECT_EQ(TJson::Parse(R"([hello,world,null,no,truest,falser,nullary])"),
            TJson::TArray({"hello", "world", TJson(), "no", "truest", "falser","nullary"}));

  EXPECT_EQ(TJson::Parse(R"(["/usr/include/stdc-predef.h","/usr/include/c++/4.9.0/ostream"])"),
            TJson::TArray({"/usr/include/stdc-predef.h", "/usr/include/c++/4.9.0/ostream"}));
  EXPECT_EQ(TJson::Parse("[1,]"), TJson::TArray({1}));
  EXPECT_EQ(TJson::Parse("{\"a\": 1,}"), TJson::TObject({{"a", 1}}));
}

FIXTURE(EscapeSequences) {
  EXPECT_EQ(TJson::Parse(R"foo("\"\\\/\b\f\n\r\t")foo").GetString(), "\"\\/\b\f\n\r\t");
  EXPECT_EQ(AsStr(TJson("\"\\/\b\f\n\r\t7")), R"foo("\"\\/\b\f\n\r\t7")foo");
  /* Control characters without a short escape must come out as \u00XX --
     raw, they make the output invalid JSON (#276). */
  EXPECT_EQ(AsStr(TJson(string("a\x01" "b\x1f\v"))), R"foo("a\u0001b\u001f\u000b")foo");
  /* ...and round-trip through our own reader. */
  EXPECT_EQ(TJson::Parse(AsStr(TJson(string("a\x01" "b\x1f\v")))).GetString(), string("a\x01" "b\x1f\v"));
  /* Raw multi-byte UTF-8 passes through untouched (legal JSON). */
  EXPECT_EQ(AsStr(TJson(string("caf\xc3\xa9"))), "\"caf\xc3\xa9\"");
  EXPECT_EQ(TJson::Parse(AsStr(TJson(string("caf\xc3\xa9")))).GetString(), "caf\xc3\xa9");
}

FIXTURE(KindToStr) {
  EXPECT_EQ(AsStr(TJson::Bool), "bool");
  EXPECT_EQ(AsStr(TJson::Null), "null");
  EXPECT_EQ(AsStr(TJson::Number), "number");
  EXPECT_EQ(AsStr(TJson::Array), "array");
  EXPECT_EQ(AsStr(TJson::Object), "object");
  EXPECT_EQ(AsStr(TJson::String), "string");
}

FIXTURE(RoundTrip) {
  EXPECT_EQ(TJson::Parse(AsStr(TJson(12345678))), 12345678);
  EXPECT_EQ(TJson::Parse(AsStr(TJson(0))), 0);
  EXPECT_EQ(TJson::Parse(AsStr(TJson(101))), 101);
  EXPECT_EQ(TJson::Parse(AsStr(TJson(-101))), -101);
  EXPECT_EQ(TJson::Parse(AsStr(TJson(98.6))), 98.6);
  EXPECT_EQ(TJson::Parse(AsStr(TJson(Array))), Array);
  EXPECT_EQ(TJson::Parse(AsStr(TJson(Object))), Object);
}

/* AppendTo is the reply path's writer; it must produce exactly the text Write does. */
static string ViaAppend(const TJson &json) {
  string out = "prefix";
  json.AppendTo(out);
  return out.substr(6);
}

FIXTURE(AppendToMatchesWrite) {
  vector<TJson> samples = {
      TJson(), TJson(true), TJson(false), TJson(0), TJson(-0.0), TJson(42), TJson(-7), TJson(98.6),
      TJson(0.1234565), TJson(1e-9), TJson(-1e-7), TJson(1e300), TJson(int64_t(9007199254740993)),
      TJson(uint64_t(18446744073709551615ULL)), TJson(""), TJson("plain text"),
      TJson(string("\"\\/\b\f\n\r\t7")), TJson(string("a\x01" "b\x1f\v")), TJson(string("caf\xc3\xa9")),
      TJson(string("\x7f trailing \\")), TJson(Array), TJson(Object)};
  TJson nested = TJson::Object;
  nested["rows"] = TJson(TJson::TArray{TJson(Array), TJson(Object), TJson("x\"y")});
  nested["status"] = "ok";
  nested["k\ney"] = TJson();
  samples.push_back(nested);
  for (const auto &json : samples) {
    EXPECT_EQ(ViaAppend(json), AsStr(json));
  }
}

/* A byte-at-a-time reference for AppendString. */
static std::string ReferenceQuote(const std::string &text) {
  std::string out = "\"";
  for (char ch: text) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        if (c < 0x20) {
          constexpr const char *hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 0xF];
        } else {
          out += ch;
        }
      }
    }
  }
  out += '"';
  return out;
}

static std::string Quote(const std::string &text) {
  std::string out;
  Base::TJson::AppendString(out, text);
  return out;
}

FIXTURE(AppendStringEscaping) {
  EXPECT_EQ(Quote(""), "\"\"");
  EXPECT_EQ(Quote("plain"), "\"plain\"");
  EXPECT_EQ(Quote("a\"b\\c"), "\"a\\\"b\\\\c\"");
  EXPECT_EQ(Quote("\b\f\n\r\t"), "\"\\b\\f\\n\\r\\t\"");
  EXPECT_EQ(Quote(std::string("\x01\x1f", 2)), "\"\\u0001\\u001f\"");
  EXPECT_EQ(Quote(std::string("a\0b", 3)), "\"a\\u0000b\"");
  /* Multi-byte UTF-8 and invalid UTF-8 pass through untouched, as does DEL. */
  EXPECT_EQ(Quote("h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80"), "\"h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80\"");
  EXPECT_EQ(Quote("\xff\xfe\x80\xc0\x7f"), "\"\xff\xfe\x80\xc0\x7f\"");
  /* Appends rather than replaces. */
  std::string out = "x";
  Base::TJson::AppendString(out, "y");
  EXPECT_EQ(out, "x\"y\"");
  /* Every byte value, at every offset and with every tail length, against the reference; this
     covers the word-at-a-time scan's boundaries. */
  for (int byte = 0; byte < 256; ++byte) {
    for (size_t len = 0; len <= 20; ++len) {
      for (size_t pos = 0; pos < len; ++pos) {
        std::string text(len, 'a');
        text[pos] = static_cast<char>(byte);
        EXPECT_EQ(Quote(text), ReferenceQuote(text));
        text[len - 1] = static_cast<char>(byte);
        EXPECT_EQ(Quote(text), ReferenceQuote(text));
      }
    }
  }
  /* Bytes next to the specials, and all-special words. */
  std::string mix;
  for (int byte = 0; byte < 256; ++byte) {
    mix.push_back(static_cast<char>(byte));
  }
  EXPECT_EQ(Quote(mix), ReferenceQuote(mix));
  EXPECT_EQ(Quote(std::string(33, '"')), ReferenceQuote(std::string(33, '"')));
  EXPECT_EQ(Quote(std::string(33, '\xa0')), ReferenceQuote(std::string(33, '\xa0')));
}
