// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "handoff_claim_reader.hpp"
#include "listener_runtime.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

scratchbird::listener::ListenerConfig TestConfig() {
  scratchbird::listener::ListenerConfig config;
  config.protocol_family = "sbsql";
  config.server_endpoint = "unix:/tmp/eler060-test-sbps.sock";
  config.database_selector = "dev_bootstrap_path:/tmp/eler060-test.sbdb";
  config.port = 0;
  config.enable_accept_loop = false;
  return config;
}

scratchbird::listener::proto::DbbtToken Token(std::uint8_t marker,
                                              scratchbird::listener::proto::Bytes client_nonce,
                                              scratchbird::listener::proto::Bytes server_nonce,
                                              std::uint64_t expires_at_ms) {
  scratchbird::listener::proto::DbbtToken token;
  for (std::size_t i = 0; i < token.db_uuid.size(); ++i) {
    token.db_uuid[i] = static_cast<std::uint8_t>(marker + i);
  }
  for (std::size_t i = 0; i < token.manager_session_id.size(); ++i) {
    token.manager_session_id[i] = static_cast<std::uint8_t>(0x80 + marker + i);
  }
  for (std::size_t i = 0; i < token.mac.size(); ++i) {
    token.mac[i] = static_cast<std::uint8_t>(0x40 + marker + i);
  }
  token.listener_id = 1;
  token.issued_at_ms = 1000;
  token.expires_at_ms = expires_at_ms;
  token.client_nonce = std::move(client_nonce);
  token.server_nonce = std::move(server_nonce);
  return token;
}

scratchbird::listener::proto::Lpreface Preface(const std::string& principal) {
  scratchbird::listener::proto::Lpreface preface;
  preface.listener_id = 1;
  preface.db_selector = "dev_bootstrap_path:/tmp/eler060-test.sbdb";
  preface.requested_profile = "SBsql";
  preface.auth_provider_family = "security_database_temporary_token";
  preface.auth_principal = principal;
  preface.auth_token = principal + "-temporary-token";
  return preface;
}

scratchbird::listener::proto::Bytes Nonce(std::uint8_t marker) {
  scratchbird::listener::proto::Bytes nonce;
  nonce.reserve(16);
  for (std::uint8_t i = 0; i < 16; ++i) {
    nonce.push_back(static_cast<std::uint8_t>(marker + i));
  }
  return nonce;
}

scratchbird::listener::ParserHandoffClientEvidence Evidence(
    scratchbird::listener::proto::Bytes client_nonce,
    scratchbird::listener::proto::Bytes server_nonce) {
  scratchbird::listener::ParserHandoffClientEvidence evidence;
  evidence.client_nonce = std::move(client_nonce);
  evidence.server_nonce = std::move(server_nonce);
  evidence.has_client_endpoint = true;
  evidence.client_addr = "127.0.0.1";
  evidence.client_port = 50001;
  return evidence;
}

#ifndef _WIN32
struct SocketPair {
  int read_fd{-1};
  int write_fd{-1};
  ~SocketPair() {
    if (read_fd >= 0) ::close(read_fd);
    if (write_fd >= 0) ::close(write_fd);
  }
};

SocketPair MakeSocketPair() {
  int fds[2]{-1, -1};
  Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
        std::string("socketpair must succeed: ") + std::strerror(errno));
  return SocketPair{fds[0], fds[1]};
}

void SendAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto rc = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (rc > 0) {
      sent += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    Check(false, std::string("send must succeed: ") + std::strerror(errno));
  }
}

std::string RecvBytes(int fd, std::size_t bytes) {
  std::string out(bytes, '\0');
  std::size_t got = 0;
  while (got < bytes) {
    const auto rc = ::recv(fd, out.data() + got, bytes - got, 0);
    if (rc > 0) {
      got += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    Check(false, std::string("recv must succeed: ") + std::strerror(errno));
  }
  return out;
}

void TestSocketClaimReader() {
  using scratchbird::listener::ParserHandoffClientEvidence;
  using scratchbird::listener::ReadOptionalLprefaceHandoffClaimFromSocket;
  using scratchbird::listener::proto::EncodeLprefaceHandoffClaim;
  using scratchbird::listener::proto::Hex;

  const auto client_nonce = Nonce(0x70);
  const auto server_nonce = Nonce(0x90);

  {
    auto sockets = MakeSocketPair();
    SendAll(sockets.write_fd, EncodeLprefaceHandoffClaim(client_nonce, server_nonce) + "SBPS");
    ParserHandoffClientEvidence base;
    base.has_client_endpoint = true;
    base.client_addr = "127.0.0.1";
    base.client_port = 50100;
    const auto read = ReadOptionalLprefaceHandoffClaimFromSocket(sockets.read_fd, base, 1000);
    Check(read.recognized, "valid LPREFACE claim line must be recognized");
    Check(read.consumed, "valid LPREFACE claim line must be consumed");
    Check(!read.malformed, "valid LPREFACE claim line must not be malformed");
    Check(read.evidence.client_nonce == client_nonce &&
              read.evidence.server_nonce == server_nonce,
          "valid LPREFACE claim line must populate both nonces");
    Check(RecvBytes(sockets.read_fd, 4) == "SBPS",
          "reader must leave parser protocol bytes after the claim untouched");
  }

  {
    auto sockets = MakeSocketPair();
    SendAll(sockets.write_fd, "SBPS-HELLO");
    ParserHandoffClientEvidence base;
    const auto read = ReadOptionalLprefaceHandoffClaimFromSocket(sockets.read_fd, base, 100);
    Check(!read.recognized && read.non_claim_input,
          "ordinary parser input must not be treated as an LPREFACE claim");
    Check(RecvBytes(sockets.read_fd, 10) == "SBPS-HELLO",
          "non-claim parser input must not be consumed by the claim reader");
  }

  {
    auto sockets = MakeSocketPair();
    SendAll(sockets.write_fd,
            "SB-LPREFACE-CLAIM/1 client_nonce=zz server_nonce=" + Hex(server_nonce) + "\nTAIL");
    ParserHandoffClientEvidence base;
    const auto read = ReadOptionalLprefaceHandoffClaimFromSocket(sockets.read_fd, base, 1000);
    Check(read.recognized, "malformed LPREFACE claim prefix must be recognized");
    Check(read.consumed, "malformed LPREFACE claim line must be consumed before rejection");
    Check(read.malformed, "malformed LPREFACE claim must fail closed");
    Check(!read.diagnostic_code.empty(), "malformed LPREFACE claim must expose a diagnostic code");
    Check(RecvBytes(sockets.read_fd, 4) == "TAIL",
          "malformed claim consumption must remain bounded to one line");
  }
}
#endif

} // namespace

int main() {
  using scratchbird::listener::ListenerRuntime;
  using scratchbird::listener::proto::CurrentEpochMilliseconds;

  ListenerRuntime runtime(TestConfig());
  const auto future = CurrentEpochMilliseconds() + 60000;
  const auto client_a = Nonce(0x10);
  const auto server_a = Nonce(0x30);
  const auto client_b = Nonce(0x50);
  const auto server_b = Nonce(0x60);

  runtime.QueuePendingHandoffBindingForTest(Token(0x10, client_a, server_a, future),
                                            Preface("client-a"));
  runtime.QueuePendingHandoffBindingForTest(Token(0x20, client_b, server_b, future),
                                            Preface("client-b"));
  Check(runtime.PendingHandoffBindingCountForTest() == 2,
        "two pending DBBT/LPREFACE bindings must be visible before selection");

  const auto handoff_start = std::chrono::steady_clock::now();
  auto claimed_b = runtime.TakePendingHandoffBindingForTest(Evidence(client_b, server_b));
  const auto handoff_stop = std::chrono::steady_clock::now();
  const auto handoff_claim_latency_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          handoff_stop - handoff_start).count();
  Check(handoff_claim_latency_us >= 0,
        "listener accept-to-handoff latency measurement must be non-negative");
  Check(handoff_claim_latency_us < 100000,
        "listener accept-to-handoff latency budget exceeded");
  Check(claimed_b.present, "second queued binding must be claimable out of order by its nonce");
  Check(claimed_b.auth_principal == "client-b",
        "out-of-order claim must not steal the first FIFO binding");
  Check(runtime.PendingHandoffBindingCountForTest() == 1,
        "first binding must remain queued after out-of-order second claim");

  auto single_nonce_wrong_server = runtime.TakePendingHandoffBindingForTest(Evidence(client_a, Nonce(0xe0)));
  Check(!single_nonce_wrong_server.present,
        "matching only the client nonce must not authorize a DBBT/LPREFACE binding");
  Check(runtime.PendingHandoffBindingCountForTest() == 1,
        "partial nonce evidence must leave the original binding queued");

  auto wrong = runtime.TakePendingHandoffBindingForTest(Evidence(Nonce(0xa0), Nonce(0xb0)));
  Check(!wrong.present, "unrelated client evidence must not consume a pending binding");
  Check(runtime.PendingHandoffBindingCountForTest() == 1,
        "mismatched evidence must leave the original binding queued");

  scratchbird::listener::ParserHandoffClientEvidence missing;
  auto missing_claim = runtime.TakePendingHandoffBindingForTest(missing);
  Check(!missing_claim.present, "missing client evidence must refuse pending DBBT/LPREFACE binding");
  Check(runtime.PendingHandoffBindingCountForTest() == 1,
        "missing evidence refusal must not consume the binding");

  auto claimed_a = runtime.TakePendingHandoffBindingForTest(Evidence(client_a, server_a));
  Check(claimed_a.present, "original FIFO head must still be claimable by the correct evidence");
  Check(claimed_a.auth_principal == "client-a",
        "correct evidence must claim the original binding");
  Check(runtime.PendingHandoffBindingCountForTest() == 0,
        "all live bindings must be consumed after the correct client claims them");

  ListenerRuntime expiry_runtime(TestConfig());
  const auto past = CurrentEpochMilliseconds() - 1;
  const auto expired_client = Nonce(0xc0);
  const auto expired_server = Nonce(0xd0);
  expiry_runtime.QueuePendingHandoffBindingForTest(Token(0x30, expired_client, expired_server, past),
                                                   Preface("expired"));
  Check(expiry_runtime.PendingHandoffBindingCountForTest() == 0,
        "expired binding must not count as pending live work");
  auto expired = expiry_runtime.TakePendingHandoffBindingForTest(Evidence(expired_client, expired_server));
  Check(!expired.present, "expired binding must not be claimable even with matching evidence");
  Check(expiry_runtime.PendingHandoffBindingCountForTest() == 0,
        "expired binding pruning must leave no pending live binding");

#ifndef _WIN32
  TestSocketClaimReader();
#endif

  std::cout << "ELER-060 DBBT/LPREFACE exact binding conformance passed "
            << "listener_accept_to_handoff_latency_us="
            << handoff_claim_latency_us << '\n';
  return 0;
}
