/* <orly/rt.h>

   Umbrella header for the runtime helper library. Re-exports the
   rt module set (`built_in`, `collated_by`, `collected_by`,
   `desc`, `generator`, `mutable`, `opt`, `runtime_error`,
   `shortest_path`, `string`, `str_replace`, `time_diff_obj`,
   `time_pnt_obj`, `tuple`, `unknown`) plus `type/rt.h` for the
   C++-native -> orly `TType` bridge. One include for everything
   orlyc emits calls into.

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

#include <orly/rt/built_in.h>
#include <orly/rt/collated_by.h>
#include <orly/rt/collected_by.h>
#include <orly/rt/desc.h>
#include <orly/rt/generator.h>
#include <orly/rt/mutable.h>
#include <orly/rt/opt.h>
#include <orly/rt/runtime_error.h>
#include <orly/rt/shortest_path.h>
#include <orly/rt/string.h>
#include <orly/rt/str_replace.h>
#include <orly/rt/time_diff_obj.h>
#include <orly/rt/time_pnt_obj.h>
#include <orly/rt/tuple.h>
#include <orly/rt/unknown.h>
#include <orly/type/rt.h>