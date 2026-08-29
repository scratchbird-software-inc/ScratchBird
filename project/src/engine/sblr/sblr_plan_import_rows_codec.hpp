// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

// SEARCH_KEY: SB_ENGINE_SBLR_PLAN_IMPORT_ROWS_CODEC_V1
// Pure binary codec for the manifest-authorized SBOP/IPLP/ISRC/IFMT/IMAP/
// IPOL/IPEV carriers. Decoded values are claims only; this codec grants no
// catalog, security, MGA, route, execution, mutation, or finality authority.

using PlanImportRowsUuidV1 = std::array<std::uint8_t, 16>;
using PlanImportRowsSha256V1 = std::array<std::uint8_t, 32>;

inline constexpr std::uint16_t kPlanImportRowsOpcodeCodeV1 = 793;
inline constexpr std::size_t kPlanImportRowsDescriptorRefBytesV1 = 24;
inline constexpr std::size_t kPlanImportRowsChildRefBytesV1 = 56;
inline constexpr std::size_t kPlanImportRowsPlanDescriptorBytesV1 = 512;
inline constexpr std::size_t kPlanImportRowsNestedHeaderBytesV1 = 104;
inline constexpr std::size_t kPlanImportRowsSourcePayloadBytesV1 = 64;
inline constexpr std::size_t kPlanImportRowsFormatPayloadBytesV1 = 8;
inline constexpr std::size_t kPlanImportRowsMappingRecordBytesV1 = 96;
inline constexpr std::size_t kPlanImportRowsPolicyPayloadBytesV1 = 72;
inline constexpr std::size_t kPlanImportRowsExecutorEvidenceBytesV1 = 208;
inline constexpr std::uint32_t kPlanImportRowsMaximumMappingsV1 = 262144;
inline constexpr std::uint64_t kPlanImportRowsAcceptedValidationBitsV1 =
    0x00000000000003ffULL;

inline constexpr std::uint16_t kPlanImportRowsSourceFingerprintPresentV1 =
    0x0001;
inline constexpr std::uint32_t kPlanImportRowsMappingRequiredV1 = 0x00000001;
inline constexpr std::uint8_t kPlanImportRowsStrictBulkLoadRequestedV1 = 0x01;
inline constexpr std::uint8_t
    kPlanImportRowsReferenceRelaxedSemanticsRequestedV1 = 0x02;

enum class PlanImportRowsSourceKindV1 : std::uint16_t {
  native_sbsql_import = 1,
  csv_stream = 2,
  delimited_text = 3,
  fixed_width_text = 4,
  jsonl_stream = 5,
  document_stream = 6,
  binary_typed_rows = 7,
  reference_dump_replay = 8,
  reference_bulk_api = 9,
  live_ingest_stream = 10,
  bulk_import_job = 11,
  xml_stream = 12,
  line_protocol_stream = 13,
};

enum class PlanImportRowsFormatFamilyV1 : std::uint16_t {
  csv = 1,
  delimited_text = 2,
  fixed_width = 3,
  jsonl = 4,
  document = 5,
  binary_typed_rows = 6,
  reference_dump = 7,
  reference_bulk = 8,
  live_ingest = 9,
  xml = 10,
  line_protocol = 11,
  bulk_job = 12,
};

enum class PlanImportRowsInsertModeV1 : std::uint16_t {
  copy_import = 1,
  native_bulk = 2,
  reference_bulk = 3,
};

enum class PlanImportRowsRejectModeV1 : std::uint8_t {
  fail_fast = 1,
  reject_row = 2,
  reject_table = 3,
  quarantine = 4,
};

enum class PlanImportRowsRejectPayloadPolicyV1 : std::uint8_t {
  diagnostic_only = 1,
  redacted_payload_reference = 2,
  encrypted_payload_reference = 3,
};

enum class PlanImportRowsResumePolicyV1 : std::uint8_t {
  fail_closed = 1,
  resume_from_checkpoint = 2,
  operator_review_required = 3,
};

struct PlanImportRowsCodecDiagnosticV1 {
  std::string code;
  std::string detail;
};

struct PlanImportRowsDescriptorRefV1 {
  PlanImportRowsUuidV1 descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
};

struct PlanImportRowsChildRefV1 {
  PlanImportRowsUuidV1 row_uuid{};
  std::uint64_t row_generation = 0;
  PlanImportRowsSha256V1 canonical_row_sha256{};
};

struct PlanImportRowsNestedHeaderV1 {
  PlanImportRowsUuidV1 row_uuid{};
  std::uint64_t row_generation = 0;
  PlanImportRowsUuidV1 owner_plan_descriptor_uuid{};
  std::uint64_t owner_plan_descriptor_generation = 0;
  PlanImportRowsSha256V1 row_sha256{};
};

struct PlanImportRowsPlanDescriptorV1 {
  PlanImportRowsUuidV1 descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  PlanImportRowsUuidV1 request_uuid{};
  PlanImportRowsUuidV1 authenticated_statement_receipt_uuid{};
  std::uint64_t structural_occurrence_id = 0;
  std::uint64_t local_transaction_id = 0;
  PlanImportRowsUuidV1 transaction_uuid{};
  PlanImportRowsUuidV1 mga_snapshot_uuid{};
  std::uint64_t mga_snapshot_generation = 0;
  PlanImportRowsUuidV1 security_snapshot_uuid{};
  std::uint64_t security_snapshot_generation = 0;
  PlanImportRowsUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_snapshot_generation = 0;
  PlanImportRowsUuidV1 resource_admission_uuid{};
  std::uint64_t resource_admission_generation = 0;
  std::uint64_t catalog_generation = 0;
  PlanImportRowsUuidV1 target_table_uuid{};
  PlanImportRowsUuidV1 target_relation_descriptor_uuid{};
  std::uint64_t target_relation_descriptor_generation = 0;
  PlanImportRowsChildRefV1 import_source_envelope_ref{};
  PlanImportRowsChildRefV1 import_format_envelope_ref{};
  PlanImportRowsChildRefV1 column_mapping_vector_ref{};
  PlanImportRowsChildRefV1 import_policy_ref{};
  std::uint64_t executor_availability_generation = 0;
  PlanImportRowsSha256V1 descriptor_evidence_sha256{};
  std::vector<std::uint8_t> exact_bytes;
};

struct PlanImportRowsSourceEnvelopeV1 {
  PlanImportRowsNestedHeaderV1 header{};
  PlanImportRowsSourceKindV1 source_kind =
      static_cast<PlanImportRowsSourceKindV1>(0);
  std::uint16_t flags = 0;
  PlanImportRowsUuidV1 engine_source_binding_uuid{};
  std::uint64_t engine_source_binding_generation = 0;
  PlanImportRowsSha256V1 source_fingerprint_sha256{};
  std::vector<std::uint8_t> exact_bytes;
};

struct PlanImportRowsFormatEnvelopeV1 {
  PlanImportRowsNestedHeaderV1 header{};
  PlanImportRowsFormatFamilyV1 format_family =
      static_cast<PlanImportRowsFormatFamilyV1>(0);
  std::vector<std::uint8_t> exact_bytes;
};

struct PlanImportRowsMappingRecordV1 {
  std::uint32_t source_field_ordinal = 0;
  std::uint32_t flags = 0;
  PlanImportRowsUuidV1 target_column_uuid{};
  std::uint64_t target_column_generation = 0;
  PlanImportRowsUuidV1 target_datatype_descriptor_uuid{};
  std::uint64_t target_datatype_descriptor_generation = 0;
  PlanImportRowsUuidV1 target_type_uuid{};
  std::uint64_t target_type_generation = 0;
  std::uint16_t codec_id = 0;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
};

struct PlanImportRowsMappingVectorV1 {
  PlanImportRowsNestedHeaderV1 header{};
  std::vector<PlanImportRowsMappingRecordV1> mappings;
  std::vector<std::uint8_t> exact_bytes;
};

struct PlanImportRowsPolicyV1 {
  PlanImportRowsNestedHeaderV1 header{};
  PlanImportRowsRejectModeV1 reject_mode =
      static_cast<PlanImportRowsRejectModeV1>(0);
  PlanImportRowsRejectPayloadPolicyV1 reject_payload_policy =
      static_cast<PlanImportRowsRejectPayloadPolicyV1>(0);
  PlanImportRowsResumePolicyV1 resume_policy =
      static_cast<PlanImportRowsResumePolicyV1>(0);
  std::uint8_t flags = 0;
  std::uint32_t reject_limit_ppm = 0;
  std::uint64_t reject_limit_rows = 0;
  PlanImportRowsUuidV1 reject_target_relation_uuid{};
  std::uint64_t reject_target_relation_generation = 0;
  PlanImportRowsSha256V1 reject_target_relation_sha256{};
  std::vector<std::uint8_t> exact_bytes;
};

struct PlanImportRowsExecutorEvidenceV1 {
  PlanImportRowsUuidV1 evidence_uuid{};
  std::uint64_t evidence_generation = 0;
  PlanImportRowsUuidV1 request_descriptor_uuid{};
  std::uint64_t request_descriptor_generation = 0;
  PlanImportRowsSha256V1 request_projection_sha256{};
  std::uint64_t executor_availability_generation = 0;
  PlanImportRowsUuidV1 transaction_uuid{};
  std::uint64_t local_transaction_id = 0;
  PlanImportRowsUuidV1 mga_snapshot_uuid{};
  std::uint64_t mga_snapshot_generation = 0;
  std::uint64_t completed_validation_bits = 0;
  PlanImportRowsSha256V1 evidence_sha256{};
  std::vector<std::uint8_t> exact_bytes;
};

struct PlanImportRowsCarrierSetV1 {
  PlanImportRowsPlanDescriptorV1 descriptor{};
  PlanImportRowsSourceEnvelopeV1 source{};
  PlanImportRowsFormatEnvelopeV1 format{};
  PlanImportRowsMappingVectorV1 mapping{};
  PlanImportRowsPolicyV1 policy{};
};

struct PlanImportRowsLiveAuthorityV1 {
  PlanImportRowsUuidV1 authenticated_statement_receipt_uuid{};
  std::uint64_t structural_occurrence_id = 0;
  std::uint64_t local_transaction_id = 0;
  PlanImportRowsUuidV1 transaction_uuid{};
  PlanImportRowsUuidV1 mga_snapshot_uuid{};
  std::uint64_t mga_snapshot_generation = 0;
  PlanImportRowsUuidV1 security_snapshot_uuid{};
  std::uint64_t security_snapshot_generation = 0;
  PlanImportRowsUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_snapshot_generation = 0;
  PlanImportRowsUuidV1 resource_admission_uuid{};
  std::uint64_t resource_admission_generation = 0;
  std::uint64_t catalog_generation = 0;
  PlanImportRowsUuidV1 target_table_uuid{};
  PlanImportRowsUuidV1 target_relation_descriptor_uuid{};
  std::uint64_t target_relation_descriptor_generation = 0;
  std::uint64_t executor_availability_generation = 0;
};

bool EncodePlanImportRowsDescriptorRefV1(
    const PlanImportRowsDescriptorRefV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsDescriptorRefV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsDescriptorRefV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool EncodePlanImportRowsPlanDescriptorV1(
    const PlanImportRowsPlanDescriptorV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsPlanDescriptorV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsPlanDescriptorV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool EncodePlanImportRowsSourceEnvelopeV1(
    const PlanImportRowsSourceEnvelopeV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsSourceEnvelopeV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsSourceEnvelopeV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool EncodePlanImportRowsFormatEnvelopeV1(
    const PlanImportRowsFormatEnvelopeV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsFormatEnvelopeV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsFormatEnvelopeV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool EncodePlanImportRowsMappingVectorV1(
    const PlanImportRowsMappingVectorV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsMappingVectorV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsMappingVectorV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool EncodePlanImportRowsPolicyV1(
    const PlanImportRowsPolicyV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsPolicyV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsPolicyV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool EncodePlanImportRowsExecutorEvidenceV1(
    const PlanImportRowsExecutorEvidenceV1& value,
    std::vector<std::uint8_t>* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool DecodePlanImportRowsExecutorEvidenceV1(
    const std::uint8_t* bytes,
    std::size_t size,
    PlanImportRowsExecutorEvidenceV1* out,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool ValidatePlanImportRowsDescriptorReferenceV1(
    const PlanImportRowsDescriptorRefV1& reference,
    const PlanImportRowsPlanDescriptorV1& descriptor,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool ValidatePlanImportRowsCarrierSetV1(
    const PlanImportRowsCarrierSetV1& carriers,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool ValidatePlanImportRowsPolicyAdmissionV1(
    const PlanImportRowsPolicyV1& policy,
    bool reference_relaxed_semantics_authorized,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool ValidatePlanImportRowsLiveAuthorityV1(
    const PlanImportRowsPlanDescriptorV1& descriptor,
    const PlanImportRowsLiveAuthorityV1& authority,
    PlanImportRowsCodecDiagnosticV1* diagnostic);
bool ValidatePlanImportRowsExecutorEvidenceBindingV1(
    const PlanImportRowsExecutorEvidenceV1& evidence,
    const PlanImportRowsPlanDescriptorV1& descriptor,
    PlanImportRowsCodecDiagnosticV1* diagnostic);

bool IsPlanImportRowsSourceFormatPairAdmittedV1(
    PlanImportRowsSourceKindV1 source_kind,
    PlanImportRowsFormatFamilyV1 format_family) noexcept;
PlanImportRowsInsertModeV1 NormalizePlanImportRowsInsertModeV1(
    PlanImportRowsSourceKindV1 source_kind) noexcept;

}  // namespace scratchbird::engine::sblr
