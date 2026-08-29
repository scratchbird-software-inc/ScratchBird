// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "narrow_query_binding_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::wire {
namespace {

namespace core_hash = scratchbird::core::hash;

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

constexpr std::array<byte, 8> kMagic{
    {'S', 'B', 'Q', 'N', 'P', 'B', '0', '1'}};
constexpr std::string_view kSourceDomain =
    "ScratchBird.QuerySourceOccurrenceVector.V1";
constexpr std::string_view kOutputDomain =
    "ScratchBird.QueryOutputOccurrenceVector.V1";
constexpr std::string_view kOrderingDomain =
    "ScratchBird.QueryOrderingTermVector.V1";
constexpr std::string_view kCarrierDomain =
    "ScratchBird.QueryNarrowProfileBinding.V1";

constexpr std::size_t kCarrierEvidenceOffset = 440;
constexpr u32 kRowLimitPresentFlag = 1u << 0u;
constexpr u32 kRowOffsetPresentFlag = 1u << 1u;
constexpr u32 kResultBoundFlags =
    kRowLimitPresentFlag | kRowOffsetPresentFlag;
constexpr u32 kMaximumAliasBytes = 128;
constexpr u32 kMaximumNameBytes = 4096;
constexpr u32 kMaximumCodecIdBytes = 255;

constexpr const char* kOperandInvalid = "SBLR.OPERAND.INVALID";
constexpr const char* kSecurityDenied = "SECURITY.ACCESS_DENIED";
constexpr const char* kTransactionInvalid = "MGA.TRANSACTION.INVALID";
constexpr const char* kTransactionStale = "MGA.TRANSACTION.STALE";
constexpr const char* kPlanInvalid = "SBLR.PLAN_TREE.INVALID_HANDLE";
constexpr const char* kDatatypeInvalid = "DATATYPE.DESCRIPTOR_INVALID";
constexpr const char* kProjectionExpressionInvalid =
    "PROJECTION.EXPRESSION_VECTOR_INVALID";
constexpr const char* kProjectionOutputInvalid =
    "PROJECTION.OUTPUT_ROWSET_INVALID";
constexpr const char* kOrderingInvalid = "SORT.ORDERING_VECTOR_INVALID";
constexpr const char* kCollationInvalid = "SORT.COLLATION_PROFILE_INVALID";
constexpr const char* kResultShapeInvalid = "RESULT_SET.SHAPE_INVALID";
constexpr const char* kResourceExceeded = "RESOURCE.BUDGET_EXCEEDED";
constexpr const char* kCancelled = "PROCESS.CANCELLED";

bool Fail(NarrowQueryBindingError* error,
          NarrowQueryBindingErrorCode code,
          std::string diagnostic,
          std::string field,
          u32 record_index,
          std::string detail) {
  if (error != nullptr) {
    error->code = code;
    error->diagnostic_code = std::move(diagnostic);
    error->field = std::move(field);
    error->record_index = record_index;
    error->detail = std::move(detail);
  }
  return false;
}

void ClearError(NarrowQueryBindingError* error) {
  if (error != nullptr) {
    *error = {};
  }
}

bool UuidPresent(const NarrowQueryUuid& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool HashPresent(const NarrowQueryHash& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool SameUuid(const NarrowQueryUuid& left, const NarrowQueryUuid& right) {
  return left == right;
}

bool SameHash(const NarrowQueryHash& left, const NarrowQueryHash& right) {
  return left == right;
}

bool ValidProfile(NarrowQueryProfile profile) {
  switch (profile) {
    case NarrowQueryProfile::ordered_projection:
    case NarrowQueryProfile::projection_occurrence:
    case NarrowQueryProfile::alias_distinct_self_join:
      return true;
  }
  return false;
}

bool ValidDirection(NarrowQueryDirection direction) {
  return direction == NarrowQueryDirection::ascending ||
         direction == NarrowQueryDirection::descending;
}

bool ValidNullPlacement(NarrowQueryNullPlacement placement) {
  return placement == NarrowQueryNullPlacement::first ||
         placement == NarrowQueryNullPlacement::last;
}

bool ValidUtf8NoNul(std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first == 0) {
      return false;
    }
    if (first <= 0x7fu) {
      ++offset;
      continue;
    }
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
      continuation_count = 1;
      code_point = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
      continuation_count = 2;
      code_point = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      continuation_count = 3;
      code_point = first & 0x07u;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1u) {
      return false;
    }
    for (std::size_t index = 0; index < continuation_count; ++index) {
      const auto next =
          static_cast<unsigned char>(value[offset + index + 1u]);
      if ((next & 0xc0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6u) | (next & 0x3fu);
    }
    if ((continuation_count == 2u && code_point < 0x800u) ||
        (continuation_count == 3u && code_point < 0x10000u) ||
        (code_point >= 0xd800u && code_point <= 0xdfffu) ||
        code_point > 0x10ffffu) {
      return false;
    }
    offset += continuation_count + 1u;
  }
  return true;
}

bool ValidCodecId(std::string_view value) {
  if (value.empty() || value.size() > kMaximumCodecIdBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-' || character == ':';
  });
}

bool AddWithin(u64 left, u64 right, u64 maximum, u64* result) {
  if (right > maximum || left > maximum - right) {
    return false;
  }
  *result = left + right;
  return true;
}

bool ValidMgaRelationDecodedBytesPerPass(u64 value) {
  return value >= kNarrowQueryMinimumMgaRelationDecodedBytesPerPass &&
         value <= kNarrowQueryMaximumMgaRelationDecodedBytesPerPass;
}

void StoreUuid(std::vector<byte>* bytes,
               std::size_t offset,
               const NarrowQueryUuid& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

void StoreHash(std::vector<byte>* bytes,
               std::size_t offset,
               const NarrowQueryHash& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

NarrowQueryUuid LoadUuid(std::span<const byte> bytes, std::size_t offset) {
  NarrowQueryUuid value{};
  std::copy_n(bytes.begin() + offset, value.size(), value.begin());
  return value;
}

NarrowQueryHash LoadHash(std::span<const byte> bytes, std::size_t offset) {
  NarrowQueryHash value{};
  std::copy_n(bytes.begin() + offset, value.size(), value.begin());
  return value;
}

void AppendU32(std::vector<byte>* bytes, u32 value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 4u);
  StoreLittle32(bytes->data() + offset, value);
}

void AppendU64(std::vector<byte>* bytes, u64 value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 8u);
  StoreLittle64(bytes->data() + offset, value);
}

void AppendUuid(std::vector<byte>* bytes, const NarrowQueryUuid& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendHash(std::vector<byte>* bytes, const NarrowQueryHash& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendString(std::vector<byte>* bytes, std::string_view value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

bool ComputeEvidence(std::string_view domain,
                     std::span<const byte> material_bytes,
                     NarrowQueryHash* result,
                     NarrowQueryBindingError* error,
                     std::string field) {
  std::vector<byte> material;
  material.reserve(domain.size() + material_bytes.size());
  for (const char character : domain) {
    material.push_back(static_cast<byte>(character));
  }
  material.insert(material.end(), material_bytes.begin(), material_bytes.end());
  const auto digest = core_hash::ComputeSha256Digest(material);
  if (!digest.ok()) {
    return Fail(error, NarrowQueryBindingErrorCode::hash_failure,
                kOperandInvalid, std::move(field), 0,
                "SHA-256 computation failed");
  }
  std::copy(digest.digest.begin(), digest.digest.end(), result->begin());
  return true;
}

bool ComputeVectorEvidence(std::string_view domain,
                           u32 count,
                           std::span<const byte> records,
                           NarrowQueryHash* result,
                           NarrowQueryBindingError* error,
                           std::string field) {
  std::vector<byte> framed;
  framed.resize(4u);
  StoreLittle32(framed.data(), count);
  framed.insert(framed.end(), records.begin(), records.end());
  return ComputeEvidence(domain, framed, result, error, std::move(field));
}

using UuidGeneration = std::pair<NarrowQueryUuid, u64>;
using SourceColumnKey =
    std::tuple<NarrowQueryUuid, u64, NarrowQueryUuid, u32>;

bool SourceMatches(const NarrowQuerySourceOccurrence& source,
                   const NarrowQueryUuid& occurrence_uuid,
                   u64 occurrence_generation) {
  return SameUuid(source.source_occurrence_uuid, occurrence_uuid) &&
         source.source_occurrence_generation == occurrence_generation;
}

bool SameRepeatedRelationAuthority(
    const NarrowQuerySourceOccurrence& left,
    const NarrowQuerySourceOccurrence& right) {
  return SameUuid(left.relation_descriptor_uuid,
                  right.relation_descriptor_uuid) &&
         left.relation_descriptor_generation ==
             right.relation_descriptor_generation &&
         SameUuid(left.schema_uuid, right.schema_uuid) &&
         left.validated_resource_epoch == right.validated_resource_epoch &&
         SameHash(left.relation_projection_sha256,
                  right.relation_projection_sha256);
}

bool ValidateBindingStructure(const NarrowQueryBinding& binding,
                              bool require_hashes,
                              NarrowQueryBindingError* error) {
  if (!ValidProfile(binding.profile)) {
    return Fail(error, NarrowQueryBindingErrorCode::profile_invalid,
                kOperandInvalid, "profile_code", 0, "unknown profile code");
  }
  if (binding.sources.empty() ||
      binding.sources.size() > kNarrowQueryMaximumSources ||
      binding.outputs.empty() ||
      binding.outputs.size() > kNarrowQueryMaximumOutputs ||
      binding.ordering_terms.size() > kNarrowQueryMaximumOrderingTerms) {
    return Fail(error, NarrowQueryBindingErrorCode::count_invalid,
                kOperandInvalid, "vector_counts", 0,
                "vector count is outside the admitted bounds");
  }
  if ((!binding.row_limit_present && binding.row_limit != 0) ||
      (!binding.row_offset_present && binding.row_offset != 0)) {
    return Fail(error, NarrowQueryBindingErrorCode::result_bound_invalid,
                kOperandInvalid, "post_projection_result_bound", 0,
                "an absent result-bound field has a nonzero value");
  }
  if (binding.row_limit_present && binding.row_offset_present &&
      binding.row_limit >
          std::numeric_limits<u64>::max() - binding.row_offset) {
    return Fail(error, NarrowQueryBindingErrorCode::result_bound_invalid,
                kOperandInvalid, "post_projection_result_bound", 0,
                "row offset plus row limit overflows unsigned 64-bit");
  }
  const auto bound_end =
      binding.row_limit_present && binding.row_offset_present
          ? binding.row_offset + binding.row_limit
          : 0;
  if ((binding.row_limit_present &&
       binding.row_limit > kNarrowQueryMaximumExplicitResultRows) ||
      (binding.row_offset_present &&
       binding.row_offset > kNarrowQueryMaximumExplicitResultRows) ||
      (binding.row_limit_present && binding.row_offset_present &&
       bound_end > kNarrowQueryMaximumExplicitResultRows)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded, "post_projection_result_bound", 0,
                "result bound exceeds the narrow-profile row maximum");
  }
  if (!UuidPresent(binding.statement_receipt_uuid)) {
    return Fail(error, NarrowQueryBindingErrorCode::statement_receipt_mismatch,
                kTransactionStale, "statement_receipt_uuid", 0,
                "statement receipt is zero");
  }
  if (!UuidPresent(binding.owning_transaction_uuid) ||
      binding.owning_local_transaction_id == 0) {
    return Fail(error, NarrowQueryBindingErrorCode::transaction_invalid,
                kTransactionInvalid, "owning_transaction", 0,
                "transaction identity is zero");
  }
  if (!UuidPresent(binding.statement_snapshot_uuid)) {
    return Fail(error, NarrowQueryBindingErrorCode::snapshot_mismatch,
                kTransactionStale, "statement_snapshot_uuid", 0,
                "statement snapshot is zero");
  }
  if (!UuidPresent(binding.datatype_catalog_snapshot_uuid) ||
      binding.datatype_catalog_generation == 0 ||
      binding.datatype_registry_generation == 0) {
    return Fail(error, NarrowQueryBindingErrorCode::catalog_mismatch,
                kDatatypeInvalid, "datatype_catalog_binding", 0,
                "datatype catalog identity or generation is zero");
  }
  if (!UuidPresent(binding.security_context_uuid) ||
      !UuidPresent(binding.policy_snapshot_uuid) ||
      binding.policy_generation == 0) {
    return Fail(error, NarrowQueryBindingErrorCode::security_mismatch,
                kSecurityDenied, "security_policy_binding", 0,
                "security or policy identity is zero");
  }
  if (!UuidPresent(binding.resource_grant_receipt_uuid) ||
      binding.resource_grant_generation == 0) {
    return Fail(error, NarrowQueryBindingErrorCode::resource_grant_mismatch,
                kResourceExceeded, "resource_grant_binding", 0,
                "resource grant identity is zero");
  }
  if (!UuidPresent(binding.cancellation_receipt_uuid) ||
      binding.cancellation_generation == 0) {
    return Fail(error,
                NarrowQueryBindingErrorCode::cancellation_receipt_mismatch,
                kTransactionStale, "cancellation_receipt_binding", 0,
                "cancellation receipt identity is zero");
  }
  if (!UuidPresent(binding.execution_uuid) ||
      !UuidPresent(binding.result_set_uuid) ||
      !UuidPresent(binding.row_descriptor_uuid) ||
      binding.row_descriptor_generation == 0) {
    return Fail(error, NarrowQueryBindingErrorCode::result_handle_mismatch,
                kResultShapeInvalid, "result_handle", 0,
                "result handle identity is zero");
  }
  if (!UuidPresent(binding.source_vector_uuid) ||
      binding.source_vector_generation == 0 ||
      !UuidPresent(binding.output_vector_uuid) ||
      binding.output_vector_generation == 0) {
    return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                kOperandInvalid, "vector_identity", 0,
                "source or output vector identity is zero");
  }
  const bool ordered =
      binding.profile == NarrowQueryProfile::ordered_projection;
  if (ordered != UuidPresent(binding.ordering_vector_uuid) ||
      (ordered ? binding.ordering_vector_generation == 0
               : binding.ordering_vector_generation != 0)) {
    return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                kOrderingInvalid, "ordering_vector_identity", 0,
                "ordering vector identity does not match the profile");
  }
  if (require_hashes &&
      (!HashPresent(binding.source_vector_sha256) ||
       !HashPresent(binding.output_vector_sha256) ||
       !HashPresent(binding.ordering_vector_sha256) ||
       !HashPresent(binding.descriptor_evidence_sha256))) {
    return Fail(error, NarrowQueryBindingErrorCode::carrier_evidence_mismatch,
                kOperandInvalid, "evidence_hash", 0,
                "one or more evidence hashes are zero");
  }
  if (!ValidMgaRelationDecodedBytesPerPass(
          binding.maximum_mga_relation_decoded_bytes_per_pass)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded,
                "maximum_mga_relation_decoded_bytes_per_pass", 0,
                "retained MGA relation decoded-byte ceiling is outside the "
                "inclusive 64 KiB through 1 TiB range");
  }

  std::set<UuidGeneration> source_occurrences;
  std::set<std::string> aliases;
  for (std::size_t index = 0; index < binding.sources.size(); ++index) {
    const auto& source = binding.sources[index];
    if (source.source_ordinal != index || source.alias.empty() ||
        source.alias.size() > kMaximumAliasBytes ||
        !ValidUtf8NoNul(source.alias)) {
      return Fail(error, NarrowQueryBindingErrorCode::source_record_invalid,
                  kPlanInvalid, "source_record", static_cast<u32>(index),
                  "source ordinal or alias encoding is invalid");
    }
    if (!UuidPresent(source.source_occurrence_uuid) ||
        source.source_occurrence_generation == 0 ||
        !UuidPresent(source.relation_descriptor_uuid) ||
        source.relation_descriptor_generation == 0 ||
        !UuidPresent(source.relation_object_uuid) ||
        !UuidPresent(source.schema_uuid) || source.validated_resource_epoch == 0 ||
        !HashPresent(source.relation_projection_sha256)) {
      return Fail(error, NarrowQueryBindingErrorCode::source_identity_invalid,
                  kPlanInvalid, "source_identity", static_cast<u32>(index),
                  "source identity generation or projection hash is zero");
    }
    if (!source_occurrences.emplace(source.source_occurrence_uuid,
                                    source.source_occurrence_generation).second) {
      return Fail(error, NarrowQueryBindingErrorCode::source_identity_invalid,
                  kPlanInvalid, "source_occurrence", static_cast<u32>(index),
                  "source occurrence pair is duplicated");
    }
    if (!aliases.insert(source.alias).second) {
      return Fail(error, NarrowQueryBindingErrorCode::source_alias_invalid,
                  kPlanInvalid, "source_alias", static_cast<u32>(index),
                  "source alias is duplicated");
    }
  }

  std::set<UuidGeneration> output_occurrences;
  std::set<UuidGeneration> output_descriptors;
  std::vector<std::string> earlier_names;
  std::vector<SourceColumnKey> output_source_columns;
  for (std::size_t index = 0; index < binding.outputs.size(); ++index) {
    const auto& output = binding.outputs[index];
    if (output.output_ordinal != index ||
        output.name.size() > kMaximumNameBytes ||
        !ValidUtf8NoNul(output.name) || !ValidCodecId(output.codec_id) ||
        output.nullability > 2 || output.null_encoding != 1) {
      return Fail(error, NarrowQueryBindingErrorCode::output_record_invalid,
                  kProjectionOutputInvalid, "output_record",
                  static_cast<u32>(index),
                  "output ordinal string or null-state field is invalid");
    }
    const auto expected_occurrence = static_cast<u32>(std::count(
        earlier_names.begin(), earlier_names.end(), output.name));
    if (output.name_occurrence != expected_occurrence) {
      return Fail(error, NarrowQueryBindingErrorCode::output_record_invalid,
                  kProjectionOutputInvalid, "name_occurrence",
                  static_cast<u32>(index),
                  "name occurrence is not the count of earlier exact names");
    }
    earlier_names.push_back(output.name);
    if (!UuidPresent(output.output_occurrence_uuid) ||
        output.output_occurrence_generation == 0 ||
        !UuidPresent(output.source_occurrence_uuid) ||
        output.source_occurrence_generation == 0 ||
        !UuidPresent(output.source_column_uuid) ||
        !UuidPresent(output.output_descriptor_uuid) ||
        output.output_descriptor_generation == 0) {
      return Fail(error, NarrowQueryBindingErrorCode::output_identity_invalid,
                  kProjectionOutputInvalid, "output_identity",
                  static_cast<u32>(index),
                  "output occurrence source or descriptor identity is zero");
    }
    if (!output_occurrences.emplace(output.output_occurrence_uuid,
                                    output.output_occurrence_generation).second ||
        !output_descriptors.emplace(output.output_descriptor_uuid,
                                    output.output_descriptor_generation).second) {
      return Fail(error, NarrowQueryBindingErrorCode::output_identity_invalid,
                  kProjectionOutputInvalid, "output_identity",
                  static_cast<u32>(index),
                  "output occurrence or descriptor pair is duplicated");
    }
    const auto source = std::find_if(
        binding.sources.begin(), binding.sources.end(), [&](const auto& row) {
          return SourceMatches(row, output.source_occurrence_uuid,
                               output.source_occurrence_generation);
        });
    if (source == binding.sources.end()) {
      return Fail(error, NarrowQueryBindingErrorCode::output_source_invalid,
                  kProjectionExpressionInvalid, "output_source_occurrence",
                  static_cast<u32>(index),
                  "output source occurrence is absent or stale");
    }
    if (!UuidPresent(output.datatype_descriptor_uuid) ||
        output.datatype_descriptor_generation == 0 ||
        !UuidPresent(output.datatype_type_uuid) ||
        output.datatype_type_generation == 0 ||
        output.datatype_binary_type_code == 0 || output.codec_version == 0 ||
        output.codec_generation == 0) {
      return Fail(error, NarrowQueryBindingErrorCode::output_datatype_invalid,
                  kDatatypeInvalid, "output_datatype", static_cast<u32>(index),
                  "output datatype/type/codec identity is zero");
    }
    output_source_columns.emplace_back(
        output.source_occurrence_uuid, output.source_occurrence_generation,
        output.source_column_uuid, output.source_column_ordinal);
  }

  std::set<UuidGeneration> term_occurrences;
  std::vector<SourceColumnKey> ordering_source_columns;
  for (std::size_t index = 0; index < binding.ordering_terms.size(); ++index) {
    const auto& term = binding.ordering_terms[index];
    const bool collation_present = UuidPresent(term.collation_uuid);
    if (term.term_ordinal != index ||
        !UuidPresent(term.ordering_term_uuid) ||
        term.ordering_term_generation == 0 ||
        !UuidPresent(term.source_occurrence_uuid) ||
        term.source_occurrence_generation == 0 ||
        !UuidPresent(term.source_column_uuid) ||
        !ValidDirection(term.direction) ||
        !ValidNullPlacement(term.null_placement) ||
        collation_present != (term.collation_generation != 0)) {
      return Fail(error, NarrowQueryBindingErrorCode::ordering_record_invalid,
                  kOrderingInvalid, "ordering_record", static_cast<u32>(index),
                  "ordering ordinal identity code or collation pair is invalid");
    }
    if (!term_occurrences.emplace(term.ordering_term_uuid,
                                  term.ordering_term_generation).second) {
      return Fail(error, NarrowQueryBindingErrorCode::ordering_identity_invalid,
                  kOrderingInvalid, "ordering_term_identity",
                  static_cast<u32>(index),
                  "ordering term pair is duplicated");
    }
    const auto source = std::find_if(
        binding.sources.begin(), binding.sources.end(), [&](const auto& row) {
          return SourceMatches(row, term.source_occurrence_uuid,
                               term.source_occurrence_generation);
        });
    if (source == binding.sources.end()) {
      return Fail(error, NarrowQueryBindingErrorCode::ordering_source_invalid,
                  kOrderingInvalid, "ordering_source_occurrence",
                  static_cast<u32>(index),
                  "ordering source occurrence is absent or stale");
    }
    ordering_source_columns.emplace_back(
        term.source_occurrence_uuid, term.source_occurrence_generation,
        term.source_column_uuid, term.source_column_ordinal);
  }

  if (binding.profile == NarrowQueryProfile::ordered_projection) {
    if (binding.sources.size() != 1 || binding.outputs.empty() ||
        binding.ordering_terms.size() < 2 ||
        binding.ordering_terms.size() > kNarrowQueryMaximumOrderingTerms ||
        std::set<SourceColumnKey>(output_source_columns.begin(),
                                  output_source_columns.end()).size() !=
            output_source_columns.size() ||
        std::set<SourceColumnKey>(ordering_source_columns.begin(),
                                  ordering_source_columns.end()).size() !=
            ordering_source_columns.size()) {
      return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                  kOrderingInvalid, "ordered_projection_shape", 0,
                  "ordered projection count or distinct-column rule failed");
    }
  } else if (binding.profile == NarrowQueryProfile::projection_occurrence) {
    const auto distinct = std::set<SourceColumnKey>(
        output_source_columns.begin(), output_source_columns.end());
    if (binding.sources.size() != 1 || binding.outputs.size() < 2 ||
        !binding.ordering_terms.empty() ||
        distinct.size() == output_source_columns.size()) {
      return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                  kProjectionExpressionInvalid, "projection_occurrence_shape",
                  0, "projection occurrence repetition rule failed");
    }
  } else {
    if (binding.sources.size() < 2 || binding.sources.size() > 9 ||
        !binding.ordering_terms.empty()) {
      return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                  kPlanInvalid, "self_join_shape", 0,
                  "self-join source or ordering count is invalid");
    }
    bool repeated_object = false;
    for (std::size_t left = 0; left < binding.sources.size(); ++left) {
      for (std::size_t right = left + 1; right < binding.sources.size();
           ++right) {
        if (SameUuid(binding.sources[left].relation_object_uuid,
                     binding.sources[right].relation_object_uuid)) {
          repeated_object = true;
          if (!SameRepeatedRelationAuthority(binding.sources[left],
                                             binding.sources[right])) {
            return Fail(
                error, NarrowQueryBindingErrorCode::source_authority_stale,
                kPlanInvalid, "repeated_relation_authority",
                static_cast<u32>(right),
                "repeated object UUID has contradictory descriptor authority");
          }
        }
      }
    }
    if (!repeated_object) {
      return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                  kPlanInvalid, "self_join_repeated_object", 0,
                  "self-join profile has no repeated relation object UUID");
    }
    for (std::size_t index = 0; index < binding.sources.size(); ++index) {
      const auto& source = binding.sources[index];
      const bool referenced = std::any_of(
          binding.outputs.begin(), binding.outputs.end(), [&](const auto& row) {
            return SourceMatches(source, row.source_occurrence_uuid,
                                 row.source_occurrence_generation);
          });
      if (!referenced) {
        return Fail(error, NarrowQueryBindingErrorCode::profile_shape_invalid,
                    kProjectionExpressionInvalid,
                    "self_join_output_coverage", static_cast<u32>(index),
                    "one source occurrence contributes no output");
      }
    }
  }
  return true;
}

void EncodeSource(const NarrowQuerySourceOccurrence& source,
                  std::vector<byte>* bytes) {
  const auto start = bytes->size();
  bytes->resize(start + kNarrowQuerySourcePrefixBytes, 0);
  const auto record_bytes = static_cast<u32>(
      kNarrowQuerySourcePrefixBytes + source.alias.size());
  StoreLittle32(bytes->data() + start, record_bytes);
  StoreLittle32(bytes->data() + start + 4u, source.source_ordinal);
  StoreLittle32(bytes->data() + start + 8u,
                static_cast<u32>(source.alias.size()));
  StoreUuid(bytes, start + 16u, source.source_occurrence_uuid);
  StoreLittle64(bytes->data() + start + 32u,
                source.source_occurrence_generation);
  StoreUuid(bytes, start + 40u, source.relation_descriptor_uuid);
  StoreLittle64(bytes->data() + start + 56u,
                source.relation_descriptor_generation);
  StoreUuid(bytes, start + 64u, source.relation_object_uuid);
  StoreUuid(bytes, start + 80u, source.schema_uuid);
  StoreLittle64(bytes->data() + start + 96u,
                source.validated_resource_epoch);
  StoreHash(bytes, start + 104u, source.relation_projection_sha256);
  AppendString(bytes, source.alias);
}

void EncodeOutput(const NarrowQueryOutputOccurrence& output,
                  std::vector<byte>* bytes) {
  const auto start = bytes->size();
  bytes->resize(start + kNarrowQueryOutputPrefixBytes, 0);
  const auto record_bytes = static_cast<u32>(
      kNarrowQueryOutputPrefixBytes + output.name.size() +
      output.codec_id.size());
  StoreLittle32(bytes->data() + start, record_bytes);
  StoreLittle32(bytes->data() + start + 4u, output.output_ordinal);
  StoreLittle32(bytes->data() + start + 8u, output.name_occurrence);
  StoreLittle16(bytes->data() + start + 12u,
                static_cast<u16>(output.name.size()));
  StoreLittle16(bytes->data() + start + 14u,
                static_cast<u16>(output.codec_id.size()));
  StoreUuid(bytes, start + 16u, output.output_occurrence_uuid);
  StoreLittle64(bytes->data() + start + 32u,
                output.output_occurrence_generation);
  StoreUuid(bytes, start + 40u, output.source_occurrence_uuid);
  StoreLittle64(bytes->data() + start + 56u,
                output.source_occurrence_generation);
  StoreUuid(bytes, start + 64u, output.source_column_uuid);
  StoreLittle32(bytes->data() + start + 80u, output.source_column_ordinal);
  StoreUuid(bytes, start + 88u, output.output_descriptor_uuid);
  StoreLittle64(bytes->data() + start + 104u,
                output.output_descriptor_generation);
  StoreUuid(bytes, start + 112u, output.datatype_descriptor_uuid);
  StoreLittle64(bytes->data() + start + 128u,
                output.datatype_descriptor_generation);
  StoreUuid(bytes, start + 136u, output.datatype_type_uuid);
  StoreLittle64(bytes->data() + start + 152u,
                output.datatype_type_generation);
  StoreLittle32(bytes->data() + start + 160u,
                output.datatype_binary_type_code);
  StoreLittle16(bytes->data() + start + 164u, output.codec_version);
  (*bytes)[start + 166u] = output.nullability;
  (*bytes)[start + 167u] = output.null_encoding;
  StoreLittle64(bytes->data() + start + 168u, output.codec_generation);
  StoreLittle32(bytes->data() + start + 176u,
                output.canonical_value_bytes);
  AppendString(bytes, output.name);
  AppendString(bytes, output.codec_id);
}

void EncodeOrdering(const NarrowQueryOrderingTerm& term,
                    std::vector<byte>* bytes) {
  const auto start = bytes->size();
  bytes->resize(start + kNarrowQueryOrderingRecordBytes, 0);
  StoreLittle32(bytes->data() + start, kNarrowQueryOrderingRecordBytes);
  StoreLittle32(bytes->data() + start + 4u, term.term_ordinal);
  StoreUuid(bytes, start + 8u, term.ordering_term_uuid);
  StoreLittle64(bytes->data() + start + 24u,
                term.ordering_term_generation);
  StoreUuid(bytes, start + 32u, term.source_occurrence_uuid);
  StoreLittle64(bytes->data() + start + 48u,
                term.source_occurrence_generation);
  StoreUuid(bytes, start + 56u, term.source_column_uuid);
  StoreLittle32(bytes->data() + start + 72u, term.source_column_ordinal);
  (*bytes)[start + 76u] = static_cast<u8>(term.direction);
  (*bytes)[start + 77u] = static_cast<u8>(term.null_placement);
  StoreUuid(bytes, start + 80u, term.collation_uuid);
  StoreLittle64(bytes->data() + start + 96u, term.collation_generation);
}

bool ValidateContextShape(const NarrowQueryBindingValidationContext& context,
                          bool needs_collation_validator,
                          NarrowQueryBindingError* error) {
  if (!UuidPresent(context.statement_receipt_uuid) ||
      !UuidPresent(context.owning_transaction_uuid) ||
      context.owning_local_transaction_id == 0 ||
      !UuidPresent(context.statement_snapshot_uuid) ||
      !UuidPresent(context.datatype_catalog_snapshot_uuid) ||
      context.datatype_catalog_generation == 0 ||
      context.datatype_registry_generation == 0 ||
      !UuidPresent(context.security_context_uuid) ||
      !UuidPresent(context.policy_snapshot_uuid) ||
      context.policy_generation == 0 ||
      !UuidPresent(context.resource_grant_receipt_uuid) ||
      context.resource_grant_generation == 0 ||
      !UuidPresent(context.cancellation_receipt_uuid) ||
      context.cancellation_generation == 0 ||
      !UuidPresent(context.execution_uuid) ||
      !UuidPresent(context.result_set_uuid) ||
      !UuidPresent(context.row_descriptor_uuid) ||
      context.row_descriptor_generation == 0 ||
      context.maximum_total_bytes == 0 ||
      context.maximum_total_bytes > kNarrowQueryMaximumCarrierBytes ||
      !ValidMgaRelationDecodedBytesPerPass(
          context.maximum_mga_relation_decoded_bytes_per_pass) ||
      context.maximum_result_rows == 0 ||
      !context.validate_canonical_alias || !context.validate_source ||
      !context.validate_output_datatype ||
      (needs_collation_validator && !context.validate_collation)) {
    return Fail(error, NarrowQueryBindingErrorCode::validation_context_invalid,
                kOperandInvalid, "validation_context", 0,
                "live context identity bounds or authority hook is absent");
  }
  return true;
}

bool ValidateLiveContext(const NarrowQueryBinding& binding,
                         const NarrowQueryBindingValidationContext& context,
                         NarrowQueryBindingError* error) {
  if (!SameUuid(binding.statement_receipt_uuid,
                context.statement_receipt_uuid)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::statement_receipt_mismatch,
                kTransactionStale, "statement_receipt_uuid", 0,
                "binding crosses the live statement receipt");
  }
  if (!SameUuid(binding.owning_transaction_uuid,
                context.owning_transaction_uuid) ||
      binding.owning_local_transaction_id !=
          context.owning_local_transaction_id) {
    return Fail(error, NarrowQueryBindingErrorCode::transaction_stale,
                kTransactionStale, "owning_transaction", 0,
                "binding crosses the live MGA transaction");
  }
  if (!SameUuid(binding.statement_snapshot_uuid,
                context.statement_snapshot_uuid)) {
    return Fail(error, NarrowQueryBindingErrorCode::snapshot_mismatch,
                kTransactionStale, "statement_snapshot_uuid", 0,
                "binding crosses the live MGA snapshot");
  }
  if (!SameUuid(binding.datatype_catalog_snapshot_uuid,
                context.datatype_catalog_snapshot_uuid) ||
      binding.datatype_catalog_generation !=
          context.datatype_catalog_generation ||
      binding.datatype_registry_generation !=
          context.datatype_registry_generation) {
    return Fail(error, NarrowQueryBindingErrorCode::catalog_mismatch,
                kDatatypeInvalid, "datatype_catalog_binding", 0,
                "binding crosses the live datatype catalog generation");
  }
  if (!SameUuid(binding.security_context_uuid,
                context.security_context_uuid) ||
      !SameUuid(binding.policy_snapshot_uuid, context.policy_snapshot_uuid) ||
      binding.policy_generation != context.policy_generation) {
    return Fail(error, NarrowQueryBindingErrorCode::security_mismatch,
                kSecurityDenied, "security_policy_binding", 0,
                "binding crosses the live security or policy snapshot");
  }
  if (!SameUuid(binding.resource_grant_receipt_uuid,
                context.resource_grant_receipt_uuid) ||
      binding.resource_grant_generation !=
          context.resource_grant_generation) {
    return Fail(error, NarrowQueryBindingErrorCode::resource_grant_mismatch,
                kResourceExceeded, "resource_grant_binding", 0,
                "binding crosses the live resource grant");
  }
  if (binding.maximum_mga_relation_decoded_bytes_per_pass !=
      context.maximum_mga_relation_decoded_bytes_per_pass) {
    return Fail(error, NarrowQueryBindingErrorCode::resource_grant_mismatch,
                kResourceExceeded,
                "maximum_mga_relation_decoded_bytes_per_pass", 0,
                "binding scan-byte resource policy differs from the exact "
                "retained live grant");
  }
  const auto bound_end =
      binding.row_limit_present && binding.row_offset_present
          ? binding.row_offset + binding.row_limit
          : 0;
  if ((binding.row_limit_present &&
       binding.row_limit > context.maximum_result_rows) ||
      (binding.row_offset_present &&
       binding.row_offset > context.maximum_result_rows) ||
      (binding.row_limit_present && binding.row_offset_present &&
       bound_end > context.maximum_result_rows)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded, "post_projection_result_bound", 0,
                "result bound exceeds the exact retained-grant row ceiling");
  }
  if (!SameUuid(binding.cancellation_receipt_uuid,
                context.cancellation_receipt_uuid) ||
      binding.cancellation_generation != context.cancellation_generation) {
    return Fail(error,
                NarrowQueryBindingErrorCode::cancellation_receipt_mismatch,
                kTransactionStale, "cancellation_receipt_binding", 0,
                "binding crosses the live cancellation receipt");
  }
  if (!SameUuid(binding.execution_uuid, context.execution_uuid) ||
      !SameUuid(binding.result_set_uuid, context.result_set_uuid) ||
      !SameUuid(binding.row_descriptor_uuid, context.row_descriptor_uuid) ||
      binding.row_descriptor_generation !=
          context.row_descriptor_generation) {
    return Fail(error, NarrowQueryBindingErrorCode::result_handle_mismatch,
                kResultShapeInvalid, "result_handle", 0,
                "binding crosses the expected query result handle");
  }
  for (std::size_t index = 0; index < binding.sources.size(); ++index) {
    const auto& source = binding.sources[index];
    if (!context.validate_canonical_alias(source.alias)) {
      return Fail(error, NarrowQueryBindingErrorCode::source_alias_invalid,
                  kPlanInvalid, "source_alias", static_cast<u32>(index),
                  "alias is not the engine canonical NFC identifier");
    }
    const auto decision = context.validate_source(source);
    if (decision == NarrowQueryAuthorityDecision::hidden_or_unauthorized) {
      return Fail(error, NarrowQueryBindingErrorCode::source_unauthorized,
                  kSecurityDenied, "source_authority",
                  static_cast<u32>(index),
                  "source authority is hidden or unauthorized");
    }
    if (decision != NarrowQueryAuthorityDecision::accepted) {
      return Fail(error, NarrowQueryBindingErrorCode::source_authority_stale,
                  kPlanInvalid, "source_authority", static_cast<u32>(index),
                  "source descriptor authority is stale or contradictory");
    }
  }
  for (std::size_t index = 0; index < binding.outputs.size(); ++index) {
    const auto decision = context.validate_output_datatype(binding.outputs[index]);
    if (decision == NarrowQueryAuthorityDecision::hidden_or_unauthorized) {
      return Fail(error, NarrowQueryBindingErrorCode::output_unauthorized,
                  kSecurityDenied, "output_datatype",
                  static_cast<u32>(index),
                  "output datatype authority is hidden or unauthorized");
    }
    if (decision != NarrowQueryAuthorityDecision::accepted) {
      return Fail(error, NarrowQueryBindingErrorCode::output_datatype_invalid,
                  kDatatypeInvalid, "output_datatype",
                  static_cast<u32>(index),
                  "output datatype authority is stale or contradictory");
    }
  }
  for (std::size_t index = 0; index < binding.ordering_terms.size(); ++index) {
    const auto decision = context.validate_collation(binding.ordering_terms[index]);
    if (decision == NarrowQueryAuthorityDecision::hidden_or_unauthorized) {
      return Fail(error, NarrowQueryBindingErrorCode::output_unauthorized,
                  kSecurityDenied, "ordering_collation",
                  static_cast<u32>(index),
                  "collation authority is hidden or unauthorized");
    }
    if (decision != NarrowQueryAuthorityDecision::accepted) {
      return Fail(error,
                  NarrowQueryBindingErrorCode::ordering_collation_invalid,
                  kCollationInvalid, "ordering_collation",
                  static_cast<u32>(index),
                  "collation authority is stale or contradictory");
    }
  }
  if (context.cancelled) {
    return Fail(error, NarrowQueryBindingErrorCode::cancelled, kCancelled,
                "cancellation_receipt", 0,
                "live cancellation receipt is cancelled");
  }
  return true;
}

struct RecordExtents {
  std::size_t source_begin = kNarrowQueryBindingHeaderBytes + sizeof(u64);
  std::size_t output_begin = kNarrowQueryBindingHeaderBytes + sizeof(u64);
  std::size_t ordering_begin = kNarrowQueryBindingHeaderBytes + sizeof(u64);
  std::size_t end = kNarrowQueryBindingHeaderBytes + sizeof(u64);
};

bool ScanRecordExtents(std::span<const byte> encoded,
                       u16 source_count,
                       u32 output_count,
                       u32 ordering_count,
                       bool result_bound_present,
                       RecordExtents* extents,
                       NarrowQueryBindingError* error) {
  std::size_t offset = kNarrowQueryBindingHeaderBytes + sizeof(u64);
  if (result_bound_present) {
    if (encoded.size() - offset < kNarrowQueryResultBoundBytes) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "post_projection_result_bound", 0,
                  "conditional result-bound extent is truncated");
    }
    offset += kNarrowQueryResultBoundBytes;
  }
  extents->source_begin = offset;
  for (u32 index = 0; index < source_count; ++index) {
    if (offset > encoded.size() || encoded.size() - offset <
                                         kNarrowQuerySourcePrefixBytes) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "source_record_extent", index,
                  "source record prefix is truncated");
    }
    const auto record_bytes = LoadLittle32(encoded.data() + offset);
    const auto alias_bytes = LoadLittle32(encoded.data() + offset + 8u);
    if (record_bytes < kNarrowQuerySourcePrefixBytes ||
        alias_bytes > kMaximumAliasBytes ||
        record_bytes != kNarrowQuerySourcePrefixBytes + alias_bytes ||
        record_bytes > encoded.size() - offset) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "source_record_extent", index,
                  "source record length is invalid");
    }
    offset += record_bytes;
  }
  extents->output_begin = offset;
  for (u32 index = 0; index < output_count; ++index) {
    if (offset > encoded.size() || encoded.size() - offset <
                                         kNarrowQueryOutputPrefixBytes) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "output_record_extent", index,
                  "output record prefix is truncated");
    }
    const auto record_bytes = LoadLittle32(encoded.data() + offset);
    const auto name_bytes = LoadLittle16(encoded.data() + offset + 12u);
    const auto codec_bytes = LoadLittle16(encoded.data() + offset + 14u);
    const auto suffix_bytes = static_cast<u64>(name_bytes) + codec_bytes;
    if (record_bytes < kNarrowQueryOutputPrefixBytes ||
        name_bytes > kMaximumNameBytes ||
        codec_bytes == 0 || codec_bytes > kMaximumCodecIdBytes ||
        record_bytes != kNarrowQueryOutputPrefixBytes + suffix_bytes ||
        record_bytes > encoded.size() - offset) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "output_record_extent", index,
                  "output record length is invalid");
    }
    offset += record_bytes;
  }
  extents->ordering_begin = offset;
  for (u32 index = 0; index < ordering_count; ++index) {
    if (offset > encoded.size() || encoded.size() - offset <
                                         kNarrowQueryOrderingRecordBytes ||
        LoadLittle32(encoded.data() + offset) !=
            kNarrowQueryOrderingRecordBytes) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "ordering_record_extent", index,
                  "ordering record length is invalid");
    }
    offset += kNarrowQueryOrderingRecordBytes;
  }
  extents->end = offset;
  if (offset != encoded.size()) {
    return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                kOperandInvalid, "carrier_extent", 0,
                "carrier has trailing or unclaimed bytes");
  }
  return true;
}

NarrowQuerySourceOccurrence DecodeSource(std::span<const byte> bytes,
                                         std::size_t* offset) {
  const auto start = *offset;
  const auto record_bytes = LoadLittle32(bytes.data() + start);
  const auto alias_bytes = LoadLittle32(bytes.data() + start + 8u);
  NarrowQuerySourceOccurrence source;
  source.source_ordinal = LoadLittle32(bytes.data() + start + 4u);
  source.source_occurrence_uuid = LoadUuid(bytes, start + 16u);
  source.source_occurrence_generation =
      LoadLittle64(bytes.data() + start + 32u);
  source.relation_descriptor_uuid = LoadUuid(bytes, start + 40u);
  source.relation_descriptor_generation =
      LoadLittle64(bytes.data() + start + 56u);
  source.relation_object_uuid = LoadUuid(bytes, start + 64u);
  source.schema_uuid = LoadUuid(bytes, start + 80u);
  source.validated_resource_epoch =
      LoadLittle64(bytes.data() + start + 96u);
  source.relation_projection_sha256 = LoadHash(bytes, start + 104u);
  source.alias.assign(
      reinterpret_cast<const char*>(bytes.data() + start + 136u), alias_bytes);
  *offset += record_bytes;
  return source;
}

NarrowQueryOutputOccurrence DecodeOutput(std::span<const byte> bytes,
                                         std::size_t* offset) {
  const auto start = *offset;
  const auto record_bytes = LoadLittle32(bytes.data() + start);
  const auto name_bytes = LoadLittle16(bytes.data() + start + 12u);
  const auto codec_bytes = LoadLittle16(bytes.data() + start + 14u);
  NarrowQueryOutputOccurrence output;
  output.output_ordinal = LoadLittle32(bytes.data() + start + 4u);
  output.name_occurrence = LoadLittle32(bytes.data() + start + 8u);
  output.output_occurrence_uuid = LoadUuid(bytes, start + 16u);
  output.output_occurrence_generation =
      LoadLittle64(bytes.data() + start + 32u);
  output.source_occurrence_uuid = LoadUuid(bytes, start + 40u);
  output.source_occurrence_generation =
      LoadLittle64(bytes.data() + start + 56u);
  output.source_column_uuid = LoadUuid(bytes, start + 64u);
  output.source_column_ordinal = LoadLittle32(bytes.data() + start + 80u);
  output.output_descriptor_uuid = LoadUuid(bytes, start + 88u);
  output.output_descriptor_generation =
      LoadLittle64(bytes.data() + start + 104u);
  output.datatype_descriptor_uuid = LoadUuid(bytes, start + 112u);
  output.datatype_descriptor_generation =
      LoadLittle64(bytes.data() + start + 128u);
  output.datatype_type_uuid = LoadUuid(bytes, start + 136u);
  output.datatype_type_generation =
      LoadLittle64(bytes.data() + start + 152u);
  output.datatype_binary_type_code =
      LoadLittle32(bytes.data() + start + 160u);
  output.codec_version = LoadLittle16(bytes.data() + start + 164u);
  output.nullability = bytes[start + 166u];
  output.null_encoding = bytes[start + 167u];
  output.codec_generation = LoadLittle64(bytes.data() + start + 168u);
  output.canonical_value_bytes = LoadLittle32(bytes.data() + start + 176u);
  output.name.assign(
      reinterpret_cast<const char*>(bytes.data() + start + 184u), name_bytes);
  output.codec_id.assign(
      reinterpret_cast<const char*>(bytes.data() + start + 184u + name_bytes),
      codec_bytes);
  *offset += record_bytes;
  return output;
}

NarrowQueryOrderingTerm DecodeOrdering(std::span<const byte> bytes,
                                       std::size_t* offset) {
  const auto start = *offset;
  NarrowQueryOrderingTerm term;
  term.term_ordinal = LoadLittle32(bytes.data() + start + 4u);
  term.ordering_term_uuid = LoadUuid(bytes, start + 8u);
  term.ordering_term_generation = LoadLittle64(bytes.data() + start + 24u);
  term.source_occurrence_uuid = LoadUuid(bytes, start + 32u);
  term.source_occurrence_generation =
      LoadLittle64(bytes.data() + start + 48u);
  term.source_column_uuid = LoadUuid(bytes, start + 56u);
  term.source_column_ordinal = LoadLittle32(bytes.data() + start + 72u);
  term.direction = static_cast<NarrowQueryDirection>(bytes[start + 76u]);
  term.null_placement =
      static_cast<NarrowQueryNullPlacement>(bytes[start + 77u]);
  term.collation_uuid = LoadUuid(bytes, start + 80u);
  term.collation_generation = LoadLittle64(bytes.data() + start + 96u);
  *offset += kNarrowQueryOrderingRecordBytes;
  return term;
}

}  // namespace

bool EncodeNarrowQueryBinding(const NarrowQueryBinding& binding,
                              std::vector<byte>* encoded,
                              NarrowQueryBindingError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, NarrowQueryBindingErrorCode::invalid_argument,
                kOperandInvalid, "encoded", 0,
                "encoded output pointer is null");
  }
  if (!ValidateBindingStructure(binding, false, error)) {
    return false;
  }

  u64 total_bytes = kNarrowQueryBindingHeaderBytes + sizeof(u64);
  const bool result_bound_present =
      binding.row_limit_present || binding.row_offset_present;
  if (result_bound_present &&
      !AddWithin(total_bytes, kNarrowQueryResultBoundBytes,
                 kNarrowQueryMaximumCarrierBytes, &total_bytes)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded, "post_projection_result_bound", 0,
                "conditional result-bound extent exceeds 16 MiB");
  }
  for (const auto& source : binding.sources) {
    if (!AddWithin(total_bytes,
                   kNarrowQuerySourcePrefixBytes + source.alias.size(),
                   kNarrowQueryMaximumCarrierBytes, &total_bytes)) {
      return Fail(error,
                  NarrowQueryBindingErrorCode::resource_limit_exceeded,
                  kResourceExceeded, "source_record_extent",
                  source.source_ordinal, "carrier size exceeds 16 MiB");
    }
  }
  for (const auto& output : binding.outputs) {
    if (!AddWithin(total_bytes,
                   kNarrowQueryOutputPrefixBytes + output.name.size() +
                       output.codec_id.size(),
                   kNarrowQueryMaximumCarrierBytes, &total_bytes)) {
      return Fail(error,
                  NarrowQueryBindingErrorCode::resource_limit_exceeded,
                  kResourceExceeded, "output_record_extent",
                  output.output_ordinal, "carrier size exceeds 16 MiB");
    }
  }
  u64 ordering_bytes = 0;
  if (!AddWithin(0,
                 binding.ordering_terms.size() *
                     static_cast<u64>(kNarrowQueryOrderingRecordBytes),
                 kNarrowQueryMaximumCarrierBytes, &ordering_bytes) ||
      !AddWithin(total_bytes, ordering_bytes, kNarrowQueryMaximumCarrierBytes,
                 &total_bytes)) {
    return Fail(error, NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded, "ordering_record_extent", 0,
                "carrier size exceeds 16 MiB");
  }

  std::vector<byte> bytes(kNarrowQueryBindingHeaderBytes, 0);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  StoreLittle16(bytes.data() + 8u, kNarrowQueryBindingVersion);
  StoreLittle16(bytes.data() + 10u, kNarrowQueryBindingHeaderBytes);
  const u32 result_bound_flags =
      (binding.row_limit_present ? kRowLimitPresentFlag : 0u) |
      (binding.row_offset_present ? kRowOffsetPresentFlag : 0u);
  StoreLittle32(bytes.data() + 12u, result_bound_flags);
  StoreLittle64(bytes.data() + 16u, total_bytes);
  StoreLittle16(bytes.data() + 24u, static_cast<u16>(binding.profile));
  StoreLittle16(bytes.data() + 26u,
                static_cast<u16>(binding.sources.size()));
  StoreLittle32(bytes.data() + 28u,
                static_cast<u32>(binding.outputs.size()));
  StoreLittle32(bytes.data() + 32u,
                static_cast<u32>(binding.ordering_terms.size()));
  StoreUuid(&bytes, 40u, binding.statement_receipt_uuid);
  StoreUuid(&bytes, 56u, binding.owning_transaction_uuid);
  StoreLittle64(bytes.data() + 72u, binding.owning_local_transaction_id);
  StoreUuid(&bytes, 80u, binding.statement_snapshot_uuid);
  StoreUuid(&bytes, 96u, binding.datatype_catalog_snapshot_uuid);
  StoreLittle64(bytes.data() + 112u, binding.datatype_catalog_generation);
  StoreLittle64(bytes.data() + 120u, binding.datatype_registry_generation);
  StoreUuid(&bytes, 128u, binding.security_context_uuid);
  StoreUuid(&bytes, 144u, binding.policy_snapshot_uuid);
  StoreLittle64(bytes.data() + 160u, binding.policy_generation);
  StoreUuid(&bytes, 168u, binding.resource_grant_receipt_uuid);
  StoreLittle64(bytes.data() + 184u, binding.resource_grant_generation);
  StoreUuid(&bytes, 192u, binding.cancellation_receipt_uuid);
  StoreLittle64(bytes.data() + 208u, binding.cancellation_generation);
  StoreUuid(&bytes, 216u, binding.execution_uuid);
  StoreUuid(&bytes, 232u, binding.result_set_uuid);
  StoreUuid(&bytes, 248u, binding.row_descriptor_uuid);
  StoreLittle64(bytes.data() + 264u, binding.row_descriptor_generation);
  StoreUuid(&bytes, 272u, binding.source_vector_uuid);
  StoreLittle64(bytes.data() + 288u, binding.source_vector_generation);
  StoreUuid(&bytes, 328u, binding.output_vector_uuid);
  StoreLittle64(bytes.data() + 344u, binding.output_vector_generation);
  StoreUuid(&bytes, 384u, binding.ordering_vector_uuid);
  StoreLittle64(bytes.data() + 400u, binding.ordering_vector_generation);

  AppendU64(&bytes,
            binding.maximum_mga_relation_decoded_bytes_per_pass);

  if (result_bound_present) {
    AppendU64(&bytes, binding.row_limit);
    AppendU64(&bytes, binding.row_offset);
  }

  const auto source_begin = bytes.size();
  for (const auto& source : binding.sources) {
    EncodeSource(source, &bytes);
  }
  const auto output_begin = bytes.size();
  for (const auto& output : binding.outputs) {
    EncodeOutput(output, &bytes);
  }
  const auto ordering_begin = bytes.size();
  for (const auto& term : binding.ordering_terms) {
    EncodeOrdering(term, &bytes);
  }
  if (bytes.size() != total_bytes) {
    return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                kOperandInvalid, "carrier_extent", 0,
                "encoded carrier extent differs from the preflight extent");
  }

  NarrowQueryHash source_hash{};
  NarrowQueryHash output_hash{};
  NarrowQueryHash ordering_hash{};
  NarrowQueryHash carrier_hash{};
  if (!ComputeVectorEvidence(
          kSourceDomain, static_cast<u32>(binding.sources.size()),
          std::span<const byte>(bytes.data() + source_begin,
                                output_begin - source_begin),
          &source_hash, error, "source_vector_sha256") ||
      !ComputeVectorEvidence(
          kOutputDomain, static_cast<u32>(binding.outputs.size()),
          std::span<const byte>(bytes.data() + output_begin,
                                ordering_begin - output_begin),
          &output_hash, error, "output_vector_sha256") ||
      !ComputeVectorEvidence(
          kOrderingDomain, static_cast<u32>(binding.ordering_terms.size()),
          std::span<const byte>(bytes.data() + ordering_begin,
                                bytes.size() - ordering_begin),
          &ordering_hash, error, "ordering_vector_sha256")) {
    return false;
  }
  StoreHash(&bytes, 296u, source_hash);
  StoreHash(&bytes, 352u, output_hash);
  StoreHash(&bytes, 408u, ordering_hash);
  if (!ComputeEvidence(kCarrierDomain, bytes, &carrier_hash, error,
                       "descriptor_evidence_sha256")) {
    return false;
  }
  StoreHash(&bytes, kCarrierEvidenceOffset, carrier_hash);
  *encoded = std::move(bytes);
  return true;
}

bool DecodeAndValidateNarrowQueryBinding(
    std::span<const byte> encoded,
    const NarrowQueryBindingValidationContext& context,
    NarrowQueryBinding* decoded,
    NarrowQueryBindingError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, NarrowQueryBindingErrorCode::invalid_argument,
                kOperandInvalid, "decoded", 0,
                "decoded output pointer is null");
  }
  if (encoded.size() < kNarrowQueryBindingHeaderBytes + sizeof(u64)) {
    return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                kOperandInvalid, "carrier_extent", 0,
                "carrier is shorter than the fixed header plus the mandatory "
                "scan-byte resource extent");
  }
  if (encoded.size() > kNarrowQueryMaximumCarrierBytes ||
      encoded.size() > context.maximum_total_bytes) {
    return Fail(error,
                NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded, "carrier_extent", 0,
                "carrier exceeds the Core or live-context byte ceiling");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), encoded.begin())) {
    return Fail(error, NarrowQueryBindingErrorCode::magic_invalid,
                kOperandInvalid, "magic", 0, "carrier magic is not SBQNPB01");
  }
  if (LoadLittle16(encoded.data() + 8u) != kNarrowQueryBindingVersion) {
    return Fail(error, NarrowQueryBindingErrorCode::version_invalid,
                kOperandInvalid, "layout_version", 0,
                "carrier version is not 1");
  }
  if (LoadLittle16(encoded.data() + 10u) !=
          kNarrowQueryBindingHeaderBytes ||
      LoadLittle64(encoded.data() + 16u) != encoded.size()) {
    return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                kOperandInvalid, "header_or_total_bytes", 0,
                "header or total extent is not exact");
  }
  const auto result_bound_flags = LoadLittle32(encoded.data() + 12u);
  if ((result_bound_flags & ~kResultBoundFlags) != 0 ||
      LoadLittle32(encoded.data() + 36u) != 0) {
    return Fail(error, NarrowQueryBindingErrorCode::reserved_invalid,
                kOperandInvalid, "header_reserved", 0,
                "header has an unknown presence flag or nonzero reserved bytes");
  }
  const bool row_limit_present =
      (result_bound_flags & kRowLimitPresentFlag) != 0;
  const bool row_offset_present =
      (result_bound_flags & kRowOffsetPresentFlag) != 0;
  const bool result_bound_present = row_limit_present || row_offset_present;
  const auto maximum_mga_relation_decoded_bytes_per_pass =
      LoadLittle64(encoded.data() + kNarrowQueryBindingHeaderBytes);
  if (!ValidMgaRelationDecodedBytesPerPass(
          maximum_mga_relation_decoded_bytes_per_pass)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::resource_limit_exceeded,
                kResourceExceeded,
                "maximum_mga_relation_decoded_bytes_per_pass", 0,
                "encoded MGA relation decoded-byte ceiling is outside the "
                "inclusive 64 KiB through 1 TiB range");
  }
  u64 row_limit = 0;
  u64 row_offset = 0;
  if (result_bound_present) {
    if (encoded.size() <
        kNarrowQueryBindingHeaderBytes + sizeof(u64) +
            kNarrowQueryResultBoundBytes) {
      return Fail(error, NarrowQueryBindingErrorCode::extent_invalid,
                  kOperandInvalid, "post_projection_result_bound", 0,
                  "conditional result-bound extent is truncated");
    }
    row_limit = LoadLittle64(encoded.data() +
                             kNarrowQueryBindingHeaderBytes + sizeof(u64));
    row_offset = LoadLittle64(encoded.data() +
                              kNarrowQueryBindingHeaderBytes + sizeof(u64) +
                              8u);
    if ((!row_limit_present && row_limit != 0) ||
        (!row_offset_present && row_offset != 0)) {
      return Fail(error, NarrowQueryBindingErrorCode::result_bound_invalid,
                  kOperandInvalid, "post_projection_result_bound", 0,
                  "an absent result-bound field has a nonzero encoded value");
    }
    if (row_limit_present && row_offset_present &&
        row_limit > std::numeric_limits<u64>::max() - row_offset) {
      return Fail(error, NarrowQueryBindingErrorCode::result_bound_invalid,
                  kOperandInvalid, "post_projection_result_bound", 0,
                  "row offset plus row limit overflows unsigned 64-bit");
    }
    const auto bound_end = row_limit_present && row_offset_present
                               ? row_offset + row_limit
                               : 0;
    if ((row_limit_present &&
         row_limit > kNarrowQueryMaximumExplicitResultRows) ||
        (row_offset_present &&
         row_offset > kNarrowQueryMaximumExplicitResultRows) ||
        (row_limit_present && row_offset_present &&
         bound_end > kNarrowQueryMaximumExplicitResultRows)) {
      return Fail(error,
                  NarrowQueryBindingErrorCode::resource_limit_exceeded,
                  kResourceExceeded, "post_projection_result_bound", 0,
                  "result bound exceeds the narrow-profile row maximum");
    }
  }
  const auto profile =
      static_cast<NarrowQueryProfile>(LoadLittle16(encoded.data() + 24u));
  if (!ValidProfile(profile)) {
    return Fail(error, NarrowQueryBindingErrorCode::profile_invalid,
                kOperandInvalid, "profile_code", 0, "unknown profile code");
  }
  const auto source_count = LoadLittle16(encoded.data() + 26u);
  const auto output_count = LoadLittle32(encoded.data() + 28u);
  const auto ordering_count = LoadLittle32(encoded.data() + 32u);
  if (source_count == 0 || source_count > kNarrowQueryMaximumSources ||
      output_count == 0 || output_count > kNarrowQueryMaximumOutputs ||
      ordering_count > kNarrowQueryMaximumOrderingTerms) {
    return Fail(error, NarrowQueryBindingErrorCode::count_invalid,
                kOperandInvalid, "vector_counts", 0,
                "header vector count is outside the Core bound");
  }

  RecordExtents extents;
  if (!ScanRecordExtents(encoded, source_count, output_count, ordering_count,
                         result_bound_present, &extents, error)) {
    return false;
  }
  NarrowQueryHash expected_source_hash{};
  NarrowQueryHash expected_output_hash{};
  NarrowQueryHash expected_ordering_hash{};
  NarrowQueryHash expected_carrier_hash{};
  if (!ComputeVectorEvidence(
          kSourceDomain, source_count,
          encoded.subspan(extents.source_begin,
                          extents.output_begin - extents.source_begin),
          &expected_source_hash, error, "source_vector_sha256") ||
      !ComputeVectorEvidence(
          kOutputDomain, output_count,
          encoded.subspan(extents.output_begin,
                          extents.ordering_begin - extents.output_begin),
          &expected_output_hash, error, "output_vector_sha256") ||
      !ComputeVectorEvidence(
          kOrderingDomain, ordering_count,
          encoded.subspan(extents.ordering_begin,
                          extents.end - extents.ordering_begin),
          &expected_ordering_hash, error, "ordering_vector_sha256")) {
    return false;
  }
  if (!SameHash(LoadHash(encoded, 296u), expected_source_hash)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::source_vector_evidence_mismatch,
                kOperandInvalid, "source_vector_sha256", 0,
                "source vector evidence does not match exact bytes");
  }
  if (!SameHash(LoadHash(encoded, 352u), expected_output_hash)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::output_vector_evidence_mismatch,
                kOperandInvalid, "output_vector_sha256", 0,
                "output vector evidence does not match exact bytes");
  }
  if (!SameHash(LoadHash(encoded, 408u), expected_ordering_hash)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::ordering_vector_evidence_mismatch,
                kOperandInvalid, "ordering_vector_sha256", 0,
                "ordering vector evidence does not match exact bytes");
  }
  std::vector<byte> carrier_material(encoded.begin(), encoded.end());
  std::fill_n(carrier_material.begin() + kCarrierEvidenceOffset, 32u, 0);
  if (!ComputeEvidence(kCarrierDomain, carrier_material,
                       &expected_carrier_hash, error,
                       "descriptor_evidence_sha256")) {
    return false;
  }
  if (!SameHash(LoadHash(encoded, kCarrierEvidenceOffset),
                expected_carrier_hash)) {
    return Fail(error,
                NarrowQueryBindingErrorCode::carrier_evidence_mismatch,
                kOperandInvalid, "descriptor_evidence_sha256", 0,
                "carrier evidence does not match exact bytes");
  }

  NarrowQueryBinding binding;
  binding.profile = profile;
  binding.statement_receipt_uuid = LoadUuid(encoded, 40u);
  binding.owning_transaction_uuid = LoadUuid(encoded, 56u);
  binding.owning_local_transaction_id = LoadLittle64(encoded.data() + 72u);
  binding.statement_snapshot_uuid = LoadUuid(encoded, 80u);
  binding.datatype_catalog_snapshot_uuid = LoadUuid(encoded, 96u);
  binding.datatype_catalog_generation = LoadLittle64(encoded.data() + 112u);
  binding.datatype_registry_generation = LoadLittle64(encoded.data() + 120u);
  binding.security_context_uuid = LoadUuid(encoded, 128u);
  binding.policy_snapshot_uuid = LoadUuid(encoded, 144u);
  binding.policy_generation = LoadLittle64(encoded.data() + 160u);
  binding.resource_grant_receipt_uuid = LoadUuid(encoded, 168u);
  binding.resource_grant_generation = LoadLittle64(encoded.data() + 184u);
  binding.cancellation_receipt_uuid = LoadUuid(encoded, 192u);
  binding.cancellation_generation = LoadLittle64(encoded.data() + 208u);
  binding.execution_uuid = LoadUuid(encoded, 216u);
  binding.result_set_uuid = LoadUuid(encoded, 232u);
  binding.row_descriptor_uuid = LoadUuid(encoded, 248u);
  binding.row_descriptor_generation = LoadLittle64(encoded.data() + 264u);
  binding.source_vector_uuid = LoadUuid(encoded, 272u);
  binding.source_vector_generation = LoadLittle64(encoded.data() + 288u);
  binding.source_vector_sha256 = LoadHash(encoded, 296u);
  binding.output_vector_uuid = LoadUuid(encoded, 328u);
  binding.output_vector_generation = LoadLittle64(encoded.data() + 344u);
  binding.output_vector_sha256 = LoadHash(encoded, 352u);
  binding.ordering_vector_uuid = LoadUuid(encoded, 384u);
  binding.ordering_vector_generation = LoadLittle64(encoded.data() + 400u);
  binding.ordering_vector_sha256 = LoadHash(encoded, 408u);
  binding.descriptor_evidence_sha256 =
      LoadHash(encoded, kCarrierEvidenceOffset);
  binding.maximum_mga_relation_decoded_bytes_per_pass =
      maximum_mga_relation_decoded_bytes_per_pass;
  binding.row_limit_present = row_limit_present;
  binding.row_limit = row_limit;
  binding.row_offset_present = row_offset_present;
  binding.row_offset = row_offset;

  std::size_t offset = extents.source_begin;
  binding.sources.reserve(source_count);
  for (u32 index = 0; index < source_count; ++index) {
    if (LoadLittle32(encoded.data() + offset + 12u) != 0) {
      return Fail(error, NarrowQueryBindingErrorCode::reserved_invalid,
                  kOperandInvalid, "source_reserved", index,
                  "source record reserved bytes are nonzero");
    }
    binding.sources.push_back(DecodeSource(encoded, &offset));
  }
  binding.outputs.reserve(output_count);
  for (u32 index = 0; index < output_count; ++index) {
    if (LoadLittle32(encoded.data() + offset + 84u) != 0 ||
        LoadLittle32(encoded.data() + offset + 180u) != 0) {
      return Fail(error, NarrowQueryBindingErrorCode::reserved_invalid,
                  kOperandInvalid, "output_reserved", index,
                  "output record reserved bytes are nonzero");
    }
    binding.outputs.push_back(DecodeOutput(encoded, &offset));
  }
  binding.ordering_terms.reserve(ordering_count);
  for (u32 index = 0; index < ordering_count; ++index) {
    if (LoadLittle16(encoded.data() + offset + 78u) != 0 ||
        LoadLittle64(encoded.data() + offset + 104u) != 0) {
      return Fail(error, NarrowQueryBindingErrorCode::reserved_invalid,
                  kOperandInvalid, "ordering_reserved", index,
                  "ordering record reserved bytes are nonzero");
    }
    binding.ordering_terms.push_back(DecodeOrdering(encoded, &offset));
  }
  if (!ValidateBindingStructure(binding, true, error)) {
    return false;
  }
  if (!ValidateContextShape(context, !binding.ordering_terms.empty(), error)) {
    return false;
  }
  if (!ValidateLiveContext(binding, context, error)) {
    return false;
  }
  *decoded = std::move(binding);
  return true;
}

const char* NarrowQueryBindingErrorCodeName(NarrowQueryBindingErrorCode code) {
  switch (code) {
    case NarrowQueryBindingErrorCode::ok:
      return "ok";
    case NarrowQueryBindingErrorCode::invalid_argument:
      return "invalid_argument";
    case NarrowQueryBindingErrorCode::magic_invalid:
      return "magic_invalid";
    case NarrowQueryBindingErrorCode::version_invalid:
      return "version_invalid";
    case NarrowQueryBindingErrorCode::extent_invalid:
      return "extent_invalid";
    case NarrowQueryBindingErrorCode::reserved_invalid:
      return "reserved_invalid";
    case NarrowQueryBindingErrorCode::profile_invalid:
      return "profile_invalid";
    case NarrowQueryBindingErrorCode::count_invalid:
      return "count_invalid";
    case NarrowQueryBindingErrorCode::result_bound_invalid:
      return "result_bound_invalid";
    case NarrowQueryBindingErrorCode::resource_limit_exceeded:
      return "resource_limit_exceeded";
    case NarrowQueryBindingErrorCode::carrier_evidence_mismatch:
      return "carrier_evidence_mismatch";
    case NarrowQueryBindingErrorCode::source_vector_evidence_mismatch:
      return "source_vector_evidence_mismatch";
    case NarrowQueryBindingErrorCode::output_vector_evidence_mismatch:
      return "output_vector_evidence_mismatch";
    case NarrowQueryBindingErrorCode::ordering_vector_evidence_mismatch:
      return "ordering_vector_evidence_mismatch";
    case NarrowQueryBindingErrorCode::validation_context_invalid:
      return "validation_context_invalid";
    case NarrowQueryBindingErrorCode::statement_receipt_mismatch:
      return "statement_receipt_mismatch";
    case NarrowQueryBindingErrorCode::transaction_invalid:
      return "transaction_invalid";
    case NarrowQueryBindingErrorCode::transaction_stale:
      return "transaction_stale";
    case NarrowQueryBindingErrorCode::snapshot_mismatch:
      return "snapshot_mismatch";
    case NarrowQueryBindingErrorCode::catalog_mismatch:
      return "catalog_mismatch";
    case NarrowQueryBindingErrorCode::security_mismatch:
      return "security_mismatch";
    case NarrowQueryBindingErrorCode::resource_grant_mismatch:
      return "resource_grant_mismatch";
    case NarrowQueryBindingErrorCode::cancellation_receipt_mismatch:
      return "cancellation_receipt_mismatch";
    case NarrowQueryBindingErrorCode::result_handle_mismatch:
      return "result_handle_mismatch";
    case NarrowQueryBindingErrorCode::source_record_invalid:
      return "source_record_invalid";
    case NarrowQueryBindingErrorCode::source_identity_invalid:
      return "source_identity_invalid";
    case NarrowQueryBindingErrorCode::source_alias_invalid:
      return "source_alias_invalid";
    case NarrowQueryBindingErrorCode::source_authority_stale:
      return "source_authority_stale";
    case NarrowQueryBindingErrorCode::source_unauthorized:
      return "source_unauthorized";
    case NarrowQueryBindingErrorCode::output_record_invalid:
      return "output_record_invalid";
    case NarrowQueryBindingErrorCode::output_identity_invalid:
      return "output_identity_invalid";
    case NarrowQueryBindingErrorCode::output_source_invalid:
      return "output_source_invalid";
    case NarrowQueryBindingErrorCode::output_datatype_invalid:
      return "output_datatype_invalid";
    case NarrowQueryBindingErrorCode::output_unauthorized:
      return "output_unauthorized";
    case NarrowQueryBindingErrorCode::ordering_record_invalid:
      return "ordering_record_invalid";
    case NarrowQueryBindingErrorCode::ordering_identity_invalid:
      return "ordering_identity_invalid";
    case NarrowQueryBindingErrorCode::ordering_source_invalid:
      return "ordering_source_invalid";
    case NarrowQueryBindingErrorCode::ordering_collation_invalid:
      return "ordering_collation_invalid";
    case NarrowQueryBindingErrorCode::profile_shape_invalid:
      return "profile_shape_invalid";
    case NarrowQueryBindingErrorCode::cancelled:
      return "cancelled";
    case NarrowQueryBindingErrorCode::hash_failure:
      return "hash_failure";
  }
  return "unknown";
}

}  // namespace scratchbird::wire
