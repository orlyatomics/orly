/* <orly/server/import_replication.test.cc>

   End-to-end master/slave coverage for the bulk-import + replication fixes:

     #367 — an index id minted by the bulk importer must reach a slave.
     #497 — a single-file import must not hand uninitialized sequence
            numbers to AddFileToRepo (it used to crash the server).
     #498 — imports are a solo-server operation: a master with a live slave
            must refuse BeginImport instead of desynchronizing sequence
            numbers and killing the slave on the next replicated write.
     #499 — the system-repo commits an install replicates must not abort
            the master's replication notification pass (their void metadata
            used to unwind it, eating the batch's remaining client acks).
     #500 — a slave disconnect must demote the master back to solo and leave
            it able to accept a new slave (the demote path used to die
            re-binding the slave port).

   The scenario, with forked orlyi processes on --mem_sim:

     1. start a SOLO master; import core-vector files whose types match no
        installed package — a two-file import (the second file's stream id
        remaps onto the first's) plus a single-file import (#497's no-merge
        path) — minting one fresh index id;
     2. install the package on the master: it adopts the minted id and all
        three imported rows are readable;
     3. attach slave 1: the join-time sync must deliver the imported files
        AND the index-id mapping (the slave logs "Replicating index [...]"
        as it applies the mapping — the #367 assertion);
     4. install a second package on the paired master: its index id must
        reach the slave on the live replication stream (#367's enqueue), the
        master's log must stay free of notification-pass errors, and a
        write's replication ack must still arrive (#499);
     5. with the slave attached, BeginImport on the master must fail with a
        clean error, and the master must remain healthy (#498);
     6. write a row through the package and wait for the slave to
        acknowledge the replication;
     7. kill slave 1: the master must demote to solo, stay alive, and accept
        slave 2 (#500), which again receives files and mapping;
     8. write another row (live replication to slave 2), then kill the
        master: slave 2 promotes itself to solo;
     9. read every row on the promoted slave 2: the imported rows and the
        pre-join write arrived as synced data files (#501 pinned them
        unreadable), the last write arrived on the live stream, and all of
        them are reachable only because the package bound to the imported
        index id (#367).

   The kills in this scenario are SIGKILL, deliberately: the slave promotes
   on connection DEATH, and a hard kill is what delivers that
   deterministically.  Graceful shutdown while paired — including with an
   UNRESPONSIVE peer — is pinned separately by the
   GracefulShutdownUnresponsiveSlave fixture below (#461).

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

#include <alloca.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <base/io/binary_output_only_stream.h>
#include <base/io/device.h>
#include <base/util/error.h>
#include <base/zero.h>
#include <orly/atom/core_vector_builder.h>
#include <orly/client/client.h>
#include <orly/compiler.h>
#include <orly/protocol.h>
#include <orly/rt/opt.h>
#include <orly/sabot/to_native.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Socket;
using namespace Orly;
using namespace Orly::Client;

namespace {

/* Matches Orly::Indy::GlobalPovId (orly/indy/manager.h), which is too heavy
   an include for this test. */
const Base::TUuid GlobalPovId("C4EF7C46-28C5-4000-8CCD-C8E799E2C3F3");

/* The package under test: one index, keyed <[str, int]> with int values. */
const char *SamplePackage =
    "package #1;\n"
    "read_val = (*<['values', n]>::(int?)) where { n = given::(int); };\n"
    "write_val = ((true) effecting { new <['values', n]> <- x; } ) where {\n"
    "  n = given::(int);\n"
    "  x = given::(int);\n"
    "};\n";

/* A second package, installed while the master is paired: its index id is
   minted at install time and must travel the LIVE replication stream. */
const char *Sample2Package =
    "package #1;\n"
    "read_val2 = (*<['values', n]>::(int?)) where { n = given::(int); };\n"
    "write_val2 = ((true) effecting { new <['values', n]> <- x; } ) where {\n"
    "  n = given::(int);\n"
    "  x = given::(int);\n"
    "};\n";

/* Grab a free TCP port from the kernel.  The port is released again before
   the server binds it, so a parallel test could steal it; the window is
   tiny and a collision just fails this run's startup wait. */
in_port_t ProbeFreePort() {
  TFd sock(socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in addr;
  Base::Zero(addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Util::IfLt0(::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
  socklen_t len = sizeof(addr);
  Util::IfLt0(getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len));
  return ntohs(addr.sin_port);
}

bool CanConnect(in_port_t port) {
  TFd sock(socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in addr;
  Base::Zero(addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
}

bool WaitForPort(in_port_t port, seconds deadline) {
  const auto give_up = steady_clock::now() + deadline;
  while (steady_clock::now() < give_up) {
    if (CanConnect(port)) {
      return true;
    }
    this_thread::sleep_for(milliseconds(250));
  }
  return false;
}

bool LogContains(const string &path, const string &needle) {
  ifstream strm(path);
  string line;
  while (getline(strm, line)) {
    if (line.find(needle) != string::npos) {
      return true;
    }
  }
  return false;
}

/* Occurrences of needle in the log so far. */
size_t CountInLog(const string &path, const string &needle) {
  ifstream strm(path);
  string line;
  size_t count = 0;
  while (getline(strm, line)) {
    if (line.find(needle) != string::npos) {
      ++count;
    }
  }
  return count;
}

bool WaitForLog(const string &path, const string &needle, seconds deadline) {
  const auto give_up = steady_clock::now() + deadline;
  while (steady_clock::now() < give_up) {
    if (LogContains(path, needle)) {
      return true;
    }
    this_thread::sleep_for(milliseconds(250));
  }
  return false;
}

/* A forked orlyi.  Kills and reaps the child on destruction. */
class TChildServer final {
  NO_COPY(TChildServer);
  public:

  TChildServer(const vector<string> &args, const string &log_path)
      : Pid(fork()) {
    Util::IfLt0(Pid);
    if (!Pid) {
      int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
      }
      vector<const char *> argv;
      argv.reserve(args.size() + 1);
      for (const auto &arg : args) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);
      execv(argv[0], const_cast<char **>(argv.data()));
      _exit(127);
    }
  }

  ~TChildServer() {
    Interrupt();
    if (Pid > 0 && !Reap(seconds(10))) {
      kill(Pid, SIGKILL);
      Reap(seconds(10));
    }
  }

  bool IsAlive() const {
    return Pid > 0 && kill(Pid, 0) == 0;
  }

  void Interrupt() {
    if (Pid > 0) {
      kill(Pid, SIGINT);
    }
  }

  void Kill() {
    if (Pid > 0) {
      kill(Pid, SIGKILL);
    }
  }

  /* SIGSTOP: the process freezes but its sockets stay open -- the shape of
     an UNRESPONSIVE peer, as opposed to Kill()'s vanished one. */
  void Suspend() {
    if (Pid > 0) {
      kill(Pid, SIGSTOP);
    }
  }

  /* One-line status of the child for failure messages: distinguishes a
     frozen-but-alive child (state T) from one an assert or the kernel's OOM
     killer took -- the difference between a fixture bug and a dead server
     (#520). */
  string Describe() {
    if (Pid <= 0) {
      return "already reaped";
    }
    int status;
    pid_t ret = waitpid(Pid, &status, WNOHANG);
    if (ret == Pid) {
      Pid = -1;
      ostringstream strm;
      if (WIFEXITED(status)) {
        strm << "exited with status " << WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        strm << "killed by signal " << WTERMSIG(status);
      } else {
        strm << "reaped with raw status " << status;
      }
      return strm.str();
    }
    ifstream stat_strm("/proc/" + to_string(Pid) + "/stat");
    string stat_line;
    getline(stat_strm, stat_line);
    const auto close_paren = stat_line.rfind(')');
    const string state = (close_paren != string::npos && close_paren + 2 < stat_line.size())
        ? string(1, stat_line[close_paren + 2]) : string("?");
    return "alive, state [" + state + "]";
  }

  /* Wait for the child to exit; true if it did. */
  bool Reap(seconds deadline) {
    if (Pid <= 0) {
      return true;
    }
    const auto give_up = steady_clock::now() + deadline;
    for (;;) {
      int status;
      pid_t ret = waitpid(Pid, &status, WNOHANG);
      if (ret == Pid || (ret < 0 && errno == ECHILD)) {
        Pid = -1;
        return true;
      }
      if (steady_clock::now() >= give_up) {
        return false;
      }
      this_thread::sleep_for(milliseconds(250));
    }
  }

  private:

  pid_t Pid;

};  // TChildServer

/* Client that records replication acknowledgements so the test can wait for
   an update to reach the slave. */
class TExerciseClient final
    : public TClient {
  public:

  TExerciseClient(const TAddress &addr)
      : TClient(addr, std::nullopt, seconds(600)) {}

  /* True once the slave has acknowledged the update; false on deadline. */
  bool WaitForReplication(const Base::TUuid &tracker_id, const Base::TUuid &repo_id, seconds deadline) {
    std::unique_lock<std::mutex> lock(ReplicationMutex);
    auto search_pair = make_pair(tracker_id, repo_id);
    return ReplicationCond.wait_for(lock, deadline, [this, &search_pair] {
      return ReplicatedUpdateToRepo.find(search_pair) != ReplicatedUpdateToRepo.end();
    });
  }

  private:

  virtual void OnPovFailed(const Base::TUuid &/*repo_id*/) override {}

  virtual void OnUpdateAccepted(const Base::TUuid &/*repo_id*/, const Base::TUuid &/*tracking_id*/) override {}

  virtual void OnUpdateReplicated(const Base::TUuid &repo_id, const Base::TUuid &tracking_id) override {
    std::lock_guard<std::mutex> lock(ReplicationMutex);
    ReplicatedUpdateToRepo.insert(make_pair(tracking_id, repo_id));
    ReplicationCond.notify_all();
  }

  virtual void OnUpdateDurable(const Base::TUuid &/*repo_id*/, const Base::TUuid &/*tracking_id*/) override {}

  virtual void OnUpdateSemiDurable(const Base::TUuid &/*repo_id*/, const Base::TUuid &/*tracking_id*/) override {}

  std::set<std::pair<Base::TUuid, Base::TUuid>> ReplicatedUpdateToRepo;
  std::mutex ReplicationMutex;
  std::condition_variable ReplicationCond;

};  // TExerciseClient

/* Write a one-row core-vector import file: <['values', n]> <- x, under a
   random index id that only the importing server will know about. */
void WriteImportFile(const string &path, int64_t n, int64_t x) {
  Atom::TCoreVectorBuilder builder;
  builder.Push(1L);  // num transactions
  builder.Push(0L);  // dummy metadata
  builder.Push(TUuid(TUuid::Twister));  // tx id
  builder.Push(1L);  // tx metadata
  builder.Push(1L);  // num kv pairs
  builder.Push(TUuid(TUuid::Twister));  // the index id to be minted
  builder.Push(make_tuple(string("values"), n));
  builder.Push(x);
  Io::TBinaryOutputOnlyStream strm(
      make_shared<Io::TDevice>(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)));
  builder.Write(strm);
}

vector<string> MakeServerArgs(const string &orlyi_path,
                              const string &instance_name,
                              const string &pkg_dir,
                              in_port_t port,
                              in_port_t slave_port,
                              const string &starting_state,
                              in_port_t master_slave_port) {
  vector<string> args = {
      orlyi_path,
      "--mem_sim",
      "--mem_sim_mb=64",
      "--mem_sim_slow_mb=32",
      "--create=true",
      "--instance_name=" + instance_name,
      "--starting_state=" + starting_state,
      "--port_number=" + to_string(port),
      "--slave_port_number=" + to_string(slave_port),
      "--ws_port_number=" + to_string(ProbeFreePort()),
      "--reporting_port_number=" + to_string(ProbeFreePort()),
      "--connection_backlog=10",
      "--package_dir=" + pkg_dir,
      "--max_parallel_frames=4000",
      "--page_cache_size=256",
      "--block_cache_size=64",
      "--le",
      "--log_info",
      "--log_notice",
      "--log_warning"};
  if (starting_state == "SLAVE") {
    args.push_back("--address_of_master=127.0.0.1:" + to_string(master_slave_port));
  }
  return args;
}

/* read_val(n) via the given client; unknown if the key is absent. */
Rt::TOpt<int64_t> ReadVal(const shared_ptr<TExerciseClient> &client, const Base::TUuid &pov_id, int64_t n) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto read_result = client->Try(pov_id, { "sample" }, TClosure(string("read_val"),
                                                                string("n"), n));
  Rt::TOpt<int64_t> out;
  Sabot::ToNative(*Sabot::State::TAny::TWrapper((*read_result)->GetValue().NewState((*read_result)->GetArena().get(), state_alloc)), out);
  return out;
}

/* write_val(n, x) via the given client and wait for the slave to
   acknowledge the replication; false on deadline. */
bool WriteValReplicated(const shared_ptr<TExerciseClient> &client, const Base::TUuid &pov_id, int64_t n, int64_t x) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto push_result = client->Try(pov_id, { "sample" }, TClosure(string("write_val"),
                                                                string("n"), n,
                                                                string("x"), x));
  bool committed;
  Sabot::ToNative(*Sabot::State::TAny::TWrapper((*push_result)->GetValue().NewState((*push_result)->GetArena().get(), state_alloc)), committed);
  if (!committed) {
    return false;
  }
  const std::optional<TTracker> tracker = (*push_result)->GetTracker();
  if (!tracker) {
    return false;
  }
  return client->WaitForReplication(tracker->Id, pov_id, seconds(60)) &&
         client->WaitForReplication(tracker->Id, GlobalPovId, seconds(60));
}

/* read_val(n) against the given server with retries, for reads against a
   slave that is still promoting or catching up.  Returns the value once a
   successful RPC yields a known value; unknown only after the deadline. */
Rt::TOpt<int64_t> ReadWithRetry(const TAddress &addr, int64_t n, seconds deadline) {
  const auto give_up = steady_clock::now() + deadline;
  for (;;) {
    try {
      auto client = make_shared<TExerciseClient>(addr);
      auto pov_id = client->NewFastPrivatePov(std::nullopt, seconds(0));
      Rt::TOpt<int64_t> out = ReadVal(client, **pov_id, n);
      if (out.IsKnown()) {
        return out;
      }
    } catch (const exception &ex) {
      cout << "read_val(" << n << ") retry after [" << ex.what() << "]" << endl;
    }
    if (steady_clock::now() >= give_up) {
      return Rt::TOpt<int64_t>();
    }
    this_thread::sleep_for(seconds(1));
  }
}

/* Print each server log's tail at fixture exit: the logs live in a temp dir
   that CI never uploads, and a dead child's last lines are the only way to
   tell a crash from an OOM kill from a stall. */
class TLogTailDumper final {
  NO_COPY(TLogTailDumper);
  public:

  TLogTailDumper() {}

  ~TLogTailDumper() {
    for (const auto &path : Paths) {
      cout << "==== tail of " << path << " ====" << endl;
      ifstream strm(path);
      deque<string> tail;
      string line;
      while (getline(strm, line)) {
        tail.push_back(line);
        if (tail.size() > 60) {
          tail.pop_front();
        }
      }
      for (const auto &kept : tail) {
        cout << kept << endl;
      }
    }
  }

  void Add(const string &path) {
    Paths.push_back(path);
  }

  private:

  vector<string> Paths;

};  // TLogTailDumper

string GetScratchDir() {
  char tmpl[] = "/tmp/import_repl_test_XXXXXX";
  const char *dir = mkdtemp(tmpl);
  if (!dir) {
    throw runtime_error("mkdtemp failed");
  }
  return string(dir);
}

string GetOrlyiPath() {
  ostringstream strm;
  strm << SRC_ROOT << "/../out_orly/";
  #ifndef NDEBUG
  strm << "debug";
  #else
  strm << "release";
  #endif
  strm << "/orly/server/orlyi";
  return strm.str();
}

}  // anonymous namespace

FIXTURE(ImportReplication) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = GetScratchDir();
  const string orlyi_path = GetOrlyiPath();
  TLogTailDumper log_dumper;
  if (!ifstream(orlyi_path).good()) {
    throw runtime_error("orlyi binary not built at [" + orlyi_path + "]; run `make debug` first");
  }

  /* Compile the sample package and set up a shared package dir. */
  const string pkg_dir = scratch + "/packages";
  Util::IfLt0(mkdir(pkg_dir.c_str(), 0755));
  { ofstream marker(pkg_dir + "/__orly__"); }
  {
    ofstream src(scratch + "/sample.orly");
    src << SamplePackage;
  }
  Compiler::Compile(TPath(scratch + "/sample.orly"), Jhm::TTree(pkg_dir), {});
  {
    ofstream src(scratch + "/sample2.orly");
    src << Sample2Package;
  }
  Compiler::Compile(TPath(scratch + "/sample2.orly"), Jhm::TTree(pkg_dir), {});

  /* The import files whose rows will mint a fresh index id on the master. */
  WriteImportFile(scratch + "/import.0.bin", 42L, 4242L);
  WriteImportFile(scratch + "/import.1.bin", 43L, 4343L);
  const string import_pattern = scratch + "/import.*.bin";
  /* A lone third file, imported by itself: the no-merge path (#497). */
  WriteImportFile(scratch + "/single.bin", 44L, 4444L);

  /* Launch the master (solo) and wait for it to accept connections. */
  const in_port_t master_port = ProbeFreePort();
  const in_port_t master_slave_port = ProbeFreePort();
  const string master_log = scratch + "/master.log";
  log_dumper.Add(master_log);
  TChildServer master(
      MakeServerArgs(orlyi_path, "import_repl_master", pkg_dir, master_port,
                     master_slave_port, "SOLO", 0),
      master_log);
  if (!WaitForPort(master_port, seconds(240))) {
    throw runtime_error("master never came up; see " + master_log);
  }
  const TAddress master_addr(TAddress::IPv4Loopback, master_port);

  /* Import while solo — the only legal time (#498).  The importer mints the
     index id itself: this mapping is what must later reach the slaves. */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    client->BeginImport()->Sync();
    /* merge_simultaneous must exceed the file count divided by the merge
       rounds you can afford: a value of 1 merges each file into itself and
       the merge loop never converges. */
    client->ImportCoreVector(import_pattern, "sample", 1, 1, 4)->Sync();
    /* A single-file import skips the merge phase entirely; it used to hand
       uninitialized sequence numbers to AddFileToRepo (#497). */
    client->ImportCoreVector(scratch + "/single.bin", "sample", 1, 1, 4)->Sync();
    client->EndImport()->Sync();

    /* Install the package: it must adopt the importer's index id, making
       all three imported rows readable. */
    client->InstallPackage({ "sample" }, 1)->Sync();
    auto pov_id = client->NewFastPrivatePov(std::nullopt, seconds(0));
    Rt::TOpt<int64_t> row_42 = ReadVal(client, **pov_id, 42L);
    EXPECT_TRUE(row_42.IsKnown());
    if (row_42.IsKnown()) {
      EXPECT_EQ(row_42.GetVal(), 4242L);
    }
    /* The second file's row: its stream carried a different random index id
       that the importer remapped onto the minted one. */
    Rt::TOpt<int64_t> row_43 = ReadVal(client, **pov_id, 43L);
    EXPECT_TRUE(row_43.IsKnown());
    if (row_43.IsKnown()) {
      EXPECT_EQ(row_43.GetVal(), 4343L);
    }
    /* The single-file import's row (#497). */
    Rt::TOpt<int64_t> row_44 = ReadVal(client, **pov_id, 44L);
    EXPECT_TRUE(row_44.IsKnown());
    if (row_44.IsKnown()) {
      EXPECT_EQ(row_44.GetVal(), 4444L);
    }
  }

  /* Attach slave 1.  The join-time sync must deliver the imported data
     files (inventory sync) and the index-id mapping, which the slave logs
     as it applies it (#367). */
  const in_port_t slave_1_port = ProbeFreePort();
  const string slave_1_log = scratch + "/slave_1.log";
  log_dumper.Add(slave_1_log);
  TChildServer slave_1(
      MakeServerArgs(orlyi_path, "import_repl_slave_1", pkg_dir, slave_1_port,
                     ProbeFreePort(), "SLAVE", master_slave_port),
      slave_1_log);
  if (!WaitForPort(slave_1_port, seconds(240))) {
    throw runtime_error("slave 1 never came up; see " + slave_1_log);
  }
  if (!WaitForLog(slave_1_log, "to [Slave]", seconds(120))) {
    throw runtime_error("slave 1 never reached Slave state; see " + slave_1_log);
  }
  EXPECT_TRUE(WaitForLog(slave_1_log, "Replicating index [", seconds(60)));
  EXPECT_TRUE(LogContains(slave_1_log, "sample tuple(str, int64)"));

  /* Install a second package on the PAIRED master: its index id is minted
     now, so it must reach the slave on the live replication stream (the
     enqueue side of #367 -- the first package's mapping traveled with the
     join instead), and the system-repo commits the install replicates must
     not disrupt the notification pass (#499). */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    client->InstallPackage({ "sample2" }, 1)->Sync();
    EXPECT_TRUE(WaitForLog(slave_1_log, "sample2 tuple(str, int64)", seconds(120)));
    EXPECT_TRUE(!LogContains(master_log, "RunReplicateTransaction error"));
    /* A write's replication ack must survive sharing the stream with the
       install's system-repo commits (#499 ate the rest of the batch's
       notifications). */
    auto pov_id = client->NewFastPrivatePov(std::nullopt, seconds(0));
    EXPECT_TRUE(WriteValReplicated(client, **pov_id, 9L, 909L));
  }

  /* With a slave attached, an import must be refused up front (#498), and
     the refusal must leave the master healthy. */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    bool import_refused = false;
    try {
      client->BeginImport()->Sync();
    } catch (const exception &/*ex*/) {
      import_refused = true;
    }
    EXPECT_TRUE(import_refused);

    /* Master still healthy: install on the slave, write through the master,
       and wait for the slave to acknowledge the replication. */
    auto slave_client = make_shared<TExerciseClient>(TAddress(TAddress::IPv4Loopback, slave_1_port));
    slave_client->InstallPackage({ "sample" }, 1)->Sync();
    auto pov_id = client->NewFastPrivatePov(std::nullopt, seconds(0));
    EXPECT_TRUE(WriteValReplicated(client, **pov_id, 7L, 707L));
  }

  /* Kill slave 1: the master must demote back to solo, stay alive, and
     re-arm for a new slave (#500).  The pause first lets the just-destroyed
     clients' dispatch threads wind down before their server goes away. */
  this_thread::sleep_for(seconds(2));
  cout << "Stopping slave 1" << endl;
  /* SIGKILL, not SIGINT: what this test needs is the master's reaction to
     the slave VANISHING, which a hard kill delivers deterministically (a
     graceful slave stop demotes the master through the same path but on the
     slave's schedule). */
  slave_1.Kill();
  slave_1.Reap(seconds(60));
  if (!WaitForLog(master_log, "demoted back to solo", seconds(120))) {
    throw runtime_error("master never demoted after slave 1 died; see " + master_log);
  }
  EXPECT_TRUE(master.IsAlive());

  /* Attach slave 2: the re-armed master must accept it (#500), and the
     join must again deliver files and mapping. */
  const in_port_t slave_2_port = ProbeFreePort();
  const string slave_2_log = scratch + "/slave_2.log";
  log_dumper.Add(slave_2_log);
  TChildServer slave_2(
      MakeServerArgs(orlyi_path, "import_repl_slave_2", pkg_dir, slave_2_port,
                     ProbeFreePort(), "SLAVE", master_slave_port),
      slave_2_log);
  if (!WaitForPort(slave_2_port, seconds(240))) {
    throw runtime_error("slave 2 never came up; see " + slave_2_log);
  }
  if (!WaitForLog(slave_2_log, "to [Slave]", seconds(120))) {
    throw runtime_error("slave 2 never reached Slave state; see " + slave_2_log);
  }
  EXPECT_TRUE(WaitForLog(slave_2_log, "Replicating index [", seconds(60)));

  /* Install on slave 2, then push one more row through the master so the
     live stream to slave 2 is exercised too. */ {
    auto slave_client = make_shared<TExerciseClient>(TAddress(TAddress::IPv4Loopback, slave_2_port));
    slave_client->InstallPackage({ "sample" }, 1)->Sync();
    auto client = make_shared<TExerciseClient>(master_addr);
    auto pov_id = client->NewFastPrivatePov(std::nullopt, seconds(0));
    EXPECT_TRUE(WriteValReplicated(client, **pov_id, 8L, 808L));
  }

  /* Both servers must have survived the whole exercise.  Reading the
     replicated rows back on a promoted slave 2 is what SHOULD close this
     out; it waits on #501 (see the header comment). */
  EXPECT_TRUE(master.IsAlive());
  EXPECT_TRUE(slave_2.IsAlive());

  /* Fail over: kill the master; slave 2 promotes itself to solo.  SIGKILL,
     because the slave promotes on connection DEATH; a graceful master
     shutdown demotes itself first and the slave never takes the promotion
     path (graceful paired shutdown is the other fixture's to pin, #461). */
  this_thread::sleep_for(seconds(2));
  cout << "Stopping the master" << endl;
  master.Kill();
  master.Reap(seconds(60));
  if (!WaitForLog(slave_2_log, "slave promoted to solo", seconds(120))) {
    throw runtime_error("slave 2 never promoted; see " + slave_2_log);
  }

  /* Every row must be readable on the promoted slave: the imported rows and
     the pre-join write prove the synced data files are readable (#501, #367),
     and the last write proves the live stream to a post-demote slave (#500).
     The first read absorbs whatever promotion work remains. */
  const int64_t expected_by_key[][2] = {{42L, 4242L}, {43L, 4343L}, {44L, 4444L}, {9L, 909L}, {7L, 707L}, {8L, 808L}};
  seconds deadline = seconds(120);
  for (const auto &pair : expected_by_key) {
    Rt::TOpt<int64_t> row = ReadWithRetry(TAddress(TAddress::IPv4Loopback, slave_2_port), pair[0], deadline);
    EXPECT_TRUE(row.IsKnown());
    if (row.IsKnown()) {
      EXPECT_EQ(row.GetVal(), pair[1]);
    }
    deadline = seconds(30);
  }
}

/* #461: graceful shutdown of a PAIRED master whose slave has gone
   unresponsive.  SIGSTOP keeps the slave's replication socket open but the
   process silent: the master's replicate loop writes its PushNotifications
   RPC into the socket (the fresh RPC-write log line is the evidence it got
   there) and parks in future->Sync() with no answer ever coming.  A
   graceful SIGINT must still complete: StopReplicationServices() hard-closes
   the replication socket, which fails the parked future and collapses the
   reader loop, so JoinReplicationServices() returns.  Before that fix this
   fixture hung at Reap() and failed on the deadline.

   The fixture also holds an ESTABLISHED client connection (a raw handshake,
   then silence) across the shutdown: the drain must hard-close it so its
   TConnection releases the session's durable ptr before Clear() runs,
   instead of Clear() logging-and-leaking the still-ptr'd session (#460). */
FIXTURE(GracefulShutdownUnresponsiveSlave) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = GetScratchDir();
  const string orlyi_path = GetOrlyiPath();
  TLogTailDumper log_dumper;
  if (!ifstream(orlyi_path).good()) {
    throw runtime_error("orlyi binary not built at [" + orlyi_path + "]; run `make debug` first");
  }

  /* One package, installed while solo (like the main fixture); all this
     fixture needs is one write to push through the replication stream once
     the slave has stopped answering. */
  const string pkg_dir = scratch + "/packages";
  Util::IfLt0(mkdir(pkg_dir.c_str(), 0755));
  { ofstream marker(pkg_dir + "/__orly__"); }
  {
    ofstream src(scratch + "/sample.orly");
    src << SamplePackage;
  }
  Compiler::Compile(TPath(scratch + "/sample.orly"), Jhm::TTree(pkg_dir), {});

  const in_port_t master_port = ProbeFreePort();
  const in_port_t master_slave_port = ProbeFreePort();
  const string master_log = scratch + "/master.log";
  log_dumper.Add(master_log);
  TChildServer master(
      MakeServerArgs(orlyi_path, "graceful_master", pkg_dir, master_port,
                     master_slave_port, "SOLO", 0),
      master_log);
  if (!WaitForPort(master_port, seconds(240))) {
    throw runtime_error("master never came up; see " + master_log);
  }
  const TAddress master_addr(TAddress::IPv4Loopback, master_port);
  /* install scope */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    client->InstallPackage({ "sample" }, 1)->Sync();
  }

  const in_port_t slave_port = ProbeFreePort();
  const string slave_log = scratch + "/slave.log";
  log_dumper.Add(slave_log);
  TChildServer slave(
      MakeServerArgs(orlyi_path, "graceful_slave", pkg_dir, slave_port,
                     ProbeFreePort(), "SLAVE", master_slave_port),
      slave_log);
  if (!WaitForLog(slave_log, "to [Slave]", seconds(240))) {
    throw runtime_error("slave never reached Slave state; see " + slave_log);
  }

  /* A raw, handshaked-then-silent client connection, still ESTABLISHED when
     the master shuts down: the #460 shape.  (A real TClient would abort()
     this test process when the server hard-closes on it, so speak the
     12-byte handshake by hand and just hold the fd.)  The master's drain
     must hard-close it so its TConnection releases the session's durable
     ptr before Clear() runs. */
  TFd silent_client(socket(AF_INET, SOCK_STREAM, 0));
  /* handshake scope */ {
    sockaddr_in addr;
    Base::Zero(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(master_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Util::IfLt0(::connect(silent_client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    Handshake::THandshake<Handshake::TNewSession> handshake((seconds(600)));
    Util::IfLt0(::send(silent_client, &handshake, sizeof(handshake), 0));
    Handshake::TNewSession::TReply reply;
    size_t got = 0;
    while (got < sizeof(reply)) {
      ssize_t piece = ::recv(silent_client, reinterpret_cast<char *>(&reply) + got, sizeof(reply) - got, 0);
      Util::IfLt0(piece);
      if (!piece) {
        throw runtime_error("master closed the silent connection during its handshake; see " + master_log);
      }
      got += static_cast<size_t>(piece);
    }
  }

  /* Freeze the slave, then push one write through the master so the
     replicate loop has something to push against the frozen peer. */
  const char *push_marker = "Write TSlave::PushNotificationsId took";
  const char *ack_marker = "Write TSlave::PushNotificationsId acked";
  cout << "Suspending the slave" << endl;
  slave.Suspend();
  /* write scope */ {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    auto client = make_shared<TExerciseClient>(master_addr);
    /* A ttl long enough that the durable layer cleaner cannot expire the
       pov mid-test: with the slave frozen, the pov's write is parked on the
       replication stream, and destroying a repo whose memory layer still
       holds that write asserts in ~TRepo (#521) -- a real engine bug, but
       not the one this fixture pins. */
    auto pov_id = client->NewFastPrivatePov(std::nullopt, seconds(600));
    auto push_result = client->Try(**pov_id, { "sample" }, TClosure(string("write_val"),
                                                                    string("n"), 1L,
                                                                    string("x"), 101L));
    bool committed;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*push_result)->GetValue().NewState((*push_result)->GetArena().get(), state_alloc)), committed);
    EXPECT_TRUE(committed);
  }
  /* Wait until a push is OUTSTANDING (written, never acked): that is the
     replicate loop provably parked in future->Sync() on the frozen peer,
     the state the SIGINT below must survive.  Not 'wait for a fresh push':
     if a push races the SIGSTOP itself -- and the silent client's own
     session handshake above makes one likely -- the loop parks BEFORE the
     pov's write can generate another marker, and a fresh one can never
     come (#520).  This wait terminates either way: the loop is already
     parked (an unacked marker exists), or it is idle and the pov write
     forces one more push that the frozen slave can never answer. */
  const auto marker_deadline = steady_clock::now() + seconds(60);
  while (CountInLog(master_log, push_marker) <= CountInLog(master_log, ack_marker)) {
    if (steady_clock::now() >= marker_deadline) {
      throw runtime_error("master never parked a replication push against the frozen slave"
                          " (slave: " + slave.Describe() + "); see " + master_log);
    }
    this_thread::sleep_for(milliseconds(250));
  }

  /* The point of the fixture: graceful shutdown must complete despite the
     in-flight replication RPC.  Record the slave's state first: if it was
     killed rather than frozen (OOM, assert), the master sees a dead peer --
     a different scenario than the one this fixture pins, and the one thing
     the CI logs could not tell us in #520. */
  cout << "Slave state before interrupting the master: " << slave.Describe() << endl;
  cout << "Interrupting the master" << endl;
  master.Interrupt();
  EXPECT_TRUE(master.Reap(seconds(60)));
  EXPECT_TRUE(LogContains(master_log, "RunReplicationQueue shutting down (#461)"));
  /* The drain must have seen the silent connection (#460)... */
  EXPECT_TRUE(LogContains(master_log, "draining ["));
  /* ...and Clear() must therefore have found no still-ptr'd durable to
     log-and-leak. */
  EXPECT_TRUE(!LogContains(master_log, "leaking it"));

  slave.Kill();
  slave.Reap(seconds(60));
}
