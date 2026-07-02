/* <orly/csv_to_bin/level3.test.cc>

   Unit test for <orly/csv_to_bin/level3.h>.

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

#include <orly/csv_to_bin/level3.h>
#include <cmath>
#include <optional>

#include <base/strm/mem/static_in.h>
#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly::CsvToBin;

using Strm::Mem::TStaticIn;

static const TLevel1::TOptions Simple = { ',', '\'', true, true, '\\', true };

FIXTURE(OneLiner) {
  Strm::Mem::TStaticIn mem(
      "true,false,1b4e28ba-2fa1-11d2-883f-b9a761bde3fb,"
      "'hello ''doctor'' name',-123,98.6,"
      "2014-07-04 04:03:09.102-0800,\\N,101");
  TLevel1 level1(&mem, Simple);
  TLevel2 level2(level1);
  TLevel3 level3(level2);
  bool a, b;
  TUuid c;
  string d;
  int64_t e;
  double f;
  Chrono::TTimePnt g;
  std::optional<int64_t> h, i;
  level3
      >> StartOfFile >> StartOfRecord
      >> StartOfField >> a >> EndOfField
      >> StartOfField >> b >> EndOfField
      >> StartOfField >> c >> EndOfField
      >> StartOfField >> d >> EndOfField
      >> StartOfField >> e >> EndOfField
      >> StartOfField >> f >> EndOfField
      >> StartOfField >> g >> EndOfField
      >> StartOfField >> h >> EndOfField
      >> StartOfField >> i >> EndOfField
      >> EndOfRecord >> EndOfFile;
  EXPECT_TRUE(a);
  EXPECT_FALSE(b);
  EXPECT_TRUE(c);
  EXPECT_EQ(d, "hello 'doctor' name");
  EXPECT_EQ(e, -123);
  EXPECT_EQ(f, 98.6);
  EXPECT_TRUE(
      g == Chrono::CreateTimePnt(2014, 7, 4, 4, 3, 9, 102000000, -480));
  EXPECT_FALSE(h.has_value());
  EXPECT_TRUE(i.has_value());
  EXPECT_EQ(*i, 101);
}

FIXTURE(Doubles) {
  Strm::Mem::TStaticIn mem(
      "98.6,-1.5e3,+.5,720.,1E-2,inf,-Infinity,NAN");
  TLevel1 level1(&mem, Simple);
  TLevel2 level2(level1);
  TLevel3 level3(level2);
  double a, b, c, d, e, f, g, h;
  level3
      >> StartOfFile >> StartOfRecord
      >> StartOfField >> a >> EndOfField
      >> StartOfField >> b >> EndOfField
      >> StartOfField >> c >> EndOfField
      >> StartOfField >> d >> EndOfField
      >> StartOfField >> e >> EndOfField
      >> StartOfField >> f >> EndOfField
      >> StartOfField >> g >> EndOfField
      >> StartOfField >> h >> EndOfField
      >> EndOfRecord >> EndOfFile;
  EXPECT_EQ(a, 98.6);
  EXPECT_EQ(b, -1500.0);
  EXPECT_EQ(c, 0.5);
  EXPECT_EQ(d, 720.0);
  EXPECT_EQ(e, 0.01);
  EXPECT_TRUE(std::isinf(f) && f > 0);
  EXPECT_TRUE(std::isinf(g) && g < 0);
  EXPECT_TRUE(std::isnan(h));
}

FIXTURE(DoubleConservesRestOfField) {
  /* Reading a double must consume only the bytes that spell the number,
     leaving the rest of the field for further extraction. */
  Strm::Mem::TStaticIn mem("98.6F 12e3rpm");
  TLevel1 level1(&mem, Simple);
  TLevel2 level2(level1);
  TLevel3 level3(level2);
  double a, b;
  string rest;
  level3
      >> StartOfFile >> StartOfRecord >> StartOfField
      >> a >> rest >> EndOfField
      >> EndOfRecord >> EndOfFile;
  EXPECT_EQ(a, 98.6);
  EXPECT_EQ(rest, "F 12e3rpm");
  Strm::Mem::TStaticIn mem2("12e3rpm");
  TLevel1 level1b(&mem2, Simple);
  TLevel2 level2b(level1b);
  TLevel3 level3b(level2b);
  level3b
      >> StartOfFile >> StartOfRecord >> StartOfField
      >> b >> rest >> EndOfField
      >> EndOfRecord >> EndOfFile;
  EXPECT_EQ(b, 12000.0);
  EXPECT_EQ(rest, "rpm");
}

FIXTURE(BadDoubles) {
  auto parse = [](const char *text) {
    Strm::Mem::TStaticIn mem(text);
    TLevel1 level1(&mem, Simple);
    TLevel2 level2(level1);
    TLevel3 level3(level2);
    double dummy;
    level3 >> StartOfFile >> StartOfRecord >> StartOfField >> dummy;
  };
  auto no_number = [&parse] { parse("abc"); };
  EXPECT_THROW_FUNC(TLevel3::TSyntaxError, no_number);
  auto lone_sign = [&parse] { parse("+"); };
  EXPECT_THROW_FUNC(TLevel3::TSyntaxError, lone_sign);
  auto lone_point = [&parse] { parse("."); };
  EXPECT_THROW_FUNC(TLevel3::TSyntaxError, lone_point);
  auto empty_exp = [&parse] { parse("12e"); };
  EXPECT_THROW_FUNC(TLevel3::TSyntaxError, empty_exp);
  auto bad_exp = [&parse] { parse("12e+x"); };
  EXPECT_THROW_FUNC(TLevel3::TSyntaxError, bad_exp);
  auto bad_kwd = [&parse] { parse("-nap"); };
  EXPECT_THROW_FUNC(TLevel3::TSyntaxError, bad_kwd);
  auto too_big = [&parse] { parse("1e999"); };
  EXPECT_THROW_FUNC(TLevel3::TNumberOutOfRange, too_big);
}

FIXTURE(Scanner) {
  Strm::Mem::TStaticIn mem(
      "   ,   ,   \n"
      "\n"
      ",");
  TLevel1 level1(&mem, Simple);
  TLevel2 level2(level1);
  TLevel3 level3(level2);
  level3 >> StartOfFile;
  size_t record_count = 0, field_count = 0;
  while (level3.AtRecord()) {
    level3 >> StartOfRecord;
    while (level3.AtField()) {
      level3 >> StartOfField;
      level3 >> SkipBytes >> EndOfField;
      ++field_count;
    }
    level3 >> EndOfRecord;
    ++record_count;
  }
  level3 >> EndOfFile;
  EXPECT_EQ(record_count, 3u);
  EXPECT_EQ(field_count, 6u);
}
