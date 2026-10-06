/* <orly/auth.test.cc>

   Unit test for <orly/auth.h>.

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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <base/fd.h>
#include <base/util/io.h>
#include <base/test/kit.h>

using namespace std;
using namespace Orly::Auth;

static const string Token = "0123456789abcdef-the-token";

FIXTURE(TokensEqual) {
  EXPECT_TRUE(TokensEqual(Token, Token));
  EXPECT_FALSE(TokensEqual("0123456789abcdef-the-tokeN", Token));
  EXPECT_FALSE(TokensEqual(Token.substr(0, Token.size() - 1), Token));
  EXPECT_FALSE(TokensEqual(Token + "x", Token));
  EXPECT_FALSE(TokensEqual("", Token));
  /* A token that repeats the expected one (the loop wraps around it) is still wrong. */
  EXPECT_FALSE(TokensEqual(Token + Token, Token));
  /* Nothing matches an empty expected token. */
  EXPECT_FALSE(TokensEqual("", ""));
}

FIXTURE(CheckToken) {
  CheckToken(Token, "test");
  EXPECT_THROW(invalid_argument, [] { CheckToken("short", "test"); });
  EXPECT_THROW(invalid_argument, [] { CheckToken(string(MaxTokenSize + 1, 'a'), "test"); });
  EXPECT_THROW(invalid_argument, [] { CheckToken("0123456789abcdef with a space", "test"); });
  EXPECT_THROW(invalid_argument, [] { CheckToken("0123456789abcdef\twith-a-tab", "test"); });
  /* The message names the source, never the token. */
  try {
    CheckToken("0123456789abcdef secret", "--auth_token_file");
  } catch (const invalid_argument &ex) {
    const string what = ex.what();
    EXPECT_NE(what.find("--auth_token_file"), string::npos);
    EXPECT_EQ(what.find("secret"), string::npos);
  }
}

/* A scratch file that removes itself. */
class TScratchFile {
  public:

  explicit TScratchFile(const string &contents) {
    char path[] = "/tmp/orly_auth_test_XXXXXX";
    const int fd = mkstemp(path);
    Path = path;
    close(fd);
    ofstream(Path, ios::binary) << contents;
  }

  ~TScratchFile() {
    unlink(Path.c_str());
  }

  string Path;
};

FIXTURE(ReadTokenFile) {
  EXPECT_EQ(ReadTokenFile(TScratchFile(Token).Path), Token);
  EXPECT_EQ(ReadTokenFile(TScratchFile(Token + "\n").Path), Token);
  EXPECT_EQ(ReadTokenFile(TScratchFile(Token + "\r\n").Path), Token);
  /* Only one trailing newline goes, so a file with two is rejected by CheckToken later. */
  EXPECT_EQ(ReadTokenFile(TScratchFile(Token + "\n\n").Path), Token + "\n");
  EXPECT_THROW(runtime_error, [] { ReadTokenFile("/nonexistent/orly/token"); });
}

FIXTURE(ResolveToken) {
  const char *file_env = "ORLY_AUTH_TEST_TOKEN_FILE", *value_env = "ORLY_AUTH_TEST_TOKEN";
  unsetenv(file_env);
  unsetenv(value_env);
  TScratchFile file(Token + "\n");
  /* Nothing set: off. */
  EXPECT_FALSE(ResolveToken("", "", "auth_token", file_env, value_env).has_value());
  /* Each source on its own. */
  EXPECT_EQ(*ResolveToken(file.Path, "", "auth_token", file_env, value_env), Token);
  EXPECT_EQ(*ResolveToken("", Token, "auth_token", file_env, value_env), Token);
  setenv(value_env, Token.c_str(), 1);
  EXPECT_EQ(*ResolveToken("", "", "auth_token", file_env, value_env), Token);
  /* A flag beats the environment. */
  EXPECT_EQ(*ResolveToken("", "0123456789abcdef-from-the-flag", "auth_token", file_env, value_env),
            "0123456789abcdef-from-the-flag");
  unsetenv(value_env);
  setenv(file_env, file.Path.c_str(), 1);
  EXPECT_EQ(*ResolveToken("", "", "auth_token", file_env, value_env), Token);
  /* Two at the same level is an error. */
  setenv(value_env, Token.c_str(), 1);
  auto both_envs = [&] { ResolveToken("", "", "auth_token", file_env, value_env); };
  EXPECT_THROW_FUNC(invalid_argument, both_envs);
  auto both_flags = [&] { ResolveToken(file.Path, Token, "auth_token", file_env, value_env); };
  EXPECT_THROW_FUNC(invalid_argument, both_flags);
  unsetenv(file_env);
  unsetenv(value_env);
  /* A bad token is an error wherever it comes from. */
  auto short_token = [&] { ResolveToken("", "short", "auth_token", file_env, value_env); };
  EXPECT_THROW_FUNC(invalid_argument, short_token);
}

/* Runs the master's challenge on one end of a socket pair and `slave` on the other. */
template <typename TSlave>
static TJoinVerdict Join(const string &master_token, const TSlave &slave, string &why) {
  Base::TFd master_fd, slave_fd;
  Base::TFd::SocketPair(master_fd, slave_fd, AF_UNIX, SOCK_STREAM);
  thread slave_thread([&slave, &slave_fd] {
    slave(slave_fd);
  });
  const auto verdict = ChallengeSlave(master_fd, master_token, chrono::milliseconds(2000), why);
  master_fd.Reset();
  slave_thread.join();
  return verdict;
}

FIXTURE(ReplicationMatchingToken) {
  bool answered = false;
  string why;
  EXPECT_TRUE(Join(Token, [&answered](int fd) { answered = AnswerMaster(fd, Token); }, why) == TJoinVerdict::Accepted);
  EXPECT_TRUE(answered);
}

FIXTURE(ReplicationMismatchedToken) {
  bool refused = false;
  string why;
  const auto verdict = Join(Token, [&refused](int fd) {
    try {
      AnswerMaster(fd, "0123456789abcdef-another-token");
    } catch (const TReplicationRefused &) {
      refused = true;
    }
  }, why);
  EXPECT_TRUE(verdict == TJoinVerdict::Refused);
  EXPECT_TRUE(refused);
  EXPECT_NE(why.find("different replication token"), string::npos);
  EXPECT_EQ(why.find("another-token"), string::npos);
}

/* A slave with no token doesn't answer: it reads the challenge as replication traffic and,
   here, hangs up. */
FIXTURE(ReplicationSlaveWithoutToken) {
  string why;
  const auto verdict = Join(Token, [](int fd) {
    char buf[sizeof(ReplicationChallenge)];
    Util::TryReadExactly(fd, buf, sizeof(buf));
    shutdown(fd, SHUT_RDWR);
  }, why);
  EXPECT_TRUE(verdict == TJoinVerdict::NoAnswer);
  EXPECT_NE(why.find("replication token"), string::npos);
}

/* A slave with a token, joining a master without one, leaves the master's replication bytes
   unread and goes ahead without a token. */
FIXTURE(ReplicationMasterWithoutToken) {
  Base::TFd master_fd, slave_fd;
  Base::TFd::SocketPair(master_fd, slave_fd, AF_UNIX, SOCK_STREAM);
  const string traffic = "replication traffic, not a challenge";
  Util::WriteExactly(master_fd, traffic.data(), traffic.size());
  EXPECT_FALSE(AnswerMaster(slave_fd, Token));
  string got(traffic.size(), '\0');
  Util::ReadExactly(slave_fd, got.data(), got.size());
  EXPECT_EQ(got, traffic);
}

/* A master that hangs up before saying anything is reported as such, not taken as consent. */
FIXTURE(ReplicationMasterHangsUp) {
  Base::TFd master_fd, slave_fd;
  Base::TFd::SocketPair(master_fd, slave_fd, AF_UNIX, SOCK_STREAM);
  master_fd.Reset();
  auto answer = [&] { AnswerMaster(slave_fd, Token); };
  EXPECT_THROW_FUNC(TReplicationRefused, answer);
}

/* A peer that connects and hangs up at once is a refusal, not an exception out of the master. */
FIXTURE(ReplicationPeerGoneBeforeChallenge) {
  Base::TFd master_fd, slave_fd;
  Base::TFd::SocketPair(master_fd, slave_fd, AF_UNIX, SOCK_STREAM);
  slave_fd.Reset();
  string why;
  EXPECT_TRUE(ChallengeSlave(master_fd, Token, chrono::milliseconds(2000), why) == TJoinVerdict::NoAnswer);
  EXPECT_FALSE(why.empty());
}
