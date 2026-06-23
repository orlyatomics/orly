/* <orly/spa/flux_capacitor/sync.cc>

   Implements <orly/spa/flux_capacitor/sync.h>.

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

#include <orly/spa/flux_capacitor/sync.h>

#include <base/util/error.h>

using namespace Base;
using namespace Orly::Spa::FluxCapacitor;

#ifndef NDEBUG
TSyncException TSyncExcept;
#endif

TSync::TSync() : KeyInitialized(false), RwLockInitialized(false) {
  try {
    Util::IfNe0(pthread_key_create(&Key, TLocal::DeleteLocal));
    KeyInitialized = true;
    Util::IfNe0(pthread_rwlock_init(&RwLock, 0));
    RwLockInitialized = true;
  } catch (...) {
    Cleanup();
    throw;
  }
}

TSync::~TSync() {
  Cleanup();
}

void TSync::TLocal::DecrLockCount() {
  /* Make sure we're not decrementing a zero. */
  assert(LockCount > 0);
  --LockCount;
  if (!LockCount) {
    pthread_rwlock_unlock(&Sync.RwLock);
  }
}

void TSync::TLocal::IncrLockCount(bool is_exclusive) {
  assert(LockCount >= 0);
  /* Make sure this thread either
        (1) has not yet locked this target,
        (2) already has a shared or exclusive lock and is requesting an additional shared lock (which is always ok), or
        (3) already has an exclusive lock on the target (which makes requesting any addtional lock ok).
     Requesting an exclusive lock when the thread already has a shared lock is explicitly not allowed.  This assertion is
     meant to catch that case, which is always a programming error. */
  #ifndef NDEBUG
  if (!(LockCount == 0 || !is_exclusive || IsExclusive)) {
    throw TSyncExcept;
  }
  #endif
  if (!LockCount) {
    Util::IfNe0((is_exclusive ? pthread_rwlock_wrlock : pthread_rwlock_rdlock)(&Sync.RwLock));
    IsExclusive = is_exclusive;
  }
  ++LockCount;
  /* Make sure the increment didn't the counter over. */
  assert(LockCount >= 0);
}

void TSync::TLocal::DeleteLocal(void *arg) {
  auto *local = static_cast<TLocal *>(arg);
  /* Stop tracking before we free, so a concurrent Cleanup() can't also reclaim this one. */
  {
    std::lock_guard<std::mutex> lock(local->Sync.LocalsLock);
    local->Sync.Locals.erase(local);
  }
  delete local;
}

TSync::TLocal *TSync::TLocal::GetLocal(const TSync &sync) {
  TLocal *local = static_cast<TLocal *>(pthread_getspecific(sync.Key));
  if (!local) {
    local = new TLocal(sync);
    try {
      Util::IfNe0(pthread_setspecific(sync.Key, local));
    } catch (...) {
      delete local;
      throw;
    }
    /* Track the new local so Cleanup() can reclaim it if its thread never runs DeleteLocal
       (e.g. the thread outlives the target, or the process tears down after pthread_key_delete). */
    std::lock_guard<std::mutex> lock(sync.LocalsLock);
    sync.Locals.insert(local);
  }
  return local;
}

TSync::TLocal::TLocal(const TSync &sync) : Sync(sync), LockCount(0) {}

TSync::TLocal::~TLocal() {}

void TSync::Cleanup() {
  if (RwLockInitialized) {
    pthread_rwlock_destroy(&RwLock);
  }
  if (KeyInitialized) {
    /* Delete the key first: pthread_key_delete() does not run the key's destructor, but it does guarantee no further
       TLocal can be registered against this target while we mow down the survivors below.  Any TLocal still tracked here
       belongs to a thread that locked the target earlier and has not yet terminated (its DeleteLocal never ran); without
       this sweep those objects would leak. */
    pthread_key_delete(Key);
    std::lock_guard<std::mutex> lock(LocalsLock);
    for (TLocal *local : Locals) {
      delete local;
    }
    Locals.clear();
  }
}
