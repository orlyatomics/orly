/* <base/uuid.h>

   A value-type wrapper for uuid_t defined in <uuid/uuid.h>.

   The text format used by TUuid looks like this: 1b4e28ba-2fa1-11d2-883f-b9a761bde3fb
   Which, as a printf pattern, would be: %08x-%04x-%04x-%04x-%012x

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

#include <algorithm>
#include <bit>
#include <cassert>
#include <compare>
#include <cstdint>
#include <cstring>
#include <istream>
#include <random>
#include <ostream>
#include <stdexcept>
#include <uuid/uuid.h>

#include <base/no_default_case.h>
#include <base/spin_lock.h>
#include <base/io/binary_input_stream.h>
#include <base/io/binary_output_stream.h>

namespace Base {

  /* Three-way compare of two raw uuids, in the order libuuid's uuid_compare gives: time_low,
     time_mid, time_hi_and_version, clock_seq, then the node, each unsigned and big-endian as stored,
     which is the same as an unsigned lexicographic compare of the 16 bytes.  That order is the on-disk
     sort order of uuid keys, so it must never change.  Returns -1, 0 or 1 (libuuid returns any
     difference; callers only test the sign).  Inline, because uuid_compare is an out-of-line call
     made once per key comparison; two big-endian 64-bit loads are cheaper than memcmp, which GCC
     will not inline for an ordering. */
  inline int CompareUuid(const uuid_t a, const uuid_t b) {
    uint64_t a_hi, a_lo, b_hi, b_lo;
    std::memcpy(&a_hi, a, 8);
    std::memcpy(&a_lo, a + 8, 8);
    std::memcpy(&b_hi, b, 8);
    std::memcpy(&b_lo, b + 8, 8);
    if constexpr (std::endian::native == std::endian::little) {
      a_hi = __builtin_bswap64(a_hi);
      a_lo = __builtin_bswap64(a_lo);
      b_hi = __builtin_bswap64(b_hi);
      b_lo = __builtin_bswap64(b_lo);
    }
    if (a_hi != b_hi) {
      return a_hi < b_hi ? -1 : 1;
    }
    if (a_lo != b_lo) {
      return a_lo < b_lo ? -1 : 1;
    }
    return 0;
  }

  /* A value-type wrapper for uuid_t defined in <uuid/uuid.h>. */
  class TUuid {
    public:

    /* The size of a string representation of a uuid. */
    static const constexpr size_t StrSize = 36;

    /* The minimum size, in bytes, of the buffers used by Format() and Parse(). */
    static const constexpr size_t MinBufSize = StrSize + 1;

    /* The algorithm to use when generating a new uuid. */
    enum TAlgo {

      /* Use /dev/random, if it exists; otherwise, behave as TimeAndMAC. */
      Best,

      /* Use /dev/random, if it exists; otheriwse, use a pseudo-random generator. */
      Random,

      /* Use the system time and MAC address of the ethernet card, if available.
         Try to avoid collisions by using global lock counter or UUID daemon, if the process has permission to do so.
         Collision is unlikely but possible. */
      TimeAndMAC,

      /* As TimeAndMAC, but do not allow collisions.
         If a collision-free guarantee is not possible, throw TUnsafeError. */
      TimeAndMACSafe,

      /* TWister */
      Twister

    };  // TAlgo

    /* The casing to use when formatting a uuid as text. */
    enum TCasing {

      /* Use the system default casing. */
      DefaultCasing,

      /* Use all upper-case letters. */
      Upper,

      /* Use all lower-case letters. */
      Lower

    };  // TCasing

    /* The exception thrown if text cannot be parsed as a uuid. */
    class TSyntaxError
        : public std::runtime_error {
      public:

      /* Do-little. */
      TSyntaxError()
          : std::runtime_error("cannot parse text as uuid") {}

    };  // TSyntaxError

    /* The exception thrown if the TimeAndMACSafe algorithm cannot run. */
    class TUnsafeError
        : public std::runtime_error {
      public:

      /* Do-little. */
      TUnsafeError()
          : std::runtime_error("cannot generate uuid safely") {}

    };  // TUnsafeError

    static bool IsValidUuid(const char *s);

    /* Default construct in the null state. */
    TUuid() {
      uuid_clear(Data);
    }

    /* Move constructor. */
    TUuid(TUuid &&that) {
      std::swap(Data, that.Data);
    }

    /* Copy constructor. */
    TUuid(const TUuid &that) {
      std::memcpy(Data, that.Data, sizeof(Data));
    }

    /* Copy constructor. */
    TUuid(const uuid_t &that) {
      std::memcpy(Data, that, sizeof(Data));
    }

    /* Construct with a unique state. */
    TUuid(TAlgo that);

    /* Construct from a 36-character, null-terminated string.
       See the comment at the top of this file for more information about text formatting. */
    explicit TUuid(const char *that) {
      if (uuid_parse(that, Data) != 0) {
        throw TSyntaxError();
      }
    }

    /* Swaperator. */
    TUuid &operator=(TUuid &&that) {
      if (this != &that) {
        std::swap(Data, that.Data);
      }
      return *this;
    }

    /* Copy assignment. */
    TUuid &operator=(const TUuid &that) {
      if (this != &that) {
        std::memcpy(Data, that.Data, sizeof(Data));
      }
      return *this;
    }

    /* Copy assignment. */
    TUuid &operator=(const uuid_t &that) {
      if (&Data != &that) {
        std::memcpy(Data, that, sizeof(Data));
      }
      return *this;
    }

    /* Unique assignment. */
    TUuid &operator=(TAlgo that) {
      return *this = TUuid(that);
    }

    /* True iff. the uuid is not null. */
    explicit operator bool() const {
      return uuid_is_null(Data) == 0;
    }

    /* Comparator. */
    bool operator==(const TUuid &that) const {
      return std::memcmp(Data, that.Data, sizeof(Data)) == 0;
    }

    /* Comparator. */
    bool operator!=(const TUuid &that) const {
      return std::memcmp(Data, that.Data, sizeof(Data)) != 0;
    }

    /* Comparator. */
    bool operator<(const TUuid &that) const {
      return CompareUuid(Data, that.Data) < 0;
    }

    /* Comparator. */
    bool operator<=(const TUuid &that) const {
      return CompareUuid(Data, that.Data) <= 0;
    }

    /* Comparator. */
    bool operator>(const TUuid &that) const {
      return CompareUuid(Data, that.Data) > 0;
    }

    /* Comparator. */
    bool operator>=(const TUuid &that) const {
      return CompareUuid(Data, that.Data) >= 0;
    }

    /* Three-way comparator. */
    std::strong_ordering operator<=>(const TUuid &that) const {
      int cmp = CompareUuid(Data, that.Data);
      if (cmp < 0) return std::strong_ordering::less;
      if (cmp > 0) return std::strong_ordering::greater;
      return std::strong_ordering::equal;
    }

    /* Comparator. */
    int Compare(const TUuid &that) const {
      return CompareUuid(Data, that.Data);
    }

    /* Format the uuid as a 36-character string, followed by a null terminator.
       The buffer must be at least MinBufSize bytes long.
       See the comment at the top of this file for more information about text formatting. */
    void Format(char *buf, TCasing casing = DefaultCasing) const {
      assert(buf);
      /* Formatted here rather than by uuid_unparse*, which go through sprintf: a reply of 1000
         rows formats thousands of ids.  The default casing is whatever this platform's
         uuid_unparse produces, asked once. */
      bool upper;
      switch (casing) {
        case DefaultCasing: {
          static const bool default_upper = [] {
            const uuid_t probe = {0xab};
            char text[MinBufSize];
            uuid_unparse(probe, text);
            return text[0] == 'A';
          }();
          upper = default_upper;
          break;
        }
        case Upper: {
          upper = true;
          break;
        }
        case Lower: {
          upper = false;
          break;
        }
        NO_DEFAULT_CASE;
      }
      const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
      char *out = buf;
      for (size_t idx = 0; idx < sizeof(Data); ++idx) {
        if (idx == 4 || idx == 6 || idx == 8 || idx == 10) {
          *out++ = '-';
        }
        *out++ = digits[Data[idx] >> 4];
        *out++ = digits[Data[idx] & 0xF];
      }
      *out = '\0';
    }

    /* Format with an underscore instead of '-' */
    void FormatUnderscore(char *buf, TCasing casing = DefaultCasing) const {
      Format(buf, casing);
      buf[8] = '_';
      buf[13] = '_';
      buf[18] = '_';
      buf[23] = '_';
    }

    /* Hasher. */
    size_t GetHash() const {
      return *reinterpret_cast<size_t *>(const_cast<unsigned char *>(Data));
    }

    const uuid_t &GetRaw() const {
      return Data;
    }

    /* Parse a 36-character, null-terminated string as a uuid.
       The buffer must be at least MinBufSize bytes long.
       See the comment at the top of this file for more information about text formatting. */
    TUuid &Parse(const char *buf) {
      return *this = TUuid(buf);
    }

    /* Read from std stream. */
    void Read(std::istream &strm) {
      char buf[MinBufSize];
      strm.read(buf, StrSize);
      buf[StrSize] = '\0';
      *this = TUuid(buf);
    }

    /* Read from a binary stream. */
    void Read(Io::TBinaryInputStream &strm) {
      strm >> Data;
    }

    /* Return to the null state. */
    TUuid &Reset() {
      uuid_clear(Data);
      return *this;
    }

    /* Write to a std stream. */
    void Write(std::ostream &strm) const {
      char buf[MinBufSize];
      Format(buf);
      strm << buf;
    }

    /* Write to a binary stream. */
    void Write(Io::TBinaryOutputStream &strm) const {
      strm << Data;
    }

    /* An instance of this class in the null state.
       Useful for providing default arguments, etc. */
    static const TUuid Null;

    private:

    /* The raw data of the uuid. */
    uuid_t Data;

  };  // TUuid

  /* A standard stream extractor for Base::TUuid. */
  inline std::istream &operator>>(std::istream &strm, TUuid &that) {
    that.Read(strm);
    return strm;
  }

  /* A standard stream inserter for Base::TUuid. */
  inline std::ostream &operator<<(std::ostream &strm, const TUuid &that) {
    that.Write(strm);
    return strm;
  }

  /* Binary extractor for Base::TUuid. */
  inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, TUuid &that) {
    that.Read(strm);
    return strm;
  }

  /* Binary inserter for Base::TUuid. */
  inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const TUuid &that) {
    that.Write(strm);
    return strm;
  }

}  // Base

namespace std {

  /* A standard hasher for Base::TUuid. */
  template <>
  struct hash<Base::TUuid> {
    typedef size_t result_type;
    typedef Base::TUuid argument_type;
    size_t operator()(const Base::TUuid &that) const {
      return that.GetHash();
    }
  };

}  // std