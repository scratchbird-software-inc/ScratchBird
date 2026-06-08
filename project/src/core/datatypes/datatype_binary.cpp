// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
namespace core_hash = scratchbird::core::hash;

inline constexpr byte kDatatypeBinaryMagic[8] = {'S', 'B', 'D', 'V', 'A', 'L', '0', '1'};
inline constexpr byte kDatatypeDescriptorEnvelopeMagic[8] = {
    'S', 'B', 'D', 'E', 'S', 'C', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetTypeId = 8;
inline constexpr u32 kOffsetFlags = 12;
inline constexpr u32 kOffsetHeaderBytes = 14;
inline constexpr u32 kOffsetPayloadBytes = 16;
inline constexpr u32 kOffsetPayloadChecksum = 24;
inline constexpr u32 kDescriptorOffsetVersion = 8;
inline constexpr u32 kDescriptorOffsetKind = 10;
inline constexpr u32 kDescriptorOffsetProfile = 12;
inline constexpr u32 kDescriptorOffsetFlags = 14;
inline constexpr u32 kDescriptorOffsetHeaderBytes = 16;
inline constexpr u32 kDescriptorOffsetRecordCount = 20;
inline constexpr u32 kDescriptorOffsetRecordBytes = 24;
inline constexpr u32 kDescriptorOffsetDigestBytes = 32;
inline constexpr u32 kDescriptorOffsetDigestLow64 = 32;
inline constexpr u32 kDescriptorOffsetDigestHigh64 = 40;
inline constexpr u32 kDatatypeDescriptorEnvelopeDigestBytes = 32;
inline constexpr u32 kDatatypeDescriptorEnvelopeHeaderBytes = 64;
inline constexpr u16 kDatatypeDescriptorEnvelopeLayoutVersion = 2;
inline constexpr u64 kFnvOffsetBasis64 = 1469598103934665603ull;
inline constexpr u64 kFnvPrime64 = 1099511628211ull;

namespace BinaryFlag {
inline constexpr u16 is_null = 1u << 0;
inline constexpr u16 payload_is_toast_reference = 1u << 1;
}  // namespace BinaryFlag

Status BinaryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status BinaryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

DatatypeBinaryResult BinaryError(std::string diagnostic_code,
                                 std::string message_key,
                                 std::string detail = {}) {
  DatatypeBinaryResult result;
  result.status = BinaryErrorStatus();
  result.diagnostic = MakeDatatypeBinaryDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

DatatypeDescriptorEnvelopeResult DescriptorError(std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {}) {
  DatatypeDescriptorEnvelopeResult result;
  result.status = BinaryErrorStatus();
  result.diagnostic = MakeDatatypeBinaryDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

bool IsValidFixedPayloadSize(const DatatypeStorageLayout& layout, u32 payload_size) {
  if (layout.type_id == CanonicalTypeId::null_type) {
    return payload_size == 0;
  }
  if (layout.storage_class == DatatypeStorageClass::inline_fixed) {
    return payload_size == layout.inline_bytes;
  }
  if (layout.type_id == CanonicalTypeId::decimal || layout.type_id == CanonicalTypeId::blob) {
    return payload_size == layout.inline_bytes;
  }
  return true;
}

u16 FlagsFor(const DatatypeBinaryValue& value) {
  u16 flags = 0;
  if (value.is_null) {
    flags |= BinaryFlag::is_null;
  }
  if (value.payload_is_toast_reference) {
    flags |= BinaryFlag::payload_is_toast_reference;
  }
  return flags;
}

void AppendU16(std::vector<byte>* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

void AppendBytes(std::vector<byte>* out, const byte* data, std::size_t size) {
  if (size == 0) {
    return;
  }
  out->insert(out->end(), data, data + size);
}

bool ValidDescriptorFieldName(const std::string& name) {
  if (name.empty() || name.size() > std::numeric_limits<u16>::max()) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool DescriptorEnvelopeKindSupported(DatatypeDescriptorEnvelopeKind kind) {
  switch (kind) {
    case DatatypeDescriptorEnvelopeKind::datatype_transport:
    case DatatypeDescriptorEnvelopeKind::domain_policy:
    case DatatypeDescriptorEnvelopeKind::structured_descriptor:
    case DatatypeDescriptorEnvelopeKind::document_descriptor:
    case DatatypeDescriptorEnvelopeKind::advanced_family_descriptor:
      return true;
    case DatatypeDescriptorEnvelopeKind::unknown:
      return false;
  }
  return false;
}

bool DescriptorIntegrityProfileSupported(DatatypeDescriptorIntegrityProfile profile) {
  switch (profile) {
    case DatatypeDescriptorIntegrityProfile::strong:
    case DatatypeDescriptorIntegrityProfile::protected_keyed:
      return true;
    case DatatypeDescriptorIntegrityProfile::fast:
    case DatatypeDescriptorIntegrityProfile::unknown:
      return false;
  }
  return false;
}

u64 Fnv1a64(const std::vector<byte>& bytes, u64 seed) {
  u64 hash = seed;
  for (const byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime64;
  }
  return hash;
}

void AttachDescriptorDigest(DatatypeDescriptorEnvelopeResult* result,
                            const core_hash::HashDigestResult& computed,
                            std::string algorithm) {
  result->digest_algorithm = std::move(algorithm);
  result->digest_material = core_hash::DigestVector(computed.digest);
  result->digest_low64 = core_hash::DigestLow64(computed.digest);
  result->digest_high64 = core_hash::DigestHigh64(computed.digest);
  result->digest_bytes = computed.digest_bytes;
}

DatatypeDescriptorEnvelopeResult DescriptorHashError(
    const core_hash::HashDigestResult& failure) {
  DatatypeDescriptorEnvelopeResult result;
  result.status = failure.status;
  result.diagnostic = MakeDatatypeBinaryDiagnostic(
      result.status,
      failure.diagnostic.diagnostic_code.empty()
          ? "SB-DATATYPE-DESCRIPTOR-CRYPTO-DIGEST-FAILED"
          : failure.diagnostic.diagnostic_code,
      failure.diagnostic.message_key.empty()
          ? "datatype.descriptor.crypto_digest_failed"
          : failure.diagnostic.message_key);
  return result;
}

DatatypeDescriptorEnvelopeResult ComputeDescriptorEnvelopeDigest(
    DatatypeDescriptorIntegrityProfile profile,
    const std::vector<byte>& digest_input,
    const std::vector<byte>& protected_key_material) {
  DatatypeDescriptorEnvelopeResult result;
  result.status = BinaryOkStatus();
  result.envelope.integrity_profile = profile;

  switch (profile) {
    case DatatypeDescriptorIntegrityProfile::strong:
    {
      const auto computed = core_hash::ComputeSha256Digest(digest_input);
      if (!computed.ok()) {
        return DescriptorHashError(computed);
      }
      AttachDescriptorDigest(&result, computed, "sha256");
      return result;
    }
    case DatatypeDescriptorIntegrityProfile::protected_keyed:
      if (protected_key_material.empty()) {
        return DescriptorError("SB-DATATYPE-DESCRIPTOR-PROTECTED-MATERIAL-REQUIRED",
                               "datatype.descriptor.protected_material_required");
      }
    {
      const auto computed =
          core_hash::ComputeHmacSha256Digest(protected_key_material,
                                             digest_input);
      if (!computed.ok()) {
        return DescriptorHashError(computed);
      }
      AttachDescriptorDigest(&result, computed, "hmac-sha256");
      return result;
    }
    case DatatypeDescriptorIntegrityProfile::fast:
    case DatatypeDescriptorIntegrityProfile::unknown:
      break;
  }

  return DescriptorError("SB-DATATYPE-DESCRIPTOR-INTEGRITY-PROFILE-UNSUPPORTED",
                         "datatype.descriptor.integrity_profile_unsupported",
                         DatatypeDescriptorIntegrityProfileName(profile));
}

std::vector<byte> DigestInputWithZeroedDescriptorDigest(std::vector<byte> encoded) {
  if (encoded.size() >= kDatatypeDescriptorEnvelopeHeaderBytes) {
    std::fill(encoded.begin() + kDescriptorOffsetDigestBytes,
              encoded.begin() + kDescriptorOffsetDigestBytes +
                  kDatatypeDescriptorEnvelopeDigestBytes,
              static_cast<byte>(0));
  }
  return encoded;
}

DatatypeDescriptorEnvelopeResult ValidateDescriptorEnvelope(
    const DatatypeDescriptorEnvelope& envelope,
    const std::vector<byte>& protected_key_material) {
  if (envelope.layout_version != kDatatypeDescriptorEnvelopeLayoutVersion) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-LAYOUT-VERSION-UNSUPPORTED",
                           "datatype.descriptor.layout_version_unsupported");
  }
  if (!DescriptorEnvelopeKindSupported(envelope.kind)) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-KIND-UNSUPPORTED",
                           "datatype.descriptor.kind_unsupported",
                           DatatypeDescriptorEnvelopeKindName(envelope.kind));
  }
  if (!DescriptorIntegrityProfileSupported(envelope.integrity_profile)) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-INTEGRITY-PROFILE-WEAK",
                           "datatype.descriptor.integrity_profile_weak",
                           DatatypeDescriptorIntegrityProfileName(envelope.integrity_profile));
  }
  if (envelope.integrity_profile == DatatypeDescriptorIntegrityProfile::protected_keyed &&
      protected_key_material.empty()) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-PROTECTED-MATERIAL-REQUIRED",
                           "datatype.descriptor.protected_material_required");
  }
  if (envelope.records.empty()) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-RECORDS-REQUIRED",
                           "datatype.descriptor.records_required");
  }
  if (envelope.records.size() > std::numeric_limits<u32>::max()) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-RECORD-COUNT-INVALID",
                           "datatype.descriptor.record_count_invalid");
  }
  std::set<std::string> names;
  for (const auto& record : envelope.records) {
    if (!ValidDescriptorFieldName(record.field_name)) {
      return DescriptorError("SB-DATATYPE-DESCRIPTOR-FIELD-NAME-INVALID",
                             "datatype.descriptor.field_name_invalid",
                             record.field_name);
    }
    if (!names.insert(record.field_name).second) {
      return DescriptorError("SB-DATATYPE-DESCRIPTOR-FIELD-DUPLICATE",
                             "datatype.descriptor.field_duplicate",
                             record.field_name);
    }
  }

  DatatypeDescriptorEnvelopeResult result;
  result.status = BinaryOkStatus();
  result.envelope = envelope;
  return result;
}

}  // namespace

u64 ComputeDatatypeBinaryChecksum(const std::vector<byte>& bytes) {
  u64 hash = kFnvOffsetBasis64;
  for (byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime64;
  }
  return hash;
}

const char* DatatypeDescriptorEnvelopeKindName(DatatypeDescriptorEnvelopeKind kind) {
  switch (kind) {
    case DatatypeDescriptorEnvelopeKind::datatype_transport:
      return "datatype_transport";
    case DatatypeDescriptorEnvelopeKind::domain_policy:
      return "domain_policy";
    case DatatypeDescriptorEnvelopeKind::structured_descriptor:
      return "structured_descriptor";
    case DatatypeDescriptorEnvelopeKind::document_descriptor:
      return "document_descriptor";
    case DatatypeDescriptorEnvelopeKind::advanced_family_descriptor:
      return "advanced_family_descriptor";
    case DatatypeDescriptorEnvelopeKind::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* DatatypeDescriptorIntegrityProfileName(DatatypeDescriptorIntegrityProfile profile) {
  switch (profile) {
    case DatatypeDescriptorIntegrityProfile::fast:
      return "fast";
    case DatatypeDescriptorIntegrityProfile::strong:
      return "strong";
    case DatatypeDescriptorIntegrityProfile::protected_keyed:
      return "protected_keyed";
    case DatatypeDescriptorIntegrityProfile::unknown:
      return "unknown";
  }
  return "unknown";
}

DatatypeDescriptorEnvelopeResult EncodeDatatypeDescriptorEnvelope(
    const DatatypeDescriptorEnvelope& envelope,
    const std::vector<byte>& protected_key_material) {
  const auto validation = ValidateDescriptorEnvelope(envelope, protected_key_material);
  if (!validation.ok()) {
    return validation;
  }

  std::vector<byte> records;
  for (const auto& record : envelope.records) {
    AppendU16(&records, static_cast<u16>(record.field_name.size()));
    AppendU16(&records, 0);
    AppendU64(&records, static_cast<u64>(record.payload.size()));
    AppendBytes(&records,
                reinterpret_cast<const byte*>(record.field_name.data()),
                record.field_name.size());
    AppendBytes(&records, record.payload.data(), record.payload.size());
  }

  DatatypeDescriptorEnvelopeResult result;
  result.status = BinaryOkStatus();
  result.envelope = envelope;
  result.encoded.assign(kDatatypeDescriptorEnvelopeHeaderBytes, 0);
  std::memcpy(result.encoded.data(),
              kDatatypeDescriptorEnvelopeMagic,
              sizeof(kDatatypeDescriptorEnvelopeMagic));
  StoreLittle16(result.encoded.data() + kDescriptorOffsetVersion, envelope.layout_version);
  StoreLittle16(result.encoded.data() + kDescriptorOffsetKind, static_cast<u16>(envelope.kind));
  StoreLittle16(result.encoded.data() + kDescriptorOffsetProfile,
                static_cast<u16>(envelope.integrity_profile));
  StoreLittle16(result.encoded.data() + kDescriptorOffsetFlags, 0);
  StoreLittle32(result.encoded.data() + kDescriptorOffsetHeaderBytes,
                kDatatypeDescriptorEnvelopeHeaderBytes);
  StoreLittle32(result.encoded.data() + kDescriptorOffsetRecordCount,
                static_cast<u32>(envelope.records.size()));
  StoreLittle64(result.encoded.data() + kDescriptorOffsetRecordBytes,
                static_cast<u64>(records.size()));
  result.encoded.insert(result.encoded.end(), records.begin(), records.end());

  const auto digest = ComputeDescriptorEnvelopeDigest(
      envelope.integrity_profile,
      DigestInputWithZeroedDescriptorDigest(result.encoded),
      protected_key_material);
  if (!digest.ok()) {
    return digest;
  }
  result.digest_low64 = digest.digest_low64;
  result.digest_high64 = digest.digest_high64;
  result.digest_bytes = digest.digest_bytes;
  result.digest_algorithm = digest.digest_algorithm;
  result.digest_material = digest.digest_material;
  std::copy(result.digest_material.begin(),
            result.digest_material.end(),
            result.encoded.begin() + kDescriptorOffsetDigestBytes);
  StoreLittle64(result.encoded.data() + kDescriptorOffsetDigestLow64, result.digest_low64);
  StoreLittle64(result.encoded.data() + kDescriptorOffsetDigestHigh64, result.digest_high64);
  return result;
}

DatatypeDescriptorEnvelopeResult DecodeDatatypeDescriptorEnvelope(
    const std::vector<byte>& encoded,
    const std::vector<byte>& protected_key_material) {
  if (encoded.size() < kDatatypeDescriptorEnvelopeHeaderBytes) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-ENVELOPE-SHORT",
                           "datatype.descriptor.envelope_short");
  }
  if (std::memcmp(encoded.data(),
                  kDatatypeDescriptorEnvelopeMagic,
                  sizeof(kDatatypeDescriptorEnvelopeMagic)) != 0) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-MAGIC-INVALID",
                           "datatype.descriptor.magic_invalid");
  }

  DatatypeDescriptorEnvelope envelope;
  envelope.layout_version = LoadLittle16(encoded.data() + kDescriptorOffsetVersion);
  envelope.kind = static_cast<DatatypeDescriptorEnvelopeKind>(
      LoadLittle16(encoded.data() + kDescriptorOffsetKind));
  envelope.integrity_profile = static_cast<DatatypeDescriptorIntegrityProfile>(
      LoadLittle16(encoded.data() + kDescriptorOffsetProfile));
  const u16 flags = LoadLittle16(encoded.data() + kDescriptorOffsetFlags);
  const u32 header_bytes = LoadLittle32(encoded.data() + kDescriptorOffsetHeaderBytes);
  const u32 record_count = LoadLittle32(encoded.data() + kDescriptorOffsetRecordCount);
  const u64 record_bytes = LoadLittle64(encoded.data() + kDescriptorOffsetRecordBytes);
  const u64 expected_low64 = LoadLittle64(encoded.data() + kDescriptorOffsetDigestLow64);
  const u64 expected_high64 = LoadLittle64(encoded.data() + kDescriptorOffsetDigestHigh64);
  std::vector<byte> expected_digest_material(
      encoded.begin() + kDescriptorOffsetDigestBytes,
      encoded.begin() + kDescriptorOffsetDigestBytes +
          kDatatypeDescriptorEnvelopeDigestBytes);

  if (flags != 0) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-FLAGS-RESERVED",
                           "datatype.descriptor.flags_reserved");
  }
  if (header_bytes != kDatatypeDescriptorEnvelopeHeaderBytes ||
      encoded.size() != static_cast<std::size_t>(header_bytes + record_bytes)) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-ENVELOPE-SIZE-INVALID",
                           "datatype.descriptor.envelope_size_invalid");
  }

  const auto digest = ComputeDescriptorEnvelopeDigest(
      envelope.integrity_profile,
      DigestInputWithZeroedDescriptorDigest(encoded),
      protected_key_material);
  if (!digest.ok()) {
    return digest;
  }
  if (digest.digest_low64 != expected_low64 ||
      digest.digest_high64 != expected_high64 ||
      !core_hash::ConstantTimeEqual(digest.digest_material,
                                    expected_digest_material)) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-INTEGRITY-MISMATCH",
                           "datatype.descriptor.integrity_mismatch",
                           DatatypeDescriptorIntegrityProfileName(envelope.integrity_profile));
  }

  std::size_t offset = header_bytes;
  for (u32 index = 0; index < record_count; ++index) {
    if (offset + 12 > encoded.size()) {
      return DescriptorError("SB-DATATYPE-DESCRIPTOR-RECORD-TRUNCATED",
                             "datatype.descriptor.record_truncated");
    }
    const u16 name_bytes = LoadLittle16(encoded.data() + offset);
    const u16 reserved = LoadLittle16(encoded.data() + offset + 2);
    const u64 payload_bytes = LoadLittle64(encoded.data() + offset + 4);
    offset += 12;
    if (reserved != 0) {
      return DescriptorError("SB-DATATYPE-DESCRIPTOR-RECORD-RESERVED",
                             "datatype.descriptor.record_reserved");
    }
    if (offset + name_bytes > encoded.size() ||
        payload_bytes > static_cast<u64>(encoded.size() - offset - name_bytes)) {
      return DescriptorError("SB-DATATYPE-DESCRIPTOR-RECORD-TRUNCATED",
                             "datatype.descriptor.record_truncated");
    }
    DatatypeDescriptorRecord record;
    record.field_name.assign(reinterpret_cast<const char*>(encoded.data() + offset),
                             name_bytes);
    offset += name_bytes;
    record.payload.assign(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                          encoded.begin() + static_cast<std::ptrdiff_t>(offset + payload_bytes));
    offset += static_cast<std::size_t>(payload_bytes);
    envelope.records.push_back(std::move(record));
  }
  if (offset != encoded.size()) {
    return DescriptorError("SB-DATATYPE-DESCRIPTOR-TRAILING-BYTES",
                           "datatype.descriptor.trailing_bytes");
  }

  const auto validation = ValidateDescriptorEnvelope(envelope, protected_key_material);
  if (!validation.ok()) {
    return validation;
  }

  DatatypeDescriptorEnvelopeResult result;
  result.status = BinaryOkStatus();
  result.envelope = std::move(envelope);
  result.encoded = encoded;
  result.digest_low64 = digest.digest_low64;
  result.digest_high64 = digest.digest_high64;
  result.digest_bytes = digest.digest_bytes;
  result.digest_algorithm = digest.digest_algorithm;
  result.digest_material = digest.digest_material;
  return result;
}

DatatypeBinaryResult ValidateDatatypeBinaryValue(const DatatypeBinaryValue& value) {
  const auto layout = LookupDatatypeStorageLayout(value.type_id);
  if (!layout.ok()) {
    DatatypeBinaryResult result;
    result.status = layout.status;
    result.diagnostic = layout.diagnostic;
    return result;
  }

  if (value.is_null) {
    if (!value.payload.empty()) {
      return BinaryError("SB-DATATYPE-BINARY-NULL-HAS-PAYLOAD",
                         "datatype.binary.null_has_payload",
                         CanonicalTypeName(value.type_id));
    }
    DatatypeBinaryResult result;
    result.status = BinaryOkStatus();
    result.value = value;
    return result;
  }

  if (value.type_id == CanonicalTypeId::null_type) {
    return BinaryError("SB-DATATYPE-BINARY-NULL-TYPE-MUST-BE-NULL",
                       "datatype.binary.null_type_must_be_null");
  }

  if (!IsValidFixedPayloadSize(layout.layout, static_cast<u32>(value.payload.size()))) {
    return BinaryError("SB-DATATYPE-BINARY-PAYLOAD-SIZE-INVALID",
                       "datatype.binary.payload_size_invalid",
                       CanonicalTypeName(value.type_id));
  }

  if (value.type_id == CanonicalTypeId::boolean && value.payload.size() == 1 &&
      value.payload[0] != 0 && value.payload[0] != 1) {
    return BinaryError("SB-DATATYPE-BINARY-BOOLEAN-PAYLOAD-INVALID",
                       "datatype.binary.boolean_payload_invalid");
  }

  if (value.payload_is_toast_reference && !layout.layout.may_overflow_to_toast) {
    return BinaryError("SB-DATATYPE-BINARY-TOAST-NOT-ALLOWED",
                       "datatype.binary.toast_not_allowed",
                       CanonicalTypeName(value.type_id));
  }

  DatatypeBinaryResult result;
  result.status = BinaryOkStatus();
  result.value = value;
  return result;
}

DatatypeBinaryResult EncodeDatatypeBinaryValue(const DatatypeBinaryValue& value) {
  const auto validation = ValidateDatatypeBinaryValue(value);
  if (!validation.ok()) {
    return validation;
  }

  DatatypeBinaryResult result;
  result.status = BinaryOkStatus();
  result.value = value;
  result.encoded.assign(kDatatypeBinaryEnvelopeHeaderBytes + value.payload.size(), 0);
  std::memcpy(result.encoded.data() + kOffsetMagic, kDatatypeBinaryMagic, sizeof(kDatatypeBinaryMagic));
  StoreLittle32(result.encoded.data() + kOffsetTypeId, static_cast<u32>(value.type_id));
  StoreLittle16(result.encoded.data() + kOffsetFlags, FlagsFor(value));
  StoreLittle16(result.encoded.data() + kOffsetHeaderBytes, kDatatypeBinaryEnvelopeHeaderBytes);
  StoreLittle32(result.encoded.data() + kOffsetPayloadBytes, static_cast<u32>(value.payload.size()));
  StoreLittle64(result.encoded.data() + kOffsetPayloadChecksum, ComputeDatatypeBinaryChecksum(value.payload));
  if (!value.payload.empty()) {
    std::copy(value.payload.begin(), value.payload.end(), result.encoded.begin() + kDatatypeBinaryEnvelopeHeaderBytes);
  }
  return result;
}

DatatypeBinaryResult DecodeDatatypeBinaryValue(const std::vector<byte>& encoded) {
  if (encoded.size() < kDatatypeBinaryEnvelopeHeaderBytes) {
    return BinaryError("SB-DATATYPE-BINARY-ENVELOPE-SHORT",
                       "datatype.binary.envelope_short");
  }
  if (std::memcmp(encoded.data() + kOffsetMagic, kDatatypeBinaryMagic, sizeof(kDatatypeBinaryMagic)) != 0) {
    return BinaryError("SB-DATATYPE-BINARY-MAGIC-INVALID",
                       "datatype.binary.magic_invalid");
  }
  const u16 header_bytes = LoadLittle16(encoded.data() + kOffsetHeaderBytes);
  const u32 payload_bytes = LoadLittle32(encoded.data() + kOffsetPayloadBytes);
  if (header_bytes != kDatatypeBinaryEnvelopeHeaderBytes ||
      encoded.size() != static_cast<std::size_t>(header_bytes) + payload_bytes) {
    return BinaryError("SB-DATATYPE-BINARY-ENVELOPE-SIZE-INVALID",
                       "datatype.binary.envelope_size_invalid");
  }

  DatatypeBinaryValue value;
  value.type_id = static_cast<CanonicalTypeId>(LoadLittle32(encoded.data() + kOffsetTypeId));
  const u16 flags = LoadLittle16(encoded.data() + kOffsetFlags);
  value.is_null = (flags & BinaryFlag::is_null) != 0;
  value.payload_is_toast_reference = (flags & BinaryFlag::payload_is_toast_reference) != 0;
  value.payload.assign(encoded.begin() + header_bytes, encoded.end());
  const u64 expected_checksum = LoadLittle64(encoded.data() + kOffsetPayloadChecksum);
  if (expected_checksum != ComputeDatatypeBinaryChecksum(value.payload)) {
    return BinaryError("SB-DATATYPE-BINARY-PAYLOAD-CHECKSUM-MISMATCH",
                       "datatype.binary.payload_checksum_mismatch",
                       CanonicalTypeName(value.type_id));
  }

  DatatypeBinaryResult validation = ValidateDatatypeBinaryValue(value);
  if (!validation.ok()) {
    return validation;
  }
  validation.encoded = encoded;
  return validation;
}

DiagnosticRecord MakeDatatypeBinaryDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.binary");
}

}  // namespace scratchbird::core::datatypes
