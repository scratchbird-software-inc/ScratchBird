// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "shared_memory_ipc_ring.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace scratchbird::ipc {
namespace {
namespace platform = scratchbird::core::platform;

bool IsZeroUuid(const std::array<std::uint8_t, 16>& uuid) {
  return std::all_of(uuid.begin(), uuid.end(),
                     [](std::uint8_t byte) { return byte == 0; });
}

SharedMemoryRingResult Fail(std::string code, std::string message) {
  SharedMemoryRingResult result;
  result.diagnostic = {std::move(code), std::move(message)};
  return result;
}

SharedMemoryRingResult Ok() {
  SharedMemoryRingResult result;
  result.ok = true;
  return result;
}

platform::RuntimeCompatibilityDescriptor RingCompatibility(
    const SharedMemoryRingOptions& options) {
  auto descriptor = options.runtime_compatibility;
  if (descriptor.route_id.empty()) {
    descriptor =
        platform::CurrentRuntimeCompatibilityDescriptor("ipc.shared_memory_ring");
  }
  descriptor.route_id =
      descriptor.route_id.empty() ? "ipc.shared_memory_ring" : descriptor.route_id;
  descriptor.source_component = "ipc.shared_memory_ring";
  if (descriptor.required_endian == platform::RuntimeEndian::unknown) {
    descriptor.required_endian = platform::CurrentRuntimeEndian();
  }
  if (descriptor.provider_endian == platform::RuntimeEndian::unknown) {
    descriptor.provider_endian = platform::CurrentRuntimeEndian();
  }
  if (descriptor.required_alignment == 0) {
    descriptor.required_alignment = 8;
  }
  if (descriptor.provider_alignment == 0) {
    descriptor.provider_alignment = alignof(std::max_align_t);
  }
  descriptor.deterministic_scalar_fallback_required = false;
  descriptor.deterministic_scalar_fallback_available = false;
  descriptor.fail_closed_on_mismatch = true;
  return descriptor;
}

std::uint16_t RuntimeEndianCode(platform::RuntimeEndian endian) {
  switch (endian) {
    case platform::RuntimeEndian::little:
      return 1;
    case platform::RuntimeEndian::big:
      return 2;
    case platform::RuntimeEndian::unknown:
      return 0;
  }
  return 0;
}

bool IsForbiddenEvidence(std::string_view item, std::string* token) {
  constexpr std::string_view kForbidden[] = {
      "row_object_fallback=true",
      "row_object_formatting=true",
      "text_fallback=true",
      "parser_authority=true",
      "client_authority=true",
      "provider_authority=true",
      "security_authority=true",
      "mga_authority=true",
      "visibility_authority=true",
      "finality_authority=true",
      "wal_authority=true",
  };
  for (const auto forbidden : kForbidden) {
    if (item.find(forbidden) != std::string_view::npos) {
      *token = std::string(forbidden);
      return true;
    }
  }
  return false;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    out |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
  }
  return out;
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1]) << 8u;
}

bool KnownPayloadKind(SharedMemoryPayloadKind kind) {
  return kind == SharedMemoryPayloadKind::kBinarySblr ||
         kind == SharedMemoryPayloadKind::kBinaryResultFrame;
}

bool RangesOverlap(std::uint64_t left_offset,
                   std::uint64_t left_length,
                   std::uint64_t right_offset,
                   std::uint64_t right_length) {
  return left_offset < right_offset + right_length &&
         right_offset < left_offset + left_length;
}

bool CandidateOverlapsActive(
    const std::vector<SharedMemoryIpcRing::ActivePayloadRecord>& records,
    std::uint64_t offset,
    std::uint64_t length) {
  for (const auto& record : records) {
    if (!record.active) {
      continue;
    }
    if (RangesOverlap(offset, length, record.handle.payload_offset,
                      record.handle.payload_length)) {
      return true;
    }
  }
  return false;
}

std::optional<std::uint64_t> FindPayloadOffset(
    const std::vector<SharedMemoryIpcRing::ActivePayloadRecord>& records,
    std::uint64_t capacity,
    std::uint64_t length) {
  if (length > capacity) {
    return std::nullopt;
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> active;
  for (const auto& record : records) {
    if (!record.active) {
      continue;
    }
    active.push_back(
        {record.handle.payload_offset, record.handle.payload_length});
  }
  std::sort(active.begin(), active.end());

  std::uint64_t cursor = 0;
  for (const auto& [offset, active_length] : active) {
    if (cursor <= offset && length <= offset - cursor) {
      return cursor;
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - active_length) {
      return std::nullopt;
    }
    cursor = std::max(cursor, offset + active_length);
  }
  if (cursor <= capacity && length <= capacity - cursor) {
    return cursor;
  }
  return std::nullopt;
}

bool SamePayloadIdentity(const SharedMemoryPayloadHandle& left,
                         const SharedMemoryPayloadHandle& right) {
  return left.ring_generation == right.ring_generation &&
         left.slot_index == right.slot_index &&
         left.slot_generation == right.slot_generation &&
         left.payload_generation == right.payload_generation;
}

bool SamePayloadHandle(const SharedMemoryPayloadHandle& left,
                       const SharedMemoryPayloadHandle& right) {
  return SamePayloadIdentity(left, right) &&
         left.kind == right.kind &&
         left.payload_offset == right.payload_offset &&
         left.payload_length == right.payload_length &&
         left.payload_crc32c == right.payload_crc32c;
}

}  // namespace

SharedMemoryRingResult SharedMemoryIpcRing::Create(
    const SharedMemoryRingOptions& options,
    SharedMemoryIpcRing* ring) {
  if (ring == nullptr) {
    return Fail("IPC.RING.MALFORMED", "shared-memory IPC ring output is null");
  }
  if (options.slot_count == 0 ||
      options.slot_count > (std::numeric_limits<std::uint64_t>::max() / 2u)) {
    return Fail("IPC.RING.MALFORMED", "shared-memory IPC ring slot count is invalid");
  }
  if (options.payload_capacity == 0 ||
      options.payload_capacity >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return Fail("IPC.RING.MALFORMED",
                "shared-memory IPC ring payload capacity is invalid");
  }
  if (options.max_inline_payload_bytes > options.max_payload_bytes ||
      options.max_payload_bytes > kSharedMemoryIpcMaxPayloadBytes ||
      options.max_payload_bytes > options.payload_capacity) {
    return Fail("IPC.RING.OVERSIZED_PAYLOAD",
                "shared-memory IPC ring payload limits are invalid");
  }
  const auto compatibility_descriptor = RingCompatibility(options);
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      compatibility_descriptor);
  if (!compatibility.ok) {
    auto result = Fail("IPC.RING.RUNTIME_COMPATIBILITY_MISMATCH",
                       compatibility.diagnostic_code);
    result.evidence = compatibility.evidence;
    return result;
  }

  SharedMemoryIpcRing created;
  created.options_ = options;
  created.header_.slot_count = options.slot_count;
  created.header_.payload_capacity = options.payload_capacity;
  created.header_.runtime_generation =
      compatibility_descriptor.provider_runtime_generation == 0
          ? platform::CurrentRuntimeCompatibilityGeneration()
          : compatibility_descriptor.provider_runtime_generation;
  created.header_.compatibility_epoch =
      compatibility_descriptor.provider_compatibility_epoch == 0
          ? platform::CurrentRuntimeCompatibilityEpoch()
          : compatibility_descriptor.provider_compatibility_epoch;
  created.header_.runtime_endian =
      RuntimeEndianCode(compatibility_descriptor.provider_endian);
  created.header_.required_alignment =
      static_cast<std::uint16_t>(std::min<std::uint64_t>(
          65535, compatibility_descriptor.required_alignment));
  created.slots_.resize(static_cast<std::size_t>(options.slot_count));
  created.payload_arena_.resize(static_cast<std::size_t>(options.payload_capacity));
  *ring = std::move(created);
  auto result = Ok();
  result.evidence = compatibility.evidence;
  return result;
}

SharedMemoryRingHeaderView SharedMemoryIpcRing::header() const {
  return header_;
}

std::uint64_t SharedMemoryIpcRing::available_slots() const {
  return header_.slot_count - readable_slots();
}

std::uint64_t SharedMemoryIpcRing::readable_slots() const {
  return header_.producer_cursor - header_.consumer_cursor;
}

SharedMemoryRingResult SharedMemoryIpcRing::ValidateHeader(
    std::uint32_t magic,
    std::uint16_t major,
    std::uint16_t minor) const {
  if (magic != kSharedMemoryIpcRingMagic) {
    return Fail("IPC.RING.BAD_MAGIC", "shared-memory IPC ring magic is not SBIR");
  }
  if (major != kSharedMemoryIpcRingMajor || minor > kSharedMemoryIpcRingMinor) {
    return Fail("IPC.RING.VERSION_MISMATCH",
                "shared-memory IPC ring version is not compatible");
  }
  return Ok();
}

SharedMemoryRingResult ValidateSharedMemoryRingRouteAuthority(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& transaction_uuid,
    std::uint64_t auth_epoch,
    bool require_transaction_authority) {
  if (IsZeroUuid(session_uuid) || auth_epoch == 0) {
    return Fail("IPC.RING.MISSING_AUTHORITY",
                "shared-memory IPC routed work is missing server-owned session/auth evidence");
  }
  if (require_transaction_authority && IsZeroUuid(transaction_uuid)) {
    return Fail("IPC.RING.MISSING_AUTHORITY",
                "shared-memory IPC routed work is missing server-owned transaction evidence");
  }
  return Ok();
}

SharedMemoryRingResult SharedMemoryIpcRing::EnqueueRequest(
    SharedMemoryPayloadKind payload_kind,
    std::uint64_t request_id,
    std::array<std::uint8_t, 16> session_uuid,
    std::array<std::uint8_t, 16> transaction_uuid,
    std::uint64_t auth_epoch,
    const std::vector<std::uint8_t>& payload) {
  return Enqueue(SharedMemoryRingFrameKind::kRequest, payload_kind, request_id,
                 session_uuid, transaction_uuid, auth_epoch, payload);
}

SharedMemoryRingResult SharedMemoryIpcRing::EnqueueResult(
    std::uint64_t request_id,
    std::array<std::uint8_t, 16> session_uuid,
    std::array<std::uint8_t, 16> transaction_uuid,
    std::uint64_t auth_epoch,
    const std::vector<std::uint8_t>& payload) {
  return Enqueue(SharedMemoryRingFrameKind::kResult,
                 SharedMemoryPayloadKind::kBinaryResultFrame, request_id,
                 session_uuid, transaction_uuid, auth_epoch, payload);
}

SharedMemoryRingResult SharedMemoryIpcRing::Enqueue(
    SharedMemoryRingFrameKind frame_kind,
    SharedMemoryPayloadKind payload_kind,
    std::uint64_t request_id,
    std::array<std::uint8_t, 16> session_uuid,
    std::array<std::uint8_t, 16> transaction_uuid,
    std::uint64_t auth_epoch,
    const std::vector<std::uint8_t>& payload) {
  if (header_.magic != kSharedMemoryIpcRingMagic) {
    return Fail("IPC.RING.BAD_MAGIC", "shared-memory IPC ring header is corrupt");
  }
  if (header_.producer_cursor < header_.consumer_cursor ||
      header_.producer_cursor - header_.consumer_cursor > header_.slot_count) {
    return Fail("IPC.RING.CURSOR_OVERRUN",
                "shared-memory IPC ring producer/consumer cursors are inconsistent");
  }
  const auto authority = ValidateSharedMemoryRingRouteAuthority(
      session_uuid, transaction_uuid, auth_epoch,
      options_.require_transaction_authority);
  if (!authority.ok) {
    return authority;
  }
  if (!KnownPayloadKind(payload_kind)) {
    return Fail("IPC.RING.UNKNOWN_PAYLOAD_KIND",
                "shared-memory IPC ring payload kind is unknown");
  }
  if (payload.empty() || payload.size() > options_.max_payload_bytes) {
    return Fail("IPC.RING.OVERSIZED_PAYLOAD",
                "shared-memory IPC ring payload is empty or exceeds the configured maximum");
  }
  if (available_slots() == 0) {
    return Fail("IPC.RING.BACKPRESSURE",
                "shared-memory IPC ring has no free descriptor slots");
  }
  const auto slot_index = header_.producer_cursor % header_.slot_count;
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (slot.occupied) {
    return Fail("IPC.RING.CURSOR_OVERRUN",
                "shared-memory IPC ring descriptor slot would be overwritten");
  }

  SharedMemoryRingFrame frame;
  frame.frame_kind = frame_kind;
  frame.payload_kind = payload_kind;
  frame.request_id = request_id;
  frame.sequence = frame_kind == SharedMemoryRingFrameKind::kRequest
                       ? header_.next_request_sequence++
                       : header_.next_result_sequence++;
  frame.session_uuid = session_uuid;
  frame.transaction_uuid = transaction_uuid;
  frame.auth_epoch = auth_epoch;

  if (payload.size() <= options_.max_inline_payload_bytes) {
    frame.inline_payload = payload;
  } else {
    auto allocated = AllocatePayloadRecord(payload_kind, slot_index,
                                           slot.slot_generation, payload,
                                           &frame.handle);
    if (!allocated.ok) {
      return allocated;
    }
    frame.has_handle = true;
  }

  slot.frame = std::move(frame);
  slot.occupied = true;
  ++header_.producer_cursor;

  SharedMemoryRingResult result = Ok();
  result.frame = slot.frame;
  result.handle = slot.frame.handle;
  return result;
}

SharedMemoryRingResult SharedMemoryIpcRing::Dequeue(SharedMemoryRingFrame* frame) {
  if (frame == nullptr) {
    return Fail("IPC.RING.MALFORMED", "shared-memory IPC dequeue output is null");
  }
  if (header_.producer_cursor < header_.consumer_cursor ||
      header_.producer_cursor - header_.consumer_cursor > header_.slot_count) {
    return Fail("IPC.RING.CURSOR_OVERRUN",
                "shared-memory IPC ring producer/consumer cursors are inconsistent");
  }
  if (readable_slots() == 0) {
    return Fail("IPC.RING.EMPTY", "shared-memory IPC ring has no readable frames");
  }
  const auto slot_index = header_.consumer_cursor % header_.slot_count;
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (!slot.occupied) {
    return Fail("IPC.RING.CURSOR_OVERRUN",
                "shared-memory IPC ring consumer cursor points at an empty descriptor");
  }

  *frame = slot.frame;
  slot.occupied = false;
  ++slot.slot_generation;
  ++header_.consumer_cursor;

  SharedMemoryRingResult result = Ok();
  result.frame = *frame;
  result.handle = frame->handle;
  return result;
}

SharedMemoryRingResult SharedMemoryIpcRing::ResolveHandle(
    const SharedMemoryPayloadHandle& handle,
    std::vector<std::uint8_t>* payload) const {
  if (payload == nullptr) {
    return Fail("IPC.RING.MALFORMED", "shared-memory IPC handle output is null");
  }
  if (handle.ring_generation != header_.ring_generation ||
      handle.slot_index >= header_.slot_count ||
      handle.payload_offset > payload_arena_.size() ||
      handle.payload_length > payload_arena_.size() - handle.payload_offset) {
    return Fail("IPC.RING.INVALID_HANDLE",
                "shared-memory IPC zero-copy payload handle is invalid");
  }
  const auto active = std::find_if(
      active_payloads_.begin(), active_payloads_.end(),
      [&handle](const ActivePayloadRecord& record) {
        return record.active && SamePayloadIdentity(record.handle, handle);
      });
  if (active == active_payloads_.end()) {
    return Fail("IPC.RING.STALE_HANDLE",
                "shared-memory IPC zero-copy payload handle generation is stale");
  }
  if (!SamePayloadHandle(active->handle, handle)) {
    return Fail("IPC.RING.INVALID_HANDLE",
                "shared-memory IPC zero-copy payload handle is invalid");
  }
  payload->assign(payload_arena_.begin() +
                      static_cast<std::ptrdiff_t>(handle.payload_offset),
                  payload_arena_.begin() +
                      static_cast<std::ptrdiff_t>(handle.payload_offset +
                                                  handle.payload_length));
  const auto crc = payload->empty() ? 0 : LocalIpcCrc32c(payload->data(), payload->size());
  if (crc != handle.payload_crc32c) {
    payload->clear();
    return Fail("IPC.RING.INVALID_HANDLE",
                "shared-memory IPC zero-copy payload handle CRC32C is invalid");
  }
  SharedMemoryRingResult result = Ok();
  result.handle = handle;
  return result;
}

SharedMemoryRingResult SharedMemoryIpcRing::ReleasePayloadHandle(
    const SharedMemoryPayloadHandle& handle) {
  if (handle.ring_generation != header_.ring_generation ||
      handle.slot_index >= header_.slot_count ||
      handle.payload_offset > payload_arena_.size() ||
      handle.payload_length > payload_arena_.size() - handle.payload_offset) {
    return Fail("IPC.RING.INVALID_HANDLE",
                "shared-memory IPC zero-copy payload handle is invalid");
  }
  auto active = std::find_if(
      active_payloads_.begin(), active_payloads_.end(),
      [&handle](const ActivePayloadRecord& record) {
        return record.active && SamePayloadIdentity(record.handle, handle);
      });
  if (active == active_payloads_.end()) {
    return Fail("IPC.RING.STALE_HANDLE",
                "shared-memory IPC zero-copy payload handle generation is stale");
  }
  if (!SamePayloadHandle(active->handle, handle)) {
    return Fail("IPC.RING.INVALID_HANDLE",
                "shared-memory IPC zero-copy payload handle is invalid");
  }
  active->active = false;
  return Ok();
}

SharedMemoryRingResult SharedMemoryIpcRing::ValidateEvidenceClaims(
    const std::vector<std::string>& evidence_claims) const {
  for (const auto& item : evidence_claims) {
    std::string token;
    if (IsForbiddenEvidence(item, &token)) {
      return Fail("IPC.RING.UNSUPPORTED_FALLBACK",
                  "shared-memory IPC ring refuses unsupported authority or row-object/text fallback claim: " +
                      token);
    }
  }
  return Ok();
}

SharedMemoryRingResult ValidateSharedMemoryRingParserPacketEnvelope(
    const std::vector<std::uint8_t>& packet_bytes) {
  constexpr std::uint32_t kParserPacketMagic = 0x53504943u;  // SPIC
  constexpr std::size_t kParserPacketHeaderSize = 24;
  constexpr std::uint32_t kParserProtocolMinSupported = 1;
  constexpr std::uint32_t kParserProtocolMaxSupported = 1;
  if (packet_bytes.size() < kParserPacketHeaderSize) {
    return Fail("IPC.RING.MALFORMED_FRAME",
                "shared-memory IPC parser/server packet header is truncated");
  }
  if (ReadU32(packet_bytes, 0) != kParserPacketMagic) {
    return Fail("IPC.RING.BAD_MAGIC",
                "shared-memory IPC parser/server packet magic is invalid");
  }
  constexpr std::uint16_t kSblrSubmitRequestOpcode = 8;
  if (ReadU16(packet_bytes, 4) != kSblrSubmitRequestOpcode) {
    return Fail("IPC.RING.MALFORMED_FRAME",
                "shared-memory IPC parser/server packet is not an SBLR submit request");
  }
  const auto protocol_version = ReadU32(packet_bytes, 8);
  if (protocol_version < kParserProtocolMinSupported ||
      protocol_version > kParserProtocolMaxSupported) {
    return Fail("IPC.RING.VERSION_MISMATCH",
                "shared-memory IPC parser/server packet protocol version is not supported");
  }
  const auto payload_length = ReadU32(packet_bytes, 20);
  if (payload_length > kSharedMemoryIpcMaxPayloadBytes ||
      packet_bytes.size() != kParserPacketHeaderSize + payload_length) {
    return Fail("IPC.RING.MALFORMED_FRAME",
                "shared-memory IPC parser/server packet payload is malformed or truncated");
  }
  return Ok();
}

SharedMemoryRingResult SharedMemoryIpcRing::AllocatePayloadRecord(
    SharedMemoryPayloadKind payload_kind,
    std::uint64_t slot_index,
    std::uint64_t slot_generation,
    const std::vector<std::uint8_t>& payload,
    SharedMemoryPayloadHandle* handle) {
  if (handle == nullptr) {
    return Fail("IPC.RING.MALFORMED",
                "shared-memory IPC zero-copy handle output is null");
  }
  if (payload.size() > payload_arena_.size()) {
    return Fail("IPC.RING.OVERSIZED_PAYLOAD",
                "shared-memory IPC ring zero-copy payload exceeds arena capacity");
  }
  const auto offset = FindPayloadOffset(
      active_payloads_, static_cast<std::uint64_t>(payload_arena_.size()),
      static_cast<std::uint64_t>(payload.size()));
  if (!offset.has_value() ||
      CandidateOverlapsActive(active_payloads_, *offset, payload.size())) {
    return Fail("IPC.RING.CAPACITY_EXHAUSTED",
                "shared-memory IPC ring zero-copy payload arena is exhausted");
  }
  std::copy(payload.begin(), payload.end(),
            payload_arena_.begin() + static_cast<std::ptrdiff_t>(*offset));

  SharedMemoryPayloadHandle allocated;
  allocated.kind = payload_kind;
  allocated.ring_generation = header_.ring_generation;
  allocated.slot_index = slot_index;
  allocated.slot_generation = slot_generation;
  allocated.payload_generation = next_payload_generation_++;
  allocated.payload_offset = *offset;
  allocated.payload_length = payload.size();
  allocated.payload_crc32c = LocalIpcCrc32c(payload.data(), payload.size());
  active_payloads_.push_back({allocated, true});
  *handle = allocated;
  return Ok();
}

}  // namespace scratchbird::ipc
