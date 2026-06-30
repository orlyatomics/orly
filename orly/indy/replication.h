/* <orly/indy/replication.h>

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

#include <optional>

#include <orly/atom/core_vector.h>
#include <orly/atom/core_vector_builder.h>
#include <orly/indy/transaction_base.h>
#include <orly/time.h>

namespace Orly {

  namespace Indy {

    class TIndexMapReplica {
      public:

      TIndexMapReplica() {}

      void Push(const Base::TUuid &index_id, const std::string &pkg_key, const Indy::TKey &val) {
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        CoreVecBuilder.Push(index_id);
        CoreVecBuilder.Push(pkg_key);
        CoreVecBuilder.PushState(val.GetState(state_alloc));
      }

      void Write(Io::TBinaryOutputStream &stream) const {
        CoreVecBuilder.Write(stream);
      }

      void Read(Io::TBinaryInputStream &stream) {
        assert(!CoreVec);
        CoreVec = std::make_unique<Atom::TCoreVector>(stream);
      }

      const Atom::TCoreVector &GetCoreVec() const {
        assert(CoreVec);
        return *CoreVec;
      }

      private:

      Atom::TCoreVectorBuilder CoreVecBuilder;
      std::unique_ptr<Atom::TCoreVector> CoreVec;

    };  // TIndexMapReplica

    enum TTransactionAction {
      Push,
      Pop,
      Fail,
      Pause,
      UnPause
    };

    class TReplicationQueue {
      NO_COPY(TReplicationQueue);
      public:

      /* Forward Declarations. */
      class TReplicationItem;

      typedef InvCon::UnorderedList::TCollection<TReplicationQueue, TReplicationItem> TItemCollection;

      class TReplicationItem {
        NO_COPY(TReplicationItem);
        public:

        typedef InvCon::UnorderedList::TMembership<TReplicationItem, TReplicationQueue> TQueueMembership;

        enum TKind {
          Repo,
          Durable,
          Transaction,
          IndexId
        };

        virtual ~TReplicationItem();

        virtual TKind GetKind() const = 0;

        protected:

        TReplicationItem();

        private:

        TQueueMembership::TImpl QueueMembership;

        friend class TReplicationQueue;

      };  // TReplicationItem

      TReplicationQueue();

      ~TReplicationQueue();

      void Insert(TReplicationItem *item) NO_THROW;

      void Swap(TReplicationQueue &that);

      inline void Clear();

      TItemCollection *GetItemCollection() const;

      inline bool IsEmpty() const;

      private:

      mutable TItemCollection::TImpl ItemCollection;

    };  // TReplicationQueue

    class TRepoReplication
        : public TReplicationQueue::TReplicationItem {
      NO_COPY(TRepoReplication);
      public:

      class TReplica {
        public:

        inline TReplica(const Base::TUuid &id, bool is_safe, const TTtl &ttl, const std::optional<Base::TUuid> &parent_id);

        inline TReplica(const TRepoReplication &replication_obj);

        inline TReplica(const TReplica &that);

        inline TReplica(TReplica &&that);

        inline const TTtl &GetTtl() const;

        inline const Base::TUuid &GetId() const;

        inline bool GetIsSafe() const;

        inline const std::optional<Base::TUuid> &GetOptParentId() const;

        private:

        const TTtl Ttl;

        const Base::TUuid Id;

        const bool IsSafe;

        const std::optional<Base::TUuid> OptParentId;

      };  // TReplica

      TRepoReplication(const Base::TUuid &repo_id, bool is_safe, const TTtl &ttl, const std::optional<Base::TUuid> &opt_parent_repo_id);

      virtual ~TRepoReplication();

      inline virtual TKind GetKind() const;

      inline const Base::TUuid &GetId() const;

      inline const TTtl &GetTtl() const;

      inline bool GetIsSafe() const;

      inline const std::optional<Base::TUuid> &GetOptParentRepoId() const;

      private:

      const TTtl Ttl;

      const Base::TUuid RepoId;

      const bool IsSafe;

      const std::optional<Base::TUuid> OptParentRepoId;

    };  // TRepoReplication

    class TDurableReplication
        : public TReplicationQueue::TReplicationItem {
      NO_COPY(TDurableReplication);
      public:

      class TReplica {
        public:

        inline TReplica(const Base::TUuid &id, const TTtl &ttl, const std::string &serialized_obj);

        inline TReplica(const TDurableReplication &replication_obj);

        inline TReplica(const TReplica &that);

        inline TReplica(TReplica &&that);

        inline const TTtl &GetTtl() const;

        inline const Base::TUuid &GetId() const;

        inline const std::string &GetSerializedObj() const;

        private:

        const TTtl Ttl;

        const Base::TUuid Id;

        const std::string SerializedObj;

      };  // TReplica

      TDurableReplication(const Base::TUuid &durable_id, const TTtl &ttl, const std::string &serialized_obj);

      virtual ~TDurableReplication();

      inline virtual TKind GetKind() const;

      inline const Base::TUuid &GetId() const;

      inline const TTtl &GetTtl() const;

      inline const std::string &GetSerializedObj() const;

      private:

      const TTtl DurableTtl;

      const Base::TUuid DurableId;

      const std::string SerializedObj;

    };  // TDurableReplication

    class TIndexIdReplication
        : public TReplicationQueue::TReplicationItem {
      NO_COPY(TIndexIdReplication);
      public:

      class TReplica {
        public:

        inline TReplica(const Base::TUuid &id, const std::string &pkg_key, const Indy::TKey &val);

        inline TReplica(const TIndexIdReplication &replication_obj);

        inline TReplica(const TReplica &that);

        inline TReplica(TReplica &&that);

        inline const Base::TUuid &GetId() const;

        inline const std::string &GetPkgKey() const;

        inline const Indy::TKey &GetVal() const;

        private:

        const Base::TUuid Id;

        std::string PkgKey;
        Indy::TKey Val;

        Atom::TSuprena Suprena;

      };  // TReplica

      TIndexIdReplication(const Base::TUuid &id, const std::string &pkg_key, const Indy::TKey &val);

      virtual ~TIndexIdReplication();

      inline virtual TKind GetKind() const;

      inline const Base::TUuid &GetId() const;

      inline const std::string &GetPkgKey() const;

      inline const Indy::TKey &GetVal() const;

      private:

      const Base::TUuid Id;

      Atom::TSuprena Suprena;

      const std::string PkgKey;
      Indy::TKey Val;

    };  // TIndexIdReplication

    class TTransactionReplication
          : public TReplicationQueue::TReplicationItem {
      NO_COPY(TTransactionReplication);
      public:

      TTransactionReplication();

      TTransactionReplication(L1::TTransaction::TReplica &&replica);

      virtual ~TTransactionReplication();

      inline void SwapReplica(L1::TTransaction::TReplica &&replica);

      inline virtual TKind GetKind() const;

      inline const L1::TTransaction::TReplica &GetReplica() const;

      private:

      L1::TTransaction::TReplica Replica;

    };  // TTransactionReplication

    class TReplicationStreamer {
      public:

      TReplicationStreamer();

      ~TReplicationStreamer();

      void Write(Io::TBinaryOutputStream &stream) const;

      void Read(Io::TBinaryInputStream &stream);

      void PushIndexId(const TIndexIdReplication &index_replica);

      void PushTransaction(const L1::TTransaction::TReplica &replica);

      void PushDurable(const TDurableReplication &durable_replica);

      void PushRepo(const TRepoReplication &repo_replica);

      inline const Atom::TCoreVector &GetIndexIdVec() const {
        assert(IndexIdVector);
        return *IndexIdVector;
      }

      inline const Atom::TCoreVector &GetRepoVec() const {
        assert(RepoVector);
        return *RepoVector;
      }

      inline const Atom::TCoreVector &GetDurableVec() const {
        assert(DurableVector);
        return *DurableVector;
      }

      inline const Atom::TCoreVector &GetTransactionVec() const {
        assert(TransactionVector);
        return *TransactionVector;
      }

      inline bool IsEmpty() const {
        return IndexIdBuilder.GetCores().empty() && RepoBuilder.GetCores().empty() && DurableBuilder.GetCores().empty() && TransactionBuilder.GetCores().empty();
      }

      private:

      Atom::TCoreVectorBuilder IndexIdBuilder;

      Atom::TCoreVectorBuilder RepoBuilder;

      Atom::TCoreVectorBuilder DurableBuilder;

      Atom::TCoreVectorBuilder TransactionBuilder;

      std::unique_ptr<Atom::TCoreVector> IndexIdVector;

      std::unique_ptr<Atom::TCoreVector> RepoVector;

      std::unique_ptr<Atom::TCoreVector> DurableVector;

      std::unique_ptr<Atom::TCoreVector> TransactionVector;

    };  // TReplicationStreamer

    /***************
      *** INLINE ***
      *************/

    inline bool TReplicationQueue::IsEmpty() const {
      return ItemCollection.IsEmpty();
    }

    inline void TReplicationQueue::Insert(TReplicationItem *item) NO_THROW {
      ItemCollection.Insert(&item->QueueMembership);
    }

    inline void TReplicationQueue::Clear() {
      ItemCollection.DeleteEachMember();
    }

    inline TReplicationQueue::TItemCollection *TReplicationQueue::GetItemCollection() const {
      return &ItemCollection;
    }

    inline TReplicationQueue::TReplicationItem::TKind TRepoReplication::GetKind() const {
      return TKind::Repo;
    }

    inline TReplicationQueue::TReplicationItem::TKind TDurableReplication::GetKind() const {
      return TKind::Durable;
    }

    inline const Base::TUuid &TDurableReplication::GetId() const {
      return DurableId;
    }

    inline const TTtl &TDurableReplication::GetTtl() const {
      return DurableTtl;
    }

    inline const std::string &TDurableReplication::GetSerializedObj() const {
      return SerializedObj;
    }

    inline TReplicationQueue::TReplicationItem::TKind TIndexIdReplication::GetKind() const {
      return TKind::IndexId;
    }

    inline const Base::TUuid &TIndexIdReplication::GetId() const {
      return Id;
    }

    inline const std::string &TIndexIdReplication::GetPkgKey() const {
      return PkgKey;
    }

    inline const Indy::TKey &TIndexIdReplication::GetVal() const {
      return Val;
    }

    inline TReplicationQueue::TReplicationItem::TKind TTransactionReplication::GetKind() const {
      return TKind::Transaction;
    }

    inline TRepoReplication::TReplica::TReplica(const Base::TUuid &id, bool is_safe, const TTtl &ttl, const std::optional<Base::TUuid> &opt_parent_id)
        : Ttl(ttl), Id(id), IsSafe(is_safe), OptParentId(opt_parent_id) {}

    inline TRepoReplication::TReplica::TReplica(const TRepoReplication &replication_obj)
        : Ttl(replication_obj.Ttl), Id(replication_obj.RepoId), IsSafe(replication_obj.IsSafe), OptParentId(replication_obj.OptParentRepoId) {}

    inline TRepoReplication::TReplica::TReplica(const TReplica &that)
        : Ttl(that.Ttl), Id(that.Id), IsSafe(that.IsSafe), OptParentId(that.OptParentId) {}

    inline TRepoReplication::TReplica::TReplica(TReplica &&that)
        : Ttl(that.Ttl), Id(std::move(that.Id)), IsSafe(that.IsSafe), OptParentId(std::move(that.OptParentId)) {}

    inline const TTtl &TRepoReplication::GetTtl() const {
      return Ttl;
    }

    inline const Base::TUuid &TRepoReplication::GetId() const {
      return RepoId;
    }

    inline bool TRepoReplication::GetIsSafe() const {
      return IsSafe;
    }

    inline const std::optional<Base::TUuid> &TRepoReplication::GetOptParentRepoId() const {
      return OptParentRepoId;
    }

    inline const TTtl &TRepoReplication::TReplica::GetTtl() const {
      return Ttl;
    }

    inline const Base::TUuid &TRepoReplication::TReplica::GetId() const {
      return Id;
    }

    inline bool TRepoReplication::TReplica::GetIsSafe() const {
      return IsSafe;
    }

    inline const std::optional<Base::TUuid> &TRepoReplication::TReplica::GetOptParentId() const {
      return OptParentId;
    }

    inline TDurableReplication::TReplica::TReplica(const Base::TUuid &id, const TTtl &ttl, const std::string &serialized_obj)
        : Ttl(ttl), Id(id), SerializedObj(serialized_obj) {}

    inline TDurableReplication::TReplica::TReplica(const TDurableReplication &replication_obj)
        : Ttl(replication_obj.DurableTtl), Id(replication_obj.DurableId), SerializedObj(replication_obj.SerializedObj) {}

    inline TDurableReplication::TReplica::TReplica(const TReplica &that)
        : Ttl(that.Ttl), Id(that.Id), SerializedObj(that.SerializedObj) {}

    inline TDurableReplication::TReplica::TReplica(TReplica &&that)
        : Ttl(that.Ttl), Id(std::move(that.Id)), SerializedObj(std::move(that.SerializedObj)) {}

    inline const TTtl &TDurableReplication::TReplica::GetTtl() const {
      return Ttl;
    }

    inline const Base::TUuid &TDurableReplication::TReplica::GetId() const {
      return Id;
    }

    inline const std::string &TDurableReplication::TReplica::GetSerializedObj() const {
      return SerializedObj;
    }

    inline const L1::TTransaction::TReplica &TTransactionReplication::GetReplica() const {
      return Replica;
    }

    inline void TTransactionReplication::SwapReplica(L1::TTransaction::TReplica &&replica) {
      Replica = std::move(replica);
    }

    /* Binary streamers for Orly::Indy::TReplicationStreamer */
    inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const TReplicationStreamer &streamer) { streamer.Write(strm); return strm; }
    inline Io::TBinaryOutputStream &&operator<<(Io::TBinaryOutputStream &&strm, const TReplicationStreamer &streamer) { streamer.Write(strm); return std::move(strm); }
    inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, TReplicationStreamer &streamer) { streamer.Read(strm); return strm; }
    inline Io::TBinaryInputStream &&operator>>(Io::TBinaryInputStream &&strm, TReplicationStreamer &streamer) { streamer.Read(strm); return std::move(strm); }

    /* Binary streamers for Orly::Indy::TIndexMapReplica */
    inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const TIndexMapReplica &streamer) { streamer.Write(strm); return strm; }
    inline Io::TBinaryOutputStream &&operator<<(Io::TBinaryOutputStream &&strm, const TIndexMapReplica &streamer) { streamer.Write(strm); return std::move(strm); }
    inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, TIndexMapReplica &streamer) { streamer.Read(strm); return strm; }
    inline Io::TBinaryInputStream &&operator>>(Io::TBinaryInputStream &&strm, TIndexMapReplica &streamer) { streamer.Read(strm); return std::move(strm); }

  }  // Indy

}  // Orly