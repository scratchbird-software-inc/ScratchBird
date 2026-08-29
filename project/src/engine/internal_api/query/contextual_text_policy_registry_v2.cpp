// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/contextual_text_policy_registry_v2.hpp"

#include "api_diagnostics.hpp"
#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace datatypes = scratchbird::core::datatypes;
namespace uuid = scratchbird::core::uuid;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kCatalogSnapshotUuid =
    "019d0000-0000-7000-8000-00000000d701";

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

bool ExactNonNilUuid(std::string_view text) {
  if (text.empty()) return false;
  const auto parsed = uuid::ParseUuid(std::string(text));
  return parsed.ok() && !uuid::IsNilUuid(parsed.value) &&
         uuid::UuidToString(parsed.value) == text;
}

bool ToWireUuid(std::string_view text, sblr::ContextualTextUuidV2* out) {
  if (out == nullptr || !ExactNonNilUuid(text)) return false;
  const auto parsed = uuid::ParseUuid(std::string(text));
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
  return true;
}

EngineContextualTextPolicyRowV2 Row(
    std::string_view row_id,
    std::string_view identity_uuid,
    EngineContextualTextPolicyKindV2 kind,
    std::string_view exact_semantic_contract) {
  EngineContextualTextPolicyRowV2 row;
  row.row_id = row_id;
  (void)ToWireUuid(identity_uuid, &row.identity_uuid);
  row.generation = 1;
  row.kind = kind;
  row.status = "accepted";
  row.exact_semantic_contract = exact_semantic_contract;
  return row;
}

EngineContextualTextPolicyRowSetV2 ExactRows() {
  EngineContextualTextPolicyRowSetV2 rows;
  rows.normalization = Row(
      "text.normalization.unicode_scalar_identity.v1",
      "35342fdb-2a81-5dfb-8dc8-c99b78acde3c",
      EngineContextualTextPolicyKindV2::normalization_policy,
      "well_formed_Unicode_scalar_sequence:identity_no_normalization");
  rows.render = Row(
      "text.render.canonical_utf8.v1",
      "c5fb802c-17b7-5737-8ee8-0abf3ad2900f",
      EngineContextualTextPolicyKindV2::render_policy,
      "exact_canonical_UTF8_bytes:metadata_only");
  rows.canonicalization = Row(
      "text.canonicalization.contextual_literal.v2",
      "233c7c3d-7454-52b6-8c8e-cfda48f8c683",
      EngineContextualTextPolicyKindV2::canonicalization_profile,
      "shortest_UTF8:scalar_count:identity_normalization:target_limits:"
      "d71a_v1_generation_1:decode_reencode_identity");
  rows.comparison = Row(
      "text.comparison.descriptor_collated_equality.v1",
      "835325ab-4c2b-5d07-aa03-271a9bd3a79c",
      EngineContextualTextPolicyKindV2::comparison_contract,
      "exact_live_TEXT_descriptors:target_collation:SQL_three_valued_equality");
  rows.equality = Row(
      "text.equality_operation.sb_operator_equal.v1",
      "019de5fc-2400-7b73-9c38-dcf10204dbde",
      EngineContextualTextPolicyKindV2::equality_operation_binding,
      "sb.operator.equal:contextual_TEXT_overload");
  (void)ToWireUuid("019d0000-0000-7000-8000-00000000d720",
                   &rows.equality_operator_snapshot_uuid);
  rows.equality_operator_registry_generation = 1;
  return rows;
}

bool LiveContextAdmitted(const EngineRequestContext& context) {
  return context.security_context_present && context.resource_epoch != 0 &&
         ExactNonNilUuid(context.database_uuid.canonical) &&
         ExactNonNilUuid(context.session_uuid.canonical) &&
         ExactNonNilUuid(context.statement_uuid.canonical) &&
         ExactNonNilUuid(context.statement_receipt_uuid.canonical) &&
         ExactNonNilUuid(context.statement_snapshot_uuid.canonical) &&
         context.datatype_catalog_snapshot_uuid.canonical ==
             kCatalogSnapshotUuid &&
         context.datatype_catalog_generation == 1 &&
         context.datatype_registry_generation == 1;
}

bool ExactBuiltinEqualitySnapshot(
    const EngineContextualTextPolicyRowSetV2& rows) {
  const auto current =
      datatypes::LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();
  sblr::ContextualTextUuidV2 snapshot{};
  sblr::ContextualTextUuidV2 equality{};
  return current.ok &&
         ToWireUuid(current.snapshot_uuid, &snapshot) &&
         ToWireUuid(current.equality_operator_uuid, &equality) &&
         snapshot == rows.equality_operator_snapshot_uuid &&
         current.registry_generation ==
             rows.equality_operator_registry_generation &&
         equality == rows.equality.identity_uuid &&
         current.equality_operator_generation == rows.equality.generation;
}

}  // namespace

EngineContextualTextPolicyLookupResultV2
LookupEngineContextualTextPolicyRowSetV2(
    const EngineRequestContext& exact_live_context) {
  EngineContextualTextPolicyLookupResultV2 result;
  if (!LiveContextAdmitted(exact_live_context)) {
    result.diagnostic = Diagnostic(
        "CTB.TEXT.DESCRIPTOR_INVALID",
        "engine.contextual_text_policy.live_context_invalid");
    return result;
  }
  result.rows = ExactRows();
  if (!ExactBuiltinEqualitySnapshot(result.rows)) {
    result.diagnostic = Diagnostic(
        "CTB.TEXT.DESCRIPTOR_INVALID",
        "engine.contextual_text_policy.equality_registry_stale");
    result.rows = {};
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineContextualTextPolicyLookupResultV2
LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2() {
  EngineContextualTextPolicyLookupResultV2 result;
  result.rows = ExactRows();
  if (!ExactBuiltinEqualitySnapshot(result.rows)) {
    result.diagnostic = Diagnostic(
        "CTB.TEXT.DESCRIPTOR_INVALID",
        "engine.contextual_text_policy.equality_registry_stale");
    result.rows = {};
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

bool RevalidateEngineContextualTextPolicyRowSetV2(
    const EngineRequestContext& exact_live_context,
    const EngineContextualTextPolicyRowSetV2& retained,
    EngineApiDiagnostic* diagnostic) {
  const auto current =
      LookupEngineContextualTextPolicyRowSetV2(exact_live_context);
  if (!current.ok) {
    if (diagnostic != nullptr) *diagnostic = current.diagnostic;
    return false;
  }
  if (!(current.rows == retained)) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
          "engine.contextual_text_policy.row_set_stale");
    }
    return false;
  }
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

const EngineContextualTextPolicyRowV2* FindEngineContextualTextPolicyRowV2(
    const EngineContextualTextPolicyRowSetV2& rows,
    const EngineContextualTextPolicyKindV2 kind) {
  switch (kind) {
    case EngineContextualTextPolicyKindV2::normalization_policy:
      return &rows.normalization;
    case EngineContextualTextPolicyKindV2::render_policy:
      return &rows.render;
    case EngineContextualTextPolicyKindV2::canonicalization_profile:
      return &rows.canonicalization;
    case EngineContextualTextPolicyKindV2::comparison_contract:
      return &rows.comparison;
    case EngineContextualTextPolicyKindV2::equality_operation_binding:
      return &rows.equality;
  }
  return nullptr;
}

}  // namespace scratchbird::engine::internal_api
