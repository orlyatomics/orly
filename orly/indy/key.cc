/* <orly/indy/key.cc>

   Implements <orly/indy/key.h>.

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

#include <orly/indy/key.h>

#include <cassert>
#include <stdexcept>

using namespace Orly::Indy;

size_t TKey::GetNumDefined() const {
  if (!GetArena() || !GetCore().IsTuple()) {
    return 0UL;
  }
  const std::optional<Atom::TCore::TOffset> off = GetCore().TryGetOffset();
  if (!off) {
    return 0UL;
  }
  void *pin_alloc = alloca(sizeof(Atom::TCore::TArena::TFinalPin));
  Atom::TCore::TArena::TFinalPin::TWrapper pin(
      GetArena()->Pin(*off, sizeof(Atom::TCore::TNote) + (sizeof(Atom::TCore) * *GetCore().TryGetElemCount()), pin_alloc));
  return pin->GetNote()->GetTupleNumNonFree();
}

void TKey::CheckFreesTrail() const {
  if (!GetArena() || !GetCore().IsTuple()) {
    return;
  }
  const std::optional<Atom::TCore::TOffset> off = GetCore().TryGetOffset();
  assert(off);
  void *pin_alloc = alloca(sizeof(Atom::TCore::TArena::TFinalPin));
  Atom::TCore::TArena::TFinalPin::TWrapper pin(
      GetArena()->Pin(*off, sizeof(Atom::TCore::TNote) + (sizeof(Atom::TCore) * *GetCore().TryGetElemCount()), pin_alloc));
  if (!pin->GetNote()->HasTrailingFrees()) {
    throw std::invalid_argument("a pattern's free members must all follow its bound ones (orly/synth/db_keys_expr.cc)");
  }
}
