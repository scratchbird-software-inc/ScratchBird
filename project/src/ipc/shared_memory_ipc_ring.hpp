// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "local_ipc_contract.hpp"
#include "runtime_capabilities.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::ipc {

inline constexpr std::uint32_t kSharedMemoryIpcRingMagic = 0x52494253u;  // "SBIR"
inline constexpr std::uint16_t kSharedMemoryIpcRingMajor = 1;
inline constexpr std::uint16_t kSharedMemoryIpcRingMinor = 0;
inline constexpr std::uint64_t kSharedMemoryIpcMaxPayloadBytes =
    static_cast<std::uint64_t>(kLocalIpcMaxPayloadBytes);

enum class SharedMemoryPayloadKind : std::uint16_t {
  kBinarySblr = 1,
  kBinaryResultFrame = 2,
};

enum class SharedMemoryRingFrameKind : std::uint16_t {
  kRequest = 1,
  kResult = 2,
};

struct SharedMemoryPayloadHandle {
  SharedMemoryPayloadKind kind{SharedMemoryPayloadKind::kBinarySblr};
  std::uint64_t ring_generation{0};
  std::uint64_t slot_index{0};
  std::uint64_t slot_generation{0};
  std::uint64_t payload_generation{0};
  std::uint64_t payload_offset{0};
  std::uint64_t payload_length{0};
  std::uint32_t payload_crc32c{0};
};

struct SharedMemoryRingFrame {
  SharedMemoryRingFrameKind frame_kind{SharedMemoryRingFrameKind::kRequest};
  SharedMemoryPayloadKind payload_kind{SharedMemoryPayloadKind::kBinarySblr};
  std::uint64_t request_id{0};
  std::uint64_t sequence{0};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> transaction_uuid{};
  std::uint64_t auth_epoch{0};
  std::uint32_t flags{0};
  std::vector<std::uint8_t> inline_payload;
  bool has_handle{false};
  SharedMemoryPayloadHandle handle;
};

struct SharedMemoryRingOptions {
  std::uint64_t slot_count{8};
  std::uint64_t payload_capacity{1024 * 1024};
  std::uint64_t max_inline_payload_bytes{4096};
  std::uint64_t max_payload_bytes{kSharedMemoryIpcMaxPayloadBytes};
  bool require_transaction_authority{true};
  scratchbird::core::platform::RuntimeCompatibilityDescriptor
      runtime_compatibility;
};

struct SharedMemoryRingDiagnostic {
  std::string code;
  std::string message;
};

struct SharedMemoryRingResult {
  bool ok{false};
  SharedMemoryRingDiagnostic diagnostic;
  SharedMemoryRingFrame frame;
  SharedMemoryPayloadHandle handle;
  std::vector<std::string> evidence;

  explicit operator bool() const { return ok; }
};

struct SharedMemoryRingHeaderView {
  std::uint32_t magic{kSharedMemoryIpcRingMagic};
  std::uint16_t major{kSharedMemoryIpcRingMajor};
  std::uint16_t minor{kSharedMemoryIpcRingMinor};
  std::uint64_t ring_generation{1};
  std::uint64_t slot_count{0};
  std::uint64_t payload_capacity{0};
  std::uint64_t producer_cursor{0};
  std::uint64_t consumer_cursor{0};
  std::uint64_t next_request_sequence{1};
  std::uint64_t next_result_sequence{1};
  std::uint64_t runtime_generation{
      scratchbird::core::platform::CurrentRuntimeCompatibilityGeneration()};
  std::uint64_t compatibility_epoch{
      scratchbird::core::platform::CurrentRuntimeCompatibilityEpoch()};
  std::uint16_t runtime_endian{0};
  std::uint16_t required_alignment{1};
};

class SharedMemoryIpcRing {
 public:
  static SharedMemoryRingResult Create(const SharedMemoryRingOptions& options,
                                       SharedMemoryIpcRing* ring);

  [[nodiscard]] SharedMemoryRingHeaderView header() const;
  [[nodiscard]] std::uint64_t available_slots() const;
  [[nodiscard]] std::uint64_t readable_slots() const;

  SharedMemoryRingResult EnqueueRequest(SharedMemoryPayloadKind payload_kind,
                                        std::uint64_t request_id,
                                        std::array<std::uint8_t, 16> session_uuid,
                                        std::array<std::uint8_t, 16> transaction_uuid,
                                        std::uint64_t auth_epoch,
                                        const std::vector<std::uint8_t>& payload);

  SharedMemoryRingResult EnqueueResult(std::uint64_t request_id,
                                       std::array<std::uint8_t, 16> session_uuid,
                                       std::array<std::uint8_t, 16> transaction_uuid,
                                       std::uint64_t auth_epoch,
                                       const std::vector<std::uint8_t>& payload);

  SharedMemoryRingResult Dequeue(SharedMemoryRingFrame* frame);
  SharedMemoryRingResult ResolveHandle(const SharedMemoryPayloadHandle& handle,
                                       std::vector<std::uint8_t>* payload) const;
  SharedMemoryRingResult ReleasePayloadHandle(
      const SharedMemoryPayloadHandle& handle);
  SharedMemoryRingResult ValidateHeader(std::uint32_t magic,
                                        std::uint16_t major,
                                        std::uint16_t minor) const;
  SharedMemoryRingResult ValidateEvidenceClaims(
      const std::vector<std::string>& evidence_claims) const;

  struct ActivePayloadRecord {
    SharedMemoryPayloadHandle handle;
    bool active{false};
  };

 private:
  struct Slot {
    bool occupied{false};
    SharedMemoryRingFrame frame;
    std::uint64_t slot_generation{1};
  };

  SharedMemoryRingResult Enqueue(SharedMemoryRingFrameKind frame_kind,
                                 SharedMemoryPayloadKind payload_kind,
                                 std::uint64_t request_id,
                                 std::array<std::uint8_t, 16> session_uuid,
                                 std::array<std::uint8_t, 16> transaction_uuid,
                                 std::uint64_t auth_epoch,
                                 const std::vector<std::uint8_t>& payload);
  SharedMemoryRingResult AllocatePayloadRecord(
      SharedMemoryPayloadKind payload_kind,
      std::uint64_t slot_index,
      std::uint64_t slot_generation,
      const std::vector<std::uint8_t>& payload,
      SharedMemoryPayloadHandle* handle);

  SharedMemoryRingOptions options_;
  SharedMemoryRingHeaderView header_;
  std::vector<Slot> slots_;
  std::vector<ActivePayloadRecord> active_payloads_;
  std::vector<std::uint8_t> payload_arena_;
  std::uint64_t next_payload_generation_{1};
};

SharedMemoryRingResult ValidateSharedMemoryRingRouteAuthority(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& transaction_uuid,
    std::uint64_t auth_epoch,
    bool require_transaction_authority);

SharedMemoryRingResult ValidateSharedMemoryRingParserPacketEnvelope(
    const std::vector<std::uint8_t>& packet_bytes);

}  // namespace scratchbird::ipc
