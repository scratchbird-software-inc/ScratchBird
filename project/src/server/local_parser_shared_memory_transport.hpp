// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "shared_memory_ipc_ring.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace scratchbird::server {

struct LocalParserSharedMemoryRoute {
  std::uint64_t request_id{0};
  std::array<std::uint8_t, 16> server_session_uuid{};
  std::array<std::uint8_t, 16> server_transaction_uuid{};
  std::uint64_t server_auth_epoch{0};
};

class LocalParserSharedMemoryTransport {
 public:
  static scratchbird::ipc::SharedMemoryRingResult Create(
      const scratchbird::ipc::SharedMemoryRingOptions& request_options,
      const scratchbird::ipc::SharedMemoryRingOptions& result_options,
      LocalParserSharedMemoryTransport* transport);

  scratchbird::ipc::SharedMemoryRingResult SubmitRequest(
      const LocalParserSharedMemoryRoute& route,
      const std::vector<std::uint8_t>& parser_packet_bytes);
  scratchbird::ipc::SharedMemoryRingResult ServerReceiveRequest(
      scratchbird::ipc::SharedMemoryRingFrame* frame);
  scratchbird::ipc::SharedMemoryRingResult PublishResult(
      const LocalParserSharedMemoryRoute& route,
      const std::vector<std::uint8_t>& direct_binary_result_frame);
  scratchbird::ipc::SharedMemoryRingResult ParserReceiveResult(
      scratchbird::ipc::SharedMemoryRingFrame* frame);
  scratchbird::ipc::SharedMemoryRingResult ResolvePayload(
      const scratchbird::ipc::SharedMemoryPayloadHandle& handle,
      std::vector<std::uint8_t>* payload) const;
  scratchbird::ipc::SharedMemoryRingResult ReleasePayload(
      const scratchbird::ipc::SharedMemoryPayloadHandle& handle);

 private:
  scratchbird::ipc::SharedMemoryIpcRing request_ring_;
  scratchbird::ipc::SharedMemoryIpcRing result_ring_;
};

scratchbird::ipc::SharedMemoryRingResult EnqueueLocalParserSblrRequest(
    scratchbird::ipc::SharedMemoryIpcRing* ring,
    const LocalParserSharedMemoryRoute& route,
    const std::vector<std::uint8_t>& binary_sblr);

scratchbird::ipc::SharedMemoryRingResult EnqueueLocalParserBinaryResult(
    scratchbird::ipc::SharedMemoryIpcRing* ring,
    const LocalParserSharedMemoryRoute& route,
    const std::vector<std::uint8_t>& direct_binary_result_frame);

}  // namespace scratchbird::server
