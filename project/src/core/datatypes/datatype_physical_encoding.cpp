// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_physical_encoding.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

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

u32 Checksum(CanonicalTypeId type_id,
             DatatypePhysicalValueState state,
             const std::vector<byte>& payload) {
  u32 value = 2166136261u;
  auto mix = [&value](u32 next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<u32>(type_id));
  mix(static_cast<u32>(state));
  for (byte item : payload) {
    mix(item);
  }
  return value;
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

  if (value.payload.empty()) {
    *detail = "payload_missing";
    return false;
  }

  switch (value.state) {
    case DatatypePhysicalValueState::value:
      if (layout.storage_class == DatatypeStorageClass::inline_fixed &&
          layout.inline_bytes != value.payload.size()) {
        *detail = "inline_fixed_size_mismatch";
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
  if (layout.type_id == CanonicalTypeId::null_type) {
    value.state = DatatypePhysicalValueState::sql_null;
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
  const auto layout = LookupDatatypeStorageLayout(value.type_id);
  if (!layout.ok()) {
    return Failure("SB-DATATYPE-PHYSICAL-UNKNOWN-TYPE",
                   "datatype.physical.unknown_type",
                   CanonicalTypeName(value.type_id));
  }
  std::string detail;
  if (!PayloadAllowedByLayout(value, layout.layout, &detail)) {
    return Failure("SB-DATATYPE-PHYSICAL-PAYLOAD-REFUSED",
                   "datatype.physical.payload_refused",
                   detail);
  }
  if (value.payload.size() > 0xffffffffu) {
    return Failure("SB-DATATYPE-PHYSICAL-PAYLOAD-TOO-LARGE",
                   "datatype.physical.payload_too_large",
                   CanonicalTypeName(value.type_id));
  }

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
  std::memcpy(result.bytes.data() + kHeaderBytes,
              value.payload.data(),
              value.payload.size());
  return result;
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
  DatatypePhysicalValue value;
  value.type_id = type_id;
  value.state = state;
  value.payload.assign(data + kHeaderBytes, data + size);
  if (Checksum(value.type_id, value.state, value.payload) !=
      LoadLittle32(data + kOffsetChecksum)) {
    return Failure("SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
                   "datatype.physical.checksum_mismatch",
                   CanonicalTypeName(type_id));
  }

  const auto layout = LookupDatatypeStorageLayout(value.type_id);
  if (!layout.ok()) {
    return Failure("SB-DATATYPE-PHYSICAL-UNKNOWN-TYPE",
                   "datatype.physical.unknown_type",
                   CanonicalTypeName(value.type_id));
  }
  std::string detail;
  if (!PayloadAllowedByLayout(value, layout.layout, &detail)) {
    return Failure("SB-DATATYPE-PHYSICAL-PAYLOAD-REFUSED",
                   "datatype.physical.payload_refused",
                   detail);
  }

  DatatypePhysicalEncodingResult result;
  result.status = PhysicalOkStatus();
  result.value = std::move(value);
  result.bytes.assign(data, data + size);
  return result;
}

}  // namespace scratchbird::core::datatypes
