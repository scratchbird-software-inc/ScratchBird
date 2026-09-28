// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "control_plane.hpp"
#include <chrono>
#include <cstdint>
#include <span>

namespace scratchbird::listener {

// Local native transport adapter. No OS handle is published in pool snapshots,
// UUIDs, wire records or persistence. This proves a sender lifetime, not package
// provenance or database authority. The Linux implementation uses SCM_PIDFD.
class ControlPeerIdentity {
 public:
  ControlPeerIdentity() = default;
  ~ControlPeerIdentity();
  ControlPeerIdentity(const ControlPeerIdentity&) = delete;
  ControlPeerIdentity& operator=(const ControlPeerIdentity&) = delete;

  bool Prepare(std::intptr_t socket);
  bool ExpectSender(std::int64_t process_pid);
  bool Alive() const;
  bool ReadFrame(std::intptr_t socket, ListenerControlDecodeResult* decoded,
                 std::uint32_t timeout_ms);

 private:
  bool ReadExact(std::intptr_t socket, std::span<std::uint8_t> bytes,
                 std::chrono::steady_clock::time_point deadline);
  int process_handle_ = -1;
  std::int64_t expected_pid_ = 0;
  std::uint32_t expected_uid_ = 0;
  std::uint32_t expected_gid_ = 0;
  std::uint64_t socket_cookie_ = 0;
  bool prepared_ = false;
  bool refused_ = false;
};

}  // namespace scratchbird::listener
