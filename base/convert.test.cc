/* <base/convert.test.cc>

   Unit test for <base/convert.h>

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

#include <base/convert.h>

#include <base/test/kit.h>

#include <climits>

using namespace Base;

FIXTURE(Int) {
  int i;
  TConverter(AsPiece("42")).ReadInt(i);
  EXPECT_EQ(i, 42);
}

FIXTURE(TypeLimits) {
  long i;
  TConverter(AsPiece("9223372036854775807")).ReadInt(i);
  EXPECT_EQ(i, 9223372036854775807l);
  TConverter(AsPiece("-9223372036854775808")).ReadInt(i);
  EXPECT_EQ(i, LONG_MIN);
}

FIXTURE(ProxyInt) {
  int i = TConvertProxy(AsPiece("123"));
  EXPECT_EQ(i, 123);
  size_t size = TConvertProxy(AsPiece("456"));
  EXPECT_EQ(size, 456ul);
}

FIXTURE(Signs) {
  int i;
  TConverter(AsPiece("+42")).ReadInt(i);
  EXPECT_EQ(i, 42);
  TConverter(AsPiece("-42")).ReadInt(i);
  EXPECT_EQ(i, -42);
  /* Leading whitespace is consumed. */
  TConverter(AsPiece("  123")).ReadInt(i);
  EXPECT_EQ(i, 123);
}

FIXTURE(SignRequired) {
  int i;
  /* Without a sign, a sign-required TryReadInt declines without consuming. */
  EXPECT_FALSE(TConverter(AsPiece("42")).TryReadInt(i, true /*sign_required*/));
  EXPECT_TRUE(TConverter(AsPiece("-42")).TryReadInt(i, true));
  EXPECT_EQ(i, -42);
  EXPECT_THROW(TSyntaxError, []() {
    int val;
    TConverter(AsPiece("42")).ReadInt(val, true /*sign_required*/);
  });
}

FIXTURE(SyntaxErrors) {
  /* No digits at all. */
  EXPECT_THROW(TSyntaxError, []() {
    int val;
    TConverter(AsPiece("abc")).ReadInt(val);
  });
  /* A sign with no digits after it. */
  EXPECT_THROW(TSyntaxError, []() {
    int val;
    TConverter(AsPiece("+x")).ReadInt(val);
  });
}

FIXTURE(Bounds) {
  /* One past LONG_MAX / LONG_MIN overflow the type. */
  EXPECT_THROW(TSyntaxError, []() {
    long val;
    TConverter(AsPiece("9223372036854775808")).ReadInt(val);
  });
  EXPECT_THROW(TSyntaxError, []() {
    long val;
    TConverter(AsPiece("-9223372036854775809")).ReadInt(val);
  });
}

FIXTURE(Digits) {
  int d = -1;
  TConverter csr(AsPiece("7a"));
  EXPECT_TRUE(csr.TryReadDigit(d));
  EXPECT_EQ(d, 7);
  EXPECT_FALSE(csr.TryReadDigit(d));  // 'a' is not a digit
}

FIXTURE(ProxyRejectsTrailingJunk) {
  EXPECT_THROW(TSyntaxError, []() {
    int val = TConvertProxy(AsPiece("12x"));
    (void)val;
  });
}
FIXTURE(HexAndOctal) {
  int i;
  TConverter(AsPiece("0x2A")).ReadInt(i);
  EXPECT_EQ(i, 42);
  TConverter(AsPiece("0Xff")).ReadInt(i);
  EXPECT_EQ(i, 255);
  TConverter(AsPiece("-0x10")).ReadInt(i);
  EXPECT_EQ(i, -16);
  TConverter(AsPiece("0o52")).ReadInt(i);
  EXPECT_EQ(i, 42);
  TConverter(AsPiece("  0x2A")).ReadInt(i);  // leading whitespace
  EXPECT_EQ(i, 42);
  /* A bare leading zero stays decimal; a lone 0 is just zero. */
  TConverter(AsPiece("052")).ReadInt(i);
  EXPECT_EQ(i, 52);
  TConverter(AsPiece("0")).ReadInt(i);
  EXPECT_EQ(i, 0);
  /* 0x with no hex digit after it parses as decimal 0 (stream left at 'x'),
     matching the decimal parser's take-what-matches behavior. */
  TConverter csr(AsPiece("0xzz"));
  csr.ReadInt(i);
  EXPECT_EQ(i, 0);
  /* Hex bounds: one past LONG_MAX overflows. */
  EXPECT_THROW(TSyntaxError, []() {
    long val;
    TConverter(AsPiece("0x8000000000000000")).ReadInt(val);
  });
  long l;
  TConverter(AsPiece("0x7fffFFFFffffFFFF")).ReadInt(l);
  EXPECT_EQ(l, 9223372036854775807L);
  TConverter(AsPiece("-0x8000000000000000")).ReadInt(l);
  EXPECT_EQ(l, -9223372036854775807L - 1);
}
