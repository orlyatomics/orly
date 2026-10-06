/* <base/log.cc>

   Implements <base/log.h>.

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

#include <errno.h>
#include <paths.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

using namespace Base;

/* A syslog() that never blocks on the log daemon (#641).

   glibc's syslog() holds one process-wide lock across the stderr echo and a blocking send() on
   /dev/log. When the daemon stops reading (journald frozen, under memory pressure, behind a slow
   forwarder), the socket's queue fills, the thread inside syslog() blocks in the send, and every
   other thread that logs blocks on the lock. The whole server stops, and since the threads stop
   before they print, the log shows nothing.

   So this file provides the syslog API itself: openlog, closelog, setlogmask, syslog, vsyslog and
   the _FORTIFY_SOURCE entry points. A program that links this object (anything using TLog, so
   every server binary and test) calls these instead of glibc's. They differ from glibc's in two
   ways only:

   - The send to /dev/log never blocks for long. A line that finds the daemon's queue full waits
     up to 50ms for room, then is dropped from the system log and counted
     (TLog::GetDroppedCount()); while the daemon stays stalled, later lines are dropped without
     waiting. The next line that gets through is followed by one notice saying how many were
     dropped.
   - No lock is held while writing: the stderr echo is one write() of the whole line, and the
     send needs only a shared lock, which a reconnect takes exclusively.

   The stderr echo still keeps every line, in the same format as glibc's LOG_PERROR. A write to a
   stderr that nobody reads can still block the thread that logs; that's true of any program that
   writes to stderr, and it no longer stops the threads that don't log. */

namespace {

  const char *SocketPath = _PATH_LOG;

  std::atomic<const char *> Ident{nullptr};
  std::atomic<int> Option{0};
  std::atomic<int> Facility{LOG_USER};
  std::atomic<int> Mask{0xff};

  /* Taken shared to send, exclusively to connect or close, so a socket is never closed (and its
     number reused) under a sender. */
  std::shared_mutex SocketMutex;
  int SocketFd = -1;

  std::atomic<uint64_t> DroppedCount{0};
  std::atomic<uint64_t> ReportedDroppedCount{0};

  /* Call with SocketMutex held exclusively. */
  void CloseSocket() {
    if (SocketFd >= 0) {
      close(SocketFd);
      SocketFd = -1;
    }
  }

  /* Call with SocketMutex held exclusively. */
  void OpenSocket() {
    if (SocketFd >= 0) {
      return;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      return;
    }
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SocketPath, sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
      /* No log daemon (the usual case in a container): the line goes to stderr only, as with
         glibc. Not a drop. */
      close(fd);
      return;
    }
    SocketFd = fd;
  }

  enum class TSent { Ok, Full, Down };

  /* How long a line may wait for room in a full socket before it is dropped. A daemon that is
     merely busy drains its queue within this (the kernel's default queue is only 10 datagrams, so
     a burst of lines fills it easily); a stalled one doesn't, and then Stalled is set and later
     lines don't wait at all until one gets through. So a stall costs one wait, not one per line. */
  constexpr int FullWaitMs = 50;

  std::atomic<bool> Stalled{false};

  /* Never blocks for longer than FullWaitMs. */
  TSent TrySend(const char *buf, size_t size) {
    std::shared_lock<std::shared_mutex> lock(SocketMutex);
    if (SocketFd < 0) {
      return TSent::Down;
    }
    for (bool waited = Stalled.load(); ; waited = true) {
      if (send(SocketFd, buf, size, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) {
        Stalled = false;
        return TSent::Ok;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
        return TSent::Down;
      }
      if (waited) {
        Stalled = true;
        return TSent::Full;
      }
      /* For a connected datagram socket, POLLOUT means the peer's queue has room. */
      pollfd pfd { SocketFd, POLLOUT, 0 };
      poll(&pfd, 1, FullWaitMs);
    }
  }

  /* "<pri>Mmm dd hh:mm:ss ", the part of a line that goes to the socket but not to stderr. */
  size_t FormatPrefix(char *buf, size_t size, int pri) {
    int len = snprintf(buf, size, "<%d>", pri);
    if (len < 0 || static_cast<size_t>(len) >= size) {
      return 0;
    }
    time_t now = time(nullptr);
    struct tm tm;
    if (localtime_r(&now, &tm)) {
      len += strftime(buf + len, size - len, "%h %e %T ", &tm);
    }
    return len;
  }

  /* "ident[pid]: " */
  std::string FormatTag() {
    const char *ident = Ident.load();
    if (!ident) {
      ident = program_invocation_short_name;
    }
    std::string tag(ident ? ident : "");
    if (Option.load() & LOG_PID) {
      tag += "[" + std::to_string(getpid()) + "]";
    }
    tag += ": ";
    return tag;
  }

  void SendToSocket(int pri, const std::string &tag, const char *msg, size_t msg_size) {
    char prefix[64];
    const size_t prefix_size = FormatPrefix(prefix, sizeof(prefix), pri);
    std::string line;
    line.reserve(prefix_size + tag.size() + msg_size);
    line.append(prefix, prefix_size).append(tag).append(msg, msg_size);
    TSent sent = TrySend(line.data(), line.size());
    if (sent == TSent::Down) {
      /* Not connected yet, or the daemon went away (restarted): reconnect and try once more. If
         another thread is reconnecting, don't wait for it. */
      std::unique_lock<std::shared_mutex> lock(SocketMutex, std::try_to_lock);
      if (!lock.owns_lock()) {
        return;
      }
      CloseSocket();
      OpenSocket();
      lock.unlock();
      sent = TrySend(line.data(), line.size());
    }
    if (sent == TSent::Full) {
      ++DroppedCount;
      return;
    }
    if (sent != TSent::Ok) {
      return;
    }
    /* Got through: say how many were dropped since the last notice, once. */
    uint64_t reported = ReportedDroppedCount.load();
    const uint64_t dropped = DroppedCount.load();
    if (dropped == reported || !ReportedDroppedCount.compare_exchange_strong(reported, dropped)) {
      return;
    }
    char notice[160];
    const int notice_size = snprintf(notice, sizeof(notice),
        "%.*s%s[%lu] log line(s) were dropped from the system log: the log socket was full (#641)",
        static_cast<int>(prefix_size), prefix, tag.c_str(), static_cast<unsigned long>(dropped - reported));
    if (notice_size > 0) {
      TrySend(notice, std::min(static_cast<size_t>(notice_size), sizeof(notice) - 1));
    }
  }

  void WriteAll(int fd, const char *buf, size_t size) {
    while (size) {
      const ssize_t written = write(fd, buf, size);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }
      buf += written;
      size -= written;
    }
  }

  void Log(int pri, const char *fmt, va_list args) {
    const int saved_errno = errno;
    if (!(LOG_MASK(LOG_PRI(pri)) & Mask.load())) {
      return;
    }
    if (!(pri & LOG_FACMASK)) {
      pri |= Facility.load();
    }
    /* Format the message. %m reads errno, so restore it first. */
    char stack_buf[1024];
    std::vector<char> heap_buf;
    const char *msg = stack_buf;
    va_list copy;
    va_copy(copy, args);
    errno = saved_errno;
    int msg_size = vsnprintf(stack_buf, sizeof(stack_buf), fmt, copy);
    va_end(copy);
    if (msg_size < 0) {
      errno = saved_errno;
      return;
    }
    if (static_cast<size_t>(msg_size) >= sizeof(stack_buf)) {
      heap_buf.resize(msg_size + 1);
      errno = saved_errno;
      vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args);
      msg = heap_buf.data();
    }
    const std::string tag = FormatTag();
    if (Option.load() & LOG_PERROR) {
      std::string line;
      line.reserve(tag.size() + msg_size + 1);
      line.append(tag).append(msg, msg_size).push_back('\n');
      WriteAll(STDERR_FILENO, line.data(), line.size());
    }
    SendToSocket(pri, tag, msg, msg_size);
    errno = saved_errno;
  }

}  // namespace

extern "C" {

  void openlog(const char *ident, int option, int facility) {
    Ident = ident;
    Option = option;
    if (facility && !(facility & ~LOG_FACMASK)) {
      Facility = facility;
    }
    if (option & LOG_NDELAY) {
      std::unique_lock<std::shared_mutex> lock(SocketMutex);
      OpenSocket();
    }
  }

  void closelog() {
    std::unique_lock<std::shared_mutex> lock(SocketMutex);
    CloseSocket();
    Ident = nullptr;
  }

  int setlogmask(int mask) {
    return mask ? Mask.exchange(mask) : Mask.load();
  }

  void vsyslog(int pri, const char *fmt, va_list args) {
    Log(pri, fmt, args);
  }

  void syslog(int pri, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Log(pri, fmt, args);
    va_end(args);
  }

  /* What <syslog.h> calls under _FORTIFY_SOURCE. The build turns that off, but a translation unit
     compiled with it must not slip back to glibc's blocking syslog. */
  void __vsyslog_chk(int pri, int, const char *fmt, va_list args) {
    Log(pri, fmt, args);
  }

  void __syslog_chk(int pri, int, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Log(pri, fmt, args);
    va_end(args);
  }

}  // extern "C"

uint64_t TLog::GetDroppedCount() {
  return DroppedCount.load();
}

void TLog::SetSocketPath(const char *path) {
  std::unique_lock<std::shared_mutex> lock(SocketMutex);
  CloseSocket();
  SocketPath = path ? path : _PATH_LOG;
}

int TLog::TCmd::GetOpenFlags() const {
  return LOG_PID | (Echo ? LOG_PERROR : 0);
}

int TLog::TCmd::GetMask() const {
  int result = LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR);
  if (All) {
    if (Info) {
      result |= LOG_UPTO(LOG_INFO);
    } else if (Notice) {
      result |= LOG_UPTO(LOG_NOTICE);
    } else if (Warning) {
      result |= LOG_UPTO(LOG_WARNING);
    } else {
      result |= LOG_UPTO(LOG_DEBUG);
    }
  } else {
    if (Warning) {
      result |= LOG_MASK(LOG_WARNING);
    }
    if (Notice) {
      result |= LOG_MASK(LOG_NOTICE);
    }
    if (Info) {
      result |= LOG_MASK(LOG_INFO);
    }
    if (Debug) {
      result |= LOG_MASK(LOG_DEBUG);
    }
  }
  return result;
}

TLog::TCmd::TMeta::TMeta(const char *desc)
    : Base::TCmd::TMeta(desc) {
  Param(&TCmd::Echo,    "log_echo",    Optional, "log_echo\0le\0", "Echo the log to stderr.");
  Param(&TCmd::All,     "log_all",     Optional, "log_all\0la\0", "Log all messages, or all messages up to the given level.");
  Param(&TCmd::Warning, "log_warning", Optional, "log_warning\0lw\0", "Log warning messages, or up to warning messages (if --log_all).");
  Param(&TCmd::Notice,  "log_notice",  Optional, "log_notice\0ln\0", "Log notice messages, or up to notice messages (if --log_all).");
  Param(&TCmd::Info,    "log_info",    Optional, "log_info\0li\0", "Log info messages, or up to info messages (if --log_all).");
  Param(&TCmd::Debug,   "log_debug",   Optional, "log_debug\0ld\0", "Log debug messages, or up to debug messages (if --log_all).");
}

TLog::TLog(const TCmd &cmd) {
  openlog(cmd.GetProg(), cmd.GetOpenFlags(), LOG_USER);
  setlogmask(cmd.GetMask());
  syslog(LOG_NOTICE, "log started");
}

TLog::~TLog() {
  syslog(LOG_NOTICE, "log stopped");
  closelog();
}
