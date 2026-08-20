// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

// MGA-CMO-ADMITTED-MANAGEMENT-ENVELOPE-WIRE-V1
inline constexpr std::uint16_t kManagementEnvelopeWireMajor = 1;
inline constexpr std::uint16_t kManagementEnvelopeWireMinor = 0;
// SBOP v1 reserves 24 bytes for the typed-literal carrier, so a management
// frame carried by SBOP is bounded below the general 65,536-byte scalar cap.
inline constexpr std::uint32_t kManagementEnvelopeMaximumBytes = 65'512;
inline constexpr std::uint16_t kManagementEnvelopeMaximumFields = 32;
inline constexpr std::uint32_t kManagementEnvelopeMaximumFieldBytes = 16'384;

enum class SblrManagementEnvelopeKind : std::uint16_t {
  operation = 1,
  payload = 2,
  result = 3,
  progress = 4,
  diagnostic = 5,
  metric_snapshot_ref = 6,
};

struct SblrManagementEnvelopeField {
  std::string name;
  std::vector<std::uint8_t> value;
};

struct SblrManagementEnvelopeRecord {
  SblrManagementEnvelopeKind kind = SblrManagementEnvelopeKind::operation;
  std::vector<SblrManagementEnvelopeField> fields;
};

struct SblrManagementEnvelopeCodecResult {
  bool ok = false;
  SblrManagementEnvelopeRecord record;
  std::vector<std::uint8_t> canonical_bytes;
  std::string sha256_hex;
  std::string diagnostic_id;
  std::string detail;
};

struct SblrManagementEnvelopeDispatchResult {
  bool accepted = false;
  std::string diagnostic_id;
  std::string detail;
  std::vector<scratchbird::engine::internal_api::EngineEvidenceReference> evidence;
};

bool IsManagementEnvelopeOperation(std::string_view operation_id) noexcept;
std::string_view ManagementEnvelopeOperationId(SblrManagementEnvelopeKind kind) noexcept;
std::string_view ManagementEnvelopeOpcode(SblrManagementEnvelopeKind kind) noexcept;
std::uint16_t ManagementEnvelopeOpcodeCode(SblrManagementEnvelopeKind kind) noexcept;

SblrManagementEnvelopeCodecResult EncodeSblrManagementEnvelopeRecord(
    const SblrManagementEnvelopeRecord& record);
SblrManagementEnvelopeCodecResult DecodeSblrManagementEnvelopeRecord(
    const std::uint8_t* data, std::size_t size);
SblrManagementEnvelopeCodecResult DecodeSblrManagementEnvelopeOperand(
    const SblrOperationEnvelope& envelope);
SblrOperand MakeSblrManagementEnvelopeOperand(
    const SblrManagementEnvelopeCodecResult& encoded);

SblrManagementEnvelopeDispatchResult DispatchSblrManagementEnvelope(
    const SblrOperationEnvelope& envelope,
    const scratchbird::engine::internal_api::EngineRequestContext& context);

}  // namespace scratchbird::engine::sblr
