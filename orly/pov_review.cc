/* <orly/pov_review.cc>

   Implements <orly/pov_review.h>.

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

#include <orly/pov_review.h>

#include <memory>
#include <sstream>
#include <stdexcept>

#include <orly/atom/suprena.h>
#include <orly/method_result.h>
#include <orly/var/jsonify.h>
#include <orly/var/new_sabot.h>
#include <orly/var/sabot_to_var.h>

using namespace std;
using namespace Base;
using namespace Orly;

namespace {

  Var::TVar CoreToVar(const Atom::TCore &core, Atom::TCore::TArena *arena) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    return Var::ToVar(*Sabot::State::TAny::TWrapper(core.NewState(arena, state_alloc)));
  }

  const char *MutatorName(TMutator mutator) {
    switch (mutator) {
      case TMutator::Add: return "add";
      case TMutator::And: return "and";
      case TMutator::Assign: return "assign";
      case TMutator::Div: return "div";
      case TMutator::Exp: return "exp";
      case TMutator::Intersection: return "intersection";
      case TMutator::Max: return "max";
      case TMutator::Min: return "min";
      case TMutator::Mod: return "mod";
      case TMutator::Mult: return "mult";
      case TMutator::Or: return "or";
      case TMutator::SymmetricDiff: return "symmetric_diff";
      case TMutator::Sub: return "sub";
      case TMutator::Union: return "union";
      case TMutator::Xor: return "xor";
    }
    return "?";
  }

  const char *KindName(TPovChange::TKind kind) {
    switch (kind) {
      case TPovChange::TKind::Added: return "added";
      case TPovChange::TKind::Changed: return "changed";
      case TPovChange::TKind::Removed: return "removed";
      case TPovChange::TKind::Delta: return "delta";
    }
    return "?";
  }

  TJson OptToJson(const optional<Var::TVar> &var) {
    return var ? Var::ToJson(*var) : TJson();
  }

  TJson ConflictsToJson(const vector<TPovConflict> &conflicts) {
    TJson::TArray result;
    for (const auto &conflict: conflicts) {
      TJson::TObject obj;
      if (conflict.Number) {
        obj["number"] = conflict.Number;
      }
      obj["key"] = Var::ToJson(conflict.Key);
      obj["op"] = string(conflict.IsDelete ? "delete" : "put");
      if (conflict.Raced) {
        obj["raced"] = true;
      }
      result.push_back(TJson(std::move(obj)));
    }
    return TJson(std::move(result));
  }

  void WriteVar(Io::TBinaryOutputStream &strm, const Var::TVar &var) {
    auto arena = make_shared<Atom::TSuprena>();
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    Atom::TCore core(arena.get(), Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, var)).get());
    strm << TMethodResult(arena, core, nullopt);
  }

  void ReadVar(Io::TBinaryInputStream &strm, Var::TVar &var) {
    TMethodResult box;
    strm >> box;
    var = CoreToVar(box.GetValue(), box.GetArena().get());
  }

  void WriteOptVar(Io::TBinaryOutputStream &strm, const optional<Var::TVar> &var) {
    strm << static_cast<bool>(var);
    if (var) {
      WriteVar(strm, *var);
    }
  }

  void ReadOptVar(Io::TBinaryInputStream &strm, optional<Var::TVar> &var) {
    bool known;
    strm >> known;
    if (known) {
      var.emplace();
      ReadVar(strm, *var);
    } else {
      var.reset();
    }
  }

  void WriteConflicts(Io::TBinaryOutputStream &strm, const vector<TPovConflict> &conflicts) {
    strm << static_cast<uint64_t>(conflicts.size());
    for (const auto &conflict: conflicts) {
      strm << conflict.Number << conflict.IsDelete << conflict.Raced;
      WriteVar(strm, conflict.Key);
    }
  }

  void ReadConflicts(Io::TBinaryInputStream &strm, vector<TPovConflict> &conflicts) {
    uint64_t size;
    strm >> size;
    conflicts.clear();
    for (uint64_t i = 0; i < size; ++i) {
      TPovConflict conflict;
      strm >> conflict.Number >> conflict.IsDelete >> conflict.Raced;
      ReadVar(strm, conflict.Key);
      conflicts.push_back(std::move(conflict));
    }
  }

}  // namespace

TConflictMode Orly::ParseConflictMode(const string &text) {
  if (text == "none") {
    return TConflictMode::None;
  }
  if (text == "report") {
    return TConflictMode::Report;
  }
  if (text == "refuse") {
    return TConflictMode::Refuse;
  }
  throw invalid_argument("conflicts must be \"none\", \"report\" or \"refuse\", not \"" + text + "\"");
}

const char *Orly::ToString(TConflictMode mode) {
  switch (mode) {
    case TConflictMode::None: return "none";
    case TConflictMode::Report: return "report";
    case TConflictMode::Refuse: return "refuse";
  }
  return "?";
}

/* TPovDiffOptions. */

TClosure TPovDiffOptions::ToClosure() const {
  TClosure closure("diff");
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto add = [&closure, state_alloc](const string &name, const Var::TVar &var) {
    closure.AddArgBySabot(name, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, var)).get());
  };
  if (Start) {
    add("start", *Start);
  }
  if (Stop) {
    add("stop", *Stop);
  }
  if (After) {
    add("after", *After);
  }
  add("limit", Var::TVar(static_cast<int64_t>(Limit)));
  if (Since) {
    add("since", Var::TVar(*Since));
  }
  return closure;
}

TPovDiffOptions TPovDiffOptions::FromClosure(const TClosure &closure) {
  TPovDiffOptions options;
  for (const auto &item: closure.GetCoreByName()) {
    options.Set(item.first, CoreToVar(item.second, closure.GetArena().get()));
  }
  return options;
}

void TPovDiffOptions::Set(const string &name, const Var::TVar &value) {
  if (name == "start") {
    Start = value;
  } else if (name == "stop") {
    Stop = value;
  } else if (name == "after") {
    After = value;
  } else if (name == "limit") {
    int64_t limit;
    try {
      limit = Var::TVar::TDt<int64_t>::As(value);
    } catch (const exception &) {
      throw invalid_argument("diff_pov: .limit must be an int");
    }
    if (limit < 1 || static_cast<uint64_t>(limit) > MaxLimit) {
      ostringstream strm;
      strm << "diff_pov: .limit must be from 1 to " << MaxLimit;
      throw invalid_argument(strm.str());
    }
    Limit = static_cast<uint64_t>(limit);
  } else if (name == "since") {
    try {
      Since = Var::TVar::TDt<string>::As(value);
    } catch (const exception &) {
      throw invalid_argument("diff_pov: .since must be a str");
    }
  } else {
    throw invalid_argument("diff_pov: unknown option ." + name + "; the options are .start, .stop, .after, .limit and .since");
  }
}

/* JSON. */

TConflictMode Orly::GetNewPovOptions(const TOptionList &options) {
  TConflictMode mode = TConflictMode::None;
  for (const auto &option: options) {
    if (option.first != "conflicts") {
      throw invalid_argument("new pov: unknown option ." + option.first + "; the option is .conflicts");
    }
    string text;
    try {
      text = Var::TVar::TDt<string>::As(option.second);
    } catch (const exception &) {
      throw invalid_argument("new pov: .conflicts must be a str");
    }
    mode = ParseConflictMode(text);
  }
  return mode;
}

TPovDiffOptions Orly::GetDiffOptions(const TOptionList &options) {
  TPovDiffOptions result;
  for (const auto &option: options) {
    result.Set(option.first, option.second);
  }
  return result;
}

bool Orly::GetPromoteOptions(const TOptionList &options) {
  bool force = false;
  for (const auto &option: options) {
    if (option.first != "force") {
      throw invalid_argument("promote_pov: unknown option ." + option.first + "; the option is .force");
    }
    try {
      force = Var::TVar::TDt<bool>::As(option.second);
    } catch (const exception &) {
      throw invalid_argument("promote_pov: .force must be a bool");
    }
  }
  return force;
}

uint64_t Orly::GetReviewOptions(const TOptionList &options) {
  uint64_t after = 0UL;
  for (const auto &option: options) {
    if (option.first != "after") {
      throw invalid_argument("review_pov: unknown option ." + option.first + "; the option is .after");
    }
    int64_t value;
    try {
      value = Var::TVar::TDt<int64_t>::As(option.second);
    } catch (const exception &) {
      throw invalid_argument("review_pov: .after must be an int");
    }
    if (value < 0) {
      throw invalid_argument("review_pov: .after must not be negative");
    }
    after = static_cast<uint64_t>(value);
  }
  return after;
}

TJson Orly::ToJson(const TPovDiff &diff) {
  TJson::TArray changes;
  for (const auto &change: diff.Changes) {
    TJson::TObject obj;
    obj["key"] = Var::ToJson(change.Key);
    obj["kind"] = string(KindName(change.Kind));
    obj["before"] = OptToJson(change.Before);
    obj["after"] = OptToJson(change.After);
    if (change.Kind == TPovChange::TKind::Delta) {
      obj["op"] = string(MutatorName(change.Op));
      obj["delta"] = OptToJson(change.Delta);
    }
    changes.push_back(TJson(std::move(obj)));
  }
  TJson::TObject result;
  result["changes"] = TJson(std::move(changes));
  result["next"] = OptToJson(diff.Next);
  result["next_literal"] = diff.Next ? TJson(diff.NextLiteral) : TJson();
  result["updates"] = diff.Updates;
  return TJson(std::move(result));
}

TJson Orly::ToJson(const TPovDiscard &discard) {
  TJson::TObject result;
  result["discarded_updates"] = discard.Updates;
  result["discarded_entries"] = discard.Entries;
  return TJson(std::move(result));
}

TJson Orly::ToJson(const TPovPromote &promote) {
  TJson::TObject result;
  result["status"] = string(promote.Status == TPovPromote::TStatus::Refused ? "refused" : "promoting");
  result["pending"] = promote.Pending;
  result["mark"] = promote.Mark;
  result["conflicts"] = ConflictsToJson(promote.Conflicts);
  return TJson(std::move(result));
}

TJson Orly::ToJson(const TPovReview &review) {
  TJson::TObject result;
  result["conflict_mode"] = string(ToString(review.Mode));
  result["status"] = string(review.Status == 'P' ? "paused" : review.Status == 'F' ? "failed" : "normal");
  result["pending"] = review.Pending;
  result["pending_entries"] = review.PendingEntries;
  result["blocked"] = review.Blocked;
  result["blocked_on"] = ConflictsToJson(review.BlockedOn);
  result["conflicts"] = ConflictsToJson(review.Conflicts);
  result["conflict_count"] = review.ConflictCount;
  result["changed_keys"] = review.ChangedKeys;
  result["overflowed"] = review.Overflowed;
  return TJson(std::move(result));
}

/* Binary. */

void Orly::Write(Io::TBinaryOutputStream &strm, const TPovDiff &that) {
  strm << static_cast<uint64_t>(that.Changes.size());
  for (const auto &change: that.Changes) {
    strm << static_cast<char>(change.Kind) << static_cast<int32_t>(change.Op);
    WriteVar(strm, change.Key);
    WriteOptVar(strm, change.Before);
    WriteOptVar(strm, change.After);
    WriteOptVar(strm, change.Delta);
  }
  WriteOptVar(strm, that.Next);
  strm << that.NextLiteral << that.Updates;
}

void Orly::Read(Io::TBinaryInputStream &strm, TPovDiff &that) {
  uint64_t size;
  strm >> size;
  that.Changes.clear();
  for (uint64_t i = 0; i < size; ++i) {
    TPovChange change;
    char kind;
    int32_t op;
    strm >> kind >> op;
    change.Kind = static_cast<TPovChange::TKind>(kind);
    change.Op = static_cast<TMutator>(op);
    ReadVar(strm, change.Key);
    ReadOptVar(strm, change.Before);
    ReadOptVar(strm, change.After);
    ReadOptVar(strm, change.Delta);
    that.Changes.push_back(std::move(change));
  }
  ReadOptVar(strm, that.Next);
  strm >> that.NextLiteral >> that.Updates;
}

void Orly::Write(Io::TBinaryOutputStream &strm, const TPovDiscard &that) {
  strm << that.Updates << that.Entries;
}

void Orly::Read(Io::TBinaryInputStream &strm, TPovDiscard &that) {
  strm >> that.Updates >> that.Entries;
}

void Orly::Write(Io::TBinaryOutputStream &strm, const TPovPromote &that) {
  strm << static_cast<char>(that.Status) << that.Pending << that.Mark;
  WriteConflicts(strm, that.Conflicts);
}

void Orly::Read(Io::TBinaryInputStream &strm, TPovPromote &that) {
  char status;
  strm >> status >> that.Pending >> that.Mark;
  that.Status = static_cast<TPovPromote::TStatus>(status);
  ReadConflicts(strm, that.Conflicts);
}

void Orly::Write(Io::TBinaryOutputStream &strm, const TPovReview &that) {
  strm << static_cast<char>(that.Mode) << that.Status << that.Pending << that.PendingEntries << that.Blocked;
  WriteConflicts(strm, that.BlockedOn);
  WriteConflicts(strm, that.Conflicts);
  strm << that.ConflictCount << that.ChangedKeys << that.Overflowed;
}

void Orly::Read(Io::TBinaryInputStream &strm, TPovReview &that) {
  char mode;
  strm >> mode >> that.Status >> that.Pending >> that.PendingEntries >> that.Blocked;
  that.Mode = static_cast<TConflictMode>(mode);
  ReadConflicts(strm, that.BlockedOn);
  ReadConflicts(strm, that.Conflicts);
  strm >> that.ConflictCount >> that.ChangedKeys >> that.Overflowed;
}
