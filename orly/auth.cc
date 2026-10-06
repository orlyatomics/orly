/* <orly/auth.cc>

   Implements <orly/auth.h>.

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

#include <orly/auth.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <base/util/io.h>

using namespace std;
using namespace Orly::Auth;

bool Orly::Auth::TokensEqual(string_view presented, string_view expected) noexcept {
  /* Fold every byte of the presented token into `diff`, comparing it with the expected byte at
     the same position (or a stand-in past the end). No early exit, and no branch on the data. */
  unsigned diff = (presented.size() == expected.size()) ? 0U : 1U;
  const size_t expected_size = expected.size();
  for (size_t i = 0; i < presented.size(); ++i) {
    const unsigned char want = expected_size ? static_cast<unsigned char>(expected[i % expected_size]) : 0;
    diff |= static_cast<unsigned char>(presented[i]) ^ want;
  }
  return diff == 0 && !expected.empty();
}

void Orly::Auth::CheckToken(string_view token, const string &source) {
  if (token.size() < MinTokenSize || token.size() > MaxTokenSize) {
    ostringstream strm;
    strm << source << ": a token must be " << MinTokenSize << " to " << MaxTokenSize
         << " bytes; this one is " << token.size() << " (generate one with `openssl rand -hex 32`)";
    throw invalid_argument(strm.str());
  }
  for (const char c : token) {
    if (c < 0x21 || c > 0x7e) {
      throw invalid_argument(
          source + ": a token must be printable ASCII with no spaces (generate one with `openssl rand -hex 32`)");
    }
  }
}

string Orly::Auth::ReadTokenFile(const string &path) {
  ifstream strm(path, ios::binary);
  if (!strm) {
    throw runtime_error("cannot read token file \"" + path + "\": " + strerror(errno));
  }
  string token((istreambuf_iterator<char>(strm)), istreambuf_iterator<char>());
  if (strm.bad()) {
    throw runtime_error("cannot read token file \"" + path + "\"");
  }
  if (!token.empty() && token.back() == '\n') {
    token.pop_back();
    if (!token.empty() && token.back() == '\r') {
      token.pop_back();
    }
  }
  return token;
}

optional<string> Orly::Auth::ResolveToken(
    const string &file_flag, const string &value_flag, const char *flag_name,
    const char *file_env, const char *value_env) {
  const string file_flag_name = string("--") + flag_name + "_file";
  const string value_flag_name = string("--") + flag_name;
  if (!file_flag.empty() && !value_flag.empty()) {
    throw invalid_argument("give " + file_flag_name + " or " + value_flag_name + ", not both");
  }
  string token, source;
  if (!file_flag.empty()) {
    source = file_flag_name;
    token = ReadTokenFile(file_flag);
  } else if (!value_flag.empty()) {
    source = value_flag_name;
    token = value_flag;
  } else {
    const char *file_env_value = getenv(file_env);
    const char *value_env_value = getenv(value_env);
    const bool has_file_env = file_env_value && *file_env_value;
    const bool has_value_env = value_env_value && *value_env_value;
    if (has_file_env && has_value_env) {
      throw invalid_argument(string("set ") + file_env + " or " + value_env + ", not both");
    }
    if (has_file_env) {
      source = file_env;
      token = ReadTokenFile(file_env_value);
    } else if (has_value_env) {
      source = value_env;
      token = value_env_value;
    } else {
      return nullopt;
    }
  }
  CheckToken(token, source);
  return token;
}

optional<string> Orly::Auth::ClientTokenFromEnv() {
  return ResolveToken("", "", "auth_token", TokenFileEnvVar, TokenEnvVar);
}

TJoinVerdict Orly::Auth::ChallengeSlave(
    int fd, string_view token, chrono::milliseconds timeout, string &why) {
  /* Everything a peer does here, hanging up included, ends in a verdict, never an exception: the
     master's listener must survive whatever connects to it. */
  try {
    Util::WriteExactly(fd, ReplicationChallenge, sizeof(ReplicationChallenge), timeout);
    char intro[sizeof(ReplicationChallenge)];
    if (!Util::TryReadExactly(fd, intro, sizeof(intro), timeout)) {
      why = "the slave hung up without answering the replication challenge; is it started with the replication token?";
      return TJoinVerdict::NoAnswer;
    }
    if (memcmp(intro, ReplicationChallenge, sizeof(intro)) != 0) {
      why = "the slave did not answer the replication challenge; is it started with the replication token?";
      return TJoinVerdict::NoAnswer;
    }
    uint16_t nbo_size;
    Util::ReadExactly(fd, &nbo_size, sizeof(nbo_size), timeout);
    const size_t size = ntohs(nbo_size);
    if (size > MaxTokenSize) {
      why = "the slave's replication token is longer than any this server accepts";
      Util::TryWriteExactly(fd, &ReplicationRefused, 1, timeout);
      return TJoinVerdict::Refused;
    }
    string presented(size, '\0');
    if (size) {
      Util::ReadExactly(fd, presented.data(), size, timeout);
    }
    if (!TokensEqual(presented, token)) {
      why = "the slave presented a different replication token";
      Util::TryWriteExactly(fd, &ReplicationRefused, 1, timeout);
      return TJoinVerdict::Refused;
    }
    Util::WriteExactly(fd, &ReplicationAccepted, 1, timeout);
    return TJoinVerdict::Accepted;
  } catch (const system_error &err) {
    if (err.code().value() == ETIMEDOUT) {
      why = "the slave did not answer the replication challenge within " + to_string(timeout.count()) +
            " ms; is it started with the replication token?";
      return TJoinVerdict::NoAnswer;
    }
    why = string("the replication challenge failed: ") + err.what();
    return TJoinVerdict::NoAnswer;
  } catch (const exception &ex) {
    why = string("the replication challenge failed: ") + ex.what();
    return TJoinVerdict::NoAnswer;
  }
}

bool Orly::Auth::AnswerMaster(int fd, string_view token) {
  /* Peek, so that a master without a token, which starts replicating at once, finds its bytes
     still in the socket for the replication protocol. */
  char intro[sizeof(ReplicationChallenge)];
  for (;;) {
    const ssize_t got = recv(fd, intro, sizeof(intro), MSG_PEEK);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw system_error(errno, system_category(), "peeking at the master's first bytes");
    }
    if (got == 0) {
      throw TReplicationRefused(
          "the master closed the connection before replication started; check its log, and that "
          "both sides have the same replication token");
    }
    if (memcmp(intro, ReplicationChallenge, static_cast<size_t>(got)) != 0) {
      return false;
    }
    if (static_cast<size_t>(got) == sizeof(intro)) {
      break;
    }
    /* A prefix of the challenge so far: wait for the rest. */
    this_thread::sleep_for(chrono::milliseconds(10));
  }
  Util::ReadExactly(fd, intro, sizeof(intro));
  if (token.size() > MaxTokenSize) {
    throw invalid_argument("replication token too long");
  }
  string answer(ReplicationChallenge, sizeof(ReplicationChallenge));
  const uint16_t nbo_size = htons(static_cast<uint16_t>(token.size()));
  answer.append(reinterpret_cast<const char *>(&nbo_size), sizeof(nbo_size));
  answer.append(token);
  Util::WriteExactly(fd, answer.data(), answer.size());
  char verdict = 0;
  if (!Util::TryReadExactly(fd, &verdict, 1)) {
    throw TReplicationRefused("the master hung up after the replication challenge without a verdict");
  }
  if (verdict == ReplicationRefused) {
    throw TReplicationRefused("the master refused this slave's replication token");
  }
  if (verdict != ReplicationAccepted) {
    throw TReplicationRefused("the master answered the replication challenge with an unknown verdict");
  }
  return true;
}
