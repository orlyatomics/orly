/* <orly/server/rfc3339.h>

   Parses an RFC 3339 date-time, such as 2026-05-17T09:30:00Z or 2026-05-17T11:30:00.25+02:00, for `try ... at time "..."`
   (#889). It is strict: the T may be a lowercase t, but a space is not allowed in its place, and the zone is required, since
   a time with none would be read in a zone the client may not share with the server.

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

#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace Orly {

  namespace Server {

    /* The time `text` names, or nullopt if it isn't an RFC 3339 date-time. Fractional seconds finer than a
       microsecond are truncated. A time the system clock cannot represent (the nanosecond clock spans about 1677 to
       2262) is clamped to the nearest time it can, rather than overflowing the conversion. */
    [[nodiscard]] inline std::optional<std::chrono::system_clock::time_point> ParseRfc3339(const std::string &text) {
      using namespace std::chrono;
      size_t pos = 0;
      auto digits = [&](size_t count) -> std::optional<int> {
        if (pos + count > text.size()) {
          return std::nullopt;
        }
        int value = 0;
        for (size_t i = 0; i < count; ++i) {
          const char c = text[pos + i];
          if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
          }
          value = value * 10 + (c - '0');
        }
        pos += count;
        return value;
      };
      auto literal = [&](char c) {
        if (pos < text.size() && text[pos] == c) {
          ++pos;
          return true;
        }
        return false;
      };
      const auto year = digits(4);
      if (!year || !literal('-')) {
        return std::nullopt;
      }
      const auto month = digits(2);
      if (!month || !literal('-')) {
        return std::nullopt;
      }
      const auto day = digits(2);
      if (!day || !(literal('T') || literal('t'))) {
        return std::nullopt;
      }
      const auto hour = digits(2);
      if (!hour || !literal(':')) {
        return std::nullopt;
      }
      const auto minute = digits(2);
      if (!minute || !literal(':')) {
        return std::nullopt;
      }
      const auto second = digits(2);
      if (!second) {
        return std::nullopt;
      }
      long micros = 0;
      if (literal('.')) {
        size_t count = 0;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
          if (count < 6) {
            micros = micros * 10 + (text[pos] - '0');
            ++count;
          }
          ++pos;
        }
        if (!count) {
          return std::nullopt;
        }
        for (; count < 6; ++count) {
          micros *= 10;
        }
      }
      long offset_minutes = 0;
      if (literal('Z') || literal('z')) {
      } else {
        const bool negative = pos < text.size() && text[pos] == '-';
        if (!literal('+') && !literal('-')) {
          return std::nullopt;
        }
        const auto offset_hour = digits(2);
        if (!offset_hour || !literal(':')) {
          return std::nullopt;
        }
        const auto offset_minute = digits(2);
        if (!offset_minute || *offset_hour > 23 || *offset_minute > 59) {
          return std::nullopt;
        }
        offset_minutes = (*offset_hour * 60 + *offset_minute) * (negative ? -1 : 1);
      }
      if (pos != text.size()) {
        return std::nullopt;
      }
      /* A leap second (60) is accepted by the RFC; it names the same instant as the next second does here. */
      if (*month < 1 || *month > 12 || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
      }
      const year_month_day ymd(std::chrono::year(*year), std::chrono::month(static_cast<unsigned>(*month)),
                               std::chrono::day(static_cast<unsigned>(*day)));
      if (!ymd.ok()) {
        return std::nullopt;
      }
      const auto when = sys_days(ymd) + hours(*hour) + minutes(*minute) + seconds(*second) + microseconds(micros)
                        - minutes(offset_minutes);
      /* `when` counts microseconds; casting it to a finer clock multiplies, which overflows for a far year. */
      constexpr auto earliest = duration_cast<microseconds>(system_clock::duration::min());
      constexpr auto latest = duration_cast<microseconds>(system_clock::duration::max());
      if (when.time_since_epoch() <= earliest) {
        return system_clock::time_point::min();
      }
      if (when.time_since_epoch() >= latest) {
        return system_clock::time_point::max();
      }
      return time_point_cast<system_clock::duration>(when);
    }

  }  // Server

}  // Orly
