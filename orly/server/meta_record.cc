/* <orly/server/meta_record.cc>

   Implements <orly/server/meta_record.h>.

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

#include <orly/server/meta_record.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>

#include <orly/var/int.h>
#include <orly/var/str.h>
#include <orly/var/util.h>

using namespace std;
using namespace Base;
using namespace Orly::Server;

/* Metadata for TMetaRecord::TEntry. */
RECORD_ELEM(TMetaRecord::TEntry, TUuid, SessionId);
RECORD_ELEM(TMetaRecord::TEntry, std::optional<TUuid>, UserId);
RECORD_ELEM(TMetaRecord::TEntry, TMetaRecord::TEntry::TPackageFqName, PackageFqName);
RECORD_ELEM(TMetaRecord::TEntry, string, MethodName);
RECORD_ELEM(TMetaRecord::TEntry, TMetaRecord::TEntry::TArgByName, ArgByName);
RECORD_ELEM(TMetaRecord::TEntry, TMetaRecord::TEntry::TExpectedPredicateResults, ExpectedPredicateResults);
RECORD_ELEM(TMetaRecord::TEntry, Base::Chrono::TTimePnt, RunTimestamp);
RECORD_ELEM(TMetaRecord::TEntry, uint64_t, RandomSeed);

/* Metadata for TMetaRecord. */
RECORD_ELEM(TMetaRecord, TMetaRecord::TEntryByUpdateId, EntryByUpdateId);

const TMetaRecord::TEntry &TMetaRecord::GetEntry(const TUuid &id) const {
  auto iter = EntryByUpdateId.find(id);
  assert(iter != EntryByUpdateId.end());
  return iter->second;
}

namespace {

  const string CallsKey = "$calls";
  const string PackageKey = "$package";
  const string MethodKey = "$method";

  /* Splits "<i>.<rest>" into i and rest. False if `key` has no index prefix. */
  bool SplitIndex(const string &key, size_t &index, string &rest) {
    auto dot = key.find('.');
    if (dot == string::npos || dot == 0) {
      return false;
    }
    auto [end, ec] = from_chars(key.data(), key.data() + dot, index);
    if (ec != errc() || end != key.data() + dot) {
      return false;
    }
    rest = key.substr(dot + 1);
    return true;
  }

  [[noreturn]] void ThrowMalformed(const string &why) {
    throw runtime_error("malformed meta record (#751): " + why);
  }

  const string &AsStr(const Orly::Var::TVar &var, const string &key) {
    auto *str = var.TryAs<Orly::Var::TStr>();
    if (!str) {
      ThrowMalformed("\"" + key + "\" is not a str");
    }
    return str->GetVal();
  }

}  // namespace

TMetaRecord::TEntry::TArgByName TMetaRecord::TEntry::EncodeBatch(const vector<TCall> &calls) {
  assert(!calls.empty());
  bool mixed = false;
  for (const auto &call: calls) {
    mixed = mixed || call.PackageFqName != calls.front().PackageFqName || call.MethodName != calls.front().MethodName;
  }
  TArgByName arg_by_name;
  arg_by_name.insert(make_pair(CallsKey, Var::TVar(static_cast<int64_t>(calls.size()))));
  for (size_t i = 0; i < calls.size(); ++i) {
    string prefix = to_string(i) + ".";
    for (const auto &item: calls[i].ArgByName) {
      arg_by_name.insert(make_pair(prefix + item.first, item.second));
    }
    if (mixed) {
      string package;
      for (const auto &part: calls[i].PackageFqName) {
        package += (package.empty() ? "" : "/") + part;
      }
      arg_by_name.insert(make_pair(prefix + PackageKey, Var::TVar(package)));
      arg_by_name.insert(make_pair(prefix + MethodKey, Var::TVar(calls[i].MethodName)));
    }
  }
  return arg_by_name;
}

vector<TMetaRecord::TEntry::TCall> TMetaRecord::TEntry::GetCalls() const {
  size_t count = 0;
  bool is_batch = false;
  auto calls_iter = ArgByName.find(CallsKey);
  if (calls_iter != ArgByName.end()) {
    auto *n = calls_iter->second.TryAs<Var::TInt>();
    if (!n || n->GetVal() < 1) {
      ThrowMalformed("\"$calls\" is not a positive int");
    }
    is_batch = true;
    count = static_cast<size_t>(n->GetVal());
  } else {
    /* A batch recorded before "$calls" existed: its calls are as many as its highest index
       prefix says. (Such a batch of calls without args can't be told from one call.) */
    size_t index;
    string rest;
    for (const auto &item: ArgByName) {
      if (SplitIndex(item.first, index, rest)) {
        is_batch = true;
        count = max(count, index + 1);
      }
    }
  }
  if (!is_batch) {
    return {TCall{PackageFqName, MethodName, ArgByName}};
  }
  vector<TCall> calls(count, TCall{PackageFqName, MethodName, TArgByName()});
  for (const auto &item: ArgByName) {
    if (item.first == CallsKey) {
      continue;
    }
    size_t index;
    string rest;
    if (!SplitIndex(item.first, index, rest)) {
      ThrowMalformed("batch arg \"" + item.first + "\" has no call index");
    }
    if (index >= count) {
      ThrowMalformed("batch arg \"" + item.first + "\" names a call past the " + to_string(count) + " recorded");
    }
    auto &call = calls[index];
    if (rest == PackageKey) {
      const auto &package = AsStr(item.second, item.first);
      call.PackageFqName.clear();
      for (size_t start = 0;;) {
        auto slash = package.find('/', start);
        call.PackageFqName.push_back(package.substr(start, slash - start));
        if (slash == string::npos) {
          break;
        }
        start = slash + 1;
      }
    } else if (rest == MethodKey) {
      call.MethodName = AsStr(item.second, item.first);
    } else {
      call.ArgByName.insert(make_pair(rest, item.second));
    }
  }
  return calls;
}
