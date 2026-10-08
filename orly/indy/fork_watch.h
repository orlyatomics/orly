/* <orly/indy/fork_watch.h>

   Conflict tracking for a POV under review (#746).

   A POV made with a conflict mode carries a fork watch.  From the moment the POV is made (its
   fork), the watch records every key its parent chain changes -- the parent, the parent's parent,
   and so on up to the global POV -- except changes that only move content along that chain.  When
   one of the POV's own updates reaches its parent, any of its overwrites or deletes of a recorded
   key is a conflict: the POV is replacing a change it never saw.  The POV's commutative updates
   (`+=`, `|=` and the other defer-safe mutators) never conflict: they compose with whatever the
   parent did.  A commutative write in the parent is still a change, though, so the POV
   overwriting or deleting that key does conflict.

   What counts as a change in the chain.  Every repo in the chain calls OnAppend for every update
   it appends, under that repo's DataLock, with the repo it was promoted from, if any:
     - promoted from the POV itself: the POV's own update arriving in its parent.  Its overwrites
       and deletes of recorded keys are conflicts (recorded, numbered), and those keys stop being
       recorded changes, since the POV's version now replaces them.
     - promoted from a repo in the chain: content moving up the chain, which the POV could already
       see (it was there at the fork, or it was recorded when it first entered the chain).  Not a
       change.
     - anything else -- a write straight into a shared POV of the chain, or a promotion from a
       repo outside it, such as a sibling of the POV: a change.  Its keys are recorded.
   Because every chain append takes this watch's mutex inside its repo's DataLock, the arrival
   check and the recording are ordered exactly: a conflict is reported iff the parent chain
   changed the key after the fork and before the POV's update landed in the parent.

   The refusing mode (TMode::Refuse) also lets Tetris test an update before it promotes it
   (FindConflicts) and hold the POV back instead (Block).  That test runs against the changes
   recorded so far, outside the parent's DataLock, so a change that lands in the parent between the
   test and the promotion's commit is still overwritten; the arrival check then reports it as a
   conflict with Raced set.  The window is the same one Tetris's own assertion tests have.

   Memory: the watch copies each changed key once, into its own arena.  Past MaxKeys it stops
   recording and sets Overflowed; from then on every overwrite or delete counts as a conflict,
   because the watch can no longer tell.  Conflicts are numbered from 1; the watch keeps the last
   MaxConflicts of them.

   The watch lives only in the master's memory, like the POV's repo (POVs don't survive a restart,
   #439).  A slave doesn't keep one, so after a failover the POV promotes unwatched.

   Named save points (#745) would hang off this object: a save point is a version of the POV,
   and a diff or a discard "since a save point" needs versioned reads, which don't exist yet.  See
   docs/design/versioned-reads.md.

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

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <orly/atom/suprena.h>
#include <orly/indy/key.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    /* Forward declarations. */
    class TUpdate;

    class TForkWatch {
      NO_COPY(TForkWatch);
      public:

      /* What to do about a conflict. */
      enum class TMode : char {

        /* Promote anyway, and report the conflict. */
        Report = 'R',

        /* Hold the POV back: Tetris doesn't promote an update that would conflict. */
        Refuse = 'F'

      };  // TMode

      /* One conflict, as ForEachConflict and ForEachBlockingKey give it. */
      struct TConflict {

        /* Numbered from 1, in the order found.  0 for a key Block recorded. */
        uint64_t Number;

        /* The key, in the watch's arena; valid while the watch lives. */
        TIndexKey Key;

        /* True if the POV's update deletes the key; false if it overwrites it. */
        bool IsDelete;

        /* True if found on arrival in refusing mode: the parent changed the key between Tetris's
           test and the promotion's commit. */
        bool Raced;

      };  // TConflict

      /* The most keys recorded before the watch overflows (see above), and the most conflicts
         it keeps. */
      static constexpr size_t DefaultMaxKeys = 1'000'000UL;
      static constexpr size_t MaxConflicts = 10'000UL;

      /* A watch for the POV `pov_id`, whose parent chain, nearest first, is `chain`. */
      TForkWatch(const Base::TUuid &pov_id, std::vector<Base::TUuid> chain, TMode mode, size_t max_keys = DefaultMaxKeys);

      /* The POV this watches for. */
      const Base::TUuid &GetPovId() const {
        return PovId;
      }

      TMode GetMode() const {
        return Mode;
      }

      /* Called by a repo of the chain as it appends `update`, under that repo's DataLock;
         promoted_from is the repo Tetris promoted it from, or the zero id.  Never throws: if
         recording runs out of memory, the watch overflows. */
      void OnAppend(const TUpdate &update, const Base::TUuid &promoted_from) noexcept;

      /* The overwrites and deletes in `update` of keys the chain changed since the fork, in the
         update's order.  Tetris's test before it promotes, and the test of a whole backlog before
         a promotion is asked for. */
      std::vector<TConflict> FindConflicts(const TUpdate &update) const;

      /* True if the chain changed `key` since the fork (or the watch overflowed). */
      bool IsChanged(const TIndexKey &key) const;

      /* Tetris, in refusing mode, before it promotes the POV's update with sequence number
         seq_num: true if it must not (it would conflict, and seq_num is past any Force), and
         then the watch is blocked on the update's conflicting keys until a later call finds none.
         False, and unblocked, otherwise. */
      bool ShouldBlock(const TUpdate &update, TSequenceNumber seq_num);

      /* Let the POV's updates up to seq_num promote whatever they conflict with (they are still
         reported), and unblock. */
      void Force(TSequenceNumber seq_num);

      /* Forget every recorded change and the block, as if the POV had just been made: after a
         discard the POV is its parent again.  Conflicts found so far are kept, numbered as
         before. */
      void Refork();

      /* A snapshot of the watch's state. */
      struct TState {
        bool Blocked = false;
        bool Overflowed = false;
        size_t ChangedKeys = 0UL;
        uint64_t ConflictCount = 0UL;
        std::optional<TSequenceNumber> ForcedUpTo;
      };
      TState GetState() const;

      /* Calls back with each conflict kept whose number is greater than `after`, in order. */
      void ForEachConflict(uint64_t after, const std::function<void (const TConflict &)> &cb) const;

      /* Calls back with each key the watch is blocked on (none unless blocked). */
      void ForEachBlockingKey(const std::function<void (const TConflict &)> &cb) const;

      private:

      /* Copies `key` into Arena.  Call with Mutex held. */
      TIndexKey CopyKey(const TIndexKey &key);

      /* Appends a conflict.  Call with Mutex held. */
      void AddConflict(const TIndexKey &key, bool is_delete, bool raced);

      /* See accessors. */
      const Base::TUuid PovId;
      const std::vector<Base::TUuid> Chain;
      const TMode Mode;
      const size_t MaxKeys;

      /* Covers everything below. */
      mutable std::mutex Mutex;

      /* Holds the copies of keys in Changed, Conflicts and BlockingKeys.  It only grows, until
         Refork copies the conflicts it keeps into a new one. */
      std::unique_ptr<Atom::TSuprena> Arena;

      /* The keys the chain changed since the fork. */
      std::unordered_set<TIndexKey> Changed;

      /* True once Changed would have passed MaxKeys. */
      bool Overflowed = false;

      /* The last MaxConflicts conflicts, and how many there have been. */
      std::deque<TConflict> Conflicts;
      uint64_t ConflictCount = 0UL;

      /* What Tetris last blocked on; empty when it isn't blocked. */
      std::vector<TConflict> BlockingKeys;

      /* See Force(). */
      std::optional<TSequenceNumber> ForcedUpTo;

      /* True if ShouldBlock last let an update through that it found conflicts in, because it
         was forced: its conflicts on arrival are expected, not raced. */
      bool PromotingForced = false;

    };  // TForkWatch

  }  // Indy

}  // Orly
