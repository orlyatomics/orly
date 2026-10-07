/* <orly/auth.h>

   Optional shared-secret authentication (#710).

   Off unless a token is configured. With a token, orlyi refuses a client
   connection (WebSocket or binary protocol) that does not present it before
   any statement runs, and a master refuses a slave that does not present the
   replication token. With no token, nothing here touches the wire.

   This is a shared secret, not an identity: every client holding it has the
   same rights, and it travels in the clear unless a TLS proxy or tunnel
   carries the connection (see docs/PROTOCOL.md). Never log a token: the
   functions here name where a token came from, never what it is.

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

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Orly {

  namespace Auth {

    /* Bounds on a token's length, in bytes. A token is printable ASCII with no spaces, so it
       survives a file, an environment variable, JSON and a shell unchanged. */
    constexpr std::size_t MinTokenSize = 16;
    constexpr std::size_t MaxTokenSize = 1024;

    /* Where a client token comes from when no flag gives one. */
    constexpr const char *TokenEnvVar = "ORLY_AUTH_TOKEN";
    constexpr const char *TokenFileEnvVar = "ORLY_AUTH_TOKEN_FILE";

    /* Where a replication token comes from when no flag gives one. Without either, replication
       uses the client token, if there is one. */
    constexpr const char *ReplicationTokenEnvVar = "ORLY_REPLICATION_TOKEN";
    constexpr const char *ReplicationTokenFileEnvVar = "ORLY_REPLICATION_TOKEN_FILE";

    /* True iff the two are equal. Takes the same time whatever the expected token is and
       wherever the two first differ: the loop runs over the presented token's length, which the
       presenter already knows. */
    bool TokensEqual(std::string_view presented, std::string_view expected) noexcept;

    /* Throws std::invalid_argument, naming `source` but never the token, unless `token` is
       MinTokenSize to MaxTokenSize bytes of printable ASCII without spaces. */
    void CheckToken(std::string_view token, const std::string &source);

    /* The token in a file: its contents less one trailing newline (\n or \r\n). Throws
       std::runtime_error naming the path (never the contents) if it can't be read. */
    std::string ReadTokenFile(const std::string &path);

    /* Resolves a token from at most one of: a file flag, a value flag, then (only when neither
       flag is set) a file environment variable or a value environment variable. Returns nullopt
       when none is set. Throws std::invalid_argument if two sources at the same level are set,
       or the token fails CheckToken. `flag_name` is the flags' stem ("auth_token" names
       --auth_token and --auth_token_file). */
    std::optional<std::string> ResolveToken(
        const std::string &file_flag, const std::string &value_flag, const char *flag_name,
        const char *file_env, const char *value_env);

    /* The client token a native (binary-protocol) client presents: ORLY_AUTH_TOKEN_FILE or
       ORLY_AUTH_TOKEN, or nullopt. Throws as ResolveToken does. */
    std::optional<std::string> ClientTokenFromEnv();

    /* Replication (#710). A master that requires a token sends ReplicationChallenge to a slave
       as soon as it connects, before anything else; the slave answers with the same 8 bytes,
       the token's length as a big-endian uint16, and the token; the master replies one byte,
       ReplicationAccepted or ReplicationRefused, and closes the connection on a refusal. A
       master without a token sends no challenge, so a tokenless pair's bytes are unchanged. */
    constexpr char ReplicationChallenge[8] = {'O', 'R', 'L', 'Y', 'A', 'U', 'T', 'H'};
    constexpr char ReplicationAccepted = 'A';
    constexpr char ReplicationRefused = 'R';

    /* How long a master waits for a slave's answer to the challenge. */
    constexpr std::chrono::milliseconds ReplicationAnswerTimeout{10000};

    /* The outcome of a master's challenge. */
    enum class TJoinVerdict {
      /* The slave presented the token. */
      Accepted,
      /* The slave presented a different token. */
      Refused,
      /* The slave sent no answer, or not one in this protocol: it has no replication token. */
      NoAnswer
    };

    /* Master side: challenge the slave on `fd` and check its answer against `token`, waiting at
       most `timeout`. Writes the verdict byte for an answer it could read. On anything but
       Accepted, sets `why` to a line for the log (it never contains a token). */
    TJoinVerdict ChallengeSlave(
        int fd, std::string_view token, std::chrono::milliseconds timeout, std::string &why);

    /* Thrown on the slave side when a master refuses its token, or the exchange breaks off. */
    class TReplicationRefused
        : public std::runtime_error {
      public:

      using std::runtime_error::runtime_error;

    };  // TReplicationRefused

    /* Slave side: wait for the master's first bytes on `fd`. If they are the challenge, answer
       with `token` and return true once the master accepts; throw TReplicationRefused if it
       refuses or hangs up. If they are not (a master with no token, which starts replicating
       straight away), consume nothing and return false. */
    bool AnswerMaster(int fd, std::string_view token);

  }  // Auth

}  // Orly
