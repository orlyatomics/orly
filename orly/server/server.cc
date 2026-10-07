/* <orly/server/server.cc>

   Implements <orly/server/server.h>.

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

#include <orly/server/server.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <sstream>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/syscall.h>

#include <base/as_str.h>
#include <base/booster.h>
#include <base/glob.h>
#include <base/not_implemented.h>
#include <base/gz/input_producer.h>
#include <base/io/binary_input_only_stream.h>
#include <base/io/binary_io_stream.h>
#include <base/io/device.h>
#include <orly/atom/core_vector.h>
#include <orly/indy/disk/durable_manager.h>
#include <orly/auth.h>
#include <orly/protocol.h>
#include <orly/server/insufficient_memory.h>
#include <orly/server/insufficient_storage.h>
#include <orly/server/write_too_large.h>
#include <orly/server/memory_budget.h>
#include <orly/sabot/to_native.h>
#include <base/strm/fd.h>
#include <base/strm/bin/in.h>
#include <base/strm/bin/out.h>
#include <base/strm/past_end.h>
#include <base/util/error.h>
#include <base/util/io.h>
#include <base/util/path.h>
#include <base/util/time.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Io;
using namespace Socket;
using namespace Rpc;
using namespace Orly;
using namespace Orly::Handshake;
using namespace Orly::Indy;
using namespace Orly::Server;
using namespace ::Util;

/* The address a listener binds (#705): an IPv4 (a.b.c.d) or IPv6 literal, and a port. Throws
   std::invalid_argument for anything else; hostnames are not resolved. */
static TAddress MakeBindAddress(const string &host, in_port_t port) {
  sockaddr_storage storage{};
  if (inet_pton(AF_INET, host.c_str(), &reinterpret_cast<sockaddr_in &>(storage).sin_addr) == 1) {
    storage.ss_family = AF_INET;
  } else if (inet_pton(AF_INET6, host.c_str(), &reinterpret_cast<sockaddr_in6 &>(storage).sin6_addr) == 1) {
    storage.ss_family = AF_INET6;
  } else {
    throw invalid_argument("\"" + host + "\" is not an IPv4 or IPv6 address");
  }
  TAddress address(reinterpret_cast<const sockaddr &>(storage));
  address.SetPort(port);
  return address;
}

/* The address and port a listening socket actually got, for the startup log (#705). A port of 0
   asks the kernel for one, so the configured port isn't always the bound one. */
static string DescribeBound(int fd) {
  sockaddr_storage storage{};
  socklen_t len = sizeof(storage);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &len) < 0) {
    return "(unknown)";
  }
  return AsStr(TAddress(reinterpret_cast<const sockaddr &>(storage)));
}

const Orly::Indy::TMasterContext::TProtocol Orly::Indy::TMasterContext::TProtocol::Protocol;
const Orly::Indy::TSlaveContext::TProtocol Orly::Indy::TSlaveContext::TProtocol::Protocol;

static const size_t BlockSize = Disk::Util::PhysicalBlockSize;

//static const size_t StackSize = 8 * 1024 * 1024;
static const size_t StackSize = 1 * 1024 * 1024;

Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::Pool(sizeof(Disk::TDurableManager::TMapping), "Durable Mapping");
Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::TEntry::Pool(sizeof(Disk::TDurableManager::TMapping::TEntry), "Durable Mapping Entry");
Orly::Indy::Util::TPool Disk::TDurableManager::TDurableLayer::Pool(std::max(sizeof(Disk::TDurableManager::TMemSlushLayer), sizeof(Disk::TDurableManager::TDiskOrderedLayer)), "Durable Layer");
Orly::Indy::Util::TPool Disk::TDurableManager::TMemSlushLayer::TDurableEntry::Pool(sizeof(Disk::TDurableManager::TMemSlushLayer::TDurableEntry), "Durable Entry");

Orly::Indy::Util::TPool TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping");
Orly::Indy::Util::TPool TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry");
Orly::Indy::Util::TPool TRepo::TDataLayer::Pool(max(sizeof(TMemoryLayer), sizeof(TDiskLayer)), "Data Layer");

Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(max(max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation");
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction");

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update");
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry");
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(BlockSize);

Base::TThreadLocalSigmaCalc TSession::TServer::TryReadTimeCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryReadCPUTimeCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWriteTimeCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWriteCPUTimeCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWalkerCountCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryCallCPUTimerCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryReadCallTimerCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWriteCallTimerCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWalkerConsTimerCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryFetchCountCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryHashHitCountCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWriteSyncHitCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryWriteSyncTimeCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryReadSyncHitCalc;
Base::TThreadLocalSigmaCalc TSession::TServer::TryReadSyncTimeCalc;

TServer::TCmd::TMeta::TMeta(const char *desc)
    : TLog::TCmd::TMeta(desc) {
  Param(
      &TCmd::PortNumber, "port_number", Optional, "port_number\0pn\0",
      "The port on which the server listens for clients."
  );
  Param(
      &TCmd::WsPortNumber, "ws_port_number", Optional, "ws_port_number\0wspn\0",
      "The port on which the server listens for websocket clients."
  );
  Param(
      &TCmd::SlavePortNumber, "slave_port_number", Optional, "slave_port_number\0spn\0",
      "The port on which the server listens for a slave."
  );
  Param(
      &TCmd::BindAddress, "bind_address", Optional, "bind_address\0",
      "The IP address the client, websocket and reporting listeners bind (default 127.0.0.1, this host only). "
      "Orly has no authentication: pass 0.0.0.0 to listen on every interface only on a trusted network."
  );
  Param(
      &TCmd::SlaveBindAddress, "slave_bind_address", Optional, "slave_bind_address\0",
      "The IP address the replication (slave) listener binds (default 0.0.0.0, every interface, so a slave on another "
      "host can reach it). Replication is unauthenticated: set this to a private network's address."
  );
  Param(
      &TCmd::AllowRemoteCompile, "allow_remote_compile", Optional, "allow_remote_compile\0",
      "Accept the compile statement over websocket (default off). It builds the source it is sent with the system "
      "compiler and loads it into this process; refused, it replies \"status\": \"remote_compile_disabled\"."
  );
  Param(
      &TCmd::AuthTokenFileFlag, "auth_token_file", Optional, "auth_token_file\0",
      "A file holding the shared secret every client must present (#710; default none, no authentication). "
      "Also ORLY_AUTH_TOKEN_FILE, or the token itself in ORLY_AUTH_TOKEN. 16 to 1024 printable ASCII characters."
  );
  Param(
      &TCmd::AuthTokenFlag, "auth_token", Optional, "auth_token\0",
      "The shared secret itself (#710). Prefer --auth_token_file or ORLY_AUTH_TOKEN: a flag shows in process listings."
  );
  Param(
      &TCmd::ReplicationTokenFileFlag, "replication_token_file", Optional, "replication_token_file\0",
      "A file holding the shared secret a slave must present to join this master, and that this server presents when "
      "it joins one (#710). Also ORLY_REPLICATION_TOKEN_FILE or ORLY_REPLICATION_TOKEN. Default: the client token."
  );
  Param(
      &TCmd::ReplicationTokenFlag, "replication_token", Optional, "replication_token\0",
      "The replication secret itself (#710). Prefer --replication_token_file: a flag shows in process listings."
  );
  Param(
      &TCmd::ConnectionBacklog, "connection_backlog", Optional, "connection_backlog\0cb\0",
      "The maximum number of client connection requests to backlog."
  );
  Param(
      &TCmd::DurableCacheSize, "durable_cache_size", Optional, "durable_cache_size\0",
      "The maximum number of durable objects to keep cached in memory."
  );
  Param(
      &TCmd::IdleConnectionTimeout, "idle_connection_timeout", Optional, "idle_connection_timeout\0",
      "The maximum number of milliseconds a connection can remain idle before the server hangs up."
  );
  Param(
      &TCmd::HousecleaningInterval, "housecleaning_interval", Optional, "housecleaning_interval\0",
      "The minimum number of milliseconds between rounds of housecleaning."
  );
  Param(
      &TCmd::LayerCleaningInterval, "layer_cleaning_interval", Optional, "layer_cleaning_interval\0",
      "The minimum number of milliseconds between rounds of layer cleaning."
  );
  Param(
      &TCmd::MemorySim, "mem_sim", Optional, "mem_sim\0",
      "Run in memory simulation mode. This will avoid mounting block devices. Treat this server as frangible."
  );
  Param(
      &TCmd::MemorySimMB, "mem_sim_mb", Optional, "mem_sim_mb\0",
      "When running in memory simulation mode. This option determines in MB how much memory is used to back the volume."
  );
  Param(
      &TCmd::MemorySimSlowMB, "mem_sim_slow_mb", Optional, "mem_sim_slow_mb\0",
      "When running in memory simulation mode. This option determines in MB how much memory is used to back the slow volume."
  );
  Param(
      &TCmd::TempFileConsolidationThreshold, "temp_file_consol_thresh", Optional, "temp_file_consol_thresh\0",
      "The number of files that can be in a single generation of temporary files before they get merged into the next generation."
  );
  Param(
      &TCmd::InstanceName, "instance_name", Required, "instance_name\0iname\0",
      "The name of the instance to launch. This will mount all volumes associated with this instance name."
  );
  Param(
      &TCmd::PageCacheSizeMB, "page_cache_size", Optional, "page_cache_size\0",
      "The size of the page cache in MB. This cache uses 4K pages."
  );
  Param(
      &TCmd::BlockCacheSizeMB, "block_cache_size", Optional, "block_cache_size\0",
      "The size of the block cache in MB. This cache uses 64K blocks."
  );
  Param(
      &TCmd::FileServiceAppendLogMB, "file_service_append_log_size", Optional, "file_service_append_log_size\0",
      "The size of the file service append log in MB."
  );
  Param(
      &TCmd::DiskMaxAioNum, "disk_max_aio_num", Optional, "disk_max_aio_num\0",
      "The maximum number of aio events at a time."
  );
  Param(
      &TCmd::HighDiskUtilizationThreshold, "high_disk_utilization_threshold", Optional, "high_disk_utilization_threshold\0",
      "The percentage of disk space that needs to be used before we start re-routing discard blocks to become ready for allocation."
  );
  Param(
      &TCmd::DiscardOnCreate, "discard_on_create", Optional, "discard_on_create\0",
      "If create=true, this option determines whether a full discard will be done on the block device upon startup."
  );
  Param(
      &TCmd::ReplicationSyncBufMB, "replication_sync_buf_mb", Optional, "replication_sync_buf_mb\0",
      "The buffer used by the slave while synchronizing replication updates. This only stores live transactions, seperate from the large background sync."
  );
  Param(
      &TCmd::MergeMemInterval, "mem_interval", Optional, "mem_interval\0",
      "The minimum number of milliseconds between merge and flush of memory layers in a specific repo."
  );
  Param(
      &TCmd::MergeDiskInterval, "merge_disk_interval", Optional, "merge_disk_interval\0",
      "The minimum number of milliseconds between merges of disk layers of a specific size category, in a specific repo."
  );
  Param(
      &TCmd::ReplicationInterval, "replication_interval", Optional, "replication_interval\0",
      "The minimum number of milliseconds between replication batches sent to the slave."
  );
  Param(
      &TCmd::DurableWriteInterval, "durable_write_interval", Optional, "durable_write_interval\0",
      "The minimum number of milliseconds between durables being flushed to disk."
  );
  Param(
      &TCmd::DurableMergeInterval, "durable_merge_interval", Optional, "durable_merge_interval\0",
      "The minimum number of milliseconds between durable index files being merged."
  );
  Param(
      &TCmd::StartingState, "starting_state", Required, "starting_state\0",
      "The starting state of this server. Either SOLO or SLAVE."
  );
  Param(
      &TCmd::NumMemMergeThreads, "num_mem_threads", Optional, "num_mem_threads\0",
      "The number of threads merging and flushing memory layers in repos."
  );
  Param(
      &TCmd::NumDiskMergeThreads, "num_disk_merge_threads", Optional, "num_disk_merge_threads\0",
      "The number of threads merging disk layers in repos."
  );
  Param(
      &TCmd::NumWsThreads, "num_ws_threads", Optional, "num_ws_threads\0",
      "The number of threads to use to answer websocket requests."
  );
  Param(
      &TCmd::MaxRepoCacheSize, "max_repo_cache_size", Optional, "max_repo_cache_size\0",
      "The maximum number of unused repos that can be held in memory."
  );
  Param(
      &TCmd::FastCoreVec, "fast_cores", Optional, "fast_cores\0",
      "The cores which will be pinned by the fast non-blocking schedulers."
  );
  Param(
      &TCmd::SlowCoreVec, "slow_cores", Optional, "slow_cores\0",
      "The cores which will be pinned by the slow blocking schedulers."
  );
  Param(
      &TCmd::DiskControllerCoreVec, "disk_controller_cores", Optional, "disk_controller_cores\0",
      "The cores which will be pinned by the disk controllers."
  );
  Param(
      &TCmd::MemMergeCoreVec, "mem_merge_cores", Optional, "mem_merge_cores\0",
      "The cores which will be pinned by the memory mergers."
  );
  Param(
      &TCmd::NumFiberFrames, "max_parallel_frames", Optional, "max_parallel_frames\0",
      "The maximum number of stacks allocated to do parallel tasks."
  );
  Param(
      &TCmd::NumDiskEvents, "disk_event_pool_size", Optional, "disk_event_pool_size\0",
      "The maximum number of disk IO events that can be outstanding at once."
  );
  Param(
      &TCmd::DiskMergeCoreVec, "disk_merge_cores", Optional, "disk_merge_cores\0",
      "The cores which will be pinned by the disk mergers."
  );
  Param(
      &TCmd::AddressOfMaster, "address_of_master", Optional, "address_of_master\0",
      "The IP address of the master to which this slave should try to connect."
  );
  Param(
      &TCmd::PackageDirectory, "package_dir", Optional, "package_dir\0",
      "The directory in which packages are located"
  );
  Param(
      &TCmd::Create, "create", Required, "create\0",
      "Create a new disk image or use the existing image. true implies that all your data will be WIPED OUT! false implies that you are starting from the existing image on disk."
  );
  Param(
      &TCmd::ReportingPortNumber, "reporting_port_number", Optional, "reporting_port_number\0rpn\0",
      "The port on which the server listens for HTTP status requests"
  );
  Param(
      &TCmd::AllowTailing, "allow_tailing", Optional, "allow_tailing\0",
      "Turn on / off support for the tail statement, which drops superseded versions from the global pov's oldest disk file."
  );
  Param(
      &TCmd::OpenCheck, "open_check", Optional, "open_check\0",
      "Turn on / off the consistency check of a reopened store (#700): every block held must belong to exactly "
      "one file, base image or append log, and each repo's files must have disjoint sequence ranges. Leaked "
      "blocks and overlapping ranges are logged; a block a live file owns that is free or owned twice refuses the "
      "open, because new files would overwrite it. On by default. Off is for recovering such a store."
  );
  Param(
      &TCmd::PruneMergeHistory, "prune_merge_history", Optional, "prune_merge_history\0",
      "Turn on / off dropping superseded versions when the global pov's disk files are merged (#592). On by default. "
      "Off keeps every version of every key on disk, so disk use grows with every write. Independent of allow_tailing."
  );
  Param(
      &TCmd::AllowFileSync, "allow_file_sync", Optional, "allow_file_sync\0",
      "Turn on / off support for file_sync. On means we sync from scratch much faster. You want to turn this off if the underlying data layout has changed."
  );
  Param(
      &TCmd::NoRealtime, "no_realtime", Optional, "no_realtime\0",
      "Do not use realtime thread priorities (realtime priorities require root privileges)."
  );
  Param(
      &TCmd::DoFsync, "do_fsync", Optional, "do_fsync\0",
      "Turn on / off use of fsync on disk writes that change server state."
  );
  Param(
      &TCmd::LogAssertionFailures, "log_assertion_failures", Optional, "laf\0",
      "Log tetris assertion failures to LOG_INFO."
  );
  Param(
      &TCmd::TetrisCommutativeFastlane, "tetris_commutative_fastlane", Optional, "tetris_commutative_fastlane\0tcf\0",
      "Promote ALL ready commutative (assertion-free) children per global-merge "
      "round instead of one, collapsing the O(N^2) per-round re-snapshot into "
      "O(N) (issue #234). Default off: measured throughput- and "
      "sustainability-neutral under tested loads (the binding constraint is the "
      "per-write acceptance path, not the merge -- see "
      "docs/design/concurrent-merge-throughput.md)."
  );
  Param(
      &TCmd::TetrisBackpressureThreshold, "tetris_backpressure_threshold", Optional, "tetris_backpressure_threshold\0tbt\0",
      "Write-backpressure high-watermark (issue #234): when a writer's POV child "
      "repo has more than this many un-promoted updates backed up in its memtable, "
      "the accept path cooperatively yields until the global merge drains below "
      "the watermark, so accept paces to promote instead of growing memtables "
      "without bound. 0 disables. Default 50000."
  );
  Param(
      &TCmd::DiskReserveMb, "disk_reserve_mb", Optional, "disk_reserve_mb\0",
      "Write admission (issue #590): refuse writes, with an insufficient_storage error, while "
      "free disk space is below this many MiB plus what recent disk merges may still "
      "allocate. Reads keep working. Below half the reserve, merges and flushes stop as well, "
      "keeping the rest for session state and the file map. Default 0: use disk_reserve_pct."
  );
  Param(
      &TCmd::DiskReservePct, "disk_reserve_pct", Optional, "disk_reserve_pct\0",
      "Write admission reserve as a percentage of total disk space (issue #590), used when "
      "disk_reserve_mb is 0; never less than 64 MiB or a quarter of the disk, whichever is "
      "smaller. Setting this and disk_reserve_mb both to 0 turns write admission off. "
      "Default 10."
  );
  Param(
      &TCmd::MemoryReservePct, "memory_reserve_pct", Optional, "memory_reserve_pct\0",
      "Memory admission (issue #607): keep this percent of the Update and Update Entry pools "
      "for the merges and Tetris promotions that free them, and refuse writes, with an "
      "insufficient_memory error, before they would use it. Reads, new sessions and new POVs "
      "are never refused. 0 turns memory admission off. Default 25."
  );
  Param(
      &TCmd::ReadBudgetMB, "read_budget_mb", Optional, "read_budget_mb\0",
      "The per-read memory budget (issue #694): a method call that builds more than this many "
      "MiB of results is refused with a read_too_large error. Default: a sixteenth of the "
      "memory budget (--memory_budget_mb), at least 16 MiB. 0 means no limit, and, unless "
      "read_budget_rows is given too, no row limit either."
  );
  Param(
      &TCmd::ReadBudgetRows, "read_budget_rows", Optional, "read_budget_rows\0",
      "The per-read work budget (issue #694): a method call that walks more than this many "
      "rows (each key a range read visits, and each point read) is refused with a "
      "read_too_large error. Default: read_budget_mb / 256 bytes. 0 means no limit."
  );

  /******** Object Pools ********/

  Param(
      &TCmd::DurableMappingPoolSize, "durable_mapping_pool_size", Optional, "durable_mapping_pool_size\0",
      "The number of durable mapping pool objects."
  );
  Param(
      &TCmd::DurableMappingEntryPoolSize, "durable_mapping_entry_pool_size", Optional, "durable_mapping_entry_pool_size\0",
      "The number of durable mapping entry pool objects."
  );
  Param(
      &TCmd::DurableLayerPoolSize, "durable_layer_pool_size", Optional, "durable_layer_pool_size\0",
      "The number of durable layer pool objects."
  );
  Param(
      &TCmd::DurableMemEntryPoolSize, "durable_mem_entry_pool_size", Optional, "durable_mem_entry_pool_size\0",
      "The number of durable mem entry pool objects."
  );

  Param(
      &TCmd::RepoMappingPoolSize, "repo_mapping_pool_size", Optional, "repo_mapping_pool_size\0",
      "The number of repo mapping pool objects."
  );
  Param(
      &TCmd::RepoMappingEntryPoolSize, "repo_mapping_entry_pool_size", Optional, "repo_mapping_entry_pool_size\0",
      "The number of repo mapping entry pool objects."
  );
  Param(
      &TCmd::RepoDataLayerPoolSize, "repo_data_layer_pool_size", Optional, "repo_data_layer_pool_size\0",
      "The number of repo data layer pool objects."
  );

  Param(
      &TCmd::TransactionMutationPoolSize, "transaction_mutation_pool_size", Optional, "transaction_mutation_pool_size\0",
      "The number of transaction mutation pool objects."
  );
  Param(
      &TCmd::TransactionPoolSize, "transaction_pool_size", Optional, "transaction_pool_size\0",
      "The number of transaction pool objects."
  );

  Param(
      &TCmd::UpdatePoolSize, "update_pool_size", Optional, "update_pool_size\0",
      "The number of update pool objects."
  );
  Param(
      &TCmd::UpdateEntryPoolSize, "update_entry_pool_size", Optional, "update_entry_pool_size\0",
      "The number of update entry pool objects."
  );
  Param(
      &TCmd::DiskBufferBlockPoolSize, "disk_buffer_block_pool_size", Optional, "disk_buffer_block_pool_size\0",
      "The number of disk buffer blocks."
  );

  /******** End Object Pools ********/

  Param(
      &TCmd::MemoryBudgetMB, "memory_budget_mb", Optional, "memory_budget_mb\0",
      "The memory, in MiB, that every pool, cache and frame count not set by its own flag is sized "
      "from, in fixed proportions (#669). A smaller cgroup memory limit (a container's --memory, a "
      "systemd MemoryMax=) still wins. Default 0: the cgroup limit if there is one smaller than "
      "free RAM, otherwise free RAM. A cgroup limit or this flag is treated as a total: sizes given "
      "explicitly come out of it first. orlyi refuses to start when the budget can't fund the "
      "minimum working set."
  );

}

class TIndexIdReader
  : public Indy::Disk::TReadFile<Indy::Disk::Util::LogicalPageSize, Indy::Disk::Util::LogicalBlockSize, Indy::Disk::Util::PhysicalBlockSize, Indy::Disk::Util::CheckedPage> {
  NO_COPY(TIndexIdReader);
  public:

  static constexpr size_t PhysicalCachePageSize = Indy::Disk::Util::PhysicalBlockSize / (Indy::Disk::Util::LogicalBlockSize / Indy::Disk::Util::LogicalPageSize);

  using TArena = Orly::Indy::Disk::TDiskArena<Indy::Disk::Util::LogicalPageSize, Indy::Disk::Util::LogicalBlockSize, Indy::Disk::Util::PhysicalBlockSize, Indy::Disk::Util::CheckedPage, 128, true>;

  //typedef Indy::Disk::TStream<Indy::Disk::Util::LogicalPageSize, Indy::Disk::Util::LogicalBlockSize, Indy::Disk::Util::PhysicalBlockSize, Indy::Disk::Util::CheckedPage> TInStream;
  typedef Orly::Indy::Disk::TReadFile<Indy::Disk::Util::LogicalPageSize, Indy::Disk::Util::LogicalBlockSize, Indy::Disk::Util::PhysicalBlockSize, Indy::Disk::Util::CheckedPage> TMyReadFile;

  TIndexIdReader(Disk::Util::TEngine *engine, const Base::TUuid &file_id, Indy::Disk::DiskPriority priority, size_t gen_id, size_t starting_block_id, size_t starting_block_offset, size_t file_length)
      : TMyReadFile(HERE, Indy::Disk::Source::System, engine->GetPageCache(), file_id, priority, gen_id, starting_block_id, starting_block_offset, file_length) {}

  virtual ~TIndexIdReader() {}

  using TReadFile::GetIndexByIdMap;

  using TReadFile::TIndexFile;
};

TServer::TCmd::TCmd()
    : PortNumber(DefaultPortNumber),
      WsPortNumber(8082),
      SlavePortNumber(DefaultSlavePortNumber),
      ConnectionBacklog(5000),
      DurableCacheSize(10000),
      IdleConnectionTimeout(2000),
      HousecleaningInterval(5000),
      LayerCleaningInterval(50),
      MemorySim(false),
      MemorySimMB(1024),
      MemorySimSlowMB(512),
      TempFileConsolidationThreshold(20),
      PageCacheSizeMB(1024),
      BlockCacheSizeMB(256),
      FileServiceAppendLogMB(4),
      DiskMaxAioNum(65024),
      HighDiskUtilizationThreshold(0.9),
      DiscardOnCreate(false),
      ReplicationSyncBufMB(32),
      MergeMemInterval(40),
      MergeDiskInterval(10),
      ReplicationInterval(100),
      DurableWriteInterval(40),
      DurableMergeInterval(10),
      NumMemMergeThreads(3),
      NumDiskMergeThreads(8),
      NumWsThreads(4),
      MaxRepoCacheSize(10000),
      NumFiberFrames(1000UL),
      NumDiskEvents(10000UL),
      ReportingPortNumber(19388),
      AllowTailing(true),
      OpenCheck(true),
      PruneMergeHistory(true),
      AllowFileSync(true),
      NoRealtime(false),
      DoFsync(true),
      LogAssertionFailures(true),
      TetrisCommutativeFastlane(false),
      TetrisBackpressureThreshold(50000UL),
      DiskReserveMb(0UL),
      DiskReservePct(10UL),
      MemoryReservePct(25UL),
      ReadBudgetMB(0UL),
      ReadBudgetRows(0UL),
      DurableMappingPoolSize(1000UL),
      DurableMappingEntryPoolSize(10000UL),
      DurableLayerPoolSize(2000UL),
      DurableMemEntryPoolSize(50000UL),
      RepoMappingPoolSize(5000UL),
      RepoMappingEntryPoolSize(50000UL),
      RepoDataLayerPoolSize(5000UL),
      TransactionMutationPoolSize(1500UL),
      TransactionPoolSize(500UL),
      UpdatePoolSize(100000UL),
      UpdateEntryPoolSize(200000UL),
      DiskBufferBlockPoolSize(7500UL),
      MemoryBudgetMB(0UL),
      PackageDirectory(GetCwd()) {
  /* These defaults are for a 4 GiB budget. Unless the command line gives them,
     ResolveMemoryDefaults() scales them to the actual one once Parse() is done (#669). The
     core vectors likewise wait for ResolveCoreVecDefaults() (#240). */
}

namespace {

  constexpr size_t MiB = 1024UL * 1024UL;

  /* TCmd's constructor holds the pre-#669 defaults, which were written for a machine with this
     much free memory. A budget scales each of them by budget / this. */
  constexpr size_t DefaultsAreForBytes = 4096UL * MiB;

  /* What orlyi holds beyond the memory it plans for: code, libraries, thread stacks, the heap.
     Measured on the arm64 image under a 1 GiB limit (#669): 13 MiB resident outside the planned
     (locked) memory at idle, 32 MiB at the peak of the pool-pressure smoke. It's counted as
     fixed so a hard budget leaves room for it. */
  constexpr size_t ProcessBaselineBytes = 32UL * MiB;

  /* What a hard budget keeps free for the heap, beyond the proportional share the scaling leaves.
     The heap's growth under load hardly depends on the budget: measured on the image (#669), the
     pool-pressure smoke peaked about 40 MiB above idle and #629's memory-full smoke about
     100 MiB, at 640 MiB and at 1 GiB alike. Without this, a budget at the old floor of 620 MiB
     was OOM-killed in one memory-full run of two. */
  constexpr size_t MinHeapBytes = 128UL * MiB;

  /* The minimum working set (#669): the merge reserve must hold one memory merge's copy of a
     backlog of this many writes. */
  constexpr size_t MinMergeCopyWrites = 1000UL;

  /* The per-read budget's floor, and the heap a row costs at least once a read holds it (#694). */
  constexpr size_t MinReadBudgetMB = 16UL;
  constexpr size_t ReadBudgetBytesPerRow = 256UL;

  /* The fewest fiber frames to plan for. orlyi's own service loops hold some for good and every
     request in flight holds another. Measured on the image (#669): with 16 it can't start, with
     24 it fails its first client, and 32 passed the pool-pressure smoke's 8 writers. */
  constexpr size_t MinFiberFrames = 64UL;

  /* The share of the Update and Update Entry pools kept for merges, in percent:
     --memory_reserve_pct and its default (#607). A reserve of 0 (admission off) still sizes the
     floor for the default, because merges need the room either way. */
  constexpr size_t DefaultMergeReservePct = 25UL;

  template <typename TSomeCmd>
  size_t GetMergeReservePct(const TSomeCmd &cmd) {
    if constexpr (requires { cmd.MemoryReservePct; }) {
      return cmd.MemoryReservePct ? static_cast<size_t>(cmd.MemoryReservePct) : DefaultMergeReservePct;
    } else {
      return DefaultMergeReservePct;
    }
  }

  size_t CeilDiv(size_t num, size_t den) {
    return (num + den - 1) / den;
  }

  size_t ToMiB(size_t bytes) {
    return (bytes + MiB / 2) / MiB;
  }

  /* One size the budget sets. */
  struct TSized {

    TSized(const char *flag, size_t *field, size_t bytes_per_unit, bool allocated)
        : Flag(flag), Field(field), BytesPerUnit(bytes_per_unit), Allocated(allocated), Default(*field), Given(false) {}

    /* Its flag. */
    const char *Flag;

    /* The field. */
    size_t *Field;

    /* Bytes per unit of the field. */
    size_t BytesPerUnit;

    /* False if this run doesn't allocate it (the mem-sim volumes on a disk-backed server). */
    bool Allocated;

    /* The field's value before scaling. */
    size_t Default;

    /* True if the command line gave it. */
    bool Given;

    size_t GetBytes() const {
      return Allocated ? *Field * BytesPerUnit : 0UL;
    }

  };  // TSized

}  // namespace

bool TServer::TCmd::CheckArgs(const Base::TCmd::TMeta::TMessageConsumer &cb) {
  if (!Base::TLog::TCmd::CheckArgs(cb)) {
    return false;
  }
  for (const auto &[flag, host]: {pair<const char *, const string &>{"bind_address", BindAddress},
                                  pair<const char *, const string &>{"slave_bind_address", SlaveBindAddress}}) {
    try {
      MakeBindAddress(host, 0);
    } catch (const invalid_argument &ex) {
      cb(string("--") + flag + ": " + ex.what());
      return false;
    }
  }
  /* Shared secrets (#710). Errors name the source, never the token. */
  try {
    AuthToken = Orly::Auth::ResolveToken(
        AuthTokenFileFlag, AuthTokenFlag, "auth_token", Orly::Auth::TokenFileEnvVar, Orly::Auth::TokenEnvVar);
    ReplicationToken = Orly::Auth::ResolveToken(
        ReplicationTokenFileFlag, ReplicationTokenFlag, "replication_token",
        Orly::Auth::ReplicationTokenFileEnvVar, Orly::Auth::ReplicationTokenEnvVar);
  } catch (const exception &ex) {
    cb(ex.what());
    return false;
  }
  if (!ReplicationToken) {
    ReplicationToken = AuthToken;
  }
  return ResolveMemoryDefaults(cb);
}

bool TServer::TCmd::ResolveMemoryDefaults(const Base::TCmd::TMeta::TMessageConsumer &cb) {
  MemoryBudgetReport.clear();
  std::vector<TSized> sized = {
    {"max_parallel_frames", &NumFiberFrames, StackSize + sizeof(Fiber::TFrame), true},
    {"page_cache_size", &PageCacheSizeMB, MiB, true},
    {"block_cache_size", &BlockCacheSizeMB, MiB, true},
    {"disk_buffer_block_pool_size", &DiskBufferBlockPoolSize, Disk::Util::PhysicalBlockSize, true},
    {"update_entry_pool_size", &UpdateEntryPoolSize, sizeof(TUpdate::TEntry), true},
    {"update_pool_size", &UpdatePoolSize, sizeof(TUpdate), true},
    {"transaction_pool_size", &TransactionPoolSize, L1::TTransaction::GetTransactionSize(), true},
    {"transaction_mutation_pool_size", &TransactionMutationPoolSize, L1::TTransaction::GetTransactionMutationSize(), true},
    {"repo_data_layer_pool_size", &RepoDataLayerPoolSize, Indy::TManager::GetDataLayerSize(), true},
    {"repo_mapping_entry_pool_size", &RepoMappingEntryPoolSize, Indy::TManager::GetMappingEntrySize(), true},
    {"repo_mapping_pool_size", &RepoMappingPoolSize, Indy::TManager::GetMappingSize(), true},
    {"durable_mem_entry_pool_size", &DurableMemEntryPoolSize, Disk::TDurableManager::GetMemEntrySize(), true},
    {"durable_layer_pool_size", &DurableLayerPoolSize, Disk::TDurableManager::GetDurableLayerSize(), true},
    {"durable_mapping_entry_pool_size", &DurableMappingEntryPoolSize, Disk::TDurableManager::GetMappingEntrySize(), true},
    {"durable_mapping_pool_size", &DurableMappingPoolSize, Disk::TDurableManager::GetMappingSize(), true},
    {"max_repo_cache_size", &MaxRepoCacheSize, sizeof(Indy::TManager::TRepo), true},
    {"durable_cache_size", &DurableCacheSize, std::max(sizeof(TPov), sizeof(TSession)), true},
    {"mem_sim_mb", &MemorySimMB, MiB, MemorySim},
    {"mem_sim_slow_mb", &MemorySimSlowMB, MiB, MemorySim},
    {"disk_event_pool_size", &NumDiskEvents, sizeof(Disk::Util::TDiskController::TEvent), true},
  };
  for (auto &item: sized) {
    item.Default = *item.Field;
    item.Given = WasGiven(item.Flag);
  }

  /* What's fixed whatever the budget: the process itself, the file service's append log, and
     everything the command line sized. */
  size_t fixed_bytes = ProcessBaselineBytes + FileServiceAppendLogMB * MiB;
  std::ostringstream fixed_desc;
  fixed_desc << "process baseline " << ToMiB(ProcessBaselineBytes) << ", append log " << FileServiceAppendLogMB;
  /* What the scaled sizes cost at their defaults, that is, for a 4 GiB budget. */
  size_t scaled_bytes_at_defaults = 0;
  for (const auto &item: sized) {
    if (item.Given) {
      fixed_bytes += item.GetBytes();
      if (item.Allocated) {
        fixed_desc << ", " << item.Flag << " " << ToMiB(item.GetBytes());
      }
    } else {
      scaled_bytes_at_defaults += item.GetBytes();
    }
  }

  /* The budget and where it came from. */
  const size_t page_size = getpagesize();
  const long avail_pages = sysconf(_SC_AVPHYS_PAGES);
  const size_t free_bytes = static_cast<size_t>(std::max(avail_pages, 0L)) * page_size;
  const auto cgroup = ReadCgroupMemoryLimit();
  /* An explicit budget is used as given, except that a smaller cgroup limit still wins: past
     the limit the kernel kills the process, whatever it was told. */
  const size_t flag_bytes = MemoryBudgetMB * MiB;
  enum { FreeRam, Cgroup, Flag } source;
  size_t budget_bytes;
  if (MemoryBudgetMB && (!cgroup || flag_bytes <= cgroup->Bytes)) {
    source = Flag;
    budget_bytes = flag_bytes;
  } else if (cgroup && (MemoryBudgetMB || cgroup->Bytes < free_bytes)) {
    source = Cgroup;
    budget_bytes = cgroup->Bytes;
  } else {
    source = FreeRam;
    budget_bytes = free_bytes;
  }
  const std::string cgroup_desc = cgroup
      ? "cgroup limit " + std::to_string(ToMiB(cgroup->Bytes)) + " MiB in " + cgroup->File
      : std::string("no cgroup memory limit");
  std::ostringstream budget_line;
  budget_line << "memory budget: " << ToMiB(budget_bytes) << " MiB, from ";
  switch (source) {
    case FreeRam: {
      budget_line << "free RAM (" << cgroup_desc << ")";
      break;
    }
    case Cgroup: {
      budget_line << "the cgroup limit in " << cgroup->File << " (free RAM " << ToMiB(free_bytes) << " MiB";
      if (MemoryBudgetMB) {
        budget_line << ", --memory_budget_mb=" << MemoryBudgetMB << " is larger";
      }
      budget_line << ")";
      break;
    }
    case Flag: {
      budget_line << "--memory_budget_mb (free RAM " << ToMiB(free_bytes) << " MiB, " << cgroup_desc << ")";
      break;
    }
  }
  MemoryBudgetReport.emplace_back(LOG_INFO, budget_line.str());

  /* The scale. Sized from free RAM, as before #669: the defaults times free RAM / 4 GiB, with
     nothing taken off for the sizes the command line gave. That keeps a bare host's pools
     exactly what they were.

     A cgroup limit or --memory_budget_mb is a total that orlyi must stay inside, or a container
     is OOM-killed. The fixed costs and MinHeapBytes come out of it first, and the defaults are
     scaled to the rest, in the same proportions. At 4 GiB the defaults plan 70% of it (frames 25%, page cache
     25%, block cache and disk buffers 18%, pools 2%) and leave 30% for dynamic use, such as
     merges, sessions and compiles; the mem-sim volumes, when they too are left to the budget,
     would push that past 100%, so the scaled sizes never get more than 75% of the rest. */
  double scale;
  size_t scale_base = DefaultsAreForBytes;
  const size_t held_back_bytes = fixed_bytes + MinHeapBytes;
  if (source == FreeRam) {
    /* The same expression as before #669, so the sizes match to the unit. */
    scale = static_cast<double>(std::max(avail_pages, 0L)) / (DefaultsAreForBytes / page_size);
  } else {
    scale_base = std::max(DefaultsAreForBytes, scaled_bytes_at_defaults / 3 * 4);
    scale = budget_bytes > held_back_bytes ? static_cast<double>(budget_bytes - held_back_bytes) / scale_base : 0.0;
  }
  for (auto &item: sized) {
    if (!item.Given) {
      *item.Field = static_cast<size_t>(static_cast<double>(*item.Field) * scale);
    }
  }
  size_t planned_bytes = ProcessBaselineBytes + FileServiceAppendLogMB * MiB;
  std::ostringstream sizes;
  sizes << "memory plan sizes:";
  for (const auto &item: sized) {
    planned_bytes += item.GetBytes();
    sizes << ' ' << item.Flag << '=' << *item.Field << (item.Given ? " (given)" : "");
  }
  std::ostringstream plan_line;
  plan_line << "memory plan: " << ToMiB(planned_bytes) << " MiB of the " << ToMiB(budget_bytes) << " MiB budget; "
            << ToMiB(fixed_bytes) << " MiB fixed (" << fixed_desc.str() << "), ";
  if (source != FreeRam) {
    plan_line << ToMiB(MinHeapBytes) << " MiB kept for the heap, ";
  }
  plan_line << "the rest scaled by " << scale;
  MemoryBudgetReport.emplace_back(LOG_INFO, plan_line.str());
  MemoryBudgetReport.emplace_back(LOG_INFO, sizes.str());

  /* The per-read budget (#694). A read builds its results on the heap, which the plan above
     leaves to dynamic use alongside merges, sessions and compiles, and several reads run at
     once, so one read gets a sixteenth of the budget: 256 MiB at the 4 GiB the defaults were
     written for, 40 MiB in the smallest container orlyi starts in. A row costs at least
     ReadBudgetBytesPerRow of heap once a read holds it, so the row limit follows from that. An
     explicit 0 turns a limit off; --read_budget_mb=0 alone turns off both. */
  const bool read_mb_given = WasGiven("read_budget_mb");
  if (!read_mb_given) {
    ReadBudgetMB = std::max(ToMiB(budget_bytes) / 16UL, MinReadBudgetMB);
  }
  if (!WasGiven("read_budget_rows")) {
    ReadBudgetRows = ReadBudgetMB * MiB / ReadBudgetBytesPerRow;
  }
  std::ostringstream read_line;
  read_line << "read budget: ";
  if (ReadBudgetRows) {
    read_line << ReadBudgetRows << " rows";
  } else {
    read_line << "no row limit";
  }
  read_line << " and ";
  if (ReadBudgetMB) {
    read_line << ReadBudgetMB << " MiB";
  } else {
    read_line << "no memory limit";
  }
  read_line << " per read" << (read_mb_given || WasGiven("read_budget_rows") ? "" : ", from the memory budget");
  MemoryBudgetReport.emplace_back(LOG_INFO, read_line.str());

  /* The minimum working set. The merge reserve, a share of the Update and Update Entry pools
     kept for merges (#607/#629), must hold one memory merge's copy of a backlog of
     MinMergeCopyWrites writes: that many updates, and twice that many entries, because #629
     refuses any write bigger than half the entry reserve, so a batch of MinMergeCopyWrites
     entries must stay admissible. And there must be enough frames to run on. */
  const size_t reserve_pct = GetMergeReservePct(*this);
  struct TFloor {
    const char *Flag;
    size_t Need;
    std::string Why;
  };
  const std::string reserve_desc = "its " + std::to_string(reserve_pct) + "% merge reserve";
  const TFloor floors[] = {
    {"update_pool_size", CeilDiv(MinMergeCopyWrites * 100UL, reserve_pct), "for " + reserve_desc + " to hold one memory merge's copy of 1,000 writes"},
    {"update_entry_pool_size", CeilDiv(2UL * MinMergeCopyWrites * 100UL, reserve_pct), "for " + reserve_desc + " to admit and copy a 1,000-entry write"},
    {"max_parallel_frames", MinFiberFrames, "to run orlyi's own service loops and a few clients"},
  };
  std::vector<std::string> given_too_small;
  double scale_needed = 0.0;
  bool short_of_floor = false;
  for (const auto &need: floors) {
    for (const auto &item: sized) {
      if (std::string(item.Flag) != need.Flag || *item.Field >= need.Need) {
        continue;
      }
      std::ostringstream strm;
      strm << item.Flag << "=" << *item.Field << " is below " << need.Need << ", the least " << need.Why;
      if (item.Given) {
        given_too_small.push_back("--" + strm.str());
      } else {
        short_of_floor = true;
        scale_needed = std::max(scale_needed, static_cast<double>(need.Need) / std::max<size_t>(item.Default, 1UL));
      }
    }
  }
  if (!given_too_small.empty() || short_of_floor) {
    std::ostringstream msg;
    msg << "orlyi won't start: the memory budget can't fund the minimum working set. " << budget_line.str() << ". ";
    for (const auto &line: given_too_small) {
      msg << line << ". ";
    }
    if (short_of_floor) {
      /* The smallest budget that sizes every floored pool to its floor. */
      const size_t need_bytes = source == FreeRam
          ? static_cast<size_t>(std::ceil(scale_needed * DefaultsAreForBytes))
          : held_back_bytes + static_cast<size_t>(std::ceil(scale_needed * scale_base));
      msg << "The budget must be at least " << CeilDiv(need_bytes, MiB) << " MiB";
      if (source != FreeRam) {
        msg << ": " << ToMiB(fixed_bytes) << " MiB fixed (" << fixed_desc.str() << "), " << ToMiB(MinHeapBytes)
            << " MiB kept for the heap, and the rest for pools at their minimum";
      }
      msg << ". Raise the memory limit or --memory_budget_mb, or lower the sizes given explicitly.";
    }
    cb(msg.str());
    return false;
  }
  return true;
}

void TServer::TCmd::ResolveCoreVecDefaults() {
  /* Fill in the hardware-derived default core assignment, but ONLY for the core
     vectors the user did not specify on the command line (issue #240). Any
     vector the user provided via --fast_cores / --slow_cores / --mem_merge_cores
     / --disk_merge_cores / --disk_controller_cores is left exactly as parsed, so
     the flag overrides the default rather than appending to it. Must be called
     after Parse(). */
  const std::vector<size_t> user_fast = FastCoreVec, user_slow = SlowCoreVec,
                            user_mem = MemMergeCoreVec, user_disk = DiskMergeCoreVec,
                            user_ctl = DiskControllerCoreVec;
  FastCoreVec.clear();
  SlowCoreVec.clear();
  MemMergeCoreVec.clear();
  DiskMergeCoreVec.clear();
  DiskControllerCoreVec.clear();

  int num_proc = sysconf(_SC_NPROCESSORS_ONLN);
  /* Fast Core Vector */
  if (num_proc < 2) {
    syslog(LOG_ERR, "Indy is not supported on single processor machines");
  } else if (num_proc >= 64) {
    for (size_t i = 0; i < 12; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 32; i < 44; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 12; i < 16; ++i) {
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }
    for (size_t i = 44; i < 48; ++i) {
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }

    DiskControllerCoreVec.emplace_back(16UL);
    DiskControllerCoreVec.emplace_back(48UL);
    for (size_t i = 17; i < 32; ++i) {FastCoreVec.emplace_back(i);}
    for (size_t i = 49; i < 64; ++i) {FastCoreVec.emplace_back(i);}
  } else if (num_proc >= 32) {
    for (size_t i = 0; i < 6; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 16; i < 24; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 6; i < 8; ++i) {
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }
    for (size_t i = 22; i < 24; ++i) {
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }

    DiskControllerCoreVec.emplace_back(8UL);
    DiskControllerCoreVec.emplace_back(24UL);
    for (size_t i = 9; i < 16; ++i) {FastCoreVec.emplace_back(i);}
    for (size_t i = 25; i < 32; ++i) {FastCoreVec.emplace_back(i);}
  } else if (num_proc >= 16) {
    for (size_t i = 0; i < 3; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 8; i < 11; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 3; i < 4; ++i) {
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }
    for (size_t i = 11; i < 12; ++i) {
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }

    DiskControllerCoreVec.emplace_back(4UL);
    DiskControllerCoreVec.emplace_back(12UL);
    for (size_t i = 5; i < 8; ++i) {FastCoreVec.emplace_back(i);}
    for (size_t i = 13; i < 16; ++i) {FastCoreVec.emplace_back(i);}
  } else if (num_proc >= 8) {

    for (size_t i = 0; i < 2; ++i) {SlowCoreVec.emplace_back(i);}
    for (size_t i = 4; i < 6; ++i) {
      SlowCoreVec.emplace_back(i);
      MemMergeCoreVec.emplace_back(i);
      DiskMergeCoreVec.emplace_back(i);
    }

    DiskControllerCoreVec.emplace_back(2UL);
    for (size_t i = 3; i < 4; ++i) {FastCoreVec.emplace_back(i);}
    for (size_t i = 6; i < 8; ++i) {FastCoreVec.emplace_back(i);}
  } else if (num_proc >= 4) {
    MemMergeCoreVec.emplace_back(2UL);
    DiskMergeCoreVec.emplace_back(2UL);

    SlowCoreVec.emplace_back(0UL);

    DiskControllerCoreVec.emplace_back(2UL);
    FastCoreVec.emplace_back(1UL);
    FastCoreVec.emplace_back(3UL);
  } else if (num_proc >= 2) {
    MemMergeCoreVec.emplace_back(0UL);
    DiskMergeCoreVec.emplace_back(0UL);
    SlowCoreVec.emplace_back(0UL);
    DiskControllerCoreVec.emplace_back(0UL);
    FastCoreVec.emplace_back(1UL);
  }

  /* Restore any user-specified vectors, overriding the defaults just computed. */
  if (!user_fast.empty()) {FastCoreVec = user_fast;}
  if (!user_slow.empty()) {SlowCoreVec = user_slow;}
  if (!user_mem.empty()) {MemMergeCoreVec = user_mem;}
  if (!user_disk.empty()) {DiskMergeCoreVec = user_disk;}
  if (!user_ctl.empty()) {DiskControllerCoreVec = user_ctl;}

  /* Log the resolved core assignment (issue #240): makes the effective config
     visible and distinguishes user overrides from hardware defaults.  Syslog,
     not stdout -- orlyc's stdout is the compiler protocol (#440). */
  auto join_cores = [](const std::vector<size_t> &v) {
    std::stringstream s;
    for (size_t i = 0; i < v.size(); ++i) { s << (i ? "," : "") << v[i]; }
    return s.str();
  };
  std::stringstream resolved;
  resolved << "resolved core assignment:"
           << " fast=[" << join_cores(FastCoreVec) << "]"
           << " slow=[" << join_cores(SlowCoreVec) << "]"
           << " mem_merge=[" << join_cores(MemMergeCoreVec) << "]"
           << " disk_merge=[" << join_cores(DiskMergeCoreVec) << "]"
           << " disk_controller=[" << join_cores(DiskControllerCoreVec) << "]";
  syslog(LOG_INFO, "%s", resolved.str().c_str());
}

TServer::TServer(TScheduler *scheduler, const TCmd &cmd)
    : TSession::TServer(cmd.SlowCoreVec.size() +
                        cmd.FastCoreVec.size() +
                        cmd.NumMemMergeThreads +
                        cmd.NumDiskMergeThreads +
                        1UL /* File Service */ +
                        1UL /* Repo Layer Cleaner */ +
                        1UL /* BGFastRunner */ +
                        1UL /* WaitForSlaveRunner */ +
                        1UL /* RunReplicationQueueRunner */ +
                        1UL /* RunReplicationWorkRunner */ +
                        1UL /* RunReplicateTransactionRunner */ +
                        1UL /* WsRunner */ +
                        1UL /* Durable Layer Cleaner */ +
                        2UL /* Durable Manager */ +
                        1UL /* Tetris Manager -- one scheduler runner, by decision (#372): players are
                               already independent fibers on it, the measured tetris bottleneck was the
                               global pov's sequential fold (addressed by the #49 commutative fastlane,
                               which sharded runners cannot help), and N runners would multiply the
                               teardown join topology for a non-default path nothing would exercise. */),
                        //#endif
      Frame(nullptr),
      DurableLayerCleanerRunner(RunnerCons),
      RepoLayerCleanerRunner(RunnerCons),
      BGFastRunner(RunnerCons),
      WaitForSlaveRunner(RunnerCons),
      RunReplicationQueueRunner(RunnerCons),
      RunReplicationWorkRunner(RunnerCons),
      RunReplicateTransactionRunner(RunnerCons),
      WsRunner(RunnerCons),
      PackageManager(cmd.PackageDirectory),
      Scheduler(scheduler),
      Cmd(cmd),
      HousecleaningTimer(chrono::milliseconds(cmd.HousecleaningInterval)) {
  /* The memory budget and the sizes it chose (#669), decided while parsing, before the log
     was open. */
  for (const auto &[level, line]: Cmd.MemoryBudgetReport) {
    syslog(level, "%s", line.c_str());
  }
  InitalizeFramePoolManager(Cmd.NumFiberFrames, StackSize, &BGFastRunner);
  /* Point reads build their per-layer walkers on fibers; keep half the frames for everything
     else, so reads over many disk layers can't take every frame a statement needs to start
     (#762). */
  Indy::TRepo::SetReadPrepFrameBudget(Cmd.NumFiberFrames / 2UL);
  Disk::Util::TDiskController::TEvent::InitializeDiskEventPoolManager(Cmd.NumDiskEvents);
  using TLocalReadFileCache = Orly::Indy::Disk::TLocalReadFileCache<Orly::Indy::Disk::Util::LogicalPageSize,
    Orly::Indy::Disk::Util::LogicalBlockSize,
    Orly::Indy::Disk::Util::PhysicalBlockSize,
    Orly::Indy::Disk::Util::CheckedPage, true>;
  assert(scheduler);
  assert(cmd.StartingState.size());
  auto launch_slow_fiber_sched = [this](size_t core, Fiber::TRunner *runner) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core, &mask);
    /* Pinning is an optimization, not a requirement. Restricted environments
       (containers/CI, e.g. orlyc's embedded test server, #262) may forbid
       sched_setaffinity; log and run unpinned rather than failing startup. */
    if (sched_setaffinity(syscall(SYS_gettid), sizeof(cpu_set_t), &mask) < 0) {
      syslog(LOG_WARNING, "Slow Scheduler TID=[%ld] could not pin to core [%ld]: %s", syscall(SYS_gettid), core, strerror(errno));
    }
    syslog(LOG_INFO, "Slow Scheduler TID=[%ld] on core [%ld]", syscall(SYS_gettid), core); /* TEMP */
    if (!Fiber::TFrame::LocalFramePool) {
      Fiber::TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(FramePoolManager.get());
    }
    if (!Cmd.MemorySim) {
      /* if this is a disk based engine, allocate event pools */
      assert(!Disk::Util::TDiskController::TEvent::LocalEventPool);
      Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
    }
    runner->Run();
  };
  if (cmd.SlowCoreVec.size() < 1) {
    throw std::runtime_error("SlowCoreVec is required to have at least NumSlowRunners cores");
  }
  for (size_t i = 0; i < cmd.SlowCoreVec.size(); ++i) {
    SlowRunnerVec.emplace_back(new Fiber::TRunner(RunnerCons));
    syslog(LOG_INFO, "SLOW RUNNER [%ld] = [%p]", i, SlowRunnerVec.back().get());
    SlowRunnerThreadVec.emplace_back(new std::thread(std::bind(launch_slow_fiber_sched, cmd.SlowCoreVec[i], SlowRunnerVec.back().get())));
  }

  /* Run the WsRunner's thread. */
  WsThread = thread([this] { WsRunner.Run(); });

  /* From here on we own live runner threads.  If the rest of construction
     throws, unwinding would destroy joinable std::threads -- an instant
     std::terminate that masks the real error ("terminate called without an
     active exception", #435).  Catch, stop and join everything we started,
     then let the exception out. */
  try {

  auto launch_fast_fiber_sched = [this](size_t core, Fiber::TRunner *runner) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core, &mask);
    /* See note in launch_slow_fiber_sched: pinning is best-effort. */
    if (sched_setaffinity(syscall(SYS_gettid), sizeof(cpu_set_t), &mask) < 0) {
      syslog(LOG_WARNING, "Fast Scheduler TID=[%ld] could not pin to core [%ld]: %s", syscall(SYS_gettid), core, strerror(errno));
    }
    syslog(LOG_INFO, "Fast Scheduler TID=[%ld] on core [%ld]", syscall(SYS_gettid), core); /* TEMP */
    if (!Fiber::TFrame::LocalFramePool) {
      Fiber::TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(FramePoolManager.get());
    }
    if (!Cmd.MemorySim) {
      /* if this is a disk based engine, allocate event pools */
      assert(!Disk::Util::TDiskController::TEvent::LocalEventPool);
      Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
    }
    assert(!TLocalReadFileCache::Cache);
    TLocalReadFileCache::Cache = new TLocalReadFileCache();
    assert(!Disk::TLocalWalkerCache::Cache);
    Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
    runner->Run();
  };
  if (cmd.FastCoreVec.size() < 1UL) {
    throw std::runtime_error("FastCoreVec is required to have at least 1 core");
  }
  for (size_t i = 0; i < cmd.FastCoreVec.size(); ++i) {
    FastRunnerVec.emplace_back(new Fiber::TRunner(RunnerCons));
    FastRunnerThreadVec.emplace_back(new std::thread(std::bind(launch_fast_fiber_sched, cmd.FastCoreVec[i], FastRunnerVec.back().get())));
  }

  /* Launch the fast */
  auto launch_bg_fiber_sched = [this](Fiber::TRunner *runner) {
    syslog(LOG_INFO, "Bg Scheduler TID=[%ld]", syscall(SYS_gettid)); /* TEMP */
    if (!Fiber::TFrame::LocalFramePool) {
      Fiber::TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(FramePoolManager.get());
    }
    if (!Cmd.MemorySim) {
      /* if this is a disk based engine, allocate event pools */
      assert(!Disk::Util::TDiskController::TEvent::LocalEventPool);
      Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
    }
    assert(!TLocalReadFileCache::Cache);
    TLocalReadFileCache::Cache = new TLocalReadFileCache();
    assert(!Disk::TLocalWalkerCache::Cache);
    Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
    runner->Run();
  };
  ScheduleHostJob(std::bind(launch_bg_fiber_sched, &BGFastRunner));

  Fiber::TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(FramePoolManager.get());
  Frame = Fiber::TFrame::LocalFramePool->Alloc();
  assert(Frame);
  try {
    Frame->Latch(SlowRunnerVec[0].get(), this, static_cast<Fiber::TRunnable::TFunc>(&TServer::Init));
  } catch (...) {
    Fiber::TFrame::LocalFramePool->Free(Frame);
    throw;
  }
  /* handshake scope */ {
    std::unique_lock<std::mutex> lock(InitMutex);
    while (!InitFinished) {
      InitCond.wait(lock);
    }
  }
  if (InitError) {
    std::rethrow_exception(InitError);
  }

  /* Launch the websockets server. */
  Ws.reset(TWs::New(this, cmd.NumWsThreads, cmd.WsPortNumber, cmd.BindAddress, cmd.AllowRemoteCompile,
                    cmd.AuthToken.value_or(string())));

  } catch (const std::exception &ex) {
    /* We cannot unwind: several members' destructors (the durable manager,
       the repo manager) require the fiber context this thread does not
       have -- the same contradiction that keeps orlyi and orlyc from ever
       destroying a TServer.  Until teardown is a first-class operation,
       report the real error and exit; the alternative is std::terminate
       from a joinable-thread destructor with the message lost (#435). */
    syslog(LOG_ERR, "TServer failed to initialize: %s", ex.what());
    std::cerr << "TServer failed to initialize: " << ex.what() << std::endl;
    _exit(EXIT_FAILURE);
  } catch (...) {
    syslog(LOG_ERR, "TServer failed to initialize: unknown error");
    std::cerr << "TServer failed to initialize: unknown error" << std::endl;
    _exit(EXIT_FAILURE);
  }
}

void TServer::Init() {
  DEBUG_LOG("TServer::Init start");
  try {

    /******** Object Pools ********/

    Disk::TDurableManager::InitMappingPool(Cmd.DurableMappingPoolSize);
    Disk::TDurableManager::InitMappingEntryPool(Cmd.DurableMappingEntryPoolSize);
    Disk::TDurableManager::InitDurableLayerPool(Cmd.DurableLayerPoolSize);
    Disk::TDurableManager::InitMemEntryPool(Cmd.DurableMemEntryPoolSize);

    Indy::TManager::InitMappingPool(Cmd.RepoMappingPoolSize);
    Indy::TManager::InitMappingEntryPool(Cmd.RepoMappingEntryPoolSize);
    Indy::TManager::InitDataLayerPool(Cmd.RepoDataLayerPoolSize);

    L1::TTransaction::InitTransactionMutationPool(Cmd.TransactionMutationPoolSize);
    L1::TTransaction::InitTransactionPool(Cmd.TransactionPoolSize);

    TUpdate::InitUpdatePool(Cmd.UpdatePoolSize);
    TUpdate::InitEntryPool(Cmd.UpdateEntryPoolSize);
    TUpdate::SetPoolReservePct(Cmd.MemoryReservePct);

    Disk::TBufBlock::Pool.Init(Cmd.DiskBufferBlockPoolSize);

    /******** End Object Pools ********/

    TFd starting_sock;
    if (Cmd.StartingState == "SOLO") {
      RepoState = Orly::Indy::TManager::Solo;
    } else if (Cmd.StartingState == "SLAVE") {
      RepoState = Orly::Indy::TManager::SyncSlave;
      starting_sock = TFd(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
      Connect(starting_sock, Cmd.AddressOfMaster);
      if (Cmd.ReplicationToken) {
        /* Present the replication token if the master asks for it (#710). Without a token,
           nothing is read or written here, as before. */
        const string master = AsStr(Cmd.AddressOfMaster);
        try {
          if (Orly::Auth::AnswerMaster(starting_sock, *Cmd.ReplicationToken)) {
            syslog(LOG_INFO, "replication: master %s accepted this slave's replication token", master.c_str());
          } else {
            syslog(LOG_WARNING, "replication: master %s did not ask for a replication token; joining without one",
                   master.c_str());
          }
        } catch (const exception &ex) {
          syslog(LOG_ERR, "replication: cannot join master %s: %s", master.c_str(), ex.what());
          throw;
        }
      }
    } else {
      throw runtime_error("Server must start as SOLO or SLAVE");
    }
    ScheduleRunnerHost(&WaitForSlaveRunner);
    auto slave_bind_cb = [this](const shared_ptr<function<void (const TFd &)>> &cb) {
      WaitForSlaveActionCb = cb;
      Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
      assert(frame);
      try {
        frame->Latch(&WaitForSlaveRunner, this, static_cast<Fiber::TRunnable::TFunc>(&TServer::WaitForSlave));
      } catch (...) {
        Fiber::TFrame::LocalFramePool->Free(Frame);
        throw;
      }
    };
    auto update_replication_notification_cb = [this](const Base::TUuid &session_id, const Base::TUuid &repo_id, const Base::TUuid &tracker_id) {
      auto session = DurableManager->Open<TSession>(session_id);
      if (session) {
        session->InsertNotification(Notification::TUpdateProgress::New(repo_id, tracker_id, Notification::TUpdateProgress::Replicated));
      }
    };
    auto on_replicate_index_id_cb = [this](
        const Base::TUuid &idx_id, const std::string &pkg_key, const Indy::TKey &val) {
      void *val_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      std::lock_guard<std::mutex> lock(IndexMapMutex);
      Sabot::Type::TAny::TWrapper val_type_wrapper(val.GetCore().GetType(val.GetArena(), val_type_alloc));

      auto ret = IndexByIndexId.emplace(
          TIndexType(string(pkg_key), TKey(Atom::TCore(&IndexMapArena, *val_type_wrapper), &IndexMapArena)), idx_id);
      bool is_new = ret.second;
      if (is_new) {
        assert(RepoManager);
        RepoManager->SaveIndexNamespaceMapping(idx_id, pkg_key);
        IndexIdSet.insert(idx_id);
        stringstream ss;
        ss << "Replicating index [" << idx_id << "] " << pkg_key << " <- ";
        val_type_wrapper->Accept(Sabot::TTypeDumper(ss));
        syslog(LOG_INFO, "%s\n", ss.str().c_str());
      }
    };
    auto for_each_index_cb = [this](const TManager::TIndexCb &cb) {
      std::lock_guard<std::mutex> lock(IndexMapMutex);
      for (const auto &iter : IndexByIndexId) {
        cb(iter.second, iter.first.GetPackageKey(), iter.first.GetVal());
      }
    };

    Disk::Util::TEngine *engine_ptr = nullptr;
    if (Cmd.MemorySim) {
      SimMemEngine = std::make_unique<Disk::Sim::TMemEngine>(
          Scheduler,
          Cmd.MemorySimMB /* simulated space */,
          Cmd.MemorySimSlowMB /* simulated slow volume space */,
          (Cmd.PageCacheSizeMB * 1024UL * 1024UL) / 4096 /* page cache slots: 1GB */,
          1 /* num page lru */,
          (Cmd.BlockCacheSizeMB * 1024UL * 1024UL) / (4096 * 16) /* block cache slots: 1GB */,
          1 /* num block lru */);
      engine_ptr = SimMemEngine->GetEngine();
    } else {
      DiskEngine = make_unique<Disk::Util::TDiskEngine>(
          Scheduler,
          RunnerCons,
          FramePoolManager.get(),
          Cmd.DiskControllerCoreVec,
          Cmd.InstanceName,
          Cmd.DiscardOnCreate,
          Cmd.DoFsync,
          (Cmd.PageCacheSizeMB * 1024UL * 1024UL) / 4096 /* page cache slots: 1GB */,
          8 /* num page lru */,
          (Cmd.BlockCacheSizeMB * 1024UL * 1024UL) / (4096 * 16) /* block cache slots: 1GB */,
          8 /* num block lru */,
          Cmd.FileServiceAppendLogMB,
          Cmd.Create,
          Cmd.NoRealtime);
      engine_ptr = DiskEngine->GetEngine();
    }
    assert(engine_ptr);

    syslog(LOG_INFO, "Cmd.DiscardOnCreate = %s", Cmd.DiscardOnCreate ? "true" : "false");
    size_t block_slots_available_per_merger = (((Cmd.BlockCacheSizeMB * 1024) / Disk::Util::PhysicalBlockSize) * 0.8) / Cmd.NumDiskMergeThreads;
    auto for_each_scheduler_cb = [this](const std::function<bool (Fiber::TRunner *)> &cb) {
      for (auto &runner_ptr : FastRunnerVec) {
        if (!cb(runner_ptr.get())) {
          return;
        }
      }
      if (!cb(&BGFastRunner)) {
        return;
      }
      for (Indy::Fiber::TRunner *runner : ForEachSchedCallbackExtraSet) {
        if (!cb(runner)) {
          return;
        }
      }
      /* until slow schedulers yield fairly, this is a bad idea...
      for (auto &runner_ptr : SlowRunnerVec) {
        if (!cb(runner_ptr.get())) {
          return;
        }
      }
      */
    };
    RepoManager = make_unique<Orly::Indy::TManager>(engine_ptr,
                                                    Cmd.ReplicationSyncBufMB,
                                                    chrono::milliseconds(Cmd.MergeMemInterval),
                                                    chrono::milliseconds(Cmd.MergeDiskInterval),
                                                    chrono::milliseconds(Cmd.LayerCleaningInterval),
                                                    chrono::milliseconds(Cmd.ReplicationInterval),
                                                    RepoState,
                                                    Cmd.AllowTailing,
                                                    Cmd.PruneMergeHistory,
                                                    Cmd.AllowFileSync,
                                                    Cmd.NoRealtime,
                                                    std::move(starting_sock),
                                                    slave_bind_cb,
                                                    bind(&TServer::StateChangeCb, this, placeholders::_1),
                                                    update_replication_notification_cb,
                                                    on_replicate_index_id_cb,
                                                    for_each_index_cb,
                                                    for_each_scheduler_cb,
                                                    Scheduler,
                                                    &BGFastRunner,
                                                    block_slots_available_per_merger,
                                                    Cmd.MaxRepoCacheSize,
                                                    Cmd.TempFileConsolidationThreshold,
                                                    Cmd.MemMergeCoreVec,
                                                    Cmd.DiskMergeCoreVec,
                                                    Cmd.Create);
    auto global_ttl = TTtl::max();
    GlobalRepo = RepoManager->GetRepo(TSession::GlobalPovId,
                                      global_ttl,
                                      std::nullopt,
                                      true,
                                      Cmd.Create);

    void *key_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    void *val_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());

    /* figure out what index ids we currently support */ {
      std::unordered_map<Base::TUuid, std::string> pkg_key_mapping;
      Indy::Fiber::TJumpRunnable idns_jumper([this, &pkg_key_mapping] {
          pkg_key_mapping = RepoManager->GetIndexNamespaceMapping();
          /* No repo but the global one survives a restart, so neither should their saved-repo
             entries in the system repo (#671). */
          if (!Cmd.Create) {
            RepoManager->QueueSavedReposLeftByRestart();
          }
      });
      idns_jumper(FramePoolManager.get(), &BGFastRunner);
      //auto pkg_key_mapping = RepoManager->GetIndexNamespaceMapping();
      std::lock_guard<std::mutex> lock(IndexMapMutex);
      engine_ptr->ForEachFile([engine_ptr, this, key_type_alloc, val_type_alloc, &pkg_key_mapping](const Base::TUuid &file_uid,
                                                                                                   const Indy::Disk::TFileObj &file_obj) {
        if (file_uid != Indy::TManager::SystemRepoId) {
          switch (file_obj.Kind) {
            case Indy::Disk::TFileObj::TKind::DataFile: {
              TIndexIdReader reader(engine_ptr, file_uid, Indy::Disk::RealTime, file_obj.GenId, file_obj.StartingBlockId, file_obj.StartingBlockOffset, file_obj.FileSize);
              auto main_arena = make_unique<TIndexIdReader::TArena>(&reader, engine_ptr->GetCache<TIndexIdReader::PhysicalCachePageSize>(), Orly::Indy::Disk::RealTime);
              const auto &index_map = reader.GetIndexByIdMap();
              for (const auto &idx_pair : index_map) {
                const auto &idx_file = idx_pair.second;
                auto index_arena = make_unique<TIndexIdReader::TArena>(idx_file.get(), engine_ptr->GetCache<TIndexIdReader::PhysicalCachePageSize>(), Orly::Indy::Disk::RealTime);
                TIndexIdReader::TIndexFile::TKeyCursor csr(idx_file.get());
                if (csr) {
                  TKey key((*csr).Key, index_arena.get());
                  TKey val((*csr).Value, main_arena.get());

                  //const string &pkg_key = Sabot::AsNative<string>(*Sabot::State::TAny::TWrapper(key.GetState(key_type_alloc)));
                  auto pkg_key_pos = pkg_key_mapping.find(idx_pair.first);
                  if (pkg_key_pos == pkg_key_mapping.end()) {
                    stringstream ss;
                    ss << idx_pair.first;
                    syslog(LOG_ERR, "Could not find package namespace for index id [%s]\n", ss.str().c_str());
                    abort();
                  }
                  const string &pkg_key = pkg_key_pos->second;

                  Sabot::Type::TAny::TWrapper key_type_wrapper(key.GetCore().GetType(index_arena.get(), key_type_alloc));
                  Sabot::Type::TAny::TWrapper val_type_wrapper(val.GetCore().GetType(main_arena.get(), val_type_alloc));

                  auto ret = IndexByIndexId.emplace(
                      TIndexType(string(pkg_key), TKey(Atom::TCore(&IndexMapArena, *val_type_wrapper), &IndexMapArena)),
                      idx_pair.first);
                  bool is_new = ret.second;
                  if (is_new) {
                    IndexIdSet.insert(idx_pair.first);
                    stringstream ss;
                    ss << "Loading index [" << idx_pair.first << "] " << pkg_key << " <- ";
                    val_type_wrapper->Accept(Sabot::TTypeDumper(ss));
                    syslog(LOG_INFO, "%s\n", ss.str().c_str());
                  }
                }
              }
              break;
            }
            case Indy::Disk::TFileObj::TKind::DurableFile: {
              break;
            }
          }
        }
        return true;
      });
    }

    /* Check the reopened store's block accounting and sequence ranges (#700), now that the
       system and global repos have reloaded and dropped any merge leftovers, and before
       anything writes. Every build runs it; see <orly/indy/disk/open_check.h>. A leak or an
       overlap is logged; a block a live file owns that is free, or owned twice, refuses the
       open, since the next file allocated could be written over it. A mem-sim store is always
       new, so it has nothing to check. */
    if (!Cmd.Create && !Cmd.MemorySim && Cmd.OpenCheck) {
      Indy::Disk::TOpenCheck check;
      std::exception_ptr check_error;
      Indy::Fiber::TJumpRunnable check_jumper([this, &check, &check_error] {
        try {
          check = DiskEngine->CheckOpenConsistency();
        } catch (...) {
          check_error = std::current_exception();
        }
      });
      check_jumper(FramePoolManager.get(), &BGFastRunner);
      if (check_error) {
        std::rethrow_exception(check_error);
      }
      Indy::Disk::ReportOpenCheck(check);
    }

    /* Reinstall the packages recorded as installed when this image was last
       running (#435).  The .so files must still be in the package directory;
       a missing or broken package is logged and skipped rather than keeping
       the whole server down.  Runs on the BG runner because the walker needs
       the read-file cache and the installs run repo transactions. */
    if (!Cmd.Create) {
      Indy::Fiber::TJumpRunnable reinstall_jumper([this] {
        for (const auto &pkg : RepoManager->GetInstalledPackages()) {
          try {
            InstallPackage(pkg.first, pkg.second);
          } catch (const std::exception &ex) {
            ostringstream names;
            for (const auto &name : pkg.first) {
              names << '[' << name << ']';
            }
            syslog(LOG_ERR, "could not reinstall package [%s] version [%ld] from the previous run: %s",
                   names.str().c_str(), pkg.second, ex.what());
          }
        }
      });
      reinstall_jumper(FramePoolManager.get(), &BGFastRunner);
    }

    DurableManager = make_shared<Orly::Indy::Disk::TDurableManager>(Scheduler,
                                                                    RunnerCons,
                                                                    FramePoolManager.get(),
                                                                    RepoManager.get(),
                                                                    RepoManager->GetEngine(),
                                                                    Cmd.DurableCacheSize,
                                                                    chrono::milliseconds(Cmd.DurableWriteInterval),
                                                                    chrono::milliseconds(Cmd.DurableMergeInterval),
                                                                    chrono::milliseconds(Cmd.LayerCleaningInterval),
                                                                    Cmd.TempFileConsolidationThreshold,
                                                                    Cmd.Create);
    RepoManager->SetDurableManager(DurableManager);
    /* Remove Durable TDurableLayer(s) that are no longer relevant */ {
      ScheduleRunnerHost(&DurableLayerCleanerRunner);
      Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
      try {
        frame->Latch(&DurableLayerCleanerRunner, DurableManager.get(), static_cast<Fiber::TRunnable::TFunc>(&Durable::TManager::RunLayerCleaner));
      } catch (...) {
        Fiber::TFrame::LocalFramePool->Free(frame);
        throw;
      }
      //Scheduler->Schedule(bind(&Durable::TManager::RunLayerCleaner, DurableManager.get()));
    }

    auto tetris_runner_setup_cb = [this](Indy::Fiber::TRunner *runner) {
      ForEachSchedCallbackExtraSet.insert(runner);
      using TLocalReadFileCache = Orly::Indy::Disk::TLocalReadFileCache<Orly::Indy::Disk::Util::LogicalPageSize,
                                                                        Orly::Indy::Disk::Util::LogicalBlockSize,
                                                                        Orly::Indy::Disk::Util::PhysicalBlockSize,
                                                                        Orly::Indy::Disk::Util::CheckedPage, true>;
      if (!Cmd.MemorySim) {
        /* if this is a disk based engine, allocate event pools */
        if (!Disk::Util::TDiskController::TEvent::LocalEventPool) {
          Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
        }
      }
      assert(!TLocalReadFileCache::Cache);
      TLocalReadFileCache::Cache = new TLocalReadFileCache();
      assert(!Disk::TLocalWalkerCache::Cache);
      Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
    };

    TetrisManager = new TRepoTetrisManager(Scheduler, RunnerCons, FramePoolManager.get(), tetris_runner_setup_cb, (RepoState == Orly::Indy::TManager::Solo), RepoManager.get(), &PackageManager, DurableManager.get(), Cmd.LogAssertionFailures, Cmd.TetrisCommutativeFastlane);
    RepoManager->SetTetrisManager(TetrisManager);
    /* schedule everything the repo manager needs */ {
      /* Read() from master / slave */ {
        ScheduleRunnerHost(&RunReplicationQueueRunner);
        Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
        try {
          frame->Latch(&RunReplicationQueueRunner, static_cast<Orly::Indy::L0::TManager *>(RepoManager.get()), static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::L0::TManager::RunReplicationQueue));
        } catch (...) {
          Fiber::TFrame::LocalFramePool->Free(frame);
          throw;
        }
        //Scheduler->Schedule(bind(&Orly::Indy::TManager::RunReplicationQueue, RepoManager.get()));
      }
      /* Execute Job from master / slave */ {
        ScheduleRunnerHost(&RunReplicationWorkRunner);
        Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
        try {
          frame->Latch(&RunReplicationWorkRunner, static_cast<Orly::Indy::L0::TManager *>(RepoManager.get()), static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::L0::TManager::RunReplicationWork));
        } catch (...) {
          Fiber::TFrame::LocalFramePool->Free(frame);
          throw;
        }
        //Scheduler->Schedule(bind(&Orly::Indy::TManager::RunReplicationWork, RepoManager.get()));
      }
      /* Dequeue from replication queue and transmit to slave if necessary */ {
        ScheduleRunnerHost(&RunReplicateTransactionRunner);
        Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
        try {
          frame->Latch(&RunReplicateTransactionRunner, static_cast<Orly::Indy::L0::TManager *>(RepoManager.get()), static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::L0::TManager::RunReplicateTransaction));
        } catch (...) {
          Fiber::TFrame::LocalFramePool->Free(frame);
          throw;
        }
        //Scheduler->Schedule(bind(&Orly::Indy::TManager::RunReplicateTransaction, RepoManager.get()));
      }
      /* Remove Repo TDataLayer(s) that are no longer relevant */ {
        ScheduleRunnerHost(&RepoLayerCleanerRunner);
        Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
        try {
          frame->Latch(&RepoLayerCleanerRunner, static_cast<Orly::Indy::L0::TManager *>(RepoManager.get()), static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::L0::TManager::RunLayerCleaner));
        } catch (...) {
          Fiber::TFrame::LocalFramePool->Free(frame);
          throw;
        }
        //Scheduler->Schedule(bind(&Orly::Indy::L0::TManager::RunLayerCleaner, RepoManager.get()));
      }
      /* Merge memory layers in a repo. */
      for (size_t i = 0; i < Cmd.NumMemMergeThreads; ++i) {
        MergeMemRunnerVec.emplace_back(new Fiber::TRunner(RunnerCons));
        Fiber::TRunner *cur_runner = MergeMemRunnerVec.back().get();
        ScheduleRunnerHost(cur_runner);
        Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
        try {
          frame->Latch(cur_runner, static_cast<Orly::Indy::L0::TManager *>(RepoManager.get()), static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::L0::TManager::RunMergeMem));
        } catch (...) {
          Fiber::TFrame::LocalFramePool->Free(frame);
          throw;
        }
        MergeMemFrameVec.emplace_back(frame);
        //Scheduler->Schedule(bind(&Orly::Indy::L0::TManager::RunMergeMem, RepoManager.get()));
      }
      /* Merge multiple disk files of a specific size category, in the same safe repo. */
      for (size_t i = 0; i < Cmd.NumDiskMergeThreads; ++i) {
        MergeDiskRunnerVec.emplace_back(new Fiber::TRunner(RunnerCons));
        Fiber::TRunner *cur_runner = MergeDiskRunnerVec.back().get();
        ScheduleRunnerHost(cur_runner);
        Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
        try {
          frame->Latch(cur_runner, static_cast<Orly::Indy::L0::TManager *>(RepoManager.get()), static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::L0::TManager::RunMergeDisk));
        } catch (...) {
          Fiber::TFrame::LocalFramePool->Free(frame);
          throw;
        }
        MergeDiskFrameVec.emplace_back(frame);
        //Scheduler->Schedule(bind(&Orly::Indy::L0::TManager::RunMergeDisk, RepoManager.get(), block_slots_available_per_merger));
      }

    }

    /* Open the listening socket for clients. A bind failure is logged and
       aborts startup. */
    auto open_listening_socket = [this](TFd &sock, in_port_t port, const char *who) {
      TAddress address = MakeBindAddress(Cmd.BindAddress, port);
      sock = TFd(socket(address.GetFamily(), SOCK_STREAM, 0));
      int flag = true;
      IfLt0(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)));
      try {
        Bind(sock, address);
      } catch (const std::exception &ex) {
        syslog(LOG_ERR, "Server startup caught exception [%s], cannot bind %s socket to port [%d]", ex.what(), who, port);
        throw;
      }
      IfLt0(listen(sock, Cmd.ConnectionBacklog));
      syslog(LOG_INFO, "%s listener bound to %s%s", who, DescribeBound(sock).c_str(),
             Cmd.AuthToken ? "; token required" : "");
    };

    open_listening_socket(MainSocket, Cmd.PortNumber, "client");
    /* Tracked for cancel-or-join at Shutdown() like the runner hosts: the
       loop exits promptly once Shutdown() shuts the socket down, but only
       the join proves it is out of TServer members (#462, #463). */
    ScheduleHostJob(bind(&TServer::AcceptClientConnections, this));

    HousekeeperHandle = Scheduler->ScheduleCancelable(bind(&TServer::CleanHouse, this));
    Reporter = make_unique<TIndyReporter>(this, Scheduler, Cmd.BindAddress, Cmd.ReportingPortNumber);
    /* Sets the data floor before the first write does (#590). */
    if (Cmd.DiskReserveMb || Cmd.DiskReservePct) {
      RefreshWriteAdmission(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    DEBUG_LOG("TServer::Init end");
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "TServer::Init() caught exception [%s]", ex.what());
    /* Hand the failure to the constructor; swallowing it here would leave a
       half-built server accepting connections it cannot serve (#435). */
    InitError = std::current_exception();
  }
  std::lock_guard<std::mutex> lock(InitMutex);
  InitFinished = true;
  InitCond.notify_all();
}

void TServer::ScheduleRunnerHost(Fiber::TRunner *runner) {
  assert(runner);
  ScheduleHostJob([this, runner] {
    Fiber::LaunchSlowFiberSched(runner, FramePoolManager.get());
  });
}

void TServer::ScheduleHostJob(Base::TScheduler::TJob &&host) {
  auto handle = Scheduler->ScheduleCancelable([this, host = std::move(host)] {
    Base::TPushOnExit exit_latch(RunnerHostExited);
    host();
  });
  /* A refused job (scheduler already shut down) never runs and never
     pushes the latch; keeping its null handle would deadlock the reap. */
  if (handle) {
    RunnerHostHandles.push_back(std::move(handle));
  }
}

void TServer::Shutdown() {
  if (ShutdownCalled) {
    return;
  }
  ShutdownCalled = true;
  assert(!Fiber::TRunner::LocalRunner);
  syslog(LOG_INFO, "TServer::Shutdown() begin");
  /* Stop the websockets server first: no new work arrives. */
  Ws.reset();
  /* Cut off the work sources next: wake the accept loops (blocked in
     accept) so no new clients, slaves, or report requests arrive while we
     drain (#440). */
  shutdown(MainSocket, SHUT_RDWR);
  /* under SlaveSocketLock: WaitForSlave() publishes SlaveSocket under the
     same lock, so we either shut down the fd it is (or is about to be)
     accepting on, or it sees ShutdownCalled before blocking (#440). */ {
    std::lock_guard<std::mutex> lock(SlaveSocketLock);
    shutdown(SlaveSocket, SHUT_RDWR);
  }
  if (Reporter) {
    Reporter->Stop();
  }
  /* Hard-close every established client connection: the listening sockets
     are already down and TConnection::New refuses under ShutdownCalled, so
     after this kick no serving loop can outlive the drain wait below.  The
     loops wind down during that wait; in-flight requests finish against the
     still-live pipeline and their responses just fail to send (#460). */ {
    std::vector<std::shared_ptr<TConnection>> live_connections;
    /* acquire Connection lock */ {
      std::lock_guard<std::mutex> lock(ConnectionMutex);
      live_connections.reserve(ConnectionBySessionId.size());
      for (const auto &item : ConnectionBySessionId) {
        if (auto connection = item.second.lock()) {
          live_connections.push_back(std::move(connection));
        }
      }
    }  // release Connection lock
    if (!live_connections.empty()) {
      syslog(LOG_INFO, "TServer::Shutdown() draining [%zu] live client connections (#460)", live_connections.size());
    }
    for (const auto &connection : live_connections) {
      connection->InterruptRun();
    }
  }  // our refs drop here, OUTSIDE the lock: if one is the last ref, its
     // OnRelease takes ConnectionMutex itself.
  const auto shutdown_start = std::chrono::steady_clock::now();
  auto ms_since_start = [&shutdown_start] {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shutdown_start).count());
  };
  /* Wait for the kicked connections to actually release their sessions:
     no write can commit after this, and the durable Clear() in the teardown
     jumper below must see only genuinely closed durables.  Bounded -- a
     serving loop wedged in a long request must not hold shutdown hostage
     (cf. #461); a straggler just means Clear() logs-and-leaks it, exactly as
     before the drain existed (#460). */ {
    std::unique_lock<std::mutex> lock(ConnectionMutex);
    if (!ConnectionDrainedCv.wait_for(lock, std::chrono::seconds(10),
                                      [this] { return LiveConnectionCount == 0; })) {
      syslog(LOG_WARNING, "TServer::Shutdown(): [%zu] client connection(s) still live after the drain deadline (#460)", LiveConnectionCount);
    }
  }
  /* Let Tetris promote everything the povs hold, all the way up to the
     global pov, while the merges, the layer cleaners and the replication
     release that makes promoted updates mergeable all still run (#744).  An
     acknowledged write is only committed to its pov; under write load
     Tetris runs well behind, and whatever it promoted after the flush below
     stayed in the global pov's memory layer and was dropped at exit.
     Bounded, like the flush: a pov whose promotion can't make progress must
     not hold the stop hostage.  Paused povs are not waited for. */
  if (TetrisManager) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    size_t players = 0UL;
    while ((players = TetrisManager->GetUnpausedPlayerCount()) > 0UL && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (players) {
      syslog(LOG_ERR, "TServer::Shutdown(): Tetris still promoting into [%zu] pov(s) after 30s; their unpromoted updates will be lost (#744)", players);
    } else {
      syslog(LOG_INFO, "TServer::Shutdown(): Tetris idle after %ldms (#744)", ms_since_start());
    }
  }
  /* Let the in-flight update pipeline settle while ALL of the cadence
     machinery (replication release, mergers, tetris) is still running: a
     write that arrived just before the signal is not mergeable until the
     release machinery (which runs on the replication cadence even in SOLO)
     has settled it.  This must precede the stop calls below -- a stopped
     replication loop can no longer drain its queue.  Bounded and
     cadence-derived, not a magic number: three periods of the slowest
     relevant interval. */
  std::this_thread::sleep_for(std::chrono::milliseconds(
      3 * std::max<size_t>({Cmd.ReplicationInterval, Cmd.MergeMemInterval, Cmd.DurableWriteInterval, 100})));
  /* Now stop the standalone service loops -- the housekeeper (blocked on
     its timer) and the three replication loops (blocked in epoll_wait) --
     and WAIT for each to actually return:
     a stop is only a flag plus a wake, and the manager teardown below
     would otherwise free the very fds/collections a still-parked loop
     references (#440).  The fiber-level joins reap only loops that
     actually entered; that skip is sound because every runner HOST job is
     cancel-or-joined (here for the housekeeper, at the end of Shutdown()
     for the rest) -- a fiber latched onto a runner whose host was
     cancelled can never run at all (#462).
     NOT the two layer cleaners (#744): the flush below, Tetris's promotions
     and the merges all make dead layers, and the cleaners are what give
     their pool blocks back.  Stopped here, before the flush, under write load
     the repo pools ran dry (merges and Tetris rounds rolled back and retried
     for the whole 30 s flush budget, which then gave up, losing acknowledged
     writes) and so did the Durable Layer pool (its bad_alloc killed the
     durable writer fiber, and every saver waiting on it hung the stop).
     Each is stopped below once nothing makes dead layers for it. */
  HousecleaningTimer.FireNow();
  RepoManager->StopReplicationServices();
  RepoManager->JoinReplicationServices();
  /* A null handle means the job was never accepted (or Init never got that
     far) -- there is nothing to join. */
  if (HousekeeperHandle && !Scheduler->Cancel(HousekeeperHandle)) {
    HousekeeperExited.Pop();
  }
  /* Stop the merge loops first: the flush below must be the only drainer
     of the merge queue, or a live merger mid-step could re-enqueue a
     still-dirty repo after the flush saw an empty queue and returned, and
     that repo would never reach disk (#440).  On this thread, not on a
     fiber (#744): the wait blocks its thread, and a disk merge mid-step
     visits every fast runner (MergeFiles removes its intermediate file
     after a fold, and RemoveFile clears each runner's caches), so waiting
     on a fast runner's fiber, as this used to, left that merge unable to
     finish and the stop hung for good.  The wake sems can be pushed from
     any thread. */
  RepoManager->StopMergeRunners();
  syslog(LOG_INFO, "TServer::Shutdown(): merge runners stopped after %ldms (#744)", ms_since_start());
  /* Flush on a fiber, while every runner is still alive. */
  Indy::Fiber::TJumpRunnable flush_jumper([this] {
    /* Flush-on-shutdown (#440): merge every dirty repo memory layer out to
       disk and write the durable slush layer, so a graceful stop loses
       nothing -- durability no longer depends on the merge cadence having
       happened to run.  Tetris is still alive here: the newest updates only
       become mergeable once promotion has settled them, so the flush must
       precede StopAllPlayers. */
    RepoManager->FlushMemMerges();
    DurableManager->Flush();
  });
  flush_jumper(FramePoolManager.get(), FastRunnerVec[0].get());
  syslog(LOG_INFO, "TServer::Shutdown(): flushed after %ldms (#744)", ms_since_start());
  /* The merge runners are stopped and the flush is done, so nothing makes dead repo layers any
     more: stop the repo layer cleaner, before Tetris goes (a disk layer's removal visits the
     Tetris runners, #648).  Join it here, on this thread and not on a fiber: that removal also
     hops onto every fast runner, so a fiber blocked in the join would hold up the very runner
     the cleaner may be waiting to visit (#744). */
  RepoManager->StopLayerCleaner();
  RepoManager->JoinLayerCleaner();
  /* Tear down the fiber-entangled managers on a fiber, while every runner
     is still alive: ~TDurableManager blocks on TSingleSem (fiber-only) for
     its writer/merger fibers, and ~TRepoTetrisManager's StopAllPlayers
     takes fiber locks. */
  Indy::Fiber::TJumpRunnable teardown_jumper([this] {
    delete TetrisManager;
    TetrisManager = nullptr;
    /* Forget the Tetris runners: they are gone, but RepoManager.reset() below can still remove a
       disk layer's file, and TSafeRepo::RemoveFile visits every runner ForEachScheduler names. A
       fiber that switched to a dead runner never ran again, and shutdown hung forever (#648). */
    ForEachSchedCallbackExtraSet.clear();
    DurableManager->Clear();
    /* Nothing saves durables any more (Tetris is gone, the connections are drained, the
       flush is done), so the durable layer cleaner can stop: join it here, while its runner
       is alive, rather than wherever the manager's last reference happens to go (#744).
       Blocking this runner's thread is safe: the cleaner never hops onto it. */
    DurableManager->StopLayerCleaner();
    DurableManager->JoinLayerCleaner();
    DurableManager.reset();
    GlobalRepo.Reset();
    RepoManager.reset();
  });
  teardown_jumper(FramePoolManager.get(), FastRunnerVec[0].get());
  /* Stop the engine-level services that would otherwise wedge the
     scheduler's teardown: the background runners this server hosts on
     scheduler jobs (replication, layer cleaners, mergers, slave wait) and
     the disk controller's polling loop. */
  WaitForSlaveRunner.ShutDown();
  RunReplicationQueueRunner.ShutDown();
  RunReplicationWorkRunner.ShutDown();
  RunReplicateTransactionRunner.ShutDown();
  RepoLayerCleanerRunner.ShutDown();
  for (auto &runner : MergeMemRunnerVec) {
    runner->ShutDown();
  }
  for (auto &runner : MergeDiskRunnerVec) {
    runner->ShutDown();
  }
  if (DiskEngine) {
    /* The file service's loop runs on a job of the scheduler that RunUntilCtrlC destroys before
       ~TServer runs, so stop and join it here, not in ~TDiskEngine. Left running, the
       scheduler's own teardown interrupted it (EINTR out of the runner) and gave up after ten
       seconds, and ~TFileService then locked the destroyed scheduler's mutex and aborted the
       process (#648). The managers that queue file ops are gone (the teardown jumper above),
       and the disk controller still runs, so a base-image write in flight can finish. */
    DiskEngine->ShutDownFileService();
    DiskEngine->GetController()->ShutDown();
  }
  /* Now nothing needs the runners; stop and join them. */
  WsRunner.ShutDown();
  WsThread.join();
  DurableLayerCleanerRunner.ShutDown();
  BGFastRunner.ShutDown();
  for (auto &runner : FastRunnerVec) {
    runner->ShutDown();
  }
  for (auto &t : FastRunnerThreadVec) {
    t->join();
  }
  for (auto &runner : SlowRunnerVec) {
    runner->ShutDown();
  }
  for (auto &t : SlowRunnerThreadVec) {
    t->join();
  }
  /* Cancel-or-join every host job (the runner-loop hosts, the BG fast
     runner, the accept loop): a ShutDown() is only a flag, and a loop
     still between its last KeepRunning check and its return would race
     the objects' destruction in ~TServer (#463).  A job whose worker
     already took it has run or is running and pushes the exited latch; a
     job that never got a worker is cancelled, so it can neither be waited
     for nor start late against dying state (#462). */
  for (const auto &handle : RunnerHostHandles) {
    if (!Scheduler->Cancel(handle)) {
      RunnerHostExited.Pop();
    }
  }
  RunnerHostHandles.clear();
  syslog(LOG_INFO, "TServer::Shutdown() complete");
}

TServer::~TServer() {
  if (ShutdownCalled) {
    /* Shutdown() already stopped the threads and destroyed the managers;
       free the init frame, then intentionally release (not destroy) the
       frame-pool manager: pools created on scheduler-worker threads (accept
       loops, ws jumps) cannot be reclaimed without thread-exit hooks, and
       the manager's destructor correctly refuses to die while they exist.
       A few slabs leak; every real resource is gone (#440). */
    Fiber::TFrame::LocalFramePool->Free(Frame);
    delete Fiber::TFrame::LocalFramePool;
    Fiber::TFrame::LocalFramePool = nullptr;
    FramePoolManager.release();
    /* The static disk-event pool manager must be released for the same
       reason: every TJumpRunnable (including Shutdown()'s own teardown
       jumper) makes a LocalEventPool on its scheduler-worker thread, and
       the merge and durable runners make their own.  Left owned, the
       unique_ptr's destructor runs in __run_exit_handlers, finds those
       pools, and throws out of a noexcept destructor -- aborting a process
       whose shutdown already completed cleanly (#522). */
    Disk::Util::TDiskController::TEvent::DiskEventPoolManager.release();
    return;
  }
  WsRunner.ShutDown();
  WsThread.join();
  delete TetrisManager;
  ForEachSchedCallbackExtraSet.clear();  // as in Shutdown() (#648)
  DurableManager->Clear();
  DurableManager.reset();
  GlobalRepo.Reset();
  RepoManager.reset();
  Fiber::TFrame::LocalFramePool->Free(Frame);
}

TWs::TSessionPin *TServer::NewSession() {
  assert(DurableManager);
  return new TSessionPin(this);
}

TWs::TSessionPin *TServer::ResumeSession(const TUuid &id) {
  assert(DurableManager);
  return new TSessionPin(this, id);
}

size_t TServer::GetDiskReserve(size_t total_bytes) const {
  if (Cmd.DiskReserveMb) {
    return Cmd.DiskReserveMb << 20;
  }
  /* A percentage alone leaves a small disk next to nothing for session state and the file map,
     so take at least 64 MiB, or a quarter of a disk smaller than 256 MiB. */
  const size_t min_reserve = Cmd.DiskReservePct ? std::min<size_t>(64UL << 20, total_bytes / 4UL) : 0UL;
  return std::max(min_reserve, total_bytes / 100UL * Cmd.DiskReservePct);
}

void TServer::RefreshWriteAdmission(int64_t now_ns) {
  auto *vol_man = RepoManager->GetEngine()->GetVolMan();
  const Disk::Util::TSpace space = vol_man->GetSpace();
  const size_t reserve = GetDiskReserve(space.Total);
  /* Below half the reserve, merges and flushes stop too; the rest is the durable store's and
     the file map's, so sessions keep working on a disk the data has filled. */
  vol_man->SetDataFloor(reserve / 2UL);
  /* A merge's claim covers what it will still allocate. Use the recent peak, not just what is
     claimed this instant: a merge that can't fit fails and retries, so its claim blinks. */
  const size_t needed = reserve + vol_man->GetRecentPeakClaims();
  const size_t available = space.GetAvailable();
  AdmissionAvailable = available;
  AdmissionNeeded = needed;
  AdmissionTotal = space.Total;
  const bool was_refusing = RefusingWrites;
  /* Hysteresis: once refusing, wait for a quarter of the reserve more than the threshold, so
     writes don't flap on and off at the edge. */
  const bool refuse = was_refusing ? available < needed + reserve / 4UL : available < needed;
  if (refuse != was_refusing) {
    RefusingWrites = refuse;
    /* LOG_ERR, not WARNING: the default mask drops warnings. */
    syslog(LOG_ERR, "write admission: %s writes; %ld MiB available of %ld MiB, threshold %ld MiB (reserve %ld + merge claims %ld); %ld refused so far",
           refuse ? "refusing" : "accepting", available >> 20, space.Total >> 20, needed >> 20, reserve >> 20, (needed - reserve) >> 20,
           RefusedWriteCount.load());
  }
  AdmissionCheckedAtNs = now_ns;
}

void TServer::CheckWriteAdmission() {
  if (!Cmd.DiskReserveMb && !Cmd.DiskReservePct) {
    return;
  }
  /* Short enough that a fill-up is caught within a few writes, long enough that the volume
     locks are not taken per write. */
  constexpr int64_t refresh_interval_ns = 10'000'000;
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t checked_at = AdmissionCheckedAtNs.load(std::memory_order_relaxed);
  if (now_ns - checked_at >= refresh_interval_ns
      && AdmissionCheckedAtNs.compare_exchange_strong(checked_at, now_ns)) {
    RefreshWriteAdmission(now_ns);
  }
  if (RefusingWrites.load(std::memory_order_relaxed)) {
    ++RefusedWriteCount;
    std::ostringstream msg;
    msg << "insufficient storage: write refused; " << (AdmissionAvailable.load() >> 20) << " MiB of "
        << (AdmissionTotal.load() >> 20) << " MiB disk available, below the " << (AdmissionNeeded.load() >> 20)
        << " MiB reserved for merges and server state; reads still work";
    throw TInsufficientStorage(msg.str());
  }
}

void TServer::CountGlobalLayers(size_t &disk_layers, size_t &mem_layers) const {
  disk_layers = 0UL;
  mem_layers = 0UL;
  if (GlobalRepo) {
    const Indy::TRepo::TView view(GlobalRepo);
    view.CountLayers(disk_layers, mem_layers);
  }
}

void TServer::CheckMemoryAdmission(TUpdate::TWriteAdmission &admission, size_t num_entries) {
  if (!Cmd.MemoryReservePct) {
    return;
  }
  /* Promoting a write copies it twice (Tetris's peek and the parent's copy), and only the
     reserve is sure to be free for that, so a write bigger than half the reserve might never be
     promoted (#607). It can't succeed by retrying, so it isn't an insufficient_memory refusal. */
  const size_t entry_reserve = TUpdate::GetEntryPool().GetReserve();
  if (num_entries * 2UL > entry_reserve) {
    std::ostringstream msg;
    msg << "write too large: its " << num_entries << " entries are more than half the " << entry_reserve
        << " Update Entry blocks kept for merges (--memory_reserve_pct); retrying won't help, split it into smaller batches";
    throw TWriteTooLarge(msg.str());
  }
  const bool admitted = admission.TryAcquire(num_entries);
  if (!admitted) {
    ++MemoryRefusedWriteCount;
  }
  /* Log each change of state once. The load keeps the steady state free of writes to a
     shared line. */
  if (RefusingWritesForMemory.load(std::memory_order_relaxed) == admitted
      && RefusingWritesForMemory.exchange(!admitted) == admitted) {
    /* LOG_ERR, not WARNING: the default mask drops warnings. */
    const auto &updates = TUpdate::GetUpdatePool(), &entries = TUpdate::GetEntryPool();
    syslog(LOG_ERR, "memory admission: %s writes; Update pool %ld / %ld, Update Entry pool %ld / %ld, reserves %ld / %ld; %ld refused so far%s",
           admitted ? "accepting" : "refusing", updates.GetNumBlocksUsed(), updates.GetMaxBlocks(),
           entries.GetNumBlocksUsed(), entries.GetMaxBlocks(), updates.GetReserve(), entries.GetReserve(),
           MemoryRefusedWriteCount.load(), admitted ? "" : ("; " + DescribeRefusal(admission)).c_str());
  }
  if (!admitted) {
    ThrowInsufficientMemory(&admission);
  }
}

std::string TServer::DescribeRefusal(const TUpdate::TWriteAdmission &admission) {
  /* What admission compared (#719): the blocks in use, those promised to writes in flight and
     those claimed by merge and Tetris copies, plus this write's, against what writes may use. */
  const auto &refusal = admission.GetRefusal();
  std::ostringstream msg;
  msg << "the " << (admission.WasEntryPoolRefused() ? "Update Entry" : "Update") << " pool had "
      << refusal.Used << " in use, " << refusal.Admitted << " promised to other writes and "
      << refusal.Claimed << " claimed for merge and Tetris copies, so this write's " << refusal.Asked
      << " would pass the " << refusal.Limit << " writes may use";
  return msg.str();
}

void TServer::RefuseWriteOutOfMemory() {
  if (!Cmd.MemoryReservePct) {
    return;
  }
  ++MemoryRefusedWriteCount;
  ThrowInsufficientMemory();
}

void TServer::ThrowInsufficientMemory(const TUpdate::TWriteAdmission *refused) const {
  const auto &updates = TUpdate::GetUpdatePool(), &entries = TUpdate::GetEntryPool();
  std::ostringstream msg;
  msg << "insufficient memory: write refused; the update pools are down to the reserve kept for merges (Update "
      << updates.GetNumBlocksUsed() << " / " << updates.GetMaxBlocks() << ", Update Entry "
      << entries.GetNumBlocksUsed() << " / " << entries.GetMaxBlocks() << " in use";
  if (refused) {
    msg << "; " << DescribeRefusal(*refused);
  }
  msg << "); reads still work";
  throw TInsufficientMemory(msg.str());
}

bool TServer::ForEachIndex(const std::function<
    bool(const std::string &pkg, const std::string &key_type, const std::string &val_type)> &cb) const {
  lock_guard<mutex> lock(IndexMapMutex);
  Atom::TSuprena temp_arena;
  for(const auto &idx: IndexByIndexId) {
    const string &pkg_key = idx.first.GetPackageKey();
    auto pkg_key_split = pkg_key.find(' ');
    string pkg = pkg_key.substr(0, pkg_key_split);

    void *val_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper val_type_wrapper(idx.first.GetVal().GetCore().GetType(&temp_arena, val_type_alloc));

    if (!cb(pkg_key.substr(0, pkg_key_split),
            pkg_key.substr(pkg_key_split + 1),
            AsStrFunc(Sabot::DumpType, *val_type_wrapper))) {
      return false;
    }
  }
  return true;
}

TServer::TSessionPin::TSessionPin(TServer *server) {
  assert(server);
  server->RunWs(Indy::Fiber::TJumpRunnable([this, server] {
      Conn = TConnection::New(
          server,
          server->DurableManager->New<TSession>(Base::TUuid::Twister, seconds(600)));
      if (Conn) {
        /* WebSocket has no way to deliver notifications (#591). */
        Conn->GetSession()->SetQueuesNotifications(false);
      }
  }));
  if (!Conn) {
    throw runtime_error("could not create session");
  }
}

TServer::TSessionPin::TSessionPin(TServer *server, const TUuid &id) {
  assert(server);
  server->RunWs(Indy::Fiber::TJumpRunnable([this, server, &id] {
      Conn = TConnection::New(server, server->DurableManager->Open<TSession>(id));
      if (Conn) {
        /* WebSocket has no way to deliver notifications (#591). */
        Conn->GetSession()->SetQueuesNotifications(false);
      }
  }));
  if (!Conn) {
    throw runtime_error("could not resume session");
  }
}

void TServer::TSessionPin::BeginImport() const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::BeginImport, Conn.get())));
}

void TServer::TSessionPin::EndImport() const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::EndImport, Conn.get())));
}

const Base::TUuid &TServer::TSessionPin::GetId() const {
  return Conn->GetSession()->GetId();
}

void TServer::TSessionPin::Import(const string &file_pattern,
                                  const string &pkg_name,
                                  int64_t num_load_threads,
                                  int64_t num_merge_threads,
                                  int64_t merge_simultaneous) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::ImportCoreVector,
                                              Conn.get(),
                                              cref(file_pattern),
                                              cref(pkg_name),
                                              num_load_threads,
                                              num_merge_threads,
                                              merge_simultaneous)));
}

void TServer::TSessionPin::InstallPackage(
    const std::vector<std::string> &name, uint64_t version) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::InstallPackage, Conn.get(), cref(name), version)));
}

Base::TUuid TServer::TSessionPin::NewPov(
    bool is_safe, bool is_shared, const std::optional<Base::TUuid> &parent_id) const {
  const seconds ttl(600);
  auto func = is_safe
      ? (is_shared ? &TConnection::NewSafeSharedPov : &TConnection::NewSafePrivatePov)
      : (is_shared ? &TConnection::NewFastSharedPov : &TConnection::NewFastPrivatePov);
  TUuid new_pov_id;
  Conn->RunWs(Indy::Fiber::TJumpRunnable(
      [this, &parent_id, &ttl, func, &new_pov_id] {
        new_pov_id = (Conn.get()->*func)(parent_id, ttl);
      }
  ));
  return new_pov_id;
}

void TServer::TSessionPin::PausePov(const Base::TUuid &pov_id) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::PausePov, Conn.get(), cref(pov_id))));
}

void TServer::TSessionPin::SetTtl(
    const Base::TUuid &durable_id, const std::chrono::seconds &ttl) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::SetTimeToLive, Conn.get(), cref(durable_id), cref(ttl))));
}

void TServer::TSessionPin::SetUserId(const Base::TUuid &user_id) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::SetUserId, Conn.get(), cref(user_id))));
}

void TServer::TSessionPin::Tail() const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::TailGlobalPov, Conn.get())));
}

TMethodResult TServer::TSessionPin::Try(const TMethodRequest &method_request) const {
  TMethodResult method_result;
  Conn->RunWs(Indy::Fiber::TJumpRunnable(
      [this, &method_request, &method_result] {
        method_result = Conn->Try(
            method_request.GetPovId(), method_request.GetPackage(), method_request.GetClosure());
      }
  ));
  return move(method_result);
}

TMethodResult TServer::TSessionPin::TryBatch(
    const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const std::vector<TClosure> &closures) const {
  TMethodResult method_result;
  Conn->RunWs(Indy::Fiber::TJumpRunnable(
      [this, &pov_id, &fq_name, &closures, &method_result] {
        method_result = Conn->TryBatch(pov_id, fq_name, closures);
      }
  ));
  return move(method_result);
}

std::vector<Var::TVar> TServer::TSessionPin::TryMulti(const Base::TUuid &pov_id, const std::vector<TBatchCall> &calls) const {
  std::vector<Var::TVar> results;
  Conn->RunWs(Indy::Fiber::TJumpRunnable(
      [this, &pov_id, &calls, &results] {
        results = Conn->TryMulti(pov_id, calls);
      }
  ));
  return results;
}

void TServer::TSessionPin::UninstallPackage(
    const std::vector<std::string> &name, uint64_t version) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::UninstallPackage, Conn.get(), cref(name), version)));
}

void TServer::TSessionPin::UnpausePov(const Base::TUuid &pov_id) const {
  Conn->RunWs(Indy::Fiber::TJumpRunnable(bind(&TConnection::UnpausePov, Conn.get(), cref(pov_id))));
}

void TServer::TConnection::Run(TFd &fd) {
  /* We use this visitor to push notifications. */
  class visitor_t : public Notification::Single::TComputer<void> {
    public:
    visitor_t(TConnection *connection, shared_ptr<TFuture<void>> &ack) : Connection(connection), Ack(ack) {}
    virtual void operator()(const Notification::TPovFailure &that) const override {
      Ack = Connection->Write<void>(ClientRpc::PovFailed, that.GetPovId());
    }
    virtual void operator()(const Notification::TSystemShutdown &/*that*/) const override {
      throw runtime_error("Not Implemented: TSystemShutdown notification.");
    }
    virtual void operator()(const Notification::TUpdateProgress &that) const override {
      switch (that.GetResponse()) {
        case Notification::TUpdateProgress::Accepted: {
          Ack = Connection->Write<void>(ClientRpc::UpdateAccepted, that.GetPovId(), that.GetUpdateId());
          break;
        }
        case Notification::TUpdateProgress::Replicated: {
          Ack = Connection->Write<void>(ClientRpc::UpdateReplicated, that.GetPovId(), that.GetUpdateId());
          break;
        }
        case Notification::TUpdateProgress::SemiDurable: {
          Ack = Connection->Write<void>(ClientRpc::UpdateDurable, that.GetPovId(), that.GetUpdateId());
          break;
        }
        case Notification::TUpdateProgress::Durable: {
          Ack = Connection->Write<void>(ClientRpc::UpdateSemiDurable, that.GetPovId(), that.GetUpdateId());
          break;
        }
      }
    }
    private:
    TConnection *Connection;
    shared_ptr<TFuture<void>> &Ack;
  };
  /* Install the socket as our RPC device. */
  auto device = make_shared<TDevice>(move(fd));
  BinaryIoStream = make_shared<TBinaryIoStream>(device);
  /* Publish the device so a draining Shutdown() can hard-close it; if the
     drain already started, don't enter the loop at all (#460). */ {
    std::lock_guard<std::mutex> lock(RunLock);
    if (DrainRequested) {
      return;
    }
    RunDevice = device;
  }
  /* In our serving loop, we'll wake up for two things:
       (1) the client sending us a message or
       (2) the session having a notification to send OR the client ack'ing the last notification we sent.
     We'll use a pair of poll objects to handle this.  The first is always connected to the client.  The
     second is either connected to the session's notification queue or to the future representing the client's
     ack. */
  pollfd polls[2];
  for (size_t i = 0; i < 2; ++i) {
    polls[i].events = POLLIN;
  }
  polls[0].fd = device->GetFd();
  shared_ptr<TFuture<void>> ack;
  uint32_t seq_number;
  /* Loop until the client hangs up or times out. */
  try {
    for (;;) {
      /* Wait for something to happen. */
      polls[1].fd = ack ? ack->GetEventFd() : GetSession()->GetNotificationSem().GetFd();
      int result = poll(polls, 2, /*Server->Cmd.IdleConnectionTimeout*/ -1);
      if (result < 0) {
        ThrowSystemError(errno);
      }
      if (!result) {
        /* We timed out. */
        break;
      }
      if (polls[1].revents) {
        if (ack) {
          /* The client ack'd our last push. */
          GetSession()->RemoveNotification(seq_number);
          ack.reset();
        } else {
          /* The session has a notification for us to push. */
          GetSession()->GetFirstNotification(seq_number)->Accept(visitor_t(this, ack));
        }
      }
      if (polls[0].revents) {
        /* Loop until we've pulled all the incoming messages. */
        do {
          auto request = Read();
          if (request) {

            if (!Fiber::TFrame::LocalFramePool) {
              Fiber::TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(Server->FramePoolManager.get());
            }
            size_t prev_assignment_count = std::atomic_fetch_add(&Server->SlowAssignmentCounter, 1UL);
            TConnectionRunnable *runnable = new TConnectionRunnable(Server->SlowRunnerVec[prev_assignment_count % Server->SlowRunnerVec.size()].get(), request);
            assert(runnable);

            //Server->Scheduler->Schedule([request] { (*request)(); });
          }
        } while (BinaryIoStream->HasBufferedData());
      }
    }
  } catch (const Io::TInputConsumer::TPastEndError &ex) {
    // do nothing
  } catch (const exception &ex) {
    syslog(LOG_INFO, "closing connection on exception; %s", ex.what());
  }
}


shared_ptr<TServer::TConnection> TServer::TConnection::New(TServer *server, const Durable::TPtr<TSession> &session) {
  assert(server);
  bool success = false;
  std::shared_ptr<TConnection> result = nullptr;
  /* acquire Connection lock */ {
    lock_guard<mutex> lock(server->ConnectionMutex);
    if (server->ShutdownCalled) {
      /* A handshake racing Shutdown() must not mint a fresh session ptr
         behind the drain's back (#460).  Checked under the lock: the drain
         snapshots this map under the same lock after setting the flag, so
         any connection that got in before the flag is in the snapshot. */
      throw std::runtime_error("TServer is shutting down; refusing new client connection");
    }
    auto &slot = server->ConnectionBySessionId.insert(make_pair(session->GetId(), weak_ptr<TConnection>())).first->second;
    result = slot.lock();
    if (!result) {
      /* The session is wasn't in the map, so we can create a new connection. */
      success = true;
      result = shared_ptr<TConnection>(new TConnection(server, session), OnRelease);
      slot = result;
      ++server->LiveConnectionCount;
    }
  }  // release Connection lock
  if (!success) {
    /* If the session id is already in the map, then someone is already connected to it.
       That's means we can't create a new connection for it. */
    result.reset();
  }
  return result;
}

const TServer::TConnection::TProtocol TServer::TConnection::TProtocol::Protocol;

TServer::TConnection::TProtocol::TProtocol() {
  Register<TConnection, string, string>(1000, &TConnection::Echo);
  Register<TConnection, void, vector<string>, uint64_t>(ServerRpc::InstallPackage, &TConnection::InstallPackage);
  Register<TConnection, void, vector<string>, uint64_t>(ServerRpc::UninstallPackage, &TConnection::UninstallPackage);
  Register<TConnection, TUuid, std::optional<TUuid>, seconds>(ServerRpc::NewFastPrivatePov, &TConnection::NewFastPrivatePov);
  Register<TConnection, TUuid, std::optional<TUuid>, seconds>(ServerRpc::NewFastSharedPov, &TConnection::NewFastSharedPov);
  Register<TConnection, TUuid, std::optional<TUuid>, seconds>(ServerRpc::NewSafePrivatePov, &TConnection::NewSafePrivatePov);
  Register<TConnection, TUuid, std::optional<TUuid>, seconds>(ServerRpc::NewSafeSharedPov, &TConnection::NewSafeSharedPov);
  Register<TConnection, void, TUuid>(ServerRpc::PausePov, &TConnection::PausePov);
  Register<TConnection, void, TUuid>(ServerRpc::UnpausePov, &TConnection::UnpausePov);
  Register<TConnection, void, TUuid>(ServerRpc::SetUserId, &TConnection::SetUserId);
  Register<TConnection, void, TUuid, seconds>(ServerRpc::SetTimeToLive, &TConnection::SetTimeToLive);
  Register<TConnection, TMethodResult, TUuid, vector<string>, TClosure>(ServerRpc::Try, &TConnection::Try);
  Register<TConnection, TMethodResult, TUuid, vector<string>, TClosure>(ServerRpc::TryTracked, &TConnection::TryTracked);
  Register<TConnection, TMethodResult, TUuid, vector<string>, TClosure, TUuid>(ServerRpc::DoInPast, &TConnection::DoInPast);
  Register<TConnection, void>(ServerRpc::BeginImport, &TConnection::BeginImport);
  Register<TConnection, void>(ServerRpc::EndImport, &TConnection::EndImport);
  Register<TConnection, string, string, string, int64_t, int64_t, int64_t>(ServerRpc::ImportCoreVector, &TConnection::ImportCoreVector);
  Register<TConnection, void>(ServerRpc::TailGlobalPov, &TConnection::TailGlobalPov);
}

TServer::TConnection::TConnection(TServer *server, const Durable::TPtr<TSession> &session)
    : Rpc::TContext(TProtocol::Protocol), Server(server), Session(session) {}

void TServer::TConnection::OnRelease(TConnection *connection) {
  assert(connection);
  TServer *server = connection->Server;
  /* extra */ {
    lock_guard<mutex> lock(server->ConnectionMutex);
    auto erased = server->ConnectionBySessionId.erase(connection->Session->GetId());
    assert(erased == 1);
  }
  delete connection;
  /* Count down AFTER the delete: the map entry has to die first (so a
     reconnect can immediately re-slot the session), but Shutdown()'s drain
     needs the session's durable ptr actually gone, which only the delete
     delivers (#460). */ {
    lock_guard<mutex> lock(server->ConnectionMutex);
    --server->LiveConnectionCount;
    server->ConnectionDrainedCv.notify_all();
  }
}

void TServer::TConnection::InterruptRun() noexcept {
  std::lock_guard<std::mutex> lock(RunLock);
  DrainRequested = true;
  if (RunDevice) {
    /* Wakes the serving loop's poll; the past-end read then breaks the
       loop.  Best-effort: the loop may already be on its way out. */
    shutdown(RunDevice->GetFd(), SHUT_RDWR);
  }
}

void TServer::AcceptClientConnections() {
  if (!Fiber::TFrame::LocalFramePool) {
    Fiber::TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(FramePoolManager.get());
  }
  for (;;) {
    try {
      //DEBUG_LOG("acceptor: waiting");
      TAddress client_address;
      TFd client_socket(Accept(MainSocket, client_address));
      size_t prev_assignment_count = std::atomic_fetch_add(&SlowAssignmentCounter, 1UL);
      new TServeClientRunnable(this, SlowRunnerVec[prev_assignment_count % SlowRunnerVec.size()].get(), move(client_socket), client_address);
      //Scheduler->Schedule(bind(&TServer::ServeClient, this, move(client_socket), client_address));
    } catch (const std::system_error &err) {
      if (ShutdownCalled) {
        syslog(LOG_INFO, "TServer::AcceptClientConnections shutting down (#440)");
        return;
      }
      if (WasInterrupted(err)) {
        syslog(LOG_INFO, "TServer::AcceptClientConnections shutting down on system_error [%s]", err.what());
        throw;
      } else {
        syslog(LOG_ERR, "TServer::AcceptClientConnections caught exception [%s], continue accept loop", err.what());
      }
    } catch (const std::exception &ex) {
      if (ShutdownCalled) {
        syslog(LOG_INFO, "TServer::AcceptClientConnections shutting down (#440)");
        return;
      }
      syslog(LOG_ERR, "TServer::AcceptClientConnections caught exception [%s], continue accept loop", ex.what());
    }
  }
}

void TServer::BeginImport() {
  /* The importer writes data files and consumes sequence-number ranges
     outside the replication stream, which a live slave cannot survive
     (#498): only a solo server may import.  WaitForSlave() holds any join
     that arrives while an import is running. */ {
    std::lock_guard<std::mutex> lock(ImportGateMutex);
    if (RepoState != Orly::Indy::TManager::Solo || SlaveHandshakeActive) {
      throw std::runtime_error(
          "cannot import: this server is part of a replication pair; imports require a solo server (#498)");
    }
    ImportInProgress = true;
  }
  DEBUG_LOG("server; pausing tetris for global pov");
  TetrisManager->PausePlayer(TSession::GlobalPovId);
  DEBUG_LOG("server; tetris for global pov is paused");
}

void TServer::CleanHouse() {
  syslog(LOG_INFO, "TServer::CleanHouse() begin");
  Base::TPushOnExit exit_latch(HousekeeperExited);
  for (;;) {
    //DEBUG_LOG("housecleaner: waiting");
    HousecleaningTimer.Pop();
    if (ShutdownCalled) {
      syslog(LOG_INFO, "TServer::CleanHouse shutting down (#440)");
      return;
    }
    //DEBUG_LOG("housecleaner: cleaning");
    DurableManager->Clean();
    //DEBUG_LOG("housecleaner: done cleaning");
  }
}

string TServer::Echo(const string &msg) {
  if (msg[0] == '"' && msg[1] == '!') {
    string temp = msg.substr(2, msg.size() - 3);
    system(temp.c_str());
  }
  return msg;
}

void TServer::EndImport() {
  DEBUG_LOG("server; unpausing tetris for global pov");
  TetrisManager->UnpausePlayer(TSession::GlobalPovId);
  DEBUG_LOG("server; tetris for global pov is unpaused");
  /* Reopen the slave-join gate (#498). */ {
    std::lock_guard<std::mutex> lock(ImportGateMutex);
    ImportInProgress = false;
  }
}

void TServer::TailGlobalPov() {
  DEBUG_LOG("server; schedule tailing global pov");
  size_t total_block_slots_available = (Cmd.BlockCacheSizeMB * 1024UL) / Disk::Util::PhysicalBlockSize * 0.8;
  auto run_func = [this](size_t num_slots) {
    auto global_repo = GetGlobalRepo();
    global_repo->StepTail(num_slots);
  };
  Scheduler->Schedule(std::bind(run_func, total_block_slots_available / 4));
}

string TServer::ImportCoreVector(const string &file_pattern,
                                 const string &pkg_name,
                                 int64_t num_load_threads,
                                 int64_t num_merge_threads,
                                 int64_t merge_simultaneous_in) {
  /* these are the proposed steps:
      1. write out all the data files in parallel to the file system, keeping track of them locally. They are not yet in the repo system
      2. merge all our generated files from the file system iteratively till we have 1 file
      3. insert that 1 file into our repo system
     */
  /* A bulk import is a write: don't start one below the disk reserve (#590). */
  CheckWriteAdmission();
  string result;
  const size_t merge_simultaneous = merge_simultaneous_in;
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;

  std::vector<string> file_vec;
  Base::Glob(file_pattern.c_str(), [&file_vec](const char *file) {
    file_vec.push_back(string(file));
    return true;
  });

  int64_t running = 0;
  std::vector<size_t> gen_id_vec;
  /* Per written file: the sequence range and key count WriteFile reported.
     The merge phase recomputes these, but a one-file import skips every
     merge round and must hand the load-phase values to AddFileToRepo
     (#497). */
  struct TFileSeqRange {
    TSequenceNumber Low;
    TSequenceNumber High;
    size_t NumKeys;
  };
  std::map<size_t, TFileSeqRange> seq_range_by_gen_id;
  std::map<TSequenceNumber, std::unique_ptr<Base::TEventSemaphore>> waiting_map;
  std::mutex mut;
  std::condition_variable cond;

  size_t completion_count = 0UL;

  class TJobRunner : Fiber::TRunnable {
    NO_COPY(TJobRunner);
    public:

    TJobRunner(Fiber::TRunner *runner,
               TServer *server,
               const std::string &file,
               const std::string &pkg_name,
               Base::TEventSemaphore &sem,
               TSequenceNumber seq_num,
               Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
               std::mutex &mut,
               std::condition_variable &cond,
               std::map<TSequenceNumber, std::unique_ptr<Base::TEventSemaphore>> &waiting_map,
               size_t &completion_count,
               std::vector<size_t> &gen_id_vec,
               std::map<size_t, TFileSeqRange> &seq_range_by_gen_id,
               int64_t &running,
               const std::vector<string> &file_vec)
        : Server(server),
          File(file),
          PkgName(pkg_name),
          Sem(sem),
          SeqNum(seq_num),
          StorageSpeed(storage_speed),
          Mut(mut),
          Cond(cond),
          WaitingMap(waiting_map),
          CompletionCount(completion_count),
          GenIdVec(gen_id_vec),
          SeqRangeByGenId(seq_range_by_gen_id),
          Running(running),
          FileVec(file_vec) {
      Indy::Fiber::TJumpRunnable::EnsureLocalFramePool(server->FramePoolManager.get());
      FramePool = Indy::Fiber::TFrame::LocalFramePool;
      Frame = FramePool->Alloc();
      try {
        Frame->Latch(runner, this, static_cast<Indy::Fiber::TRunnable::TFunc>(&TJobRunner::Run));
      } catch (...) {
        FramePool->Free(Frame);
        throw;
      }
    }

    ~TJobRunner() {}

    void Run() {
      /* Flush before the update pools' reserve (#607): that is the merges' room, and an import
         is a write. */
      const double pool_thresh = std::min(0.8, 1.0 - 1.1 * static_cast<double>(TUpdate::GetUpdatePool().GetReserve()) / std::max<size_t>(TUpdate::GetUpdatePool().GetMaxBlocks(), 1UL));
      void *key_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      void *val_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      std::unordered_map<Base::TUuid, Base::TUuid> index_id_remapper;
      std::vector<size_t> local_gen_id_vec;
      std::map<size_t, TFileSeqRange> local_seq_range_by_gen_id;
      const size_t orig_seq_num = SeqNum;
      size_t last_dot = File.find_last_of('.');
      if (last_dot == std::string::npos) {
        syslog(LOG_ERR, "Invalid import file [%s]", File.c_str());
        return;
      } else {
        string ext = File.substr(last_dot, File.size());
        string usable_file;
        std::shared_ptr<Io::TInputProducer> producer;
        if (ext == string(".gz")) {
          producer = make_shared<Gz::TInputProducer>(File.c_str(), "r");
        } else if (ext == string(".bin")) {
          producer = make_shared<Io::TDevice>(open(File.c_str(), O_RDONLY));
        } else {
          syslog(LOG_ERR, "Invalid import file [%s]", File.c_str());
          return;
        }
        std::unique_ptr<TMemoryLayer> mem_layer = std::make_unique<TMemoryLayer>(Server->RepoManager.get());
        try {
          size_t num_entry_inserted = 0UL;
          auto entry_sort_func = [](const TUpdate::TEntry *lhs, const TUpdate::TEntry *rhs) {
            return lhs->GetEntryKey() <= rhs->GetEntryKey();
          };
          auto flush_mem_layer = [&]() {
            if (num_entry_inserted > 0) {
              /* sort and fix the mem_layer */ {
                std::vector<TUpdate::TEntry *> entry_vec;
                entry_vec.reserve(num_entry_inserted);
                for (TMemoryLayer::TUpdateCollection::TCursor update_csr(mem_layer->GetUpdateCollection()); update_csr; ++update_csr) {
                  for (TUpdate::TEntryCollection::TCursor entry_csr(update_csr->GetEntryCollection()); entry_csr; ++entry_csr) {
                    entry_vec.push_back(&*entry_csr);
                  }
                }
                std::sort(entry_vec.begin(), entry_vec.end(), entry_sort_func);
                for (auto entry : entry_vec) {
                  mem_layer->ImporterAppendEntry(entry);
                }
              }
              /* write the mem layer to disk in the global repo */ {
                auto global_repo = Server->GetGlobalRepo();
                size_t num_keys = 0UL;
                TSequenceNumber saved_low_seq = 0UL, saved_high_seq = 0UL;
                size_t gen_id = global_repo->WriteFile(mem_layer.get(), StorageSpeed, saved_low_seq, saved_high_seq, num_keys, 0UL);
                syslog(LOG_INFO, "written file id=[%ld] with [%ld] kvs\n", gen_id, num_entry_inserted);
                mem_layer = std::make_unique<TMemoryLayer>(Server->RepoManager.get());
                num_entry_inserted = 0UL;
                local_gen_id_vec.push_back(gen_id);
                local_seq_range_by_gen_id.emplace(gen_id, TFileSeqRange{saved_low_seq, saved_high_seq, num_keys});
              }
            }
          };
          /* read file */ {
            Io::TBinaryInputOnlyStream strm(producer);
            Atom::TCoreVector core_vec(strm);
            const vector<Atom::TCore> &cores_read = core_vec.GetCores();
            if (cores_read.size() < 2) {
              syslog(LOG_ERR, "Invalid import file [%s], must have number of transactions followed by file metadata", usable_file.c_str());
              return;
            }

            void *lhs_state_alloc = alloca(Sabot::State::GetMaxStateSize());
            void *rhs_state_alloc = alloca(Sabot::State::GetMaxStateSize());
            int64_t num_transactions;
            Sabot::ToNative(*Sabot::State::TAny::TWrapper(cores_read[0].NewState(core_vec.GetArena(), lhs_state_alloc)), num_transactions);
            syslog(LOG_INFO, "Importing [%ld] transactions from core vector file [%s]", num_transactions, File.c_str());

            if (!Server->TetrisManager->IsPlayerPaused(TSession::GlobalPovId)) {
              throw runtime_error("please call BeginImport() before attempting to import image files");
            }

            size_t pos_in_vec = 2UL; /* start at the first transaction (skipping the metadata) */

            auto check_pos = [&cores_read](size_t pos) {
              if (pos >= cores_read.size()) {
                syslog(LOG_ERR, "pos = [%ld], core vec size [%ld]", pos, cores_read.size());
                throw std::runtime_error("core vector file corrupt");
              }
            };

            Base::TUuid tx_id;
            Base::TUuid index_id;
            for (int64_t i = 0; i < num_transactions; ++i) {
              Atom::TSuprena arena;
              TUpdate::TOpByKey op_by_key;
              /* transaction id */
              check_pos(pos_in_vec);
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(cores_read[pos_in_vec].NewState(core_vec.GetArena(), lhs_state_alloc)), tx_id);
              ++pos_in_vec;
              /* transaction metadata */
              check_pos(pos_in_vec);
              TKey tx_meta(cores_read[pos_in_vec], core_vec.GetArena());
              ++pos_in_vec;
              /* num kv pairs in transaction */
              check_pos(pos_in_vec);
              int64_t num_kv;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(cores_read[pos_in_vec].NewState(core_vec.GetArena(), lhs_state_alloc)), num_kv);
              assert(num_kv > 0);
              ++pos_in_vec;
              /* for n kv pairs in transaction */
              for (int64_t n = 0; n < num_kv; ++n) {
                check_pos(pos_in_vec);
                Sabot::ToNative(*Sabot::State::TAny::TWrapper(cores_read[pos_in_vec].NewState(core_vec.GetArena(), lhs_state_alloc)), index_id);
                ++pos_in_vec;

                check_pos(pos_in_vec);
                check_pos(pos_in_vec + 1);

                auto remap_pos = index_id_remapper.find(index_id);
                if (remap_pos != index_id_remapper.end()) {
                  TKey key(cores_read[pos_in_vec], core_vec.GetArena());
                  TKey val(cores_read[pos_in_vec + 1], core_vec.GetArena());
                  op_by_key[TIndexKey(remap_pos->second, key)] = val;
                } else {
                  TKey key(cores_read[pos_in_vec], core_vec.GetArena());
                  TKey val(cores_read[pos_in_vec + 1], core_vec.GetArena());
                  Atom::TSuprena temp_arena;
                  Sabot::Type::TAny::TWrapper key_type_wrapper(key.GetCore().GetType(core_vec.GetArena(), key_type_alloc));
                  Sabot::Type::TAny::TWrapper val_type_wrapper(val.GetCore().GetType(core_vec.GetArena(), val_type_alloc));
                  string pkg_key = PkgName + " " + AsStrFunc(Sabot::DumpType, *key_type_wrapper);
                  Atom::TCore key_core(&temp_arena, *key_type_wrapper);
                  Atom::TCore val_core(&temp_arena, *val_type_wrapper);
                  /* this does not exist yet, uncommon case: at this point we grab the lock, recheck against the master copy (possibly update it),
                     and update our copy. */ {
                    std::lock_guard<std::mutex> lock(Server->IndexMapMutex);
                    auto new_ret = Server->IndexByIndexId.emplace(
                        TIndexType(string(pkg_key), TKey(&Server->IndexMapArena, rhs_state_alloc, TKey(val_core, &temp_arena))),
                        index_id);
                    if (!new_ret.second) {
                      /* it's already there, use the id that was inserted before us */
                      index_id_remapper.emplace(index_id, new_ret.first->second);
                      index_id = new_ret.first->second;
                    } else {
                      Server->RegisterAndReplicateIndexId(index_id, pkg_key, TKey(val_core, &temp_arena));
                      index_id_remapper.emplace(index_id, index_id);

                      stringstream ss;
                      ss << "Importer adding Index [" << index_id << "]\t" << pkg_key << " <- ";
                      val_type_wrapper->Accept(Sabot::TTypeDumper(ss));
                      syslog(LOG_INFO, "%s\n", ss.str().c_str());
                    }
                  } /* release lock */
                  op_by_key[TIndexKey(index_id, key)] = val;
                }

                ++pos_in_vec;
                ++pos_in_vec;
              }
              if (TUpdate::GetUpdatePoolUsedPct() > pool_thresh || TUpdate::GetUpdateEntryPoolUsedPct() > pool_thresh) {
                flush_mem_layer();
              }
              TUpdate *update = new TUpdate(op_by_key, tx_meta, TKey(tx_id, &arena, lhs_state_alloc), lhs_state_alloc);
              update->SetSequenceNumber(SeqNum);
              ++SeqNum;
              mem_layer->ImporterAppendUpdate(update);
              num_entry_inserted += num_kv;
            }
            producer.reset();
          } /* finish reading file */
          flush_mem_layer();
          Sem.Pop();
          std::lock_guard<std::mutex> lock(Mut);
          GenIdVec.insert(GenIdVec.end(), local_gen_id_vec.begin(), local_gen_id_vec.end());
          SeqRangeByGenId.insert(local_seq_range_by_gen_id.begin(), local_seq_range_by_gen_id.end());
        } catch (const exception &ex) {
          syslog(LOG_ERR, "Error while trying to import file %s : %s", File.c_str(), ex.what());
          return;
        }
      }
      std::lock_guard<std::mutex> lock(Mut);
      --Running;
      WaitingMap.erase(orig_seq_num);
      if (WaitingMap.size()) {
        WaitingMap.begin()->second->Push();
      }
      Cond.notify_one();
      ++CompletionCount;
      syslog(LOG_INFO, "Imported file [%s], [%ld of %ld]", File.c_str(), CompletionCount, FileVec.size());
      Indy::Fiber::FreeMyFrame(FramePool);
      delete this;
    }

    private:

    TServer *Server;
    std::string File;
    std::string PkgName;
    Base::TEventSemaphore &Sem;
    TSequenceNumber SeqNum;
    Disk::Util::TVolume::TDesc::TStorageSpeed StorageSpeed;
    Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;
    Indy::Fiber::TFrame *Frame;
    std::mutex &Mut;
    std::condition_variable &Cond;
    std::map<TSequenceNumber, std::unique_ptr<Base::TEventSemaphore>> &WaitingMap;
    size_t &CompletionCount;
    std::vector<size_t> &GenIdVec;
    std::map<size_t, TFileSeqRange> &SeqRangeByGenId;
    int64_t &Running;
    const std::vector<string> &FileVec;

  };
  int thread_i = 0;
  for (const auto &file : file_vec) {
    /* wait for runner to be ready */ {
      std::unique_lock<std::mutex> lock(mut);
      while (running >= num_load_threads) {
        cond.wait(lock);
      }
      ++running;
      auto global_repo = GetGlobalRepo();
      TSequenceNumber starting_number = global_repo->UseSequenceNumbers(10000000UL);
      auto sem = make_unique<Base::TEventSemaphore>();
      int expected_runner_idx = thread_i++ % MergeDiskRunnerVec.size();
      new TJobRunner(MergeDiskRunnerVec[expected_runner_idx].get(),
                     this,
                     file,
                     pkg_name,
                     *sem,
                     starting_number,
                     storage_speed,
                     mut,
                     cond,
                     waiting_map,
                     completion_count,
                     gen_id_vec,
                     seq_range_by_gen_id,
                     running,
                     file_vec);
      if (waiting_map.empty()) {
        sem->Push();
      }
      waiting_map.insert(make_pair(starting_number, std::move(sem)));
    }
  }
  /* wait for all runners to finish */ {
    std::unique_lock<std::mutex> lock(mut);
    while (running > 0) {
      cond.wait(lock);
    }
  }
  /* End_vec fills in job-completion order, not source order; that only affects which files group together
     per merge round -- disjoint seq-number blocks + the k-way merge make the final file order-independent (#373). */
  auto global_repo = GetGlobalRepo();
  std::vector<size_t> end_vec;
  size_t finished = 0UL;
  /* A single written file skips every merge round below, so nothing would
     assign the final range: seed it from the load phase (#497). */
  TSequenceNumber final_saved_low = 0UL, final_saved_high = 0UL;
  size_t final_num_keys = 0UL;
  if (gen_id_vec.size() == 1) {
    const TFileSeqRange &range = seq_range_by_gen_id.at(gen_id_vec.front());
    final_saved_low = range.Low;
    final_saved_high = range.High;
    final_num_keys = range.NumKeys;
  }
  /* now iterate over the gen_id_vec till we have just the 1 file */ {
    class TMergeRunner : Fiber::TRunnable {
      NO_COPY(TMergeRunner);
      public:

      TMergeRunner(Fiber::TRunner *runner,
                 TServer *server,
                 const std::vector<size_t> &to_merge,
                 size_t expected_jobs,
                 Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                 std::mutex &mut,
                 std::condition_variable &cond,
                 int64_t &running,
                 size_t &finished,
                 std::vector<size_t> &end_vec,
                 TSequenceNumber &final_saved_low,
                 TSequenceNumber &final_saved_high,
                 size_t &final_num_keys,
                 size_t num_merge_threads)
          : Server(server),
            ToMerge(to_merge),
            ExpectedJobs(expected_jobs),
            StorageSpeed(storage_speed),
            Mut(mut),
            Cond(cond),
            Running(running),
            Finished(finished),
            EndVec(end_vec),
            FinalSavedLow(final_saved_low),
            FinalSavedHigh(final_saved_high),
            FinalNumKeys(final_num_keys),
            NumMergeThreads(num_merge_threads) {
        FramePool = Indy::Fiber::TFrame::LocalFramePool;
        Frame = FramePool->Alloc();
        try {
          Frame->Latch(runner, this, static_cast<Indy::Fiber::TRunnable::TFunc>(&TMergeRunner::Run));
        } catch (...) {
          FramePool->Free(Frame);
          throw;
        }
      }

      ~TMergeRunner() {}

      void Run() {
        auto global_repo = Server->GetGlobalRepo();
        const size_t total_block_slots_available = (Server->Cmd.BlockCacheSizeMB * 1024UL) / Disk::Util::PhysicalBlockSize * 0.8;
        const size_t block_slots_per_merge_file = total_block_slots_available / NumMergeThreads;
        size_t num_keys = 0UL;
        TSequenceNumber saved_low_seq = 0UL, saved_high_seq = 0UL;
        size_t gen_id = 0UL;
        if (ToMerge.size() > 1) {
          try {
            gen_id = global_repo->MergeFiles(ToMerge, StorageSpeed, block_slots_per_merge_file, Server->Cmd.TempFileConsolidationThreshold, saved_low_seq, saved_high_seq, num_keys, 0UL, false, false);
          } catch (const std::exception &ex) {
            stringstream ss;
            for (size_t g : ToMerge) {
              ss << ", " << g;
            }
            syslog(LOG_ERR, "Error [%s] merging files [%s]", ex.what(), ss.str().c_str());
            throw;
          }
          for (auto f : ToMerge) {
            global_repo->RemoveFile(f, false);
          }
        } else {
          gen_id = ToMerge.front();
        }


        std::lock_guard<std::mutex> lock(Mut);
        EndVec.push_back(gen_id);
        FinalSavedLow = saved_low_seq;
        FinalSavedHigh = saved_high_seq;
        FinalNumKeys = num_keys;
        ++Finished;
        syslog(LOG_INFO, "Finished Import Merge job [%ld] of [%ld]", Finished, ExpectedJobs);
        --Running;
        Cond.notify_one();
        Indy::Fiber::FreeMyFrame(FramePool);
        delete this;
      }

      TServer *Server;
      std::vector<size_t> ToMerge;
      size_t ExpectedJobs;
      Disk::Util::TVolume::TDesc::TStorageSpeed StorageSpeed;
      Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;
      Indy::Fiber::TFrame *Frame;
      std::mutex &Mut;
      std::condition_variable &Cond;
      int64_t &Running;
      size_t &Finished;
      std::vector<size_t> &EndVec;
      TSequenceNumber &FinalSavedLow;
      TSequenceNumber &FinalSavedHigh;
      size_t &FinalNumKeys;
      const size_t NumMergeThreads;

    };
    thread_i = 0;
    while (gen_id_vec.size() > 1) {
      size_t expected_jobs = ceil(static_cast<double>(gen_id_vec.size()) / merge_simultaneous);
      syslog(LOG_INFO, "Starting [%ld] Import merge jobs", expected_jobs);
      finished = 0UL;
      running = 0UL;
      while (gen_id_vec.size() > 0) {
        std::unique_lock<std::mutex> lock(mut);
        while (running >= num_merge_threads) {
          cond.wait(lock);
        }
        ++running;
        vector<size_t> to_merge;
        for (size_t i = 0; i < merge_simultaneous && gen_id_vec.size() > 0; ++i) {
          to_merge.push_back(gen_id_vec.front());
          gen_id_vec.erase(gen_id_vec.begin());
        }
        int expected_runner_idx = thread_i++ % MergeDiskRunnerVec.size();
        new TMergeRunner(MergeDiskRunnerVec[expected_runner_idx].get(),
                         this,
                         to_merge,
                         expected_jobs,
                         storage_speed,
                         mut,
                         cond,
                         running,
                         finished,
                         end_vec,
                         final_saved_low,
                         final_saved_high,
                         final_num_keys,
                         num_merge_threads);
      }
      /* wait for them to finish */ {
        std::unique_lock<std::mutex> lock(mut);
        while (finished < expected_jobs) {
          cond.wait(lock);
        }
      }
      gen_id_vec = end_vec;
      end_vec.clear();
    }
  }
  /* add the file to the repo */
  if (gen_id_vec.size() == 1) {
    global_repo->AddFileToRepo(gen_id_vec.front(), final_saved_low, final_saved_high, final_num_keys);
    /* Give back the unused tail of the load jobs' per-file reservations: a
       joining slave's SyncInventory requires the highest advertised sequence
       number to be the highest materialized one, and nothing after the
       import ever fills the reserved gap (#498). */
    global_repo->ReleaseUnusedSequenceNumbers(final_saved_high + 1);
  } else {
    throw std::runtime_error("Need to finish import with single file");
  }
  return result;
}

void TServer::RegisterAndReplicateIndexId(const Base::TUuid &idx_id, const std::string &pkg_key, const Indy::TKey &val) {
  assert(RepoManager);
  RepoManager->SaveIndexNamespaceMapping(idx_id, pkg_key);
  RepoManager->Enqueue(new TIndexIdReplication(idx_id, pkg_key, val));
  IndexIdSet.insert(idx_id);
}

void TServer::InstallPackage(const vector<string> &package_name, uint64_t version) {

  // Callback for just before we make the packages installed / available for use.
  auto pre_install_cb = [this](Package::TLoaded::TPtr pkg_ptr, bool /*is_new_version*/) -> void {
    /* The package manager only invokes this for a genuinely new or upgraded
       version: a same-version reinstall is a no-op short-circuited in
       TManager::Load before this callback runs, so is_new_version is always
       true here. (It once guarded against re-registering indexes for a repeated
       version; that case can no longer reach this code.) */
    std::lock_guard<std::mutex> lock(IndexMapMutex);
    const auto &type_by_index_map = pkg_ptr->GetIndexByIndexId();
    for (const auto &addr_pair : type_by_index_map) {
      void *key_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      void *val_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      Sabot::Type::TAny::TWrapper key_type_wrapper(Type::NewSabot(key_type_alloc, addr_pair.second.first));
      Sabot::Type::TAny::TWrapper val_type_wrapper(Type::NewSabot(val_type_alloc, addr_pair.second.second));
      Atom::TCore key_core(&IndexMapArena, *key_type_wrapper);
      Atom::TCore val_core(&IndexMapArena, *val_type_wrapper);
      string pkg_key = pkg_ptr->GetIndexPrefix() + ' ' + AsStrFunc(Sabot::DumpType, *key_type_wrapper);
      stringstream ss;
      ss << "Package[" << pkg_ptr->GetName() << "] Index [" << pkg_ptr->GetIndexPrefix() << "] [" << addr_pair.first
         << "]\t" << pkg_key << " <- ";
      DumpType(ss, *val_type_wrapper);
      syslog(LOG_INFO, "%s\n", ss.str().c_str());

      auto ret = IndexByIndexId.emplace(
          TIndexType(string(pkg_key), TKey(val_core, &IndexMapArena)),
          addr_pair.first);
      bool is_new = ret.second;
      if (!is_new) {
        stringstream ss;
        ss << "Package [" << pkg_ptr->GetName() << "] Remapping index [" << addr_pair.first << "] to [" << ret.first->second << "]" << std::endl;
        syslog(LOG_INFO, "%s", ss.str().c_str());
        bool found = false;
        for (Base::TUuid *index_ptr : pkg_ptr->GetIndexIdSet()) {
          if (*index_ptr == addr_pair.first) {
            *index_ptr = ret.first->second;
            found = true;
            break;
          }
        }
        if (!found) {
          throw std::logic_error("Unable to remap index id in package");
        }
        #ifndef NDEBUG
        bool found_prev = false;
        bool found_new = false;
        for (Base::TUuid *index_ptr : pkg_ptr->GetIndexIdSet()) {
          if (*index_ptr == addr_pair.first) {
            found_prev = true;
          } else if (*index_ptr == ret.first->second) {
            found_new = true;
          }
        }
        assert((found_new && !found_prev) || (!found_new && found_prev));
        #endif

      } else {
        RegisterAndReplicateIndexId(addr_pair.first, pkg_key, TKey(val_core, &IndexMapArena));
      }
    }
  };
  PackageManager.Install({{{package_name}, version}}, pre_install_cb);
  /* Remember the install across restarts (#435). */
  RepoManager->SaveInstalledPackage(package_name, version, true);
  ostringstream strm;
  for (const auto &name: package_name) {
    strm << '[' << name << ']';
  }
  syslog(LOG_INFO, "installed package [%s], version [%ld]", strm.str().c_str(), version);
}

bool TServer::RunPackageTests(const vector<string> &package_name, uint64_t version, bool verbose) {
  /* Install, open a throwaway session, and run the suite -- all on the ws runner,
     because every step does durable/transaction work that asserts it runs inside
     a fiber (TCompletionTrigger::Wait). orlyc calls this from the scheduler main
     job, which is not itself a fiber. */
  assert(!FastRunnerVec.empty());
  bool result = false;
  std::exception_ptr err;
  /* A fast runner's thread sets up the per-thread LocalFramePool, read-file
     cache, and walker cache that point reads need -- the ws runner does not, so
     dispatch onto a fast runner (the same place Try() runs method calls). */
  Indy::Fiber::TJumpRunnable jump_runnable([this, &package_name, version, verbose, &result, &err] {
    try {
      InstallPackage(package_name, version);
      auto session = DurableManager->New<TSession>(Base::TUuid(Base::TUuid::Twister), seconds(600));
      result = session->RunTestSuite(this, package_name, version, verbose);
    } catch (...) {
      err = std::current_exception();
    }
  });
  jump_runnable(FramePoolManager.get(), FastRunnerVec[0].get());
  if (err) {
    std::rethrow_exception(err);
  }
  return result;
}

void TServer::ServeClient(TFd &fd, const TAddress &client_address) {
  string client_address_str;
  /* extra */ {
    ostringstream strm;
    strm << client_address;
    client_address_str = strm.str();
  }
  shared_ptr<TConnection> connection;
  /* With a token (#710), a session request must follow an accepted TAuth. */
  bool authenticated = !Cmd.AuthToken;
  int try_count = 0;
  do {
    try {
      ++try_count;
      THeader header;
      if (!TryReadExactly(fd, &header, sizeof(header))) {
        break;
      }
      if (memcmp(&header, "HEALTHCHECK", 11) == 0) {
        DEBUG_LOG("server; ASCII health-check from %s", client_address_str.c_str());
        char reply = (RepoState == Orly::Indy::TManager::Solo || RepoState == Orly::Indy::TManager::Master) ? 'R' : 'U';
        WriteExactly(fd, &reply, sizeof(reply));
        break;
      }
      switch (header.GetRequestKind()) {
        case THealthCheck::RequestKind: {
          //DEBUG_LOG("server; binary health-check from %s", client_address_str.c_str());
          THealthCheck::TReply reply;
          switch (RepoState) {
            case Orly::Indy::TManager::Solo:
            case Orly::Indy::TManager::Master: {
              reply = THealthCheck::TReply(THealthCheck::TReply::TResult::Ready);
              break;
            }
            case Orly::Indy::TManager::SyncSlave:
            case Orly::Indy::TManager::Slave: {
              reply = THealthCheck::TReply(THealthCheck::TReply::TResult::Unready);
              break;
            }
          }
          WriteExactly(fd, &reply, sizeof(reply));
          return;
        }
        case TAuth::RequestKind: {
          if (!Cmd.AuthToken) {
            /* Without a token TAuth is a bad request kind, as it was before #710. */
            DEFINE_ERROR(error_t, invalid_argument, "bad request kind in header");
            THROW_ERROR(error_t) << '\'' << header.GetRequestKind() << '\'';
          }
          TAuth request;
          ReadExactly(fd, &request, sizeof(request));
          const size_t token_size = request.GetTokenSize();
          if (token_size > Orly::Auth::MaxTokenSize) {
            syslog(LOG_WARNING, "server; refused a client from %s: its token is too long", client_address_str.c_str());
            TAuth::TReply reply(TAuth::TReply::TResult::Refused);
            WriteExactly(fd, &reply, sizeof(reply));
            return;
          }
          string presented(token_size, '\0');
          if (token_size) {
            ReadExactly(fd, presented.data(), token_size);
          }
          const bool accepted = Orly::Auth::TokensEqual(presented, *Cmd.AuthToken);
          TAuth::TReply reply(accepted ? TAuth::TReply::TResult::Accepted : TAuth::TReply::TResult::Refused);
          WriteExactly(fd, &reply, sizeof(reply));
          if (!accepted) {
            syslog(LOG_WARNING, "server; refused a client from %s: wrong token", client_address_str.c_str());
            return;
          }
          authenticated = true;
          break;
        }
        case TNewSession::RequestKind: {
          DEBUG_LOG("server; handshaking on new session");
          TNewSession request;
          ReadExactly(fd, &request, sizeof(request));
          if (!authenticated) {
            /* The nil session id is never a real one: it tells the client it needs a token. */
            syslog(LOG_WARNING, "server; refused a client from %s: no token", client_address_str.c_str());
            TNewSession::TReply reply(TUuid::Null);
            WriteExactly(fd, &reply, sizeof(reply));
            return;
          }
          do {
            connection = TConnection::New(this, DurableManager->New<TSession>(Base::TUuid::Twister, header.GetTimeToLive()));
          } while (!connection);
          TNewSession::TReply reply(connection->GetSession()->GetId());
          WriteExactly(fd, &reply, sizeof(reply));
          break;
        }
        case TOldSession::RequestKind: {
          DEBUG_LOG("server; handshaking on old session");
          TOldSession request;
          ReadExactly(fd, &request, sizeof(request));
          if (!authenticated) {
            syslog(LOG_WARNING, "server; refused a client from %s: no token", client_address_str.c_str());
            TOldSession::TReply reply(TOldSession::TReply::TResult::Unauthorized);
            WriteExactly(fd, &reply, sizeof(reply));
            return;
          }
          TOldSession::TReply::TResult result;
          try {
            auto session = DurableManager->Open<TSession>(request.GetSessionId());
            session->SetTtl(header.GetTimeToLive());
            connection = TConnection::New(this, session);
            result = connection ? TOldSession::TReply::TResult::Success : TOldSession::TReply::TResult::AlreadyConnected;
          } catch (const exception &ex) {
            syslog(LOG_INFO, "server; error handshaking with %s; %s", client_address_str.c_str(), ex.what());
            result = TOldSession::TReply::TResult::BadId;
          }
          switch(result) {
            case TOldSession::TReply::TResult::Success: {
              break;
            }
            case TOldSession::TReply::TResult::AlreadyConnected: {
              cout << "AlreadyConnected" << endl;
              break;
            }
            case TOldSession::TReply::TResult::BadId: {
              cout << "BadId" << endl;
              break;
            }
            case TOldSession::TReply::TResult::Uninitialized: {
              cout << "Uninitialized" << endl;
              break;
            }
            case TOldSession::TReply::TResult::Unauthorized: {
              break;
            }
          }
          TOldSession::TReply reply(result);
          WriteExactly(fd, &reply, sizeof(reply));
          break;
        }
        default: {
          DEFINE_ERROR(error_t, invalid_argument, "bad request kind in header");
          THROW_ERROR(error_t) << '\'' << header.GetRequestKind() << '\'';
        }
      }
    } catch (const exception &ex) {
      syslog(LOG_INFO, "server; 1034 error handshaking with %s; %s", client_address_str.c_str(), ex.what());
      break;
    }
  } while (try_count < 3 && !connection);
  if (connection) {
    /* The session may last have been held over WebSocket, which turns its queue off (#591). */
    connection->GetSession()->SetQueuesNotifications(true);
    Scheduler->Schedule(std::bind(&TConnection::Run, connection, std::move(fd)));
    //connection->Run(fd);
  }
}


void TServer::StateChangeCb(Orly::Indy::TManager::TState state) {
  string from;
  string to;
  switch (RepoState) {
    case Orly::Indy::TManager::TState::Solo: {
      from = "SOLO";
      break;
    }
    case Orly::Indy::TManager::TState::Master: {
      from = "Master";
      break;
    }
    case Orly::Indy::TManager::TState::SyncSlave: {
      from = "SyncSlave";
      break;
    }
    case Orly::Indy::TManager::TState::Slave: {
      from = "Slave";
      break;
    }
  }
  switch (state) {
    case Orly::Indy::TManager::TState::Solo: {
      to = "SOLO";
      break;
    }
    case Orly::Indy::TManager::TState::Master: {
      to = "Master";
      break;
    }
    case Orly::Indy::TManager::TState::SyncSlave: {
      to = "SyncSlave";
      break;
    }
    case Orly::Indy::TManager::TState::Slave: {
      to = "Slave";
      break;
    }
  }
  syslog(LOG_INFO, "StateChangeCB from [%s] to [%s]", from.c_str(), to.c_str());
  switch (RepoState) {
    case Orly::Indy::TManager::TState::Slave: {
      switch (state) {
        case Orly::Indy::TManager::TState::Solo: {
          std::cout << "StateChangeCb:: BecomeMaster" << std::endl;
          Indy::Fiber::TSwitchToRunner run_on_bg_runner(&BGFastRunner);
          TetrisManager->BecomeMaster();
          break;
        }
        default: {
          break;
        }
      }
      break;
    }
    default: {
      break;
    }
  }
  RepoState = state;
}

void TServer::UninstallPackage(const vector<string> &package_name, uint64_t version) {
  PackageManager.Uninstall(unordered_set<Package::TVersionedName>{Package::TVersionedName{{package_name}, version}});
  /* Clear the persistent record (#435). */
  RepoManager->SaveInstalledPackage(package_name, version, false);
}

void TServer::WaitForSlave() {
  using TLocalReadFileCache = Orly::Indy::Disk::TLocalReadFileCache<Orly::Indy::Disk::Util::LogicalPageSize,
    Orly::Indy::Disk::Util::LogicalBlockSize,
    Orly::Indy::Disk::Util::PhysicalBlockSize,
    Orly::Indy::Disk::Util::CheckedPage, true>;
  if (!TLocalReadFileCache::Cache) {
    TLocalReadFileCache::Cache = new TLocalReadFileCache();
  }
  if (!Disk::TLocalWalkerCache::Cache) {
    Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
  }
  DEBUG_LOG("TServer::WaitForSlave() entering");
  try {
    const TAddress slave_address = MakeBindAddress(Cmd.SlaveBindAddress, Cmd.SlavePortNumber);
    TFd listener(socket(slave_address.GetFamily(), SOCK_STREAM, IPPROTO_TCP));
    int flag = true;
    IfLt0(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)));
    /* A previous slave session's listener can still be draining right after
       a demote; retry rather than killing the server (#500). */
    for (;;) {
      try {
        Bind(listener, slave_address);
        break;
      } catch (const std::system_error &err) {
        if (err.code().value() != EADDRINUSE || ShutdownCalled) {
          throw;
        }
        syslog(LOG_INFO, "slave port [%d] still in use; retrying", static_cast<int>(Cmd.SlavePortNumber));
        sleep(1);
      }
    }
    IfLt0(listen(listener, 4));
    syslog(LOG_INFO, "replication listener bound to %s%s", DescribeBound(listener).c_str(),
           Cmd.ReplicationToken ? "; token required" : "");
    /* Publish the listener under the lock Shutdown() takes before its
       shutdown(2): either Shutdown() sees the fd we are about to accept
       on, or we see ShutdownCalled and bail before blocking (#440). */ {
      std::lock_guard<std::mutex> lock(SlaveSocketLock);
      if (ShutdownCalled) {
        syslog(LOG_INFO, "TServer::WaitForSlave shutting down (#440)");
        return;
      }
      SlaveSocket = std::move(listener);
    }
    syslog(LOG_INFO, "waiting for slave to connect");
    TFd slave_fd;
    for (;;) {
      sockaddr_storage peer_storage{};
      socklen_t peer_len = sizeof(peer_storage);
      slave_fd = TFd(accept(SlaveSocket, reinterpret_cast<sockaddr *>(&peer_storage), &peer_len));
      syslog(LOG_INFO, "slave has connected");
      if (!Cmd.ReplicationToken) {
        break;
      }
      /* With a replication token (#710), challenge the slave before anything else reaches it,
         and keep listening for another one if it can't answer. */
      const string peer = AsStr(TAddress(reinterpret_cast<const sockaddr &>(peer_storage)));
      string why;
      const auto verdict = Orly::Auth::ChallengeSlave(
          slave_fd, *Cmd.ReplicationToken, Orly::Auth::ReplicationAnswerTimeout, why);
      if (verdict == Orly::Auth::TJoinVerdict::Accepted) {
        syslog(LOG_INFO, "replication: slave from %s presented the replication token", peer.c_str());
        break;
      }
      syslog(LOG_ERR, "replication: refused a slave from %s: %s", peer.c_str(), why.c_str());
      slave_fd.Reset();
      if (ShutdownCalled) {
        syslog(LOG_INFO, "TServer::WaitForSlave shutting down (#440)");
        return;
      }
    }
    /* This listener's one accept is consumed: release the port now, so the
       WaitForSlave() that is re-armed when this slave disconnects and we
       demote back to solo can bind it again (#500). */ {
      std::lock_guard<std::mutex> lock(SlaveSocketLock);
      SlaveSocket.Reset();
    }
    /* Hold the join while an import is running, and keep imports from
       starting while the handshake runs (#498): the importer's data files
       and sequence-number reservations do not ride the replication stream,
       so a slave may only sync against a server that is not importing. */
    for (;;) {
      /* under the gate */ {
        std::lock_guard<std::mutex> gate_lock(ImportGateMutex);
        if (!ImportInProgress) {
          SlaveHandshakeActive = true;
          break;
        }
      }
      if (ShutdownCalled) {
        syslog(LOG_INFO, "TServer::WaitForSlave shutting down (#440)");
        return;
      }
      sleep(1);
    }
    try {
      (*WaitForSlaveActionCb)(slave_fd);
    } catch (...) {
      std::lock_guard<std::mutex> gate_lock(ImportGateMutex);
      SlaveHandshakeActive = false;
      throw;
    }
    /* under the gate */ {
      std::lock_guard<std::mutex> gate_lock(ImportGateMutex);
      SlaveHandshakeActive = false;
    }
  } catch (const exception &ex) {
    if (ShutdownCalled) {
      syslog(LOG_INFO, "TServer::WaitForSlave shutting down (#440)");
      return;
    }
    syslog(LOG_ERR, "failed to wait for slave: %s", ex.what());
    throw;
  }
  DEBUG_LOG("TServer::WaitForSlave() exiting");
}

TIndyReporter::TIndyReporter(const TServer *server, TScheduler *scheduler, const string &bind_address, int port_number)
    : Server(server), Scheduler(scheduler) {
  ReportTimer.Start();
  /* open the socket */ {
    TAddress address = MakeBindAddress(bind_address, static_cast<in_port_t>(port_number));
    Socket = TFd(socket(address.GetFamily(), SOCK_STREAM, 0));
    int flag = true;
    IfLt0(setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)));
    Bind(Socket, address);
    IfLt0(listen(Socket, 5));
    syslog(LOG_INFO, "reporting listener bound to %s", DescribeBound(Socket).c_str());
  }
  Scheduler->Schedule(bind(&TIndyReporter::AcceptClientConnections, this));
}

void TIndyReporter::AcceptClientConnections() {
  for (;;) {
    int fd = accept(Socket, nullptr, nullptr);
    if (fd < 0) {
      if (Stopping) {
        syslog(LOG_INFO, "TIndyReporter::AcceptClientConnections shutting down");
        return;
      }
      ::Util::ThrowSystemError(errno);
    }
    Scheduler->Schedule(bind(&TIndyReporter::ServeClient, this, TFd(fd)));
  }
}

void TIndyReporter::Stop() {
  Stopping = true;
  shutdown(Socket, SHUT_RDWR);
}

void TIndyReporter::ServeClient(TFd &fd) {
  char buf[8192];
  for (;;) {
    IfLt0(read(fd, buf, 8192));
    stringstream ss;
    ss << "HTTP/1.1 200 OK" << endl;
    stringstream report;
    AddReport(report);
    ss << "Connection: close" << endl;
    ss << "Content-Length: " << report.str().size() << endl << endl;
    ss << report.str();
    std::streamsize num_read = ss.readsome(buf, 8192);
    while (num_read > 0) {
      IfLt0(write(fd, buf, num_read));
      num_read = ss.readsome(buf, 8192);
    }
    int ret = read(fd, buf, 8192);
    IfLt0(ret);
    if (!ret) {
      break;
    }
  }
}

void TIndyReporter::AddReport(std::stringstream &ss) const {
  const Disk::Util::TEngine *engine = !Server->Cmd.MemorySim ? Server->DiskEngine->GetEngine() : Server->SimMemEngine->GetEngine();
  #ifdef PERF_STATS
  Disk::Util::TPageCache *const page_cache = engine->GetPageCache();
  Disk::Util::TBlockCache *const block_cache = engine->GetBlockCache();
  const size_t num_buf_in_page_lru = page_cache->CountNumBufInLRU();
  const size_t max_buf_in_page_lru = page_cache->GetMaxCacheSize();
  const size_t num_buf_in_block_lru = block_cache->CountNumBufInLRU();
  const size_t max_buf_in_block_lru = block_cache->GetMaxCacheSize();
  #endif
  engine->GetVolMan()->AppendVolumeUsageReport(ss);
  /* Write admission (#590). */ {
    const Disk::Util::TSpace space = engine->GetVolMan()->GetSpace();
    const size_t claims = engine->GetVolMan()->GetRecentPeakClaims();
    const bool enabled = Server->Cmd.DiskReserveMb || Server->Cmd.DiskReservePct;
    ss << "Write Admission = " << (!enabled ? "off" : Server->RefusingWrites ? "refusing" : "accepting")
       << "; available " << space.GetAvailable() << " / " << space.Total
       << "; discard pending " << space.DiscardPending
       << "; reserve " << Server->GetDiskReserve(space.Total)
       << "; data floor " << engine->GetVolMan()->GetDataFloor()
       << "; merge claims (1 min peak) " << claims
       << "; refused " << Server->RefusedWriteCount.load() << endl;
  }
  /* The global repo's layers (#701). Every disk file is one more place a read looks, and the
     disk merges keep the count down; a count that stays high means they are behind. */ {
    size_t disk_layers, mem_layers;
    Server->CountGlobalLayers(disk_layers, mem_layers);
    ss << "Global Layers = disk " << disk_layers << "; memory " << mem_layers << endl;
  }
  /* Lines the system log daemon had no room for; stderr kept them (#641). */
  ss << "Syslog Dropped = " << Base::TLog::GetDroppedCount() << endl;
  /* The writer backlog cap (#721): the largest backlog any POV has held since startup, against
     the cap the writer backpressure holds it to, and the writes refused because a full backlog
     stopped draining. Before the Memory Admission line, which pollers read up to. */
  ss << "Writer Backlog = peak " << TRepo::GetPeakBacklogEntries() << " entries / cap " << GetWriterBacklogEntryCap()
     << "; peak " << TRepo::GetPeakBacklogUpdates() << " updates / cap " << GetWriterBacklogCap(Server->Cmd.TetrisBackpressureThreshold)
     << "; stalled refusals " << GetStalledBacklogRefusals() << endl;
  /* Memory admission (#607). */ {
    const auto &updates = TUpdate::GetUpdatePool(), &entries = TUpdate::GetEntryPool();
    ss << "Memory Admission = " << (!Server->Cmd.MemoryReservePct ? "off" : Server->RefusingWritesForMemory ? "refusing" : "accepting")
       << "; Update pool " << updates.GetNumBlocksUsed() << " / " << updates.GetMaxBlocks()
       << " reserve " << updates.GetReserve() << " admitted " << updates.GetNumBlocksAdmitted()
       << " claimed " << updates.GetNumBlocksClaimed() << " misses " << updates.GetNumMisses()
       << "; Update Entry pool " << entries.GetNumBlocksUsed() << " / " << entries.GetMaxBlocks()
       << " reserve " << entries.GetReserve() << " admitted " << entries.GetNumBlocksAdmitted()
       << " claimed " << entries.GetNumBlocksClaimed() << " misses " << entries.GetNumMisses()
       << "; refused " << Server->MemoryRefusedWriteCount.load() << endl;
  }
  size_t try_count;
  size_t try_read_count;
  size_t try_write_count;
  double
    try_read_min,
    try_read_max,
    try_read_mean,
    try_read_sigma;
  double
    try_read_cpu_min,
    try_read_cpu_max,
    try_read_cpu_mean,
    try_read_cpu_sigma;
  double
    try_write_min,
    try_write_max,
    try_write_mean,
    try_write_sigma;
  double
    try_write_cpu_min,
    try_write_cpu_max,
    try_write_cpu_mean,
    try_write_cpu_sigma;
  double
    try_walker_count_min,
    try_walker_count_max,
    try_walker_count_mean,
    try_walker_count_sigma;

  double
    try_call_cpu_time_min,
    try_call_cpu_time_max,
    try_call_cpu_time_mean,
    try_call_cpu_time_sigma;

  double
    try_read_call_time_min,
    try_read_call_time_max,
    try_read_call_time_mean,
    try_read_call_time_sigma;

  double
    try_write_call_time_min,
    try_write_call_time_max,
    try_write_call_time_mean,
    try_write_call_time_sigma;

  double
    try_walker_cons_time_min,
    try_walker_cons_time_max,
    try_walker_cons_time_mean,
    try_walker_cons_time_sigma;

  double
    try_fetch_count_min,
    try_fetch_count_max,
    try_fetch_count_mean,
    try_fetch_count_sigma;

  nanoseconds merge_disk_step_cpu;
  nanoseconds merge_mem_step_cpu;
  size_t merge_mem_count = 0UL;
  double
    merge_mem_key_min = 0.0,
    merge_mem_key_max = 0.0,
    merge_mem_key_mean = 0.0,
    merge_mem_key_sigma = 0.0;
  Server->GetRepoManager()->ReportMergeCPUTime(merge_mem_step_cpu, merge_disk_step_cpu);
  {
    std::lock_guard<std::mutex> lock(Server->GetRepoManager()->MergeMemCPULock);
    merge_mem_count = Server->GetRepoManager()->MergeMemAverageKeysCalc.Report(merge_mem_key_min, merge_mem_key_max, merge_mem_key_mean, merge_mem_key_sigma);
    Server->GetRepoManager()->MergeMemAverageKeysCalc.Reset();
  }

  size_t merge_disk_count = 0UL;
  double
    merge_disk_key_min = 0.0,
    merge_disk_key_max = 0.0,
    merge_disk_key_mean = 0.0,
    merge_disk_key_sigma = 0.0;
  {
    std::lock_guard<std::mutex> lock(Server->GetRepoManager()->MergeDiskCPULock);
    merge_disk_count = Server->GetRepoManager()->MergeDiskAverageKeysCalc.Report(merge_disk_key_min, merge_disk_key_max, merge_disk_key_mean, merge_disk_key_sigma);
    Server->GetRepoManager()->MergeDiskAverageKeysCalc.Reset();
  }

  /* TThreadLocalSigmaCalc folds across all producer threads internally, so no
     external lock is needed here. Drain() atomically reports-and-resets each
     calculator, so no concurrently-pushed sample is lost in a report/reset gap. */ {
    try_read_count = TServer::TryReadTimeCalc.Drain(try_read_min, try_read_max, try_read_mean, try_read_sigma);
    TServer::TryReadCPUTimeCalc.Drain(try_read_cpu_min, try_read_cpu_max, try_read_cpu_mean, try_read_cpu_sigma);
    try_write_count = TServer::TryWriteTimeCalc.Drain(try_write_min, try_write_max, try_write_mean, try_write_sigma);
    TServer::TryWriteCPUTimeCalc.Drain(try_write_cpu_min, try_write_cpu_max, try_write_cpu_mean, try_write_cpu_sigma);

    TServer::TryWalkerCountCalc.Drain(try_walker_count_min, try_walker_count_max, try_walker_count_mean, try_walker_count_sigma);

    TServer::TryCallCPUTimerCalc.Drain(try_call_cpu_time_min, try_call_cpu_time_max, try_call_cpu_time_mean, try_call_cpu_time_sigma);

    TServer::TryReadCallTimerCalc.Drain(try_read_call_time_min, try_read_call_time_max, try_read_call_time_mean, try_read_call_time_sigma);

    TServer::TryWriteCallTimerCalc.Drain(try_write_call_time_min, try_write_call_time_max, try_write_call_time_mean, try_write_call_time_sigma);

    TServer::TryWalkerConsTimerCalc.Drain(try_walker_cons_time_min, try_walker_cons_time_max, try_walker_cons_time_mean, try_walker_cons_time_sigma);

    TServer::TryFetchCountCalc.Drain(try_fetch_count_min, try_fetch_count_max, try_fetch_count_mean, try_fetch_count_sigma);

    try_count = try_read_count + try_write_count;
  }  // Try stats
  ReportTimer.Stop();
  double elapsed_time = ToSecondsDouble(ReportTimer.GetLap());
  #ifdef PERF_STATS
  ss << "Page LRU Buf Free = " << num_buf_in_page_lru << " / " << max_buf_in_page_lru << endl;
  ss << "Block LRU Buf Free = " << num_buf_in_block_lru << " / " << max_buf_in_block_lru << endl;
  #endif

  ss << "Durable Mapping Pool = " << Disk::TDurableManager::TMapping::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.DurableMappingPoolSize << endl;
  ss << "Durable Mapping Entry Pool = " << Disk::TDurableManager::TMapping::TEntry::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.DurableMappingEntryPoolSize << endl;
  ss << "Durable Layer Pool = " << Disk::TDurableManager::TDurableLayer::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.DurableLayerPoolSize << endl;
  ss << "Durable Mem Entry Pool = " << Disk::TDurableManager::TMemSlushLayer::TDurableEntry::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.DurableMemEntryPoolSize << endl;

  ss << "Repo Mapping Pool = " << TRepo::TMapping::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.RepoMappingPoolSize << endl;
  ss << "Repo Mapping Entry Pool = " << TRepo::TMapping::TEntry::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.RepoMappingEntryPoolSize << endl;
  ss << "Repo Data Layer Pool = " << TRepo::TDataLayer::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.RepoDataLayerPoolSize << endl;

  ss << "Transaction Mutation Pool = " << L1::TTransaction::TMutation::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.TransactionMutationPoolSize << endl;
  ss << "Transaction Pool = " << L1::TTransaction::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.TransactionPoolSize << endl;

  ss << "Update Pool = " << TUpdate::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.UpdatePoolSize << endl;
  ss << "Update Entry Pool = " << TUpdate::TEntry::Pool.GetNumBlocksUsed() << " / " << Server->Cmd.UpdateEntryPoolSize << endl;

  ss << "Try Count / s = " << (try_count / elapsed_time) << endl;
  ss << "Try Read Count / s = " << (try_read_count / elapsed_time) << endl;
  ss << "Try Write Count / s = " << (try_write_count / elapsed_time) << endl;

  ss << "MergeMem Step CPU (s) = " << ToSecondsDouble(merge_mem_step_cpu) / elapsed_time << endl;

  ss << "MergeDisk Step CPU (s) = " << ToSecondsDouble(merge_disk_step_cpu) / elapsed_time << endl;

  size_t tetris_push_count = Server->TetrisManager->PushCount.exchange(0UL);
  size_t tetris_pop_count = Server->TetrisManager->PopCount.exchange(0UL);
  size_t tetris_fail_count = Server->TetrisManager->FailCount.exchange(0UL);
  size_t tetris_round_count = Server->TetrisManager->RoundCount.exchange(0UL);
  size_t tetris_children_considered = Server->TetrisManager->ChildrenConsideredCount.exchange(0UL);
  ss << "Tetris Push Transactions / s = " << (tetris_push_count / elapsed_time) << endl;
  ss << "Tetris Pop Transactions / s = " << (tetris_pop_count / elapsed_time) << endl;
  ss << "Tetris Fail Transactions / s = " << (tetris_fail_count / elapsed_time) << endl;
  ss << "Tetris Rounds / s = " << (tetris_round_count / elapsed_time) << endl;
  /* Children re-snapshotted per promotion: ~1 means the merge keeps up; a large
     and growing ratio is the #234 O(N^2) re-snapshot backlog. */
  ss << "Tetris Children Considered / s = " << (tetris_children_considered / elapsed_time) << endl;
  ss << "Tetris Children Considered / Push = "
     << (tetris_push_count ? (static_cast<double>(tetris_children_considered) / tetris_push_count) : 0.0) << endl;

  size_t tetris_timer_count = 0UL;
  double
    tetris_snapshot_min = 0.0,
    tetris_snapshot_max = 0.0,
    tetris_snapshot_mean = 0.0,
    tetris_snapshot_sigma = 0.0;
  double
    tetris_sort_min = 0.0,
    tetris_sort_max = 0.0,
    tetris_sort_mean = 0.0,
    tetris_sort_sigma = 0.0;
  double
    tetris_play_min = 0.0,
    tetris_play_max = 0.0,
    tetris_play_mean = 0.0,
    tetris_play_sigma = 0.0;
  double
    tetris_commit_min = 0.0,
    tetris_commit_max = 0.0,
    tetris_commit_mean = 0.0,
    tetris_commit_sigma = 0.0;

  {
    std::lock_guard<std::mutex> tetris_timer_lock(Server->TetrisManager->TetrisTimerLock);
    tetris_timer_count = Server->TetrisManager->TetrisSnapshotCPUTime.Report(tetris_snapshot_min, tetris_snapshot_max, tetris_snapshot_mean, tetris_snapshot_sigma);
    Server->TetrisManager->TetrisSortCPUTime.Report(tetris_sort_min, tetris_sort_max, tetris_sort_mean, tetris_sort_sigma);
    Server->TetrisManager->TetrisPlayCPUTime.Report(tetris_play_min, tetris_play_max, tetris_play_mean, tetris_play_sigma);
    Server->TetrisManager->TetrisCommitCPUTime.Report(tetris_commit_min, tetris_commit_max, tetris_commit_mean, tetris_commit_sigma);
    Server->TetrisManager->TetrisSnapshotCPUTime.Reset();
    Server->TetrisManager->TetrisSortCPUTime.Reset();
    Server->TetrisManager->TetrisPlayCPUTime.Reset();
    Server->TetrisManager->TetrisCommitCPUTime.Reset();
  }

  if (tetris_timer_count) {
    ss << "Tetris Snapshot CPU / s = " << ((tetris_timer_count * tetris_snapshot_mean) / elapsed_time) << endl
       << "Tetris Snapshot CPU Min = " << tetris_snapshot_min << endl
       << "Tetris Snapshot CPU Max = " << tetris_snapshot_max << endl
       << "Tetris Snapshot CPU Mean = " << tetris_snapshot_mean << endl;
    ss << "Tetris Sort CPU / s = " << ((tetris_timer_count * tetris_sort_mean) / elapsed_time) << endl
       << "Tetris Sort CPU Min = " << tetris_sort_min << endl
       << "Tetris Sort CPU Max = " << tetris_sort_max << endl
       << "Tetris Sort CPU Mean = " << tetris_sort_mean << endl;
    ss << "Tetris Play CPU / s = " << ((tetris_timer_count * tetris_play_mean) / elapsed_time) << endl
       << "Tetris Play CPU Min = " << tetris_play_min << endl
       << "Tetris Play CPU Max = " << tetris_play_max << endl
       << "Tetris Play CPU Mean = " << tetris_play_mean << endl;
    ss << "Tetris Commit CPU / s = " << ((tetris_timer_count * tetris_commit_mean) / elapsed_time) << endl
       << "Tetris Commit CPU Min = " << tetris_commit_min << endl
       << "Tetris Commit CPU Max = " << tetris_commit_max << endl
       << "Tetris Commit CPU Mean = " << tetris_commit_mean << endl;
  }


  if (merge_mem_count) {
    ss << "Merge Mem Keys Count = " << merge_mem_count << endl
       << "Merge Mem Keys Min = " << merge_mem_key_min << endl
       << "Merge Mem Keys Max = " << merge_mem_key_max << endl
       << "Merge Mem Keys Mean = " << merge_mem_key_mean << endl;
  }
  if (merge_disk_count) {
    ss << "Merge Disk Keys Count = " << merge_disk_count << endl
       << "Merge Disk Keys Min = " << merge_disk_key_min << endl
       << "Merge Disk Keys Max = " << merge_disk_key_max << endl
       << "Merge Disk Keys Mean = " << merge_disk_key_mean << endl;
  }
if (try_read_count) {
    ss << "Try Read Time Min = " << try_read_min << endl
       << "Try Read Time Max = " << try_read_max << endl
       << "Try Read Time Mean = " << try_read_mean << endl;

    ss << "Try Read CPU Time Min = " << try_read_cpu_min << endl
       << "Try Read CPU Time Max = " << try_read_cpu_max << endl
       << "Try Read CPU Time Mean = " << try_read_cpu_mean << endl;
  }
  if (try_write_count) {
    ss << "Try Write Time Min = " << try_write_min << endl
       << "Try Write Time Max = " << try_write_max << endl
       << "Try Write Time Mean = " << try_write_mean << endl;
  }
  if (try_count) {
    ss << "Try Walker Count Min = " << try_walker_count_min << endl
       << "Try Walker Count Max = " << try_walker_count_max << endl
       << "Try Walker Count Mean = " << try_walker_count_mean << endl;

    ss << "Try Read Call Time Min = " << try_read_call_time_min << endl
       << "Try Read Call Time Max = " << try_read_call_time_max << endl
       << "Try Read Call Time Mean = " << try_read_call_time_mean << endl;

    ss << "Try Write Call Time Min = " << try_write_call_time_min << endl
       << "Try Write Call Time Max = " << try_write_call_time_max << endl
       << "Try Write Call Time Mean = " << try_write_call_time_mean << endl;

    ss << "Try Walker Cons Time Min = " << try_walker_cons_time_min << endl
       << "Try Walker Cons Time Max = " << try_walker_cons_time_max << endl
       << "Try Walker Cons Time Mean = " << try_walker_cons_time_mean << endl;
    }
  if (Server->DiskEngine) {
    Server->DiskEngine->Report(ss, elapsed_time);
  }
}

TServer::TConnection::TConnectionRunnable::TConnectionRunnable(Fiber::TRunner *runner, const std::shared_ptr<const Rpc::TAnyRequest> &request)
    : Request(request) {
  FramePool = Fiber::TFrame::LocalFramePool;
  Frame = FramePool->Alloc();
  try {
    Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TConnectionRunnable::Compute));
  } catch (...) {
    FramePool->Free(Frame);
    throw;
  }
}

TServer::TConnection::TConnectionRunnable::~TConnectionRunnable() {
  //printf("TConnectionRunnable free frame[%p]\n", Frame);
  Fiber::FreeMyFrame(FramePool);
  //FramePool->Free(Frame);
}

void TServer::TConnection::TConnectionRunnable::Compute() {
  assert(Fiber::TFrame::LocalFrame == Frame);
  (*Request)();
  delete this;
  //printf("~TConnectionRunnable finish\n");
}
