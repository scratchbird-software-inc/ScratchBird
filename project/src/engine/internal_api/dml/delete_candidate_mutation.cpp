// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_candidate_mutation.hpp"
#include "dml/test_optimization_profile.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/transactional_relation_store.hpp"
#include "dml/delete_batch.hpp"
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "hash_digest.hpp"
#include "core/platform/savepoint_crash_injection.hpp"
#include <bit>
#include <charconv>
#include <limits>

namespace scratchbird::engine::internal_api {
namespace {
namespace p = datatype_operator_projection;
EngineApiDiagnostic Error(std::string detail, std::string code = "RESOURCE.BUDGET_EXCEEDED") {
  return MakeEngineApiDiagnostic(std::move(code), "sblr.dml_delete_rows.candidate_refused", std::move(detail), true);
}
std::string Fold(std::string value) {
  for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
void Number(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (unsigned n = 0; n < 8; ++n) bytes.push_back(static_cast<std::uint8_t>(value >> (n * 8)));
}
bool Identity(std::vector<std::uint8_t>& bytes, const std::string& value) {
  wire::TypedUpdateUuid uuid{};
  if (!p::TypedUuid(value, &uuid)) return false;
  bytes.insert(bytes.end(), uuid.begin(), uuid.end()); return true;
}
bool Add(std::uint64_t amount, std::uint64_t bound, std::uint64_t* total) {
  if (*total > bound || amount > bound - *total) return false;
  *total += amount; return true;
}
}  // namespace
EngineDmlDeleteCandidateMutationV1 ExecuteDmlDeleteCandidateMutationV1(
    const EngineRequestContext& context, DmlDeleteExecutionLeaseV1& lease) {
  EngineDmlDeleteCandidateMutationV1 output;
  const auto fail = [&](EngineApiDiagnostic diagnostic) {
    output.diagnostic = std::move(diagnostic); return output;
  };
  auto diagnostic = lease.RevalidateLive(context);
  if (diagnostic.error) return fail(std::move(diagnostic));
  const auto& b = lease.bundle(); const auto& d = b.descriptor;
  const auto target = p::UuidText(d.target_relation_uuid);
  const auto budget = b.resource_budget.maximum_total_canonical_value_bytes;
  if (budget < 65536) return fail(Error("minimum_bounded_heap_workspace"));
  std::vector<CrudRowVersionRecord> candidates;
  std::string predicate_column;
  bool predicate_nullable = false;
  std::int64_t predicate_literal = 0;
  EngineApiDiagnostic callback_failure;
  const bool all_rows = b.predicate.records.size() == 1;
  MgaVisibleHeapRelationStreamRequest stream;
  stream.relation_uuid = target; stream.maximum_memory_bytes = budget;
  stream.maximum_decoded_bytes_per_pass = budget;
  stream.cancellation_requested = [&] {
    callback_failure = lease.RevalidateResource(context); return callback_failure.error;
  };
  stream.prepare_consumer_for_visible_rows = [&](const MgaRelationStorageDescriptor& relation,
      std::uint64_t count, std::uint64_t* growth) {
    if (relation.relation_uuid != target || relation.relation_generation != d.target_relation_generation ||
        relation.descriptor_uuid != p::UuidText(b.effects.relation_descriptor_uuid) ||
        relation.descriptor_generation != b.effects.relation_descriptor_generation ||
        count > b.resource_budget.maximum_candidate_rows || count > (budget / 8) / sizeof(CrudRowVersionRecord)) {
      callback_failure = Error("visible_candidate_or_descriptor_bound"); return false;
    }
    if (!all_rows) {
      const auto& node = b.predicate.records[0]; const auto& literal = b.predicate.records[1];
      for (const auto& column : relation.columns) {
        if (column.column_uuid != p::UuidText(node.referenced_column_uuid)) continue;
        if (!predicate_column.empty() || column.column_generation != node.referenced_column_generation) {
          callback_failure = Error("predicate_column_generation", "DATATYPE.DESCRIPTOR.INVALID"); return false;
        }
        predicate_column = Fold(column.canonical_name_key); predicate_nullable = column.nullable;
      }
      if (predicate_column.empty() || (literal.canonical_value.size() != 4 && literal.canonical_value.size() != 8)) {
        callback_failure = Error("predicate_column_or_fixed_width_literal", "DATATYPE.DESCRIPTOR.INVALID"); return false;
      }
      std::uint64_t bits = 0;
      for (std::size_t n = 0; n < literal.canonical_value.size(); ++n) bits |= std::uint64_t(literal.canonical_value[n]) << (8 * n);
      predicate_literal = literal.canonical_value.size() == 4
          ? std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits)) : std::bit_cast<std::int64_t>(bits);
    }
    candidates.reserve(count); *growth = budget / 8; return true;
  };
  stream.consumer_retained_memory_bytes = [&] {
    return HeapReadRowVectorMemoryBytes(candidates).value_or(std::numeric_limits<std::uint64_t>::max());
  };
  stream.consume_visible_row = [&](std::uint64_t, const CrudRowVersionRecord& row) {
    if (row.deleted || row.table_uuid != target) {
      callback_failure = Error("visible_source_identity", "DML.DELETE_FAILED"); return false;
    }
    if (!all_rows) {
      const std::string* value = nullptr;
      for (const auto& field : row.values) if (Fold(field.first) == predicate_column) {
        if (value) { callback_failure = Error("duplicate_predicate_column", "DATATYPE.DESCRIPTOR.INVALID"); return false; }
        value = &field.second;
      }
      if (!value) { callback_failure = Error("missing_predicate_column", "DATATYPE.DESCRIPTOR.INVALID"); return false; }
      if (*value == "<NULL>" && predicate_nullable) return true; // UNKNOWN does not qualify.
      std::int64_t parsed = 0;
      const auto converted = std::from_chars(value->data(), value->data() + value->size(), parsed);
      if (converted.ec != std::errc{} || converted.ptr != value->data() + value->size() ||
          (b.predicate.records[1].canonical_value.size() == 4 &&
           (parsed < std::numeric_limits<std::int32_t>::min() ||
            parsed > std::numeric_limits<std::int32_t>::max()))) {
        callback_failure = Error("noncanonical_integer_source", "DATATYPE.DESCRIPTOR.INVALID"); return false;
      }
      if (parsed != predicate_literal) return true;
    }
    std::uint64_t dynamic = 0;
    const auto retained = HeapReadRowVectorMemoryBytes(candidates);
    if (!retained || !AccountHeapReadRowDynamicMemoryBytes(row, &dynamic) || dynamic > budget / 8 ||
        *retained > budget / 4 || dynamic > budget / 4 - *retained) {
      callback_failure = Error("retained_candidate_bound"); return false;
    }
    candidates.push_back(row); return true;
  };
  const auto scanned = lease.StreamCandidates(context, stream);
  if (!scanned.ok) return fail(callback_failure.error ? callback_failure : scanned.diagnostic);
  if (!scanned.complete_mga_chain_validation || !scanned.exact_segment_extent_revalidated ||
      !scanned.complete_value_delivery || !scanned.memory_receipt_complete)
    return fail(Error("complete_bounded_MGA_stream_required", "DML.DELETE_FAILED"));
  // Canonical DELETE already has one bounded MGA stream, not the legacy
  // index-candidate shortcut. Record the actual route in every test profile.
  dml::RecordTestOptimizationBranch("delete_canonical_scan");
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.row_uuid < b.row_uuid; });
  for (std::size_t n = 1; n < candidates.size(); ++n)
    if (candidates[n - 1].row_uuid == candidates[n].row_uuid)
      return fail(Error("duplicate_visible_candidate", "DML.DELETE_FAILED"));
  diagnostic = lease.RevalidateLive(context);
  if (diagnostic.error) return fail(std::move(diagnostic));

  // Metadata only: never reload the selected relation's row state.
  MgaRelationStoreState metadata;
  diagnostic = LoadMgaMetadata(&metadata.relation_metadata, context);
  if (diagnostic.error) return fail(std::move(diagnostic));
  diagnostic = OverlayMgaTransactionAuthorityForStoreModule(context, &metadata.relation_metadata, true);
  if (diagnostic.error) return fail(std::move(diagnostic));
  auto view = BuildMgaRelationReadView(std::move(metadata));
  const auto table = FindVisibleMgaTable(view, target, context.local_transaction_id);
  if (!table || table->temporary) return fail(Error("target_lifetime_changed", "MGA.TRANSACTION.STALE"));
  const auto indexes = VisibleMgaIndexesForTable(view, target, context.local_transaction_id);
  if (indexes.size() != b.effects.index_count) return fail(Error("index_set_changed", "MGA.TRANSACTION.STALE"));
  EngineDeleteRowsRequest request; request.context = context; request.target_table.uuid = target;
  const auto batch = BuildDeleteBatchContext(request, view, *table, indexes);
  std::vector<CrudRowVersionRecord> tombstones;
  tombstones.reserve(candidates.size());
  std::vector<DmlTransactionalIndexEntryRequest> retires;
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> deltas;
  std::uint64_t staged_bytes = HeapReadRowVectorMemoryBytes(candidates).value_or(budget);
  if (!Add(tombstones.capacity() * sizeof(CrudRowVersionRecord), budget / 2, &staged_bytes))
    return fail(Error("tombstone_vector_bound"));
  for (const auto& row : candidates) {
    diagnostic = lease.RevalidateResource(context);
    if (diagnostic.error) return fail(std::move(diagnostic));
    std::uint64_t dynamic = 0;
    if (!AccountHeapReadRowDynamicMemoryBytes(row, &dynamic) || !Add(dynamic, budget / 2, &staged_bytes))
      return fail(Error("tombstone_payload_bound"));
    auto tombstone = row;
    tombstone.creator_tx = context.local_transaction_id; tombstone.event_sequence = tombstone.sequence = 0;
    tombstone.version_uuid = GenerateCrudEngineUuid("row");
    tombstone.previous_version_uuid = row.version_uuid; tombstone.previous_sequence = row.sequence;
    tombstone.deleted = true;
    for (const auto& plan : batch.index_plan.entries) {
      if (!IsAdmittedMgaTransactionalIndexFamily(plan.index) ||
          (plan.action != DeleteIndexMaintenanceAction::synchronous_tombstone_rewrite &&
           plan.action != DeleteIndexMaintenanceAction::tombstone_delta_ledger))
        return fail(Error("unadmitted_transactional_index_plan", "SBLR.OPERATION_UNSUPPORTED"));
      // Key expansion uses an existing provider with vector output. Bound its
      // input conservatively before calling it, then charge each exact key and
      // payload before retaining a request (including multi-entry profiles).
      if (dynamic > budget / 1024) return fail(Error("index_key_expansion_workspace_bound"));
      const auto keys = CrudIndexKeysForValues(plan.index, row.values);
      const auto payload = CrudFieldValue(row.values, plan.index.column_name);
      for (const auto& key : keys) {
        if (retires.size() + deltas.size() + candidates.size() >= b.resource_budget.maximum_effects ||
            !Add(2 * (sizeof(DmlTransactionalIndexEntryRequest) + key.size() + payload.size() + 1024), budget / 2, &staged_bytes))
          return fail(Error("index_effect_or_memory_bound"));
        retires.push_back({plan.index, target, row.row_uuid, tombstone.version_uuid, row.version_uuid, key, payload});
      }
      if (plan.action == DeleteIndexMaintenanceAction::tombstone_delta_ledger) {
        if (retires.size() + deltas.size() + candidates.size() >= b.resource_budget.maximum_effects ||
            !Add(2 * (dynamic + sizeof(MgaSecondaryIndexDeltaLedgerEntryInput) + 1024), budget / 2, &staged_bytes))
          return fail(Error("delta_effect_or_memory_bound"));
        MgaSecondaryIndexDeltaLedgerEntryInput delta;
        delta.index = plan.index; delta.table_uuid = target; delta.row_uuid = row.row_uuid;
        delta.version_uuid = tombstone.version_uuid; delta.values = row.values;
        delta.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::delete_row;
        delta.source_evidence_reference = "engine.dml.delete.secondary_index_delta:" + context.statement_uuid;
        deltas.push_back(std::move(delta));
      }
    }
    tombstones.push_back(std::move(tombstone));
  }
  if (tombstones.size() > b.resource_budget.maximum_effects) return fail(Error("tombstone_effect_bound"));
  diagnostic = lease.RevalidateLive(context);
  if (diagnostic.error) return fail(std::move(diagnostic));
  TransactionalRelationStore relation_store(context);
  auto append = relation_store.OpenHotAppendContext();
  if (!tombstones.empty()) {
    diagnostic = append.AppendRowVersions(&tombstones, nullptr);
    if (diagnostic.error) return fail(std::move(diagnostic));
    diagnostic = append.FlushRowVersions();
    if (diagnostic.error) return fail(std::move(diagnostic));
    scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary("delete_after_rows", context.local_transaction_id);
    diagnostic = lease.RevalidateResource(context);
    if (diagnostic.error) return fail(std::move(diagnostic));
    diagnostic = relation_store.AppendSecondaryIndexDeltaLedgerEntries(deltas, &output.result.evidence);
    if (diagnostic.error) return fail(std::move(diagnostic));
    MgaTransactionalIndexProvider provider(context, &append);
    const auto retired = provider.PrepareRetireEntries(retires);
    if (!retired.ok) return fail(retired.diagnostic);
    diagnostic = append.FlushIndexEntries();
    if (diagnostic.error) return fail(std::move(diagnostic));
    scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary("delete_after_indexes", context.local_transaction_id);
  }
  constexpr std::string_view domain = "ScratchBird.DmlDelete.Effects.V1";
  std::vector<std::uint8_t> evidence(domain.begin(), domain.end());
  evidence.insert(evidence.end(), d.descriptor_evidence_sha256.begin(), d.descriptor_evidence_sha256.end());
  evidence.insert(evidence.end(), b.effects.index_set_sha256.begin(), b.effects.index_set_sha256.end());
  Number(evidence, candidates.size()); Number(evidence, tombstones.size());
  Number(evidence, retires.size()); Number(evidence, deltas.size()); Number(evidence, tombstones.size());
  for (std::size_t n = 0; n < tombstones.size(); ++n) {
    const auto& row = tombstones[n];
    if (!Identity(evidence, row.row_uuid) || !Identity(evidence, candidates[n].version_uuid) ||
        !Identity(evidence, row.version_uuid) || !row.event_sequence)
      return fail(Error("written_tombstone_identity", "DML.DELETE_FAILED"));
    Number(evidence, row.event_sequence);
  }
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(evidence);
  if (!hash.ok()) return fail(Error("effect_evidence_failed", "DML.DELETE_FAILED"));
  output.effect_set_sha256 = hash.digest;
  output.result.ok = true; output.result.operation_id = "dml.delete_rows";
  output.result.matched_count = candidates.size(); output.result.deleted_count = tombstones.size();
  output.result.dml_summary.rows_changed = tombstones.size();
  output.result.dml_summary.visible_rows_scanned = scanned.visible_row_count;
  output.result.evidence.push_back({"dml_delete_rows_storage", "bounded_MGA_tombstones_and_transactional_indexes"});
  output.result.evidence.push_back({"relation_state_full_loads", "0"});
  output.result.evidence.push_back({"dml_delete_rows_scoped_row_reloads", "0"});
  output.ok = true;
  output.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return output;
}
}  // namespace scratchbird::engine::internal_api
