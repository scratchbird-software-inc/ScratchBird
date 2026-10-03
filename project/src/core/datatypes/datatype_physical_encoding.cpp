// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_physical_encoding.hpp"

#include "canonical_utf8.hpp"

#include <array>
#include <cstring>
#include <new>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

inline constexpr std::size_t kCanonicalCharacterMaximumBytes = 16'777'216;
inline constexpr std::size_t kCanonicalBinaryMaximumBytes = 16'777'216;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kPhysicalEncodingMagic = {
    'S', 'B', 'D', 'P', 'V', '0', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetType = 8;
inline constexpr u32 kOffsetState = 12;
inline constexpr u32 kOffsetFlags = 14;
inline constexpr u32 kOffsetPayloadBytes = 16;
inline constexpr u32 kOffsetChecksum = 20;

Status PhysicalOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status PhysicalErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::datatypes};
}

DiagnosticRecord PhysicalDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.physical_encoding");
}

DatatypePhysicalEncodingResult Failure(std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail = {}) {
  DatatypePhysicalEncodingResult result;
  result.status = PhysicalErrorStatus();
  result.diagnostic = PhysicalDiagnostic(result.status,
                                         std::move(diagnostic_code),
                                         std::move(message_key),
                                         std::move(detail));
  return result;
}

DatatypePhysicalEncodingResult BinaryResourceFailure(std::string detail) {
  DatatypePhysicalEncodingResult result;
  result.status = {StatusCode::memory_allocation_failed, Severity::error,
                   Subsystem::datatypes};
  result.diagnostic = PhysicalDiagnostic(
      result.status, "CTB.BINARY.RESOURCE_EXHAUSTED",
      "datatype.physical.resource_exhausted", std::move(detail));
  return result;
}

u32 Checksum(CanonicalTypeId type_id,
             DatatypePhysicalValueState state,
             const byte* payload,
             u64 payload_size) {
  u32 value = 2166136261u;
  auto mix = [&value](u32 next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<u32>(type_id));
  mix(static_cast<u32>(state));
  for (u64 index = 0; index < payload_size; ++index) {
    mix(payload[index]);
  }
  return value;
}

u32 Checksum(CanonicalTypeId type_id,
             DatatypePhysicalValueState state,
             const std::vector<byte>& payload) {
  return Checksum(type_id, state, payload.data(), payload.size());
}

std::vector<byte> FixedPayload(u32 size, byte seed) {
  std::vector<byte> payload(size);
  for (u32 index = 0; index < size; ++index) {
    payload[index] = static_cast<byte>(seed + index);
  }
  return payload;
}

bool PayloadAllowedByLayout(const DatatypePhysicalValue& value,
                            const DatatypeStorageLayout& layout,
                            std::string* detail) {
  if (value.state == DatatypePhysicalValueState::sql_null) {
    if (!value.payload.empty()) {
      *detail = "null_payload_present";
      return false;
    }
    return true;
  }

  // SBDPV001 is a structural component envelope, not page/LOB authority.
  // base.binary therefore admits only a direct VALUE or payload-free SQL_NULL
  // at this boundary.
  if (value.type_id == CanonicalTypeId::binary &&
      value.state != DatatypePhysicalValueState::value) {
    *detail = "binary_state_not_admitted";
    return false;
  }

  const bool empty_direct_value =
      value.state == DatatypePhysicalValueState::value &&
      (value.type_id == CanonicalTypeId::character ||
       value.type_id == CanonicalTypeId::binary);
  if (value.payload.empty() && !empty_direct_value) {
    *detail = "payload_missing";
    return false;
  }

  switch (value.state) {
    case DatatypePhysicalValueState::value:
      if (value.type_id == CanonicalTypeId::character) {
        if (value.payload.size() > kCanonicalCharacterMaximumBytes) {
          *detail = "character_length_exceeded";
          return false;
        }
        if (!ValidateCanonicalUtf8(value.payload.data(), value.payload.size())) {
          *detail = "character_utf8_invalid";
          return false;
        }
      }
      if (value.type_id == CanonicalTypeId::binary &&
          value.payload.size() > kCanonicalBinaryMaximumBytes) {
        *detail = "binary_length_exceeded";
        return false;
      }
      if (layout.storage_class == DatatypeStorageClass::inline_fixed &&
          layout.inline_bytes != value.payload.size()) {
        *detail = "inline_fixed_size_mismatch";
        return false;
      }
      if (value.type_id == CanonicalTypeId::boolean &&
          (value.payload.size() != 1 ||
           (value.payload[0] != static_cast<byte>(0) &&
            value.payload[0] != static_cast<byte>(1)))) {
        *detail = "boolean_payload_invalid";
        return false;
      }
      return true;
    case DatatypePhysicalValueState::overflow_root:
      if (!layout.may_overflow_to_toast &&
          layout.storage_class != DatatypeStorageClass::toast_reference) {
        *detail = "overflow_root_not_allowed";
        return false;
      }
      return value.payload.size() >= 16;
    case DatatypePhysicalValueState::overflow_chunk:
      if (!layout.may_overflow_to_toast &&
          layout.storage_class != DatatypeStorageClass::toast_reference) {
        *detail = "overflow_chunk_not_allowed";
        return false;
      }
      return true;
    case DatatypePhysicalValueState::locator_handle:
      if (layout.encoding != DatatypeBinaryEncoding::locator_envelope &&
          layout.storage_class != DatatypeStorageClass::toast_reference) {
        *detail = "locator_handle_not_allowed";
        return false;
      }
      return value.payload.size() >= 8;
    case DatatypePhysicalValueState::opaque_handle:
      if (layout.encoding != DatatypeBinaryEncoding::opaque_extension_binary) {
        *detail = "opaque_handle_not_allowed";
        return false;
      }
      return true;
    case DatatypePhysicalValueState::protected_chunk_root:
      return value.payload.size() >= 16;
    case DatatypePhysicalValueState::unknown:
      *detail = "state_unknown";
      return false;
  }
  *detail = "state_unknown";
  return false;
}

const char* PayloadRefusalDiagnosticCode(
    DatatypePhysicalValueState state,
    const std::string& detail) noexcept {
  if (state == DatatypePhysicalValueState::sql_null) {
    return "DATATYPE.NULL_STATE.INVALID";
  }
  if (detail == "character_length_exceeded") {
    return "CTB.TEXT.LENGTH_EXCEEDED";
  }
  if (detail == "character_utf8_invalid") {
    return "CTB.TEXT.INVALID_ENCODING";
  }
  if (detail == "binary_length_exceeded") {
    return "CTB.BINARY.LENGTH_EXCEEDED";
  }
  if (detail == "binary_state_not_admitted") {
    return "CTB.BINARY.FRAME_INVALID";
  }
  return "SB-DATATYPE-PHYSICAL-PAYLOAD-REFUSED";
}

}  // namespace

const char* DatatypePhysicalValueStateName(
    DatatypePhysicalValueState state) {
  switch (state) {
    case DatatypePhysicalValueState::sql_null:
      return "sql_null";
    case DatatypePhysicalValueState::value:
      return "value";
    case DatatypePhysicalValueState::overflow_root:
      return "overflow_root";
    case DatatypePhysicalValueState::overflow_chunk:
      return "overflow_chunk";
    case DatatypePhysicalValueState::locator_handle:
      return "locator_handle";
    case DatatypePhysicalValueState::opaque_handle:
      return "opaque_handle";
    case DatatypePhysicalValueState::protected_chunk_root:
      return "protected_chunk_root";
    case DatatypePhysicalValueState::unknown:
      return "unknown";
  }
  return "unknown";
}

DatatypePhysicalValue SampleDatatypePhysicalValueForLayout(
    const DatatypeStorageLayout& layout) {
  DatatypePhysicalValue value;
  value.type_id = layout.type_id;
  value.state = DatatypePhysicalValueState::value;
  if (layout.type_id == CanonicalTypeId::boolean) {
    value.payload = {static_cast<byte>(0)};
    return value;
  }
  if (layout.storage_class == DatatypeStorageClass::inline_fixed) {
    value.payload = FixedPayload(layout.inline_bytes, 0x31);
  } else if (layout.storage_class == DatatypeStorageClass::toast_reference) {
    value.state = DatatypePhysicalValueState::locator_handle;
    value.payload = FixedPayload(24, 0x51);
  } else if (layout.encoding == DatatypeBinaryEncoding::locator_envelope) {
    value.state = DatatypePhysicalValueState::locator_handle;
    value.payload = FixedPayload(16, 0x61);
  } else if (layout.encoding == DatatypeBinaryEncoding::opaque_extension_binary) {
    value.state = DatatypePhysicalValueState::opaque_handle;
    value.payload = FixedPayload(16, 0x71);
  } else {
    value.payload = FixedPayload(layout.inline_bytes != 0 ? layout.inline_bytes : 16,
                                 0x41);
  }
  return value;
}

DatatypePhysicalEncodingResult EncodeDatatypePhysicalValue(
    const DatatypePhysicalValue& value) {
  if (value.type_id == CanonicalTypeId::null_type ||
      value.type_id == CanonicalTypeId::unknown) {
    return Failure("DATATYPE.DESCRIPTOR.INVALID",
                   "datatype.physical.standalone_type_invalid");
  }
  const auto layout = LookupDatatypeStorageLayout(value.type_id);
  if (!layout.ok()) {
    return Failure("SB-DATATYPE-PHYSICAL-UNKNOWN-TYPE",
                   "datatype.physical.unknown_type",
                   CanonicalTypeName(value.type_id));
  }
  std::string detail;
  if (!PayloadAllowedByLayout(value, layout.layout, &detail)) {
    return Failure(PayloadRefusalDiagnosticCode(value.state, detail),
                   "datatype.physical.payload_refused",
                   detail);
  }
  if (value.payload.size() > 0xffffffffu) {
    return Failure("SB-DATATYPE-PHYSICAL-PAYLOAD-TOO-LARGE",
                   "datatype.physical.payload_too_large",
                   CanonicalTypeName(value.type_id));
  }

  try {
    DatatypePhysicalEncodingResult result;
    result.status = PhysicalOkStatus();
    result.value = value;
    result.bytes.resize(kHeaderBytes + value.payload.size());
    std::memcpy(result.bytes.data() + kOffsetMagic,
                kPhysicalEncodingMagic.data(),
                kPhysicalEncodingMagic.size());
    StoreLittle32(result.bytes.data() + kOffsetType,
                  static_cast<u32>(value.type_id));
    StoreLittle16(result.bytes.data() + kOffsetState,
                  static_cast<u16>(value.state));
    StoreLittle16(result.bytes.data() + kOffsetFlags, 0);
    StoreLittle32(result.bytes.data() + kOffsetPayloadBytes,
                  static_cast<u32>(value.payload.size()));
    StoreLittle32(result.bytes.data() + kOffsetChecksum,
                  Checksum(value.type_id, value.state, value.payload));
    if (!value.payload.empty()) {
      std::memcpy(result.bytes.data() + kHeaderBytes,
                  value.payload.data(),
                  value.payload.size());
    }
    return result;
  } catch (const std::bad_alloc&) {
    if (value.type_id != CanonicalTypeId::binary) throw;
    return BinaryResourceFailure("binary_encode_allocation_failed");
  }
}

DatatypePhysicalEncodingResult DecodeDatatypePhysicalValue(
    const byte* data,
    u64 size) {
  if (data == nullptr || size < kHeaderBytes) {
    return Failure("SB-DATATYPE-PHYSICAL-TRUNCATED",
                   "datatype.physical.truncated");
  }
  if (std::memcmp(data + kOffsetMagic,
                  kPhysicalEncodingMagic.data(),
                  kPhysicalEncodingMagic.size()) != 0) {
    return Failure("SB-DATATYPE-PHYSICAL-BAD-MAGIC",
                   "datatype.physical.bad_magic");
  }
  const auto type_id = static_cast<CanonicalTypeId>(
      LoadLittle32(data + kOffsetType));
  const auto state = static_cast<DatatypePhysicalValueState>(
      LoadLittle16(data + kOffsetState));
  const u32 flags = LoadLittle16(data + kOffsetFlags);
  if (flags != 0) {
    return Failure("SB-DATATYPE-PHYSICAL-BAD-FLAGS",
                   "datatype.physical.bad_flags",
                   CanonicalTypeName(type_id));
  }
  const u32 payload_size = LoadLittle32(data + kOffsetPayloadBytes);
  if (size != static_cast<u64>(kHeaderBytes) + payload_size) {
    return Failure("SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH",
                   "datatype.physical.length_mismatch",
                   CanonicalTypeName(type_id));
  }
  // Reject base.binary states and lengths from the fixed header before
  // checksum traversal and before materializing caller-controlled bytes.
  if (type_id == CanonicalTypeId::binary) {
    if (state == DatatypePhysicalValueState::sql_null && payload_size != 0) {
      return Failure("DATATYPE.NULL_STATE.INVALID",
                     "datatype.physical.payload_refused",
                     "null_payload_present");
    }
    if (state != DatatypePhysicalValueState::sql_null &&
        state != DatatypePhysicalValueState::value) {
      return Failure("CTB.BINARY.FRAME_INVALID",
                     "datatype.physical.payload_refused",
                     "binary_state_not_admitted");
    }
    if (state == DatatypePhysicalValueState::value &&
        payload_size > kCanonicalBinaryMaximumBytes) {
      return Failure("CTB.BINARY.LENGTH_EXCEEDED",
                     "datatype.physical.payload_refused",
                     "binary_length_exceeded");
    }
  }
  if (Checksum(type_id, state, data + kHeaderBytes, payload_size) !=
      LoadLittle32(data + kOffsetChecksum)) {
    return Failure("SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
                   "datatype.physical.checksum_mismatch",
                   CanonicalTypeName(type_id));
  }
  if (type_id == CanonicalTypeId::null_type ||
      type_id == CanonicalTypeId::unknown) {
    return Failure("DATATYPE.DESCRIPTOR.INVALID",
                   "datatype.physical.standalone_type_invalid");
  }
  // SQL NULL has no physical payload. Reject its state/length contradiction
  // before materializing caller-controlled bytes into an owned vector.
  if (state == DatatypePhysicalValueState::sql_null && payload_size != 0) {
    return Failure("DATATYPE.NULL_STATE.INVALID",
                   "datatype.physical.payload_refused",
                   "null_payload_present");
  }
  // Reject oversized inline character frames before copying caller-controlled
  // payload bytes into the owned value buffer.
  if (state == DatatypePhysicalValueState::value &&
      type_id == CanonicalTypeId::character &&
      payload_size > kCanonicalCharacterMaximumBytes) {
    return Failure("CTB.TEXT.LENGTH_EXCEEDED",
                   "datatype.physical.payload_refused",
                   "character_length_exceeded");
  }
  DatatypePhysicalValue value;
  value.type_id = type_id;
  value.state = state;
  try {
    value.payload.assign(data + kHeaderBytes, data + size);
  } catch (const std::bad_alloc&) {
    if (type_id != CanonicalTypeId::binary) throw;
    return BinaryResourceFailure("binary_decode_payload_allocation_failed");
  }

  const auto layout = LookupDatatypeStorageLayout(value.type_id);
  if (!layout.ok()) {
    return Failure("SB-DATATYPE-PHYSICAL-UNKNOWN-TYPE",
                   "datatype.physical.unknown_type",
                   CanonicalTypeName(value.type_id));
  }
  std::string detail;
  if (!PayloadAllowedByLayout(value, layout.layout, &detail)) {
    return Failure(PayloadRefusalDiagnosticCode(value.state, detail),
                   "datatype.physical.payload_refused",
                   detail);
  }

  DatatypePhysicalEncodingResult result;
  result.status = PhysicalOkStatus();
  result.value = std::move(value);
  try {
    result.bytes.assign(data, data + size);
  } catch (const std::bad_alloc&) {
    if (type_id != CanonicalTypeId::binary) throw;
    return BinaryResourceFailure("binary_decode_frame_allocation_failed");
  }
  return result;
}

}  // namespace scratchbird::core::datatypes
