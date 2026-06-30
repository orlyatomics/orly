/* <orly/indy/file_sync.h>

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

#include <base/uuid.h>
#include <base/io/binary_input_only_stream.h>
#include <base/io/binary_output_only_stream.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/util/block_vec.h>

namespace Orly {

  namespace Indy {

    class TFileSync {
      public:

      /* This constructor is used on the read end. We default construct before we call read. */
      TFileSync() : Type(Destination), Engine(nullptr), StartingBlockId(0UL), StartingBlockOffset(0UL), Context(0UL) {}

      TFileSync(Disk::Util::TEngine *engine, const Base::TUuid &file_id, size_t gen_id, size_t context)
          : Type(Source), Engine(engine), FileId(file_id), GenId(gen_id), StartingBlockId(0UL), StartingBlockOffset(0UL), Context(context) {}

      ~TFileSync() {}

      void Write(Io::TBinaryOutputStream &stream) const;

      void Read(Io::TBinaryInputStream &stream);

      inline size_t GetStartingBlockId() const {
        return StartingBlockId;
      }

      inline size_t GetStartingBlockOffset() const {
        return StartingBlockOffset;
      }

      inline size_t GetFileLength() const {
        return BlockVec.Size() * Disk::Util::LogicalBlockSize;
      }

      private:

      enum TType {
        Source,
        Destination
      };

      /* This represents whether we are a Source (We will be written, should not be read), or a Destination (We will be read, should not be written) */
      TType Type;

      Disk::Util::TEngine *Engine;

      Base::TUuid FileId;

      size_t GenId;

      size_t StartingBlockId;
      size_t StartingBlockOffset;

      Indy::Util::TBlockVec BlockVec;

      size_t Context;

      static const size_t CopyBufSize;

    };  // TFileSync

    /* Binary streamers for Orly::Indy::TFileSync */
    inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const TFileSync &streamer) { streamer.Write(strm); return strm; }
    inline Io::TBinaryOutputStream &&operator<<(Io::TBinaryOutputStream &&strm, const TFileSync &streamer) { streamer.Write(strm); return std::move(strm); }
    inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, TFileSync &streamer) { streamer.Read(strm); return strm; }
    inline Io::TBinaryInputStream &&operator>>(Io::TBinaryInputStream &&strm, TFileSync &streamer) { streamer.Read(strm); return std::move(strm); }

  }  // Indy

}  // Orly
