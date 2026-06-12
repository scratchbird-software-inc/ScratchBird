// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/merge_api.hpp"

#include "crud_support/crud_store.hpp"
#include "dml/insert_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include "metric_producer.hpp"
#include "physical_plan.hpp"
#include "relational_planner.hpp"

#include <optional>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

EngineObjectReference MergeTarget(const EngineMergeRowsRequest& request) {
  return !request.target_table.uuid.canonical.empty() ? request.target_table : request.target_object;
}

std::vector<EngineRowValue> MergeRows(const EngineMergeRowsRequest& request) {
  return !request.input_rows.empty() ? request.input_rows : request.rows;
}

bool MergeOptionEnabled(const EngineMergeRowsRequest& request,
                        const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (candidate == option) {
      return true;
    }
  }
  for (const auto& candidate : request.diagnostic_options) {
    if (candidate == option) {
      return true;
    }
  }
  return false;
}

EnginePredicateEnvelope MergePredicateForRow(const EngineMergeRowsRequest& request,
                                             const EngineRowValue& row) {
  EnginePredicateEnvelope predicate = !request.match_predicate.predicate_kind.empty() ? request.match_predicate
                                                                                     : request.predicate;
  if (predicate.predicate_kind == "row_uuid_match" && predicate.canonical_predicate_envelope.empty() &&
      !row.requested_row_uuid.canonical.empty()) {
    predicate.canonical_predicate_envelope = row.requested_row_uuid.canonical;
  }
  if (predicate.predicate_kind == "column_equals" && predicate.bound_values.empty() &&
      !predicate.canonical_predicate_envelope.empty()) {
    for (const auto& [field, typed] : row.fields) {
      if (field == predicate.canonical_predicate_envelope) {
        predicate.bound_values.push_back(typed);
        break;
      }
    }
  }
  return predicate;
}

std::string PredicateDigest(const EnginePredicateEnvelope& predicate) {
  std::string digest = predicate.predicate_kind + ":" +
                       predicate.canonical_predicate_envelope + ":" +
                       std::to_string(predicate.bound_values.size());
  for (const auto& value : predicate.bound_values) {
    digest += ":" + value.encoded_value;
  }
  return digest;
}

std::string CrudIndexResolvedFamily(const CrudIndexRecord& index) {
  return index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
}

bool MergeIndexUsableForPredicate(const CrudIndexRecord& index,
                                  const EnginePredicateEnvelope& predicate) {
  if (!CrudIndexSupportsPredicate(index, predicate)) {
    return false;
  }
  const std::string family = CrudIndexResolvedFamily(index);
  return predicate.predicate_kind == "column_equals" &&
         (family == kCrudIndexFamilyBtree || family == kCrudIndexFamilyHash ||
          family.empty()) &&
         !index.approximate &&
         family != kCrudIndexFamilyDonorEmulated;
}

std::optional<CrudIndexRecord> SelectMergeMatchIndex(
    const std::vector<CrudIndexRecord>& visible_indexes,
    const EnginePredicateEnvelope& predicate,
    bool* unusable_index_present) {
  if (unusable_index_present != nullptr) {
    *unusable_index_present = false;
  }
  for (const auto& index : visible_indexes) {
    if (!CrudIndexSupportsPredicate(index, predicate)) {
      continue;
    }
    if (MergeIndexUsableForPredicate(index, predicate)) {
      return index;
    }
    if (unusable_index_present != nullptr) {
      *unusable_index_present = true;
    }
  }
  return std::nullopt;
}

DmlTargetAccessPlanRequest BuildMergeTargetAccessPlanRequest(
    const EngineMergeRowsRequest& request,
    const CrudTableRecord& table,
    const EnginePredicateEnvelope& predicate,
    const std::vector<CrudIndexRecord>& visible_indexes,
    bool* unsupported_predicate,
    bool* unusable_index_present) {
  if (unsupported_predicate != nullptr) {
    *unsupported_predicate = false;
  }
  if (unusable_index_present != nullptr) {
    *unusable_index_present = false;
  }

  DmlTargetAccessPlanRequest plan_request;
  plan_request.mutation_kind = "dml.merge_rows";
  plan_request.database_uuid = request.context.database_uuid.canonical;
  plan_request.relation_uuid = table.table_uuid;
  plan_request.relation_present = true;
  plan_request.predicate_kind = predicate.predicate_kind;
  plan_request.predicate_descriptor_digest = PredicateDigest(predicate);
  plan_request.access_descriptor_present = true;
  plan_request.security_policy_digest =
      request.context.principal_uuid.canonical + ":" +
      request.context.current_role_uuid.canonical + ":" +
      std::to_string(request.context.security_epoch);
  plan_request.redaction_policy_digest =
      "resource_epoch:" + std::to_string(request.context.resource_epoch);
  plan_request.access_policy_digest =
      request.context.session_uuid.canonical + ":" +
      std::to_string(request.context.resource_epoch);
  plan_request.collation_profile_digest =
      request.context.identifier_profile_uuid + ":" +
      request.context.language_context.language_tag;
  plan_request.local_transaction_id = request.context.local_transaction_id;
  plan_request.mga_visibility_recheck_planned = true;
  plan_request.security_recheck_planned = true;
  plan_request.grants_proven = request.context.security_context_present;
  plan_request.security_context_present = request.context.security_context_present;
  plan_request.parser_or_donor_authority = false;
  const std::uint64_t observed_catalog_epoch =
      request.bound_object_identity.catalog_generation_id != 0
          ? request.bound_object_identity.catalog_generation_id
          : request.context.catalog_generation_id;
  const std::uint64_t observed_security_epoch =
      request.bound_object_identity.security_epoch != 0
          ? request.bound_object_identity.security_epoch
          : request.context.security_epoch;
  const std::uint64_t observed_policy_epoch =
      request.bound_object_identity.resource_epoch != 0
          ? request.bound_object_identity.resource_epoch
          : request.context.resource_epoch;
  plan_request.observed_catalog_epoch = observed_catalog_epoch;
  plan_request.current_catalog_epoch = request.context.catalog_generation_id;
  plan_request.observed_security_epoch = observed_security_epoch;
  plan_request.current_security_epoch = request.context.security_epoch;
  plan_request.observed_policy_epoch = observed_policy_epoch;
  plan_request.current_policy_epoch = request.context.resource_epoch;
  plan_request.index_epoch = observed_catalog_epoch;
  plan_request.object_epoch = observed_catalog_epoch;
  plan_request.compatibility_epoch =
      request.context.snapshot_visible_through_local_transaction_id != 0
          ? request.context.snapshot_visible_through_local_transaction_id
          : request.context.local_transaction_id;

  if (predicate.predicate_kind == "row_uuid_match" &&
      !predicate.canonical_predicate_envelope.empty()) {
    plan_request.predicate_kind = "row_uuid_match";
    plan_request.row_uuid = predicate.canonical_predicate_envelope;
    plan_request.estimated_rows = 1;
    return plan_request;
  }
  if (predicate.predicate_kind == "column_equals" &&
      !predicate.canonical_predicate_envelope.empty() &&
      !predicate.bound_values.empty()) {
    const auto index =
        SelectMergeMatchIndex(visible_indexes, predicate, unusable_index_present);
    if (index) {
      plan_request.predicate_kind = index->unique ? "unique_eq" : "scalar_eq";
      plan_request.index_uuid = index->index_uuid;
      plan_request.index_family = CrudIndexResolvedFamily(*index);
      plan_request.index_unique = index->unique;
      plan_request.estimated_rows = index->unique ? 1 : 0;
      return plan_request;
    }
    plan_request.explicit_table_scan_fallback = true;
    return plan_request;
  }

  if (unsupported_predicate != nullptr) {
    *unsupported_predicate = true;
  }
  plan_request.explicit_table_scan_fallback = true;
  return plan_request;
}

void AddMergeTargetAccessPlanEvidence(const DmlTargetAccessPlan& plan,
                                      std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"merge_target_access_plan",
                       SerializeDmlTargetAccessPlanEvidence(plan)});
  evidence->push_back({"merge_target_access_kind",
                       DmlTargetAccessKindName(plan.access_kind)});
  for (const auto& entry : plan.evidence) {
    evidence->push_back({"merge_target_access_plan_evidence", entry});
  }
  for (const auto& diagnostic : plan.diagnostics) {
    evidence->push_back({"merge_target_access_plan_refusal", diagnostic});
  }
}

void AppendMergeRowLocatorStreamEvidence(
    const DmlRowLocatorStreamResult& stream,
    std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"merge_row_locator_stream",
                       stream.ok ? DmlRowLocatorStreamSourceName(stream.source)
                                 : "refused"});
  evidence->push_back({"merge_row_locator_stream_ok",
                       stream.ok ? "true" : "false"});
  evidence->push_back({"merge_row_locator_count",
                       std::to_string(stream.locators.size())});
  for (const auto& item : stream.evidence) {
    evidence->push_back({"merge_row_locator_stream_evidence",
                         item.evidence_kind + "=" + item.evidence_id});
  }
}

DmlTargetAccessPlan BuildRowUuidLocatorPlanFromRows(
    const DmlTargetAccessPlanRequest& base_request,
    const std::vector<CrudRowVersionRecord>& rows) {
  if (rows.empty()) {
    DmlTargetAccessPlan plan;
    plan.ok = true;
    plan.access_kind = DmlTargetAccessKind::row_uuid_list;
    plan.physical_access_kind = "row_uuid_lookup";
    plan.executor_capability = "row_uuid_lookup";
    plan.relation_uuid = base_request.relation_uuid;
    plan.predicate_kind = "row_uuid_in_list";
    plan.predicate_descriptor_digest =
        "irc052_empty_persisted_index_locator_stream";
    plan.index_uuid = base_request.index_uuid;
    plan.estimated_rows = 0;
    plan.evidence.push_back("dml_target_access_kind=row_uuid_list");
    plan.evidence.push_back("physical_index_tree_available=false");
    plan.evidence.push_back("irc060_required_for_physical_scan=true");
    return plan;
  }
  DmlTargetAccessPlanRequest locator_request = base_request;
  locator_request.index_uuid.clear();
  locator_request.index_unique = false;
  locator_request.index_family = "btree";
  locator_request.predicate_kind = rows.size() == 1 ? "row_uuid_match"
                                                    : "row_uuid_in_list";
  locator_request.predicate_descriptor_digest =
      "irc052_persisted_index_row_uuid_locator_stream:" +
      std::to_string(rows.size());
  locator_request.row_uuid = rows.size() == 1 ? rows.front().row_uuid : "";
  locator_request.row_uuids.clear();
  for (const auto& row : rows) {
    locator_request.row_uuids.push_back(row.row_uuid);
  }
  locator_request.estimated_rows = static_cast<std::uint64_t>(rows.size());
  return BuildDmlTargetAccessPlan(locator_request);
}

DmlRowLocatorStreamResult BuildMergeLocatorStream(
    const DmlTargetAccessPlan& plan,
    DmlRowLocatorStreamConsumer consumer) {
  DmlRowLocatorStreamRequest request;
  request.consumer = consumer;
  request.access_plan = plan;
  request.access_plan_engine_authority_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.parser_or_donor_authority = false;
  request.index_or_cache_finality_authority = false;
  return BuildDmlRowLocatorStream(request);
}

void AddMergeHotPointAdmissionEvidence(
    const DmlTargetAccessPlanRequest& plan_request,
    const std::string& row_uuid,
    std::vector<EngineEvidenceReference>* evidence) {
  std::vector<std::string> cache_evidence;
  DmlTargetAccessPlanRequest locator_request = plan_request;
  if (locator_request.row_uuid.empty()) {
    locator_request.row_uuid = row_uuid;
    locator_request.predicate_kind = "row_uuid_match";
    locator_request.predicate_descriptor_digest = "row_uuid_match:" + row_uuid;
    locator_request.row_uuids.clear();
  }
  AdmitDmlHotPointLookupCacheSuccessfulRowLocator(locator_request,
                                                  row_uuid,
                                                  &cache_evidence);
  for (const auto& item : cache_evidence) {
    evidence->push_back({"merge_hot_point_lookup_cache", item});
  }
}

struct MergeMatchLookupResult {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::optional<CrudRowVersionRecord> row;
};

MergeMatchLookupResult FindMergeMatchWithPlan(
    const CrudState& state,
    const std::string& table_uuid,
    const EnginePredicateEnvelope& predicate,
    const EngineRequestContext& context,
    const DmlTargetAccessPlanRequest& plan_request,
    const DmlTargetAccessPlan& plan,
    std::vector<EngineEvidenceReference>* evidence) {
  MergeMatchLookupResult result;
  switch (plan.access_kind) {
    case DmlTargetAccessKind::row_uuid_singleton:
      evidence->push_back({"merge_row_candidate_stream", "row_uuid_singleton"});
      {
        const auto stream =
            BuildMergeLocatorStream(plan, DmlRowLocatorStreamConsumer::merge);
        AppendMergeRowLocatorStreamEvidence(stream, evidence);
        if (!stream.ok) {
          result.ok = false;
          result.diagnostic = stream.diagnostic;
          return result;
        }
      }
      result.row = FindVisibleCrudRowForContext(state, table_uuid, plan.row_uuid, context);
      if (result.row) {
        AddMergeHotPointAdmissionEvidence(plan_request, result.row->row_uuid, evidence);
      }
      return result;
    case DmlTargetAccessKind::row_uuid_list:
      evidence->push_back({"merge_row_candidate_stream", "row_uuid_list"});
      {
        const auto stream =
            BuildMergeLocatorStream(plan, DmlRowLocatorStreamConsumer::merge);
        AppendMergeRowLocatorStreamEvidence(stream, evidence);
        if (!stream.ok) {
          result.ok = false;
          result.diagnostic = stream.diagnostic;
          return result;
        }
      }
      return result;
    case DmlTargetAccessKind::unique_index_lookup:
    case DmlTargetAccessKind::nonunique_index_lookup: {
      const auto indexed = IndexedMgaRowsForPredicateForContext(
          state,
          table_uuid,
          predicate,
          context,
          plan.access_kind == DmlTargetAccessKind::unique_index_lookup ? 1 : 0);
      evidence->insert(evidence->end(), indexed.evidence.begin(), indexed.evidence.end());
      evidence->push_back({"merge_row_candidate_stream", "indexed_predicate"});
      evidence->push_back({"index_lookup", indexed.index_evidence_id});
      if (indexed.index_used) {
        evidence->push_back({"physical_index_tree_available", "false"});
        evidence->push_back({"irc060_required_for_physical_scan", "true"});
        const auto locator_plan =
            BuildRowUuidLocatorPlanFromRows(plan_request, indexed.rows);
        const auto stream =
            BuildMergeLocatorStream(locator_plan, DmlRowLocatorStreamConsumer::merge);
        AppendMergeRowLocatorStreamEvidence(stream, evidence);
        if (!stream.ok) {
          result.ok = false;
          result.diagnostic = stream.diagnostic;
          return result;
        }
        evidence->push_back({"merge_row_locator_stream",
                             "consumed_row_uuid_after_index_probe"});
        if (!indexed.rows.empty()) {
          result.row = indexed.rows.front();
          AddMergeHotPointAdmissionEvidence(plan_request, result.row->row_uuid, evidence);
        }
        return result;
      }
      if (indexed.index_refused) {
        evidence->push_back({"merge_target_access_index_refusal",
                             indexed.diagnostic.detail.empty()
                                 ? indexed.diagnostic.message_key
                                 : indexed.diagnostic.detail});
        result.ok = false;
        result.diagnostic =
            indexed.diagnostic.detail.empty()
                ? MakeInvalidRequestDiagnostic("dml.merge_rows",
                                               "mga_indexed_lookup_refused")
                : indexed.diagnostic;
      } else {
        evidence->push_back({"merge_target_access_index_refusal",
                             "planned_index_lookup_not_used"});
        result.ok = false;
        result.diagnostic = MakeInvalidRequestDiagnostic(
            "dml.merge_rows",
            "planned_index_lookup_not_used");
      }
      return result;
    }
    case DmlTargetAccessKind::table_scan: {
      evidence->push_back({"merge_row_candidate_stream", "table_scan"});
      const auto rows = VisibleCrudRowsForContext(state, table_uuid, context);
      for (const auto& row : rows) {
        if (CrudRowMatchesPredicate(row, predicate)) {
          result.row = row;
          return result;
        }
      }
      return result;
    }
    case DmlTargetAccessKind::refused:
    case DmlTargetAccessKind::range_index_lookup:
    case DmlTargetAccessKind::summary_pruned:
      evidence->push_back({"merge_row_candidate_stream", "refused"});
      return result;
  }
  return result;
}

struct MergeActionPartition {
  std::size_t source_ordinal = 0;
  EngineRowValue source_row;
  EnginePredicateEnvelope predicate;
  std::optional<CrudRowVersionRecord> matched_row;
  DmlTargetAccessPlan plan;
  bool matched = false;
};

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void RecordMergeMetric(const char* action, double value) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_dml_merge_rows_total",
      scratchbird::core::metrics::Labels({{"component", "engine.dml.merge"}, {"action", action}}),
      value,
      "engine_merge");
}

EngineTypedValue RowUuidSetValue(const std::string& row_uuid) {
  EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "row_uuid";
  value.descriptor.encoded_descriptor = "type=row_uuid";
  value.encoded_value = row_uuid;
  return value;
}

EnginePredicateEnvelope RowUuidSetPredicate(const std::vector<std::string>& row_uuids) {
  EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "row_uuid_in_list";
  predicate.canonical_predicate_envelope = "row_uuid";
  predicate.bound_values.reserve(row_uuids.size());
  for (const auto& row_uuid : row_uuids) {
    predicate.bound_values.push_back(RowUuidSetValue(row_uuid));
  }
  return predicate;
}

std::string TypedValueDigest(const EngineTypedValue& value) {
  return value.descriptor.canonical_type_name + ":" +
         value.descriptor.encoded_descriptor + ":" +
         (value.is_null ? "null" : value.encoded_value);
}

std::string AssignmentsDigest(
    const std::vector<std::pair<std::string, EngineTypedValue>>& assignments) {
  std::string digest;
  for (const auto& [field, value] : assignments) {
    digest += field + "=" + TypedValueDigest(value) + ";";
  }
  return digest;
}

using MergeReturningRowsByOrdinal = std::map<std::size_t, EngineRowValue>;

void AddRowsByUuid(const EngineResultShape& shape,
                   std::unordered_map<std::string, EngineRowValue>* rows_by_uuid) {
  for (const auto& row : shape.rows) {
    if (!row.requested_row_uuid.canonical.empty()) {
      (*rows_by_uuid)[row.requested_row_uuid.canonical] = row;
    }
  }
}

void AppendEvidence(std::vector<EngineEvidenceReference>* target,
                    const std::vector<EngineEvidenceReference>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

struct MergeActionBatchMember {
  std::size_t source_ordinal = 0;
  std::string matched_row_uuid;
  EngineRowValue source_row;
};

struct MergeUpdateActionBatch {
  std::vector<std::pair<std::string, EngineTypedValue>> assignments;
  std::vector<MergeActionBatchMember> members;
};

struct MergeDeleteActionBatch {
  std::vector<MergeActionBatchMember> members;
};

EngineMergeRowsResult MergeFailureFromUpdate(const EngineMergeRowsRequest& request,
                                             const EngineUpdateRowsResult& updated) {
  auto result = MakeCrudDiagnosticResult<EngineMergeRowsResult>(
      request.context,
      "dml.merge_rows",
      updated.diagnostics.empty()
          ? MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_update_batch_failed")
          : updated.diagnostics.front());
  result.evidence = updated.evidence;
  return result;
}

EngineMergeRowsResult MergeFailureFromInsert(const EngineMergeRowsRequest& request,
                                             const EngineInsertRowsResult& inserted) {
  auto result = MakeCrudDiagnosticResult<EngineMergeRowsResult>(
      request.context,
      "dml.merge_rows",
      inserted.diagnostics.empty()
          ? MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_insert_batch_failed")
          : inserted.diagnostics.front());
  result.evidence = inserted.evidence;
  return result;
}

EngineMergeRowsResult MergeFailureFromDelete(const EngineMergeRowsRequest& request,
                                             const EngineDeleteRowsResult& deleted) {
  auto result = MakeCrudDiagnosticResult<EngineMergeRowsResult>(
      request.context,
      "dml.merge_rows",
      deleted.diagnostics.empty()
          ? MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_delete_batch_failed")
          : deleted.diagnostics.front());
  result.evidence = deleted.evidence;
  return result;
}

void AddMutationOptimizerEvidence(const char* mutation_kind,
                                  bool transaction_context_present,
                                  bool visibility_proven,
                                  std::vector<EngineEvidenceReference>* evidence) {
  namespace opt = scratchbird::engine::optimizer;
  namespace plan = scratchbird::engine::planner;
  const auto decision = opt::PlanLocalMutation(mutation_kind, transaction_context_present, visibility_proven);
  evidence->push_back({"optimizer_mutation_kind", mutation_kind});
  if (!decision.ok) {
    const std::string detail = decision.diagnostics.empty() ? "mutation_plan_rejected" : decision.diagnostics.front();
    evidence->push_back({"optimizer_plan_rejected", detail});
    return;
  }
  evidence->push_back({"optimizer_selected_access", plan::PhysicalAccessKindName(decision.access_kind)});
  evidence->push_back({"optimizer_executor_capability", opt::RequiredExecutorCapabilityForAccessKind(decision.access_kind)});
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_MERGE_API_BEHAVIOR
// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_MERGE_MULTI_ACTION_ODFR_020
EngineMergeRowsResult EngineMergeRows(const EngineMergeRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "local_transaction_id_required"));
  }
  const EngineObjectReference target = MergeTarget(request);
  const std::vector<EngineRowValue> source_rows = MergeRows(request);
  if (target.uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "target_table_uuid_required"));
  }
  if (source_rows.empty()) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "source_row_required"));
  }
  bool delete_branch_requested = request.delete_when_matched;
  for (const auto& option : request.option_envelopes) {
    if (option == "delete_when_matched:true" ||
        option == "when_matched_delete:true" ||
        option == "merge_delete_branch:true") {
      delete_branch_requested = true;
    }
  }
  if (!request.update_when_matched && !request.insert_when_not_matched &&
      !delete_branch_requested) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "no_merge_action_enabled"));
  }
  const auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const std::string table_uuid = target.uuid.canonical;
  const auto table = FindVisibleCrudTable(state, table_uuid, request.context.local_transaction_id);
  if (!table) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "target_table_not_visible"));
  }
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(
        request.context,
        "dml.merge_rows",
        MakeInvalidRequestDiagnostic("dml.merge_rows",
                                     "temporary_table_requires_session_uuid"));
  }
  auto result = MakeCrudSuccessResult<EngineMergeRowsResult>(request.context, "dml.merge_rows");
  AddMutationOptimizerEvidence("merge", request.context.local_transaction_id != 0, true, &result.evidence);
  const auto visible_indexes =
      VisibleCrudIndexesForTable(state, table_uuid, request.context.local_transaction_id);
  std::vector<MergeActionPartition> partitions;
  partitions.reserve(source_rows.size());
  EngineApiU64 matched_source_rows = 0;
  EngineApiU64 unmatched_source_rows = 0;
  bool repeated_full_scan = false;
  bool unique_conflict_proof_index_backed = false;
  for (std::size_t source_ordinal = 0; source_ordinal < source_rows.size(); ++source_ordinal) {
    const EngineRowValue& source_row = source_rows[source_ordinal];
    EnginePredicateEnvelope predicate = MergePredicateForRow(request, source_row);
    if (predicate.predicate_kind.empty()) {
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "match_predicate_required"));
    }
    bool unsupported_predicate = false;
    bool unusable_index_present = false;
    const auto plan_request =
        BuildMergeTargetAccessPlanRequest(request,
                                          *table,
                                          predicate,
                                          visible_indexes,
                                          &unsupported_predicate,
                                          &unusable_index_present);
    DmlTargetAccessPlan plan = BuildDmlTargetAccessPlan(plan_request);
    AddMergeTargetAccessPlanEvidence(plan, &result.evidence);
    if (!plan.ok) {
      auto rejected = MakeCrudDiagnosticResult<EngineMergeRowsResult>(
          request.context,
          "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_target_access_plan_refused"));
      rejected.evidence = std::move(result.evidence);
      return rejected;
    }
    if (plan.access_kind == DmlTargetAccessKind::table_scan) {
      repeated_full_scan = true;
      result.evidence.push_back({"merge_target_access_fallback",
                                 unsupported_predicate
                                     ? "unsupported predicate"
                                     : (unusable_index_present ? "unusable index"
                                                               : "unindexed predicate")});
    }
    if (plan.access_kind == DmlTargetAccessKind::unique_index_lookup) {
      unique_conflict_proof_index_backed = true;
    }
    const auto lookup =
        FindMergeMatchWithPlan(state,
                               table_uuid,
                               predicate,
                               request.context,
                               plan_request,
                               plan,
                               &result.evidence);
    if (!lookup.ok) {
      auto rejected = MakeCrudDiagnosticResult<EngineMergeRowsResult>(
          request.context,
          "dml.merge_rows",
          lookup.diagnostic.error ? lookup.diagnostic
                                  : MakeInvalidRequestDiagnostic("dml.merge_rows",
                                                                 "mga_indexed_lookup_refused"));
      rejected.evidence = std::move(result.evidence);
      rejected.evidence.push_back({"merge_target_access_refusal",
                                   "mga_indexed_lookup_refused"});
      return rejected;
    }
    MergeActionPartition partition;
    partition.source_ordinal = source_ordinal;
    partition.source_row = source_row;
    partition.predicate = std::move(predicate);
    partition.matched_row = lookup.row;
    partition.plan = std::move(plan);
    partition.matched = partition.matched_row.has_value();
    if (partition.matched) {
      ++matched_source_rows;
    } else {
      ++unmatched_source_rows;
    }
    partitions.push_back(std::move(partition));
  }
  result.evidence.push_back({"merge_action_partitioning", "single_pass_source"});
  result.evidence.push_back({"merge_matched_source_rows", std::to_string(matched_source_rows)});
  result.evidence.push_back({"merge_unmatched_source_rows", std::to_string(unmatched_source_rows)});
  result.evidence.push_back({"merge_repeated_full_scan", BoolText(repeated_full_scan)});
  if (unique_conflict_proof_index_backed) {
    result.evidence.push_back({"merge_unique_conflict_proof", "index_backed"});
  }
  result.evidence.push_back({"merge_returning", "affected_rows"});
  result.evidence.push_back({"merge_output_order", "source_order"});
  result.evidence.push_back({"merge_action_execution", "action_batches"});
  result.evidence.push_back({"mga_visibility_recheck", "required"});
  result.evidence.push_back({"security_recheck", "required"});
  result.evidence.push_back({"mga_finality_authority", "engine_transaction_inventory"});
  result.evidence.push_back({"parser_or_donor_authority", "false"});

  std::vector<EngineRowValue> insert_rows;
  std::vector<std::size_t> insert_ordinals;
  std::map<std::string, MergeUpdateActionBatch> update_batches_by_digest;
  MergeDeleteActionBatch delete_batch;
  for (const MergeActionPartition& partition : partitions) {
    if (partition.matched_row) {
      ++result.matched_count;
      if (delete_branch_requested) {
        delete_batch.members.push_back({partition.source_ordinal,
                                        partition.matched_row->row_uuid,
                                        partition.source_row});
        continue;
      }
      if (!request.update_when_matched) { continue; }
      auto assignments =
          !request.update_assignments.empty() ? request.update_assignments
                                              : partition.source_row.fields;
      const auto digest = AssignmentsDigest(assignments);
      auto& batch = update_batches_by_digest[digest];
      if (batch.assignments.empty()) {
        batch.assignments = std::move(assignments);
      }
      batch.members.push_back({partition.source_ordinal,
                               partition.matched_row->row_uuid,
                               partition.source_row});
      continue;
    }
    if (!request.insert_when_not_matched) { continue; }
    insert_ordinals.push_back(partition.source_ordinal);
    insert_rows.push_back(partition.source_row);
  }

  MergeReturningRowsByOrdinal returning_rows_by_ordinal;
  EngineApiU64 update_batch_count = 0;
  for (const auto& [digest, batch] : update_batches_by_digest) {
    (void)digest;
    if (batch.members.empty()) { continue; }
    std::vector<std::string> row_uuids;
    row_uuids.reserve(batch.members.size());
    for (const auto& member : batch.members) {
      row_uuids.push_back(member.matched_row_uuid);
    }
    EngineUpdateRowsRequest update;
    update.context = request.context;
    update.target_table = target;
    update.update_predicate = RowUuidSetPredicate(row_uuids);
    update.assignments = batch.assignments;
    const auto updated = EngineUpdateRows(update);
    if (!updated.ok) {
      return MergeFailureFromUpdate(request, updated);
    }
    ++update_batch_count;
    result.updated_count += updated.updated_count;
    result.merged_count += updated.updated_count;
    std::unordered_map<std::string, EngineRowValue> rows_by_uuid;
    AddRowsByUuid(updated.result_shape, &rows_by_uuid);
    for (const auto& member : batch.members) {
      const auto found = rows_by_uuid.find(member.matched_row_uuid);
      if (found != rows_by_uuid.end()) {
        returning_rows_by_ordinal[member.source_ordinal] = found->second;
      }
    }
    AppendEvidence(&result.evidence, updated.evidence);
    if (updated.updated_count != 0) {
      result.evidence.push_back({"merge_action", "update"});
    }
    result.evidence.push_back({"merge_action", "update_batch"});
  }

  if (update_batch_count != 0 &&
      MergeOptionEnabled(
          request,
          "orh121.fault_injection.partial_merge_batch.after_update_batch")) {
    auto interrupted = MakeCrudDiagnosticResult<EngineMergeRowsResult>(
        request.context,
        "dml.merge_rows",
        MakeInvalidRequestDiagnostic(
            "dml.merge_rows",
            "fault_injection.partial_merge_batch.after_update_batch"));
    interrupted.evidence = std::move(result.evidence);
    interrupted.evidence.push_back(
        {"merge_fault_injection", "partial_batch_after_update_batch"});
    interrupted.evidence.push_back(
        {"merge_fault_injection_recovery_required", "rollback_reopen"});
    interrupted.evidence.push_back(
        {"merge_fault_injection_mga_authority",
         "engine_transaction_inventory"});
    interrupted.evidence.push_back(
        {"merge_fault_injection_parser_or_donor_authority", "false"});
    return interrupted;
  }

  if (!insert_rows.empty()) {
    EngineInsertRowsRequest insert;
    insert.context = request.context;
    insert.target_table = target;
    insert.borrowed_input_rows =
        std::span<const EngineRowValue>(insert_rows.data(), insert_rows.size());
    const auto inserted = EngineInsertRows(insert);
    if (!inserted.ok) {
      return MergeFailureFromInsert(request, inserted);
    }
    result.inserted_count += inserted.inserted_count;
    result.merged_count += inserted.inserted_count;
    for (std::size_t index = 0;
         index < insert_ordinals.size() && index < inserted.result_shape.rows.size();
         ++index) {
      returning_rows_by_ordinal[insert_ordinals[index]] =
          inserted.result_shape.rows[index];
    }
    AppendEvidence(&result.evidence, inserted.evidence);
    if (inserted.inserted_count != 0) {
      result.evidence.push_back({"merge_action", "insert"});
    }
    result.evidence.push_back({"merge_action", "insert_batch"});
  }

  if (!delete_batch.members.empty()) {
    std::vector<std::string> row_uuids;
    row_uuids.reserve(delete_batch.members.size());
    for (const auto& member : delete_batch.members) {
      row_uuids.push_back(member.matched_row_uuid);
    }
    EngineDeleteRowsRequest delete_request;
    delete_request.context = request.context;
    delete_request.target_table = target;
    delete_request.delete_predicate = RowUuidSetPredicate(row_uuids);
    const auto deleted = EngineDeleteRows(delete_request);
    if (!deleted.ok) {
      return MergeFailureFromDelete(request, deleted);
    }
    result.merged_count += deleted.deleted_count;
    std::unordered_map<std::string, EngineRowValue> rows_by_uuid;
    AddRowsByUuid(deleted.result_shape, &rows_by_uuid);
    for (const auto& member : delete_batch.members) {
      const auto found = rows_by_uuid.find(member.matched_row_uuid);
      if (found != rows_by_uuid.end()) {
        returning_rows_by_ordinal[member.source_ordinal] = found->second;
      }
    }
    AppendEvidence(&result.evidence, deleted.evidence);
    if (deleted.deleted_count != 0) {
      result.evidence.push_back({"merge_action", "delete"});
    }
    result.evidence.push_back({"merge_action", "delete_batch"});
  }

  std::vector<EngineRowValue> affected_rows;
  affected_rows.reserve(returning_rows_by_ordinal.size());
  for (auto& [source_ordinal, row] : returning_rows_by_ordinal) {
    (void)source_ordinal;
    affected_rows.push_back(std::move(row));
  }
  result.evidence.push_back({"merge_update_batch_count", std::to_string(update_batch_count)});
  result.evidence.push_back({"merge_insert_batch_count", insert_rows.empty() ? "0" : "1"});
  result.evidence.push_back({"merge_delete_batch_count", delete_batch.members.empty() ? "0" : "1"});
  result.result_shape.result_kind = "dml_affected_rows";
  result.result_shape.rows = std::move(affected_rows);
  result.evidence.push_back({"merge_surface",
                             delete_branch_requested
                                 ? "matched_delete_or_not_matched_insert"
                                 : "matched_update_or_not_matched_insert"});
  result.evidence.push_back({"dml_returning", "affected_rows"});
  RecordMergeMetric("matched", static_cast<double>(result.matched_count));
  RecordMergeMetric("inserted", static_cast<double>(result.inserted_count));
  RecordMergeMetric("updated", static_cast<double>(result.updated_count));
  return result;
}

}  // namespace scratchbird::engine::internal_api
