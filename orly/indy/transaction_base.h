/* <orly/indy/transaction_base.h>

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

#include <orly/indy/manager_base.h>
#include <orly/indy/repo.h>
#include <orly/indy/status.h>

namespace Orly {

  namespace Indy {

    /* Forward Delcarations. */
    class TTransactionReplication;
    class TReplicationStreamer;

    namespace L1 {

      /* Forward Delcarations. */
      class TManager;

      class TTransaction {
        NO_COPY(TTransaction);
        public:

        typedef InvCon::UnorderedList::TMembership<TTransaction, TManager> TManagerMembership;

        bool Push(const L0::TManager::TPtr<TRepo> &repo,
                  const std::shared_ptr<TUpdate> &update,
                  const std::optional<TSequenceNumber> &ensure_or_discard = std::optional<TSequenceNumber>());

        bool Pop(const L0::TManager::TPtr<TRepo> &repo,
                 const std::optional<TSequenceNumber> &ensure_or_discard = std::optional<TSequenceNumber>());

        bool Fail(const L0::TManager::TPtr<TRepo> &repo,
                  const std::optional<TSequenceNumber> &follow_or_discard = std::optional<TSequenceNumber>());

        bool Pause(const L0::TManager::TPtr<TRepo> &repo,
                   const std::optional<TSequenceNumber> &follow_or_discard = std::optional<TSequenceNumber>());

        bool UnPause(const L0::TManager::TPtr<TRepo> &repo,
                     const std::optional<TSequenceNumber> &follow_or_discard = std::optional<TSequenceNumber>());

        const std::shared_ptr<TUpdate> &Peek(const L0::TManager::TPtr<TRepo> &repo);

        void Prepare();

        /* Mark the prepared action to be committed.  The commitment will occur when this object destructs.
           If there is no prepared action, do nothing. */
        void CommitAction() NO_THROW;

        /* Discard the prepared action, if any. */
        void DiscardAction() NO_THROW;

        /* Test-only instrumentation hook, invoked at commit time inside
           ~TTransaction *between* the pusher-apply pass (which makes the
           promoted update visible in the parent repo via AppendUpdate) and
           the popper-apply pass (which removes it from the child repo). It
           lets a unit test deterministically observe the cross-repo
           transition and assert there is no "neither" transient. Defaults
           to empty; never set in production, so it is a single null check
           on the commit path. See orly/indy/transaction_base.cc. */
        static std::function<void ()> OnCommitBetweenPushAndPopForTest;

        class TTransactionCompletion {
          NO_COPY(TTransactionCompletion);
          public:

          enum TTransactionResult {
            Completed,
            Failed
          };

          typedef InvCon::UnorderedList::TMembership<TTransactionCompletion, TManager> TManagerMembership;

          TTransactionCompletion(TManager *manager);

          ~TTransactionCompletion();

          void RegisterCompletion();

          void RegisterFailure();

          void SetWaitFor(size_t wait_for);

          inline void SetCb(const std::shared_ptr<std::function<void (TTransactionResult)>> &cb);

          private:

          size_t WaitFor;

          size_t NumCompleted;
          std::mutex Mutex;

          std::shared_ptr<std::function<void (TTransactionResult)>> Cb;

          TManagerMembership::TImpl ManagerMembership;

        };  // TTransactionCompletion

        class TReplica {
          public:

          /* Forward Declarations. */
          class TMutation;

          typedef std::list<TMutation> TMutationList;

          class TMutation {
            public:

            enum TKind {
              Pusher,
              Popper,
              Failer,
              Pauser,
              UnPauser
            };

            class TUpdate {
              public:

              typedef std::vector<std::pair<TIndexKey, Atom::TCore>> TOpByKey;

              TUpdate();

              TUpdate(TUpdate &&update);

              TUpdate(const TUpdate &update);

              TUpdate(const Orly::Indy::TUpdate *update);

              ~TUpdate();

              TUpdate &operator=(TUpdate &&that);

              TUpdate &operator=(const TUpdate &that);

              inline const Atom::TCore &GetMetadata() const;

              inline const Atom::TCore &GetId() const;

              inline const TOpByKey &GetOpByKey() const;

              inline const std::shared_ptr<Atom::TSuprena> &GetSuprena() const;

              inline void SetArena(const std::shared_ptr<Atom::TSuprena> &suprena);

              void Write(Io::TBinaryOutputStream &strm, const Atom::TCore::TRemap &remap) const;

              void Read(Io::TBinaryInputStream &strm, const Atom::TCore::TRemap &remap);

              private:

              std::shared_ptr<Atom::TSuprena> Suprena;

              Atom::TCore Metadata;

              Atom::TCore Id;

              TOpByKey OpByKey;

              friend class Orly::Indy::TReplicationStreamer;

            };  // TUpdate

            TMutation(TMutation &&mutation);

            TMutation(const TMutation &mutation);

            TMutation(TKind kind, const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num);

            TMutation(TKind kind, const Base::TUuid &repo_id, const Orly::Indy::TUpdate *update, const std::optional<TSequenceNumber> &seq_num);

            TMutation(TKind kind, const Base::TUuid &repo_id, TUpdate &&update, const std::optional<TSequenceNumber> &seq_num);

            virtual ~TMutation();

            TMutation &operator=(TMutation &&that);

            TMutation &operator=(const TMutation &that);

            inline TKind GetKind() const;

            inline const Base::TUuid &GetRepoId() const;

            inline const std::optional<TSequenceNumber> &GetSequenceNumber() const;

            inline const TUpdate &GetUpdate() const;

            inline TUpdate &GetUpdate();

            inline void SetSequenceNumber(std::optional<TSequenceNumber> seq_num) NO_THROW;

            inline TSequenceNumber &GetNextUpdate() NO_THROW;

            private:

            TKind Kind;

            Base::TUuid RepoId;

            TUpdate Update;

            std::optional<TSequenceNumber> SequenceNumber;

            TSequenceNumber NextUpdate;

          };  // TMutation

          TReplica(TReplica &&replica);

          TReplica(const TReplica &replica);

          TReplica();

          ~TReplica();

          TReplica &operator=(TReplica &&that);

          TReplica &operator=(const TReplica &that);

          TMutation *Push(const Base::TUuid &repo_id, const TUpdate *update);

          TMutation *Pop(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num);

          TMutation *Fail(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num);

          TMutation *Pause(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num);

          TMutation *UnPause(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num);

          void Reset();

          inline TMutationList &GetMutationList() const;

          inline size_t GetSize() const;

          private:

          mutable TMutationList MutationList;

          friend class TManager;
          friend class Orly::Indy::TReplicationStreamer;

        };  // TReplica

        static void *operator new(size_t size) {
          return Pool.Alloc(size);
        }

        static void operator delete(void *ptr, size_t) {
          Pool.Free(ptr);
        }

        static void InitTransactionPool(size_t num_obj) {
          Pool.Init(num_obj);
        }

        static constexpr size_t GetTransactionSize() {
          return sizeof(TTransaction);
        }

        static void InitTransactionMutationPool(size_t num_obj) {
          TMutation::Pool.Init(num_obj);
        }

        static constexpr size_t GetTransactionMutationSize() {
          return sizeof(TMutation);
        }

        private:

        /* Forward Declarations. */
        class TMutation;

        typedef InvCon::UnorderedMultimap::TCollection<TTransaction, TMutation, Base::TUuid> TMutationCollection;

        TTransaction(TManager *manager, bool should_replicate);

        ~TTransaction() NO_THROW;

        class TMutation {
          NO_COPY(TMutation);
          public:

          enum TKind {
            Pusher,
            Popper,
            StatusChanger
          };

          typedef InvCon::UnorderedMultimap::TMembership<TMutation, TTransaction, Base::TUuid> TTransactionMembership;

          virtual ~TMutation() NO_THROW;

          virtual TKind GetKind() const = 0;

          inline const Base::TUuid &GetRepoId() const;

          TMutation *TryGetNextMember() const {
            return TransactionMembership.TryGetNextMember();
          }

          static void *operator new(size_t size) {
            return Pool.Alloc(size);
          }

          static void operator delete(void *ptr, size_t) {
            Pool.Free(ptr);
          }

          protected:

          TMutation(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo);

          TTransactionMembership::TImpl TransactionMembership;

          L0::TManager::TPtr<TRepo> Repo;

          mutable TReplica::TMutation *MyMutation;

          static Util::TPool Pool;

          friend class Server::TIndyReporter;
          friend class TTransaction;

        };  // TMutation

        class TPusher
            : public TMutation {
          NO_COPY(TPusher);
          public:

          TPusher(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo, const std::shared_ptr<TUpdate> &update);

          virtual ~TPusher() NO_THROW;

          virtual TKind GetKind() const;

          private:

          TUpdate *Update;

          friend class TTransaction;

        };  // TPusher

        class TPopper
            : public TMutation {
          NO_COPY(TPopper);
          public:

          enum TState {
            Peek,
            Pop,
            Fail
          };

          TPopper(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo, TState state);

          virtual ~TPopper() NO_THROW;

          virtual TKind GetKind() const;

          TState GetState() const;

          void DoPeek() const;

          void SetState(TState state);

          inline const std::shared_ptr<TUpdate> &GetUpdate() const;

          private:

          TState State;

          mutable std::shared_ptr<TRepo::TView> View;

          mutable std::shared_ptr<TUpdate> Update;

          friend class TTransaction;

        };  // TPopper

        class TStatusChanger
            : public TMutation {
          NO_COPY(TStatusChanger);
          public:

          TStatusChanger(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo, TStatus status);

          virtual ~TStatusChanger() NO_THROW;

          virtual TKind GetKind() const;

          private:

          TStatus Status;

          friend class TTransaction;

        };  // TStatusChanger

        bool GetCommitFlag() const;

        TManager *Manager;

        TManagerMembership::TImpl ManagerMembership;

        mutable TMutationCollection::TImpl MutationCollection;

        bool CommitFlag;

        bool ShouldReplicate;

        bool Prepared;

        bool EnsureOrDiscard;

        TReplica Replica;

        TTransactionReplication *TransactionReplication;

        TTransactionCompletion *TransactionCompletion;

        static Util::TPool Pool;

        friend class TManager;
        friend class Server::TIndyReporter;

      };  // TTransaction

      class TManager
          : public L0::TManager {
        NO_COPY(TManager);
        public:

        typedef InvCon::UnorderedList::TCollection<TManager, TTransaction> TTransactionCollection;
        typedef InvCon::UnorderedList::TCollection<TManager, TTransaction::TTransactionCompletion> TTransactionCompletionCollection;

        std::unique_ptr<TTransaction, std::function<void (TTransaction *)>> NewTransaction(bool should_replicate = true);

        protected:

        TManager(Disk::Util::TEngine *engine,
                 std::chrono::milliseconds merge_mem_delay,
                 std::chrono::milliseconds merge_disk_delay,
                 bool allow_tailing,
                 bool no_realtime,
                 std::chrono::milliseconds layer_cleaning_interval,
                 Base::TScheduler *scheduler,
                 size_t block_slots_available_per_merger,
                 size_t max_repo_cache_size,
                 size_t temp_file_consol_thresh,
                 const std::vector<size_t> &merge_mem_cores,
                 const std::vector<size_t> &merge_disk_cores,
                 bool create_new);

        virtual ~TManager();

        virtual std::mutex &GetReplicationQueueLock() NO_THROW = 0;

        virtual void Enqueue(TTransactionReplication *transaction_replication, TTransaction::TReplica &&replica) NO_THROW = 0;

        virtual TTransactionReplication *NewTransactionReplication() = 0;

        virtual void DeleteTransactionReplication(TTransactionReplication *transaction_replication) NO_THROW = 0;

        private:

        void OnCloseTransaction(TTransaction *transaction);

        mutable TTransactionCollection::TImpl TransactionCollection;
        std::mutex TransactionLock;

        mutable TTransactionCompletionCollection::TImpl TransactionCompletionCollection;
        std::mutex TransactionCompletionLock;

        std::function<void (TTransaction *)> OnCloseTransactionCb;

        /* access to Enqueue */
        friend class TTransaction;

      };  // TManager

      inline void TTransaction::CommitAction() NO_THROW {
        CommitFlag = true;
      }

      inline void TTransaction::DiscardAction() NO_THROW {
        CommitFlag = false;
      }

      inline const Atom::TCore &TTransaction::TReplica::TMutation::TUpdate::GetMetadata() const {
        return Metadata;
      }

      inline const Atom::TCore &TTransaction::TReplica::TMutation::TUpdate::GetId() const {
        return Id;
      }

      inline const TTransaction::TReplica::TMutation::TUpdate::TOpByKey &TTransaction::TReplica::TMutation::TUpdate::GetOpByKey() const {
        return OpByKey;
      }

      inline const std::shared_ptr<Atom::TSuprena> &TTransaction::TReplica::TMutation::TUpdate::GetSuprena() const {
        return Suprena;
      }

      inline void TTransaction::TReplica::TMutation::TUpdate::SetArena(const std::shared_ptr<Atom::TSuprena> &suprena) {
        Suprena = suprena;
      }

      inline TTransaction::TReplica::TMutation::TKind TTransaction::TReplica::TMutation::GetKind() const {
        return Kind;
      }

      inline const Base::TUuid &TTransaction::TReplica::TMutation::GetRepoId() const {
        return RepoId;
      }

      inline const std::optional<TSequenceNumber> &TTransaction::TReplica::TMutation::GetSequenceNumber() const {
        return SequenceNumber;
      }

      inline const TTransaction::TReplica::TMutation::TUpdate &TTransaction::TReplica::TMutation::GetUpdate() const {
        assert(Kind == Pusher);
        return Update;
      }

      inline TTransaction::TReplica::TMutation::TUpdate &TTransaction::TReplica::TMutation::GetUpdate() {
        assert(Kind == Pusher);
        return Update;
      }

      inline void TTransaction::TReplica::TMutation::SetSequenceNumber(std::optional<TSequenceNumber> seq_num) NO_THROW {
        SequenceNumber = seq_num;
      }

      inline TSequenceNumber &TTransaction::TReplica::TMutation::GetNextUpdate() NO_THROW {
        return NextUpdate;
      }

      inline TTransaction::TReplica::TMutationList &TTransaction::TReplica::GetMutationList() const {
        return MutationList;
      }

      inline size_t TTransaction::TReplica::GetSize() const {
        size_t count = 0U;
        for (auto iter = MutationList.begin(); iter != MutationList.end(); ++iter, ++count);
        return count;
      }

      inline const Base::TUuid &TTransaction::TMutation::GetRepoId() const {
        return Repo->GetId();
      }

      inline TTransaction::TMutation::TKind TTransaction::TPusher::GetKind() const {
        return TMutation::Pusher;
      }

      inline TTransaction::TMutation::TKind TTransaction::TPopper::GetKind() const {
        return TMutation::Popper;
      }

      inline TTransaction::TPopper::TState TTransaction::TPopper::GetState() const {
        return State;
      }

      inline const std::shared_ptr<TUpdate> &TTransaction::TPopper::GetUpdate() const {
        return Update;
      }

      inline void TTransaction::TPopper::SetState(TState state) {
        State = state;
      }

      inline TTransaction::TMutation::TKind TTransaction::TStatusChanger::GetKind() const {
        return TMutation::StatusChanger;
      }

      inline bool TTransaction::GetCommitFlag() const {
        return CommitFlag;
      }

      inline void TTransaction::TTransactionCompletion::SetCb(const std::shared_ptr<std::function<void (TTransactionResult)>> &cb) {
        Cb = cb;
      }

    }  // L0

  }  // Indy

}  // Orly
