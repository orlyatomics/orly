/* <orly/indy/disk/sim/fault_device.h>

   A test-only memory device that can fail an I/O, lose power, or inject storage
   faults (#608, #755).

   `TFaultPlan` is shared by every device of one engine. Each I/O a device serves counts as one
   operation of its kind (Read, ReadV, Write, Sync) while the plan is armed. A plan can:

   - fail the Nth operation of the kinds it names. A failed Read/ReadV/Write is reported the way
     the disk controller reports an I/O error (volume_manager.cc, QueueRunner): if the caller
     passed abort_on_error, which every caller except the file service's recovery reads does,
     the controller calls abort(); otherwise the I/O completes with Error, "Disk Error". The plan
     can do exactly that (TOnAbortOnError::Abort), or report the error even so (Report), to see
     what the layer above would do with it. A failed Sync throws std::system_error(EIO), which is
     what TPersistentDevice::Sync's IfLt0(fsync) throws.

   - lose power at the Nth Sync. Each device keeps two images: Live, what reads see, and Durable,
     what survives. A write lands in Live and marks its sectors dirty; a completed Sync of that
     device copies them to Durable. The Nth Sync does not complete: every device's Durable image
     freezes there, optionally with each dirty sector having reached the media with a given
     probability (a torn write, at sector granularity). Then the plan's power-loss callback runs;
     the harness saves the frozen images and _exit()s, the way a crash ends a process, and opens
     a new engine over them (see fault_engine.h).

   - simulate storage faults on the Nth operation (#755):
     - LostWrite: the Nth write reports success but drops the data (unwritten to media).
     - WriteErrAfter: the Nth write reaches media and marks sectors dirty, then reports Error
       (or aborts if abort_on_error).
     - SyncErrAfter: the Nth sync flushes dirty sectors to Durable, then throws std::system_error(EIO).
     - Misdirect: the Nth write lands at an alternate offset or delta.
     - BitFlip: flips a bit in Durable or Live to simulate latent sector corruption or bit rot.

   Nothing here is linked into a server: only tests include it.

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

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <base/class_traits.h>
#include <base/code_location.h>
#include <base/mem_aligned_ptr.h>
#include <orly/indy/disk/util/volume_manager.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Sim {

        class TFaultDevice;

        /* What a failed Read/ReadV/Write does when its caller passed abort_on_error. */
        enum class TOnAbortOnError {

          /* What the disk controller does: abort(). */
          Abort,

          /* Complete the I/O with Error anyway, as if the caller had not asked to abort. */
          Report

        };  // TOnAbortOnError

        class TFaultPlan {
          NO_COPY(TFaultPlan);
          public:

          /* Operation kinds, as a bit mask. */
          static constexpr unsigned Read = 1U, ReadV = 2U, Write = 4U, Sync = 8U, AnyIo = Read | ReadV | Write;

          static const char *GetKindName(unsigned kind) {
            switch (kind) {
              case Read: return "Read";
              case ReadV: return "ReadV";
              case Write: return "Write";
              case Sync: return "Sync";
            }
            return "?";
          }

          /* The fault that fired. */
          struct TInjected {
            unsigned Kind;
            size_t N;
            bool AbortOnError;
            std::string Where;
            std::string Action = "Fail";
          };

          TFaultPlan() = default;

          /* Count operations from now on. Faults fire only while armed. */
          void Arm() {
            std::lock_guard<std::mutex> lock(Mutex);
            Armed = true;
          }

          void Disarm() {
            std::lock_guard<std::mutex> lock(Mutex);
            Armed = false;
          }

          /* Fail the nth operation (from 1) among the kinds in 'kinds'. */
          void FailNth(size_t n, unsigned kinds, TOnAbortOnError on_abort_on_error) {
            std::lock_guard<std::mutex> lock(Mutex);
            FailAt = n;
            FailKinds = kinds;
            OnAbortOnError = on_abort_on_error;
          }

          /* Lose power at the nth Sync (from 1). Each sector written since its device's last
             completed Sync reaches the media with probability 'tear', drawn from 'seed'. Then
             'on_power_loss' runs; if it returns, the devices go on serving their Live images
             while their Durable images stay frozen. */
          void PowerLossAtSync(size_t n, double tear, uint64_t seed, const std::function<void ()> &on_power_loss) {
            std::lock_guard<std::mutex> lock(Mutex);
            PowerLossAt = n;
            Tear = tear;
            Seed = seed;
            OnPowerLoss = on_power_loss;
          }

          /* Drop the nth Write (from 1): report Success but do not modify media or mark dirty. */
          void LostWriteNth(size_t n) {
            std::lock_guard<std::mutex> lock(Mutex);
            LostWriteAt = n;
          }

          /* Write the nth Write to media and mark dirty, then report Error (or abort if abort_on_error). */
          void WriteErrAfterNth(size_t n, TOnAbortOnError on_abort_on_error = TOnAbortOnError::Report) {
            std::lock_guard<std::mutex> lock(Mutex);
            WriteErrAfterAt = n;
            WriteErrAfterOnAbortOnError = on_abort_on_error;
          }

          /* Flush dirty sectors to Durable at the nth Sync, then throw std::system_error(EIO). */
          void SyncErrAfterNth(size_t n) {
            std::lock_guard<std::mutex> lock(Mutex);
            SyncErrAfterAt = n;
          }

          /* Direct the nth Write to alternate_offset instead of its requested offset. */
          void MisdirectNth(size_t n, Util::TOffset alternate_offset) {
            std::lock_guard<std::mutex> lock(Mutex);
            MisdirectAt = n;
            MisdirectOffset = alternate_offset;
            MisdirectDelta = 0;
          }

          /* Direct the nth Write to (offset + delta) instead of offset. */
          void MisdirectNthDelta(size_t n, int64_t delta) {
            std::lock_guard<std::mutex> lock(Mutex);
            MisdirectAt = n;
            MisdirectOffset = std::nullopt;
            MisdirectDelta = delta;
          }

          /* Flip a bit in Durable at the nth Sync after flushing dirty sectors. */
          void BitFlipAtSync(size_t n, size_t byte_offset, uint8_t bit = 0) {
            std::lock_guard<std::mutex> lock(Mutex);
            BitFlipAt = n;
            BitFlipByteOffset = byte_offset;
            BitFlipBit = bit;
          }

          /* Runs when a fault fires, before a TOnAbortOnError::Abort abort(). */
          void SetOnInject(const std::function<void (const TInjected &)> &on_inject) {
            std::lock_guard<std::mutex> lock(Mutex);
            OnInject = on_inject;
          }

          /* Operations of the given kinds counted since Arm(). */
          size_t GetCount(unsigned kinds) const {
            std::lock_guard<std::mutex> lock(Mutex);
            size_t total = 0UL;
            for (unsigned i = 0U; i < 4U; ++i) {
              if (kinds & (1U << i)) {
                total += Counts[i];
              }
            }
            return total;
          }

          /* Counted operations of the kinds a FailNth names. A run that ends with this below its
             n never reached the fault. */
          size_t GetFailKindCount() const {
            return GetCount(FailKinds);
          }

          std::optional<TInjected> GetInjected() const {
            std::lock_guard<std::mutex> lock(Mutex);
            return Injected;
          }

          bool IsPoweredOff() const {
            std::lock_guard<std::mutex> lock(Mutex);
            return PoweredOff;
          }

          private:

          enum class TWriteAction { Normal, Fail, LostWrite, WriteErrAfter, Misdirect };

          struct TWriteDecision {
            TWriteAction Action = TWriteAction::Normal;
            Util::TOffset Offset = 0UL;
            bool DoAbort = false;
          };

          enum class TSyncAction { Normal, Fail, PowerLoss, PoweredOff, SyncErrAfter, BitFlip };

          static unsigned GetIndex(unsigned kind) {
            switch (kind) {
              case Read: return 0U;
              case ReadV: return 1U;
              case Write: return 2U;
              default: return 3U;
            }
          }

          /* Count a Read/ReadV; true if it must fail. abort()s here, as the controller would,
             under TOnAbortOnError::Abort. */
          bool ShouldFail(unsigned kind, bool abort_on_error, const Base::TCodeLocation &code_location) {
            std::function<void (const TInjected &)> on_inject;
            TInjected injected;
            bool do_abort = false;
            /* acquire Mutex */ {
              std::lock_guard<std::mutex> lock(Mutex);
              if (!Armed) {
                return false;
              }
              ++Counts[GetIndex(kind)];
              if (!(FailKinds & kind) || Injected) {
                return false;
              }
              size_t count = 0UL;
              for (unsigned i = 0U; i < 4U; ++i) {
                if (FailKinds & (1U << i)) {
                  count += Counts[i];
                }
              }
              if (count != FailAt) {
                return false;
              }
              std::ostringstream where;
              where << code_location;
              injected = TInjected{kind, count, abort_on_error, where.str(), "Fail"};
              Injected = injected;
              on_inject = OnInject;
              do_abort = abort_on_error && OnAbortOnError == TOnAbortOnError::Abort;
            }  // release Mutex
            if (on_inject) {
              on_inject(injected);
            }
            if (do_abort) {
              abort();
            }
            return true;
          }

          /* Evaluates a Write: whether to fail, drop (LostWrite), fail after write (WriteErrAfter),
             misdirect to another offset, or proceed normally. */
          TWriteDecision OnWrite(const Base::TCodeLocation &code_location, Util::TOffset offset, bool abort_on_error) {
            std::function<void (const TInjected &)> on_inject;
            TInjected injected;
            TWriteDecision decision{TWriteAction::Normal, offset, false};
            /* acquire Mutex */ {
              std::lock_guard<std::mutex> lock(Mutex);
              if (!Armed) {
                return decision;
              }
              const size_t n = ++Counts[GetIndex(Write)];
              if (Injected) {
                return decision;
              }
              std::ostringstream where;
              where << code_location;

              if (FailKinds & Write) {
                size_t count = 0UL;
                for (unsigned i = 0U; i < 4U; ++i) {
                  if (FailKinds & (1U << i)) {
                    count += Counts[i];
                  }
                }
                if (count == FailAt) {
                  decision.Action = TWriteAction::Fail;
                  decision.DoAbort = abort_on_error && OnAbortOnError == TOnAbortOnError::Abort;
                  injected = TInjected{Write, count, abort_on_error, where.str(), "Fail"};
                  Injected = injected;
                  on_inject = OnInject;
                }
              } else if (LostWriteAt && *LostWriteAt == n) {
                decision.Action = TWriteAction::LostWrite;
                injected = TInjected{Write, n, abort_on_error, where.str(), "LostWrite"};
                Injected = injected;
                on_inject = OnInject;
              } else if (WriteErrAfterAt && *WriteErrAfterAt == n) {
                decision.Action = TWriteAction::WriteErrAfter;
                decision.DoAbort = abort_on_error && WriteErrAfterOnAbortOnError == TOnAbortOnError::Abort;
                injected = TInjected{Write, n, abort_on_error, where.str(), "WriteErrAfter"};
                Injected = injected;
                on_inject = OnInject;
              } else if (MisdirectAt && *MisdirectAt == n) {
                decision.Action = TWriteAction::Misdirect;
                if (MisdirectOffset) {
                  decision.Offset = *MisdirectOffset;
                } else {
                  decision.Offset = static_cast<Util::TOffset>(static_cast<int64_t>(offset) + MisdirectDelta);
                }
                injected = TInjected{Write, n, abort_on_error, where.str(), "Misdirect"};
                Injected = injected;
                on_inject = OnInject;
              }
            }  // release Mutex
            if (on_inject) {
              on_inject(injected);
            }
            if (decision.DoAbort) {
              abort();
            }
            return decision;
          }

          /* Runs OnInject for an injected Sync failure, outside Mutex. */
          void NotifyInjected() {
            std::function<void (const TInjected &)> on_inject;
            std::optional<TInjected> injected;
            /* acquire Mutex */ {
              std::lock_guard<std::mutex> lock(Mutex);
              on_inject = OnInject;
              injected = Injected;
            }  // release Mutex
            if (on_inject && injected) {
              on_inject(*injected);
            }
          }

          TSyncAction OnSync() {
            std::lock_guard<std::mutex> lock(Mutex);
            if (PoweredOff) {
              return TSyncAction::PoweredOff;
            }
            if (!Armed) {
              return TSyncAction::Normal;
            }
            const size_t n = ++Counts[GetIndex(Sync)];
            if (PowerLossAt && *PowerLossAt == n) {
              return TSyncAction::PowerLoss;
            }
            if ((FailKinds & Sync) && !Injected) {
              size_t count = 0UL;
              for (unsigned i = 0U; i < 4U; ++i) {
                if (FailKinds & (1U << i)) {
                  count += Counts[i];
                }
              }
              if (count == FailAt) {
                Injected = TInjected{Sync, count, false, "Sync", "Fail"};
                return TSyncAction::Fail;
              }
            }
            if (SyncErrAfterAt && *SyncErrAfterAt == n && !Injected) {
              Injected = TInjected{Sync, n, false, "Sync", "SyncErrAfter"};
              return TSyncAction::SyncErrAfter;
            }
            if (BitFlipAt && *BitFlipAt == n && !Injected) {
              Injected = TInjected{Sync, n, false, "Sync", "BitFlip"};
              return TSyncAction::BitFlip;
            }
            return TSyncAction::Normal;
          }

          /* Freezes every device's Durable image, then runs OnPowerLoss. Defined below
             TFaultDevice. */
          inline void PowerLoss();

          /* Performs bit flip on registered devices. Defined below TFaultDevice. */
          inline void PerformBitFlip();

          void AddDevice(TFaultDevice *device) {
            std::lock_guard<std::mutex> lock(Mutex);
            Devices.push_back(device);
          }

          void RemoveDevice(TFaultDevice *device) {
            std::lock_guard<std::mutex> lock(Mutex);
            std::erase(Devices, device);
          }

          mutable std::mutex Mutex;

          bool Armed = false;

          size_t Counts[4] = {0UL, 0UL, 0UL, 0UL};

          size_t FailAt = 0UL;

          unsigned FailKinds = 0U;

          TOnAbortOnError OnAbortOnError = TOnAbortOnError::Abort;

          std::optional<size_t> PowerLossAt;

          double Tear = 0.0;

          uint64_t Seed = 0UL;

          std::optional<size_t> LostWriteAt;

          std::optional<size_t> WriteErrAfterAt;

          TOnAbortOnError WriteErrAfterOnAbortOnError = TOnAbortOnError::Report;

          std::optional<size_t> MisdirectAt;

          std::optional<Util::TOffset> MisdirectOffset;

          int64_t MisdirectDelta = 0;

          std::optional<size_t> SyncErrAfterAt;

          std::optional<size_t> BitFlipAt;

          std::optional<size_t> BitFlipByteOffset;

          uint8_t BitFlipBit = 0U;

          std::function<void ()> OnPowerLoss;

          std::function<void (const TInjected &)> OnInject;

          std::optional<TInjected> Injected;

          bool PoweredOff = false;

          std::vector<TFaultDevice *> Devices;

          friend class TFaultDevice;

        };  // TFaultPlan

        class TFaultDevice final
            : public Util::TDevice {
          NO_COPY(TFaultDevice);
          public:

          /* A device of 'num_logical_block' 512-byte blocks. Starts zeroed, or, given an image
             (a Durable image saved from another device of the same size), as that image. */
          TFaultDevice(TFaultPlan *plan, size_t num_logical_block, const std::vector<char> *image = nullptr)
              : Util::TDevice(TDesc{TDesc::Mem, Util::PhysicalSectorSize, Util::PhysicalSectorSize, num_logical_block,
                                    Util::PhysicalSectorSize * num_logical_block}, true /* fsync */, true /* corruption check */),
                Plan(plan),
                NumBytes(Util::PhysicalBlockSize /* super block */ + Desc.Capacity),
                Live(Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), NumBytes)),
                Durable(NumBytes, 0),
                Dirty(NumBytes / Util::PhysicalSectorSize, false) {
            assert(Desc.Capacity % getpagesize() == 0);
            if (image) {
              if (image->size() != NumBytes) {
                throw std::runtime_error("fault device image size mismatch");
              }
              memcpy(Live.get(), image->data(), NumBytes);
              Durable = *image;
            }
            Plan->AddDevice(this);
          }

          virtual ~TFaultDevice() {
            Plan->RemoveDevice(this);
          }

          /* What survives a power loss: the Durable image. */
          std::vector<char> GetDurableImage() const {
            std::lock_guard<std::mutex> lock(Mutex);
            return Durable;
          }

          /* What a clean shutdown would leave: the Live image. */
          std::vector<char> GetLiveImage() const {
            std::lock_guard<std::mutex> lock(Mutex);
            return std::vector<char>(Live.get(), Live.get() + NumBytes);
          }

          /* Flip one bit in Durable (e.g. to simulate latent sector corruption or bit rot). */
          void FlipDurableBit(size_t byte_offset, uint8_t bit = 0) {
            std::lock_guard<std::mutex> lock(Mutex);
            if (byte_offset < Durable.size()) {
              Durable[byte_offset] ^= static_cast<char>(1U << (bit & 7U));
            }
          }

          /* Flip one bit in Live. */
          void FlipLiveBit(size_t byte_offset, uint8_t bit = 0) {
            std::lock_guard<std::mutex> lock(Mutex);
            if (byte_offset < NumBytes) {
              Live.get()[byte_offset] ^= static_cast<char>(1U << (bit & 7U));
            }
          }

          virtual void Write(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, uint8_t /*util_src*/, void *buf, const Util::TOffset offset,
                             long long nbytes, DiskPriority /*priority*/, bool abort_on_error, const Util::TOffset /*logical_start_offset*/,
                             TCompletionTrigger &trigger) override {
            trigger.Callback(DoWrite(code_location, buf_kind, buf, offset, nbytes, abort_on_error), ErrStr);
          }

          virtual void Write(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, uint8_t /*util_src*/, void *buf, const Util::TOffset offset,
                             long long nbytes, DiskPriority /*priority*/, bool abort_on_error, const Util::TOffset /*logical_start_offset*/,
                             const Util::TIOCallback &cb) override {
            cb(DoWrite(code_location, buf_kind, buf, offset, nbytes, abort_on_error), ErrStr);
          }

          virtual void Read(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, uint8_t /*util_src*/, void *buf, const Util::TOffset offset,
                            long long nbytes, DiskPriority /*priority*/, bool abort_on_error, TCompletionTrigger &trigger) override {
            const char *err_str = "";
            const TDiskResult result = DoRead(TFaultPlan::Read, code_location, buf_kind, buf, offset, nbytes, abort_on_error, err_str);
            trigger.Callback(result, err_str);
          }

          virtual void Read(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, uint8_t /*util_src*/, void *buf, const Util::TOffset offset,
                            long long nbytes, DiskPriority /*priority*/, bool abort_on_error, const Util::TIOCallback &cb) override {
            const char *err_str = "";
            const TDiskResult result = DoRead(TFaultPlan::Read, code_location, buf_kind, buf, offset, nbytes, abort_on_error, err_str);
            cb(result, err_str);
          }

          virtual void ReadV(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, uint8_t /*util_src*/,
                             const std::vector<void *> &buf_vec, const Util::TOffset offset, long long nbytes, DiskPriority /*priority*/,
                             bool abort_on_error, TCompletionTrigger &trigger) override {
            const char *err_str = "";
            const TDiskResult result = DoReadV(code_location, buf_kind, buf_vec, offset, nbytes, abort_on_error, err_str);
            trigger.Callback(result, err_str);
          }

          virtual void ReadV(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, uint8_t /*util_src*/,
                             const std::vector<void *> &buf_vec, const Util::TOffset offset, long long nbytes, DiskPriority /*priority*/,
                             bool abort_on_error, Util::TGroupRequest *group_request) override {
            const char *err_str = "";
            const TDiskResult result = DoReadV(code_location, buf_kind, buf_vec, offset, nbytes, abort_on_error, err_str);
            CompleteGroupRequest(group_request, result, err_str);
          }

          /* As TPersistentDevice: SyncToDisk runs this on a scheduler thread when it syncs more
             than one device, and turns a failure into a runtime_error. */
          virtual void AsyncSyncFlush(std::mutex &mut, std::condition_variable &cond, size_t &num_finished, size_t &num_err) override {
            try {
              Sync();
            } catch (const std::exception &) {
              std::lock_guard<std::mutex> lock(mut);
              ++num_err;
            }
            std::lock_guard<std::mutex> lock(mut);
            ++num_finished;
            cond.notify_one();
          }

          virtual void Sync() override {
            switch (Plan->OnSync()) {
              case TFaultPlan::TSyncAction::Normal: {
                std::lock_guard<std::mutex> lock(Mutex);
                for (size_t sector = 0UL; sector < Dirty.size(); ++sector) {
                  if (Dirty[sector]) {
                    memcpy(Durable.data() + sector * Util::PhysicalSectorSize, Live.get() + sector * Util::PhysicalSectorSize, Util::PhysicalSectorSize);
                    Dirty[sector] = false;
                  }
                }
                break;
              }
              case TFaultPlan::TSyncAction::Fail: {
                Plan->NotifyInjected();
                throw std::system_error(EIO, std::system_category(), "fsync (injected)");
              }
              case TFaultPlan::TSyncAction::PowerLoss: {
                Plan->PowerLoss();
                break;
              }
              case TFaultPlan::TSyncAction::PoweredOff: {
                break;
              }
              case TFaultPlan::TSyncAction::SyncErrAfter: {
                {
                  std::lock_guard<std::mutex> lock(Mutex);
                  for (size_t sector = 0UL; sector < Dirty.size(); ++sector) {
                    if (Dirty[sector]) {
                      memcpy(Durable.data() + sector * Util::PhysicalSectorSize, Live.get() + sector * Util::PhysicalSectorSize, Util::PhysicalSectorSize);
                      Dirty[sector] = false;
                    }
                  }
                }
                Plan->NotifyInjected();
                throw std::system_error(EIO, std::system_category(), "fsync (injected after sync)");
              }
              case TFaultPlan::TSyncAction::BitFlip: {
                {
                  std::lock_guard<std::mutex> lock(Mutex);
                  for (size_t sector = 0UL; sector < Dirty.size(); ++sector) {
                    if (Dirty[sector]) {
                      memcpy(Durable.data() + sector * Util::PhysicalSectorSize, Live.get() + sector * Util::PhysicalSectorSize, Util::PhysicalSectorSize);
                      Dirty[sector] = false;
                    }
                  }
                }
                Plan->PerformBitFlip();
                Plan->NotifyInjected();
                break;
              }
            }
          }

          virtual size_t GetMaxSegments() const override {
            return Util::MaxSegmentsPerIO;
          }

          virtual size_t GetMaxSectorsKb() const override {
            return 64UL;
          }

          /* A discard changes nothing a later read depends on (a freed block is rewritten
             before it is read again), so it is not modelled. */
          virtual void DiscardAll() override {}

          virtual void DiscardRange(uint64_t /*from*/, uint64_t /*num_bytes*/) override {}

          private:

          static constexpr const char *ErrStr = "";

          TDiskResult DoWrite(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, void *buf, const Util::TOffset offset,
                              long long nbytes, bool abort_on_error) {
            const auto decision = Plan->OnWrite(code_location, offset, abort_on_error);
            switch (decision.Action) {
              case TFaultPlan::TWriteAction::Fail: {
                return Error;
              }
              case TFaultPlan::TWriteAction::LostWrite: {
                /* Dropped: report success without touching media. */
                return Success;
              }
              case TFaultPlan::TWriteAction::WriteErrAfter: {
                ApplyCorruptionCheck(buf_kind, buf, offset, nbytes);
                std::lock_guard<std::mutex> lock(Mutex);
                assert(offset + nbytes <= NumBytes);
                memcpy(Live.get() + offset, buf, nbytes);
                for (size_t sector = offset / Util::PhysicalSectorSize; sector < (offset + nbytes + Util::PhysicalSectorSize - 1) / Util::PhysicalSectorSize; ++sector) {
                  Dirty[sector] = true;
                }
                return Error;
              }
              case TFaultPlan::TWriteAction::Misdirect: {
                Util::TOffset target_offset = decision.Offset;
                if (target_offset + nbytes > NumBytes && NumBytes >= static_cast<size_t>(nbytes)) {
                  target_offset = target_offset % (NumBytes - nbytes + 1);
                }
                ApplyCorruptionCheck(buf_kind, buf, offset, nbytes);
                std::lock_guard<std::mutex> lock(Mutex);
                assert(target_offset + nbytes <= NumBytes);
                memcpy(Live.get() + target_offset, buf, nbytes);
                for (size_t sector = target_offset / Util::PhysicalSectorSize; sector < (target_offset + nbytes + Util::PhysicalSectorSize - 1) / Util::PhysicalSectorSize; ++sector) {
                  Dirty[sector] = true;
                }
                return Success;
              }
              case TFaultPlan::TWriteAction::Normal: {
                ApplyCorruptionCheck(buf_kind, buf, offset, nbytes);
                std::lock_guard<std::mutex> lock(Mutex);
                assert(offset + nbytes <= NumBytes);
                memcpy(Live.get() + offset, buf, nbytes);
                for (size_t sector = offset / Util::PhysicalSectorSize; sector < (offset + nbytes + Util::PhysicalSectorSize - 1) / Util::PhysicalSectorSize; ++sector) {
                  Dirty[sector] = true;
                }
                return Success;
              }
            }
            return Success;
          }

          TDiskResult DoRead(unsigned kind, const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, void *buf, const Util::TOffset offset,
                             long long nbytes, bool abort_on_error, const char *&err_str) {
            if (Plan->ShouldFail(kind, abort_on_error, code_location)) {
              err_str = "Disk Error";
              return Error;
            }
            return ReadImpl(code_location, buf_kind, buf, offset, nbytes, abort_on_error, err_str) ? Success : Error;
          }

          TDiskResult DoReadV(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, const std::vector<void *> &buf_vec,
                              const Util::TOffset offset, long long nbytes, bool abort_on_error, const char *&err_str) {
            if (Plan->ShouldFail(TFaultPlan::ReadV, abort_on_error, code_location)) {
              err_str = "Disk Error";
              return Error;
            }
            bool success = true;
            Util::TOffset cur_offset = offset;
            const size_t bytes_per_req = nbytes / buf_vec.size();
            for (void *buf : buf_vec) {
              success = ReadImpl(code_location, buf_kind, buf, cur_offset, bytes_per_req, abort_on_error, err_str) && success;
              cur_offset += bytes_per_req;
            }
            return success ? Success : Error;
          }

          /* As TMemoryDevice::ReadImpl: a page that fails its check is corrupt, which aborts if
             the caller asked to. */
          bool ReadImpl(const Base::TCodeLocation &code_location, Util::TBufKind buf_kind, void *buf, const Util::TOffset offset, long long nbytes, bool abort_on_error, const char *&err_str) {
            /* acquire Mutex */ {
              std::lock_guard<std::mutex> lock(Mutex);
              assert(offset + nbytes <= NumBytes);
              memcpy(buf, Live.get() + offset, nbytes);
            }  // release Mutex
            if (CheckCorruptCheck(buf_kind, buf, offset, nbytes)) {
              return true;
            }
            if (abort_on_error) {
              /* TMemoryDevice aborts without a word; say where. */
              std::ostringstream where;
              where << code_location;
              fprintf(stderr, "TFaultDevice: corrupt data reading [%lld] bytes at [%llu] (buf kind %d) from %s; aborting (abort_on_error)\n",
                      nbytes, static_cast<unsigned long long>(offset), static_cast<int>(buf_kind), where.str().c_str());
              fflush(stderr);
              abort();
            }
            err_str = "Corrupt Data";
            return false;
          }

          /* Under Plan->Mutex, from TFaultPlan::PowerLoss. */
          void Freeze(std::mt19937_64 &rng, double tear) {
            std::lock_guard<std::mutex> lock(Mutex);
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            for (size_t sector = 0UL; sector < Dirty.size(); ++sector) {
              if (Dirty[sector] && tear > 0.0 && dist(rng) < tear) {
                memcpy(Durable.data() + sector * Util::PhysicalSectorSize, Live.get() + sector * Util::PhysicalSectorSize, Util::PhysicalSectorSize);
              }
            }
          }

          TFaultPlan *Plan;

          const size_t NumBytes;

          mutable std::mutex Mutex;

          Base::TMemAlignedPtr<char> Live;

          std::vector<char> Durable;

          /* One flag per 512-byte sector: written since this device's last completed Sync. */
          std::vector<bool> Dirty;

          friend class TFaultPlan;

        };  // TFaultDevice

        inline void TFaultPlan::PowerLoss() {
          std::function<void ()> on_power_loss;
          /* acquire Mutex */ {
            std::lock_guard<std::mutex> lock(Mutex);
            if (PoweredOff) {
              return;
            }
            std::mt19937_64 rng(Seed);
            for (TFaultDevice *device : Devices) {
              device->Freeze(rng, Tear);
            }
            PoweredOff = true;
            on_power_loss = OnPowerLoss;
          }  // release Mutex
          if (on_power_loss) {
            on_power_loss();
          }
        }

        inline void TFaultPlan::PerformBitFlip() {
          std::lock_guard<std::mutex> lock(Mutex);
          if (BitFlipByteOffset) {
            for (TFaultDevice *device : Devices) {
              device->FlipDurableBit(*BitFlipByteOffset, BitFlipBit);
            }
          }
        }

      }  // Sim

    }  // Disk

  }  // Indy

}  // Orly
