/* <orly/indy/failover.h>

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
#include <condition_variable>
#include <mutex>
#include <optional>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <base/io/device.h>
#include <base/rpc/rpc.h>
#include <orly/indy/file_sync.h>
#include <orly/indy/replication.h>
#include <orly/indy/sequence_number.h>
#include <orly/indy/util/context_streamer.h>

namespace Orly {

  namespace Indy {

    /* TEMP */
    class TUpdateContext {
      public:

      void Write(Io::TBinaryOutputStream &/*strm*/) const {}

      void Read(Io::TBinaryInputStream &/*strm*/) {}

    };

    class TConnectionFailed
        : public std::runtime_error {
      public:

      /* Do-little. */
      TConnectionFailed(const std::string &error_msg);

    };  // TConnectionFailed

    class TCommonContext
        : public Rpc::TContext {
      NO_COPY(TCommonContext);
      public:

      static const Rpc::TEntryId PingId = 3001;

      virtual ~TCommonContext();

      void Shutdown();

      virtual bool Queue();

      virtual bool Work();

      virtual void Join();

      protected:

      class TProtocol
          : public Rpc::TProtocol {
        NO_COPY(TProtocol);
        protected:

        TProtocol() {
          Register<TCommonContext, void>(PingId, &TCommonContext::Ping);
        }

      };  // TProtocol

      TCommonContext(const Rpc::TProtocol &protocol, const Base::TFd &fd);

      std::mutex Mutex;

      Base::TEventSemaphore Sem;

      std::queue<std::shared_ptr<const Rpc::TAnyRequest>> RequestQueue;

      private:

      enum TReaderState {Connected, NotConnected};

      enum TWorkerState {Running, Exited, ExitedOnWriteFailure};

      virtual void Ping() = 0;

      Base::TFd Fd;

      std::shared_ptr<Io::TDevice> Device;

      TReaderState ReaderState;

      TWorkerState WorkerState;
      std::mutex WorkerMutex;
      Base::TEventSemaphore WorkerStateChanged;

    };  // TCommonContext

    class TMasterContext
        : public TCommonContext {
      NO_COPY(TMasterContext);
      public:

      static const Rpc::TEntryId FetchUpdatesId = 3500;
      static const Rpc::TEntryId NotifyFinishSyncInventoryId = 3501;
      static const Rpc::TEntryId GetViewId = 3502;
      static const Rpc::TEntryId SyncFileId = 3503;

      /* Low Seq, High Seq, GenId, NumKeys */
      typedef std::tuple<TSequenceNumber, TSequenceNumber, size_t, size_t> TFileTuple;
      typedef std::vector<TFileTuple> TViewDef;

      protected:

      class TProtocol
          : public TCommonContext::TProtocol {
        NO_COPY(TProtocol);
        public:

        static const TProtocol Protocol;

        private:

        TProtocol() {
          Register<TMasterContext, Util::TContextInputStreamer, Base::TUuid, TSequenceNumber, TSequenceNumber>(FetchUpdatesId, &TMasterContext::FetchUpdates);
          Register<TMasterContext, void>(NotifyFinishSyncInventoryId, &TMasterContext::NotifyFinishSyncInventory);
          Register<TMasterContext, TViewDef, Base::TUuid>(GetViewId, &TMasterContext::GetView);
          Register<TMasterContext, TFileSync, Base::TUuid, size_t, size_t>(SyncFileId, &TMasterContext::SyncFile);
        }

      };  // TProtocol

      TMasterContext(const Base::TFd &fd);

      virtual ~TMasterContext();

      virtual Util::TContextInputStreamer FetchUpdates(const Base::TUuid &repo_id, TSequenceNumber lowest, TSequenceNumber highest) = 0;

      virtual void NotifyFinishSyncInventory() = 0;

      virtual TViewDef GetView(const Base::TUuid &repo_id) = 0;

      virtual TFileSync SyncFile(const Base::TUuid &file_id, size_t gen_id, size_t context) = 0;

    };  // TMasterContext

    class TSlaveContext
        : public TCommonContext {
      NO_COPY(TSlaveContext);
      public:

      static const Rpc::TEntryId InventoryId = 3100;
      static const Rpc::TEntryId PushNotificationsId = 3101;
      static const Rpc::TEntryId TransitionToSlaveId = 3102;
      static const Rpc::TEntryId SyncInventoryId = 3103;
      static const Rpc::TEntryId IndexId = 3104;

      protected:

      class TProtocol
          : public TCommonContext::TProtocol {
        NO_COPY(TProtocol);
        public:

        static const TProtocol Protocol;

        private:

        TProtocol() {
          Register<TSlaveContext, void,
            Base::TUuid,
            size_t,
            std::optional<Base::TUuid>,
            bool,
            std::optional<TSequenceNumber>,
            std::optional<TSequenceNumber>,
            TSequenceNumber>(InventoryId, &TSlaveContext::Inventory);
          Register<TSlaveContext, void, TIndexMapReplica>(IndexId, &TSlaveContext::Index);
          Register<TSlaveContext, void, TReplicationStreamer>(PushNotificationsId, &TSlaveContext::PushNotifications);
          Register<TSlaveContext, void>(TransitionToSlaveId, &TSlaveContext::TransitionToSlave);
          Register<TSlaveContext, void>(SyncInventoryId, &TSlaveContext::ScheduleSyncInventory);
        }

      };  // TProtocol

      TSlaveContext(const Base::TFd &fd);

      virtual ~TSlaveContext();

      virtual void Inventory(const Base::TUuid &repo_id,
                             size_t ttl,
                             const std::optional<Base::TUuid> &parent_repo_id,
                             bool is_safe,
                             const std::optional<TSequenceNumber> &lowest,
                             const std::optional<TSequenceNumber> &highest,
                             TSequenceNumber) = 0;

      virtual void Index(const TIndexMapReplica &index_map_replica) = 0;

      virtual void PushNotifications(const TReplicationStreamer &replication_streamer) = 0;

      virtual void TransitionToSlave() = 0;

      virtual void ScheduleSyncInventory() = 0;

    };  // TSlaveContext

    /* Binary streamers for Orly::Indy::TUpdateContext */
    inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const TUpdateContext &context) { context.Write(strm); return strm; }
    inline Io::TBinaryOutputStream &&operator<<(Io::TBinaryOutputStream &&strm, const TUpdateContext &context) { context.Write(strm); return std::move(strm); }
    inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, TUpdateContext &context) { context.Read(strm); return strm; }
    inline Io::TBinaryInputStream &&operator>>(Io::TBinaryInputStream &&strm, TUpdateContext &context) { context.Read(strm); return std::move(strm); }

  }  // Indy

}  // Orly