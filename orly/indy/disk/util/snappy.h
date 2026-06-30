/* <orly/indy/disk/util/snappy.h>

   Snappy compression wrapped around the disk layer's stream / block
   interface. `TBlockSource` / `TBlockSink` adapt a `TBufBlock` to
   `snappy::Source` / `snappy::Sink`; `TStreamSource` / `TStreamSink`
   adapt the `TStream<>` / `TOutStream<>` cursors. Used wherever the
   on-disk format wants compression (notably `TIndexSortFile`'s body).

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

#include <memory>
#include <vector>

#include <string.h>

#include <snappy.h>
#include <snappy-sinksource.h>

#include <base/class_traits.h>
#include <orly/indy/disk/buf_block.h>
#include <orly/indy/disk/in_file.h>
#include <orly/indy/disk/out_stream.h>
#include <orly/indy/disk/util/volume_manager.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        namespace Snappy {

          class TBlockSource
              : public snappy::Source {
            NO_COPY(TBlockSource);
            public:

            TBlockSource(const std::unique_ptr<TBufBlock> &buf);

            virtual ~TBlockSource();

            virtual size_t Available() const override;

            virtual const char* Peek(size_t *len) override;

            virtual void Skip(size_t n) override;

            private:

            const std::unique_ptr<TBufBlock> &Buf;

            size_t BytesRead;

          };  // TBlockSource

          template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind>
          class TStreamSource
              : public snappy::Source {
            NO_COPY(TStreamSource);
            public:

            typedef TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, 0UL /*local cache */> TDataInStream;

            TStreamSource(TDataInStream &in_stream, size_t avail_bytes)
              : InStream(in_stream), AvailBytes(avail_bytes) {
            }

            virtual ~TStreamSource() {}

            inline virtual size_t Available() const override {
              return AvailBytes;
            }

            inline virtual const char* Peek(size_t *len) override {
              size_t offset_in_chunk = InStream.GetOffsetInChunk();
              *len = std::min(TDataInStream::DataChunkSize - offset_in_chunk, AvailBytes);
              return InStream.GetData();
            }

            inline virtual void Skip(size_t n) override {
              assert(AvailBytes >= n);
              AvailBytes -= n;
              InStream.Skip(n);
            }

            private:

            TDataInStream &InStream;

            size_t AvailBytes;

          };  // TStreamSource

          template <size_t PageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind>
          class TBlockSink
              : public snappy::Sink {
            NO_COPY(TBlockSink);
            public:

            typedef TOutStream<PageSize, BlockSize, PhysicalBlockSize, BufKind> TDataOutStream;

            TBlockSink(TDataOutStream &out_stream)
              : OutStream(out_stream) {}

            virtual ~TBlockSink() {}

            inline virtual void Append(const char *bytes, size_t n) override {
              OutStream.Write(bytes, n);
            }

            private:

            TDataOutStream &OutStream;

          };  // TBlockSink

          class TIoStreamSource
              : public snappy::Source {
            NO_COPY(TIoStreamSource);
            public:

            TIoStreamSource(Io::TBinaryInputStream &stream, size_t avail_bytes)
              : InStream(stream), AvailBytes(avail_bytes) {}

            virtual ~TIoStreamSource() {}

            inline virtual size_t Available() const override {
              return AvailBytes;
            }

            inline virtual const char* Peek(size_t *len) override {
              *len = std::min(InStream.GetPeekSize(), AvailBytes);
              return InStream.Peek();
            }

            inline virtual void Skip(size_t n) override {
              assert(AvailBytes >= n);
              AvailBytes -= n;
              InStream.SkipExactly(n);
            }

            private:

            Io::TBinaryInputStream &InStream;

            size_t AvailBytes;

          };  // TIoStreamSource

          class TIoStreamSink
              : public snappy::Sink {
            NO_COPY(TIoStreamSink);
            public:

            TIoStreamSink(Io::TBinaryOutputStream &stream)
              : OutStream(stream) {}

            virtual ~TIoStreamSink() {}

            inline virtual void Append(const char *bytes, size_t n) override {
              OutStream.WriteExactly(bytes, n);
            }

            private:

            Io::TBinaryOutputStream &OutStream;

          };  // TIoStreamSink

          class TRawSink
              : public snappy::Sink {
            NO_COPY(TRawSink);
            public:

            TRawSink(char *buf, size_t max_size)
              : Buf(buf), BytesIn(0UL), MaxBytes(max_size) {}

            virtual ~TRawSink() {}

            inline virtual void Append(const char *bytes, size_t n) override {
              assert(BytesIn + n <= MaxBytes);
              memcpy(&Buf[BytesIn], bytes, n);
              BytesIn += n;
              assert(BytesIn <= MaxBytes);
            }

            private:

            char *Buf;
            size_t BytesIn;
            size_t MaxBytes;

          };  // TRawSink

        }  // Snappy

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly