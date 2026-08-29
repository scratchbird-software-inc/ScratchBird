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

// SEARCH_KEY: SB_ENGINE_CONTEXTUAL_TEXT_LITERAL_V2_CODEC
// Pure codec for the manifest-authorized SBTLNR02/SBTLPR02/SBTLNS02/
// SBTLXE02/SBTLTD02 carriers.  These types contain bytes and decoded claims;
// none of them is catalog, receipt, route, or execution authority.

using ContextualTextUuidV2 = std::array<std::uint8_t, 16>;
using ContextualTextSha256V2 = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kContextualTextMaximumProfileCountV2 = 64;
inline constexpr std::size_t kContextualTextMaximumRawTokenBytesV2 = 32768;
inline constexpr std::size_t kContextualTextMaximumBodyBytesV2 = 32563;
inline constexpr std::size_t kContextualTextMaximumLogicalCarrierBytesV2 =
    65536;
inline constexpr std::size_t kContextualTextMaximumProfileBytesV2 = 33496;
inline constexpr std::size_t kContextualTextRequestHeaderBytesV2 = 192;
inline constexpr std::size_t kContextualTextDemandPrefixBytesV2 = 216;
inline constexpr std::size_t kContextualTextProfileHeaderBytesV2 = 912;
inline constexpr std::size_t kContextualTextResultHeaderBytesV2 = 336;
inline constexpr std::size_t kContextualTextExecuteHeaderBytesV2 = 400;
inline constexpr std::size_t kContextualTextMappingPrefixBytesV2 = 56;
inline constexpr std::size_t kContextualTextDescriptorBytesV2 = 533;
inline constexpr std::size_t kContextualTextCodecIdentifierBytesV2 = 21;
inline constexpr char kContextualTextCodecIdentifierV2[] =
    "datatype.text.utf8.v1";

struct ContextualTextCodecDiagnosticV2 {
  std::string code;
  std::string detail;
};

struct ContextualTextRawTokenV2 {
  std::vector<std::uint8_t> decoded_utf8;
  std::uint32_t scalar_count = 0;
};

bool DecodeContextualTextRawTokenV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextRawTokenV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic);

struct ContextualTextLiteralDemandV2 {
  std::uint64_t literal_occurrence = 0;
  std::uint8_t literal_argument_ordinal = 0;
  std::uint8_t target_argument_ordinal = 0;
  std::uint64_t comparison_occurrence = 0;
  std::uint64_t source_node_id = 0;
  std::uint32_t source_operand_ordinal = 0;
  std::uint32_t source_ordinal = 0;
  ContextualTextUuidV2 relation_uuid{};
  ContextualTextUuidV2 relation_descriptor_uuid{};
  std::uint64_t relation_descriptor_generation = 0;
  ContextualTextUuidV2 column_uuid{};
  std::uint32_t column_ordinal = 0;
  std::uint32_t parent_operand_ordinal = 0;
  std::uint64_t node_id = 0;
  std::uint32_t target_descriptor_handle = 0;
  std::uint32_t literal_descriptor_handle = 0;
  std::uint32_t scalar_count = 0;
  ContextualTextSha256V2 raw_token_sha256{};
  ContextualTextSha256V2 lexical_value_sha256{};
  std::vector<std::uint8_t> raw_token;
  std::vector<std::uint8_t> lexical_value;
};

struct ContextualTextLiteralNegotiationRequestV2 {
  ContextualTextUuidV2 statement_receipt_uuid{};
  ContextualTextUuidV2 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t resource_epoch = 0;
  ContextualTextUuidV2 mga_snapshot_uuid{};
  ContextualTextSha256V2 demand_sequence_sha256{};
  std::vector<ContextualTextLiteralDemandV2> demands;
};

struct ContextualTextDescriptorV2 {
  std::uint32_t flags = 0;
  std::uint8_t malformed_sequence_policy = 0;
  std::uint8_t null_encoding = 0;
  ContextualTextUuidV2 descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  ContextualTextUuidV2 type_uuid{};
  std::uint64_t type_generation = 0;
  ContextualTextUuidV2 codec_uuid{};
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::uint64_t character_limit = 0;
  std::uint64_t byte_limit = 0;
  ContextualTextUuidV2 charset_uuid{};
  std::uint64_t charset_generation = 0;
  ContextualTextUuidV2 collation_uuid{};
  std::uint64_t collation_generation = 0;
  ContextualTextUuidV2 normalization_policy_uuid{};
  std::uint64_t normalization_policy_generation = 0;
  ContextualTextUuidV2 padding_policy_uuid{};
  std::uint64_t padding_policy_generation = 0;
  ContextualTextUuidV2 case_accent_policy_uuid{};
  std::uint64_t case_accent_policy_generation = 0;
  ContextualTextUuidV2 render_policy_uuid{};
  std::uint64_t render_policy_generation = 0;
  ContextualTextUuidV2 canonicalization_profile_uuid{};
  std::uint64_t canonicalization_profile_generation = 0;
  ContextualTextUuidV2 comparison_contract_uuid{};
  std::uint64_t comparison_contract_generation = 0;
  ContextualTextUuidV2 equality_operation_uuid{};
  std::uint64_t equality_operation_generation = 0;
  ContextualTextUuidV2 datatype_catalog_snapshot_uuid{};
  std::uint64_t datatype_catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::uint64_t resource_epoch = 0;
  ContextualTextSha256V2 descriptor_evidence_sha256{};
  std::vector<std::uint8_t> exact_bytes;
};

struct ContextualTextLiteralProfileV2 {
  ContextualTextUuidV2 profile_uuid{};
  ContextualTextUuidV2 profile_set_uuid{};
  std::uint64_t profile_set_generation = 0;
  ContextualTextUuidV2 literal_binding_uuid{};
  std::uint64_t literal_binding_generation = 0;
  std::uint64_t literal_occurrence = 0;
  std::uint64_t node_id = 0;
  std::uint64_t comparison_occurrence = 0;
  ContextualTextUuidV2 statement_receipt_uuid{};
  ContextualTextUuidV2 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t resource_epoch = 0;
  ContextualTextUuidV2 mga_snapshot_uuid{};
  ContextualTextUuidV2 descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  ContextualTextUuidV2 type_uuid{};
  std::uint64_t type_generation = 0;
  ContextualTextUuidV2 codec_uuid{};
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::uint8_t literal_argument_ordinal = 0;
  std::uint8_t target_argument_ordinal = 0;
  ContextualTextUuidV2 source_occurrence_uuid{};
  std::uint64_t source_generation = 0;
  ContextualTextUuidV2 relation_uuid{};
  ContextualTextUuidV2 relation_descriptor_uuid{};
  std::uint64_t relation_descriptor_generation = 0;
  ContextualTextUuidV2 column_uuid{};
  std::uint32_t column_ordinal = 0;
  std::uint32_t parent_operand_ordinal = 0;
  std::uint32_t target_descriptor_handle = 0;
  std::uint32_t literal_descriptor_handle = 0;
  std::uint32_t scalar_count = 0;
  std::uint64_t target_character_limit = 0;
  std::uint64_t target_byte_limit = 0;
  ContextualTextUuidV2 charset_uuid{};
  std::uint64_t charset_generation = 0;
  ContextualTextUuidV2 collation_uuid{};
  std::uint64_t collation_generation = 0;
  ContextualTextUuidV2 normalization_policy_uuid{};
  std::uint64_t normalization_policy_generation = 0;
  ContextualTextUuidV2 padding_policy_uuid{};
  std::uint64_t padding_policy_generation = 0;
  ContextualTextUuidV2 case_accent_policy_uuid{};
  std::uint64_t case_accent_policy_generation = 0;
  ContextualTextUuidV2 render_policy_uuid{};
  std::uint64_t render_policy_generation = 0;
  ContextualTextUuidV2 canonicalization_profile_uuid{};
  std::uint64_t canonicalization_profile_generation = 0;
  ContextualTextUuidV2 comparison_contract_uuid{};
  std::uint64_t comparison_contract_generation = 0;
  ContextualTextUuidV2 equality_operation_uuid{};
  std::uint64_t equality_operation_generation = 0;
  ContextualTextUuidV2 literal_budget_uuid{};
  std::uint64_t literal_budget_generation = 0;
  std::uint64_t literal_negotiation_byte_grant = 0;
  std::uint64_t canonical_body_aggregate_grant = 0;
  ContextualTextSha256V2 raw_token_sha256{};
  ContextualTextSha256V2 lexical_value_sha256{};
  ContextualTextSha256V2 canonical_body_sha256{};
  ContextualTextSha256V2 target_projection_sha256{};
  ContextualTextSha256V2 descriptor_evidence_sha256{};
  ContextualTextSha256V2 target_context_sha256{};
  ContextualTextSha256V2 demand_sequence_sha256{};
  ContextualTextSha256V2 binding_sha256{};
  std::vector<std::uint8_t> canonical_body;
  std::vector<std::uint8_t> exact_bytes;
};

struct ContextualTextLiteralProfileMappingV2 {
  std::uint64_t literal_occurrence = 0;
  std::uint64_t node_id = 0;
  ContextualTextUuidV2 literal_binding_uuid{};
  std::uint64_t literal_binding_generation = 0;
  std::uint32_t literal_descriptor_handle = 0;
  std::uint32_t target_descriptor_handle = 0;
  ContextualTextLiteralProfileV2 profile;
};

struct ContextualTextLiteralProfileSetV2 {
  ContextualTextUuidV2 statement_receipt_uuid{};
  ContextualTextUuidV2 profile_set_uuid{};
  std::uint64_t profile_set_generation = 0;
  ContextualTextUuidV2 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t resource_epoch = 0;
  ContextualTextUuidV2 mga_snapshot_uuid{};
  ContextualTextUuidV2 literal_budget_uuid{};
  std::uint64_t literal_budget_generation = 0;
  std::uint64_t literal_negotiation_byte_grant = 0;
  std::uint64_t canonical_body_aggregate_grant = 0;
  ContextualTextSha256V2 demand_sequence_sha256{};
  ContextualTextSha256V2 target_context_sequence_sha256{};
  ContextualTextSha256V2 ordered_profiles_sha256{};
  ContextualTextSha256V2 carrier_sha256{};
  std::vector<ContextualTextLiteralProfileMappingV2> mappings;
  std::vector<std::uint8_t> exact_bytes;
};

struct ContextualTextLiteralExecuteV2 : ContextualTextLiteralProfileSetV2 {
  ContextualTextSha256V2 pre_contextual_operand_vector_sha256{};
  ContextualTextSha256V2 sbxn_sha256{};
};

bool DecodeContextualTextLiteralNegotiationRequestV2(
    const std::uint8_t* bytes, std::size_t size,
    ContextualTextLiteralNegotiationRequestV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool EncodeContextualTextLiteralNegotiationRequestV2(
    const ContextualTextLiteralNegotiationRequestV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool DecodeContextualTextDescriptorV2(
    const std::uint8_t* bytes, std::size_t size,
    ContextualTextDescriptorV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool EncodeContextualTextDescriptorV2(
    const ContextualTextDescriptorV2& value, std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool DecodeContextualTextLiteralProfileV2(
    const std::uint8_t* bytes, std::size_t size,
    ContextualTextLiteralProfileV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool EncodeContextualTextLiteralProfileV2(
    const ContextualTextLiteralProfileV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool DecodeContextualTextLiteralProfileSetV2(
    const std::uint8_t* bytes, std::size_t size,
    ContextualTextLiteralProfileSetV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool EncodeContextualTextLiteralProfileSetV2(
    const ContextualTextLiteralProfileSetV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool DecodeContextualTextLiteralExecuteV2(
    const std::uint8_t* bytes, std::size_t size,
    ContextualTextLiteralExecuteV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic);
bool EncodeContextualTextLiteralExecuteV2(
    const ContextualTextLiteralExecuteV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic);

ContextualTextSha256V2 ComputeContextualTextDemandSequenceSha256V2(
    const std::vector<std::uint8_t>& exact_record_vector,
    std::uint32_t count);
ContextualTextSha256V2 ComputeContextualTextRawTokenSha256V2(
    const ContextualTextLiteralDemandV2& demand);
ContextualTextSha256V2 ComputeContextualTextLexicalValueSha256V2(
    const ContextualTextLiteralDemandV2& demand);
ContextualTextSha256V2 ComputeContextualTextCanonicalBodySha256V2(
    const ContextualTextLiteralProfileV2& profile);
ContextualTextSha256V2 ComputeContextualTextTargetProjectionSha256V2(
    const ContextualTextLiteralDemandV2& demand,
    const ContextualTextUuidV2& source_occurrence_uuid,
    std::uint64_t source_generation,
    const std::vector<std::uint8_t>& exact_public_relation_projection_v3);
ContextualTextSha256V2 ComputeContextualTextTargetContextSha256V2(
    const ContextualTextLiteralNegotiationRequestV2& request,
    const ContextualTextLiteralDemandV2& demand,
    const ContextualTextUuidV2& source_occurrence_uuid,
    std::uint64_t source_generation,
    const ContextualTextDescriptorV2& descriptor,
    const ContextualTextSha256V2& target_projection_sha256,
    const ContextualTextSha256V2& descriptor_evidence_sha256);
ContextualTextSha256V2 ComputeContextualTextPreContextualOperandVectorSha256V2(
    const std::vector<std::uint8_t>& exact_top_level_operand_records,
    std::uint32_t operand_count);
ContextualTextSha256V2 ComputeContextualTextDescriptorEvidenceSha256V2(
    const std::vector<std::uint8_t>& exact_descriptor);
ContextualTextSha256V2 ComputeContextualTextBindingSha256V2(
    const std::vector<std::uint8_t>& exact_profile);
ContextualTextSha256V2 ComputeContextualTextResultCarrierSha256V2(
    const std::vector<std::uint8_t>& exact_result);
ContextualTextSha256V2 ComputeContextualTextExecuteCarrierSha256V2(
    const std::vector<std::uint8_t>& exact_execute);
ContextualTextSha256V2 ComputeContextualTextSbxnSha256V2(
    const std::vector<std::uint8_t>& exact_sbxn);

}  // namespace scratchbird::engine::sblr
