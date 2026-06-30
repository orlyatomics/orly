/* <orly/indy/disk_layer.h>

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

#include <cassert>

#include <base/class_traits.h>
#include <base/inv_con/ordered_list.h>
#include <orly/indy/manager_base.h>
#include <orly/indy/update.h>

namespace Orly {

  namespace Indy {

    class TDiskLayer
        : public L0::TManager::TRepo::TDataLayer {
      NO_COPY(TDiskLayer);
      public:

      TDiskLayer(L0::TManager *manager,
                 L0::TManager::TRepo *repo,
                 size_t gen_id,
                 size_t num_keys,
                 TSequenceNumber lowest_seq,
                 TSequenceNumber highest_seq);

      virtual ~TDiskLayer();

      virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &from,
                                                                     const TIndexKey &to) const override;

      /* exact_point is ignored: disk layers already seek via TIndexManager
         (hashed), so there is no head-scan to avoid (#257). */
      virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &key, bool exact_point = false) const override;

      virtual std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalker(TSequenceNumber from) const override;

      inline virtual TKind GetKind() const override;

      inline size_t GetGenId() const;

      inline size_t GetNumKeys() const;

      inline virtual size_t GetSize() const override;

      inline virtual TSequenceNumber GetLowestSeq() const override;

      inline virtual TSequenceNumber GetHighestSeq() const override;

      private:

      L0::TManager::TRepo *Repo;

      size_t GenId;

      size_t NumKeys;

      TSequenceNumber LowestSeq, HighestSeq;

    };  // TDiskLayer

    inline L0::TManager::TRepo::TDataLayer::TKind TDiskLayer::GetKind() const {
      return L0::TManager::TRepo::TDataLayer::Disk;
    }

    inline size_t TDiskLayer::GetGenId() const {
      return GenId;
    }

    inline size_t TDiskLayer::GetNumKeys() const {
      return NumKeys;
    }

    inline size_t TDiskLayer::GetSize() const {
      return NumKeys;
    }

    inline TSequenceNumber TDiskLayer::GetLowestSeq() const {
      return LowestSeq;
    }

    inline TSequenceNumber TDiskLayer::GetHighestSeq() const {
      return HighestSeq;
    }

  }  // Indy

}  // Orly

