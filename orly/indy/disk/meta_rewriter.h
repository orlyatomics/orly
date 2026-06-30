/* <orly/indy/disk/meta_rewriter.h>

   One-shot utility that rewrites a data file's meta section in place.
   Used during recovery / repair when the meta block needs updating
   after the data blocks have been written. Returns the new meta
   location as a `(starting_block, offset)` pair.

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

#include <base/class_traits.h>
#include <orly/indy/disk/in_file.h>
#include <orly/indy/disk/out_stream.h>
#include <orly/indy/util/block_vec.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TMetaRewriter {
        NO_CONSTRUCTION(TMetaRewriter);
        public:

        typedef TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, 0UL> TInStream;
        typedef TOutStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage> TDataOutStream;

        static std::pair<size_t, size_t> RewriteMetaData(Util::TEngine *engine, const Indy::Util::TBlockVec &block_vec, size_t starting_block_offset);

      };  // TMetaRewriter

    }  // Disk

  }  // Indy

}  // Orly