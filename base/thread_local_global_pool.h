/* <base/thread_local_global_pool.h>

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
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <syslog.h>

#include <base/assert_true.h>
#include <base/demangle.h>
#include <base/class_traits.h>
#include <base/likely.h>
#include <base/mlock.h>
#include <base/spin_lock.h>
#include <base/inv_con/atomic_unordered_list.h>

namespace Base {

  template <typename TObj, typename... TArgs>
  class TThreadLocalGlobalPoolManager {
    NO_COPY(TThreadLocalGlobalPoolManager);
    public:

    /* Forward Declaration. */
    class TThreadLocalPool;

    class TObjBase {
      NO_COPY(TObjBase);
      public:

      protected:

      TObjBase()
          : NextObj(nullptr) {}

      private:

      TObjBase *NextObj;

      friend class TThreadLocalPool;
      friend class TThreadLocalGlobalPoolManager;

    };  // TObjBase

    class TThreadLocalPool {
      NO_COPY(TThreadLocalPool);
      public:

      static_assert(std::is_base_of<TObjBase, TObj>::value, "TThreadLocalPool requires TObj to be derived from TObjBase");

      typedef InvCon::AtomicUnorderedList::TMembership<TThreadLocalPool, TThreadLocalGlobalPoolManager> TManagerMembership;

      TThreadLocalPool(TThreadLocalGlobalPoolManager *manager)
          : FreeQueue(nullptr), AvailableQueue(nullptr), Manager(manager), ManagerMembership(this, &Base::AssertTrue(manager)->PoolCollection) {
        assert(manager);
      }

      /* Hands back everything still free here, so a pool that goes away doesn't take its
         objects with it. An object still in use mustn't be freed here afterwards. */
      virtual ~TThreadLocalPool() {
        if (AvailableQueue) {
          Manager->Return(AvailableQueue);
        }
        TObjBase *free_queue = FreeQueue.exchange(nullptr, std::memory_order_acquire);
        if (free_queue) {
          Manager->Return(free_queue);
        }
      }

      inline void Free(TObjBase *obj) {
        #ifndef NDEBUG
        static_cast<TObj *>(obj)->AssertCanFree();
        #endif
        /* Treiber-stack push onto the cross-thread free list. FreeQueue is a
           std::atomic so this read-modify-write doesn't race the atomic
           exchange-pop in TryAllocUncommon (the plain read here used to be a
           data race vs that exchange, even though the CAS made it benign on
           hardware -- TSan #269 follow-up). Release pairs with the acquire on
           the pop so the popper sees this object's NextObj link. */
        TObjBase *head = FreeQueue.load(std::memory_order_relaxed);
        do {
          obj->NextObj = head;
        } while (!FreeQueue.compare_exchange_weak(
            head, obj, std::memory_order_release, std::memory_order_relaxed));
      }

      inline TObj *Alloc() {
        TObj *obj = TryAlloc();
        if (!obj) {
          syslog(LOG_ERR, "Bad Alloc in TThreadLocalPool [%s]", Demangle<TObj>().get());
          throw std::bad_alloc();
        }
        return obj;
      }

      /* Like Alloc(), but returns null instead of logging and throwing when every pool is
         empty, for a caller that has something else to do then (#762). With steal false, it
         takes only from this pool and the global one, never from another thread's free queue:
         for optional work that must not take the objects other threads are waiting for. */
      inline TObj *TryAlloc(bool steal = true) {
        TObjBase *alloc_obj = AvailableQueue;
        if (alloc_obj) {
          /* Our available queue had something to offer. */
          AvailableQueue = alloc_obj->NextObj;
        } else {
          alloc_obj = TryAllocUncommon(steal);
        }
        return static_cast<TObj *>(alloc_obj);
      }

      inline TThreadLocalGlobalPoolManager *GetPoolManager() const {
        return Manager;
      }

      private:

      TObjBase *TryAllocUncommon(bool steal = true) {
        TObjBase *alloc_obj = nullptr;
        /* let's swap in our free queue and try to allocate from that. */
        TObjBase *cur_tail = FreeQueue.exchange(nullptr, std::memory_order_acquire);
        if (cur_tail) {
          /* There were element(s) on the free queue. */
          assert(AvailableQueue == nullptr);
          MakeStackAvailable(cur_tail);
          assert(AvailableQueue != nullptr);
          alloc_obj = AvailableQueue;
          AvailableQueue = alloc_obj->NextObj;

        } else {
          /* the free queue was empty as well. Time to borrow from the global pool */
          alloc_obj = Manager->TryAlloc();
          if (!alloc_obj && steal) {
            /* the global pool was empty as well. Time to try to borrow from other pools...
               - We want to borrow starting from our right neighbor.
               - Until AtomicUnorderedList supports a "ForEachFromThis" we will use it (possibly) twice to scan right from our current member
            */

            bool skip_to = true;
            bool reached = false;
            auto for_each_cb = [this, &cur_tail, &alloc_obj, &skip_to, &reached](const TThreadLocalPool &pool) {
              if (skip_to) {
                /* we skip until we reach this */
                if (!reached && &pool != this) {
                  return true;
                } else {
                  reached = true;
                }
              } else {
                /* we pass through until we reach this */
                if (&pool == this) {
                  return false;
                }
              }
              if (&pool != this) {
                cur_tail = pool.FreeQueue.exchange(nullptr, std::memory_order_acquire);
                if (cur_tail) {
                  /* There were element(s) on the other pool's free queue. */
                  assert(AvailableQueue == nullptr);
                  MakeStackAvailable(cur_tail);
                  assert(AvailableQueue != nullptr);
                  alloc_obj = AvailableQueue;
                  AvailableQueue = alloc_obj->NextObj;
                  return false;
                }
              }
              return true;
            };
            Manager->PoolCollection.ForEach(for_each_cb);
            if (!alloc_obj) {
              /* we looked at all the neigbors to our right. Now we start from the left and keep going till we see oursleves. */
              skip_to = false;
              Manager->PoolCollection.ForEach(for_each_cb);
            }
          }
        }
        return alloc_obj;
      }

      /* Takes a stack swapped in from a free queue, ours or another pool's, as our available
         queue. Only this thread can pop that queue, so nothing parked on it can be stolen: a
         pool that keeps a whole burst's worth there keeps it for good once its thread goes
         idle, and others run dry while most of the objects sit free. So keep at most
         Manager->MaxParked and hand the rest back to the global pool. */
      inline void MakeStackAvailable(TObjBase *cur_tail) {
        assert(cur_tail);
        AvailableQueue = cur_tail;
        TObjBase *last_kept = cur_tail;
        for (size_t kept = 1UL; kept < Manager->MaxParked && last_kept->NextObj; ++kept) {
          last_kept = last_kept->NextObj;
        }
        TObjBase *const excess = last_kept->NextObj;
        if (excess) {
          last_kept->NextObj = nullptr;
          Manager->Return(excess);
        }
      }

      /* Cross-thread free list (Treiber stack). Atomic because Free() (push,
         any thread) races TryAllocUncommon()'s exchange-pop (this or a
         neighbor pool stealing work). */
      mutable std::atomic<TObjBase *> FreeQueue;

      TObjBase *AvailableQueue;

      TThreadLocalGlobalPoolManager *Manager;

      TObj *MyAlloc;

      typename TManagerMembership::TImpl ManagerMembership;

    };  // TThreadLocalPool

    TThreadLocalGlobalPoolManager(size_t num_elems, const TArgs &... args)
        : NumElems(num_elems), MaxParked(ChooseMaxParked(num_elems)), Alloc(nullptr), PoolCollection(this) {
      Alloc = reinterpret_cast<TObj *>(malloc(sizeof(TObj) * num_elems));
      MlockN(*Alloc, num_elems);
      if (unlikely(!Alloc)) {
        throw std::bad_alloc();
      }
      TObjBase *prev_base = nullptr;
      for (size_t i = 0; i < num_elems; ++i) {
        TObj *const obj = new (Alloc + i) TObj(args...);
        TObjBase *const obj_base = static_cast<TObjBase *>(obj);
        obj_base->NextObj = prev_base;
        prev_base = obj_base;
      }
      FreeQueue = prev_base;
    }

    ~TThreadLocalGlobalPoolManager() {
      size_t counter = 0UL;
      PoolCollection.ForEach([&counter](const TThreadLocalPool &){++counter; return true;});
      if (counter != 0UL) {
        /* A destructor is noexcept, so throwing here would terminate anyway; fail fast explicitly. */
        syslog(LOG_ERR, "TThreadLocalGlobalPoolManager should not be destroyed while owning any pools");
        std::fputs("TThreadLocalGlobalPoolManager should not be destroyed while owning any pools\n", stderr);
        std::abort();
      }
      for (size_t i = 0; i < NumElems; ++i) {
        TObj *const obj = Alloc + i;
        obj->~TObj();
      }
      free(Alloc);
    }

    /* The most free objects a thread-local pool keeps for itself; see MakeStackAvailable. */
    size_t GetMaxParked() const {
      return MaxParked;
    }

    private:

    /* A 256th of the objects, but at least 16, so that dozens of pools can't strand more than
       a small share between them while a pool still reuses what it frees without a lock. */
    static constexpr size_t ChooseMaxParked(size_t num_elems) {
      return std::max<size_t>(16UL, num_elems / 256UL);
    }

    /* Pushes a null-terminated stack of free objects onto the global free list. */
    void Return(TObjBase *stack) {
      assert(stack);
      TObjBase *tail = stack;
      while (tail->NextObj) {
        tail = tail->NextObj;
      }
      Base::TSpinLock::TLock lock(FreeQueueLock);
      tail->NextObj = FreeQueue;
      FreeQueue = stack;
    }

    inline TObjBase *TryAlloc() {
      TObjBase *obj = nullptr;
      /* for now we use a spin lock. Anyone allocating from here will contend for this. The idea is that we finely distribute objects to the thread
         local pools. Once they are there they can be allocated without a lock. We will hit this function often at the early life of a local pool
         and then less often later. Consider it a warm-up semantic. */
      Base::TSpinLock::TLock lock(FreeQueueLock);
      obj = FreeQueue;
      if (obj) {
        FreeQueue = obj->NextObj;
      }
      return obj;
    }

    typedef InvCon::AtomicUnorderedList::TCollection<TThreadLocalGlobalPoolManager, TThreadLocalPool> TPoolCollection;

    const size_t NumElems;

    const size_t MaxParked;

    TObj *Alloc;

    Base::TSpinLock FreeQueueLock;
    TObjBase *FreeQueue;

    mutable typename TPoolCollection::TImpl PoolCollection;

  };  // TThreadLocalGlobalPoolManager

}  // Base
