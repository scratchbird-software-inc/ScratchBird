// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/mutation_savepoint_capability.hpp"

#include "dml/constraint_enforcement.hpp"
#include "dml/dml_executable_trigger_runtime.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/transactional_relation_store.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "core/index/index_family_registry.hpp"
#include "sblr_opcode_registry.hpp"

#include <array>

namespace scratchbird::engine::internal_api {
namespace {
using P = MgaMutationProducer;
using D = MgaSavepointDisposition;
// SEARCH_KEY: MGA_PRODUCTION_MUTATION_SAVEPOINT_CAPABILITIES
// No production producer currently requires physical compensation. Retained
// candidate/allocation bytes are unreachable once MGA hides their owning row;
// retaining those bytes does not retain a logical mutation or decide finality.
constexpr std::array kCapabilities{
  MgaMutationSavepointCapability{P::row_version, "row_version", D::write_set_rewind,
      "native_row_event_cutoff_and_transaction_inventory"},
  MgaMutationSavepointCapability{P::row_directory, "row_directory", D::write_set_rewind,
      "visible_version_chain_recheck"},
  MgaMutationSavepointCapability{P::relation_descriptor, "relation_descriptor", D::write_set_rewind,
      "native_metadata_event_cutoff"},
  MgaMutationSavepointCapability{P::index_membership, "index_membership", D::write_set_rewind,
      "native_index_event_cutoff_and_visible_row_version_recheck"},
  MgaMutationSavepointCapability{P::index_candidate_pages, "index_candidate_pages", D::retained_transaction_effect,
      "non_authoritative_candidates_require_MGA_recheck"},
  MgaMutationSavepointCapability{P::overflow_payload, "overflow_payload", D::write_set_rewind,
      "owning_row_version_visibility_before_locator_expansion"},
  MgaMutationSavepointCapability{P::allocation_reservation, "allocation_reservation", D::retained_transaction_effect,
      "unreachable_bytes_reclaimed_only_by_engine_cleanup_horizon"},
  MgaMutationSavepointCapability{P::immediate_constraint_read, "immediate_constraint_read", D::write_set_rewind,
      "read_only_validation_against_visible_MGA_rows"},
  MgaMutationSavepointCapability{P::deferred_constraint_reservation, "deferred_constraint_reservation", D::unsupported,
      "pending_check_provider_not_admitted"},
  MgaMutationSavepointCapability{P::literal_default, "literal_default", D::write_set_rewind,
      "value_materialized_into_owning_row_version"},
  MgaMutationSavepointCapability{P::sequence_default, "sequence_default", D::unsupported,
      "sequence_mode_and_retention_provider_required"},
  MgaMutationSavepointCapability{P::trigger_body, "trigger_body", D::unsupported,
      "complete_nested_effect_graph_provider_required"},
  MgaMutationSavepointCapability{P::cascade_body, "cascade_body", D::unsupported,
      "complete_nested_effect_graph_provider_required"},
  MgaMutationSavepointCapability{P::catalog_mutation, "catalog_mutation", D::unsupported,
      "catalog_provider_specific_rewind_proof_required"},
  MgaMutationSavepointCapability{P::security_mutation, "security_mutation", D::prohibited,
      "security_epoch_mutation_not_admitted_under_boundary"},
  MgaMutationSavepointCapability{P::temporary_lifetime, "temporary_lifetime", D::unsupported,
      "session_lifetime_provider_required"},
  MgaMutationSavepointCapability{P::large_value_reclaim, "large_value_reclaim", D::prohibited,
      "reclamation_requires_engine_cleanup_horizon"},
  MgaMutationSavepointCapability{P::index_rebuild, "index_rebuild", D::unsupported,
      "maintenance_publication_provider_required"},
  MgaMutationSavepointCapability{P::serializable_conflict_tracking, "serializable_conflict_tracking", D::unsupported,
      "transaction_access_retention_and_rewind_provider_proof_required"},
  MgaMutationSavepointCapability{P::metrics, "metrics", D::retained_transaction_effect,
      "observations_are_not_transactional_database_state"},
  MgaMutationSavepointCapability{P::external_effect, "external_effect", D::prohibited,
      "external_effect_has_no_MGA_rewind_authority"},
  MgaMutationSavepointCapability{P::unknown, "unknown", D::unsupported,
      "unregistered_producer"},
};
static_assert(kCapabilities.size() == static_cast<std::size_t>(P::count));
static_assert([] {
  for (std::size_t n = 0; n < kCapabilities.size(); ++n)
    if (static_cast<std::size_t>(kCapabilities[n].producer) != n) return false;
  return true;
}());

EngineApiDiagnostic Refuse(std::string detail) {
  return MakeEngineApiDiagnostic("SBLR.OPERATION_UNSUPPORTED",
      "mga.savepoint.mutation_provider_unavailable", std::move(detail), true);
}
EngineApiDiagnostic Admit(P producer) {
  const auto& capability = LookupMgaMutationSavepointCapability(producer);
  if (capability.disposition == D::unsupported || capability.disposition == D::prohibited)
    return Refuse(std::string(capability.name) + ":" + std::string(capability.authority));
  return MakeEngineApiDiagnostic("OK", "", "", false);
}
}  // namespace

std::span<const MgaMutationSavepointCapability> MgaMutationSavepointCapabilities() {
  return kCapabilities;
}
const MgaMutationSavepointCapability& LookupMgaMutationSavepointCapability(P producer) noexcept {
  const auto ordinal = static_cast<std::size_t>(producer);
  return kCapabilities[ordinal < kCapabilities.size() ? ordinal : static_cast<std::size_t>(P::unknown)];
}

bool IsAdmittedMgaSavepointIndexProfile(const CrudIndexRecord& index) {
  namespace idx = scratchbird::core::index;
  if (!IsAdmittedMgaTransactionalIndexFamily(index)) return false;
  auto family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (family.empty() && index.profile.empty()) family = "btree";
  if (family == "graph_adjacency") family = "graph";
  const auto lookup = idx::FindBuiltinIndexFamilyById(family);
  if (!lookup.ok()) return false;
  if (index.profile.empty() || index.profile == lookup.descriptor->default_semantic_profile)
    return true;
  auto profile_family = CrudIndexFamilyForProfile(index.profile);
  if (profile_family == "graph_adjacency") profile_family = "graph";
  if (family == "unique_btree" && profile_family == "btree") return true;
  return !profile_family.empty() && profile_family == family;
}

EngineApiDiagnostic AdmitMgaSavepointProducer(
    const EngineRequestContext& context, MgaMutationProducer producer) {
  const auto markers = ParseSavepoints(context);
  if (markers.marker_authority_corrupt || markers.update_statement_authority_corrupt)
    return MakeEngineApiDiagnostic("MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "mga.savepoint.mutation_marker_authority_invalid", "", true);
  const auto active = markers.active_savepoints.find(context.local_transaction_id);
  if (active == markers.active_savepoints.end() || active->second.empty())
    return MakeEngineApiDiagnostic("OK", "", "", false);
  return Admit(producer);
}

EngineApiDiagnostic AdmitMgaSavepointOperation(
    const EngineRequestContext& context, std::string_view operation_id) {
  // These exact boundary operations retain their existing native authority
  // checks. In particular a full rollback must remain available even when
  // a savepoint marker is damaged; this is not permission to repair it here.
  for (const auto boundary : {"engine.op.txn_begin", "engine.op.txn_commit",
       "engine.op.txn_rollback", "engine.op.txn_savepoint",
       "engine.op.txn_release_savepoint", "engine.op.txn_rollback_to_savepoint",
       "transaction.begin", "transaction.commit", "transaction.rollback"})
    if (operation_id == boundary) return MakeEngineApiDiagnostic("OK", "", "", false);
  const auto markers = ParseSavepoints(context);
  if (markers.marker_authority_corrupt || markers.update_statement_authority_corrupt)
    return MakeEngineApiDiagnostic("MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "mga.savepoint.mutation_marker_authority_invalid", "", true);
  const auto active = markers.active_savepoints.find(context.local_transaction_id);
  if (active == markers.active_savepoints.end() || active->second.empty())
    return MakeEngineApiDiagnostic("OK", "", "", false);
  namespace sblr = scratchbird::engine::sblr;
  const auto* entry = sblr::LookupSblrOperation(operation_id);
  if (!entry) return Admit(P::unknown);
  // Each of these APIs performs complete relation-effect preflight before
  // its direct, optimized or ordinary lane. No general local_write waiver.
  if (operation_id == "dml.insert_rows" || operation_id == "dml.update_rows" ||
      operation_id == "dml.delete_rows") return MakeEngineApiDiagnostic("OK", "", "", false);
  switch (entry->transaction_effect) {
    case sblr::SblrOpcodeTransactionEffect::none:
    case sblr::SblrOpcodeTransactionEffect::read:
      return MakeEngineApiDiagnostic("OK", "", "", false);
    case sblr::SblrOpcodeTransactionEffect::catalog_write: return Admit(P::catalog_mutation);
    case sblr::SblrOpcodeTransactionEffect::security_write: return Admit(P::security_mutation);
    case sblr::SblrOpcodeTransactionEffect::external_audit:
    case sblr::SblrOpcodeTransactionEffect::cluster_write:
    case sblr::SblrOpcodeTransactionEffect::replication_write: return Admit(P::external_effect);
    default: return Refuse("unregistered_operation_effect:" + std::string(operation_id));
  }
}

EngineApiDiagnostic AdmitMgaDmlSavepointMutation(
    const EngineRequestContext& context, std::string_view target_relation_uuid,
    MgaDmlMutationKind mutation, bool require_statement_boundary) {
  const auto markers = ParseSavepoints(context);
  if (markers.marker_authority_corrupt || markers.update_statement_authority_corrupt)
    return MakeEngineApiDiagnostic("MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "mga.savepoint.mutation_marker_authority_invalid", "", true);
  const auto active = markers.active_savepoints.find(context.local_transaction_id);
  if (!require_statement_boundary &&
      (active == markers.active_savepoints.end() || active->second.empty()))
    return MakeEngineApiDiagnostic("OK", "", "", false);
  if (context.local_transaction_id == 0 || target_relation_uuid.empty())
    return Refuse("mutation_owner_missing");
  if (mutation != MgaDmlMutationKind::insert && mutation != MgaDmlMutationKind::update &&
      mutation != MgaDmlMutationKind::delete_rows) return Admit(P::unknown);
  const auto isolation = dml_trigger_runtime::LowerAscii(context.transaction_isolation_level);
  if (isolation == "serializable" || isolation == "serializable_snapshot")
    return Admit(P::serializable_conflict_tracking);
  if (context.query_cancellation_requested && context.query_cancellation_requested())
    return MakeEngineApiDiagnostic("PROCESS.CANCELLED",
        "mga.savepoint.mutation_admission_cancelled", "", true);

  TransactionalRelationStore store(context);
  // This metadata-only route expands the same parent/child relation scope as
  // constraint execution, but never scans row payloads or index entries.
  auto loaded = store.LoadInsertTargetMetadata(std::string(target_relation_uuid));
  if (!loaded.ok) return loaded.diagnostic;
  const auto view = store.BuildReadView(&loaded);
  const auto table = FindVisibleMgaTable(view, std::string(target_relation_uuid),
                                       context.local_transaction_id);
  if (!table) return Refuse("target_relation_not_visible");
  if (table->temporary) return Admit(P::temporary_lifetime);
  const auto constraints = ValidateSavepointConstraintProviders(context, view, *table,
      mutation == MgaDmlMutationKind::insert ? "insert" :
      mutation == MgaDmlMutationKind::update ? "update" : "delete");
  if (constraints.error) return constraints;

  // The default interpreter owns descriptor decoding. Inspect every default,
  // not only the first supplied row, before it can consume a sequence value.
  if (mutation == MgaDmlMutationKind::insert) {
    const auto defaults = ValidateSavepointInsertDefaultProviders(*table);
    if (defaults.error) return defaults;
  }
  for (const auto& index : VisibleMgaIndexesForTable(view, table->table_uuid,
                                                    context.local_transaction_id)) {
    if (!IsAdmittedMgaSavepointIndexProfile(index))
      return Refuse("index_membership:unregistered_or_unreleased_family:" + index.family);
  }
  // A load error or unresolvable active target is not proof of no triggers.
  // No cache or caller-supplied event mask can waive complete-body admission.
  EngineExecutableObjectLifecycleState executable;
  const auto trigger_load = dml_trigger_runtime::LoadExecutableState(context, &executable);
  if (!trigger_load.ok) return trigger_load.diagnostic;
  for (const auto& object : executable.objects) {
    if (dml_trigger_runtime::LowerAscii(object.object_kind) != "trigger" || object.lifecycle_state != "active" ||
        object.deleted || object.invalidated) continue;
    const auto target = dml_trigger_runtime::PayloadFieldValue(
        object.payload, "trigger_target_table_uuid:");
    if (target.empty() || target == table->table_uuid) return Admit(P::trigger_body);
  }
  for (const auto producer : {P::row_version, P::row_directory, P::relation_descriptor,
       P::index_membership, P::index_candidate_pages, P::overflow_payload,
       P::allocation_reservation, P::immediate_constraint_read, P::literal_default, P::metrics}) {
    const auto admitted = Admit(producer);
    if (admitted.error) return admitted;
  }
  return MakeEngineApiDiagnostic("OK", "", "", false);
}
}  // namespace scratchbird::engine::internal_api
