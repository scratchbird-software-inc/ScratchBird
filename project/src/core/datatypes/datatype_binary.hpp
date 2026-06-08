// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-BINARY-ANCHOR
#include "datatype_layout.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kDatatypeBinaryEnvelopeHeaderBytes = 32;

enum class DatatypeBinaryFlag : u16 {
  none = 0,
  is_null = 1,
  payload_is_toast_reference = 2
};

enum class DatatypeDescriptorEnvelopeKind : u16 {
  datatype_transport = 1,
  domain_policy = 2,
  structured_descriptor = 3,
  document_descriptor = 4,
  advanced_family_descriptor = 5,
  unknown = 0xffffu
};

enum class DatatypeDescriptorIntegrityProfile : u16 {
  fast = 1,
  strong = 2,
  protected_keyed = 3,
  unknown = 0xffffu
};

struct DatatypeBinaryValue {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  bool is_null = false;
  bool payload_is_toast_reference = false;
  std::vector<byte> payload;
};

struct DatatypeDescriptorRecord {
  std::string field_name;
  std::vector<byte> payload;
};

struct DatatypeDescriptorEnvelope {
  DatatypeDescriptorEnvelopeKind kind = DatatypeDescriptorEnvelopeKind::unknown;
  DatatypeDescriptorIntegrityProfile integrity_profile =
      DatatypeDescriptorIntegrityProfile::strong;
  u16 layout_version = 2;
  std::vector<DatatypeDescriptorRecord> records;
};

struct DatatypeBinaryResult {
  Status status;
  DatatypeBinaryValue value;
  std::vector<byte> encoded;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeDescriptorEnvelopeResult {
  Status status;
  DatatypeDescriptorEnvelope envelope;
  std::vector<byte> encoded;
  std::string digest_algorithm;
  std::vector<byte> digest_material;
  u64 digest_low64 = 0;
  u64 digest_high64 = 0;
  u16 digest_bytes = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* DatatypeDescriptorEnvelopeKindName(DatatypeDescriptorEnvelopeKind kind);
const char* DatatypeDescriptorIntegrityProfileName(DatatypeDescriptorIntegrityProfile profile);
u64 ComputeDatatypeBinaryChecksum(const std::vector<byte>& bytes);
DatatypeDescriptorEnvelopeResult EncodeDatatypeDescriptorEnvelope(
    const DatatypeDescriptorEnvelope& envelope,
    const std::vector<byte>& protected_key_material = {});
DatatypeDescriptorEnvelopeResult DecodeDatatypeDescriptorEnvelope(
    const std::vector<byte>& encoded,
    const std::vector<byte>& protected_key_material = {});
DatatypeBinaryResult ValidateDatatypeBinaryValue(const DatatypeBinaryValue& value);
DatatypeBinaryResult EncodeDatatypeBinaryValue(const DatatypeBinaryValue& value);
DatatypeBinaryResult DecodeDatatypeBinaryValue(const std::vector<byte>& encoded);
DiagnosticRecord MakeDatatypeBinaryDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::core::datatypes
