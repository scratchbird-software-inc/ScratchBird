// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "handoff_claim_reader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace scratchbird::listener {

#ifndef _WIN32
namespace {

constexpr std::size_t kMaxClaimLineBytes = 512u;

int RemainingTimeoutMs(const std::chrono::steady_clock::time_point& deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return 0;
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(std::min<long long>(remaining, static_cast<long long>(std::numeric_limits<int>::max())));
}

bool WaitReadable(int fd, int timeout_ms, bool* timed_out) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  for (;;) {
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc > 0) return (pfd.revents & POLLIN) != 0;
    if (rc == 0) {
      if (timed_out) *timed_out = true;
      return false;
    }
    if (errno == EINTR) continue;
    return false;
  }
}

bool ConsumeBytes(int fd, std::size_t bytes) {
  std::array<char, 128> buffer{};
  std::size_t consumed = 0;
  while (consumed < bytes) {
    const auto want = std::min(buffer.size(), bytes - consumed);
    const ssize_t got = ::recv(fd, buffer.data(), want, 0);
    if (got > 0) {
      consumed += static_cast<std::size_t>(got);
      continue;
    }
    if (got < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

} // namespace

HandoffClaimReadResult ReadOptionalLprefaceHandoffClaimFromSocket(
    int fd,
    ParserHandoffClientEvidence base_evidence,
    std::uint32_t timeout_ms) {
  HandoffClaimReadResult result;
  result.evidence = std::move(base_evidence);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string peeked;
  peeked.reserve(kMaxClaimLineBytes);

  for (;;) {
    bool timed_out = false;
    if (!WaitReadable(fd, RemainingTimeoutMs(deadline), &timed_out)) {
      result.timed_out = timed_out;
      return result;
    }

    std::array<char, kMaxClaimLineBytes> buffer{};
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), MSG_PEEK);
    if (got == 0) return result;
    if (got < 0) {
      if (errno == EINTR) continue;
      return result;
    }
    peeked.assign(buffer.data(), static_cast<std::size_t>(got));
    if (!proto::IsLprefaceHandoffClaimPrefix(peeked)) {
      result.non_claim_input = true;
      return result;
    }

    const auto newline = peeked.find('\n');
    if (newline != std::string::npos) {
      result.recognized = true;
      const auto consume_len = newline + 1u;
      std::vector<proto::Diagnostic> diagnostics;
      const auto claim = proto::DecodeLprefaceHandoffClaim(std::string_view(peeked.data(), newline), &diagnostics);
      result.consumed = ConsumeBytes(fd, consume_len);
      if (!claim) {
        result.malformed = true;
        result.diagnostic_code = diagnostics.empty() ? "LPREFACE.CLAIM_INVALID" : diagnostics.front().code;
        return result;
      }
      result.evidence.client_nonce = claim->client_nonce;
      result.evidence.server_nonce = claim->server_nonce;
      return result;
    }

    if (peeked.size() >= kMaxClaimLineBytes || std::chrono::steady_clock::now() >= deadline) {
      result.recognized = true;
      result.malformed = true;
      result.timed_out = std::chrono::steady_clock::now() >= deadline;
      result.diagnostic_code = "LPREFACE.CLAIM_TRUNCATED";
      return result;
    }
  }
}
#endif

} // namespace scratchbird::listener
