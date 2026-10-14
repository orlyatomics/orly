/* <base/thread_local_global_pool.test.cc>

   Unit test for <base/thread_local_global_pool.h>

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

#include <base/thread_local_global_pool.h>

#include <condition_variable>
#include <thread>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Base;

class TObj;

class TObj
    : public TThreadLocalGlobalPoolManager<TObj>::TObjBase {
  NO_COPY(TObj);
  public:

  TObj() {}

  ~TObj() {}

  bool AssertCanFree() const {
    return true;
  }

};

static __thread TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool *LocalPool = nullptr;

FIXTURE(Typical) {
  const size_t pool_size = 20UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size);
  thread t1([&](){
    EXPECT_FALSE(LocalPool);
    LocalPool = new TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool(&manager);
    try {
      EXPECT_TRUE(LocalPool);
      TObj *obj = LocalPool->Alloc();
      EXPECT_TRUE(obj);
    } catch (...) {
      delete LocalPool;
      LocalPool = nullptr;
      throw;
    }
    delete LocalPool;
    LocalPool = nullptr;
  });
  t1.join();
}

FIXTURE(BadAlloc) {
  const size_t pool_size = 20UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size);
  bool caught_bad_alloc = false;
  thread t1([&](){
    EXPECT_FALSE(LocalPool);
    LocalPool = new TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool(&manager);
    try {
      EXPECT_TRUE(LocalPool);
      for (size_t i = 0; i < pool_size; ++i) {
        TObj *obj = LocalPool->Alloc();
        EXPECT_TRUE(obj);
      }
      try {
        LocalPool->Alloc();
        assert(false); /* We should be throwing a bad_alloc error */
      } catch (const std::bad_alloc &) {
        caught_bad_alloc = true;
      }
    } catch (...) {
      delete LocalPool;
      LocalPool = nullptr;
      throw;
    }
    delete LocalPool;
    LocalPool = nullptr;
  });
  t1.join();
  EXPECT_TRUE(caught_bad_alloc);
}

FIXTURE(SwapInFreePool) {
  const size_t pool_size = 20UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size);
  thread t1([&](){
    EXPECT_FALSE(LocalPool);
    LocalPool = new TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool(&manager);
    try {
      EXPECT_TRUE(LocalPool);
      for (size_t i = 0; i < pool_size * 10UL; ++i) {
        TObj *obj = LocalPool->Alloc();
        EXPECT_TRUE(obj);
        LocalPool->Free(obj);
      }
    } catch (...) {
      delete LocalPool;
      LocalPool = nullptr;
      throw;
    }
    delete LocalPool;
    LocalPool = nullptr;
  });
  t1.join();
}

FIXTURE(Borrowing) {
  const size_t pool_size = 5UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size * 2UL);
  std::mutex mut;
  std::condition_variable cond;
  size_t step = 0UL;
  thread t1([&](){
    EXPECT_FALSE(LocalPool);
    LocalPool = new TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool(&manager);
    try {
      EXPECT_TRUE(LocalPool);
      std::vector<TObj *> my_obj_vec;
      TObj *obj = LocalPool->Alloc();
      EXPECT_TRUE(obj);
      LocalPool->Free(obj);
      for (size_t i = 0; i < pool_size - 1; ++i) {
        TObj *obj = LocalPool->Alloc();
        EXPECT_TRUE(obj);
        my_obj_vec.emplace_back(obj);
      }
      /* next phase */ {
        std::lock_guard<std::mutex> lock(mut);
        step = 1UL;
        cond.notify_one();
      }
      /* wait for next phase */ {
        std::unique_lock<std::mutex> lock(mut);
        while (step != 2) {
          cond.wait(lock);
        }
      }
      for (auto obj : my_obj_vec) {
        LocalPool->Free(obj);
      }
    } catch (...) {
      delete LocalPool;
      LocalPool = nullptr;
      throw;
    }
    delete LocalPool;
    LocalPool = nullptr;
  });
  thread t2([&](){
    EXPECT_FALSE(LocalPool);
    LocalPool = new TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool(&manager);
    try {
      /* wait for next phase */ {
        std::unique_lock<std::mutex> lock(mut);
        while (step != 1) {
          cond.wait(lock);
        }
      }
      EXPECT_TRUE(LocalPool);
      std::vector<TObj *> my_obj_vec;
      for (size_t i = 0; i < pool_size + 1; ++i) {
        TObj *obj = LocalPool->Alloc();
        EXPECT_TRUE(obj);
        my_obj_vec.emplace_back(obj);
      }
      /* next phase */ {
        std::lock_guard<std::mutex> lock(mut);
        step = 2UL;
        cond.notify_one();
      }
      for (auto obj : my_obj_vec) {
        LocalPool->Free(obj);
      }
    } catch (...) {
      delete LocalPool;
      LocalPool = nullptr;
      throw;
    }
    delete LocalPool;
    LocalPool = nullptr;
  });
  t1.join();
  t2.join();
}

/* TryAlloc (#762) returns null instead of throwing, and with steal=false takes nothing from
   another pool's free queue: those are the objects other threads are waiting for. */
FIXTURE(TryAllocWithoutStealing) {
  const size_t pool_size = 4UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size);
  thread t1([&](){
    TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool owner(&manager), other(&manager);
    std::vector<TObj *> objs;
    for (size_t i = 0; i < pool_size; ++i) {
      TObj *obj = owner.TryAlloc(false);
      EXPECT_TRUE(obj);
      objs.push_back(obj);
    }
    /* The global pool is empty and nobody has freed anything. */
    EXPECT_FALSE(owner.TryAlloc());
    EXPECT_FALSE(other.TryAlloc());
    /* One comes back to the owner's free queue. Without stealing, the other pool can't have
       it; stealing, it can. */
    owner.Free(objs.back());
    objs.pop_back();
    EXPECT_FALSE(other.TryAlloc(false));
    TObj *stolen = other.TryAlloc();
    EXPECT_TRUE(stolen);
    objs.push_back(stolen);
    /* Another comes back; the owner takes it without stealing. */
    owner.Free(objs.front());
    objs.erase(objs.begin());
    TObj *mine = owner.TryAlloc(false);
    EXPECT_TRUE(mine);
    objs.push_back(mine);
    EXPECT_FALSE(owner.TryAlloc());
    EXPECT_FALSE(other.TryAlloc());
    for (auto *obj : objs) {
      owner.Free(obj);
    }
  });
  t1.join();
}

/* A pool whose thread allocated a burst, freed it and went on to allocate again used to park the
   whole burst where only that thread can reach it, so another pool got none of it. It now keeps
   at most GetMaxParked() and hands the rest back. */
FIXTURE(BurstIsNotStranded) {
  const size_t pool_size = 1000UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size);
  const size_t max_parked = manager.GetMaxParked();
  EXPECT_LT(max_parked, pool_size / 2UL);
  thread t1([&](){
    TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool bursty(&manager), other(&manager);
    std::vector<TObj *> objs;
    for (size_t i = 0; i < pool_size; ++i) {
      objs.push_back(bursty.Alloc());
    }
    for (auto *obj : objs) {
      bursty.Free(obj);
    }
    objs.clear();
    /* This takes the burst off bursty's free queue, out of other's reach. */
    TObj *kept = bursty.Alloc();
    /* Without stealing, other gets only what went back to the global pool. */
    while (TObj *obj = other.TryAlloc(false)) {
      objs.push_back(obj);
    }
    EXPECT_GE(objs.size(), pool_size - max_parked);
    for (auto *obj : objs) {
      other.Free(obj);
    }
    bursty.Free(kept);
  });
  t1.join();
}

/* A pool that goes away hands back what it holds free, parked or on its free queue. */
FIXTURE(DestroyedPoolReturnsItsObjects) {
  const size_t pool_size = 100UL;
  TThreadLocalGlobalPoolManager<TObj> manager(pool_size);
  thread t1([&](){
    TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool other(&manager);
    /* gone */ {
      TThreadLocalGlobalPoolManager<TObj>::TThreadLocalPool gone(&manager);
      std::vector<TObj *> objs;
      for (size_t i = 0; i < pool_size; ++i) {
        objs.push_back(gone.Alloc());
      }
      /* Half parked, half on its free queue. */
      for (size_t i = 0; i < pool_size / 2UL; ++i) {
        gone.Free(objs[i]);
      }
      gone.Free(gone.Alloc());
      for (size_t i = pool_size / 2UL; i < pool_size; ++i) {
        gone.Free(objs[i]);
      }
    }
    std::vector<TObj *> objs;
    while (TObj *obj = other.TryAlloc(false)) {
      objs.push_back(obj);
    }
    EXPECT_EQ(objs.size(), pool_size);
    for (auto *obj : objs) {
      other.Free(obj);
    }
  });
  t1.join();
}
