/* <orly/indy/util/sorter.h>

   `Indy::Util::TSorter<TVal, MemSize>` -- abstract base for
   sortable collections. Exposes a `TCursor` so the
   `TIndexManager` k-way merge can treat in-memory sorters and
   spilled `TIndexSortFile`s uniformly. Subclasses include
   `TMemSorter` (in-memory) and `TIndexSortFile::TCursor`
   (disk-backed).

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

#include <syslog.h>

#include <cassert>
#include <iterator>

#include <base/class_traits.h>

namespace Orly {

  namespace Indy {

    namespace Util {

      template<typename TVal>
      class TRandomIterator
            : public std::iterator<std::random_access_iterator_tag, TVal> {
        protected:

        TVal *Data;

        public:

        typedef std::random_access_iterator_tag iterator_category;
        typedef typename std::iterator<std::random_access_iterator_tag, TVal>::value_type value_type;
        typedef typename std::iterator<std::random_access_iterator_tag, TVal>::difference_type difference_type;
        typedef typename std::iterator<std::random_access_iterator_tag, TVal>::reference reference;
        typedef typename std::iterator<std::random_access_iterator_tag, TVal>::pointer pointer;

        TRandomIterator()
            : Data(0) {}

        template<typename T2>
        TRandomIterator(const TRandomIterator<T2>& r)
            : Data(&(*r)) {}

        TRandomIterator(pointer data)
            : Data(data) {}

        template<typename T2>
        TRandomIterator& operator=(const TRandomIterator<T2>& r) {
          Data = &(*r);
          return *this;
        }

        TRandomIterator& operator++() {
          ++Data;
          return *this;
        }

        TRandomIterator& operator--() {
          --Data;
          return *this;
        }

        TRandomIterator operator++(int) {
          return TRandomIterator(Data++);
        }

        TRandomIterator operator--(int) {
          return TRandomIterator(Data--);
        }

        TRandomIterator operator+(const difference_type& n) const {
          return TRandomIterator(Data + n);
        }

        TRandomIterator& operator+=(const difference_type& n) {
          Data += n;
          return *this;
        }

        TRandomIterator operator-(const difference_type& n) const {
          return TRandomIterator(pointer(Data - n));
        }

        TRandomIterator& operator-=(const difference_type& n) {
          Data -= n;
          return *this;
        }

        reference operator*() const {
          return *Data;
        }

        pointer operator->() const {
          return Data;
        }

        reference operator[](const difference_type& n) const {
          return Data[n];
        }

        template<typename T>
        friend bool operator==(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend bool operator!=(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend bool operator<(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend bool operator>(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend bool operator<=(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend bool operator>=(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend typename TRandomIterator<T>::difference_type operator+(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

        template<typename T>
        friend typename TRandomIterator<T>::difference_type operator-(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2);

      };  // TRandomIterator

      template<typename T>
      bool operator==(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data == r2.Data;
      }

      template<typename T>
      bool operator!=(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data != r2.Data;
      }

      template<typename T>
      bool operator<(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data < r2.Data;
      }

      template<typename T>
      bool operator>(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data > r2.Data;
      }

      template<typename T>
      bool operator<=(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data <= r2.Data;
      }

      template<typename T>
      bool operator>=(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data >= r2.Data;
      }

      template<typename T>
      typename TRandomIterator<T>::difference_type operator+(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return TRandomIterator<T>(r1.Data + r2.Data);
      }

      template<typename T>
      typename TRandomIterator<T>::difference_type operator-(const TRandomIterator<T>& r1, const TRandomIterator<T>& r2) {
        return r1.Data - r2.Data;
      }

      template <typename TVal, size_t MaxSize>
      class TSorter {
        NO_COPY(TSorter);
        public:

        class TCursor {
          NO_COPY(TCursor);
          public:

          virtual operator bool() const = 0;

          virtual const TVal &operator*() const = 0;

          virtual TCursor &operator++() = 0;

          virtual ~TCursor() {}

          protected:

          TCursor() {}

        };  // TCursor

        class TMemCursor
            : public TCursor {
          NO_COPY(TMemCursor);
          public:

          TMemCursor(TSorter *sorter)
              : Iter(sorter->begin()),
                End(sorter->end()) {}

          virtual ~TMemCursor() {}

          virtual operator bool() const {
            return Iter != End;
          }

          virtual const TVal &operator*() const {
            assert(Iter != End);
            return *Iter;
          }

          virtual TCursor &operator++() {
            assert(Iter != End);
            ++Iter;
            return *this;
          }

          private:

          TRandomIterator<TVal> Iter;

          TRandomIterator<TVal> End;

        };  // TMemCursor

        TSorter()
            : Size(0U) {
          Data = reinterpret_cast<TVal *>(malloc(sizeof(TVal) * MaxSize));
          if (Data == 0) {
            syslog(LOG_EMERG, "bad alloc in Util::Sorter [%ld]", sizeof(TVal) * MaxSize);
            throw std::bad_alloc();
          }
        }

        virtual ~TSorter() {
          free(Data);
        }

        const TVal &operator[](size_t pos) const {
          assert(pos < Size);
          return *(Data + pos);
        }

        template <class... Args>
        void Emplace(Args &&... args) {
          assert(Size < MaxSize);
          new (Data + Size) TVal(std::forward<Args>(args)...);
          ++Size;
        }

        size_t GetSize() const {
          return Size;
        }

        bool IsFull() const {
          return Size == MaxSize;
        }

        TRandomIterator<TVal> begin() const {
          return TRandomIterator<TVal>(Data);
        }

        TRandomIterator<TVal> end() const {
          return TRandomIterator<TVal>(Data + Size);
        }

        void Clear() {
          Size = 0U;
        }

        static size_t GetMaxSize() {
          return MaxSize;
        }

        private:

        size_t Size;

        TVal *Data;

      };  // TSorter

    }  // Util

  }  // Indy

}  // Orly