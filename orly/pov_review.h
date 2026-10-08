/* <orly/pov_review.h>

   The results and options of the POV review workflow (#746), as both protocols carry them.  The
   operations themselves are in <orly/server/pov_review.h>; see docs/pov-review.md.

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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <base/io/binary_input_stream.h>
#include <base/io/binary_output_stream.h>
#include <base/json.h>
#include <orly/closure.h>
#include <orly/shared_enum.h>
#include <orly/var.h>

namespace Orly {

  /* How a new POV treats conflicts (see <orly/indy/fork_watch.h>). */
  enum class TConflictMode : char {

    /* Not tracked: the default, and free. */
    None = 'N',

    /* Tracked; a conflicting update promotes, and is reported. */
    Report = 'R',

    /* Tracked; a conflicting update doesn't promote, and the POV waits, blocked. */
    Refuse = 'F'

  };  // TConflictMode

  /* "none", "report" or "refuse"; throws std::invalid_argument for anything else. */
  TConflictMode ParseConflictMode(const std::string &text);

  const char *ToString(TConflictMode mode);

  /* What diff_pov reads (see DiffPov). */
  struct TPovDiffOptions {

    /* The default and largest page. */
    static constexpr uint64_t DefaultLimit = 100UL;
    static constexpr uint64_t MaxLimit = 10'000UL;

    /* Only keys at or after Start, and before Stop.  (Not `.from` and `.to`: those are keywords
       of the statement grammar.) */
    std::optional<Var::TVar> Start, Stop;

    /* Only keys after After: the `next` of the previous page (keyset paging, #735). */
    std::optional<Var::TVar> After;

    /* The most changes in the page.  A page never splits one key's changes in several value
       types, so it may hold a few more. */
    uint64_t Limit = DefaultLimit;

    /* The hook for named save points (#745): diff since a save point rather than against the
       parent.  Save points need versioned reads, which don't exist yet, so DiffPov refuses
       this; see docs/design/versioned-reads.md. */
    std::optional<std::string> Since;

    /* The binary protocol's form: a closure named "diff" with the options as arguments
       (.start, .stop, .after, .limit, .since). */
    TClosure ToClosure() const;

    static TPovDiffOptions FromClosure(const TClosure &closure);

    /* Sets one option, by name, as the WebSocket statement gives it; throws
       std::invalid_argument for an unknown name or a value of the wrong type. */
    void Set(const std::string &name, const Var::TVar &value);

  };  // TPovDiffOptions

  /* One key a POV changed relative to its parent. */
  struct TPovChange {

    enum class TKind : char {

      /* The key wasn't in the parent; the POV gave it a value. */
      Added = 'A',

      /* The key was in the parent; the POV gave it another value. */
      Changed = 'C',

      /* The key was in the parent; the POV deleted it. */
      Removed = 'R',

      /* The POV only applied commutative updates of one kind (`+=`, `|=`, ...) to the key:
         Delta is what they add up to, and Op the operator. */
      Delta = 'D'

    };  // TKind

    TKind Kind = TKind::Changed;

    Var::TVar Key;

    /* The value through the parent, when the page was read; unknown if absent. */
    std::optional<Var::TVar> Before;

    /* The value through the POV: Before with the POV's changes applied; unknown if absent. */
    std::optional<Var::TVar> After;

    /* Delta only. */
    std::optional<Var::TVar> Delta;
    TMutator Op = TMutator::Assign;

  };  // TPovChange

  /* A page of a diff. */
  struct TPovDiff {

    std::vector<TPovChange> Changes;

    /* The last key of the page if more follow, to pass as After for the next page. */
    std::optional<Var::TVar> Next;

    /* Next as an orlyscript literal, which round-trips exactly (JSON can't tell 1 from 1.0). */
    std::string NextLiteral;

    /* How many unpromoted updates of the POV the diff covers. */
    uint64_t Updates = 0UL;

  };  // TPovDiff

  /* What discard_pov threw away. */
  struct TPovDiscard {
    uint64_t Updates = 0UL;
    uint64_t Entries = 0UL;
  };  // TPovDiscard

  /* A conflict, as the review and promotion results give it. */
  struct TPovConflict {

    /* Numbered from 1 in the order found; 0 for a key the POV is blocked on. */
    uint64_t Number = 0UL;

    Var::TVar Key;

    /* The POV's update deletes the key (rather than overwriting it). */
    bool IsDelete = false;

    /* Refusing mode only: the parent changed the key between Tetris's test and the promotion,
       so it promoted anyway. */
    bool Raced = false;

  };  // TPovConflict

  /* What promote_pov did. */
  struct TPovPromote {

    enum class TStatus : char {

      /* Unpaused (if it was paused); Tetris is promoting the POV's updates. */
      Promoting = 'P',

      /* Refusing mode: some of the POV's updates would conflict, so it stays as it was.
         Conflicts lists them. */
      Refused = 'R'

    };  // TStatus

    TStatus Status = TStatus::Promoting;

    /* The POV's unpromoted updates when asked. */
    uint64_t Pending = 0UL;

    /* The number of the last conflict found before this promotion: the conflicts it finds are
       numbered after it (see review_pov). */
    uint64_t Mark = 0UL;

    std::vector<TPovConflict> Conflicts;

  };  // TPovPromote

  /* A POV's promotion progress and conflicts. */
  struct TPovReview {

    TConflictMode Mode = TConflictMode::None;

    /* 'N'ormal, 'P'aused or 'F'ailed. */
    char Status = 'N';

    /* Its unpromoted updates, and the entries in them. */
    uint64_t Pending = 0UL;
    uint64_t PendingEntries = 0UL;

    /* Refusing mode: Tetris is holding the POV back; BlockedOn lists the keys. */
    bool Blocked = false;
    std::vector<TPovConflict> BlockedOn;

    /* Conflicts numbered after the `after` asked for (the last 10,000 at most), and how many
       there have been. */
    std::vector<TPovConflict> Conflicts;
    uint64_t ConflictCount = 0UL;

    /* The changes in the parent chain recorded since the fork, and whether there were too many
       to keep (every overwrite then counts as a conflict). */
    uint64_t ChangedKeys = 0UL;
    bool Overflowed = false;

  };  // TPovReview

  /* A statement's options record, as name and value in order (see
     Client::Program::TranslateOptions), and what the review statements take from it; each throws
     std::invalid_argument for an option it doesn't know or a value of the wrong type. */
  using TOptionList = std::vector<std::pair<std::string, Var::TVar>>;

  /* `new ... pov ... <{.conflicts: "report"}>`. */
  TConflictMode GetNewPovOptions(const TOptionList &options);

  /* `diff_pov {id} <{.start: ..., .stop: ..., .after: ..., .limit: ..., .since: ...}>`. */
  TPovDiffOptions GetDiffOptions(const TOptionList &options);

  /* `promote_pov {id} <{.force: true}>`: force. */
  bool GetPromoteOptions(const TOptionList &options);

  /* `review_pov {id} <{.after: n}>`: the conflicts after n. */
  uint64_t GetReviewOptions(const TOptionList &options);

  /* For the WebSocket protocol. */
  Base::TJson ToJson(const TPovDiff &diff);
  Base::TJson ToJson(const TPovDiscard &discard);
  Base::TJson ToJson(const TPovPromote &promote);
  Base::TJson ToJson(const TPovReview &review);

  /* For the binary protocol. */
  void Write(Io::TBinaryOutputStream &strm, const TPovDiff &that);
  void Read(Io::TBinaryInputStream &strm, TPovDiff &that);
  void Write(Io::TBinaryOutputStream &strm, const TPovDiscard &that);
  void Read(Io::TBinaryInputStream &strm, TPovDiscard &that);
  void Write(Io::TBinaryOutputStream &strm, const TPovPromote &that);
  void Read(Io::TBinaryInputStream &strm, TPovPromote &that);
  void Write(Io::TBinaryOutputStream &strm, const TPovReview &that);
  void Read(Io::TBinaryInputStream &strm, TPovReview &that);

  /* Binary stream inserters and extractors. */
#define ORLY_POV_REVIEW_STREAMERS(T) \
  inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const T &that) { Write(strm, that); return strm; } \
  inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, T &that) { Read(strm, that); return strm; }
  ORLY_POV_REVIEW_STREAMERS(TPovDiff)
  ORLY_POV_REVIEW_STREAMERS(TPovDiscard)
  ORLY_POV_REVIEW_STREAMERS(TPovPromote)
  ORLY_POV_REVIEW_STREAMERS(TPovReview)
#undef ORLY_POV_REVIEW_STREAMERS

}  // Orly
