/* <base/chrono.h>

   Provides helpful functions to be used with <chrono> library

   NOTE. This should really live as <orly/rt/chrono.h>.
         It's here because <tools/nycr/nycr> uses it.

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

#include <chrono>
#include <ctime>

#include <base/thrower.h>

namespace Base {

  namespace Chrono {

    DEFINE_ERROR(TChronoError, std::runtime_error, "chrono error");

    /* convenience typedef for std::chrono::system_clock */
    typedef std::chrono::system_clock TClock;

    /* convenience typedef for std::chrono::seconds */
    typedef std::chrono::nanoseconds TTimeDiff;

    /* convenience typedef for std::chrono::time_point */
    typedef std::chrono::time_point<TClock, TTimeDiff> TTimePnt;

    class TTimeDiffInfo {
      public:

      TTimeDiffInfo(
          bool is_forward,
          uint64_t day,
          uint64_t hour,
          uint64_t minute,
          uint64_t second,
	        uint64_t nanosecond);

      TTimeDiffInfo(const std::string &str);

      TTimeDiffInfo(const TTimeDiff &time_diff);

      std::string AsString() const;

      TTimeDiff AsTimeDiff() const;

      void Write(std::ostream &strm) const;

      /* Accessors */
      int64_t getDay() const {
        return Day;
      }

      int64_t getHour() const {
        return Hour;
      }

      bool getIsForward() const {
        return IsForward;
      }

      int64_t getNanosecond() const {
        return Nanosecond;
      }

      int64_t getMinute() const {
        return Minute;
      }

      int64_t getSecond() const {
        return Second;
      }

      private:

      bool IsForward;

      int64_t Day, Hour, Minute, Second, Nanosecond;

    };  // TTimeDiffInfo

    /* NOTE: UtcOffset is +/- from Zulu time in MINUTES. */
    class TTimePntInfo {
      public:

      TTimePntInfo(
          uint64_t year, uint64_t month, uint64_t day,
          uint64_t hour, uint64_t minute, uint64_t second,
          uint64_t nanosecond,
          int64_t utc_offset);

      TTimePntInfo(const std::string &str);

      TTimePntInfo(const TTimePnt &time_pnt);

      std::string AsString() const;

      TTimePnt AsTimePnt() const;

      void Validate(const TCodeLocation &here) const;

      void Write(std::ostream &strm) const;

      /* Accessors */
      uint64_t getDay() const {
        return Day;
      }

      uint64_t getHour() const {
        return Hour;
      }

      uint64_t getNanosecond() const {
        return Nanosecond;
      }

      uint64_t getMinute() const {
        return Minute;
      }

      uint64_t getMonth() const {
        return Month;
      }

      uint64_t getSecond() const {
        return Second;
      }

      int64_t getUtcOffset() const {
        return UtcOffset;
      }

      uint64_t getYear() const {
        return Year;
      }

      private:

      std::tm &AsTmObj(std::tm &out) const;

      uint64_t Year, Month, Day, Hour, Minute, Second, Nanosecond;

      int64_t UtcOffset;

    };  // TTimePntInfo

    template <typename TVal, typename TPeriod>
      TTimeDiff TimeDiffCast(const std::chrono::duration<TVal, TPeriod> &time_diff) {
      return std::chrono::duration_cast<TTimeDiff>(time_diff);
    }

    template <typename TVal>
    TTimePnt TimePntCast(const std::chrono::time_point<TVal> &time_pnt) {
      return std::chrono::time_point_cast<TTimeDiff, TClock>(time_pnt);
    }

    /* These are the built_in functions that are used in code gen */

    TTimeDiff CreateTimeDiff(
        bool is_forward,
        uint64_t day,
        uint64_t hour,
        uint64_t minute,
        uint64_t second,
	uint64_t nanosecond);

    /* Create a time point from year down to second.
       NOTE: utc_offset is +/- from Zulu time in minutes! */
    TTimePnt CreateTimePnt(
        uint64_t year, uint64_t month, uint64_t day,
        uint64_t hour, uint64_t minute, uint64_t second,
	uint64_t nanosecond,
        int64_t utc_offset);

    TTimePnt Now();

    inline std::ostream &operator<<(std::ostream &strm, const TTimeDiff &that) {
      Base::Chrono::TTimeDiffInfo(that).Write(strm);
      return strm;
    }

    inline std::ostream &operator<<(std::ostream &strm, const TTimePnt &that) {
      Base::Chrono::TTimePntInfo(that).Write(strm);
      return strm;
    }

  }  // Chrono

}  // Base

namespace std {

  template <>
  struct hash<Base::Chrono::TTimeDiff> {

    typedef size_t result_type;

    typedef Base::Chrono::TTimeDiff argument_type;

    result_type operator()(const argument_type &that) const {
      return _Hash_impl::hash(&that, sizeof(that));
    }

  };  // hash<Base::Chrono::TTimeDiff>

  template <>
  struct hash<Base::Chrono::TTimePnt> {

    typedef size_t result_type;

    typedef Base::Chrono::TTimePnt argument_type;

    result_type operator()(const argument_type &that) const {
      return _Hash_impl::hash(&that, sizeof(that));
    }

  };  // hash<Base::Chrono::TTimePnt>

}  // std
