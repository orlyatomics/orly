/* <gz/input_producer.h>

   Adapts a gzip-decoded `Gz::TFile` to `Io::TInputProducer` so
   gzipped input flows through the same chunk-and-pool pipeline as
   any other `Io` source. Construct with a path + mode or with a
   moved-in `TFd`; the producer pulls chunks from `File` and hands
   them up.

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
#include <utility>

#include <base/gz/file.h>
#include <base/io/chunk_and_pool.h>
#include <base/io/input_producer.h>

namespace Gz {

  class TInputProducer final
      : public Io::TInputProducer {
    NO_COPY(TInputProducer);
    public:

    using TChunk = Io::TChunk;
    using TPool  = Io::TPool;

    TInputProducer(const char *path, const char *mode, const TPool::TArgs &args = TPool::TArgs())
        : File(path, mode), Pool(std::make_shared<TPool>(args)) {}

    TInputProducer(Base::TFd &&fd, const char *mode, const TPool::TArgs &args = TPool::TArgs())
        : File(std::move(fd), mode), Pool(std::make_shared<TPool>(args)) {}

    /* See base class. */
    virtual std::shared_ptr<const TChunk> TryProduceInput() override;

    private:

    TFile File;

    std::shared_ptr<TPool> Pool;

  };  // TInputProducer

}  // Gz
