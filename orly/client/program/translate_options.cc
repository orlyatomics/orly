/* <orly/client/program/translate_options.cc>

   Implements <orly/client/program/translate_options.h>.

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

#include <orly/client/program/translate_options.h>

#include <orly/client/program/translate_expr.h>
#include <orly/var/sabot_to_var.h>

using namespace std;
using namespace Orly;
using namespace Orly::Client::Program;

TOptionList Orly::Client::Program::TranslateOptions(const TOptOptions *opt_options) {
  TOptionList result;
  auto options = dynamic_cast<const TOptions *>(opt_options);
  if (!options) {
    return result;
  }
  void *alloc = alloca(Sabot::State::GetMaxStateSize());
  auto list = dynamic_cast<const TObjMemberList *>(options->GetObjExpr()->GetOptObjMemberList());
  while (list) {
    auto member = list->GetObjMember();
    result.emplace_back(member->GetName()->GetLexeme().GetText(),
                        Var::ToVar(*Sabot::State::TAny::TWrapper(NewStateSabot(member->GetExpr(), alloc))));
    auto tail = dynamic_cast<const TObjMemberListTail *>(list->GetOptObjMemberListTail());
    list = tail ? tail->GetObjMemberList() : nullptr;
  }
  return result;
}
