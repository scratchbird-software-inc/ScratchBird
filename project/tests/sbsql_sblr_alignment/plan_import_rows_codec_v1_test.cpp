// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_PLAN_IMPORT_ROWS_CODEC_ONLY
#include "engine/sblr/sblr_engine_envelope.hpp"
#endif
#include "engine/sblr/sblr_plan_import_rows_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sblr = scratchbird::engine::sblr;
using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Die(std::string_view message) {
  std::cerr << "plan_import_rows_codec_v1_test: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Die(message);
}

void RequireFailure(bool result,
                    const sblr::PlanImportRowsCodecDiagnosticV1& diagnostic,
                    std::string_view expected_code,
                    std::string_view message) {
  if (result || diagnostic.code != expected_code) {
    std::cerr << "expected=" << expected_code
              << " actual=" << diagnostic.code
              << " detail=" << diagnostic.detail << '\n';
    Die(message);
  }
}

sblr::PlanImportRowsUuidV1 Uuid(std::uint8_t seed) {
  sblr::PlanImportRowsUuidV1 value{};
  for (std::size_t index = 0; index != value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

sblr::PlanImportRowsSha256V1 Sha(std::uint8_t seed) {
  sblr::PlanImportRowsSha256V1 value{};
  for (std::size_t index = 0; index != value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

std::string DigestHex(const Bytes& bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
  Require(digest.ok(), "fixture SHA-256 is unavailable");
  return scratchbird::core::hash::HexLower(digest.digest);
}

std::uint16_t U16(const Bytes& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t U32(const Bytes& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index != 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) <<
             (8 * index);
  }
  return value;
}

std::uint64_t U64(const Bytes& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index != 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) <<
             (8 * index);
  }
  return value;
}

sblr::PlanImportRowsNestedHeaderV1 ChildHeader(
    std::uint8_t row_seed,
    const sblr::PlanImportRowsUuidV1& owner_uuid,
    std::uint64_t owner_generation) {
  sblr::PlanImportRowsNestedHeaderV1 header;
  header.row_uuid = Uuid(row_seed);
  header.row_generation = static_cast<std::uint64_t>(row_seed) + 100;
  header.owner_plan_descriptor_uuid = owner_uuid;
  header.owner_plan_descriptor_generation = owner_generation;
  return header;
}

sblr::PlanImportRowsPlanDescriptorV1 BaseDescriptor() {
  sblr::PlanImportRowsPlanDescriptorV1 descriptor;
  descriptor.descriptor_uuid = Uuid(1);
  descriptor.descriptor_generation = 101;
  descriptor.request_uuid = Uuid(2);
  descriptor.authenticated_statement_receipt_uuid = Uuid(3);
  descriptor.structural_occurrence_id = 102;
  descriptor.local_transaction_id = 103;
  descriptor.transaction_uuid = Uuid(4);
  descriptor.mga_snapshot_uuid = Uuid(5);
  descriptor.mga_snapshot_generation = 104;
  descriptor.security_snapshot_uuid = Uuid(6);
  descriptor.security_snapshot_generation = 105;
  descriptor.policy_snapshot_uuid = Uuid(7);
  descriptor.policy_snapshot_generation = 106;
  descriptor.resource_admission_uuid = Uuid(8);
  descriptor.resource_admission_generation = 107;
  descriptor.catalog_generation = 108;
  descriptor.target_table_uuid = Uuid(9);
  descriptor.target_relation_descriptor_uuid = Uuid(10);
  descriptor.target_relation_descriptor_generation = 109;
  descriptor.executor_availability_generation = 110;
  return descriptor;
}

sblr::PlanImportRowsSourceEnvelopeV1 BaseSource(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsSourceEnvelopeV1 source;
  source.header = ChildHeader(21, descriptor.descriptor_uuid,
                              descriptor.descriptor_generation);
  source.source_kind = sblr::PlanImportRowsSourceKindV1::csv_stream;
  source.flags = sblr::kPlanImportRowsSourceFingerprintPresentV1;
  source.engine_source_binding_uuid = Uuid(22);
  source.engine_source_binding_generation = 121;
  source.source_fingerprint_sha256 = Sha(23);
  return source;
}

sblr::PlanImportRowsFormatEnvelopeV1 BaseFormat(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsFormatEnvelopeV1 format;
  format.header = ChildHeader(31, descriptor.descriptor_uuid,
                              descriptor.descriptor_generation);
  format.format_family = sblr::PlanImportRowsFormatFamilyV1::csv;
  return format;
}

sblr::PlanImportRowsMappingVectorV1 BaseMapping(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsMappingVectorV1 mapping;
  mapping.header = ChildHeader(41, descriptor.descriptor_uuid,
                               descriptor.descriptor_generation);
  for (std::uint32_t index = 0; index != 2; ++index) {
    sblr::PlanImportRowsMappingRecordV1 record;
    record.source_field_ordinal = index == 0 ? 0 : 4;
    record.flags = index == 0 ? sblr::kPlanImportRowsMappingRequiredV1 : 0;
    record.target_column_uuid = Uuid(static_cast<std::uint8_t>(42 + index));
    record.target_column_generation = 131 + index;
    record.target_datatype_descriptor_uuid =
        Uuid(static_cast<std::uint8_t>(44 + index));
    record.target_datatype_descriptor_generation = 141 + index;
    record.target_type_uuid = Uuid(static_cast<std::uint8_t>(46 + index));
    record.target_type_generation = 151 + index;
    record.codec_id = static_cast<std::uint16_t>(161 + index);
    record.codec_version = static_cast<std::uint16_t>(171 + index);
    record.codec_generation = 181 + index;
    mapping.mappings.push_back(record);
  }
  return mapping;
}

sblr::PlanImportRowsPolicyV1 BasePolicy(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsPolicyV1 policy;
  policy.header = ChildHeader(51, descriptor.descriptor_uuid,
                              descriptor.descriptor_generation);
  policy.reject_mode = sblr::PlanImportRowsRejectModeV1::reject_table;
  policy.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::redacted_payload_reference;
  policy.resume_policy =
      sblr::PlanImportRowsResumePolicyV1::operator_review_required;
  policy.flags = sblr::kPlanImportRowsStrictBulkLoadRequestedV1 |
                 sblr::kPlanImportRowsReferenceRelaxedSemanticsRequestedV1;
  policy.reject_limit_ppm = 25000;
  policy.reject_limit_rows = 19;
  policy.reject_target_relation_uuid = Uuid(52);
  policy.reject_target_relation_generation = 191;
  policy.reject_target_relation_sha256 = Sha(53);
  return policy;
}

sblr::PlanImportRowsCarrierSetV1 CanonicalCarriers(
    Bytes* source_bytes,
    Bytes* format_bytes,
    Bytes* mapping_bytes,
    Bytes* policy_bytes,
    Bytes* descriptor_bytes) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  sblr::PlanImportRowsCarrierSetV1 carriers;
  carriers.descriptor = BaseDescriptor();

  const auto source = BaseSource(carriers.descriptor);
  Require(sblr::EncodePlanImportRowsSourceEnvelopeV1(
              source, source_bytes, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsSourceEnvelopeV1(
              source_bytes->data(), source_bytes->size(), &carriers.source,
              &diagnostic),
          diagnostic.detail);

  const auto format = BaseFormat(carriers.descriptor);
  Require(sblr::EncodePlanImportRowsFormatEnvelopeV1(
              format, format_bytes, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsFormatEnvelopeV1(
              format_bytes->data(), format_bytes->size(), &carriers.format,
              &diagnostic),
          diagnostic.detail);

  const auto mapping = BaseMapping(carriers.descriptor);
  Require(sblr::EncodePlanImportRowsMappingVectorV1(
              mapping, mapping_bytes, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsMappingVectorV1(
              mapping_bytes->data(), mapping_bytes->size(), &carriers.mapping,
              &diagnostic),
          diagnostic.detail);

  const auto policy = BasePolicy(carriers.descriptor);
  Require(sblr::EncodePlanImportRowsPolicyV1(
              policy, policy_bytes, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsPolicyV1(
              policy_bytes->data(), policy_bytes->size(), &carriers.policy,
              &diagnostic),
          diagnostic.detail);

  const auto ref = [](const auto& child) {
    sblr::PlanImportRowsChildRefV1 value;
    value.row_uuid = child.header.row_uuid;
    value.row_generation = child.header.row_generation;
    value.canonical_row_sha256 = child.header.row_sha256;
    return value;
  };
  carriers.descriptor.import_source_envelope_ref = ref(carriers.source);
  carriers.descriptor.import_format_envelope_ref = ref(carriers.format);
  carriers.descriptor.column_mapping_vector_ref = ref(carriers.mapping);
  carriers.descriptor.import_policy_ref = ref(carriers.policy);
  Require(sblr::EncodePlanImportRowsPlanDescriptorV1(
              carriers.descriptor, descriptor_bytes, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsPlanDescriptorV1(
              descriptor_bytes->data(), descriptor_bytes->size(),
              &carriers.descriptor, &diagnostic),
          diagnostic.detail);
  return carriers;
}

template <typename Value, typename Encode>
void RequireReencode(const Value& value,
                     const Bytes& canonical,
                     Encode encode,
                     std::string_view message) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Bytes encoded;
  Require(encode(value, &encoded, &diagnostic), diagnostic.detail);
  Require(encoded == canonical, message);
}

void RequireDescriptorRefContract(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  sblr::PlanImportRowsDescriptorRefV1 reference{descriptor.descriptor_uuid,
                                                 descriptor.descriptor_generation};
  Bytes bytes;
  Require(sblr::EncodePlanImportRowsDescriptorRefV1(reference, &bytes,
                                                     &diagnostic),
          diagnostic.detail);
  Require(bytes.size() == 24 &&
              U64(bytes, 16) == descriptor.descriptor_generation,
          "SBOP descriptor-ref layout differs from [UUID,generation]");
  sblr::PlanImportRowsDescriptorRefV1 decoded;
  Require(sblr::DecodePlanImportRowsDescriptorRefV1(
              bytes.data(), bytes.size(), &decoded, &diagnostic),
          diagnostic.detail);
  Require(decoded.descriptor_uuid == reference.descriptor_uuid &&
              decoded.descriptor_generation == reference.descriptor_generation,
          "SBOP descriptor-ref decode changed identity");
  Require(sblr::ValidatePlanImportRowsDescriptorReferenceV1(
              decoded, descriptor, &diagnostic),
          diagnostic.detail);

  Bytes truncated(bytes.begin(), bytes.end() - 1);
  RequireFailure(sblr::DecodePlanImportRowsDescriptorRefV1(
                     truncated.data(), truncated.size(), &decoded, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "truncated SBOP descriptor-ref was admitted");
  bytes.push_back(0);
  RequireFailure(sblr::DecodePlanImportRowsDescriptorRefV1(
                     bytes.data(), bytes.size(), &decoded, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "trailing SBOP descriptor-ref byte was admitted");
  reference.descriptor_uuid = {};
  RequireFailure(sblr::EncodePlanImportRowsDescriptorRefV1(
                     reference, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "zero SBOP descriptor UUID was admitted");
  reference.descriptor_uuid = descriptor.descriptor_uuid;
  reference.descriptor_generation = 0;
  RequireFailure(sblr::EncodePlanImportRowsDescriptorRefV1(
                     reference, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "zero SBOP descriptor generation was admitted");
}

void RequirePlanAndChildRoundTrips(
    const sblr::PlanImportRowsCarrierSetV1& carriers,
    const Bytes& source_bytes,
    const Bytes& format_bytes,
    const Bytes& mapping_bytes,
    const Bytes& policy_bytes,
    const Bytes& descriptor_bytes) {
  Require(source_bytes.size() == 168 &&
              std::equal(source_bytes.begin(), source_bytes.begin() + 4,
                         "ISRC") &&
              U16(source_bytes, 4) == 1 && U16(source_bytes, 6) == 104 &&
              U32(source_bytes, 8) == 168 && U32(source_bytes, 64) == 64,
          "ISRC golden header or extent differs");
  Require(format_bytes.size() == 112 &&
              std::equal(format_bytes.begin(), format_bytes.begin() + 4,
                         "IFMT") &&
              U32(format_bytes, 64) == 8,
          "IFMT golden header or extent differs");
  Require(mapping_bytes.size() == 304 &&
              std::equal(mapping_bytes.begin(), mapping_bytes.begin() + 4,
                         "IMAP") &&
              U32(mapping_bytes, 64) == 200 && U32(mapping_bytes, 104) == 2 &&
              U32(mapping_bytes, 108) == 96,
          "IMAP golden header, count, or extent differs");
  Require(policy_bytes.size() == 176 &&
              std::equal(policy_bytes.begin(), policy_bytes.begin() + 4,
                         "IPOL") &&
              U32(policy_bytes, 64) == 72 && policy_bytes[104] == 3 &&
              policy_bytes[105] == 2 && policy_bytes[106] == 3,
          "IPOL golden header or stable enum map differs");
  Require(descriptor_bytes.size() == 512 &&
              std::equal(descriptor_bytes.begin(), descriptor_bytes.begin() + 4,
                         "IPLP") &&
              U16(descriptor_bytes, 4) == 1 && U16(descriptor_bytes, 6) == 512 &&
              U32(descriptor_bytes, 8) == 512 &&
              U64(descriptor_bytes, 472) ==
                  carriers.descriptor.executor_availability_generation,
          "IPLP golden header or exact extent differs");

  RequireReencode(carriers.source, source_bytes,
                  sblr::EncodePlanImportRowsSourceEnvelopeV1,
                  "ISRC decode/re-encode was not byte-identical");
  RequireReencode(carriers.format, format_bytes,
                  sblr::EncodePlanImportRowsFormatEnvelopeV1,
                  "IFMT decode/re-encode was not byte-identical");
  RequireReencode(carriers.mapping, mapping_bytes,
                  sblr::EncodePlanImportRowsMappingVectorV1,
                  "IMAP decode/re-encode was not byte-identical");
  RequireReencode(carriers.policy, policy_bytes,
                  sblr::EncodePlanImportRowsPolicyV1,
                  "IPOL decode/re-encode was not byte-identical");
  RequireReencode(carriers.descriptor, descriptor_bytes,
                  sblr::EncodePlanImportRowsPlanDescriptorV1,
                  "IPLP decode/re-encode was not byte-identical");

  Require(DigestHex(source_bytes) ==
              "60807535a29ff510d81b4228614331b14f8a58c7d2965d1e1d83e96f93966a5a",
          "ISRC golden fixture SHA-256 differs");
  Require(DigestHex(format_bytes) ==
              "af3dfbf40a027a5469c3e56e1127488686c186693bcc38533c4c39b1a8347544",
          "IFMT golden fixture SHA-256 differs");
  Require(DigestHex(mapping_bytes) ==
              "b01505d28e824221e415212d60fccafe2d7859a86e91a4f7f258c03b9c2c1453",
          "IMAP golden fixture SHA-256 differs");
  Require(DigestHex(policy_bytes) ==
              "85d60859abd34b29b86485357284c88412569cdde0fc8318a87b0f6282020384",
          "IPOL golden fixture SHA-256 differs");
  Require(DigestHex(descriptor_bytes) ==
              "a0ffab49e0c2510327bd6692a4b0933934ca70fa648717a71767aae94a7cd38f",
          "IPLP golden fixture SHA-256 differs");
}

void RequireMalformedCarrierRefusals(
    const sblr::PlanImportRowsCarrierSetV1& carriers,
    const Bytes& source_bytes,
    const Bytes& format_bytes,
    const Bytes& mapping_bytes,
    const Bytes& policy_bytes,
    const Bytes& descriptor_bytes) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  sblr::PlanImportRowsPlanDescriptorV1 descriptor;
  Bytes damaged = descriptor_bytes;
  damaged[0] ^= 0x01;
  RequireFailure(sblr::DecodePlanImportRowsPlanDescriptorV1(
                     damaged.data(), damaged.size(), &descriptor, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong IPLP magic was admitted");
  damaged = descriptor_bytes;
  damaged[4] = 2;
  RequireFailure(sblr::DecodePlanImportRowsPlanDescriptorV1(
                     damaged.data(), damaged.size(), &descriptor, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong IPLP version was admitted");
  damaged = descriptor_bytes;
  damaged[12] = 1;
  RequireFailure(sblr::DecodePlanImportRowsPlanDescriptorV1(
                     damaged.data(), damaged.size(), &descriptor, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "nonzero IPLP flags were admitted");
  damaged = descriptor_bytes;
  damaged[200] ^= 0x01;
  RequireFailure(sblr::DecodePlanImportRowsPlanDescriptorV1(
                     damaged.data(), damaged.size(), &descriptor, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong IPLP evidence hash was admitted");
  damaged = descriptor_bytes;
  damaged.pop_back();
  RequireFailure(sblr::DecodePlanImportRowsPlanDescriptorV1(
                     damaged.data(), damaged.size(), &descriptor, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "truncated IPLP was admitted");
  damaged = descriptor_bytes;
  damaged.push_back(0);
  RequireFailure(sblr::DecodePlanImportRowsPlanDescriptorV1(
                     damaged.data(), damaged.size(), &descriptor, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "IPLP trailing byte was admitted");

  sblr::PlanImportRowsSourceEnvelopeV1 source;
  damaged = source_bytes;
  damaged[68] = 1;
  RequireFailure(sblr::DecodePlanImportRowsSourceEnvelopeV1(
                     damaged.data(), damaged.size(), &source, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "nonzero ISRC header reserved byte was admitted");
  damaged = source_bytes;
  damaged[72] ^= 0x01;
  RequireFailure(sblr::DecodePlanImportRowsSourceEnvelopeV1(
                     damaged.data(), damaged.size(), &source, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong ISRC row hash was admitted");
  source = carriers.source;
  source.source_kind = static_cast<sblr::PlanImportRowsSourceKindV1>(0);
  RequireFailure(sblr::EncodePlanImportRowsSourceEnvelopeV1(
                     source, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "zero source-kind code was admitted");
  source = carriers.source;
  source.flags = 0x0002;
  RequireFailure(sblr::EncodePlanImportRowsSourceEnvelopeV1(
                     source, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "unknown ISRC flag was admitted");
  source = carriers.source;
  source.flags = 0;
  RequireFailure(sblr::EncodePlanImportRowsSourceEnvelopeV1(
                     source, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "unflagged ISRC fingerprint was admitted");
  source = carriers.source;
  source.source_fingerprint_sha256 = {};
  RequireFailure(sblr::EncodePlanImportRowsSourceEnvelopeV1(
                     source, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "flagged all-zero ISRC fingerprint was admitted");

  sblr::PlanImportRowsFormatEnvelopeV1 format;
  damaged = format_bytes;
  damaged[106] = 1;
  RequireFailure(sblr::DecodePlanImportRowsFormatEnvelopeV1(
                     damaged.data(), damaged.size(), &format, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "nonzero IFMT reserved byte was admitted");
  format = carriers.format;
  format.format_family = static_cast<sblr::PlanImportRowsFormatFamilyV1>(13);
  RequireFailure(sblr::EncodePlanImportRowsFormatEnvelopeV1(
                     format, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "unknown format-family code was admitted");

  sblr::PlanImportRowsMappingVectorV1 mapping;
  damaged = mapping_bytes;
  damaged[108] = 95;
  RequireFailure(sblr::DecodePlanImportRowsMappingVectorV1(
                     damaged.data(), damaged.size(), &mapping, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong IMAP record size was admitted");
  damaged = mapping_bytes;
  damaged[104] = 3;
  RequireFailure(sblr::DecodePlanImportRowsMappingVectorV1(
                     damaged.data(), damaged.size(), &mapping, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "inconsistent IMAP count was admitted");
  damaged = mapping_bytes;
  damaged[196] = 1;
  RequireFailure(sblr::DecodePlanImportRowsMappingVectorV1(
                     damaged.data(), damaged.size(), &mapping, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "nonzero IMAP record reserved byte was admitted");
  mapping = carriers.mapping;
  mapping.mappings[1].source_field_ordinal = 0;
  RequireFailure(sblr::EncodePlanImportRowsMappingVectorV1(
                     mapping, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "unordered IMAP source ordinal was admitted");
  mapping = carriers.mapping;
  mapping.mappings[1].target_column_uuid =
      mapping.mappings[0].target_column_uuid;
  RequireFailure(sblr::EncodePlanImportRowsMappingVectorV1(
                     mapping, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "duplicate IMAP target column was admitted");
  mapping = carriers.mapping;
  mapping.mappings[0].codec_id = 0;
  RequireFailure(sblr::EncodePlanImportRowsMappingVectorV1(
                     mapping, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "zero IMAP codec identity was admitted");
  damaged = mapping_bytes;
  damaged[104] = 0x01;
  damaged[105] = 0x00;
  damaged[106] = 0x04;
  damaged[107] = 0x00;
  RequireFailure(sblr::DecodePlanImportRowsMappingVectorV1(
                     damaged.data(), damaged.size(), &mapping, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "small over-limit IMAP carrier was admitted");
  damaged.assign(mapping_bytes.begin(), mapping_bytes.begin() + 111);
  RequireFailure(sblr::DecodePlanImportRowsMappingVectorV1(
                     damaged.data(), damaged.size(), &mapping, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "truncated IMAP prefix was admitted");

  sblr::PlanImportRowsPolicyV1 policy;
  damaged = policy_bytes;
  damaged[104] = 0;
  RequireFailure(sblr::DecodePlanImportRowsPolicyV1(
                     damaged.data(), damaged.size(), &policy, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "zero IPOL reject-mode code was admitted");
  policy = carriers.policy;
  policy.flags = 0x04;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     policy, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "unknown IPOL flag was admitted");
  policy = carriers.policy;
  policy.reject_limit_ppm = 1000001;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     policy, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "over-bound IPOL ppm was admitted");
  policy = carriers.policy;
  policy.reject_target_relation_generation = 0;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     policy, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "partial IPOL reject-target tuple was admitted");
}

void RequireMappingAndPolicyRules(
    const sblr::PlanImportRowsCarrierSetV1& carriers) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Bytes bytes;
  auto mapping = carriers.mapping;
  mapping.mappings.clear();
  mapping.header.row_uuid = Uuid(61);
  mapping.header.row_generation = 201;
  mapping.header.row_sha256 = {};
  Require(sblr::EncodePlanImportRowsMappingVectorV1(mapping, &bytes,
                                                    &diagnostic),
          diagnostic.detail);
  Require(bytes.size() == 112 && U32(bytes, 104) == 0 &&
              U32(bytes, 108) == 96,
          "zero-count IMAP did not preserve explicit no-mapping authority");
  sblr::PlanImportRowsMappingVectorV1 decoded_mapping;
  Require(sblr::DecodePlanImportRowsMappingVectorV1(
              bytes.data(), bytes.size(), &decoded_mapping, &diagnostic) &&
              decoded_mapping.mappings.empty(),
          "zero-count IMAP did not round-trip exactly");

  auto fail_fast = carriers.policy;
  fail_fast.reject_mode = sblr::PlanImportRowsRejectModeV1::fail_fast;
  fail_fast.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only;
  fail_fast.reject_limit_ppm = 0;
  fail_fast.reject_limit_rows = 0;
  fail_fast.reject_target_relation_uuid = {};
  fail_fast.reject_target_relation_generation = 0;
  fail_fast.reject_target_relation_sha256 = {};
  Require(sblr::EncodePlanImportRowsPolicyV1(fail_fast, &bytes, &diagnostic),
          diagnostic.detail);
  auto invalid = fail_fast;
  invalid.reject_limit_rows = 1;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     invalid, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "fail_fast accepted a nonzero limit");

  auto reject_row = fail_fast;
  reject_row.reject_mode = sblr::PlanImportRowsRejectModeV1::reject_row;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     reject_row, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "reject_row accepted zero row and ppm limits");
  reject_row.reject_limit_ppm = 1;
  Require(sblr::EncodePlanImportRowsPolicyV1(reject_row, &bytes, &diagnostic),
          diagnostic.detail);

  auto reject_table = carriers.policy;
  reject_table.reject_limit_ppm = 0;
  reject_table.reject_limit_rows = 0;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     reject_table, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "reject_table accepted zero row and ppm limits");
  reject_table = carriers.policy;
  reject_table.reject_target_relation_uuid = {};
  reject_table.reject_target_relation_generation = 0;
  reject_table.reject_target_relation_sha256 = {};
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     reject_table, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "reject_table accepted an absent target");

  auto quarantine = carriers.policy;
  quarantine.reject_mode = sblr::PlanImportRowsRejectModeV1::quarantine;
  quarantine.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::encrypted_payload_reference;
  Require(sblr::EncodePlanImportRowsPolicyV1(quarantine, &bytes, &diagnostic),
          diagnostic.detail);
  quarantine.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only;
  RequireFailure(sblr::EncodePlanImportRowsPolicyV1(
                     quarantine, &bytes, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "quarantine accepted diagnostic-only payload policy");

  auto relaxed = fail_fast;
  relaxed.flags = sblr::kPlanImportRowsReferenceRelaxedSemanticsRequestedV1;
  RequireFailure(sblr::ValidatePlanImportRowsPolicyAdmissionV1(
                     relaxed, false, &diagnostic),
                 diagnostic, "SBLR.OPERATION_UNSUPPORTED",
                 "unadmitted relaxed policy profile was accepted");
  Require(sblr::ValidatePlanImportRowsPolicyAdmissionV1(
              relaxed, true, &diagnostic),
          diagnostic.detail);
  relaxed.flags |= sblr::kPlanImportRowsStrictBulkLoadRequestedV1;
  Require(sblr::ValidatePlanImportRowsPolicyAdmissionV1(
              relaxed, false, &diagnostic),
          diagnostic.detail);
}

void RequireCarrierBindingsAndEnums(
    const sblr::PlanImportRowsCarrierSetV1& carriers) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Require(sblr::ValidatePlanImportRowsCarrierSetV1(carriers, &diagnostic),
          diagnostic.detail);

  auto wrong_owner = carriers;
  wrong_owner.source.header.owner_plan_descriptor_uuid = Uuid(90);
  RequireFailure(sblr::ValidatePlanImportRowsCarrierSetV1(
                     wrong_owner, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong child owner was admitted");
  auto wrong_hash = carriers;
  wrong_hash.descriptor.import_policy_ref.canonical_row_sha256[0] ^= 1;
  RequireFailure(sblr::ValidatePlanImportRowsCarrierSetV1(
                     wrong_hash, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong parent child-hash reference was admitted");

  auto unsupported = carriers;
  unsupported.format.format_family =
      sblr::PlanImportRowsFormatFamilyV1::jsonl;
  Bytes canonical;
  Require(sblr::EncodePlanImportRowsFormatEnvelopeV1(
              unsupported.format, &canonical, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsFormatEnvelopeV1(
              canonical.data(), canonical.size(), &unsupported.format,
              &diagnostic),
          diagnostic.detail);
  unsupported.descriptor.import_format_envelope_ref.row_uuid =
      unsupported.format.header.row_uuid;
  unsupported.descriptor.import_format_envelope_ref.row_generation =
      unsupported.format.header.row_generation;
  unsupported.descriptor.import_format_envelope_ref.canonical_row_sha256 =
      unsupported.format.header.row_sha256;
  Require(sblr::EncodePlanImportRowsPlanDescriptorV1(
              unsupported.descriptor, &canonical, &diagnostic),
          diagnostic.detail);
  Require(sblr::DecodePlanImportRowsPlanDescriptorV1(
              canonical.data(), canonical.size(), &unsupported.descriptor,
              &diagnostic),
          diagnostic.detail);
  RequireFailure(sblr::ValidatePlanImportRowsCarrierSetV1(
                     unsupported, &diagnostic),
                 diagnostic, "SBLR.OPERATION_UNSUPPORTED",
                 "recognized but unadmitted source/format pair was accepted");

  Require(sblr::IsPlanImportRowsSourceFormatPairAdmittedV1(
              sblr::PlanImportRowsSourceKindV1::native_sbsql_import,
              sblr::PlanImportRowsFormatFamilyV1::bulk_job) &&
              sblr::IsPlanImportRowsSourceFormatPairAdmittedV1(
                  sblr::PlanImportRowsSourceKindV1::live_ingest_stream,
                  sblr::PlanImportRowsFormatFamilyV1::line_protocol) &&
              !sblr::IsPlanImportRowsSourceFormatPairAdmittedV1(
                  sblr::PlanImportRowsSourceKindV1::xml_stream,
                  sblr::PlanImportRowsFormatFamilyV1::csv),
          "closed source/format admission matrix differs from Core");
  Require(sblr::NormalizePlanImportRowsInsertModeV1(
              sblr::PlanImportRowsSourceKindV1::reference_bulk_api) ==
              sblr::PlanImportRowsInsertModeV1::reference_bulk &&
              sblr::NormalizePlanImportRowsInsertModeV1(
                  sblr::PlanImportRowsSourceKindV1::binary_typed_rows) ==
                  sblr::PlanImportRowsInsertModeV1::native_bulk &&
              sblr::NormalizePlanImportRowsInsertModeV1(
                  sblr::PlanImportRowsSourceKindV1::csv_stream) ==
                  sblr::PlanImportRowsInsertModeV1::copy_import,
          "normalized insert-mode map differs from Core");
}

sblr::PlanImportRowsLiveAuthorityV1 LiveAuthority(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsLiveAuthorityV1 authority;
  authority.authenticated_statement_receipt_uuid =
      descriptor.authenticated_statement_receipt_uuid;
  authority.structural_occurrence_id = descriptor.structural_occurrence_id;
  authority.local_transaction_id = descriptor.local_transaction_id;
  authority.transaction_uuid = descriptor.transaction_uuid;
  authority.mga_snapshot_uuid = descriptor.mga_snapshot_uuid;
  authority.mga_snapshot_generation = descriptor.mga_snapshot_generation;
  authority.security_snapshot_uuid = descriptor.security_snapshot_uuid;
  authority.security_snapshot_generation =
      descriptor.security_snapshot_generation;
  authority.policy_snapshot_uuid = descriptor.policy_snapshot_uuid;
  authority.policy_snapshot_generation = descriptor.policy_snapshot_generation;
  authority.resource_admission_uuid = descriptor.resource_admission_uuid;
  authority.resource_admission_generation =
      descriptor.resource_admission_generation;
  authority.catalog_generation = descriptor.catalog_generation;
  authority.target_table_uuid = descriptor.target_table_uuid;
  authority.target_relation_descriptor_uuid =
      descriptor.target_relation_descriptor_uuid;
  authority.target_relation_descriptor_generation =
      descriptor.target_relation_descriptor_generation;
  authority.executor_availability_generation =
      descriptor.executor_availability_generation;
  return authority;
}

void RequireLiveAuthorityAndEvidence(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  auto live = LiveAuthority(descriptor);
  Require(sblr::ValidatePlanImportRowsLiveAuthorityV1(
              descriptor, live, &diagnostic),
          diagnostic.detail);
  ++live.mga_snapshot_generation;
  RequireFailure(sblr::ValidatePlanImportRowsLiveAuthorityV1(
                     descriptor, live, &diagnostic),
                 diagnostic, "MGA.AUTHORITY_MISMATCH",
                 "stale MGA snapshot did not use exact authority diagnostic");

  sblr::PlanImportRowsExecutorEvidenceV1 evidence;
  evidence.evidence_uuid = Uuid(71);
  evidence.evidence_generation = 211;
  evidence.request_descriptor_uuid = descriptor.descriptor_uuid;
  evidence.request_descriptor_generation = descriptor.descriptor_generation;
  evidence.request_projection_sha256 = descriptor.descriptor_evidence_sha256;
  evidence.executor_availability_generation =
      descriptor.executor_availability_generation;
  evidence.transaction_uuid = descriptor.transaction_uuid;
  evidence.local_transaction_id = descriptor.local_transaction_id;
  evidence.mga_snapshot_uuid = descriptor.mga_snapshot_uuid;
  evidence.mga_snapshot_generation = descriptor.mga_snapshot_generation;
  evidence.completed_validation_bits =
      sblr::kPlanImportRowsAcceptedValidationBitsV1;

  Bytes bytes;
  Require(sblr::EncodePlanImportRowsExecutorEvidenceV1(
              evidence, &bytes, &diagnostic),
          diagnostic.detail);
  Require(bytes.size() == 208 &&
              std::equal(bytes.begin(), bytes.begin() + 4, "IPEV") &&
              U16(bytes, 96) == 793 && U16(bytes, 98) == 1 &&
              U16(bytes, 100) == 0 && U64(bytes, 164) == 0x3ff,
          "IPEV exact layout, opcode identity, or validation mask differs");
  sblr::PlanImportRowsExecutorEvidenceV1 decoded;
  Require(sblr::DecodePlanImportRowsExecutorEvidenceV1(
              bytes.data(), bytes.size(), &decoded, &diagnostic),
          diagnostic.detail);
  RequireReencode(decoded, bytes,
                  sblr::EncodePlanImportRowsExecutorEvidenceV1,
                  "IPEV decode/re-encode was not byte-identical");
  Require(DigestHex(bytes) ==
              "8c30388297f3dd27423c067b311bbf58a81a2462b16ad07516e198489d950579",
          "IPEV golden fixture SHA-256 differs");
  Require(sblr::ValidatePlanImportRowsExecutorEvidenceBindingV1(
              decoded, descriptor, &diagnostic),
          diagnostic.detail);

  auto damaged = bytes;
  damaged[102] = 1;
  RequireFailure(sblr::DecodePlanImportRowsExecutorEvidenceV1(
                     damaged.data(), damaged.size(), &decoded, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "nonzero IPEV reserved byte was admitted");
  damaged = bytes;
  damaged[96] ^= 1;
  RequireFailure(sblr::DecodePlanImportRowsExecutorEvidenceV1(
                     damaged.data(), damaged.size(), &decoded, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "wrong IPEV opcode was admitted");
  evidence.completed_validation_bits = 0x1ff;
  RequireFailure(sblr::EncodePlanImportRowsExecutorEvidenceV1(
                     evidence, &damaged, &diagnostic),
                 diagnostic, "SBLR.OPERAND_INVALID",
                 "partial IPEV validation mask was admitted");
  decoded.request_projection_sha256[0] ^= 1;
  RequireFailure(sblr::ValidatePlanImportRowsExecutorEvidenceBindingV1(
                     decoded, descriptor, &diagnostic),
                 diagnostic, "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                 "wrong IPEV binding did not fail as missing evidence");
}

#ifndef SCRATCHBIRD_PLAN_IMPORT_ROWS_CODEC_ONLY
void RequireEnvelopeWhitelist(
    const sblr::PlanImportRowsPlanDescriptorV1& descriptor) {
  sblr::PlanImportRowsCodecDiagnosticV1 codec_diagnostic;
  Bytes ref_bytes;
  Require(sblr::EncodePlanImportRowsDescriptorRefV1(
              {descriptor.descriptor_uuid, descriptor.descriptor_generation},
              &ref_bytes, &codec_diagnostic),
          codec_diagnostic.detail);
  auto envelope = sblr::MakeSblrEnvelope(
      "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS",
      "plan-import-rows-codec-v1");
  envelope.opcode_code = 793;
  envelope.result_shape = "import_plan_result";
  envelope.diagnostic_shape = "engine.diagnostic.v1";
  envelope.parser_package_uuid = "019d0000-0000-7000-8000-000000000101";
  envelope.registry_snapshot_uuid = "019d0000-0000-7000-8000-000000000102";
  envelope.requires_transaction_context = true;
  envelope.requires_security_context = true;
  envelope.operands.push_back({"import_rows_plan_descriptor", "request", "",
                               1, sblr::SblrValueKind::descriptor_ref, 0,
                               ref_bytes});
  const auto valid = sblr::ValidateSblrEnvelope(envelope);
  if (!valid.ok) {
    for (const auto& item : valid.diagnostics) {
      std::cerr << item.code << ':' << item.message << '\n';
    }
  }
  Require(valid.ok,
          "exact dml.plan_import_rows 24-byte descriptor-ref was not whitelisted");

  auto wrong_version = envelope;
  wrong_version.operation_version_minor = 1;
  const auto wrong_version_result = sblr::ValidateSblrEnvelope(wrong_version);
  Require(!wrong_version_result.ok &&
              !wrong_version_result.diagnostics.empty() &&
              wrong_version_result.diagnostics.front().code ==
                  "SBLR.OPCODE_INVALID",
          "plan-import operation-version mismatch did not exact-refuse as opcode identity");

  auto wrong_result_shape = envelope;
  wrong_result_shape.result_shape = "query_result";
  const auto wrong_result_shape_result =
      sblr::ValidateSblrEnvelope(wrong_result_shape);
  Require(!wrong_result_shape_result.ok &&
              !wrong_result_shape_result.diagnostics.empty() &&
              wrong_result_shape_result.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "wrong plan-import result shape did not exact-refuse as an invalid operand");

  envelope.operands.front().type = "dml.plan_import_rows";
  const auto wrong_type = sblr::ValidateSblrEnvelope(envelope);
  Require(!wrong_type.ok && !wrong_type.diagnostics.empty() &&
              wrong_type.diagnostics.front().code == "SBLR.OPERAND_INVALID",
          "wrong plan-import operand metadata did not exact-refuse");
  envelope.operands.front().type = "import_rows_plan_descriptor";
  envelope.opcode_code = 792;
  const auto wrong_opcode = sblr::ValidateSblrEnvelope(envelope);
  Require(!wrong_opcode.ok && !wrong_opcode.diagnostics.empty() &&
              wrong_opcode.diagnostics.front().code == "SBLR.OPCODE_INVALID",
          "plan-import identity mismatch did not use SBLR.OPCODE_INVALID");
}
#endif

void RequireNoForbiddenText(const std::vector<Bytes>& carriers) {
  for (const auto& bytes : carriers) {
    const std::string value(bytes.begin(), bytes.end());
    for (const std::string_view forbidden :
         {"SELECT", "INSERT", "customer", "password", "credential",
          "source_locator", "parser_branch"}) {
      Require(value.find(forbidden) == std::string::npos,
              "binary plan-import carrier leaked forbidden text");
    }
  }
}

}  // namespace

int main() {
  Bytes source_bytes;
  Bytes format_bytes;
  Bytes mapping_bytes;
  Bytes policy_bytes;
  Bytes descriptor_bytes;
  const auto carriers = CanonicalCarriers(
      &source_bytes, &format_bytes, &mapping_bytes, &policy_bytes,
      &descriptor_bytes);

  RequireDescriptorRefContract(carriers.descriptor);
  RequirePlanAndChildRoundTrips(carriers, source_bytes, format_bytes,
                                mapping_bytes, policy_bytes, descriptor_bytes);
  RequireMalformedCarrierRefusals(carriers, source_bytes, format_bytes,
                                  mapping_bytes, policy_bytes, descriptor_bytes);
  RequireMappingAndPolicyRules(carriers);
  RequireCarrierBindingsAndEnums(carriers);
  RequireLiveAuthorityAndEvidence(carriers.descriptor);
#ifndef SCRATCHBIRD_PLAN_IMPORT_ROWS_CODEC_ONLY
  RequireEnvelopeWhitelist(carriers.descriptor);
#endif
  RequireNoForbiddenText({source_bytes, format_bytes, mapping_bytes,
                          policy_bytes, descriptor_bytes});
  std::cout << "plan_import_rows_codec_v1=passed\n";
  return EXIT_SUCCESS;
}
