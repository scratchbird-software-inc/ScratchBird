// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "local_parser_shared_memory_transport.hpp"

#include "direct_binary_result_frame.hpp"

#include <utility>

namespace scratchbird::server {
namespace {

scratchbird::ipc::SharedMemoryRingResult MissingRing() {
  scratchbird::ipc::SharedMemoryRingResult result;
  result.diagnostic = {
      "IPC.RING.MALFORMED",
      "local parser/server shared-memory IPC ring is not available"};
  return result;
}

scratchbird::ipc::SharedMemoryRingResult InvalidResultFrame(
    const scratchbird::wire::DirectBinaryResultFrameResult& parsed) {
  scratchbird::ipc::SharedMemoryRingResult result;
  result.diagnostic = {
      "IPC.RING.MALFORMED_RESULT_FRAME",
      parsed.diagnostic.remediation_hint.empty()
          ? "local parser/server shared-memory result frame is malformed"
          : parsed.diagnostic.remediation_hint};
  return result;
}

}  // namespace

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::Create(
    const scratchbird::ipc::SharedMemoryRingOptions& request_options,
    const scratchbird::ipc::SharedMemoryRingOptions& result_options,
    LocalParserSharedMemoryTransport* transport) {
  if (transport == nullptr) {
    return MissingRing();
  }
  LocalParserSharedMemoryTransport created;
  auto request_created = scratchbird::ipc::SharedMemoryIpcRing::Create(
      request_options, &created.request_ring_);
  if (!request_created.ok) {
    return request_created;
  }
  auto result_created = scratchbird::ipc::SharedMemoryIpcRing::Create(
      result_options, &created.result_ring_);
  if (!result_created.ok) {
    return result_created;
  }
  *transport = std::move(created);
  scratchbird::ipc::SharedMemoryRingResult result;
  result.ok = true;
  result.evidence.insert(result.evidence.end(), request_created.evidence.begin(),
                         request_created.evidence.end());
  result.evidence.insert(result.evidence.end(), result_created.evidence.begin(),
                         result_created.evidence.end());
  return result;
}

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::SubmitRequest(
    const LocalParserSharedMemoryRoute& route,
    const std::vector<std::uint8_t>& parser_packet_bytes) {
  return EnqueueLocalParserSblrRequest(&request_ring_, route,
                                       parser_packet_bytes);
}

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::ServerReceiveRequest(
    scratchbird::ipc::SharedMemoryRingFrame* frame) {
  return request_ring_.Dequeue(frame);
}

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::PublishResult(
    const LocalParserSharedMemoryRoute& route,
    const std::vector<std::uint8_t>& direct_binary_result_frame) {
  return EnqueueLocalParserBinaryResult(&result_ring_, route,
                                        direct_binary_result_frame);
}

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::ParserReceiveResult(
    scratchbird::ipc::SharedMemoryRingFrame* frame) {
  return result_ring_.Dequeue(frame);
}

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::ResolvePayload(
    const scratchbird::ipc::SharedMemoryPayloadHandle& handle,
    std::vector<std::uint8_t>* payload) const {
  if (handle.kind == scratchbird::ipc::SharedMemoryPayloadKind::kBinarySblr) {
    return request_ring_.ResolveHandle(handle, payload);
  }
  if (handle.kind ==
      scratchbird::ipc::SharedMemoryPayloadKind::kBinaryResultFrame) {
    return result_ring_.ResolveHandle(handle, payload);
  }
  scratchbird::ipc::SharedMemoryRingResult result;
  result.diagnostic = {"IPC.RING.UNKNOWN_PAYLOAD_KIND",
                       "shared-memory IPC ring payload kind is unknown"};
  return result;
}

scratchbird::ipc::SharedMemoryRingResult
LocalParserSharedMemoryTransport::ReleasePayload(
    const scratchbird::ipc::SharedMemoryPayloadHandle& handle) {
  if (handle.kind == scratchbird::ipc::SharedMemoryPayloadKind::kBinarySblr) {
    return request_ring_.ReleasePayloadHandle(handle);
  }
  if (handle.kind ==
      scratchbird::ipc::SharedMemoryPayloadKind::kBinaryResultFrame) {
    return result_ring_.ReleasePayloadHandle(handle);
  }
  scratchbird::ipc::SharedMemoryRingResult result;
  result.diagnostic = {"IPC.RING.UNKNOWN_PAYLOAD_KIND",
                       "shared-memory IPC ring payload kind is unknown"};
  return result;
}

scratchbird::ipc::SharedMemoryRingResult EnqueueLocalParserSblrRequest(
    scratchbird::ipc::SharedMemoryIpcRing* ring,
    const LocalParserSharedMemoryRoute& route,
    const std::vector<std::uint8_t>& binary_sblr) {
  if (ring == nullptr) {
    return MissingRing();
  }
  const auto authority = scratchbird::ipc::ValidateSharedMemoryRingRouteAuthority(
      route.server_session_uuid, route.server_transaction_uuid,
      route.server_auth_epoch, true);
  if (!authority.ok) {
    return authority;
  }
  const auto envelope =
      scratchbird::ipc::ValidateSharedMemoryRingParserPacketEnvelope(
          binary_sblr);
  if (!envelope.ok) {
    return envelope;
  }
  return ring->EnqueueRequest(scratchbird::ipc::SharedMemoryPayloadKind::kBinarySblr,
                              route.request_id, route.server_session_uuid,
                              route.server_transaction_uuid,
                              route.server_auth_epoch, binary_sblr);
}

scratchbird::ipc::SharedMemoryRingResult EnqueueLocalParserBinaryResult(
    scratchbird::ipc::SharedMemoryIpcRing* ring,
    const LocalParserSharedMemoryRoute& route,
    const std::vector<std::uint8_t>& direct_binary_result_frame) {
  if (ring == nullptr) {
    return MissingRing();
  }
  const auto authority = scratchbird::ipc::ValidateSharedMemoryRingRouteAuthority(
      route.server_session_uuid, route.server_transaction_uuid,
      route.server_auth_epoch, true);
  if (!authority.ok) {
    return authority;
  }
  const auto parsed =
      scratchbird::wire::ParseDirectBinaryResultFrame(direct_binary_result_frame);
  if (!parsed.ok()) {
    return InvalidResultFrame(parsed);
  }
  return ring->EnqueueResult(route.request_id, route.server_session_uuid,
                             route.server_transaction_uuid,
                             route.server_auth_epoch,
                             direct_binary_result_frame);
}

}  // namespace scratchbird::server
