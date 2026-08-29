// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_plan_import_rows_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view kPlanDescriptorDomain =
    "ScratchBird.SblrImportRowsPlanDescriptor.V1";
constexpr std::string_view kSourceEnvelopeDomain =
    "ScratchBird.SblrImportSourceEnvelope.V1";
constexpr std::string_view kFormatEnvelopeDomain =
    "ScratchBird.SblrImportFormatEnvelope.V1";
constexpr std::string_view kMappingVectorDomain =
    "ScratchBird.SblrImportColumnMappingVector.V1";
constexpr std::string_view kPolicyDomain =
    "ScratchBird.SblrImportPolicy.V1";
constexpr std::string_view kExecutorEvidenceDomain =
    "ScratchBird.SblrDmlPlanImportRowsExecutorEvidence.V1";

bool Fail(PlanImportRowsCodecDiagnosticV1* diagnostic,
          std::string code,
          std::string detail) {
  if (diagnostic != nullptr) {
    diagnostic->code = std::move(code);
    diagnostic->detail = std::move(detail);
  }
  return false;
}

void Clear(PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (diagnostic != nullptr) *diagnostic = {};
}

template <std::size_t N>
bool Nonzero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool AllZero(const std::uint8_t* bytes, std::size_t size) {
  return std::all_of(bytes, bytes + size,
                     [](std::uint8_t byte) { return byte == 0; });
}

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t* out) {
  if (out == nullptr || right > std::numeric_limits<std::size_t>::max() - left)
    return false;
  *out = left + right;
  return true;
}

bool CheckedMultiply(std::size_t left,
                     std::size_t right,
                     std::size_t* out) {
  if (out == nullptr ||
      (left != 0 && right > std::numeric_limits<std::size_t>::max() / left))
    return false;
  *out = left * right;
  return true;
}

std::uint16_t GetU16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t GetU32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i != 4; ++i)
    value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
  return value;
}

std::uint64_t GetU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  return value;
}

void PutU16(Bytes* bytes, std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void PutU32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void PutU64(Bytes* bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

template <std::size_t N>
void GetArray(const std::uint8_t* bytes,
              std::array<std::uint8_t, N>* out) {
  std::copy_n(bytes, N, out->begin());
}

template <std::size_t N>
void PutArray(Bytes* bytes,
              std::size_t offset,
              const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

void PutMagic(Bytes* bytes, const char (&magic)[5]) {
  std::copy_n(reinterpret_cast<const std::uint8_t*>(magic), 4,
              bytes->begin());
}

bool ExactMagic(const std::uint8_t* bytes, const char (&magic)[5]) {
  return std::equal(bytes, bytes + 4,
                    reinterpret_cast<const std::uint8_t*>(magic));
}

bool ComputeHash(std::string_view domain,
                 const std::uint8_t* first,
                 std::size_t first_size,
                 const std::uint8_t* second,
                 std::size_t second_size,
                 PlanImportRowsSha256V1* out) {
  std::size_t material_size = 0;
  if (out == nullptr ||
      !CheckedAdd(domain.size(), first_size, &material_size) ||
      !CheckedAdd(material_size, second_size, &material_size)) {
    return false;
  }
  Bytes material;
  material.reserve(material_size);
  material.insert(material.end(), domain.begin(), domain.end());
  if (first_size != 0) material.insert(material.end(), first, first + first_size);
  if (second_size != 0)
    material.insert(material.end(), second, second + second_size);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  if (!digest.ok()) return false;
  *out = digest.digest;
  return true;
}

bool ValidSourceKind(PlanImportRowsSourceKindV1 value) {
  const auto code = static_cast<std::uint16_t>(value);
  return code >= 1 && code <= 13;
}

bool ValidFormatFamily(PlanImportRowsFormatFamilyV1 value) {
  const auto code = static_cast<std::uint16_t>(value);
  return code >= 1 && code <= 12;
}

bool ValidRejectMode(PlanImportRowsRejectModeV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 4;
}

bool ValidRejectPayloadPolicy(PlanImportRowsRejectPayloadPolicyV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 3;
}

bool ValidResumePolicy(PlanImportRowsResumePolicyV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 3;
}

bool ValidChildRef(const PlanImportRowsChildRefV1& reference) {
  return Nonzero(reference.row_uuid) && reference.row_generation != 0;
}

bool ValidateNestedIdentity(const PlanImportRowsNestedHeaderV1& header,
                            PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!Nonzero(header.row_uuid) || header.row_generation == 0 ||
      !Nonzero(header.owner_plan_descriptor_uuid) ||
      header.owner_plan_descriptor_generation == 0) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child identity or ownership is zero");
  }
  return true;
}

bool EncodeNested(const PlanImportRowsNestedHeaderV1& header,
                  const char (&magic)[5],
                  std::string_view domain,
                  const Bytes& payload,
                  Bytes* out,
                  PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child output is null");
  }
  out->clear();
  if (!ValidateNestedIdentity(header, diagnostic)) return false;
  std::size_t total_bytes = 0;
  if (!CheckedAdd(kPlanImportRowsNestedHeaderBytesV1, payload.size(),
                  &total_bytes) ||
      total_bytes > std::numeric_limits<std::uint32_t>::max() ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child extent overflows v1 fields");
  }

  Bytes encoded(total_bytes, 0);
  PutMagic(&encoded, magic);
  PutU16(&encoded, 4, 1);
  PutU16(&encoded, 6,
         static_cast<std::uint16_t>(kPlanImportRowsNestedHeaderBytesV1));
  PutU32(&encoded, 8, static_cast<std::uint32_t>(total_bytes));
  PutArray(&encoded, 16, header.row_uuid);
  PutU64(&encoded, 32, header.row_generation);
  PutArray(&encoded, 40, header.owner_plan_descriptor_uuid);
  PutU64(&encoded, 56, header.owner_plan_descriptor_generation);
  PutU32(&encoded, 64, static_cast<std::uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(),
            encoded.begin() + kPlanImportRowsNestedHeaderBytesV1);

  PlanImportRowsSha256V1 digest{};
  if (!ComputeHash(domain, encoded.data(), 72,
                   encoded.data() + kPlanImportRowsNestedHeaderBytesV1,
                   payload.size(), &digest)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child SHA-256 is unavailable");
  }
  PutArray(&encoded, 72, digest);
  *out = std::move(encoded);
  return true;
}

bool DecodeNested(const std::uint8_t* bytes,
                  std::size_t size,
                  const char (&magic)[5],
                  std::string_view domain,
                  std::size_t expected_payload_bytes,
                  PlanImportRowsNestedHeaderV1* header,
                  const std::uint8_t** payload,
                  std::size_t* payload_bytes,
                  PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (bytes == nullptr || header == nullptr || payload == nullptr ||
      payload_bytes == nullptr || size < kPlanImportRowsNestedHeaderBytesV1 ||
      size > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child extent is invalid");
  }
  const std::uint32_t encoded_payload_bytes = GetU32(bytes + 64);
  std::size_t expected_total = 0;
  if (!ExactMagic(bytes, magic) || GetU16(bytes + 4) != 1 ||
      GetU16(bytes + 6) != kPlanImportRowsNestedHeaderBytesV1 ||
      GetU32(bytes + 8) != size || GetU32(bytes + 12) != 0 ||
      !CheckedAdd(kPlanImportRowsNestedHeaderBytesV1,
                  encoded_payload_bytes, &expected_total) ||
      expected_total != size || !AllZero(bytes + 68, 4) ||
      (expected_payload_bytes != std::numeric_limits<std::size_t>::max() &&
       encoded_payload_bytes != expected_payload_bytes)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child header or extent is invalid");
  }

  PlanImportRowsSha256V1 expected_digest{};
  if (!ComputeHash(domain, bytes, 72,
                   bytes + kPlanImportRowsNestedHeaderBytesV1,
                   encoded_payload_bytes, &expected_digest) ||
      !std::equal(expected_digest.begin(), expected_digest.end(), bytes + 72)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import child SHA-256 is invalid");
  }

  PlanImportRowsNestedHeaderV1 decoded;
  GetArray(bytes + 16, &decoded.row_uuid);
  decoded.row_generation = GetU64(bytes + 32);
  GetArray(bytes + 40, &decoded.owner_plan_descriptor_uuid);
  decoded.owner_plan_descriptor_generation = GetU64(bytes + 56);
  GetArray(bytes + 72, &decoded.row_sha256);
  if (!ValidateNestedIdentity(decoded, diagnostic)) return false;
  *header = decoded;
  *payload = bytes + kPlanImportRowsNestedHeaderBytesV1;
  *payload_bytes = encoded_payload_bytes;
  return true;
}

void PutChildRef(Bytes* bytes,
                 std::size_t offset,
                 const PlanImportRowsChildRefV1& reference) {
  PutArray(bytes, offset, reference.row_uuid);
  PutU64(bytes, offset + 16, reference.row_generation);
  PutArray(bytes, offset + 24, reference.canonical_row_sha256);
}

void GetChildRef(const std::uint8_t* bytes,
                 PlanImportRowsChildRefV1* reference) {
  GetArray(bytes, &reference->row_uuid);
  reference->row_generation = GetU64(bytes + 16);
  GetArray(bytes + 24, &reference->canonical_row_sha256);
}

bool ValidatePlanDescriptorFields(
    const PlanImportRowsPlanDescriptorV1& value,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!Nonzero(value.descriptor_uuid) || value.descriptor_generation == 0 ||
      !Nonzero(value.request_uuid) ||
      !Nonzero(value.authenticated_statement_receipt_uuid) ||
      value.structural_occurrence_id == 0 || value.local_transaction_id == 0 ||
      !Nonzero(value.transaction_uuid) || !Nonzero(value.mga_snapshot_uuid) ||
      value.mga_snapshot_generation == 0 ||
      !Nonzero(value.security_snapshot_uuid) ||
      value.security_snapshot_generation == 0 ||
      !Nonzero(value.policy_snapshot_uuid) ||
      value.policy_snapshot_generation == 0 ||
      !Nonzero(value.resource_admission_uuid) ||
      value.resource_admission_generation == 0 || value.catalog_generation == 0 ||
      !Nonzero(value.target_table_uuid) ||
      !Nonzero(value.target_relation_descriptor_uuid) ||
      value.target_relation_descriptor_generation == 0 ||
      !ValidChildRef(value.import_source_envelope_ref) ||
      !ValidChildRef(value.import_format_envelope_ref) ||
      !ValidChildRef(value.column_mapping_vector_ref) ||
      !ValidChildRef(value.import_policy_ref) ||
      value.executor_availability_generation == 0) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPLP contains a zero required identity or generation");
  }
  return true;
}

bool ValidateSourceFields(const PlanImportRowsSourceEnvelopeV1& value,
                          PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!ValidateNestedIdentity(value.header, diagnostic)) return false;
  if (!ValidSourceKind(value.source_kind) ||
      (value.flags & ~kPlanImportRowsSourceFingerprintPresentV1) != 0 ||
      !Nonzero(value.engine_source_binding_uuid) ||
      value.engine_source_binding_generation == 0) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "ISRC contains an invalid source, flag, or binding");
  }
  if ((value.flags & kPlanImportRowsSourceFingerprintPresentV1) == 0 &&
      Nonzero(value.source_fingerprint_sha256)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "ISRC absent fingerprint is not exactly zero");
  }
  if ((value.flags & kPlanImportRowsSourceFingerprintPresentV1) != 0 &&
      !Nonzero(value.source_fingerprint_sha256)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "ISRC present fingerprint is exactly zero");
  }
  return true;
}

bool ValidateFormatFields(const PlanImportRowsFormatEnvelopeV1& value,
                          PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!ValidateNestedIdentity(value.header, diagnostic) ||
      !ValidFormatFamily(value.format_family)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IFMT contains an invalid format family");
  }
  return true;
}

bool ValidateMappingFields(const PlanImportRowsMappingVectorV1& value,
                           PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!ValidateNestedIdentity(value.header, diagnostic)) return false;
  if (value.mappings.size() > kPlanImportRowsMaximumMappingsV1) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IMAP mapping count exceeds the v1 bound");
  }
  std::set<PlanImportRowsUuidV1> target_columns;
  std::uint32_t previous_source_ordinal = 0;
  for (std::size_t index = 0; index != value.mappings.size(); ++index) {
    const auto& mapping = value.mappings[index];
    if ((index == 0 && mapping.source_field_ordinal != 0) ||
        (index != 0 && mapping.source_field_ordinal <= previous_source_ordinal) ||
        (mapping.flags & ~kPlanImportRowsMappingRequiredV1) != 0 ||
        !Nonzero(mapping.target_column_uuid) ||
        mapping.target_column_generation == 0 ||
        !Nonzero(mapping.target_datatype_descriptor_uuid) ||
        mapping.target_datatype_descriptor_generation == 0 ||
        !Nonzero(mapping.target_type_uuid) || mapping.target_type_generation == 0 ||
        mapping.codec_id == 0 || mapping.codec_version == 0 ||
        mapping.codec_generation == 0 ||
        !target_columns.insert(mapping.target_column_uuid).second) {
      return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                  "IMAP contains an invalid, unordered, or duplicate mapping");
    }
    previous_source_ordinal = mapping.source_field_ordinal;
  }
  return true;
}

bool ValidatePolicyFields(const PlanImportRowsPolicyV1& value,
                          PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!ValidateNestedIdentity(value.header, diagnostic)) return false;
  if (!ValidRejectMode(value.reject_mode) ||
      !ValidRejectPayloadPolicy(value.reject_payload_policy) ||
      !ValidResumePolicy(value.resume_policy) || (value.flags & ~0x03U) != 0 ||
      value.reject_limit_ppm > 1000000U) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPOL contains an invalid enum, flag, or ppm limit");
  }

  const bool target_present = Nonzero(value.reject_target_relation_uuid);
  if ((!target_present &&
       (value.reject_target_relation_generation != 0 ||
        Nonzero(value.reject_target_relation_sha256))) ||
      (target_present &&
       (value.reject_target_relation_generation == 0 ||
        !Nonzero(value.reject_target_relation_sha256)))) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPOL reject-target tuple is incomplete");
  }

  const bool has_limit =
      value.reject_limit_rows != 0 || value.reject_limit_ppm != 0;
  switch (value.reject_mode) {
    case PlanImportRowsRejectModeV1::fail_fast:
      if (value.reject_payload_policy !=
              PlanImportRowsRejectPayloadPolicyV1::diagnostic_only ||
          has_limit || target_present) {
        return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                    "IPOL fail_fast profile is inconsistent");
      }
      break;
    case PlanImportRowsRejectModeV1::reject_row:
      if (!has_limit) {
        return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                    "IPOL reject_row requires a nonzero limit");
      }
      break;
    case PlanImportRowsRejectModeV1::reject_table:
      if (!has_limit || !target_present) {
        return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                    "IPOL reject_table requires a limit and target");
      }
      break;
    case PlanImportRowsRejectModeV1::quarantine:
      if (!target_present ||
          value.reject_payload_policy ==
              PlanImportRowsRejectPayloadPolicyV1::diagnostic_only) {
        return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                    "IPOL quarantine requires a payload reference and target");
      }
      break;
  }
  return true;
}

bool ValidateEvidenceFields(const PlanImportRowsExecutorEvidenceV1& value,
                            PlanImportRowsCodecDiagnosticV1* diagnostic) {
  if (!Nonzero(value.evidence_uuid) || value.evidence_generation == 0 ||
      !Nonzero(value.request_descriptor_uuid) ||
      value.request_descriptor_generation == 0 ||
      !Nonzero(value.request_projection_sha256) ||
      value.executor_availability_generation == 0 ||
      !Nonzero(value.transaction_uuid) || value.local_transaction_id == 0 ||
      !Nonzero(value.mga_snapshot_uuid) || value.mga_snapshot_generation == 0 ||
      value.completed_validation_bits !=
          kPlanImportRowsAcceptedValidationBitsV1) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPEV contains an invalid identity, generation, or gate mask");
  }
  return true;
}

template <typename Carrier>
bool ChildMatchesReference(const Carrier& carrier,
                           const PlanImportRowsChildRefV1& reference) {
  return carrier.header.row_uuid == reference.row_uuid &&
         carrier.header.row_generation == reference.row_generation &&
         carrier.header.row_sha256 == reference.canonical_row_sha256;
}

template <typename Carrier>
bool ChildOwnedByDescriptor(
    const Carrier& carrier,
    const PlanImportRowsPlanDescriptorV1& descriptor) {
  return carrier.header.owner_plan_descriptor_uuid == descriptor.descriptor_uuid &&
         carrier.header.owner_plan_descriptor_generation ==
             descriptor.descriptor_generation;
}

}  // namespace

bool EncodePlanImportRowsDescriptorRefV1(
    const PlanImportRowsDescriptorRefV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import descriptor-ref output is null");
  }
  out->clear();
  if (!Nonzero(value.descriptor_uuid) || value.descriptor_generation == 0) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import descriptor-ref identity is zero");
  }
  out->assign(kPlanImportRowsDescriptorRefBytesV1, 0);
  PutArray(out, 0, value.descriptor_uuid);
  PutU64(out, 16, value.descriptor_generation);
  return true;
}

bool DecodePlanImportRowsDescriptorRefV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsDescriptorRefV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (bytes == nullptr || out == nullptr ||
      size != kPlanImportRowsDescriptorRefBytesV1) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import descriptor-ref extent is not exactly 24 bytes");
  }
  PlanImportRowsDescriptorRefV1 decoded;
  GetArray(bytes, &decoded.descriptor_uuid);
  decoded.descriptor_generation = GetU64(bytes + 16);
  if (!Nonzero(decoded.descriptor_uuid) || decoded.descriptor_generation == 0) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import descriptor-ref identity is zero");
  }
  *out = decoded;
  return true;
}

bool EncodePlanImportRowsPlanDescriptorV1(
    const PlanImportRowsPlanDescriptorV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPLP output is null");
  }
  out->clear();
  if (!ValidatePlanDescriptorFields(value, diagnostic)) return false;

  Bytes encoded(kPlanImportRowsPlanDescriptorBytesV1, 0);
  PutMagic(&encoded, "IPLP");
  PutU16(&encoded, 4, 1);
  PutU16(&encoded, 6,
         static_cast<std::uint16_t>(kPlanImportRowsPlanDescriptorBytesV1));
  PutU32(&encoded, 8,
         static_cast<std::uint32_t>(kPlanImportRowsPlanDescriptorBytesV1));
  PutArray(&encoded, 16, value.descriptor_uuid);
  PutU64(&encoded, 32, value.descriptor_generation);
  PutArray(&encoded, 40, value.request_uuid);
  PutArray(&encoded, 56, value.authenticated_statement_receipt_uuid);
  PutU64(&encoded, 72, value.structural_occurrence_id);
  PutU64(&encoded, 80, value.local_transaction_id);
  PutArray(&encoded, 88, value.transaction_uuid);
  PutArray(&encoded, 104, value.mga_snapshot_uuid);
  PutU64(&encoded, 120, value.mga_snapshot_generation);
  PutArray(&encoded, 128, value.security_snapshot_uuid);
  PutU64(&encoded, 144, value.security_snapshot_generation);
  PutArray(&encoded, 152, value.policy_snapshot_uuid);
  PutU64(&encoded, 168, value.policy_snapshot_generation);
  PutArray(&encoded, 176, value.resource_admission_uuid);
  PutU64(&encoded, 192, value.resource_admission_generation);
  PutU64(&encoded, 200, value.catalog_generation);
  PutArray(&encoded, 208, value.target_table_uuid);
  PutArray(&encoded, 224, value.target_relation_descriptor_uuid);
  PutU64(&encoded, 240, value.target_relation_descriptor_generation);
  PutChildRef(&encoded, 248, value.import_source_envelope_ref);
  PutChildRef(&encoded, 304, value.import_format_envelope_ref);
  PutChildRef(&encoded, 360, value.column_mapping_vector_ref);
  PutChildRef(&encoded, 416, value.import_policy_ref);
  PutU64(&encoded, 472, value.executor_availability_generation);

  PlanImportRowsSha256V1 digest{};
  if (!ComputeHash(kPlanDescriptorDomain, encoded.data(), 480, nullptr, 0,
                   &digest)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPLP SHA-256 is unavailable");
  }
  PutArray(&encoded, 480, digest);
  *out = std::move(encoded);
  return true;
}

bool DecodePlanImportRowsPlanDescriptorV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsPlanDescriptorV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (bytes == nullptr || out == nullptr ||
      size != kPlanImportRowsPlanDescriptorBytesV1 ||
      !ExactMagic(bytes, "IPLP") || GetU16(bytes + 4) != 1 ||
      GetU16(bytes + 6) != kPlanImportRowsPlanDescriptorBytesV1 ||
      GetU32(bytes + 8) != kPlanImportRowsPlanDescriptorBytesV1 ||
      GetU32(bytes + 12) != 0) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPLP header or exact extent is invalid");
  }
  PlanImportRowsSha256V1 expected_digest{};
  if (!ComputeHash(kPlanDescriptorDomain, bytes, 480, nullptr, 0,
                   &expected_digest) ||
      !std::equal(expected_digest.begin(), expected_digest.end(), bytes + 480)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPLP descriptor evidence SHA-256 is invalid");
  }

  PlanImportRowsPlanDescriptorV1 decoded;
  GetArray(bytes + 16, &decoded.descriptor_uuid);
  decoded.descriptor_generation = GetU64(bytes + 32);
  GetArray(bytes + 40, &decoded.request_uuid);
  GetArray(bytes + 56, &decoded.authenticated_statement_receipt_uuid);
  decoded.structural_occurrence_id = GetU64(bytes + 72);
  decoded.local_transaction_id = GetU64(bytes + 80);
  GetArray(bytes + 88, &decoded.transaction_uuid);
  GetArray(bytes + 104, &decoded.mga_snapshot_uuid);
  decoded.mga_snapshot_generation = GetU64(bytes + 120);
  GetArray(bytes + 128, &decoded.security_snapshot_uuid);
  decoded.security_snapshot_generation = GetU64(bytes + 144);
  GetArray(bytes + 152, &decoded.policy_snapshot_uuid);
  decoded.policy_snapshot_generation = GetU64(bytes + 168);
  GetArray(bytes + 176, &decoded.resource_admission_uuid);
  decoded.resource_admission_generation = GetU64(bytes + 192);
  decoded.catalog_generation = GetU64(bytes + 200);
  GetArray(bytes + 208, &decoded.target_table_uuid);
  GetArray(bytes + 224, &decoded.target_relation_descriptor_uuid);
  decoded.target_relation_descriptor_generation = GetU64(bytes + 240);
  GetChildRef(bytes + 248, &decoded.import_source_envelope_ref);
  GetChildRef(bytes + 304, &decoded.import_format_envelope_ref);
  GetChildRef(bytes + 360, &decoded.column_mapping_vector_ref);
  GetChildRef(bytes + 416, &decoded.import_policy_ref);
  decoded.executor_availability_generation = GetU64(bytes + 472);
  GetArray(bytes + 480, &decoded.descriptor_evidence_sha256);
  if (!ValidatePlanDescriptorFields(decoded, diagnostic)) return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  *out = std::move(decoded);
  return true;
}

bool EncodePlanImportRowsSourceEnvelopeV1(
    const PlanImportRowsSourceEnvelopeV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidateSourceFields(value, diagnostic)) {
    if (out != nullptr) out->clear();
    return false;
  }
  Bytes payload(kPlanImportRowsSourcePayloadBytesV1, 0);
  PutU16(&payload, 0, static_cast<std::uint16_t>(value.source_kind));
  PutU16(&payload, 2, value.flags);
  PutArray(&payload, 8, value.engine_source_binding_uuid);
  PutU64(&payload, 24, value.engine_source_binding_generation);
  PutArray(&payload, 32, value.source_fingerprint_sha256);
  return EncodeNested(value.header, "ISRC", kSourceEnvelopeDomain, payload,
                      out, diagnostic);
}

bool DecodePlanImportRowsSourceEnvelopeV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsSourceEnvelopeV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID", "ISRC output is null");
  }
  PlanImportRowsSourceEnvelopeV1 decoded;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_bytes = 0;
  if (!DecodeNested(bytes, size, "ISRC", kSourceEnvelopeDomain,
                    kPlanImportRowsSourcePayloadBytesV1, &decoded.header,
                    &payload, &payload_bytes, diagnostic)) {
    return false;
  }
  if (!AllZero(payload + 4, 4)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "ISRC reserved bytes are nonzero");
  }
  decoded.source_kind =
      static_cast<PlanImportRowsSourceKindV1>(GetU16(payload));
  decoded.flags = GetU16(payload + 2);
  GetArray(payload + 8, &decoded.engine_source_binding_uuid);
  decoded.engine_source_binding_generation = GetU64(payload + 24);
  GetArray(payload + 32, &decoded.source_fingerprint_sha256);
  if (!ValidateSourceFields(decoded, diagnostic)) return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  *out = std::move(decoded);
  return true;
}

bool EncodePlanImportRowsFormatEnvelopeV1(
    const PlanImportRowsFormatEnvelopeV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidateFormatFields(value, diagnostic)) {
    if (out != nullptr) out->clear();
    return false;
  }
  Bytes payload(kPlanImportRowsFormatPayloadBytesV1, 0);
  PutU16(&payload, 0, static_cast<std::uint16_t>(value.format_family));
  return EncodeNested(value.header, "IFMT", kFormatEnvelopeDomain, payload,
                      out, diagnostic);
}

bool DecodePlanImportRowsFormatEnvelopeV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsFormatEnvelopeV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID", "IFMT output is null");
  }
  PlanImportRowsFormatEnvelopeV1 decoded;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_bytes = 0;
  if (!DecodeNested(bytes, size, "IFMT", kFormatEnvelopeDomain,
                    kPlanImportRowsFormatPayloadBytesV1, &decoded.header,
                    &payload, &payload_bytes, diagnostic)) {
    return false;
  }
  if (!AllZero(payload + 2, 6)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IFMT reserved bytes are nonzero");
  }
  decoded.format_family =
      static_cast<PlanImportRowsFormatFamilyV1>(GetU16(payload));
  if (!ValidateFormatFields(decoded, diagnostic)) return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  *out = std::move(decoded);
  return true;
}

bool EncodePlanImportRowsMappingVectorV1(
    const PlanImportRowsMappingVectorV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidateMappingFields(value, diagnostic)) {
    if (out != nullptr) out->clear();
    return false;
  }
  std::size_t record_bytes = 0;
  std::size_t payload_bytes = 0;
  if (!CheckedMultiply(value.mappings.size(),
                       kPlanImportRowsMappingRecordBytesV1, &record_bytes) ||
      !CheckedAdd(8, record_bytes, &payload_bytes) ||
      payload_bytes > std::numeric_limits<std::uint32_t>::max()) {
    if (out != nullptr) out->clear();
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IMAP extent overflows v1 fields");
  }

  Bytes payload(payload_bytes, 0);
  PutU32(&payload, 0, static_cast<std::uint32_t>(value.mappings.size()));
  PutU32(&payload, 4,
         static_cast<std::uint32_t>(kPlanImportRowsMappingRecordBytesV1));
  for (std::size_t index = 0; index != value.mappings.size(); ++index) {
    const auto& mapping = value.mappings[index];
    const std::size_t offset = 8 + index * kPlanImportRowsMappingRecordBytesV1;
    PutU32(&payload, offset, mapping.source_field_ordinal);
    PutU32(&payload, offset + 4, mapping.flags);
    PutArray(&payload, offset + 8, mapping.target_column_uuid);
    PutU64(&payload, offset + 24, mapping.target_column_generation);
    PutArray(&payload, offset + 32,
             mapping.target_datatype_descriptor_uuid);
    PutU64(&payload, offset + 48,
           mapping.target_datatype_descriptor_generation);
    PutArray(&payload, offset + 56, mapping.target_type_uuid);
    PutU64(&payload, offset + 72, mapping.target_type_generation);
    PutU16(&payload, offset + 80, mapping.codec_id);
    PutU16(&payload, offset + 82, mapping.codec_version);
    PutU64(&payload, offset + 88, mapping.codec_generation);
  }
  return EncodeNested(value.header, "IMAP", kMappingVectorDomain, payload,
                      out, diagnostic);
}

bool DecodePlanImportRowsMappingVectorV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsMappingVectorV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID", "IMAP output is null");
  }
  constexpr std::size_t kMappingPrefixBytes = 8;
  std::size_t minimum_bytes = 0;
  if (bytes == nullptr ||
      !CheckedAdd(kPlanImportRowsNestedHeaderBytesV1, kMappingPrefixBytes,
                  &minimum_bytes) ||
      size < minimum_bytes ||
      size > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IMAP payload prefix is truncated");
  }
  const std::uint32_t encoded_payload_bytes = GetU32(bytes + 64);
  const std::uint32_t mapping_count =
      GetU32(bytes + kPlanImportRowsNestedHeaderBytesV1);
  const std::uint32_t record_bytes_field =
      GetU32(bytes + kPlanImportRowsNestedHeaderBytesV1 + 4);
  std::size_t records_extent = 0;
  std::size_t expected_payload = 0;
  std::size_t expected_total = 0;
  if (mapping_count > kPlanImportRowsMaximumMappingsV1 ||
      record_bytes_field != kPlanImportRowsMappingRecordBytesV1 ||
      !CheckedMultiply(mapping_count, kPlanImportRowsMappingRecordBytesV1,
                       &records_extent) ||
      !CheckedAdd(kMappingPrefixBytes, records_extent, &expected_payload) ||
      !CheckedAdd(kPlanImportRowsNestedHeaderBytesV1, expected_payload,
                  &expected_total) ||
      expected_payload != encoded_payload_bytes || expected_total != size) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IMAP count, record size, or payload extent is invalid");
  }

  PlanImportRowsMappingVectorV1 decoded;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_bytes = 0;
  if (!DecodeNested(bytes, size, "IMAP", kMappingVectorDomain,
                    std::numeric_limits<std::size_t>::max(), &decoded.header,
                    &payload, &payload_bytes, diagnostic)) {
    return false;
  }
  decoded.mappings.reserve(mapping_count);
  for (std::size_t index = 0; index != mapping_count; ++index) {
    const std::size_t offset = 8 + index * kPlanImportRowsMappingRecordBytesV1;
    PlanImportRowsMappingRecordV1 mapping;
    mapping.source_field_ordinal = GetU32(payload + offset);
    mapping.flags = GetU32(payload + offset + 4);
    GetArray(payload + offset + 8, &mapping.target_column_uuid);
    mapping.target_column_generation = GetU64(payload + offset + 24);
    GetArray(payload + offset + 32,
             &mapping.target_datatype_descriptor_uuid);
    mapping.target_datatype_descriptor_generation =
        GetU64(payload + offset + 48);
    GetArray(payload + offset + 56, &mapping.target_type_uuid);
    mapping.target_type_generation = GetU64(payload + offset + 72);
    mapping.codec_id = GetU16(payload + offset + 80);
    mapping.codec_version = GetU16(payload + offset + 82);
    if (!AllZero(payload + offset + 84, 4)) {
      return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                  "IMAP record reserved bytes are nonzero");
    }
    mapping.codec_generation = GetU64(payload + offset + 88);
    decoded.mappings.push_back(std::move(mapping));
  }
  if (!ValidateMappingFields(decoded, diagnostic)) return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  *out = std::move(decoded);
  return true;
}

bool EncodePlanImportRowsPolicyV1(
    const PlanImportRowsPolicyV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidatePolicyFields(value, diagnostic)) {
    if (out != nullptr) out->clear();
    return false;
  }
  Bytes payload(kPlanImportRowsPolicyPayloadBytesV1, 0);
  payload[0] = static_cast<std::uint8_t>(value.reject_mode);
  payload[1] = static_cast<std::uint8_t>(value.reject_payload_policy);
  payload[2] = static_cast<std::uint8_t>(value.resume_policy);
  payload[3] = value.flags;
  PutU32(&payload, 4, value.reject_limit_ppm);
  PutU64(&payload, 8, value.reject_limit_rows);
  PutArray(&payload, 16, value.reject_target_relation_uuid);
  PutU64(&payload, 32, value.reject_target_relation_generation);
  PutArray(&payload, 40, value.reject_target_relation_sha256);
  return EncodeNested(value.header, "IPOL", kPolicyDomain, payload, out,
                      diagnostic);
}

bool DecodePlanImportRowsPolicyV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsPolicyV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID", "IPOL output is null");
  }
  PlanImportRowsPolicyV1 decoded;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_bytes = 0;
  if (!DecodeNested(bytes, size, "IPOL", kPolicyDomain,
                    kPlanImportRowsPolicyPayloadBytesV1, &decoded.header,
                    &payload, &payload_bytes, diagnostic)) {
    return false;
  }
  decoded.reject_mode = static_cast<PlanImportRowsRejectModeV1>(payload[0]);
  decoded.reject_payload_policy =
      static_cast<PlanImportRowsRejectPayloadPolicyV1>(payload[1]);
  decoded.resume_policy =
      static_cast<PlanImportRowsResumePolicyV1>(payload[2]);
  decoded.flags = payload[3];
  decoded.reject_limit_ppm = GetU32(payload + 4);
  decoded.reject_limit_rows = GetU64(payload + 8);
  GetArray(payload + 16, &decoded.reject_target_relation_uuid);
  decoded.reject_target_relation_generation = GetU64(payload + 32);
  GetArray(payload + 40, &decoded.reject_target_relation_sha256);
  if (!ValidatePolicyFields(decoded, diagnostic)) return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  *out = std::move(decoded);
  return true;
}

bool EncodePlanImportRowsExecutorEvidenceV1(
    const PlanImportRowsExecutorEvidenceV1& value,
    Bytes* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID", "IPEV output is null");
  }
  out->clear();
  if (!ValidateEvidenceFields(value, diagnostic)) return false;

  Bytes encoded(kPlanImportRowsExecutorEvidenceBytesV1, 0);
  PutMagic(&encoded, "IPEV");
  PutU16(&encoded, 4, 1);
  PutU16(&encoded, 6,
         static_cast<std::uint16_t>(kPlanImportRowsExecutorEvidenceBytesV1));
  PutU32(&encoded, 8,
         static_cast<std::uint32_t>(kPlanImportRowsExecutorEvidenceBytesV1));
  PutArray(&encoded, 16, value.evidence_uuid);
  PutU64(&encoded, 32, value.evidence_generation);
  PutArray(&encoded, 40, value.request_descriptor_uuid);
  PutU64(&encoded, 56, value.request_descriptor_generation);
  PutArray(&encoded, 64, value.request_projection_sha256);
  PutU16(&encoded, 96, kPlanImportRowsOpcodeCodeV1);
  PutU16(&encoded, 98, 1);
  PutU16(&encoded, 100, 0);
  PutU64(&encoded, 108, value.executor_availability_generation);
  PutArray(&encoded, 116, value.transaction_uuid);
  PutU64(&encoded, 132, value.local_transaction_id);
  PutArray(&encoded, 140, value.mga_snapshot_uuid);
  PutU64(&encoded, 156, value.mga_snapshot_generation);
  PutU64(&encoded, 164, value.completed_validation_bits);

  PlanImportRowsSha256V1 digest{};
  if (!ComputeHash(kExecutorEvidenceDomain, encoded.data(), 176, nullptr, 0,
                   &digest)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPEV SHA-256 is unavailable");
  }
  PutArray(&encoded, 176, digest);
  *out = std::move(encoded);
  return true;
}

bool DecodePlanImportRowsExecutorEvidenceV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsExecutorEvidenceV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (bytes == nullptr || out == nullptr ||
      size != kPlanImportRowsExecutorEvidenceBytesV1 ||
      !ExactMagic(bytes, "IPEV") || GetU16(bytes + 4) != 1 ||
      GetU16(bytes + 6) != kPlanImportRowsExecutorEvidenceBytesV1 ||
      GetU32(bytes + 8) != kPlanImportRowsExecutorEvidenceBytesV1 ||
      GetU32(bytes + 12) != 0 ||
      GetU16(bytes + 96) != kPlanImportRowsOpcodeCodeV1 ||
      GetU16(bytes + 98) != 1 || GetU16(bytes + 100) != 0 ||
      !AllZero(bytes + 102, 6) || !AllZero(bytes + 172, 4)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPEV header, identity, reserved bytes, or extent is invalid");
  }

  PlanImportRowsSha256V1 expected_digest{};
  if (!ComputeHash(kExecutorEvidenceDomain, bytes, 176, nullptr, 0,
                   &expected_digest) ||
      !std::equal(expected_digest.begin(), expected_digest.end(), bytes + 176)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "IPEV evidence SHA-256 is invalid");
  }

  PlanImportRowsExecutorEvidenceV1 decoded;
  GetArray(bytes + 16, &decoded.evidence_uuid);
  decoded.evidence_generation = GetU64(bytes + 32);
  GetArray(bytes + 40, &decoded.request_descriptor_uuid);
  decoded.request_descriptor_generation = GetU64(bytes + 56);
  GetArray(bytes + 64, &decoded.request_projection_sha256);
  decoded.executor_availability_generation = GetU64(bytes + 108);
  GetArray(bytes + 116, &decoded.transaction_uuid);
  decoded.local_transaction_id = GetU64(bytes + 132);
  GetArray(bytes + 140, &decoded.mga_snapshot_uuid);
  decoded.mga_snapshot_generation = GetU64(bytes + 156);
  decoded.completed_validation_bits = GetU64(bytes + 164);
  GetArray(bytes + 176, &decoded.evidence_sha256);
  if (!ValidateEvidenceFields(decoded, diagnostic)) return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  *out = std::move(decoded);
  return true;
}

bool ValidatePlanImportRowsDescriptorReferenceV1(
    const PlanImportRowsDescriptorRefV1& reference,
    const PlanImportRowsPlanDescriptorV1& descriptor,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  Bytes ignored;
  if (!EncodePlanImportRowsDescriptorRefV1(reference, &ignored, diagnostic) ||
      !ValidatePlanDescriptorFields(descriptor, diagnostic)) {
    return false;
  }
  if (reference.descriptor_uuid != descriptor.descriptor_uuid ||
      reference.descriptor_generation != descriptor.descriptor_generation) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "SBOP descriptor-ref does not identify the resolved IPLP row");
  }
  return true;
}

bool ValidatePlanImportRowsCarrierSetV1(
    const PlanImportRowsCarrierSetV1& carriers,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  Bytes descriptor_bytes;
  Bytes source_bytes;
  Bytes format_bytes;
  Bytes mapping_bytes;
  Bytes policy_bytes;
  if (!EncodePlanImportRowsSourceEnvelopeV1(carriers.source, &source_bytes,
                                            diagnostic) ||
      !EncodePlanImportRowsFormatEnvelopeV1(carriers.format, &format_bytes,
                                            diagnostic) ||
      !EncodePlanImportRowsMappingVectorV1(carriers.mapping, &mapping_bytes,
                                           diagnostic) ||
      !EncodePlanImportRowsPolicyV1(carriers.policy, &policy_bytes,
                                    diagnostic) ||
      !EncodePlanImportRowsPlanDescriptorV1(carriers.descriptor,
                                            &descriptor_bytes, diagnostic)) {
    return false;
  }

  const PlanImportRowsSha256V1 source_digest = [&]() {
    PlanImportRowsSha256V1 value{};
    std::copy_n(source_bytes.begin() + 72, value.size(), value.begin());
    return value;
  }();
  const PlanImportRowsSha256V1 format_digest = [&]() {
    PlanImportRowsSha256V1 value{};
    std::copy_n(format_bytes.begin() + 72, value.size(), value.begin());
    return value;
  }();
  const PlanImportRowsSha256V1 mapping_digest = [&]() {
    PlanImportRowsSha256V1 value{};
    std::copy_n(mapping_bytes.begin() + 72, value.size(), value.begin());
    return value;
  }();
  const PlanImportRowsSha256V1 policy_digest = [&]() {
    PlanImportRowsSha256V1 value{};
    std::copy_n(policy_bytes.begin() + 72, value.size(), value.begin());
    return value;
  }();
  PlanImportRowsSha256V1 descriptor_digest{};
  std::copy_n(descriptor_bytes.begin() + 480, descriptor_digest.size(),
              descriptor_digest.begin());

  if (carriers.source.header.row_sha256 != source_digest ||
      carriers.format.header.row_sha256 != format_digest ||
      carriers.mapping.header.row_sha256 != mapping_digest ||
      carriers.policy.header.row_sha256 != policy_digest ||
      carriers.descriptor.descriptor_evidence_sha256 != descriptor_digest ||
      !ChildOwnedByDescriptor(carriers.source, carriers.descriptor) ||
      !ChildOwnedByDescriptor(carriers.format, carriers.descriptor) ||
      !ChildOwnedByDescriptor(carriers.mapping, carriers.descriptor) ||
      !ChildOwnedByDescriptor(carriers.policy, carriers.descriptor) ||
      !ChildMatchesReference(carriers.source,
                             carriers.descriptor.import_source_envelope_ref) ||
      !ChildMatchesReference(carriers.format,
                             carriers.descriptor.import_format_envelope_ref) ||
      !ChildMatchesReference(carriers.mapping,
                             carriers.descriptor.column_mapping_vector_ref) ||
      !ChildMatchesReference(carriers.policy,
                             carriers.descriptor.import_policy_ref)) {
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "plan-import parent/child ownership or hash binding is invalid");
  }

  if (!IsPlanImportRowsSourceFormatPairAdmittedV1(
          carriers.source.source_kind, carriers.format.format_family)) {
    return Fail(diagnostic, "SBLR.OPERATION_UNSUPPORTED",
                "plan-import source and format pair is recognized but unadmitted");
  }
  return true;
}

bool ValidatePlanImportRowsPolicyAdmissionV1(
    const PlanImportRowsPolicyV1& policy,
    bool reference_relaxed_semantics_authorized,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidatePolicyFields(policy, diagnostic)) return false;
  const bool relaxed_requested =
      (policy.flags &
       kPlanImportRowsReferenceRelaxedSemanticsRequestedV1) != 0;
  const bool strict_requested =
      (policy.flags & kPlanImportRowsStrictBulkLoadRequestedV1) != 0;
  if (relaxed_requested && !strict_requested &&
      !reference_relaxed_semantics_authorized) {
    return Fail(diagnostic, "SBLR.OPERATION_UNSUPPORTED",
                "plan-import relaxed policy profile is not admitted");
  }
  return true;
}

bool ValidatePlanImportRowsLiveAuthorityV1(
    const PlanImportRowsPlanDescriptorV1& descriptor,
    const PlanImportRowsLiveAuthorityV1& authority,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidatePlanDescriptorFields(descriptor, diagnostic)) return false;
  if (descriptor.authenticated_statement_receipt_uuid !=
          authority.authenticated_statement_receipt_uuid ||
      descriptor.structural_occurrence_id !=
          authority.structural_occurrence_id ||
      descriptor.local_transaction_id != authority.local_transaction_id ||
      descriptor.transaction_uuid != authority.transaction_uuid ||
      descriptor.mga_snapshot_uuid != authority.mga_snapshot_uuid ||
      descriptor.mga_snapshot_generation != authority.mga_snapshot_generation ||
      descriptor.security_snapshot_uuid != authority.security_snapshot_uuid ||
      descriptor.security_snapshot_generation !=
          authority.security_snapshot_generation ||
      descriptor.policy_snapshot_uuid != authority.policy_snapshot_uuid ||
      descriptor.policy_snapshot_generation !=
          authority.policy_snapshot_generation ||
      descriptor.resource_admission_uuid != authority.resource_admission_uuid ||
      descriptor.resource_admission_generation !=
          authority.resource_admission_generation ||
      descriptor.catalog_generation != authority.catalog_generation ||
      descriptor.target_table_uuid != authority.target_table_uuid ||
      descriptor.target_relation_descriptor_uuid !=
          authority.target_relation_descriptor_uuid ||
      descriptor.target_relation_descriptor_generation !=
          authority.target_relation_descriptor_generation ||
      descriptor.executor_availability_generation !=
          authority.executor_availability_generation) {
    return Fail(diagnostic, "MGA.AUTHORITY_MISMATCH",
                "IPLP is not equal to the live authenticated MGA authority");
  }
  return true;
}

bool ValidatePlanImportRowsExecutorEvidenceBindingV1(
    const PlanImportRowsExecutorEvidenceV1& evidence,
    const PlanImportRowsPlanDescriptorV1& descriptor,
    PlanImportRowsCodecDiagnosticV1* diagnostic) {
  Clear(diagnostic);
  if (!ValidateEvidenceFields(evidence, diagnostic) ||
      !ValidatePlanDescriptorFields(descriptor, diagnostic)) {
    return false;
  }
  if (evidence.request_descriptor_uuid != descriptor.descriptor_uuid ||
      evidence.request_descriptor_generation !=
          descriptor.descriptor_generation ||
      evidence.request_projection_sha256 !=
          descriptor.descriptor_evidence_sha256 ||
      evidence.executor_availability_generation !=
          descriptor.executor_availability_generation ||
      evidence.transaction_uuid != descriptor.transaction_uuid ||
      evidence.local_transaction_id != descriptor.local_transaction_id ||
      evidence.mga_snapshot_uuid != descriptor.mga_snapshot_uuid ||
      evidence.mga_snapshot_generation != descriptor.mga_snapshot_generation) {
    return Fail(diagnostic, "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                "IPEV does not bind the validated IPLP and live MGA context");
  }
  return true;
}

bool IsPlanImportRowsSourceFormatPairAdmittedV1(
    PlanImportRowsSourceKindV1 source_kind,
    PlanImportRowsFormatFamilyV1 format_family) noexcept {
  if (!ValidSourceKind(source_kind) || !ValidFormatFamily(format_family))
    return false;
  switch (source_kind) {
    case PlanImportRowsSourceKindV1::native_sbsql_import:
      return format_family == PlanImportRowsFormatFamilyV1::csv ||
             format_family == PlanImportRowsFormatFamilyV1::delimited_text ||
             format_family == PlanImportRowsFormatFamilyV1::fixed_width ||
             format_family == PlanImportRowsFormatFamilyV1::jsonl ||
             format_family == PlanImportRowsFormatFamilyV1::document ||
             format_family ==
                 PlanImportRowsFormatFamilyV1::binary_typed_rows ||
             format_family == PlanImportRowsFormatFamilyV1::xml ||
             format_family == PlanImportRowsFormatFamilyV1::line_protocol ||
             format_family == PlanImportRowsFormatFamilyV1::bulk_job;
    case PlanImportRowsSourceKindV1::csv_stream:
      return format_family == PlanImportRowsFormatFamilyV1::csv;
    case PlanImportRowsSourceKindV1::delimited_text:
      return format_family == PlanImportRowsFormatFamilyV1::csv ||
             format_family == PlanImportRowsFormatFamilyV1::delimited_text;
    case PlanImportRowsSourceKindV1::fixed_width_text:
      return format_family == PlanImportRowsFormatFamilyV1::fixed_width;
    case PlanImportRowsSourceKindV1::jsonl_stream:
      return format_family == PlanImportRowsFormatFamilyV1::jsonl;
    case PlanImportRowsSourceKindV1::document_stream:
      return format_family == PlanImportRowsFormatFamilyV1::document ||
             format_family == PlanImportRowsFormatFamilyV1::jsonl;
    case PlanImportRowsSourceKindV1::binary_typed_rows:
      return format_family ==
             PlanImportRowsFormatFamilyV1::binary_typed_rows;
    case PlanImportRowsSourceKindV1::reference_dump_replay:
      return format_family == PlanImportRowsFormatFamilyV1::reference_dump;
    case PlanImportRowsSourceKindV1::reference_bulk_api:
      return format_family == PlanImportRowsFormatFamilyV1::reference_bulk;
    case PlanImportRowsSourceKindV1::live_ingest_stream:
      return format_family == PlanImportRowsFormatFamilyV1::live_ingest ||
             format_family == PlanImportRowsFormatFamilyV1::line_protocol;
    case PlanImportRowsSourceKindV1::bulk_import_job:
      return format_family == PlanImportRowsFormatFamilyV1::bulk_job ||
             format_family == PlanImportRowsFormatFamilyV1::csv ||
             format_family == PlanImportRowsFormatFamilyV1::jsonl ||
             format_family ==
                 PlanImportRowsFormatFamilyV1::binary_typed_rows;
    case PlanImportRowsSourceKindV1::xml_stream:
      return format_family == PlanImportRowsFormatFamilyV1::xml;
    case PlanImportRowsSourceKindV1::line_protocol_stream:
      return format_family == PlanImportRowsFormatFamilyV1::line_protocol;
  }
  return false;
}

PlanImportRowsInsertModeV1 NormalizePlanImportRowsInsertModeV1(
    PlanImportRowsSourceKindV1 source_kind) noexcept {
  switch (source_kind) {
    case PlanImportRowsSourceKindV1::reference_bulk_api:
      return PlanImportRowsInsertModeV1::reference_bulk;
    case PlanImportRowsSourceKindV1::binary_typed_rows:
    case PlanImportRowsSourceKindV1::bulk_import_job:
      return PlanImportRowsInsertModeV1::native_bulk;
    case PlanImportRowsSourceKindV1::native_sbsql_import:
    case PlanImportRowsSourceKindV1::csv_stream:
    case PlanImportRowsSourceKindV1::delimited_text:
    case PlanImportRowsSourceKindV1::fixed_width_text:
    case PlanImportRowsSourceKindV1::jsonl_stream:
    case PlanImportRowsSourceKindV1::document_stream:
    case PlanImportRowsSourceKindV1::reference_dump_replay:
    case PlanImportRowsSourceKindV1::live_ingest_stream:
    case PlanImportRowsSourceKindV1::xml_stream:
    case PlanImportRowsSourceKindV1::line_protocol_stream:
      return PlanImportRowsInsertModeV1::copy_import;
  }
  return static_cast<PlanImportRowsInsertModeV1>(0);
}

}  // namespace scratchbird::engine::sblr
