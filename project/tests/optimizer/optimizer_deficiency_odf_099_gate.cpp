// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "direct_binary_result_frame.hpp"
#include "local_parser_shared_memory_transport.hpp"
#include "parser_server_ipc.hpp"
#include "shared_memory_ipc_ring.hpp"
#include "vectorized_result_batch.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace ipc = scratchbird::ipc;
namespace parser_ipc = scratchbird::parser::sbsql;
namespace server = scratchbird::server;
namespace wire = scratchbird::wire;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::array<std::uint8_t, 16> Uuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::uint8_t>(seed + i);
  }
  return uuid;
}

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return {text.begin(), text.end()};
}

std::vector<std::uint8_t> RepeatedBytes(std::size_t count,
                                        std::uint8_t seed = 0x41) {
  std::vector<std::uint8_t> data(count);
  for (std::size_t i = 0; i < count; ++i) {
    data[i] = static_cast<std::uint8_t>(seed + (i % 23u));
  }
  return data;
}

void RequireDiag(const ipc::SharedMemoryRingResult& result,
                 std::string_view code,
                 std::string_view message) {
  Require(!result.ok, "ODF-099 failing ring operation unexpectedly succeeded");
  Require(result.diagnostic.code == code, "ODF-099 diagnostic code mismatch");
  Require(result.diagnostic.message.find(message) != std::string::npos,
          "ODF-099 diagnostic message mismatch");
}

void WriteU32(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*bytes)[offset++] = static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

void WriteU16(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              std::uint16_t value) {
  (*bytes)[offset++] = static_cast<std::uint8_t>(value & 0xffu);
  (*bytes)[offset++] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

std::vector<std::uint8_t> ParserPacket(std::uint64_t request_id,
                                       std::vector<std::uint8_t> payload) {
  parser_ipc::ParserServerPacket packet;
  packet.opcode = parser_ipc::ParserServerOpcode::kSblrSubmitRequest;
  packet.request_id = request_id;
  packet.payload = std::move(payload);
  return parser_ipc::EncodePacket(packet);
}

std::vector<std::uint8_t> DirectResultFrameBytes() {
  exec::VectorizedResultBatchBuilder builder(3);
  builder.AddColumn(exec::MakeFixedWidthResultBatchColumn(
      "id", 3, 4, RepeatedBytes(12, 1),
      exec::MakeResultBatchValidityBitmap(3)));
  auto finalized = builder.Finalize();
  Require(finalized.ok(), "ODF-099 result batch fixture did not finalize");
  auto frame = wire::BuildDirectBinaryResultFrame(finalized.batch);
  Require(frame.ok(), "ODF-099 direct binary result frame fixture failed");
  return frame.frame.bytes;
}

ipc::SharedMemoryIpcRing MakeRing(std::uint64_t slots = 3,
                                  std::uint64_t payload_capacity = 4096,
                                  std::uint64_t inline_limit = 32) {
  ipc::SharedMemoryRingOptions options;
  options.slot_count = slots;
  options.payload_capacity = payload_capacity;
  options.max_inline_payload_bytes = inline_limit;
  options.max_payload_bytes = payload_capacity;
  ipc::SharedMemoryIpcRing ring;
  auto created = ipc::SharedMemoryIpcRing::Create(options, &ring);
  Require(created.ok, "ODF-099 ring creation failed");
  return ring;
}

server::LocalParserSharedMemoryRoute Route(std::uint64_t request_id) {
  server::LocalParserSharedMemoryRoute route;
  route.request_id = request_id;
  route.server_session_uuid = Uuid(9);
  route.server_transaction_uuid = Uuid(99);
  route.server_auth_epoch = 17;
  return route;
}

void HeaderAndFrameEnvelopeDiagnostics() {
  auto ring = MakeRing();
  RequireDiag(ring.ValidateHeader(0, ipc::kSharedMemoryIpcRingMajor,
                                  ipc::kSharedMemoryIpcRingMinor),
              "IPC.RING.BAD_MAGIC", "magic");
  RequireDiag(ring.ValidateHeader(ipc::kSharedMemoryIpcRingMagic,
                                  ipc::kSharedMemoryIpcRingMajor + 1,
                                  ipc::kSharedMemoryIpcRingMinor),
              "IPC.RING.VERSION_MISMATCH", "version");

  RequireDiag(ipc::ValidateSharedMemoryRingParserPacketEnvelope(Bytes("tiny")),
              "IPC.RING.MALFORMED_FRAME", "truncated");
  auto packet = ParserPacket(7, Bytes("binary-sblr"));
  packet[0] = 0;
  RequireDiag(ipc::ValidateSharedMemoryRingParserPacketEnvelope(packet),
              "IPC.RING.BAD_MAGIC", "magic");
  packet = ParserPacket(7, Bytes("binary-sblr"));
  packet.pop_back();
  RequireDiag(ipc::ValidateSharedMemoryRingParserPacketEnvelope(packet),
              "IPC.RING.MALFORMED_FRAME", "malformed or truncated");
  packet = ParserPacket(7, Bytes("binary-sblr"));
  WriteU32(&packet, 8, parser_ipc::kParserServerIpcProtocolMaxSupported + 1);
  RequireDiag(ipc::ValidateSharedMemoryRingParserPacketEnvelope(packet),
              "IPC.RING.VERSION_MISMATCH", "protocol version");
  packet = ParserPacket(7, Bytes("binary-sblr"));
  WriteU16(&packet, 4,
           static_cast<std::uint16_t>(
               parser_ipc::ParserServerOpcode::kParserHello));
  RequireDiag(ipc::ValidateSharedMemoryRingParserPacketEnvelope(packet),
              "IPC.RING.MALFORMED_FRAME", "SBLR submit request");
}

void RequestResultOrderingAndBackpressure() {
  auto ring = MakeRing(2, 4096, 1024);
  const auto session = Uuid(1);
  const auto txn = Uuid(33);
  const auto req1 = ParserPacket(10, Bytes("sblr-request-one"));
  const auto req2 = ParserPacket(11, Bytes("sblr-request-two"));
  Require(ipc::ValidateSharedMemoryRingParserPacketEnvelope(req1).ok,
          "ODF-099 parser packet envelope rejected valid request");

  auto first = ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                   10, session, txn, 99, req1);
  auto second = ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                    11, session, txn, 99, req2);
  Require(first.ok && second.ok, "ODF-099 request enqueue failed");
  Require(first.frame.sequence == 1 && second.frame.sequence == 2,
          "ODF-099 request sequences are not monotonic");
  Require(ring.available_slots() == 0,
          "ODF-099 bounded ring did not account for full slots");
  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  12, session, txn, 99, req1),
              "IPC.RING.BACKPRESSURE", "no free descriptor slots");

  ipc::SharedMemoryRingFrame out;
  Require(ring.Dequeue(&out).ok && out.request_id == 10,
          "ODF-099 first request did not dequeue in order");
  Require(ring.Dequeue(&out).ok && out.request_id == 11,
          "ODF-099 second request did not dequeue in order");
  RequireDiag(ring.Dequeue(&out), "IPC.RING.EMPTY", "no readable frames");
}

void ZeroCopyHandleLifecycle() {
  auto ring = MakeRing(3, 8192, 16);
  const auto session = Uuid(2);
  const auto txn = Uuid(44);
  const auto payload = DirectResultFrameBytes();
  Require(payload.size() > 16, "ODF-099 direct result frame fixture too small");

  auto enqueued = ring.EnqueueResult(22, session, txn, 101, payload);
  Require(enqueued.ok, "ODF-099 zero-copy result enqueue failed");
  Require(enqueued.frame.has_handle,
          "ODF-099 large result payload was not represented by a handle");
  Require(enqueued.frame.handle.kind ==
              ipc::SharedMemoryPayloadKind::kBinaryResultFrame,
          "ODF-099 result handle kind changed");

  auto invalid = enqueued.frame.handle;
  ++invalid.ring_generation;
  std::vector<std::uint8_t> resolved;
  RequireDiag(ring.ResolveHandle(invalid, &resolved),
              "IPC.RING.INVALID_HANDLE", "invalid");

  ipc::SharedMemoryRingFrame out;
  Require(ring.Dequeue(&out).ok, "ODF-099 result dequeue failed");
  Require(out.has_handle, "ODF-099 dequeued descriptor lost payload handle");
  auto tampered = out.handle;
  ++tampered.payload_offset;
  RequireDiag(ring.ResolveHandle(tampered, &resolved),
              "IPC.RING.INVALID_HANDLE", "invalid");
  tampered = out.handle;
  tampered.kind = ipc::SharedMemoryPayloadKind::kBinarySblr;
  RequireDiag(ring.ReleasePayloadHandle(tampered),
              "IPC.RING.INVALID_HANDLE", "invalid");
  Require(ring.ResolveHandle(out.handle, &resolved).ok,
          "ODF-099 zero-copy handle did not resolve after dequeue");
  Require(resolved == payload, "ODF-099 zero-copy payload bytes changed");
  Require(ring.ReleasePayloadHandle(out.handle).ok,
          "ODF-099 zero-copy handle release failed");
  RequireDiag(ring.ResolveHandle(enqueued.frame.handle, &resolved),
              "IPC.RING.STALE_HANDLE", "stale");
  RequireDiag(ring.ReleasePayloadHandle(out.handle),
              "IPC.RING.STALE_HANDLE", "stale");
}

void CapacityAuthorityAndFallbackRefusals() {
  auto ring = MakeRing(3, 128, 8);
  const auto session = Uuid(3);
  const auto txn = Uuid(55);
  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  1, {}, txn, 1, Bytes("abc")),
              "IPC.RING.MISSING_AUTHORITY", "session/auth evidence");
  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  1, session, {}, 1, Bytes("abc")),
              "IPC.RING.MISSING_AUTHORITY", "transaction evidence");
  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  1, session, txn, 0, Bytes("abc")),
              "IPC.RING.MISSING_AUTHORITY", "session/auth evidence");

  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  2, session, txn, 1,
                                  RepeatedBytes(256)),
              "IPC.RING.OVERSIZED_PAYLOAD", "exceeds");
  RequireDiag(ring.EnqueueRequest(static_cast<ipc::SharedMemoryPayloadKind>(99),
                                  2, session, txn, 1,
                                  RepeatedBytes(16)),
              "IPC.RING.UNKNOWN_PAYLOAD_KIND", "payload kind");

  Require(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                              3, session, txn, 1, RepeatedBytes(80)).ok,
          "ODF-099 arena fixture enqueue failed");
  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  4, session, txn, 1, RepeatedBytes(80)),
              "IPC.RING.CAPACITY_EXHAUSTED", "arena is exhausted");

  RequireDiag(ring.ValidateEvidenceClaims({"row_object_fallback=true"}),
              "IPC.RING.UNSUPPORTED_FALLBACK", "row_object_fallback");
  RequireDiag(ring.ValidateEvidenceClaims({"text_fallback=true"}),
              "IPC.RING.UNSUPPORTED_FALLBACK", "text_fallback");
  RequireDiag(ring.ValidateEvidenceClaims({"parser_authority=true"}),
              "IPC.RING.UNSUPPORTED_FALLBACK", "parser_authority");
  RequireDiag(ring.ValidateEvidenceClaims({"finality_authority=true"}),
              "IPC.RING.UNSUPPORTED_FALLBACK", "finality_authority");
}

void ArenaNonOverlapRequiresRelease() {
  auto ring = MakeRing(3, 100, 8);
  const auto session = Uuid(4);
  const auto txn = Uuid(66);
  const auto first_payload = RepeatedBytes(60, 10);
  auto first = ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                   30, session, txn, 1, first_payload);
  Require(first.ok && first.frame.has_handle,
          "ODF-099 first arena allocation failed");
  ipc::SharedMemoryRingFrame out;
  Require(ring.Dequeue(&out).ok, "ODF-099 first arena dequeue failed");
  RequireDiag(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                                  31, session, txn, 1,
                                  RepeatedBytes(50, 20)),
              "IPC.RING.CAPACITY_EXHAUSTED", "arena is exhausted");
  std::vector<std::uint8_t> resolved;
  Require(ring.ResolveHandle(out.handle, &resolved).ok,
          "ODF-099 active dequeued handle did not survive non-overlap refusal");
  Require(resolved == first_payload,
          "ODF-099 active payload was overwritten before release");
  Require(ring.ReleasePayloadHandle(out.handle).ok,
          "ODF-099 arena release failed");
  Require(ring.EnqueueRequest(ipc::SharedMemoryPayloadKind::kBinarySblr,
                              32, session, txn, 1, RepeatedBytes(50, 30)).ok,
          "ODF-099 arena allocation did not reuse released capacity");
}

void RouteValidationRefusals() {
  auto ring = MakeRing(3, 4096, 16);
  auto route = Route(40);
  auto packet = ParserPacket(40, Bytes("binary-sblr"));
  WriteU32(&packet, 8, parser_ipc::kParserServerIpcProtocolMaxSupported + 1);
  RequireDiag(server::EnqueueLocalParserSblrRequest(&ring, route, packet),
              "IPC.RING.VERSION_MISMATCH", "protocol version");

  route.server_transaction_uuid = {};
  RequireDiag(server::EnqueueLocalParserSblrRequest(
                  &ring, route, ParserPacket(40, Bytes("binary-sblr"))),
              "IPC.RING.MISSING_AUTHORITY", "transaction evidence");

  route = Route(41);
  auto malformed_result = DirectResultFrameBytes();
  malformed_result.resize(10);
  RequireDiag(server::EnqueueLocalParserBinaryResult(&ring, route,
                                                     malformed_result),
              "IPC.RING.MALFORMED_RESULT_FRAME", "malformed");
}

void TransportWrapperRequestResultOrdering() {
  ipc::SharedMemoryRingOptions request_options;
  request_options.slot_count = 3;
  request_options.payload_capacity = 4096;
  request_options.max_inline_payload_bytes = 16;
  request_options.max_payload_bytes = 4096;
  ipc::SharedMemoryRingOptions result_options = request_options;

  server::LocalParserSharedMemoryTransport transport;
  Require(server::LocalParserSharedMemoryTransport::Create(
              request_options, result_options, &transport)
              .ok,
          "ODF-099 transport wrapper creation failed");

  const auto route1 = Route(50);
  const auto route2 = Route(51);
  const auto request1 = ParserPacket(50, RepeatedBytes(64, 1));
  const auto request2 = ParserPacket(51, RepeatedBytes(64, 2));
  Require(transport.SubmitRequest(route1, request1).ok,
          "ODF-099 transport submit request1 failed");
  Require(transport.SubmitRequest(route2, request2).ok,
          "ODF-099 transport submit request2 failed");

  ipc::SharedMemoryRingFrame received;
  std::vector<std::uint8_t> payload;
  Require(transport.ServerReceiveRequest(&received).ok &&
              received.request_id == 50,
          "ODF-099 transport request1 order changed");
  Require(transport.ResolvePayload(received.handle, &payload).ok &&
              payload == request1,
          "ODF-099 transport request1 payload did not resolve");
  Require(transport.ReleasePayload(received.handle).ok,
          "ODF-099 transport request1 release failed");
  Require(transport.ServerReceiveRequest(&received).ok &&
              received.request_id == 51,
          "ODF-099 transport request2 order changed");
  Require(transport.ResolvePayload(received.handle, &payload).ok &&
              payload == request2,
          "ODF-099 transport request2 payload did not resolve");
  Require(transport.ReleasePayload(received.handle).ok,
          "ODF-099 transport request2 release failed");

  const auto result1 = DirectResultFrameBytes();
  const auto result2 = DirectResultFrameBytes();
  Require(transport.PublishResult(route1, result1).ok,
          "ODF-099 transport publish result1 failed");
  Require(transport.PublishResult(route2, result2).ok,
          "ODF-099 transport publish result2 failed");
  Require(transport.ParserReceiveResult(&received).ok &&
              received.request_id == 50,
          "ODF-099 transport result1 order changed");
  Require(transport.ResolvePayload(received.handle, &payload).ok &&
              payload == result1,
          "ODF-099 transport result1 payload did not resolve");
  Require(transport.ReleasePayload(received.handle).ok,
          "ODF-099 transport result1 release failed");
  Require(transport.ParserReceiveResult(&received).ok &&
              received.request_id == 51,
          "ODF-099 transport result2 order changed");
  Require(transport.ResolvePayload(received.handle, &payload).ok &&
              payload == result2,
          "ODF-099 transport result2 payload did not resolve");
  Require(transport.ReleasePayload(received.handle).ok,
          "ODF-099 transport result2 release failed");
}

}  // namespace

int main() {
  HeaderAndFrameEnvelopeDiagnostics();
  RequestResultOrderingAndBackpressure();
  ZeroCopyHandleLifecycle();
  CapacityAuthorityAndFallbackRefusals();
  ArenaNonOverlapRequiresRelease();
  RouteValidationRefusals();
  TransportWrapperRequestResultOrdering();
  return EXIT_SUCCESS;
}
