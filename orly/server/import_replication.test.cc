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
        slave 2 (#500), which again receives files and mapping.  Before
        slave 2 joins, the solo master overwrites the same keys round after
        round, and the test waits until the master's log shows a disk merge
        of the global pov that dropped updates, so the files the join copies
        include pruned ones (#592);
     8. write another row (live replication to slave 2), then kill the
        master: slave 2 promotes itself to solo;
     9. read every row on the promoted slave 2: the imported rows and the
        pre-join write arrived as synced data files (#501 pinned them
        unreadable), the last write arrived on the live stream, and all of
        them are reachable only because the package bound to the imported
        index id (#367).  Every overwritten key reads its last value
        (#592).

   The kills in this scenario are SIGKILL, deliberately: the slave promotes
   on connection DEATH, and a hard kill is what delivers that
   deterministically.  Graceful shutdown while paired — including with an
   UNRESPONSIVE peer — is pinned separately by the
   GracefulShutdownUnresponsiveSlave fixture below (#461).

   The PrunedJoinWithoutFileSync fixture repeats the #592 part of step 7
   with a slave that runs --allow_file_sync=false. That slave pulls the
   updates over pruned files instead of copying them.

   The PauseEmptyPovReplicates fixture pauses and unpauses an empty pov on a
   paired master: the status changes must replicate without a sequence
   number to carry (#655).

   The SlaveMutationForDiscardedRepo and SlavePovUnderDiscardedParent
   fixtures replicate to a slave that has already discarded its copy of a
   pov's repo, or of the pov's parent's.  The slave must leave the pov alone,
   not build an empty stand-in for the gone repo and apply to that (#671).

   The ExpiredPovNotInventoried fixture lets two povs with a ttl expire on a
   solo master before a slave joins. The join must inventory neither: the
   first one's saved-repo entry must be gone from the system repo by then,
   and the second one's entry, still there, must not make the master build
   an empty stand-in for it (#671).

   The SlaveInventoryKeepsPovShape fixture joins a slave to a master already
   holding shared povs: one with a child, a fast one, and a child under a
   shared pov with no ttl.  The slave must build each inventoried repo with
   the ttl, safety and parent the master sent, so that after a failover a
   read through a pov under them still reaches global (#676).

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
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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
   tiny and a collision just fails this run's startup wait.

   Never hand out the same port twice in this process.  A pair probes the
   slave's ports before it starts the master, and the kernel can offer a
   released port again, so the master's websocket or reporting port could be
   the slave's client port; the slave then failed to start with "Address
   already in use". */
in_port_t ProbeFreePort() {
  static mutex handed_out_mutex;
  static set<in_port_t> handed_out;
  for (int attempt = 0;; ++attempt) {
    TFd sock(socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in addr;
    Base::Zero(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Util::IfLt0(::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    socklen_t len = sizeof(addr);
    Util::IfLt0(getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len));
    const in_port_t port = ntohs(addr.sin_port);
    lock_guard<mutex> lock(handed_out_mutex);
    if (handed_out.insert(port).second) {
      return port;
    }
    if (attempt >= 1000) {
      throw runtime_error("ProbeFreePort: the kernel keeps offering ports this test already used");
    }
  }
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
    Live().insert(this);
  }

  ~TChildServer() {
    Live().erase(this);
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
      RawStatus = status;
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

  /* Everything the kernel will say about a child that would not die.

     A Reap() timeout means a shutdown that wedged, and the log tails alone
     cannot say WHERE: the only syslog between the connection drain and
     "TServer::Shutdown() complete" is the drain's own deadline warning, so a
     wedge anywhere in between looks identical from the log (#564).

     Per-thread `wchan` names the kernel function each thread is blocked in,
     which separates a runner loop sleeping on its timer from a genuinely
     blocked wait.  `syscall` is the more valuable half: its last two fields
     are the user stack pointer and PC, and in #554 the stack pointer was what
     identified two OS threads sharing one stack while gdb was confidently
     reporting the same bogus frame for both.  Read it before trusting any
     userspace unwinder on an optimised build.

     Best-effort and noexcept throughout -- this runs on a path that has
     already failed, and must not turn a diagnosable failure into a crash. */
  void DumpWedge(ostream &strm) const noexcept {
    if (Pid <= 0) {
      strm << "  (no live child to interrogate)" << endl;
      return;
    }
    DumpThreads(to_string(Pid), strm);
  }

  /* The per-thread half of DumpWedge(), for any /proc entry: a child's pid,
     or "self" for the test process, whose client threads were the culprit
     in #537. */
  static void DumpThreads(const string &proc_entry, ostream &strm) noexcept {
    try {
      const string task_dir = "/proc/" + proc_entry + "/task";
      DIR *dir = opendir(task_dir.c_str());
      if (!dir) {
        strm << "  (cannot open " << task_dir << ")" << endl;
        return;
      }
      while (const dirent *ent = readdir(dir)) {
        const string tid = ent->d_name;
        if (tid == "." || tid == "..") {
          continue;
        }
        const string base = task_dir + "/" + tid;
        string state = "?", wchan, syscall_line;
        /* State: from status */ {
          ifstream status(base + "/status");
          for (string line; getline(status, line); ) {
            if (line.rfind("State:", 0) == 0) {
              const auto tab = line.find_first_not_of(" \t", 6);
              state = (tab == string::npos) ? "?" : line.substr(tab, 1);
              break;
            }
          }
        }
        { ifstream w(base + "/wchan"); getline(w, wchan); }
        { ifstream sc(base + "/syscall"); getline(sc, syscall_line); }
        strm << "  tid " << tid << "  state=" << state
             << "  wchan=" << (wchan.empty() ? "-" : wchan)
             << "  syscall=" << (syscall_line.empty() ? "-" : syscall_line) << endl;
      }
      closedir(dir);
    } catch (...) {
      strm << "  (interrogation threw; ignoring)" << endl;
    }
  }

  /* True iff the child has been reaped and exited zero.  This sees what no
     log EXPECT can: a post-Shutdown() abort in static destructors leaves
     every log marker in place and still exits 134 (#522). */
  bool ExitedCleanly() const {
    return RawStatus && WIFEXITED(*RawStatus) && WEXITSTATUS(*RawStatus) == 0;
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
        if (ret == Pid) {
          RawStatus = status;
        }
        Pid = -1;
        return true;
      }
      if (steady_clock::now() >= give_up) {
        return false;
      }
      this_thread::sleep_for(milliseconds(250));
    }
  }

  /* Every child still owned by a fixture, so a failure deep inside a helper
     can interrogate the servers it cannot see (#574).  The fixtures are
     single-threaded, hence no lock. */
  static set<TChildServer *> &Live() {
    static set<TChildServer *> live;
    return live;
  }

  /* Everything the kernel will say about every live child and about this
     process, taken while they all still exist: teardown SIGKILLs the
     children, after which nothing can tell a wedged server from a dead one
     (#574).  Describe() first, because a child that already died answers
     the question by itself ("killed by signal 11"). */
  static void InterrogateAll(ostream &strm) noexcept {
    try {
      for (TChildServer *child : Live()) {
        const pid_t pid = child->Pid;
        strm << "child " << pid << ": " << child->Describe() << endl;
        child->DumpWedge(strm);
      }
      strm << "this test process (client side, #537):" << endl;
      DumpThreads("self", strm);
    } catch (...) {
      strm << "  (interrogation threw; ignoring)" << endl;
    }
  }

  private:

  pid_t Pid;

  /* Raw wait status from the reap; unknown until the child has been waited
     for (and stays unknown on the ECHILD path, where someone else got it). */
  std::optional<int> RawStatus;

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

/* Wait for an RPC future's answer with a deadline: poll the future's event
   fd instead of dereferencing straight into TFuture::Sync()'s untimed park.
   A server that never answers -- the #537 CI wedge -- then becomes a named
   test failure with the log tails attached, not a silent hang that eats the
   job's whole budget.  After this returns, Sync()/deref are instant (and
   still surface TRemoteError, which callers like the #498 refusal rely on). */
void AwaitAnswered(const Rpc::TAnyFuture &future, const char *what, seconds deadline = seconds(240)) {
  pollfd p;
  p.fd = future.GetEventFd();
  p.events = POLLIN;
  p.revents = 0;
  int ret = poll(&p, 1, static_cast<int>(duration_cast<milliseconds>(deadline).count()));
  if (ret <= 0) {
    const string msg = string("RPC [") + what + "] unanswered after " +
                       to_string(deadline.count()) + "s (#537)";
    /* The throw unwinds into teardown, which kills every server, so this is
       the last moment the hang can be seen in place (#574). */
    cout << msg << " -- interrogating before teardown (#574)" << endl;
    TChildServer::InterrogateAll(cout);
    throw runtime_error(msg);
  }
}

/* Pass-through wrapper so a call site can bound the wait at the point of
   creation: auto result = Answered(client->Try(...), "Try") parks at most
   the deadline, after which derefs are instant. */
template <typename TVal>
shared_ptr<Rpc::TFuture<TVal>> Answered(shared_ptr<Rpc::TFuture<TVal>> future,
                                        const char *what,
                                        seconds deadline = seconds(240)) {
  AwaitAnswered(*future, what, deadline);
  return future;
}

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
                              in_port_t master_slave_port,
                              const vector<string> &extra_args = {}) {
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
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  return args;
}

/* read_val(n) via the given client; unknown if the key is absent. */
Rt::TOpt<int64_t> ReadVal(const shared_ptr<TExerciseClient> &client, const Base::TUuid &pov_id, int64_t n) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto read_result = Answered(client->Try(pov_id, { "sample" }, TClosure(string("read_val"),
                                                                         string("n"), n)),
                              "read_val Try");
  Rt::TOpt<int64_t> out;
  Sabot::ToNative(*Sabot::State::TAny::TWrapper((*read_result)->GetValue().NewState((*read_result)->GetArena().get(), state_alloc)), out);
  return out;
}

/* write_val(n, x) via the given client and wait for the slave to
   acknowledge the replication; false on deadline. */
bool WriteValReplicated(const shared_ptr<TExerciseClient> &client, const Base::TUuid &pov_id, int64_t n, int64_t x) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto push_result = Answered(client->Try(pov_id, { "sample" }, TClosure(string("write_val"),
                                                                         string("n"), n,
                                                                         string("x"), x)),
                              "write_val Try");
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

/* write_val(n, x) via the given client, without waiting on replication. */
void WriteVal(const shared_ptr<TExerciseClient> &client, const Base::TUuid &pov_id, int64_t n, int64_t x) {
  Answered(client->Try(pov_id, { "sample" }, TClosure(string("write_val"),
                                                      string("n"), n,
                                                      string("x"), x)),
           "write_val Try");
}

/* read_val(n) against the given server with retries, for reads against a
   slave that is still promoting or catching up.  Returns the value once a
   successful RPC yields a known value; unknown only after the deadline. */
Rt::TOpt<int64_t> ReadWithRetry(const TAddress &addr, int64_t n, seconds deadline) {
  const auto give_up = steady_clock::now() + deadline;
  for (;;) {
    try {
      auto client = make_shared<TExerciseClient>(addr);
      auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)),
                             "NewFastPrivatePov (retry read)", seconds(30));
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

/* #592: the keys the pruning scenarios overwrite, how often, and the value of each write. */
const int64_t OverwrittenKeyBase = 100L, OverwrittenKeys = 40L, OverwriteRounds = 25L;

int64_t OverwrittenVal(int64_t round, int64_t key) {
  return round * 1000L + key;
}

/* How many disk merges of the global pov's files have dropped updates, per the server's log
   (TMergeDataFileImpl logs every tail merge at info level, #592). */
size_t CountPruningMerges(const string &path) {
  ostringstream global;
  global << GlobalPovId;
  const string prefix = "MergeDataFile [" + global.str() + "]";
  ifstream strm(path);
  string line;
  size_t count = 0;
  while (getline(strm, line)) {
    if (line.find(prefix) == string::npos) {
      continue;
    }
    const size_t kept_pos = line.find("kept [");
    if (kept_pos == string::npos) {
      continue;
    }
    unsigned long kept = 0, total = 0;
    if (sscanf(line.c_str() + kept_pos, "kept [%lu] of [%lu] updates", &kept, &total) == 2 && kept < total) {
      ++count;
    }
  }
  return count;
}

/* On a solo master, overwrite the same keys round after round. Then wait until two things have
   happened: the writes have reached the global pov, and a disk merge has dropped superseded
   versions there since the writes began. A slave that joins after this reads files that merge
   has pruned (#592). */
void OverwriteUntilPruned(const TAddress &master_addr, const string &master_log) {
  const size_t pruned_before = CountPruningMerges(master_log);
  auto client = make_shared<TExerciseClient>(master_addr);
  auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
  for (int64_t round = 1L; round <= OverwriteRounds; ++round) {
    for (int64_t key = 0L; key < OverwrittenKeys; ++key) {
      WriteVal(client, **pov_id, OverwrittenKeyBase + key, OverwrittenVal(round, key));
    }
  }
  /* The writes reach the global pov by promotion, in order: wait for the last one. */
  const int64_t last_key = OverwrittenKeys - 1L;
  const auto give_up = steady_clock::now() + seconds(120);
  for (;;) {
    Rt::TOpt<int64_t> row = ReadWithRetry(master_addr, OverwrittenKeyBase + last_key, seconds(30));
    if (row.IsKnown() && row.GetVal() == OverwrittenVal(OverwriteRounds, last_key)) {
      break;
    }
    if (steady_clock::now() >= give_up) {
      throw runtime_error("the overwrites never reached the global pov; see " + master_log);
    }
    this_thread::sleep_for(milliseconds(200));
  }
  /* The memory merge flushes the global pov every 40ms and the disk merge runs every 10ms, so
     the overwrites span many files, and a pruning merge follows them closely. */
  while (CountPruningMerges(master_log) <= pruned_before) {
    if (steady_clock::now() >= give_up) {
      throw runtime_error("no disk merge of the global pov dropped any update; see " + master_log);
    }
    this_thread::sleep_for(milliseconds(200));
  }
}

/* Every overwritten key reads its last value on the server at addr (#592). */
void ExpectOverwrittenKeys(const TAddress &addr) {
  for (int64_t key = 0L; key < OverwrittenKeys; ++key) {
    Rt::TOpt<int64_t> row = ReadWithRetry(addr, OverwrittenKeyBase + key, seconds(30));
    EXPECT_TRUE(row.IsKnown());
    if (row.IsKnown()) {
      EXPECT_EQ(row.GetVal(), OverwrittenVal(OverwriteRounds, key));
    }
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
    Answered(client->BeginImport(), "BeginImport")->Sync();
    /* merge_simultaneous must exceed the file count divided by the merge
       rounds you can afford: a value of 1 merges each file into itself and
       the merge loop never converges. */
    Answered(client->ImportCoreVector(import_pattern, "sample", 1, 1, 4), "ImportCoreVector")->Sync();
    /* A single-file import skips the merge phase entirely; it used to hand
       uninitialized sequence numbers to AddFileToRepo (#497). */
    Answered(client->ImportCoreVector(scratch + "/single.bin", "sample", 1, 1, 4), "ImportCoreVector single")->Sync();
    Answered(client->EndImport(), "EndImport")->Sync();

    /* Install the package: it must adopt the importer's index id, making
       all three imported rows readable. */
    Answered(client->InstallPackage({ "sample" }, 1), "InstallPackage sample")->Sync();
    auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
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
    Answered(client->InstallPackage({ "sample2" }, 1), "InstallPackage sample2")->Sync();
    EXPECT_TRUE(WaitForLog(slave_1_log, "sample2 tuple(str, int64)", seconds(120)));
    EXPECT_TRUE(!LogContains(master_log, "RunReplicateTransaction error"));
    /* A write's replication ack must survive sharing the stream with the
       install's system-repo commits (#499 ate the rest of the batch's
       notifications). */
    auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
    EXPECT_TRUE(WriteValReplicated(client, **pov_id, 9L, 909L));
  }

  /* With a slave attached, an import must be refused up front (#498), and
     the refusal must leave the master healthy. */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    bool import_refused = false;
    try {
      Answered(client->BeginImport(), "BeginImport (paired)")->Sync();
    } catch (const exception &/*ex*/) {
      import_refused = true;
    }
    EXPECT_TRUE(import_refused);

    /* Master still healthy: install on the slave, write through the master,
       and wait for the slave to acknowledge the replication. */
    auto slave_client = make_shared<TExerciseClient>(TAddress(TAddress::IPv4Loopback, slave_1_port));
    Answered(slave_client->InstallPackage({ "sample" }, 1), "InstallPackage on slave 1")->Sync();
    auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
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

  /* While solo, overwrite the same keys round after round, until a disk merge has dropped
     superseded versions (#592). Slave 2's join must still carry every key's latest value in the
     data files it copies. */
  OverwriteUntilPruned(master_addr, master_log);

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
  /* the join copied the master's data files, pruned ones included (#592) */
  EXPECT_TRUE(LogContains(slave_2_log, "sync file ["));

  /* Install on slave 2, then push one more row through the master so the
     live stream to slave 2 is exercised too. */ {
    auto slave_client = make_shared<TExerciseClient>(TAddress(TAddress::IPv4Loopback, slave_2_port));
    Answered(slave_client->InstallPackage({ "sample" }, 1), "InstallPackage on slave 2")->Sync();
    auto client = make_shared<TExerciseClient>(master_addr);
    auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
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
  /* ...and so must every overwritten key, at its last value (#592). */
  ExpectOverwrittenKeys(TAddress(TAddress::IPv4Loopback, slave_2_port));
}

/* #592: a slave joining with --allow_file_sync=false pulls the master's updates over key ranges
   (PullUpdateRange), walking files a disk merge has pruned, instead of copying the files. After
   it promotes, every overwritten key must read its last value. */
FIXTURE(PrunedJoinWithoutFileSync) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = GetScratchDir();
  const string orlyi_path = GetOrlyiPath();
  TLogTailDumper log_dumper;
  if (!ifstream(orlyi_path).good()) {
    throw runtime_error("orlyi binary not built at [" + orlyi_path + "]; run `make debug` first");
  }
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
      MakeServerArgs(orlyi_path, "pruned_join_master", pkg_dir, master_port,
                     master_slave_port, "SOLO", 0),
      master_log);
  if (!WaitForPort(master_port, seconds(240))) {
    throw runtime_error("master never came up; see " + master_log);
  }
  const TAddress master_addr(TAddress::IPv4Loopback, master_port);
  {
    auto client = make_shared<TExerciseClient>(master_addr);
    Answered(client->InstallPackage({ "sample" }, 1), "InstallPackage sample")->Sync();
  }
  OverwriteUntilPruned(master_addr, master_log);

  const in_port_t slave_port = ProbeFreePort();
  const string slave_log = scratch + "/slave.log";
  log_dumper.Add(slave_log);
  TChildServer slave(
      MakeServerArgs(orlyi_path, "pruned_join_slave", pkg_dir, slave_port,
                     ProbeFreePort(), "SLAVE", master_slave_port, {"--allow_file_sync=false"}),
      slave_log);
  if (!WaitForPort(slave_port, seconds(240))) {
    throw runtime_error("slave never came up; see " + slave_log);
  }
  if (!WaitForLog(slave_log, "to [Slave]", seconds(120))) {
    throw runtime_error("slave never reached Slave state; see " + slave_log);
  }
  /* it pulled updates, and copied no file */
  EXPECT_TRUE(LogContains(slave_log, "TSlave: PullUpdateRange from ["));
  EXPECT_TRUE(!LogContains(slave_log, "sync file ["));
  {
    auto slave_client = make_shared<TExerciseClient>(TAddress(TAddress::IPv4Loopback, slave_port));
    Answered(slave_client->InstallPackage({ "sample" }, 1), "InstallPackage on slave")->Sync();
  }

  /* Promote the slave: SIGKILL the master (see the header comment). The pause lets the
     just-destroyed clients' dispatch threads wind down first. */
  this_thread::sleep_for(seconds(2));
  master.Kill();
  master.Reap(seconds(60));
  if (!WaitForLog(slave_log, "slave promoted to solo", seconds(120))) {
    throw runtime_error("slave never promoted; see " + slave_log);
  }
  ExpectOverwrittenKeys(TAddress(TAddress::IPv4Loopback, slave_port));
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
    Answered(client->InstallPackage({ "sample" }, 1), "InstallPackage (graceful)")->Sync();
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
    /* ttl=0 on purpose: with the slave frozen, the pov's write is parked on
       the replication stream, so the pov's repo dies (client disconnect, then
       shutdown teardown) with that write still in its memory layer.  That
       discard is sanctioned -- expiry of an unsafe pov drops unmerged data by
       contract -- and must not trip the ~TRepo lifecycle assert, which would
       abort the master mid-shutdown and fail the Reap below (#521). */
    auto pov_id = Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)),
                           "NewFastPrivatePov (graceful)");
    auto push_result = Answered(client->Try(**pov_id, { "sample" }, TClosure(string("write_val"),
                                                                             string("n"), 1L,
                                                                             string("x"), 101L)),
                                "write_val Try (graceful)");
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
  /* Interrogate BEFORE asserting: a failed Reap is the only moment the wedged
     process still exists, and #564 has so far cost two CI failures that could
     not be diagnosed afterwards from log tails.  Reap() leaves Pid set when it
     times out, so the child is still there to read. */
  const bool master_reaped = master.Reap(seconds(60));
  if (!master_reaped) {
    cout << "master did not exit within 60s of SIGINT -- interrogating before it dies (#564)" << endl;
    master.DumpWedge(cout);
  }
  EXPECT_TRUE(master_reaped);
  EXPECT_TRUE(LogContains(master_log, "RunReplicationQueue shutting down (#461)"));
  /* The drain must have seen the silent connection (#460)... */
  EXPECT_TRUE(LogContains(master_log, "draining ["));
  /* ...and Clear() must therefore have found no still-ptr'd durable to
     log-and-leak. */
  EXPECT_TRUE(!LogContains(master_log, "leaking it"));
  /* The whole teardown must have run to its end marker: an abort anywhere
     mid-Shutdown -- e.g. the #521 ~TRepo lifecycle assert tripping on the
     ttl=0 pov's repo, whose parked write it must discard as sanctioned --
     dies before this line (Reap alone cannot see that; it ignores how the
     child exited). */
  EXPECT_TRUE(LogContains(master_log, "TServer::Shutdown() complete"));
  /* ...and the exit itself must be clean.  The static disk-event pool
     manager used to find leaked per-thread pools in __run_exit_handlers
     and terminate AFTER that marker, turning every graceful shutdown into
     exit 134 (#522). */
  EXPECT_TRUE(master.ExitedCleanly());

  slave.Kill();
  slave.Reap(seconds(60));
}

/* #655: pausing or unpausing a pov with no unpromoted writes, on a master with a live slave.  The
   status change's replicated sequence number is the pov's lowest unpromoted one, which an empty pov
   doesn't have: the master's replication pass dereferenced the empty optional (an assert in debug,
   garbage on the wire in release), and the slave dereferenced its own copy's empty one again.  The
   common "pause, then write" order does exactly this, because the pov is empty when the pause
   commits.  Both servers must survive, and the slave must apply both status changes.

   No write follows: on a slave, the first replicated write to a pov created while paired crashes in
   TRepo::AppendUpdate for a reason of its own (#661). */
FIXTURE(PauseEmptyPovReplicates) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = GetScratchDir();
  const string orlyi_path = GetOrlyiPath();
  TLogTailDumper log_dumper;
  if (!ifstream(orlyi_path).good()) {
    throw runtime_error("orlyi binary not built at [" + orlyi_path + "]; run `make debug` first");
  }
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
      MakeServerArgs(orlyi_path, "pause_empty_master", pkg_dir, master_port,
                     master_slave_port, "SOLO", 0),
      master_log);
  if (!WaitForPort(master_port, seconds(240))) {
    throw runtime_error("master never came up; see " + master_log);
  }
  const TAddress master_addr(TAddress::IPv4Loopback, master_port);
  /* install scope */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    Answered(client->InstallPackage({ "sample" }, 1), "InstallPackage (pause empty)")->Sync();
  }

  const in_port_t slave_port = ProbeFreePort();
  const string slave_log = scratch + "/slave.log";
  log_dumper.Add(slave_log);
  TChildServer slave(
      MakeServerArgs(orlyi_path, "pause_empty_slave", pkg_dir, slave_port,
                     ProbeFreePort(), "SLAVE", master_slave_port),
      slave_log);
  if (!WaitForLog(slave_log, "to [Slave]", seconds(240))) {
    throw runtime_error("slave never reached Slave state; see " + slave_log);
  }

  /* client scope */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    auto pov_id = Answered(client->NewSafeSharedPov(std::nullopt), "NewSafeSharedPov (pause empty)");
    /* Both on the empty pov. */
    Answered(client->PausePov(**pov_id), "PausePov (empty)")->Sync();
    Answered(client->UnpausePov(**pov_id), "UnpausePov (empty)")->Sync();
    ostringstream strm;
    strm << **pov_id;
    const string pov_str = strm.str();
    EXPECT_TRUE(WaitForLog(slave_log, "Slave applying pause of repo [" + pov_str + "]", seconds(60)));
    EXPECT_TRUE(WaitForLog(slave_log, "Slave applying unpause of repo [" + pov_str + "]", seconds(60)));
    /* The master's replication pass is where it used to abort, so give it a moment past the ack. */
    this_thread::sleep_for(seconds(2));
  }
  const string master_state = master.Describe();
  const string slave_state = slave.Describe();
  cout << "master: " << master_state << "; slave: " << slave_state << endl;
  EXPECT_TRUE(master_state.rfind("alive", 0) == 0);
  EXPECT_TRUE(slave_state.rfind("alive", 0) == 0);

  master.Kill();
  master.Reap(seconds(60));
  slave.Kill();
  slave.Reap(seconds(60));
}

/* #661: a slave must survive writes to a pov that was created while the pair was up, and the
   writes must replicate. The slave builds its copy of the pov's repo from the replicated repo
   record and drops its pointer straight away, so the repo (ttl > 0) goes into the cache before
   the first push to it arrives. Caching used to force-release the repo's pointer to its parent
   while still claiming a parent, so the first replicated write segfaulted the slave in
   TTetrisManager::Join. A safe shared pov, because it has a ttl (the default 600 s) and its
   writes promote to the global pov, where the promoted slave can read them back. */
FIXTURE(WriteToPovCreatedWhilePaired) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = GetScratchDir();
  const string orlyi_path = GetOrlyiPath();
  TLogTailDumper log_dumper;
  if (!ifstream(orlyi_path).good()) {
    throw runtime_error("orlyi binary not built at [" + orlyi_path + "]; run `make debug` first");
  }
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
      MakeServerArgs(orlyi_path, "paired_pov_master", pkg_dir, master_port,
                     master_slave_port, "SOLO", 0),
      master_log);
  if (!WaitForPort(master_port, seconds(240))) {
    throw runtime_error("master never came up; see " + master_log);
  }
  const TAddress master_addr(TAddress::IPv4Loopback, master_port);
  {
    auto client = make_shared<TExerciseClient>(master_addr);
    Answered(client->InstallPackage({ "sample" }, 1), "InstallPackage sample")->Sync();
  }

  const in_port_t slave_port = ProbeFreePort();
  const string slave_log = scratch + "/slave.log";
  log_dumper.Add(slave_log);
  TChildServer slave(
      MakeServerArgs(orlyi_path, "paired_pov_slave", pkg_dir, slave_port,
                     ProbeFreePort(), "SLAVE", master_slave_port),
      slave_log);
  if (!WaitForPort(slave_port, seconds(240))) {
    throw runtime_error("slave never came up; see " + slave_log);
  }
  if (!WaitForLog(slave_log, "to [Slave]", seconds(120))) {
    throw runtime_error("slave never reached Slave state; see " + slave_log);
  }
  {
    auto slave_client = make_shared<TExerciseClient>(TAddress(TAddress::IPv4Loopback, slave_port));
    Answered(slave_client->InstallPackage({ "sample" }, 1), "InstallPackage on slave")->Sync();
  }

  /* The pov is created on the paired master, so the slave learns of it from the replication
     stream. Two writes: the first reaches the slave's cached copy, the second its reopened one. */
  {
    auto client = make_shared<TExerciseClient>(master_addr);
    auto pov_id = Answered(client->NewSafeSharedPov(std::nullopt), "NewSafeSharedPov");
    EXPECT_TRUE(WriteValReplicated(client, **pov_id, 61L, 6161L));
    cout << "Slave state after the first write: " << slave.Describe() << endl;
    EXPECT_TRUE(WriteValReplicated(client, **pov_id, 62L, 6262L));
    cout << "Slave state after the second write: " << slave.Describe() << endl;
  }
  EXPECT_TRUE(slave.IsAlive());
  EXPECT_TRUE(master.IsAlive());

  /* Promote the slave (SIGKILL the master, see the header comment) and read both rows back
     through the global pov. */
  this_thread::sleep_for(seconds(2));
  master.Kill();
  master.Reap(seconds(60));
  if (!WaitForLog(slave_log, "slave promoted to solo", seconds(120))) {
    throw runtime_error("slave never promoted (" + slave.Describe() + "); see " + slave_log);
  }
  const TAddress slave_addr(TAddress::IPv4Loopback, slave_port);
  const int64_t expected_by_key[][2] = {{61L, 6161L}, {62L, 6262L}};
  for (const auto &pair : expected_by_key) {
    Rt::TOpt<int64_t> row = ReadWithRetry(slave_addr, pair[0], seconds(60));
    EXPECT_TRUE(row.IsKnown());
    if (row.IsKnown()) {
      EXPECT_EQ(row.GetVal(), pair[1]);
    }
  }
  slave.Kill();
  slave.Reap(seconds(60));
}

/* Start a solo master and a slave paired with it, both with the sample package installed; the
   slave gets slave_args, and before_join runs against the solo master before the slave starts.  For
   the #671 and #676 fixtures. */
struct TPairedServers {
  TPairedServers(const string &scratch, const string &name, const vector<string> &slave_args, TLogTailDumper &log_dumper,
                 const function<void (const TAddress &)> &before_join = {})
      : PkgDir(scratch + "/packages"), MasterLog(scratch + "/master.log"), SlaveLog(scratch + "/slave.log"),
        MasterPort(ProbeFreePort()), MasterSlavePort(ProbeFreePort()), SlavePort(ProbeFreePort()),
        Master(MakeServerArgs(GetOrlyiPath(), name + "_master", PkgDir, MasterPort, MasterSlavePort, "SOLO", 0), MasterLog),
        Slave(nullptr) {
    log_dumper.Add(MasterLog);
    log_dumper.Add(SlaveLog);
    if (!WaitForPort(MasterPort, seconds(240))) {
      throw runtime_error("master never came up; see " + MasterLog);
    }
    Answered(make_shared<TExerciseClient>(MasterAddr())->InstallPackage({ "sample" }, 1), "InstallPackage (master)")->Sync();
    if (before_join) {
      before_join(MasterAddr());
    }
    Slave = make_unique<TChildServer>(
        MakeServerArgs(GetOrlyiPath(), name + "_slave", PkgDir, SlavePort, ProbeFreePort(), "SLAVE", MasterSlavePort, slave_args),
        SlaveLog);
    if (!WaitForPort(SlavePort, seconds(240)) || !WaitForLog(SlaveLog, "to [Slave]", seconds(120))) {
      throw runtime_error("slave never reached Slave state; see " + SlaveLog);
    }
    Answered(make_shared<TExerciseClient>(SlaveAddr())->InstallPackage({ "sample" }, 1), "InstallPackage (slave)")->Sync();
  }

  TAddress MasterAddr() const {
    return TAddress(TAddress::IPv4Loopback, MasterPort);
  }

  TAddress SlaveAddr() const {
    return TAddress(TAddress::IPv4Loopback, SlavePort);
  }

  /* SIGKILL the master, so the slave promotes (see the header comment). */
  void Failover() {
    Master.Kill();
    Master.Reap(seconds(60));
    if (!WaitForLog(SlaveLog, "slave promoted to solo", seconds(120))) {
      throw runtime_error("slave never promoted (" + Slave->Describe() + "); see " + SlaveLog);
    }
  }

  const string PkgDir, MasterLog, SlaveLog;
  const in_port_t MasterPort, MasterSlavePort, SlavePort;
  TChildServer Master;
  unique_ptr<TChildServer> Slave;
};

/* Write the sample package to a fresh scratch dir and compile it there. */
string NewSampleScratch() {
  const string scratch = GetScratchDir();
  Util::IfLt0(mkdir((scratch + "/packages").c_str(), 0755));
  { ofstream marker(scratch + "/packages/__orly__"); }
  {
    ofstream src(scratch + "/sample.orly");
    src << SamplePackage;
  }
  Compiler::Compile(TPath(scratch + "/sample.orly"), Jhm::TTree(scratch + "/packages"), {});
  return scratch;
}

/* #671, the slave's apply path: a replicated mutation for a repo the slave has discarded.  With
   no repo cache, the slave discards its copy of a new pov's repo as soon as it has built it, and
   discards whatever it applied a write to once that write is promoted.  The slave used to open
   the gone repo by force for each mutation, which builds an empty repo whose sequence starts at
   1, so the first mutation past sequence 1 failed the slave's sequence check (seen: the assert in
   TTransaction::Pop, in a debug build; "missing data" for a push in release). */
FIXTURE(SlaveMutationForDiscardedRepo) {
  Orly::Type::TTypeCzar type_czar;
  if (!ifstream(GetOrlyiPath()).good()) {
    throw runtime_error("orlyi binary not built at [" + GetOrlyiPath() + "]; run `make debug` first");
  }
  TLogTailDumper log_dumper;
  TPairedServers pair(NewSampleScratch(), "gone_repo", { "--max_repo_cache_size=0" }, log_dumper);
  /* client scope */ {
    auto client = make_shared<TExerciseClient>(pair.MasterAddr());
    const Base::TUuid pov_id = **Answered(client->NewSafeSharedPov(std::nullopt), "NewSafeSharedPov");
    EXPECT_TRUE(WriteValReplicated(client, pov_id, 71L, 7171L));
    /* Let the slave's merge release what it applied the first write to. */
    this_thread::sleep_for(seconds(2));
    EXPECT_TRUE(WriteValReplicated(client, pov_id, 72L, 7272L));
    this_thread::sleep_for(seconds(2));
  }
  cout << "slave: " << pair.Slave->Describe() << endl;
  EXPECT_TRUE(pair.Slave->IsAlive());
  EXPECT_FALSE(LogContains(pair.SlaveLog, "missing data"));
  /* Both writes reach the slave's global pov all the same. */
  pair.Failover();
  const int64_t expected_by_key[][2] = {{71L, 7171L}, {72L, 7272L}};
  for (const auto &row : expected_by_key) {
    Rt::TOpt<int64_t> val = ReadWithRetry(pair.SlaveAddr(), row[0], seconds(60));
    EXPECT_TRUE(val.IsKnown());
    if (val.IsKnown()) {
      EXPECT_EQ(val.GetVal(), row[1]);
    }
  }
  pair.Slave->Kill();
  pair.Slave->Reap(seconds(60));
}

/* #671, the slave's repo creation: a pov whose parent the slave has discarded.  The parent is a
   shared pov with no ttl, so the slave discards its copy of the parent's repo as soon as it has
   built it; the child has a ttl, so the slave keeps its copy.  The slave used to open the gone
   parent by force, which builds an empty repo with no parent, and gave the child that.  After a
   failover the child then read through to nothing: a key the global pov holds read as unknown.
   Now the slave doesn't build the child, so after a failover it is refused as a pov whose state is
   gone (#439), like any other pov the new master doesn't have. */
FIXTURE(SlavePovUnderDiscardedParent) {
  Orly::Type::TTypeCzar type_czar;
  if (!ifstream(GetOrlyiPath()).good()) {
    throw runtime_error("orlyi binary not built at [" + GetOrlyiPath() + "]; run `make debug` first");
  }
  TLogTailDumper log_dumper;
  TPairedServers pair(NewSampleScratch(), "gone_parent", {}, log_dumper);
  Base::TUuid child_id;
  /* client scope */ {
    auto client = make_shared<TExerciseClient>(pair.MasterAddr());
    /* A value in the global pov, for the child to read through its parent. */
    const Base::TUuid writer_id = **Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
    EXPECT_TRUE(WriteValReplicated(client, writer_id, 81L, 8181L));
    const Base::TUuid parent_id = **Answered(client->NewFastSharedPov(std::nullopt, seconds(0)), "NewFastSharedPov (parent)");
    child_id = **Answered(client->NewSafeSharedPov(parent_id), "NewSafeSharedPov (child)");
    /* On the master the child reads the global value through its parent. */
    Rt::TOpt<int64_t> on_master = ReadVal(client, child_id, 81L);
    EXPECT_TRUE(on_master.IsKnown() && on_master.GetVal() == 8181L);
  }
  /* Closing the client closes the child pov, which saves it; the save replicates, so the
     promoted slave knows the child pov. */
  this_thread::sleep_for(seconds(3));
  pair.Failover();
  bool refused = false;
  std::optional<Rt::TOpt<int64_t>> read;
  try {
    auto client = make_shared<TExerciseClient>(pair.SlaveAddr());
    read = ReadVal(client, child_id, 81L);
  } catch (const exception &ex) {
    refused = true;
    cout << "child read on the promoted slave refused: " << ex.what() << endl;
  }
  if (read) {
    cout << "child read on the promoted slave: " << (read->IsKnown() ? to_string(read->GetVal()) : string("unknown")) << endl;
  }
  /* Refusing is right; so would be reading the global value.  Reading nothing is not. */
  EXPECT_TRUE(refused || (read->IsKnown() && read->GetVal() == 8181L));
  pair.Slave->Kill();
  pair.Slave->Reap(seconds(60));
}

FIXTURE(ExpiredPovNotInventoried) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = GetScratchDir();
  const string orlyi_path = GetOrlyiPath();
  TLogTailDumper log_dumper;
  if (!ifstream(orlyi_path).good()) {
    throw runtime_error("orlyi binary not built at [" + orlyi_path + "]; run `make debug` first");
  }
  const string pkg_dir = scratch + "/packages";
  Util::IfLt0(mkdir(pkg_dir.c_str(), 0755));
  { ofstream marker(pkg_dir + "/__orly__"); }
  {
    ofstream src(scratch + "/sample.orly");
    src << SamplePackage;
  }
  Compiler::Compile(TPath(scratch + "/sample.orly"), Jhm::TTree(pkg_dir), {});

  /* With no repo cache, a repo goes as soon as nothing holds it, and a fast housekeeper expires
     a pov with a one-second ttl promptly, so the expired povs' repos are gone before the join. */
  const in_port_t master_port = ProbeFreePort();
  const in_port_t master_slave_port = ProbeFreePort();
  const string master_log = scratch + "/master.log";
  log_dumper.Add(master_log);
  TChildServer master(
      MakeServerArgs(orlyi_path, "expired_pov_master", pkg_dir, master_port,
                     master_slave_port, "SOLO", 0,
                     { "--max_repo_cache_size=0", "--housecleaning_interval=200" }),
      master_log);
  if (!WaitForPort(master_port, seconds(240))) {
    throw runtime_error("master never came up; see " + master_log);
  }
  const TAddress master_addr(TAddress::IPv4Loopback, master_port);
  /* install scope */ {
    auto client = make_shared<TExerciseClient>(master_addr);
    Answered(client->InstallPackage({ "sample" }, 1), "InstallPackage (expired pov)")->Sync();
  }
  auto to_str = [](const Base::TUuid &id) {
    ostringstream strm;
    strm << id;
    return strm.str();
  };
  /* A pov with a ttl, written through and closed: its session goes with its client, then the
     pov expires, then its repo goes. */
  auto expire_pov = [&](int64_t n) {
    auto client = make_shared<TExerciseClient>(master_addr);
    const Base::TUuid pov_id = **Answered(client->NewFastSharedPov(std::nullopt, seconds(1)), "NewFastSharedPov (expiring)");
    WriteVal(client, pov_id, n, n * 100L);
    return pov_id;
  };
  const string discarded = "TManager: discarded repo [";

  /* The control: a live pov, held open by its client until the end, must be inventoried. */
  auto live_client = make_shared<TExerciseClient>(master_addr);
  const Base::TUuid live_pov = **Answered(live_client->NewSafeSharedPov(std::nullopt, seconds(600)), "NewSafeSharedPov (live)");
  WriteVal(live_client, live_pov, 1, 100);

  /* The first expired pov's saved entry goes with the next system-repo write, which creating
     another pov with a ttl makes. */
  const Base::TUuid removed_pov = expire_pov(2);
  EXPECT_TRUE(WaitForLog(master_log, discarded + to_str(removed_pov) + "]", seconds(30)));
  Answered(live_client->NewFastSharedPov(std::nullopt, seconds(600)), "NewFastSharedPov (system-repo write)");
  EXPECT_TRUE(WaitForLog(master_log, "TManager: removed the saved entry of repo [" + to_str(removed_pov) + "]", seconds(30)));

  /* The second one expires with nothing written to the system repo after it, so the join still
     finds its saved entry. */
  const Base::TUuid stale_pov = expire_pov(3);
  EXPECT_TRUE(WaitForLog(master_log, discarded + to_str(stale_pov) + "]", seconds(30)));

  const in_port_t slave_port = ProbeFreePort();
  const string slave_log = scratch + "/slave.log";
  log_dumper.Add(slave_log);
  TChildServer slave(
      MakeServerArgs(orlyi_path, "expired_pov_slave", pkg_dir, slave_port,
                     ProbeFreePort(), "SLAVE", master_slave_port),
      slave_log);
  if (!WaitForLog(slave_log, "to [Slave]", seconds(240))) {
    throw runtime_error("slave never reached Slave state; see " + slave_log);
  }
  EXPECT_TRUE(LogContains(slave_log, "TSlave::Inventory(" + to_str(live_pov) + ")"));
  /* Before #671 the join walked every saved entry ever written and built an empty repo for each
     pov that had gone, to inventory to the slave.  Here, with no repo cache, that stand-in went as
     soon as the walk let go of it, under the sync view that still held its layers, and the master
     aborted (`~TDataLayer(): RefCount == 0`) before the slave ever joined. */
  EXPECT_FALSE(LogContains(slave_log, "TSlave::Inventory(" + to_str(removed_pov) + ")"));
  EXPECT_FALSE(LogContains(slave_log, "TSlave::Inventory(" + to_str(stale_pov) + ")"));
  EXPECT_FALSE(LogContains(master_log, "matched repo [" + to_str(removed_pov) + "]"));
  EXPECT_TRUE(LogContains(master_log, "TMaster: saved repo [" + to_str(stale_pov) + "] has gone"));

  const string master_state = master.Describe();
  const string slave_state = slave.Describe();
  cout << "master: " << master_state << "; slave: " << slave_state << endl;
  EXPECT_TRUE(master_state.rfind("alive", 0) == 0);
  EXPECT_TRUE(slave_state.rfind("alive", 0) == 0);
  live_client.reset();

  master.Kill();
  master.Reap(seconds(60));
  slave.Kill();
  slave.Reap(seconds(60));
}

/* read_val(n) through the given pov against the given server, retrying while the server refuses
   or answers unknown, for reads against a slave that has just promoted.  Unknown only after the
   deadline. */
Rt::TOpt<int64_t> ReadPovWithRetry(const TAddress &addr, const Base::TUuid &pov_id, int64_t n, seconds deadline) {
  const auto give_up = steady_clock::now() + deadline;
  for (;;) {
    try {
      auto client = make_shared<TExerciseClient>(addr);
      Rt::TOpt<int64_t> out = ReadVal(client, pov_id, n);
      if (out.IsKnown()) {
        return out;
      }
      cout << "read_val(" << n << ") through pov {" << pov_id << "}: unknown" << endl;
    } catch (const exception &ex) {
      cout << "read_val(" << n << ") through pov {" << pov_id << "} retry after [" << ex.what() << "]" << endl;
    }
    if (steady_clock::now() >= give_up) {
      return Rt::TOpt<int64_t>();
    }
    this_thread::sleep_for(seconds(1));
  }
}

/* #676: a slave's join builds each repo the master inventories.  It used to open each one with
   GetRepo(..., create=false), which ignores the ttl, parent and safety it was sent, so every repo
   the slave learnt of at the join came out safe, with no parent and a 1000 s ttl.  After a
   failover, a read through a pov under a shared pov then reached neither the parent nor global.

   The master holds, when the slave joins: a shared pov P with a child D, a fast shared pov F, and
   a shared pov Y under a fast shared pov Z with no ttl.  Z has no saved-repo entry (only a repo
   with a ttl gets one), so only Y's parent link tells the master about it.

   The reads go through povs made after the join, under P, D and Y; the slave builds each under its
   own copy of that parent.  (Reads through povs made before the join are JoinSendsPovRecords'.) */
FIXTURE(SlaveInventoryKeepsPovShape) {
  Orly::Type::TTypeCzar type_czar;
  if (!ifstream(GetOrlyiPath()).good()) {
    throw runtime_error("orlyi binary not built at [" + GetOrlyiPath() + "]; run `make debug` first");
  }
  TLogTailDumper log_dumper;
  /* Open across the join, so the master keeps the povs made before it. */
  shared_ptr<TExerciseClient> client;
  Base::TUuid parent_id, child_id, fast_id, no_ttl_parent_id, under_no_ttl_id;
  TPairedServers pair(NewSampleScratch(), "inventory_shape", {}, log_dumper, [&](const TAddress &master_addr) {
    client = make_shared<TExerciseClient>(master_addr);
    /* A value in the global pov, for the povs to read through their parents. */
    const Base::TUuid writer_id = **Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
    WriteVal(client, writer_id, 91L, 9191L);
    Rt::TOpt<int64_t> in_global = ReadWithRetry(master_addr, 91L, seconds(60));
    EXPECT_TRUE(in_global.IsKnown() && in_global.GetVal() == 9191L);
    parent_id = **Answered(client->NewSafeSharedPov(std::nullopt, seconds(700)), "NewSafeSharedPov (P)");
    child_id = **Answered(client->NewSafeSharedPov(parent_id, seconds(800)), "NewSafeSharedPov (D)");
    fast_id = **Answered(client->NewFastSharedPov(std::nullopt, seconds(600)), "NewFastSharedPov (F)");
    no_ttl_parent_id = **Answered(client->NewFastSharedPov(std::nullopt, seconds(0)), "NewFastSharedPov (Z)");
    under_no_ttl_id = **Answered(client->NewSafeSharedPov(no_ttl_parent_id, seconds(900)), "NewSafeSharedPov (Y)");
    /* Each read also makes the master build the pov's repo. */
    for (const Base::TUuid &pov_id : { child_id, fast_id, under_no_ttl_id }) {
      Rt::TOpt<int64_t> on_master = ReadVal(client, pov_id, 91L);
      EXPECT_TRUE(on_master.IsKnown() && on_master.GetVal() == 9191L);
    }
  });
  /* The slave built each inventoried repo with the master's ttl and safety. */
  auto built = [&pair](const Base::TUuid &repo_id, long ttl, bool is_safe) {
    ostringstream strm;
    strm << "Create Repo [" << repo_id << "] with ttl=[" << ttl << "], is_safe=[" << (is_safe ? "true" : "false") << "]";
    const bool found = LogContains(pair.SlaveLog, strm.str());
    if (!found) {
      cout << "slave log lacks: " << strm.str() << endl;
    }
    return found;
  };
  EXPECT_TRUE(built(parent_id, 700, true));
  EXPECT_TRUE(built(child_id, 800, true));
  EXPECT_TRUE(built(fast_id, 600, false));
  EXPECT_TRUE(built(no_ttl_parent_id, 0, false));
  EXPECT_TRUE(built(under_no_ttl_id, 900, true));
  /* Povs under P, D and Y, made while paired. */
  vector<Base::TUuid> late_ids;
  for (const Base::TUuid &parent : { parent_id, child_id, under_no_ttl_id }) {
    late_ids.push_back(**Answered(client->NewSafeSharedPov(parent, seconds(600)), "NewSafeSharedPov (after the join)"));
    Rt::TOpt<int64_t> on_master = ReadVal(client, late_ids.back(), 91L);
    EXPECT_TRUE(on_master.IsKnown() && on_master.GetVal() == 9191L);
  }
  /* Each pov's record reached the slave when it was made: a pov is saved as it is created, and
     the master streams that save at once (it is acked within milliseconds).  The slave used to
     store such a save with a garbage deadline, so its durable layer could drop the record as
     expired, and a read here failed with "durable object doesn't exist" (#676 follow-up). */
  client.reset();
  this_thread::sleep_for(seconds(3));
  pair.Failover();
  /* Each reads the global value through its parents. */
  for (const Base::TUuid &pov_id : late_ids) {
    Rt::TOpt<int64_t> read = ReadPovWithRetry(pair.SlaveAddr(), pov_id, 91L, seconds(30));
    cout << "read through pov {" << pov_id << "} on the promoted slave: " << (read.IsKnown() ? to_string(read.GetVal()) : string("unknown")) << endl;
    EXPECT_TRUE(read.IsKnown() && read.GetVal() == 9191L);
  }
  pair.Slave->Kill();
  pair.Slave->Reap(seconds(60));
}

/* #680: a pov's durable record reached a slave only when the pov was saved while the two were
   paired; the join sent the repos but not the records.  So after a failover, a pov made before
   the join was refused on the promoted slave (`durable object doesn't exist`), although the slave
   had its repo.  The join now sends the saved records of the povs it inventories.

   Here a client makes a shared pov P and a child D under it before the slave joins, keeps its
   connection open across the join, then closes it, which doesn't save them again.  Each must
   still read the global value on the promoted slave. */
FIXTURE(JoinSendsPovRecords) {
  Orly::Type::TTypeCzar type_czar;
  if (!ifstream(GetOrlyiPath()).good()) {
    throw runtime_error("orlyi binary not built at [" + GetOrlyiPath() + "]; run `make debug` first");
  }
  TLogTailDumper log_dumper;
  /* Open across the join, so the master keeps the povs made before it. */
  shared_ptr<TExerciseClient> client;
  Base::TUuid parent_id, child_id;
  TPairedServers pair(NewSampleScratch(), "join_records", {}, log_dumper, [&](const TAddress &master_addr) {
    client = make_shared<TExerciseClient>(master_addr);
    const Base::TUuid writer_id = **Answered(client->NewFastPrivatePov(std::nullopt, seconds(0)), "NewFastPrivatePov");
    WriteVal(client, writer_id, 93L, 9393L);
    Rt::TOpt<int64_t> in_global = ReadWithRetry(master_addr, 93L, seconds(60));
    EXPECT_TRUE(in_global.IsKnown() && in_global.GetVal() == 9393L);
    parent_id = **Answered(client->NewSafeSharedPov(std::nullopt, seconds(700)), "NewSafeSharedPov (P)");
    child_id = **Answered(client->NewSafeSharedPov(parent_id, seconds(800)), "NewSafeSharedPov (D)");
    for (const Base::TUuid &pov_id : { parent_id, child_id }) {
      Rt::TOpt<int64_t> on_master = ReadVal(client, pov_id, 93L);
      EXPECT_TRUE(on_master.IsKnown() && on_master.GetVal() == 9393L);
    }
  });
  client.reset();
  this_thread::sleep_for(seconds(3));
  EXPECT_TRUE(LogContains(pair.MasterLog, "saved pov record(s)"));
  pair.Failover();
  for (const Base::TUuid &pov_id : { parent_id, child_id }) {
    Rt::TOpt<int64_t> read = ReadPovWithRetry(pair.SlaveAddr(), pov_id, 93L, seconds(30));
    cout << "read through pov {" << pov_id << "} on the promoted slave: " << (read.IsKnown() ? to_string(read.GetVal()) : string("unknown")) << endl;
    EXPECT_TRUE(read.IsKnown() && read.GetVal() == 9393L);
  }
  pair.Slave->Kill();
  pair.Slave->Reap(seconds(60));
}
