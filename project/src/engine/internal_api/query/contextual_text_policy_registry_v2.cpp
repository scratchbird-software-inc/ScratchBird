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

constexpr EngineUuid kCatalogSnapshotUuid =
    EngineUuid{{0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x01}};

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

bool ExactNonNilUuid(const EngineUuid& id) {
  return !id.is_nil();
}

bool ToWireUuid(const EngineUuid& id, sblr::ContextualTextUuidV2* out) {
  if(out==nullptr||!ExactNonNilUuid(id))return false;
  *out=id.bytes;return true;
}

EngineContextualTextPolicyRowV2 Row(
    std::string_view row_id,
    const EngineUuid& identity_uuid,
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
      EngineUuid{{0x35,0x34,0x2f,0xdb,0x2a,0x81,0x5d,0xfb,0x8d,0xc8,0xc9,0x9b,0x78,0xac,0xde,0x3c}},
      EngineContextualTextPolicyKindV2::normalization_policy,
      "well_formed_Unicode_scalar_sequence:identity_no_normalization");
  rows.render = Row(
      "text.render.canonical_utf8.v1",
      EngineUuid{{0xc5,0xfb,0x80,0x2c,0x17,0xb7,0x57,0x37,0x8e,0xe8,0x0a,0xbf,0x3a,0xd2,0x90,0x0f}},
      EngineContextualTextPolicyKindV2::render_policy,
      "exact_canonical_UTF8_bytes:metadata_only");
  rows.canonicalization = Row(
      "text.canonicalization.contextual_literal.v2",
      EngineUuid{{0x23,0x3c,0x7c,0x3d,0x74,0x54,0x52,0xb6,0x8c,0x8e,0xcf,0xda,0x48,0xf8,0xc6,0x83}},
      EngineContextualTextPolicyKindV2::canonicalization_profile,
      "shortest_UTF8:scalar_count:identity_normalization:target_limits:"
      "d71a_v1_generation_1:decode_reencode_identity");
  rows.comparison = Row(
      "text.comparison.descriptor_collated_equality.v1",
      EngineUuid{{0x83,0x53,0x25,0xab,0x4c,0x2b,0x5d,0x07,0xaa,0x03,0x27,0x1a,0x9b,0xd3,0xa7,0x9c}},
      EngineContextualTextPolicyKindV2::comparison_contract,
      "exact_live_TEXT_descriptors:target_collation:SQL_three_valued_equality");
  rows.equality = Row(
      "text.equality_operation.sb_operator_equal.v1",
      EngineUuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x7b,0x73,0x9c,0x38,0xdc,0xf1,0x02,0x04,0xdb,0xde}},
      EngineContextualTextPolicyKindV2::equality_operation_binding,
      "sb.operator.equal:contextual_TEXT_overload");
  (void)ToWireUuid(EngineUuid{{0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x20}},
                   &rows.equality_operator_snapshot_uuid);
  rows.equality_operator_registry_generation = 1;
  return rows;
}

bool LiveContextAdmitted(const EngineRequestContext& context) {
  return context.security_context_present && context.resource_epoch != 0 &&
         ExactNonNilUuid(context.database_uuid) &&
         ExactNonNilUuid(context.session_uuid) &&
         ExactNonNilUuid(context.statement_uuid) &&
         ExactNonNilUuid(context.statement_receipt_uuid) &&
         ExactNonNilUuid(context.statement_snapshot_uuid) &&
         context.datatype_catalog_snapshot_uuid ==
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
