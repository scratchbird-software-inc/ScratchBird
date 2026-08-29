// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Exact component codec for SBLR-QUERY-NARROW-PROFILES-V1 / SBQNPB01.
// The SHA-256 fields are deterministic evidence, not authentication.  The
// caller supplies already-authenticated live statement authority through the
// validation context.

#include "runtime_platform.hpp"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::wire {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u8;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u16 kNarrowQueryBindingVersion = 1;
inline constexpr u16 kNarrowQueryBindingHeaderBytes = 472;
inline constexpr u32 kNarrowQuerySourcePrefixBytes = 136;
inline constexpr u32 kNarrowQueryOutputPrefixBytes = 184;
inline constexpr u32 kNarrowQueryOrderingRecordBytes = 112;
inline constexpr u32 kNarrowQueryResultBoundBytes = 16;
inline constexpr u64 kNarrowQueryMaximumCarrierBytes = 16ull * 1024ull * 1024ull;
inline constexpr u64 kNarrowQueryMaximumExplicitResultRows = 1024ull * 1024ull;
inline constexpr u64 kNarrowQueryMinimumMgaRelationDecodedBytesPerPass =
    64ull * 1024ull;
inline constexpr u64 kNarrowQueryMaximumMgaRelationDecodedBytesPerPass =
    1024ull * 1024ull * 1024ull * 1024ull;
inline constexpr u16 kNarrowQueryMaximumSources = 9;
inline constexpr u32 kNarrowQueryMaximumOutputs = 16384;
inline constexpr u32 kNarrowQueryMaximumOrderingTerms = 64;

using NarrowQueryUuid = std::array<byte, 16>;
using NarrowQueryHash = std::array<byte, 32>;

enum class NarrowQueryProfile : u16 {
  ordered_projection = 1,
  projection_occurrence = 2,
  alias_distinct_self_join = 3,
};

enum class NarrowQueryDirection : u8 {
  ascending = 1,
  descending = 2,
};

enum class NarrowQueryNullPlacement : u8 {
  first = 1,
  last = 2,
};

// Field-specific categories are stable integration data.  Producers and
// consumers map the accompanying canonical diagnostic without parsing detail
// strings.
enum class NarrowQueryBindingErrorCode : u16 {
  ok = 0,
  invalid_argument,
  magic_invalid,
  version_invalid,
  extent_invalid,
  reserved_invalid,
  profile_invalid,
  count_invalid,
  result_bound_invalid,
  resource_limit_exceeded,
  carrier_evidence_mismatch,
  source_vector_evidence_mismatch,
  output_vector_evidence_mismatch,
  ordering_vector_evidence_mismatch,
  validation_context_invalid,
  statement_receipt_mismatch,
  transaction_invalid,
  transaction_stale,
  snapshot_mismatch,
  catalog_mismatch,
  security_mismatch,
  resource_grant_mismatch,
  cancellation_receipt_mismatch,
  result_handle_mismatch,
  source_record_invalid,
  source_identity_invalid,
  source_alias_invalid,
  source_authority_stale,
  source_unauthorized,
  output_record_invalid,
  output_identity_invalid,
  output_source_invalid,
  output_datatype_invalid,
  output_unauthorized,
  ordering_record_invalid,
  ordering_identity_invalid,
  ordering_source_invalid,
  ordering_collation_invalid,
  profile_shape_invalid,
  cancelled,
  hash_failure,
};

struct NarrowQueryBindingError {
  NarrowQueryBindingErrorCode code = NarrowQueryBindingErrorCode::ok;
  std::string diagnostic_code;
  std::string field;
  u32 record_index = 0;
  std::string detail;

  bool ok() const { return code == NarrowQueryBindingErrorCode::ok; }
};

enum class NarrowQueryAuthorityDecision : u8 {
  accepted = 0,
  stale_or_mismatched,
  hidden_or_unauthorized,
};

struct NarrowQuerySourceOccurrence {
  u32 source_ordinal = 0;
  NarrowQueryUuid source_occurrence_uuid{};
  u64 source_occurrence_generation = 0;
  NarrowQueryUuid relation_descriptor_uuid{};
  u64 relation_descriptor_generation = 0;
  NarrowQueryUuid relation_object_uuid{};
  NarrowQueryUuid schema_uuid{};
  u64 validated_resource_epoch = 0;
  NarrowQueryHash relation_projection_sha256{};
  std::string alias;
};

struct NarrowQueryOutputOccurrence {
  u32 output_ordinal = 0;
  u32 name_occurrence = 0;
  NarrowQueryUuid output_occurrence_uuid{};
  u64 output_occurrence_generation = 0;
  NarrowQueryUuid source_occurrence_uuid{};
  u64 source_occurrence_generation = 0;
  NarrowQueryUuid source_column_uuid{};
  u32 source_column_ordinal = 0;
  NarrowQueryUuid output_descriptor_uuid{};
  u64 output_descriptor_generation = 0;
  NarrowQueryUuid datatype_descriptor_uuid{};
  u64 datatype_descriptor_generation = 0;
  NarrowQueryUuid datatype_type_uuid{};
  u64 datatype_type_generation = 0;
  u32 datatype_binary_type_code = 0;
  u16 codec_version = 0;
  u8 nullability = 0;
  u8 null_encoding = 1;
  u64 codec_generation = 0;
  u32 canonical_value_bytes = 0;
  std::string name;
  std::string codec_id;
};

struct NarrowQueryOrderingTerm {
  u32 term_ordinal = 0;
  NarrowQueryUuid ordering_term_uuid{};
  u64 ordering_term_generation = 0;
  NarrowQueryUuid source_occurrence_uuid{};
  u64 source_occurrence_generation = 0;
  NarrowQueryUuid source_column_uuid{};
  u32 source_column_ordinal = 0;
  NarrowQueryDirection direction = NarrowQueryDirection::ascending;
  NarrowQueryNullPlacement null_placement = NarrowQueryNullPlacement::first;
  NarrowQueryUuid collation_uuid{};
  u64 collation_generation = 0;
};

struct NarrowQueryBinding {
  NarrowQueryProfile profile = NarrowQueryProfile::ordered_projection;
  NarrowQueryUuid statement_receipt_uuid{};
  NarrowQueryUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  NarrowQueryUuid statement_snapshot_uuid{};
  NarrowQueryUuid datatype_catalog_snapshot_uuid{};
  u64 datatype_catalog_generation = 0;
  u64 datatype_registry_generation = 0;
  NarrowQueryUuid security_context_uuid{};
  NarrowQueryUuid policy_snapshot_uuid{};
  u64 policy_generation = 0;
  NarrowQueryUuid resource_grant_receipt_uuid{};
  u64 resource_grant_generation = 0;
  NarrowQueryUuid cancellation_receipt_uuid{};
  u64 cancellation_generation = 0;
  NarrowQueryUuid execution_uuid{};
  NarrowQueryUuid result_set_uuid{};
  NarrowQueryUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  NarrowQueryUuid source_vector_uuid{};
  u64 source_vector_generation = 0;
  NarrowQueryHash source_vector_sha256{};
  NarrowQueryUuid output_vector_uuid{};
  u64 output_vector_generation = 0;
  NarrowQueryHash output_vector_sha256{};
  NarrowQueryUuid ordering_vector_uuid{};
  u64 ordering_vector_generation = 0;
  NarrowQueryHash ordering_vector_sha256{};
  NarrowQueryHash descriptor_evidence_sha256{};
  // Mandatory exact retained statement resource-policy value.  It is applied
  // independently to each source occurrence and each physical MGA decode pass
  // and is not a result, carrier, optimizer-memory, I/O, or LIMIT/OFFSET bound.
  u64 maximum_mga_relation_decoded_bytes_per_pass = 0;
  bool row_limit_present = false;
  u64 row_limit = 0;
  bool row_offset_present = false;
  u64 row_offset = 0;
  std::vector<NarrowQuerySourceOccurrence> sources;
  std::vector<NarrowQueryOutputOccurrence> outputs;
  std::vector<NarrowQueryOrderingTerm> ordering_terms;
};

struct NarrowQueryBindingValidationContext {
  NarrowQueryUuid statement_receipt_uuid{};
  NarrowQueryUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  NarrowQueryUuid statement_snapshot_uuid{};
  NarrowQueryUuid datatype_catalog_snapshot_uuid{};
  u64 datatype_catalog_generation = 0;
  u64 datatype_registry_generation = 0;
  NarrowQueryUuid security_context_uuid{};
  NarrowQueryUuid policy_snapshot_uuid{};
  u64 policy_generation = 0;
  NarrowQueryUuid resource_grant_receipt_uuid{};
  u64 resource_grant_generation = 0;
  NarrowQueryUuid cancellation_receipt_uuid{};
  u64 cancellation_generation = 0;
  NarrowQueryUuid execution_uuid{};
  NarrowQueryUuid result_set_uuid{};
  NarrowQueryUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  u64 maximum_total_bytes = kNarrowQueryMaximumCarrierBytes;
  // Exact nonzero value retained by the live statement/resource grant.
  u64 maximum_mga_relation_decoded_bytes_per_pass = 0;
  // Exact live retained-grant row ceiling. Zero is not an admitted default.
  u64 maximum_result_rows = 0;
  bool cancelled = false;

  // Required fail-closed authority hooks.  Alias validation owns canonical
  // UTF-8/NFC identifier policy; the codec itself validates UTF-8 and NUL.
  std::function<bool(std::string_view)> validate_canonical_alias;
  std::function<NarrowQueryAuthorityDecision(
      const NarrowQuerySourceOccurrence&)> validate_source;
  std::function<NarrowQueryAuthorityDecision(
      const NarrowQueryOutputOccurrence&)> validate_output_datatype;
  std::function<NarrowQueryAuthorityDecision(
      const NarrowQueryOrderingTerm&)> validate_collation;
};

// Encodes a structurally exact binding and computes all vector/carrier hashes.
// Live statement authority is deliberately not created or checked by encode.
bool EncodeNarrowQueryBinding(const NarrowQueryBinding& binding,
                              std::vector<byte>* encoded,
                              NarrowQueryBindingError* error);

// Decodes, verifies every byte/hash/profile invariant, and compares all live
// context/authority fields before returning a binding.  `decoded` is unchanged
// on refusal.
bool DecodeAndValidateNarrowQueryBinding(
    std::span<const byte> encoded,
    const NarrowQueryBindingValidationContext& context,
    NarrowQueryBinding* decoded,
    NarrowQueryBindingError* error);

const char* NarrowQueryBindingErrorCodeName(NarrowQueryBindingErrorCode code);

}  // namespace scratchbird::wire
