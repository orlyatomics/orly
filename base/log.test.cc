/* <base/log.test.cc>

   Unit test for <base/event_semaphore.h>.

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

#include <base/log.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

#include <base/test/kit.h>

using namespace std;
using namespace Base;

namespace {

  /* A datagram socket standing in for /dev/log, bound in a fresh directory. */
  class TFakeLogDaemon {
    public:

    TFakeLogDaemon() {
      char dir[] = "/tmp/log_test_XXXXXX";
      if (!mkdtemp(dir)) {
        throw runtime_error("mkdtemp failed");
      }
      Dir = dir;
      Path = Dir + "/log";
      Fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      strncpy(addr.sun_path, Path.c_str(), sizeof(addr.sun_path) - 1);
      if (Fd < 0 || bind(Fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        throw runtime_error("bind failed");
      }
    }

    ~TFakeLogDaemon() {
      close(Fd);
      unlink(Path.c_str());
      rmdir(Dir.c_str());
    }

    /* Read everything queued, without blocking, and return it joined by newlines. */
    string Drain() {
      string result;
      char buf[4096];
      for (;;) {
        const ssize_t size = recv(Fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (size < 0) {
          return result;
        }
        result.append(buf, size).push_back('\n');
      }
    }

    string Dir, Path;

    int Fd;

  };  // TFakeLogDaemon

  /* Run the logging in a thread we can abandon: if syslog() blocks, the test reports it instead
     of hanging until the runner's timeout. */
  bool FinishesWithin(const function<void ()> &func, chrono::seconds limit) {
    auto promise = make_shared<std::promise<void>>();
    auto done = promise->get_future();
    thread([promise, func] {
      func();
      promise->set_value();
    }).detach();
    return done.wait_for(limit) == future_status::ready;
  }

}  // namespace

/* The log daemon stops reading while the program logs heavily (#641). With glibc's syslog() the
   first line that finds the socket full blocks in send(), holding the lock every other logging
   thread then waits on. Here every line must return, the overflow must be counted, and once the
   daemon reads again a line must arrive with a notice of what was dropped. */
FIXTURE(StalledDaemonDoesNotBlock) {
  TFakeLogDaemon daemon;
  TLog::SetSocketPath(daemon.Path.c_str());
  openlog("log.test", LOG_PID | LOG_NDELAY, LOG_USER);
  const int old_mask = setlogmask(LOG_UPTO(LOG_DEBUG));
  const uint64_t dropped_before = TLog::GetDroppedCount();
  /* Several threads, as in the server: with glibc, the ones that don't hold the lock block too. */
  const bool finished = FinishesWithin([] {
    vector<thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([t] {
        for (int i = 0; i < 5000; ++i) {
          syslog(LOG_INFO, "thread %d line %d", t, i);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
  }, chrono::seconds(30));
  if (!finished) {
    /* The logging threads are stuck in send() and can't be joined. */
    fprintf(stderr, "syslog() blocked on a full log socket (#641)\n");
    _exit(1);
  }
  const uint64_t dropped = TLog::GetDroppedCount() - dropped_before;
  EXPECT_GT(dropped, 0UL);
  /* The daemon catches up. */
  const string backlog = daemon.Drain();
  EXPECT_NE(backlog.find("log.test["), string::npos);
  syslog(LOG_ERR, "after the stall, errno says %m");
  const string after = daemon.Drain();
  EXPECT_NE(after.find("after the stall"), string::npos);
  EXPECT_NE(after.find("log line(s) were dropped from the system log"), string::npos);
  EXPECT_NE(after.find("[" + to_string(dropped) + "] log line(s)"), string::npos);
  setlogmask(old_mask);
  closelog();
  TLog::SetSocketPath(nullptr);
}

/* The format the daemon gets matches glibc's: "<pri>Mmm dd hh:mm:ss ident[pid]: message", and the
   mask filters. */
FIXTURE(Format) {
  TFakeLogDaemon daemon;
  TLog::SetSocketPath(daemon.Path.c_str());
  openlog("log.test", LOG_PID, LOG_DAEMON);
  const int old_mask = setlogmask(LOG_UPTO(LOG_NOTICE));
  syslog(LOG_INFO, "masked out");
  errno = ENOENT;
  syslog(LOG_WARNING, "value %d: %m", 42);
  const string got = daemon.Drain();
  EXPECT_EQ(got.find("masked out"), string::npos);
  const string expected_pri = "<" + to_string(LOG_DAEMON | LOG_WARNING) + ">";
  EXPECT_EQ(got.compare(0, expected_pri.size(), expected_pri), 0);
  const string expected_tail = "log.test[" + to_string(getpid()) + "]: value 42: No such file or directory\n";
  EXPECT_TRUE(got.size() >= expected_tail.size() &&
              got.compare(got.size() - expected_tail.size(), expected_tail.size(), expected_tail) == 0);
  setlogmask(old_mask);
  closelog();
  TLog::SetSocketPath(nullptr);
}
