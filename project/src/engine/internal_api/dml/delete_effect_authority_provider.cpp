// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_effect_authority_provider.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "dml/mutation_savepoint_capability.hpp"
#include "dml/transactional_relation_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"
#include "hash_digest.hpp"
#include <chrono>
#include <tuple>

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteEffectAuthorityHandleV1::Authority {
  EngineRequestContext owner;
  EngineDmlDeleteEffectSnapshotV1 snapshot;
};
namespace {
namespace projection = datatype_operator_projection;
EngineApiDiagnostic Error(std::string detail, std::string code = "SBLR.OPERATION_UNSUPPORTED") {
  return MakeEngineApiDiagnostic(std::move(code), "sblr.dml_delete_rows.effect_authority_refused",
                                 std::move(detail), true);
}
EngineApiDiagnostic Ok() { return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false); }
EngineApiDiagnostic ValidateContext(const EngineRequestContext& c, std::string_view phase) {
  const auto has = [&](std::string_view tag) {
    return std::find(c.trace_tags.begin(), c.trace_tags.end(), tag) != c.trace_tags.end();
  };
  if (!has(phase) || !c.security_context_present || !c.authorization_context.present ||
      !c.statement_metadata_snapshot_engine_owned || c.database_path.empty())
    return Error("private_DELETE_phase_required", "SECURITY.ACCESS_DENIED");
  for (const auto tag : {"private_dml_delete_rows_binder", "private_dml_delete_rows_consumer",
                         "private_dml_delete_rows_recovery", "private_dml_update_rows_binder",
                         "private_dml_update_rows_consumer", "private_dml_update_rows_recovery"})
    if (tag != phase && has(tag)) return Error("mixed_operation_or_phase", "SECURITY.ACCESS_DENIED");
  wire::TypedUpdateUuid id{};
  for (const auto* value : {&c.database_uuid, &c.session_uuid, &c.principal_uuid, &c.transaction_uuid,
                           &c.statement_receipt_uuid, &c.statement_snapshot_uuid,
                           &c.statement_metadata_snapshot_uuid, &c.authorization_context.authority_uuid})
    if (!projection::TypedUuid(value->canonical, &id)) return Error("owner_identity", "MGA.TRANSACTION.STALE");
  if (!c.local_transaction_id || !c.catalog_generation_id || !c.security_epoch ||
      !c.authorization_context.security_context_generation ||
      c.authorization_context.principal_uuid != c.principal_uuid ||
      c.read_only_mode || c.cluster_transaction_active || c.route_fence_present)
    return Error("owner_generation_or_write_fence", "MGA.TRANSACTION.STALE");
  return Ok();
}
auto Owner(const EngineRequestContext& c) {
  return std::tie(c.database_path, c.database_uuid, c.session_uuid,
      c.principal_uuid, c.current_role_uuid, c.transaction_uuid,
      c.local_transaction_id, c.statement_receipt_uuid, c.statement_snapshot_uuid,
      c.statement_snapshot_generation, c.snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_uuid, c.statement_metadata_snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_active_excluded_local_transaction_ids,
      c.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids,
      c.catalog_generation_id, c.security_epoch, c.authorization_context.authority_uuid,
      c.authorization_context.security_context_generation, c.authorization_context.policy_epoch,
      c.datatype_catalog_snapshot_uuid, c.datatype_catalog_generation, c.datatype_registry_generation,
      c.transaction_isolation_level);
}
void Number(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned n = 0; n < 8; ++n) bytes->push_back(static_cast<std::uint8_t>(value >> (8 * n)));
}
void Text(std::vector<std::uint8_t>* bytes, std::string_view value) {
  Number(bytes, value.size()); bytes->insert(bytes->end(), value.begin(), value.end());
}
bool Identity(std::vector<std::uint8_t>* bytes, const std::string& value, bool optional = false) {
  wire::TypedUpdateUuid id{};
  if (!(optional && value.empty()) && !projection::TypedUuid(value, &id)) return false;
  bytes->insert(bytes->end(), id.begin(), id.end()); return true;
}
wire::TypedUpdateHash Hash(std::string_view domain, const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> input(domain.begin(), domain.end());
  input.insert(input.end(), bytes.begin(), bytes.end());
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(input);
  return hash.ok() ? hash.digest : wire::TypedUpdateHash{};
}
bool Nonzero(const wire::TypedUpdateHash& hash) {
  return std::any_of(hash.begin(), hash.end(), [](auto b) { return b != 0; });
}
EngineApiDiagnostic Project(const EngineRequestContext& context, const std::string& target,
                            EngineDmlDeleteEffectSnapshotV1* snapshot) {
  auto diagnostic = AdmitMgaDmlSavepointMutation(context, target, MgaDmlMutationKind::delete_rows, true);
  if (diagnostic.error) return diagnostic;
  const auto loaded = TransactionalRelationStore(context).LoadRelationDescriptor(target);
  if (!loaded.ok) return loaded.diagnostic;
  const auto& relation = loaded.descriptor;
  if (ValidateMgaRelationStorageDescriptor(relation).error || relation.relation_uuid != target ||
      relation.storage_profile != "local_mga_rowstore_v1" || relation.relation_kind != "table" ||
      relation.mutation_rule != "copy_on_write") return Error("relation_storage_profile");
  if (!projection::TypedUuid(target, &snapshot->target_relation_uuid) ||
      !projection::TypedUuid(relation.descriptor_uuid, &snapshot->relation_descriptor_uuid))
    return Error("relation_identity", "DATATYPE.DESCRIPTOR.INVALID");
  snapshot->target_relation_generation = relation.relation_generation;
  snapshot->relation_descriptor_generation = relation.descriptor_generation;

  // Scope expansion is not evidence that an unresolvable child reference does
  // not exist. Resolve the no-inbound profile over all visible metadata, with
  // actual MGA authority overlaid and without reading any row/index payload.
  MgaRelationStoreState metadata;
  diagnostic = LoadMgaMetadata(&metadata.relation_metadata, context);
  if (diagnostic.error) return diagnostic;
  diagnostic = OverlayMgaTransactionAuthorityForStoreModule(context, &metadata.relation_metadata, true);
  if (diagnostic.error) return diagnostic;
  auto view = BuildMgaRelationReadView(std::move(metadata));
  const auto table = FindVisibleMgaTable(view, target, context.local_transaction_id);
  if (!table || table->temporary) return Error("target_lifetime");
  diagnostic = ValidateDmlDeleteNoInboundConstraintProfileV1(context, view, *table);
  if (diagnostic.error) return diagnostic;

  // Hash the exact logical source, excluding physical root addresses that
  // normal copy-on-write DML is itself permitted to advance. Descriptor text
  // stays in memory; durable snapshot fields contain hashes and binary UUIDs.
  std::vector<std::uint8_t> shape;
  if (!Identity(&shape, target) || !Identity(&shape, relation.descriptor_uuid))
    return Error("shape_identity");
  Number(&shape, relation.relation_generation); Number(&shape, relation.descriptor_generation);
  Text(&shape, relation.storage_profile); Text(&shape, relation.mutation_rule);
  Number(&shape, table->columns.size());
  for (const auto& [name, descriptor] : table->columns) { Text(&shape, name); Text(&shape, descriptor); }
  Number(&shape, relation.columns.size());
  for (const auto& column : relation.columns) {
    if (column.storage_class != "inline_row_value" || column.overflow_policy != "mga_large_value_locator" ||
        !Identity(&shape, column.column_uuid) ||
        !Identity(&shape, column.value_descriptor.descriptor_uuid) ||
        !Identity(&shape, column.charset_uuid, true) || !Identity(&shape, column.collation_uuid, true))
      return Error("column_storage_or_identity_profile");
    Number(&shape, column.column_generation); Number(&shape, column.ordinal);
    Text(&shape, column.canonical_name_key); Text(&shape, column.value_descriptor.encoded_descriptor);
    Number(&shape, column.nullable); Number(&shape, column.generated); Number(&shape, column.identity_column);
    Number(&shape, column.character_length); Number(&shape, column.max_inline_bytes);
    Text(&shape, column.storage_class); Text(&shape, column.overflow_policy);
  }
  snapshot->relation_shape_sha256 = Hash("ScratchBird.DmlDelete.RelationShape.V1", shape);
  auto indexes = VisibleMgaIndexesForTable(view, target, context.local_transaction_id);
  if (indexes.size() > 1048576) return Error("index_bound", "RESOURCE.BUDGET_EXCEEDED");
  std::sort(indexes.begin(), indexes.end(), [](const auto& a, const auto& b) { return a.index_uuid < b.index_uuid; });
  std::vector<std::uint8_t> index_bytes;
  Number(&index_bytes, indexes.size());
  std::string prior;
  for (const auto& index : indexes) {
    if (!IsAdmittedMgaSavepointIndexProfile(index) || index.index_uuid == prior ||
        index.table_uuid != target || !Identity(&index_bytes, index.index_uuid) ||
        !Identity(&index_bytes, index.table_uuid)) return Error("index_provider_identity_or_profile");
    prior = index.index_uuid;
    Number(&index_bytes, index.creator_tx); Number(&index_bytes, index.event_sequence);
    Text(&index_bytes, index.column_name); Text(&index_bytes, index.family); Text(&index_bytes, index.profile);
    Number(&index_bytes, index.unique); Number(&index_bytes, index.approximate); Number(&index_bytes, index.exact_fallback);
    Number(&index_bytes, index.key_envelopes.size());
    for (const auto& key : index.key_envelopes) Text(&index_bytes, key);
    Number(&index_bytes, index.include_columns.size());
    for (const auto& included : index.include_columns) Text(&index_bytes, included);
    Text(&index_bytes, index.predicate_kind); Text(&index_bytes, index.predicate_column); Text(&index_bytes, index.predicate_value);
  }
  snapshot->index_count = static_cast<std::uint32_t>(indexes.size());
  snapshot->index_set_sha256 = Hash("ScratchBird.DmlDelete.IndexSet.V1", index_bytes);
  // The actual manager preflights above proved these empty sets. These hashes
  // alone cannot prove absence, issue a handle, or bypass the next preflight.
  snapshot->constraint_set_sha256 = Hash("ScratchBird.DmlDelete.NoInboundConstraintSet.V1", {});
  snapshot->trigger_set_sha256 = Hash("ScratchBird.DmlDelete.NoTriggerSet.V1", {});
  if (!Nonzero(snapshot->relation_shape_sha256) || !Nonzero(snapshot->index_set_sha256) ||
      !Nonzero(snapshot->constraint_set_sha256) || !Nonzero(snapshot->trigger_set_sha256))
    return Error("snapshot_hash", "DML.DELETE_FAILED");
  return Ok();
}
}  // namespace

EngineDmlDeleteEffectAuthorityResultV1 CaptureDmlDeleteEffectAuthorityV1(
    const EngineRequestContext& context, const std::string& target) {
  EngineDmlDeleteEffectAuthorityResultV1 result;
  result.diagnostic = ValidateContext(context, "private_dml_delete_rows_binder");
  if (result.diagnostic.error) return result;
  result.diagnostic = Project(context, target, &result.snapshot);
  if (result.diagnostic.error) return result;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  for (auto* destination : {&result.snapshot.snapshot_uuid, &result.snapshot.constraint_set_uuid,
                            &result.snapshot.trigger_set_uuid}) {
    const auto identity = scratchbird::core::uuid::GenerateEngineIdentityV7(
        scratchbird::core::platform::UuidKind::object, now);
    if (!identity.ok()) { result.diagnostic = Error("snapshot_identity", "DML.DELETE_FAILED"); return result; }
    std::copy(identity.value.value.bytes.begin(), identity.value.value.bytes.end(), destination->begin());
  }
  result.snapshot.generation = 1;
  auto authority = std::make_shared<EngineDmlDeleteEffectAuthorityHandleV1::Authority>();
  authority->owner = context;
  authority->snapshot = result.snapshot;
  result.handle.authority_ = std::move(authority);
  result.ok = true;
  return result;
}

EngineApiDiagnostic RevalidateDmlDeleteEffectAuthorityV1(
    const EngineRequestContext& context, const EngineDmlDeleteEffectAuthorityResultV1& captured) {
  const auto valid = ValidateContext(context, "private_dml_delete_rows_consumer");
  if (valid.error) return valid;
  if (!captured.ok || !captured.handle.valid()) return Error("engine_handle_required", "SECURITY.ACCESS_DENIED");
  const auto& authority = *captured.handle.authority_;
  if (Owner(context) != Owner(authority.owner) || captured.snapshot != authority.snapshot)
    return Error("owner_or_projection_changed", "MGA.TRANSACTION.STALE");
  EngineDmlDeleteEffectSnapshotV1 current;
  const auto resolved = Project(context, projection::UuidText(authority.snapshot.target_relation_uuid), &current);
  if (resolved.error) return resolved;
  current.snapshot_uuid = authority.snapshot.snapshot_uuid; current.generation = authority.snapshot.generation;
  current.constraint_set_uuid = authority.snapshot.constraint_set_uuid;
  current.trigger_set_uuid = authority.snapshot.trigger_set_uuid;
  if (current != authority.snapshot) return Error("live_effect_graph_changed", "MGA.TRANSACTION.STALE");
  return Ok();
}
EngineApiDiagnostic RevalidateRecoveredDmlDeleteEffectProjectionV1(
    const EngineRequestContext& context, const EngineDmlDeleteEffectSnapshotV1& supplied) {
  const auto valid = ValidateContext(context, "private_dml_delete_rows_recovery");
  if (valid.error) return valid;
  if (!supplied.generation ||
      std::all_of(supplied.snapshot_uuid.begin(), supplied.snapshot_uuid.end(), [](auto b) { return b == 0; }) ||
      std::all_of(supplied.constraint_set_uuid.begin(), supplied.constraint_set_uuid.end(), [](auto b) { return b == 0; }) ||
      std::all_of(supplied.trigger_set_uuid.begin(), supplied.trigger_set_uuid.end(), [](auto b) { return b == 0; }) ||
      supplied.snapshot_uuid == supplied.constraint_set_uuid || supplied.snapshot_uuid == supplied.trigger_set_uuid ||
      supplied.constraint_set_uuid == supplied.trigger_set_uuid)
    return Error("recovered_effect_identity", "MGA.TRANSACTION.STALE");
  EngineDmlDeleteEffectSnapshotV1 current;
  const auto resolved = Project(context, projection::UuidText(supplied.target_relation_uuid), &current);
  if (resolved.error) return resolved;
  current.snapshot_uuid = supplied.snapshot_uuid; current.generation = supplied.generation;
  current.constraint_set_uuid = supplied.constraint_set_uuid; current.trigger_set_uuid = supplied.trigger_set_uuid;
  if (current != supplied) return Error("recovered_effect_source_changed", "MGA.TRANSACTION.STALE");
  return Ok();
}
}  // namespace scratchbird::engine::internal_api
