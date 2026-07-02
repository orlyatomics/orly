#!/bin/bash
#
# Copyright 2010-2026 Atomic Kismet Company
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

CC=g++
# Bootstrap stays one standard behind the main build (root.jhm runs
# -std=c++23) so a contributor with an older gcc can still build the two
# bootstrap binaries -- tools/jhm and tools/make_dep_file -- and run jhm
# at all. C++20 is the current floor (std::string::ends_with in
# make_dep_file); any gcc >= 10 suffices. If you use a newer language
# feature in these sources, bump this flag -- a mismatch only surfaces in
# from-scratch builds like CI's, not in incremental ones.
common_flags=(
  -O3 -DNDEBUG -flto=auto
  -std=c++20
  -Wall -Werror -Wextra -Wold-style-cast
  -Wno-unused -Wno-unused-parameter -Wno-unused-result
  -Wno-deprecated-declarations -Wno-deprecated -Wno-deprecated-copy
  -Wno-class-memaccess -Wno-stringop-overflow -Wno-stringop-overread
  -Wno-array-bounds -Wno-restrict -Wno-pessimizing-move -Wno-redundant-move
  -Wno-mismatched-new-delete -Wno-dangling-reference
  -Wl,--hash-style=gnu -Wl,--no-copy-dt-needed-entries -Wl,-z,relro -Wl,--no-as-needed
  )

#Build JHM
$CC -o tools/jhm                                                                                                                              \
 "${common_flags[@]}"                                                                                                                         \
  jhm/jobs/util.cc jhm/job.cc base/slice.cc jhm/test.cc jhm/jobs/flex.cc base/web/daemonize.cc                                                \
  base/io/input_consumer.cc base/io/output_consumer.cc base/thrower.cc base/strm/bin/in.cc jhm/naming.cc jhm/jhm.cc                           \
  base/io/output_producer.cc base/io/chunk_and_pool.cc base/pos.cc base/code_location.cc base/util/string.cc base/cmd.cc                      \
  base/demangle.cc base/piece.cc jhm/job_runner.cc base/subprocess.cc base/util/path.cc base/strm/bin/var_int.cc                              \
  jhm/env.cc jhm/jobs/compile_c_family.cc base/util/error.cc jhm/status_line.cc base/io/input_producer.cc                                     \
  jhm/work_finder.cc base/fd.cc base/pump.cc base/util/io.cc base/split.cc jhm/config.cc jhm/jobs/link.cc                                     \
  jhm/jobs/bison.cc base/strm/syntax_error.cc jhm/jobs/nycr.cc base/dir_walker.cc jhm/jobs/dep.cc base/util/time.cc                           \
  base/strm/out.cc base/event_semaphore.cc base/strm/in.cc base/strm/past_end.cc base/unreachable.cc base/path.cc base/backtrace.cc           \
  -I./ -DSRC_ROOT=\"`pwd`\"                                                                                                                   \
  -msse2 -pthread

#Build make_dep_file
$CC -o tools/make_dep_file                                                                                                                    \
  "${common_flags[@]}"                                                                                                                        \
  base/strm/bin/var_int.cc base/strm/syntax_error.cc base/strm/out.cc base/strm/past_end.cc base/strm/in.cc base/strm/bin/in.cc               \
  base/util/io.cc base/demangle.cc base/code_location.cc base/event_semaphore.cc base/util/error.cc base/unreachable.cc                       \
  jhm/make_dep_file.cc base/thrower.cc base/fd.cc base/split.cc base/subprocess.cc base/pump.cc base/backtrace.cc                             \
  base/web/daemonize.cc                                                                                                                       \
  -I./ -DSRC_ROOT=\"`pwd`\"                                                                                                                   \
  -msse2 -pthread

mkdir -p ../.jhm

#Build nycr
PATH="$PWD/tools:$PATH" ./tools/jhm -c bootstrap
