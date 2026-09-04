// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/contextual_text_target_authority_resolver_v2.hpp"

#include "api_diagnostics.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using Uuid = sblr::ContextualTextUuidV2;

std::mutex g_selector_provider_mutex;
std::shared_ptr<const EngineContextualTextTargetAuthoritySelectorProviderV2>
    g_selector_provider;

EngineApiDiagnostic Diagnostic(std::string code,
                               std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool Nonzero(const Uuid& value) {
  return std::ranges::any_of(value,
                             [](std::uint8_t byte) { return byte != 0; });
}

bool ParseUuid(std::string_view text, Uuid* out) {
  if (out == nullptr) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
  return true;
}

EngineContextualTextCoordinationPolicyV2 ExactCoordinationPolicy() {
  EngineContextualTextCoordinationPolicyV2 policy;
  (void)ParseUuid("b3217577-26d4-5d9e-bf58-5cfdb29c3687",
                  &policy.literal_budget_policy_uuid);
  policy.literal_budget_policy_generation = 1;
  (void)ParseUuid("3cd930a5-cf22-51a5-b36c-5a1b62d74f89",
                  &policy.descriptor_format_uuid);
  policy.descriptor_format_generation = 2;
  (void)ParseUuid("d13c373f-afca-552e-89ef-43a4f1ec5100",
                  &policy.profile_set_namespace_uuid);
  policy.profile_set_namespace_generation = 1;
  (void)ParseUuid("9932194a-7dda-5dec-8600-c6800d195f66",
                  &policy.source_occurrence_namespace_uuid);
  policy.source_occurrence_namespace_generation = 1;
  (void)ParseUuid("2f299fd4-9e29-5dae-9a39-5c4f8a3f4fd5",
                  &policy.budget_grant_namespace_uuid);
  policy.budget_grant_namespace_generation = 1;
  (void)ParseUuid("dc31437d-be8b-5469-984e-2c1cefa9ca66",
                  &policy.raw_token_codec_uuid);
  policy.raw_token_codec_generation = 1;
  policy.literal_budget_is_private_receipt_policy = true;
  policy.descriptor_format_is_sbtltd02_v2 = true;
  policy.profile_set_namespace_is_uuidv7_v2 = true;
  policy.source_occurrence_namespace_is_uuidv7_v2 = true;
  policy.budget_grant_namespace_is_uuidv7_v2 = true;
  policy.raw_token_codec_is_sbsql_single_quoted_v2 = true;
  policy.minimum_literal_negotiation_byte_grant = 4096;
  policy.maximum_literal_negotiation_byte_grant =
      sblr::kContextualTextMaximumLogicalCarrierBytesV2;
  policy.default_literal_negotiation_byte_grant =
      sblr::kContextualTextMaximumLogicalCarrierBytesV2;
  policy.minimum_canonical_body_aggregate_grant = 1;
  policy.maximum_canonical_body_aggregate_grant =
      sblr::kContextualTextMaximumBodyBytesV2;
  policy.default_canonical_body_aggregate_grant =
      sblr::kContextualTextMaximumBodyBytesV2;
  return policy;
}

std::string UuidText(const Uuid& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      out.push_back('-');
    }
    out.push_back(hex[value[index] >> 4]);
    out.push_back(hex[value[index] & 0x0f]);
  }
  return out;
}

std::optional<std::string> ExactEncodedDescriptorField(
    const std::string_view descriptor,
    const std::string_view requested_key) {
  std::optional<std::string> result;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    const auto equals = field.find('=');
    if (field.empty() || equals == std::string_view::npos || equals == 0 ||
        equals + 1 == field.size()) {
      return std::nullopt;
    }
    if (field.substr(0, equals) == requested_key) {
      if (result.has_value()) return std::nullopt;
      result = std::string(field.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

bool SameContext(const EngineRequestContext& left,
                 const EngineRequestContext& right) {
  return left.security_context_present && right.security_context_present &&
         left.database_uuid.canonical == right.database_uuid.canonical &&
         left.session_uuid.canonical == right.session_uuid.canonical &&
         left.transaction_uuid.canonical == right.transaction_uuid.canonical &&
         left.statement_uuid.canonical == right.statement_uuid.canonical &&
         left.statement_receipt_uuid.canonical ==
             right.statement_receipt_uuid.canonical &&
         left.statement_snapshot_uuid.canonical ==
             right.statement_snapshot_uuid.canonical &&
         left.datatype_catalog_snapshot_uuid.canonical ==
             right.datatype_catalog_snapshot_uuid.canonical &&
         left.datatype_catalog_generation == right.datatype_catalog_generation &&
         left.datatype_registry_generation ==
             right.datatype_registry_generation &&
         left.security_epoch == right.security_epoch &&
         left.resource_epoch == right.resource_epoch;
}

bool SameCoordinationPolicy(
    const EngineContextualTextCoordinationPolicyV2& left,
    const EngineContextualTextCoordinationPolicyV2& right) {
  return left.literal_budget_policy_uuid ==
             right.literal_budget_policy_uuid &&
         left.literal_budget_policy_generation ==
             right.literal_budget_policy_generation &&
         left.descriptor_format_uuid == right.descriptor_format_uuid &&
         left.descriptor_format_generation ==
             right.descriptor_format_generation &&
         left.profile_set_namespace_uuid ==
             right.profile_set_namespace_uuid &&
         left.profile_set_namespace_generation ==
             right.profile_set_namespace_generation &&
         left.source_occurrence_namespace_uuid ==
             right.source_occurrence_namespace_uuid &&
         left.source_occurrence_namespace_generation ==
             right.source_occurrence_namespace_generation &&
         left.budget_grant_namespace_uuid ==
             right.budget_grant_namespace_uuid &&
         left.budget_grant_namespace_generation ==
             right.budget_grant_namespace_generation &&
         left.raw_token_codec_uuid == right.raw_token_codec_uuid &&
         left.raw_token_codec_generation ==
             right.raw_token_codec_generation &&
         left.literal_budget_is_private_receipt_policy ==
             right.literal_budget_is_private_receipt_policy &&
         left.descriptor_format_is_sbtltd02_v2 ==
             right.descriptor_format_is_sbtltd02_v2 &&
         left.profile_set_namespace_is_uuidv7_v2 ==
             right.profile_set_namespace_is_uuidv7_v2 &&
         left.source_occurrence_namespace_is_uuidv7_v2 ==
             right.source_occurrence_namespace_is_uuidv7_v2 &&
         left.budget_grant_namespace_is_uuidv7_v2 ==
             right.budget_grant_namespace_is_uuidv7_v2 &&
         left.raw_token_codec_is_sbsql_single_quoted_v2 ==
             right.raw_token_codec_is_sbsql_single_quoted_v2 &&
         left.minimum_literal_negotiation_byte_grant ==
             right.minimum_literal_negotiation_byte_grant &&
         left.maximum_literal_negotiation_byte_grant ==
             right.maximum_literal_negotiation_byte_grant &&
         left.default_literal_negotiation_byte_grant ==
             right.default_literal_negotiation_byte_grant &&
         left.minimum_canonical_body_aggregate_grant ==
             right.minimum_canonical_body_aggregate_grant &&
         left.maximum_canonical_body_aggregate_grant ==
             right.maximum_canonical_body_aggregate_grant &&
         left.default_canonical_body_aggregate_grant ==
             right.default_canonical_body_aggregate_grant;
}

bool ValidCoordinationPolicy(
    const EngineContextualTextCoordinationPolicyV2& rows) {
  const auto valid_pair = [](const Uuid& uuid, std::uint64_t generation) {
    return Nonzero(uuid) && generation != 0;
  };
  return valid_pair(rows.literal_budget_policy_uuid,
                    rows.literal_budget_policy_generation) &&
         valid_pair(rows.descriptor_format_uuid,
                    rows.descriptor_format_generation) &&
         valid_pair(rows.profile_set_namespace_uuid,
                    rows.profile_set_namespace_generation) &&
         valid_pair(rows.source_occurrence_namespace_uuid,
                    rows.source_occurrence_namespace_generation) &&
         valid_pair(rows.budget_grant_namespace_uuid,
                    rows.budget_grant_namespace_generation) &&
         valid_pair(rows.raw_token_codec_uuid,
                    rows.raw_token_codec_generation) &&
         rows.literal_budget_is_private_receipt_policy &&
         rows.descriptor_format_is_sbtltd02_v2 &&
         rows.profile_set_namespace_is_uuidv7_v2 &&
         rows.source_occurrence_namespace_is_uuidv7_v2 &&
         rows.budget_grant_namespace_is_uuidv7_v2 &&
         rows.raw_token_codec_is_sbsql_single_quoted_v2 &&
         rows.minimum_literal_negotiation_byte_grant >= 4096 &&
         rows.minimum_literal_negotiation_byte_grant <=
             rows.default_literal_negotiation_byte_grant &&
         rows.default_literal_negotiation_byte_grant <=
             rows.maximum_literal_negotiation_byte_grant &&
         rows.maximum_literal_negotiation_byte_grant <=
             sblr::kContextualTextMaximumLogicalCarrierBytesV2 &&
         rows.minimum_canonical_body_aggregate_grant >= 1 &&
         rows.minimum_canonical_body_aggregate_grant <=
             rows.default_canonical_body_aggregate_grant &&
         rows.default_canonical_body_aggregate_grant <=
             rows.maximum_canonical_body_aggregate_grant &&
         rows.maximum_canonical_body_aggregate_grant <=
             sblr::kContextualTextMaximumBodyBytesV2;
}

std::uint16_t U16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1]) << 8;
}

std::uint32_t U32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         static_cast<std::uint32_t>(bytes[1]) << 8 |
         static_cast<std::uint32_t>(bytes[2]) << 16 |
         static_cast<std::uint32_t>(bytes[3]) << 24;
}

std::uint64_t U64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index != 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
  }
  return value;
}

bool Take(std::span<const std::uint8_t> bytes,
          std::size_t count,
          std::size_t* cursor,
          const std::uint8_t** output) {
  if (cursor == nullptr || output == nullptr || *cursor > bytes.size() ||
      count > bytes.size() - *cursor) {
    return false;
  }
  *output = bytes.data() + *cursor;
  *cursor += count;
  return true;
}

bool TakeUuid(std::span<const std::uint8_t> bytes,
              std::size_t* cursor,
              Uuid* output) {
  const std::uint8_t* value = nullptr;
  if (output == nullptr || !Take(bytes, output->size(), cursor, &value)) {
    return false;
  }
  std::copy_n(value, output->size(), output->begin());
  return true;
}

bool CanonicalUtf8NoNul(const std::uint8_t* bytes, std::size_t size) {
  if (bytes == nullptr && size != 0) return false;
  std::size_t cursor = 0;
  while (cursor < size) {
    const auto first = bytes[cursor++];
    if (first == 0) return false;
    if (first <= 0x7f) continue;
    std::uint32_t scalar = 0;
    std::size_t continuation_count = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      scalar = first & 0x1fu;
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      scalar = first & 0x0fu;
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      scalar = first & 0x07u;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > size - cursor) return false;
    for (std::size_t index = 0; index != continuation_count; ++index) {
      const auto continuation = bytes[cursor++];
      if ((continuation & 0xc0u) != 0x80u) return false;
      scalar = (scalar << 6) | (continuation & 0x3fu);
    }
    if ((continuation_count == 2 && scalar < 0x800u) ||
        (continuation_count == 3 && scalar < 0x10000u) ||
        scalar > 0x10ffffu ||
        (scalar >= 0xd800u && scalar <= 0xdfffu)) {
      return false;
    }
  }
  return true;
}

bool TakeText(std::span<const std::uint8_t> bytes,
              std::size_t* cursor,
              std::string* output) {
  const std::uint8_t* prefix = nullptr;
  if (output == nullptr || !Take(bytes, 4, cursor, &prefix)) return false;
  const auto size = static_cast<std::size_t>(U32(prefix));
  const std::uint8_t* value = nullptr;
  if (!Take(bytes, size, cursor, &value) ||
      !CanonicalUtf8NoNul(value, size)) {
    return false;
  }
  output->assign(reinterpret_cast<const char*>(value), size);
  return true;
}

void AppendU16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendUuid(std::vector<std::uint8_t>* bytes, const Uuid& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

bool AppendText(std::vector<std::uint8_t>* bytes, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max() ||
      !CanonicalUtf8NoNul(
          reinterpret_cast<const std::uint8_t*>(value.data()),
          value.size())) {
    return false;
  }
  AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
  return true;
}

bool DecodeProjectionV3(const std::vector<std::uint8_t>& exact,
                        EnginePublicRelationProjectionV3* output) {
  if (output == nullptr || exact.size() < 100) return false;
  const std::span<const std::uint8_t> bytes(exact);
  EnginePublicRelationProjectionV3 projection;
  std::size_t cursor = 0;
  const std::uint8_t* fixed = nullptr;
  if (!TakeUuid(bytes, &cursor, &projection.relation_descriptor_uuid) ||
      !TakeUuid(bytes, &cursor, &projection.relation_uuid) ||
      !TakeUuid(bytes, &cursor, &projection.schema_uuid) ||
      !Take(bytes, 8, &cursor, &fixed)) {
    return false;
  }
  projection.relation_descriptor_generation = U64(fixed);
  if (!Take(bytes, 8, &cursor, &fixed)) return false;
  projection.resource_epoch = U64(fixed);
  if (!TakeUuid(bytes, &cursor, &projection.catalog_snapshot_uuid) ||
      !Take(bytes, 8, &cursor, &fixed)) {
    return false;
  }
  projection.catalog_generation = U64(fixed);
  if (!Take(bytes, 8, &cursor, &fixed)) return false;
  projection.registry_generation = U64(fixed);
  if (!Take(bytes, 4, &cursor, &fixed)) return false;
  const auto count = static_cast<std::size_t>(U32(fixed));
  if (!Nonzero(projection.relation_descriptor_uuid) ||
      !Nonzero(projection.relation_uuid) || !Nonzero(projection.schema_uuid) ||
      projection.relation_descriptor_generation == 0 ||
      projection.resource_epoch == 0 ||
      !Nonzero(projection.catalog_snapshot_uuid) ||
      projection.catalog_generation == 0 ||
      projection.registry_generation == 0 || count == 0 ||
      count > (bytes.size() - cursor) / 92) {
    return false;
  }
  try {
    projection.columns.reserve(count);
  } catch (const std::bad_alloc&) {
    return false;
  }
  std::set<std::uint32_t> ordinals;
  std::set<std::string> column_uuids;
  for (std::size_t index = 0; index != count; ++index) {
    EnginePublicRelationProjectionColumnV3 column;
    if (!TakeUuid(bytes, &cursor, &column.column_uuid) ||
        !Take(bytes, 4, &cursor, &fixed)) {
      return false;
    }
    column.ordinal = U32(fixed);
    if (!TakeText(bytes, &cursor, &column.canonical_name) ||
        !TakeUuid(bytes, &cursor, &column.descriptor_uuid) ||
        !TakeText(bytes, &cursor, &column.descriptor_kind) ||
        !TakeText(bytes, &cursor, &column.canonical_type_name) ||
        !TakeText(bytes, &cursor, &column.encoded_type_descriptor) ||
        !Take(bytes, 1, &cursor, &fixed)) {
      return false;
    }
    column.attributes = fixed[0];
    if (!TakeUuid(bytes, &cursor, &column.charset_uuid) ||
        !TakeText(bytes, &cursor, &column.charset_name) ||
        !TakeUuid(bytes, &cursor, &column.collation_uuid) ||
        !TakeText(bytes, &cursor, &column.collation_name) ||
        !Take(bytes, 4, &cursor, &fixed)) {
      return false;
    }
    column.character_length = U32(fixed);
    if (!Take(bytes, 4, &cursor, &fixed)) return false;
    column.charset_min_bytes = U32(fixed);
    if (!Take(bytes, 4, &cursor, &fixed)) return false;
    column.charset_max_bytes = U32(fixed);
    if (!Take(bytes, 1, &cursor, &fixed) || fixed[0] > 1) return false;
    column.identity_present = fixed[0] == 1;
    if (column.identity_present) {
      if (!Take(bytes, 8, &cursor, &fixed)) return false;
      column.descriptor_generation = U64(fixed);
      if (!TakeUuid(bytes, &cursor, &column.type_uuid) ||
          !Take(bytes, 8, &cursor, &fixed)) {
        return false;
      }
      column.type_generation = U64(fixed);
      if (!TakeText(bytes, &cursor, &column.codec_id) ||
          !Take(bytes, 2, &cursor, &fixed)) {
        return false;
      }
      column.codec_version = U16(fixed);
      if (!Take(bytes, 8, &cursor, &fixed)) return false;
      column.codec_generation = U64(fixed);
      if (!Take(bytes, 4, &cursor, &fixed)) return false;
      column.canonical_value_width = U32(fixed);
      if (!Take(bytes, 1, &cursor, &fixed)) return false;
      column.null_encoding = fixed[0];
      if (column.descriptor_generation == 0 || !Nonzero(column.type_uuid) ||
          column.type_generation == 0 || column.codec_id.empty() ||
          column.codec_version == 0 || column.codec_generation == 0 ||
          (column.null_encoding != 1 && column.null_encoding != 2)) {
        return false;
      }
    }
    if (!Nonzero(column.column_uuid) || !Nonzero(column.descriptor_uuid) ||
        column.canonical_name.empty() || column.descriptor_kind.empty() ||
        column.canonical_type_name.empty() ||
        !ordinals.insert(column.ordinal).second ||
        !column_uuids.insert(UuidText(column.column_uuid)).second) {
      return false;
    }
    projection.columns.push_back(std::move(column));
  }
  if (cursor != bytes.size()) return false;
  *output = std::move(projection);
  return true;
}

bool SameExpectedColumn(const MgaContextualTextProjectedColumnV2& expected,
                        const EnginePublicRelationProjectionColumnV3& projected) {
  const auto& descriptor = expected.expected_text_descriptor;
  const auto embedded_datatype_descriptor_uuid = ExactEncodedDescriptorField(
      projected.encoded_type_descriptor, "datatype_descriptor_uuid");
  const bool character_limit_matches =
      descriptor.character_limit == std::numeric_limits<std::uint64_t>::max()
          ? projected.character_length == 0
          : descriptor.character_limit <=
                    std::numeric_limits<std::uint32_t>::max() &&
                projected.character_length == descriptor.character_limit;
  return expected.column_ordinal == projected.ordinal &&
         expected.column_uuid == projected.column_uuid &&
         expected.comparable_persisted_text && projected.identity_present &&
         embedded_datatype_descriptor_uuid.has_value() &&
         *embedded_datatype_descriptor_uuid ==
             UuidText(expected.projected_datatype_descriptor_uuid) &&
         expected.projected_datatype_descriptor_generation ==
             projected.descriptor_generation &&
         expected.projected_datatype_catalog_snapshot_uuid ==
             descriptor.datatype_catalog_snapshot_uuid &&
         expected.projected_datatype_catalog_generation ==
             descriptor.datatype_catalog_generation &&
         expected.projected_datatype_registry_generation ==
             descriptor.datatype_registry_generation &&
         projected.canonical_type_name == "text" &&
         descriptor.descriptor_uuid ==
             expected.projected_datatype_descriptor_uuid &&
         projected.descriptor_generation == descriptor.descriptor_generation &&
         projected.type_uuid == descriptor.type_uuid &&
         projected.type_generation == descriptor.type_generation &&
         projected.codec_id == sblr::kContextualTextCodecIdentifierV2 &&
         projected.codec_version == descriptor.codec_version &&
         projected.codec_generation == descriptor.codec_generation &&
         projected.canonical_value_width == 0 &&
         projected.null_encoding == descriptor.null_encoding &&
         projected.charset_uuid == descriptor.charset_uuid &&
         projected.collation_uuid == descriptor.collation_uuid &&
         character_limit_matches;
}

bool DescriptorMatchesPolicy(const sblr::ContextualTextDescriptorV2& descriptor,
                             const EngineContextualTextPolicyRowSetV2& rows) {
  return descriptor.normalization_policy_uuid ==
             rows.normalization.identity_uuid &&
         descriptor.normalization_policy_generation ==
             rows.normalization.generation &&
         descriptor.render_policy_uuid == rows.render.identity_uuid &&
         descriptor.render_policy_generation == rows.render.generation &&
         descriptor.canonicalization_profile_uuid ==
             rows.canonicalization.identity_uuid &&
         descriptor.canonicalization_profile_generation ==
             rows.canonicalization.generation &&
         descriptor.comparison_contract_uuid ==
             rows.comparison.identity_uuid &&
         descriptor.comparison_contract_generation ==
             rows.comparison.generation &&
         descriptor.equality_operation_uuid == rows.equality.identity_uuid &&
         descriptor.equality_operation_generation ==
             rows.equality.generation;
}

class MgaSelector final
    : public EngineContextualTextTargetAuthoritySelectorV2 {
 public:
  bool SelectCoordinationPolicy(
      const EngineRequestContext& exact_live_context,
      EngineContextualTextCoordinationPolicyV2* out,
      EngineApiDiagnostic* diagnostic) const override {
    if (out == nullptr || !exact_live_context.security_context_present ||
        exact_live_context.resource_epoch == 0) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.DESCRIPTOR_INVALID",
            "engine.contextual_text_target.coordination_context_invalid");
      }
      return false;
    }
    *out = ExactCoordinationPolicy();
    if (!ValidCoordinationPolicy(*out)) {
      *out = {};
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.DESCRIPTOR_INVALID",
            "engine.contextual_text_target.coordination_registry_invalid");
      }
      return false;
    }
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }

  bool SelectTarget(
      const EngineRequestContext& exact_live_context,
      const sblr::ContextualTextLiteralDemandV2& structural_claim,
      const EngineContextualTextPolicyRowSetV2& exact_policy_rows,
      EngineContextualTextTargetSelectionV2* out,
      EngineApiDiagnostic* diagnostic) const override {
    if (out == nullptr) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.DESCRIPTOR_INVALID",
            "engine.contextual_text_target.selection_output_missing");
      }
      return false;
    }
    auto selected = SelectVisibleMgaContextualTextTargetV2(
        exact_live_context, structural_claim, exact_policy_rows);
    if (!selected.ok) {
      *out = {};
      if (diagnostic != nullptr) {
        *diagnostic = std::move(selected.diagnostic);
      }
      return false;
    }
    EngineContextualTextTargetSelectionV2 result;
    result.exact_public_relation_projection_v3 =
        std::move(selected.selection.exact_public_relation_projection_v3);
    result.sidecar_owner = std::move(selected.selection.sidecar_owner);
    result.base_descriptor_fields =
        std::move(selected.selection.base_descriptor_fields);
    result.projected_columns =
        std::move(selected.selection.projected_columns);
    result.sealed_sidecar_set =
        std::move(selected.selection.sealed_sidecar_set);
    *out = std::move(result);
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }
};

class Resolver final : public EngineContextualTextTargetAuthorityResolverV2 {
 public:
  Resolver(
      EngineRequestContext pinned_context,
      std::shared_ptr<const EngineContextualTextTargetAuthoritySelectorV2>
          selector)
      : pinned_context_(std::move(pinned_context)),
        selector_(std::move(selector)) {}

  bool BindBudget(
      const EngineRequestContext& context,
      const sblr::ContextualTextLiteralNegotiationRequestV2& request,
      std::size_t exact_request_bytes,
      EngineContextualTextLiteralBudgetV2* budget,
      EngineApiDiagnostic* diagnostic) const override {
    if (!ValidateContext(context, diagnostic) || budget == nullptr) {
      if (budget == nullptr && diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_target.budget_output_missing");
      }
      return false;
    }
    if (selector_ == nullptr) return MissingSelector(diagnostic);
    if (request.demands.empty() || exact_request_bytes == 0 ||
        exact_request_bytes >
            sblr::kContextualTextMaximumLogicalCarrierBytesV2) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
            "engine.contextual_text_target.request_budget_invalid");
      }
      return false;
    }
    const auto policy_lookup =
        LookupEngineContextualTextPolicyRowSetV2(context);
    if (!policy_lookup.ok || !PinPolicyRows(policy_lookup.rows, diagnostic)) {
      if (!policy_lookup.ok && diagnostic != nullptr) {
        *diagnostic = policy_lookup.diagnostic;
      }
      return false;
    }
    EngineContextualTextCoordinationPolicyV2 rows;
    EngineApiDiagnostic selected_diagnostic;
    if (!selector_->SelectCoordinationPolicy(
            context, &rows, &selected_diagnostic)) {
      if (diagnostic != nullptr) *diagnostic = std::move(selected_diagnostic);
      return false;
    }
    if (!PinCoordinationPolicy(rows, diagnostic)) return false;
    if (exact_request_bytes > rows.default_literal_negotiation_byte_grant) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
            "engine.contextual_text_target.request_grant_exceeded");
      }
      return false;
    }
    budget->literal_negotiation_byte_grant =
        rows.default_literal_negotiation_byte_grant;
    budget->canonical_body_aggregate_grant =
        rows.default_canonical_body_aggregate_grant;
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }

  bool ResolveTarget(
      const EngineRequestContext& context,
      const sblr::ContextualTextLiteralDemandV2& demand,
      EngineResolvedContextualTextTargetV2* target,
      EngineApiDiagnostic* diagnostic) const override {
    if (!ValidateContext(context, diagnostic) || target == nullptr) {
      if (target == nullptr && diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_target.output_missing");
      }
      return false;
    }
    if (selector_ == nullptr) return MissingSelector(diagnostic);
    const auto policy_lookup =
        LookupEngineContextualTextPolicyRowSetV2(context);
    if (!policy_lookup.ok || !PinPolicyRows(policy_lookup.rows, diagnostic)) {
      if (!policy_lookup.ok && diagnostic != nullptr) {
        *diagnostic = policy_lookup.diagnostic;
      }
      return false;
    }
    EngineContextualTextTargetSelectionV2 selected;
    EngineApiDiagnostic selected_diagnostic;
    if (!selector_->SelectTarget(context, demand, policy_lookup.rows, &selected,
                                 &selected_diagnostic)) {
      if (diagnostic != nullptr) *diagnostic = std::move(selected_diagnostic);
      return false;
    }
    EnginePublicRelationProjectionV3 projection;
    if (!DecodeProjectionV3(selected.exact_public_relation_projection_v3,
                            &projection) ||
        projection.relation_uuid != demand.relation_uuid ||
        projection.relation_descriptor_uuid !=
            demand.relation_descriptor_uuid ||
        projection.relation_descriptor_generation !=
            demand.relation_descriptor_generation ||
        projection.resource_epoch != context.resource_epoch ||
        UuidText(projection.catalog_snapshot_uuid) !=
            context.datatype_catalog_snapshot_uuid.canonical ||
        projection.catalog_generation != context.datatype_catalog_generation ||
        projection.registry_generation !=
            context.datatype_registry_generation ||
        selected.sidecar_owner.relation_uuid != demand.relation_uuid ||
        selected.sidecar_owner.relation_descriptor_uuid !=
            demand.relation_descriptor_uuid ||
        selected.sidecar_owner.relation_descriptor_generation !=
            demand.relation_descriptor_generation) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH",
            "engine.contextual_text_target.projection_mismatch");
      }
      return false;
    }

    const auto projected = std::ranges::find_if(
        projection.columns,
        [&](const EnginePublicRelationProjectionColumnV3& column) {
          return column.ordinal == demand.column_ordinal &&
                 column.column_uuid == demand.column_uuid;
        });
    const auto expected = std::ranges::find_if(
        selected.projected_columns,
        [&](const MgaContextualTextProjectedColumnV2& column) {
          return column.column_ordinal == demand.column_ordinal &&
                 column.column_uuid == demand.column_uuid;
        });
    if (projected == projection.columns.end() ||
        expected == selected.projected_columns.end() ||
        !SameExpectedColumn(*expected, *projected) ||
        expected->projected_resource_epoch != context.resource_epoch) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH",
            "engine.contextual_text_target.column_projection_mismatch");
      }
      return false;
    }

    MgaContextualTextSidecarLookupResultV2 lookup;
    MgaContextualTextSidecarSetDiagnosticV2 sidecar_diagnostic;
    if (!LookupMgaContextualTextSidecarV2(
            selected.sidecar_owner, selected.base_descriptor_fields,
            selected.projected_columns, selected.sealed_sidecar_set,
            demand.column_ordinal, demand.column_uuid, &lookup,
            &sidecar_diagnostic)) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            sidecar_diagnostic.code.empty()
                ? "CTB.TEXT.DESCRIPTOR_INVALID"
                : sidecar_diagnostic.code,
            "engine.contextual_text_target.sealed_sidecar_invalid",
            sidecar_diagnostic.detail);
      }
      return false;
    }
    if (lookup.exact_blob.size() != sblr::kContextualTextDescriptorBytesV2 ||
        lookup.descriptor.resource_epoch != context.resource_epoch ||
        lookup.descriptor.descriptor_evidence_sha256 !=
            lookup.descriptor_evidence_sha256 ||
        !DescriptorMatchesPolicy(lookup.descriptor, policy_lookup.rows)) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.DESCRIPTOR_INVALID",
            "engine.contextual_text_target.policy_or_sidecar_mismatch");
      }
      return false;
    }

    EngineResolvedContextualTextTargetV2 resolved;
    resolved.literal_occurrence = demand.literal_occurrence;
    resolved.exact_public_relation_projection_v3 =
        std::move(selected.exact_public_relation_projection_v3);
    resolved.exact_sbtltd02 = std::move(lookup.exact_blob);
    *target = std::move(resolved);
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }

  bool RevalidateTarget(
      const EngineRequestContext& context,
      const sblr::ContextualTextLiteralDemandV2& demand,
      const EngineResolvedContextualTextTargetV2& retained,
      EngineApiDiagnostic* diagnostic) const override {
    EngineResolvedContextualTextTargetV2 current;
    if (!ResolveTarget(context, demand, &current, diagnostic)) return false;
    if (current.literal_occurrence != retained.literal_occurrence ||
        current.exact_public_relation_projection_v3 !=
            retained.exact_public_relation_projection_v3 ||
        current.exact_sbtltd02 != retained.exact_sbtltd02) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
            "engine.contextual_text_target.revalidation_stale");
      }
      return false;
    }
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }

 private:
  bool ValidateContext(const EngineRequestContext& context,
                       EngineApiDiagnostic* diagnostic) const {
    if (SameContext(pinned_context_, context)) return true;
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "ENGINE.STATEMENT_CONTEXT.RECEIPT_MISMATCH",
          "engine.contextual_text_target.context_mismatch");
    }
    return false;
  }

  bool MissingSelector(EngineApiDiagnostic* diagnostic) const {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.DESCRIPTOR_INVALID",
          "engine.contextual_text_target.selector_unavailable",
          "no engine-owned sealed-MGA and contextual-policy selector is "
          "installed for this receipt");
    }
    return false;
  }

  bool PinPolicyRows(const EngineContextualTextPolicyRowSetV2& rows,
                     EngineApiDiagnostic* diagnostic) const {
    std::lock_guard<std::mutex> guard(policy_mutex_);
    if (!pinned_policy_rows_.has_value()) {
      pinned_policy_rows_ = rows;
      return true;
    }
    if (*pinned_policy_rows_ == rows) return true;
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
          "engine.contextual_text_target.policy_snapshot_stale");
    }
    return false;
  }

  bool PinCoordinationPolicy(
      const EngineContextualTextCoordinationPolicyV2& policy,
      EngineApiDiagnostic* diagnostic) const {
    if (!ValidCoordinationPolicy(policy)) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.DESCRIPTOR_INVALID",
            "engine.contextual_text_target.coordination_policy_invalid");
      }
      return false;
    }
    std::lock_guard<std::mutex> guard(policy_mutex_);
    if (!pinned_coordination_policy_.has_value()) {
      pinned_coordination_policy_ = policy;
      return true;
    }
    if (SameCoordinationPolicy(*pinned_coordination_policy_, policy)) {
      return true;
    }
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
          "engine.contextual_text_target.coordination_policy_stale");
    }
    return false;
  }

  EngineRequestContext pinned_context_;
  std::shared_ptr<const EngineContextualTextTargetAuthoritySelectorV2>
      selector_;
  mutable std::mutex policy_mutex_;
  mutable std::optional<EngineContextualTextPolicyRowSetV2>
      pinned_policy_rows_;
  mutable std::optional<EngineContextualTextCoordinationPolicyV2>
      pinned_coordination_policy_;
};

}  // namespace

bool DecodeEnginePublicRelationProjectionV3(
    const std::vector<std::uint8_t>& exact,
    EnginePublicRelationProjectionV3* out,
    EngineApiDiagnostic* diagnostic) {
  if (!DecodeProjectionV3(exact, out)) {
    if (out != nullptr) *out = {};
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_target.projection_v3_decode_invalid");
    }
    return false;
  }
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

bool EncodeEnginePublicRelationProjectionV3(
    const EnginePublicRelationProjectionV3& projection,
    std::vector<std::uint8_t>* exact,
    EngineApiDiagnostic* diagnostic) {
  if (exact == nullptr || !Nonzero(projection.relation_descriptor_uuid) ||
      !Nonzero(projection.relation_uuid) || !Nonzero(projection.schema_uuid) ||
      projection.relation_descriptor_generation == 0 ||
      projection.resource_epoch == 0 ||
      !Nonzero(projection.catalog_snapshot_uuid) ||
      projection.catalog_generation == 0 ||
      projection.registry_generation == 0 || projection.columns.empty() ||
      projection.columns.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    if (exact != nullptr) exact->clear();
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_target.projection_v3_encode_invalid");
    }
    return false;
  }

  std::vector<std::uint8_t> encoded;
  std::set<std::uint32_t> ordinals;
  std::set<std::string> column_uuids;
  const auto fail_text_encoding = [&]() {
    exact->clear();
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_target.projection_v3_text_invalid");
    }
    return false;
  };
  try {
    AppendUuid(&encoded, projection.relation_descriptor_uuid);
    AppendUuid(&encoded, projection.relation_uuid);
    AppendUuid(&encoded, projection.schema_uuid);
    AppendU64(&encoded, projection.relation_descriptor_generation);
    AppendU64(&encoded, projection.resource_epoch);
    AppendUuid(&encoded, projection.catalog_snapshot_uuid);
    AppendU64(&encoded, projection.catalog_generation);
    AppendU64(&encoded, projection.registry_generation);
    AppendU32(&encoded,
              static_cast<std::uint32_t>(projection.columns.size()));
    for (const auto& column : projection.columns) {
      if (!Nonzero(column.column_uuid) || !Nonzero(column.descriptor_uuid) ||
          column.canonical_name.empty() || column.descriptor_kind.empty() ||
          column.canonical_type_name.empty() ||
          !ordinals.insert(column.ordinal).second ||
          !column_uuids.insert(UuidText(column.column_uuid)).second ||
          (column.identity_present &&
           (column.descriptor_generation == 0 || !Nonzero(column.type_uuid) ||
            column.type_generation == 0 || column.codec_id.empty() ||
            column.codec_version == 0 || column.codec_generation == 0 ||
            (column.null_encoding != 1 && column.null_encoding != 2)))) {
        exact->clear();
        if (diagnostic != nullptr) {
          *diagnostic = Diagnostic(
              "SBLR.OPERAND_INVALID",
              "engine.contextual_text_target.projection_v3_column_invalid");
        }
        return false;
      }
      AppendUuid(&encoded, column.column_uuid);
      AppendU32(&encoded, column.ordinal);
      if (!AppendText(&encoded, column.canonical_name))
        return fail_text_encoding();
      AppendUuid(&encoded, column.descriptor_uuid);
      if (!AppendText(&encoded, column.descriptor_kind) ||
          !AppendText(&encoded, column.canonical_type_name) ||
          !AppendText(&encoded, column.encoded_type_descriptor)) {
        return fail_text_encoding();
      }
      encoded.push_back(column.attributes);
      AppendUuid(&encoded, column.charset_uuid);
      if (!AppendText(&encoded, column.charset_name))
        return fail_text_encoding();
      AppendUuid(&encoded, column.collation_uuid);
      if (!AppendText(&encoded, column.collation_name))
        return fail_text_encoding();
      AppendU32(&encoded, column.character_length);
      AppendU32(&encoded, column.charset_min_bytes);
      AppendU32(&encoded, column.charset_max_bytes);
      encoded.push_back(column.identity_present ? 1 : 0);
      if (column.identity_present) {
        AppendU64(&encoded, column.descriptor_generation);
        AppendUuid(&encoded, column.type_uuid);
        AppendU64(&encoded, column.type_generation);
        if (!AppendText(&encoded, column.codec_id))
          return fail_text_encoding();
        AppendU16(&encoded, column.codec_version);
        AppendU64(&encoded, column.codec_generation);
        AppendU32(&encoded, column.canonical_value_width);
        encoded.push_back(column.null_encoding);
      }
    }
  } catch (const std::bad_alloc&) {
    exact->clear();
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "ENGINE.RESOURCE.EXHAUSTED",
          "engine.contextual_text_target.projection_v3_encode_allocation_failed");
    }
    return false;
  }
  EnginePublicRelationProjectionV3 decoded;
  if (!DecodeProjectionV3(encoded, &decoded)) {
    exact->clear();
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_target.projection_v3_encode_noncanonical");
    }
    return false;
  }
  *exact = std::move(encoded);
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

bool ValidateEngineContextualTextCoordinationPolicyV2(
    const EngineContextualTextCoordinationPolicyV2& rows,
    EngineApiDiagnostic* diagnostic) {
  if (!ValidCoordinationPolicy(rows)) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.DESCRIPTOR_INVALID",
          "engine.contextual_text_target.coordination_policy_invalid");
    }
    return false;
  }
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

bool InstallEngineContextualTextTargetAuthoritySelectorProviderV2(
    std::shared_ptr<
        const EngineContextualTextTargetAuthoritySelectorProviderV2> provider,
    EngineApiDiagnostic* diagnostic) {
  if (provider == nullptr) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_target.provider_missing");
    }
    return false;
  }
  std::lock_guard<std::mutex> guard(g_selector_provider_mutex);
  if (g_selector_provider != nullptr &&
      g_selector_provider.get() != provider.get()) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "ENGINE.INTERNAL.CONFLICT",
          "engine.contextual_text_target.provider_replacement_refused");
    }
    return false;
  }
  g_selector_provider = std::move(provider);
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

std::unique_ptr<EngineContextualTextTargetAuthorityResolverV2>
CreateEngineContextualTextTargetAuthorityResolverForReceiptV2(
    const EngineRequestContext& exact_live_context) {
  try {
    std::shared_ptr<
        const EngineContextualTextTargetAuthoritySelectorProviderV2>
        provider;
    {
      std::lock_guard<std::mutex> guard(g_selector_provider_mutex);
      provider = g_selector_provider;
    }
    std::shared_ptr<const EngineContextualTextTargetAuthoritySelectorV2>
        selector;
    if (provider != nullptr) {
      selector = provider->SelectForReceipt(exact_live_context);
    } else {
      selector = std::make_shared<MgaSelector>();
    }
    return std::make_unique<Resolver>(exact_live_context,
                                      std::move(selector));
  } catch (...) {
    return nullptr;
  }
}

}  // namespace scratchbird::engine::internal_api
