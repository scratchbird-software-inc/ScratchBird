// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_text_target_authority_provider.hpp"
#include "api_diagnostics.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include <algorithm>
#include <tuple>
#include <limits>

namespace scratchbird::engine::internal_api {

struct EngineDmlUpdateTextTargetHandleV2::Authority {
  EngineRequestContext context;
  EngineDmlUpdateTextTargetSnapshotV2 snapshot;
};

namespace {
EngineApiDiagnostic Refuse(std::string detail,
                          std::string code = "CTB.TEXT.DESCRIPTOR_INVALID") {
  return MakeEngineApiDiagnostic(std::move(code),
      "sblr.dml_update_rows.text_target_authority", std::move(detail), true);
}

auto ContextKey(const EngineRequestContext& c) {
  return std::tie(c.database_path, c.database_uuid,
      c.session_uuid, c.principal_uuid,
      c.transaction_uuid, c.local_transaction_id,
      c.statement_uuid, c.statement_receipt_uuid,
      c.statement_snapshot_uuid, c.statement_metadata_snapshot_uuid,
      c.statement_snapshot_generation, c.snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_active_excluded_local_transaction_ids,
      c.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids,
      c.statement_metadata_snapshot_engine_owned, c.catalog_generation_id,
      c.catalog_epoch_uuid, c.current_role_uuid,
      c.transaction_isolation_level, c.resource_admission_uuid,
      c.transaction_policy_snapshot_uuid,
      c.transaction_policy_snapshot_generation, c.read_only_mode,
      c.cluster_transaction_active, c.route_fence_present,
      c.datatype_catalog_snapshot_uuid, c.datatype_catalog_generation,
      c.datatype_registry_generation, c.resource_epoch, c.security_epoch,
      c.security_context_present, c.authorization_context.present,
      c.authorization_context.authority_uuid,
      c.authorization_context.security_context_generation,
      c.authorization_context.principal_uuid,
      c.authorization_context.catalog_generation_id,
      c.authorization_context.security_epoch, c.authorization_context.policy_epoch);
}

auto ResourceKey(const EngineResolvedResourceDescriptor& r) {
  return std::tie(r.present, r.resource_family, r.canonical_name,
      r.resource_uuid, r.parent_resource_uuid,
      r.parent_canonical_name, r.default_collation_uuid,
      r.default_collation_name, r.seed_pack_name, r.seed_pack_version,
      r.resource_epoch, r.family_epoch, r.family_version,
      r.min_bytes, r.max_bytes, r.variable_width, r.default_for_parent,
      r.case_insensitive, r.accent_insensitive);
}
}  // namespace

const EngineDmlUpdateTextTargetSnapshotV2*
EngineDmlUpdateTextTargetHandleV2::snapshot() const noexcept {
  return authority_ ? &authority_->snapshot : nullptr;
}

EngineDmlUpdateTextTargetCaptureResultV2 CaptureDmlUpdateTextTargetV2(
    const EngineRequestContext& context, const MgaTextColumnKeyV2& key,
    std::uint64_t expected_column_generation) {
  EngineDmlUpdateTextTargetCaptureResultV2 result;
  if (context.cluster_transaction_active || context.route_fence_present) {
    result.diagnostic = Refuse("TEXT target authority requires an unfenced local MGA transaction",
                               "SBLR.OPERATION_UNSUPPORTED");
    return result;
  }
  if (!context.security_context_present || !context.authorization_context.present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !CanonicalNonNilMigrationUuid(context.statement_metadata_snapshot_uuid) ||
      !CanonicalNonNilMigrationUuid(context.statement_uuid) ||
      !CanonicalNonNilMigrationUuid(context.statement_receipt_uuid) ||
      !CanonicalNonNilMigrationUuid(context.statement_snapshot_uuid) ||
      !CanonicalNonNilMigrationUuid(context.authorization_context.authority_uuid) ||
      context.authorization_context.security_context_generation == 0 ||
      context.authorization_context.principal_uuid != context.principal_uuid ||
      context.authorization_context.catalog_generation_id != context.catalog_generation_id ||
      context.authorization_context.security_epoch != context.security_epoch ||
      context.authorization_context.policy_epoch == 0 || context.security_epoch == 0 ||
      context.local_transaction_id == 0 || context.resource_epoch == 0 ||
      expected_column_generation == 0) {
    result.diagnostic = Refuse("exact authenticated statement and column authority required");
    return result;
  }
  const auto policies = LookupEngineContextualTextPolicyRowSetV2(context);
  if (!policies.ok) {
    result.diagnostic = policies.diagnostic;
    return result;
  }
  const auto relation = LoadMgaRelationStorageDescriptor(
      context, ContextualUuidTextV2(key.relation_uuid));
  if (!relation.ok) {
    result.diagnostic = relation.diagnostic;
    return result;
  }
  const auto column = std::find_if(relation.descriptor.columns.begin(),
      relation.descriptor.columns.end(), [&](const auto& c) {
        return c.column_uuid == ContextualUuidTextV2(key.column_uuid) &&
               c.ordinal == key.column_ordinal &&
               c.column_generation == expected_column_generation;
      });
  if (column == relation.descriptor.columns.end()) {
    result.diagnostic = Refuse("exact visible column generation required");
    return result;
  }
  const auto selected = SelectVisibleMgaTextAssignmentColumnV2(context, key, policies.rows);
  if (!selected.ok) {
    result.diagnostic = selected.diagnostic;
    return result;
  }
  MgaContextualTextSidecarLookupResultV2 sidecar;
  MgaContextualTextSidecarSetDiagnosticV2 error;
  const bool contextual = !column->charset_uuid.empty() || !column->collation_uuid.empty();
  if (contextual && !LookupMgaContextualTextSidecarV2(selected.selection.sidecar_owner,
          selected.selection.base_descriptor_fields, selected.selection.projected_columns,
          selected.selection.sealed_sidecar_set, key.column_ordinal, key.column_uuid,
          &sidecar, &error)) {
    result.diagnostic = Refuse(error.detail);
    return result;
  }
  const auto& descriptor = sidecar.descriptor;
  EngineResolvedResourceDescriptor cs;
  EngineResolvedResourceDescriptor co;
  if (contextual) {
    const auto charset = LookupEngineResourceDescriptorByUuid(
        context, EngineUuid{ContextualUuidTextV2(descriptor.charset_uuid)}, "charset");
    if (!charset.ok) { result.diagnostic = charset.diagnostic; return result; }
    const auto collation = LookupEngineResourceDescriptorByUuid(
        context, EngineUuid{ContextualUuidTextV2(descriptor.collation_uuid)}, "collation");
    if (!collation.ok) { result.diagnostic = collation.diagnostic; return result; }
    cs = charset.resource_descriptor;
    co = collation.resource_descriptor;
  } else {
    // The exact d718/d71a row already fixes UTF-8 assignment encoding. Resolve
    // its live engine resource; do not fabricate a collation or a contextual
    // descriptor for a persisted column which has neither.
    EngineResolveNameRequest request;
    request.context = context;
    request.sql_object_reference.expected_object_type = "charset";
    request.sql_object_reference.object_name.raw_text = "utf8";
    const auto charset = EngineResolveName(request);
    if (!charset.ok) {
      result.diagnostic = charset.diagnostics.empty()
          ? Refuse("live canonical UTF-8 resource is unavailable") : charset.diagnostics.front();
      return result;
    }
    cs = charset.resource_descriptor;
  }
  if (contextual && (descriptor.resource_epoch != context.resource_epoch ||
      !cs.present || !co.present || cs.resource_family != "charset" ||
      co.resource_family != "collation" ||
      cs.resource_uuid != ContextualUuidTextV2(descriptor.charset_uuid) ||
      co.resource_uuid != ContextualUuidTextV2(descriptor.collation_uuid) ||
      cs.resource_epoch != context.resource_epoch ||
      co.resource_epoch != context.resource_epoch ||
      cs.family_epoch != descriptor.charset_generation ||
      co.family_epoch != descriptor.collation_generation ||
      co.parent_resource_uuid != cs.resource_uuid ||
      co.parent_canonical_name != cs.canonical_name ||
      co.canonical_name.empty() || co.seed_pack_name.empty() || co.seed_pack_version.empty())) {
    result.diagnostic = Refuse("sealed descriptor and live locale resource identity differ",
                               "CTB.TEXT.RESOURCE_EPOCH_MISMATCH");
    return result;
  }
  // Interpret only the exact live catalogue row, never a caller spelling or
  // a width-based guess. Other charsets need a separately admitted conversion.
  if (!cs.present || cs.resource_family != "charset" ||
      cs.resource_epoch != context.resource_epoch || cs.family_epoch == 0 ||
      !CanonicalNonNilMigrationUuid(cs.resource_uuid) ||
      cs.seed_pack_name.empty() || cs.seed_pack_version.empty() ||
      cs.canonical_name != "UTF-8" || cs.min_bytes != 1 || cs.max_bytes != 4 ||
      !cs.variable_width) {
    result.diagnostic = Refuse("TEXT UPDATE requires the live canonical UTF-8 resource",
                               "SBLR.OPERATION_UNSUPPORTED");
    return result;
  }
  auto authority = std::make_shared<EngineDmlUpdateTextTargetHandleV2::Authority>();
  authority->context = context;
  authority->snapshot.key = key;
  authority->snapshot.column_generation = expected_column_generation;
  authority->snapshot.nullable = column->nullable;
  authority->snapshot.contextual = contextual;
  authority->snapshot.exact_persisted_value_descriptor = column->value_descriptor.encoded_descriptor;
  authority->snapshot.character_limit = contextual ? descriptor.character_limit :
      (column->character_length == 0 ? std::numeric_limits<std::uint64_t>::max() : column->character_length);
  if (!contextual && column->character_length > std::numeric_limits<std::uint64_t>::max() / cs.max_bytes) {
    result.diagnostic = Refuse("TEXT target byte limit overflows");
    return result;
  }
  authority->snapshot.byte_limit = contextual ? descriptor.byte_limit :
      (column->character_length == 0 ? std::numeric_limits<std::uint64_t>::max() : column->character_length * cs.max_bytes);
  authority->snapshot.descriptor = descriptor;
  authority->snapshot.exact_relation_projection =
      selected.selection.exact_public_relation_projection_v3;
  authority->snapshot.charset = cs;
  authority->snapshot.collation = co;
  result.handle.authority_ = std::move(authority);
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

EngineApiDiagnostic RevalidateDmlUpdateTextTargetV2(
    const EngineRequestContext& context,
    const EngineDmlUpdateTextTargetHandleV2& handle) {
  if (!handle.valid()) return Refuse("TEXT target handle is absent");
  if (ContextKey(context) != ContextKey(handle.authority_->context)) {
    return Refuse("TEXT target handle belongs to another statement authority",
                  "MGA.TRANSACTION.STALE");
  }
  const auto& retained = handle.authority_->snapshot;
  const auto current = CaptureDmlUpdateTextTargetV2(
      context, retained.key, retained.column_generation);
  if (!current.ok) return current.diagnostic;
  const auto& fresh = *current.handle.snapshot();
  if (fresh.nullable != retained.nullable ||
      fresh.contextual != retained.contextual ||
      fresh.byte_limit != retained.byte_limit ||
      fresh.character_limit != retained.character_limit ||
      fresh.exact_persisted_value_descriptor != retained.exact_persisted_value_descriptor ||
      fresh.descriptor.exact_bytes != retained.descriptor.exact_bytes ||
      fresh.exact_relation_projection != retained.exact_relation_projection ||
      ResourceKey(fresh.charset) != ResourceKey(retained.charset) ||
      ResourceKey(fresh.collation) != ResourceKey(retained.collation)) {
    return Refuse("TEXT target descriptor or locale authority changed",
                  "CTB.TEXT.RESOURCE_EPOCH_MISMATCH");
  }
  return current.diagnostic;
}

}  // namespace scratchbird::engine::internal_api
