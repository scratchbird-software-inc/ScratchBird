// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/merge_api.hpp"

#include "crud_support/crud_store.hpp"
#include "catalog/binary_view_options.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "dml/insert_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "dml/update_api.hpp"
#include "dml/transactional_relation_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/dml_summary_counters.hpp"

#include "metric_producer.hpp"
#include "uuid.hpp"

#include <algorithm>
#include "physical_plan.hpp"
#include "relational_planner.hpp"

#include <cctype>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

EngineUuid RequiredBinaryIdentity(const std::string& bytes) {
  if (bytes.empty()) return {};
  EngineUuid identity;
  if (!ReadMetadataUuid(bytes, &identity))
    throw std::invalid_argument("dml_binary_identity_required");
  return identity;
}

EngineObjectReference MergeTarget(const EngineMergeRowsRequest& request) {
  return !request.target_table.uuid.is_nil() ? request.target_table : request.target_object;
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

std::string MergeOptionValue(const EngineMergeRowsRequest& request,
                             const std::string& prefix) {
  for (const auto& candidate : request.option_envelopes) {
    if (candidate.starts_with(prefix)) {
      return candidate.substr(prefix.size());
    }
  }
  for (const auto& candidate : request.diagnostic_options) {
    if (candidate.starts_with(prefix)) {
      return candidate.substr(prefix.size());
    }
  }
  return {};
}

std::string MergeSurfaceVariant(const EngineMergeRowsRequest& request) {
  std::string variant = request.merge_surface_variant;
  if (variant.empty()) {
    variant = MergeOptionValue(request, "dml_surface_variant:");
  }
  if (variant.empty()) {
    variant = MergeOptionValue(request, "merge_surface_variant:");
  }
  if (variant.empty()) {
    variant = "merge";
  }
  for (char& c : variant) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return variant;
}

EngineTypedValue MergeTypedValueFromCrudValue(const std::string& encoded) {
  EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "text";
  value.descriptor.encoded_descriptor = "type=text";
  if (encoded == "<NULL>") {
    value.setState(EngineValueState::sql_null);
    return value;
  }
  value.encoded_value = encoded;
  return value;
}

EngineRowValue MergeRowFromCrudRow(const CrudRowVersionRecord& source_row) {
  EngineRowValue row;
  row.fields.reserve(source_row.values.size());
  for (const auto& [field, encoded] : source_row.values) {
    row.fields.push_back({field, MergeTypedValueFromCrudValue(encoded)});
  }
  return row;
}

std::vector<EngineRowValue> MergeRowsFromSourceTable(
    const MgaRelationReadView& state,
    const EngineRequestContext& context,
    const EngineUuid& source_table_uuid) {
  std::vector<EngineRowValue> rows;
  for (const auto& source_row :
       VisibleMgaRowsForContext(state, source_table_uuid, context)) {
    rows.push_back(MergeRowFromCrudRow(source_row));
  }
  return rows;
}

EnginePredicateEnvelope MergePredicateForRow(const EngineMergeRowsRequest& request,
                                             const EngineRowValue& row) {
  EnginePredicateEnvelope predicate = !request.match_predicate.predicate_kind.empty() ? request.match_predicate
                                                                                     : request.predicate;
  if (predicate.predicate_kind == "row_uuid_match" && predicate.row_uuid.is_nil() &&
      !row.requested_row_uuid.is_nil()) {
    predicate.row_uuid = row.requested_row_uuid;
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
         family != kCrudIndexFamilyReferenceEmulated;
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
  plan_request.database_uuid = request.context.database_uuid;
  plan_request.relation_uuid = table.table_uuid;
  plan_request.relation_present = true;
  plan_request.predicate_kind = predicate.predicate_kind;
  plan_request.predicate_descriptor_digest = PredicateDigest(predicate);
  plan_request.access_descriptor_present = true;
  plan_request.security_policy_digest =
      EncodeMgaMetadataFields({"merge.security.policy.v2", MetadataUuidBytes(request.context.principal_uuid),
          MetadataUuidBytes(request.context.current_role_uuid), std::to_string(request.context.security_epoch)});
  plan_request.redaction_policy_digest =
      "resource_epoch:" + std::to_string(request.context.resource_epoch);
  plan_request.access_policy_digest =
      EncodeMgaMetadataFields({"merge.access.policy.v2", MetadataUuidBytes(request.context.session_uuid),
          std::to_string(request.context.resource_epoch)});
  plan_request.collation_profile_digest =
      request.context.identifier_profile_uuid + ":" +
      request.context.language_context.language_tag;
  plan_request.local_transaction_id = request.context.local_transaction_id;
  plan_request.mga_visibility_recheck_planned = true;
  plan_request.security_recheck_planned = true;
  plan_request.grants_proven = request.context.security_context_present;
  plan_request.security_context_present = request.context.security_context_present;
  plan_request.parser_or_reference_authority = false;
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

  if (predicate.predicate_kind == "row_uuid_match") {
    plan_request.predicate_kind = "row_uuid_match";
    plan_request.row_uuid = predicate.row_uuid;
    plan_request.estimated_rows = 1;
    return plan_request;
  }
  if (predicate.predicate_kind == "row_uuid_in_list") {
    plan_request.row_uuids = predicate.row_uuids;
    plan_request.estimated_rows = predicate.row_uuids.size();
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
  std::vector<EngineEvidenceReference> group;
  group.push_back({"merge_row_locator_stream",
                       stream.ok ? DmlRowLocatorStreamSourceName(stream.source)
                                 : "refused"});
  group.push_back({"merge_row_locator_stream_ok",
                       stream.ok ? "true" : "false"});
  group.push_back({"merge_row_locator_count",
                       std::to_string(stream.locators.size())});
  AppendApiEvidenceGroup(group, stream.evidence, "merge_row_locator_stream_evidence");
  AppendApiEvidenceGroup(*evidence, group);
}

DmlTargetAccessPlan BuildRowUuidLocatorPlanFromRows(
    const DmlTargetAccessPlanRequest& base_request,
    const std::vector<CrudRowVersionRecord>& rows) {
  DmlTargetAccessPlanRequest locator_request = base_request;
  locator_request.index_uuid = {};
  locator_request.index_unique = false;
  locator_request.index_family = "btree";
  locator_request.predicate_kind = rows.size() == 1 ? "row_uuid_match" : "row_uuid_in_list";
  locator_request.predicate_descriptor_digest = "persisted_index_row_locator_projection";
  locator_request.row_uuid = rows.size() == 1 ? rows.front().row_uuid : EngineUuid{};
  locator_request.row_uuids.clear();
  if (rows.size() > 1) {
    for (const auto& row : rows) locator_request.row_uuids.push_back(row.row_uuid);
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
  request.parser_or_reference_authority = false;
  request.index_or_cache_finality_authority = false;
  return BuildDmlRowLocatorStream(request);
}

void AddMergeHotPointAdmissionEvidence(
    const DmlTargetAccessPlanRequest& plan_request,
    const EngineUuid& row_uuid,
    std::vector<EngineEvidenceReference>* evidence) {
  std::vector<std::string> cache_evidence;
  DmlTargetAccessPlanRequest locator_request = plan_request;
  if (locator_request.row_uuid.is_nil() && locator_request.index_uuid.is_nil()) {
    locator_request.row_uuid = row_uuid;
    locator_request.predicate_kind = "row_uuid_match";
    locator_request.predicate_descriptor_digest = "row_uuid_match";
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
  std::vector<CrudRowVersionRecord> rows;
};

struct MergeTargetSnapshot {
  bool enabled = false;
  std::string column;
  std::vector<CrudRowVersionRecord> visible_rows;
};

bool MergePredicateEligibleForTargetSnapshot(
    const EnginePredicateEnvelope& predicate,
    const EngineUuid& source_table_uuid) {
  return !source_table_uuid.is_nil() &&
         predicate.predicate_kind == "column_equals" &&
         !predicate.canonical_predicate_envelope.empty() &&
         predicate.bound_values.empty();
}

MergeTargetSnapshot BuildMergeTargetSnapshot(
    const MgaRelationReadView& state,
    const EngineUuid& table_uuid,
    const EngineRequestContext& context,
    const EnginePredicateEnvelope& predicate,
    const EngineUuid& source_table_uuid) {
  MergeTargetSnapshot lookup;
  if (!MergePredicateEligibleForTargetSnapshot(predicate, source_table_uuid)) {
    return lookup;
  }
  lookup.enabled = true;
  lookup.column = predicate.canonical_predicate_envelope;
  lookup.visible_rows = VisibleMgaRowsForContext(state, table_uuid, context);
  return lookup;
}

MergeMatchLookupResult FindMergeMatchesInSnapshot(
    const MergeTargetSnapshot& lookup,
    const EnginePredicateEnvelope& predicate,
    std::vector<EngineEvidenceReference>* evidence) {
  MergeMatchLookupResult result;
  // Encoded text equality is not descriptor equality. Until the owning key
  // codec supplies a canonical key, test every visible snapshot candidate.
  for (const auto& candidate : lookup.visible_rows)
    if (CrudRowMatchesPredicate(candidate, predicate)) result.rows.push_back(candidate);
  evidence->push_back({"merge_row_candidate_stream", "visible_snapshot_predicate_scan"});
  evidence->push_back({"merge_snapshot_matches", std::to_string(result.rows.size())});
  return result;
}

MergeMatchLookupResult FindMergeMatchWithPlan(
    const MgaRelationReadView& state,
    const EngineUuid& table_uuid,
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
      if (const auto row = FindVisibleMgaRowForContext(state, table_uuid, plan.row_uuid, context);
          row && CrudRowMatchesPredicate(*row, predicate)) {
        result.rows.push_back(*row);
        AddMergeHotPointAdmissionEvidence(plan_request, row->row_uuid, evidence);
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
      for (const auto& identity : plan.row_uuids)
        if (const auto row = FindVisibleMgaRowForContext(state, table_uuid, identity, context);
            row && CrudRowMatchesPredicate(*row, predicate)) result.rows.push_back(*row);
      return result;
    case DmlTargetAccessKind::unique_index_lookup:
    case DmlTargetAccessKind::nonunique_index_lookup: {
      const auto indexed = IndexedMgaRowsForPredicateForContext(
          state,
          table_uuid,
          predicate,
          context,
          0);
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
        for (const auto& row : indexed.rows)
          if (CrudRowMatchesPredicate(row, predicate)) result.rows.push_back(row);
        if (plan.access_kind == DmlTargetAccessKind::unique_index_lookup && result.rows.size() > 1) {
          result.ok = false;
          result.rows.clear();
          result.diagnostic = MakeInvalidRequestDiagnostic("dml.merge_rows", "unique_index_returned_multiple_matches");
          return result;
        }
        if (result.rows.size() == 1)
          AddMergeHotPointAdmissionEvidence(plan_request, result.rows.front().row_uuid, evidence);
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
      const auto rows = VisibleMgaRowsForContext(state, table_uuid, context);
      for (const auto& row : rows) {
        if (CrudRowMatchesPredicate(row, predicate)) {
          result.rows.push_back(row);
        }
      }
      return result;
    }
    case DmlTargetAccessKind::refused:
    case DmlTargetAccessKind::range_index_lookup:
    case DmlTargetAccessKind::summary_pruned:
      evidence->push_back({"merge_row_candidate_stream", "refused"});
      result.ok = false;
      result.diagnostic = MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_target_route_not_executed");
      return result;
  }
  result.ok = false;
  result.diagnostic = MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_target_route_invalid");
  return result;
}

struct MergeActionPartition {
  std::size_t source_ordinal = 0;
  EngineRowValue source_row;
  EnginePredicateEnvelope predicate;
  std::vector<CrudRowVersionRecord> matched_rows;
  DmlTargetAccessPlan plan;
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

EnginePredicateEnvelope RowUuidSetPredicate(const std::vector<EngineUuid>& row_uuids) {
  EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "row_uuid_in_list";
  predicate.row_uuids = row_uuids;
  return predicate;
}

void AppendMergeUpdateOptions(const EngineMergeRowsRequest& request,
                              EngineUpdateRowsRequest* update) {
  if (update == nullptr) return;
  for (const auto& option : request.option_envelopes) {
    if (option.starts_with("assignment_plan:")) {
      update->option_envelopes.push_back(option);
    }
  }
}

using MergeReturningRowsByOrdinal = std::map<std::pair<std::size_t, EngineUuid>, EngineRowValue>;

bool AddRowsByUuid(const EngineResultShape& shape,
                   std::unordered_map<EngineUuid, EngineRowValue, EngineUuidHash>* rows_by_uuid) {
  std::unordered_map<EngineUuid, EngineRowValue, EngineUuidHash> staged;
  for (const auto& row : shape.rows) {
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(row.requested_row_uuid) ||
        !staged.emplace(row.requested_row_uuid, row).second) return false;
  }
  *rows_by_uuid = std::move(staged);
  return true;
}

void AppendEvidence(std::vector<EngineEvidenceReference>* target,
                    const std::vector<EngineEvidenceReference>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

struct MergeActionBatchMember {
  std::size_t source_ordinal = 0;
  EngineUuid matched_row_uuid;
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
  if (request.match_policy != MergeMatchPolicy::all_targets &&
      request.match_policy != MergeMatchPolicy::single_target)
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
        MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_match_policy_invalid"));
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "local_transaction_id_required"));
  }
  const EngineObjectReference target = MergeTarget(request);
  std::vector<EngineRowValue> source_rows = MergeRows(request);
  const std::string merge_surface_variant = MergeSurfaceVariant(request);
  if (target.uuid.is_nil()) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "target_table_uuid_required"));
  }
  if (merge_surface_variant != "merge" &&
      merge_surface_variant != "upsert" &&
      merge_surface_variant != "cypher_merge") {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(
        request.context,
        "dml.merge_rows",
        MakeInvalidRequestDiagnostic("dml.merge_rows",
                                     "unsupported_merge_surface_variant:" +
                                         merge_surface_variant));
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
  const EngineUuid source_table_uuid = RequiredBinaryIdentity(MergeOptionValue(request, "source_uuid:"));
  std::vector<EngineUuid> relation_scope_targets{target.uuid};
  if (!source_table_uuid.is_nil() &&
      source_table_uuid != target.uuid) {
    relation_scope_targets.push_back(source_table_uuid);
  }
  TransactionalRelationStore relation_store(request.context);
  auto loaded = relation_scope_targets.size() == 1
                    ? relation_store.LoadConstraintScope(target.uuid)
                    : relation_store.LoadConstraintScopes(relation_scope_targets);
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", loaded.diagnostic); }
  MgaRelationReadView state = relation_store.BuildReadView(&loaded);
  if (source_rows.empty() && !source_table_uuid.is_nil()) {
    const auto source_table =
        FindVisibleMgaTable(state,
                             source_table_uuid,
                             request.context.local_transaction_id);
    if (!source_table) {
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(
          request.context,
          "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows",
                                       "source_table_not_visible"));
    }
    if (source_table->temporary && request.context.session_uuid.is_nil()) {
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(
          request.context,
          "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows",
                                       "temporary_source_table_requires_session_uuid"));
    }
    source_rows = MergeRowsFromSourceTable(state, request.context, source_table_uuid);
  }
  if (source_rows.empty() && source_table_uuid.is_nil()) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "source_row_required"));
  }
  const EngineUuid table_uuid = target.uuid;
  const auto table = FindVisibleMgaTable(state, table_uuid, request.context.local_transaction_id);
  if (!table) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", "target_table_not_visible"));
  }
  if (table->temporary && request.context.session_uuid.is_nil()) {
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(
        request.context,
        "dml.merge_rows",
        MakeInvalidRequestDiagnostic("dml.merge_rows",
                                     "temporary_table_requires_session_uuid"));
  }
  auto result = MakeCrudSuccessResult<EngineMergeRowsResult>(request.context, "dml.merge_rows");
  result.evidence.insert(result.evidence.end(),
                         loaded.evidence.begin(),
                         loaded.evidence.end());
  result.evidence.push_back({"dml_surface_variant", merge_surface_variant});
  result.evidence.push_back({"audit_event", "data.dml_change"});
  result.evidence.push_back({"dml_result_shape", "rs.dml.returning.v1"});
  if (merge_surface_variant == "upsert") {
    result.evidence.push_back({"upsert_canonical_route", "dml.merge_rows"});
    result.evidence.push_back({"excluded_pseudo_relation", "source_row_descriptor_bound"});
    if (!request.on_conflict_action.empty()) {
      result.evidence.push_back({"upsert_conflict_action", request.on_conflict_action});
    } else {
      const auto option_action = MergeOptionValue(request, "on_conflict_action:");
      if (!option_action.empty()) {
        result.evidence.push_back({"upsert_conflict_action", option_action});
      }
    }
    if (!request.conflict_target_column.empty()) {
      result.evidence.push_back({"upsert_conflict_target", request.conflict_target_column});
    } else {
      const auto option_target = MergeOptionValue(request, "conflict_target_column:");
      if (!option_target.empty()) {
        result.evidence.push_back({"upsert_conflict_target", option_target});
      }
    }
  }
  result.evidence.push_back({"relation_state_full_loads",
                             loaded.full_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_scoped_loads",
                             loaded.scoped_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_load_reason",
                             relation_scope_targets.size() == 1
                                 ? "target_table_merge_scope"
                                 : "target_and_source_table_merge_scope"});
  if (!source_table_uuid.is_nil()) {
    result.evidence.push_back({"merge_source_kind", "table"});
    result.evidence.push_back({"merge_source_visibility", "mga_filtered"});
  }
  AddMutationOptimizerEvidence("merge", request.context.local_transaction_id != 0, true, &result.evidence);
  const auto visible_indexes =
      VisibleMgaIndexesForTable(state, table_uuid, request.context.local_transaction_id);
  const EnginePredicateEnvelope base_merge_predicate =
      !request.match_predicate.predicate_kind.empty() ? request.match_predicate
                                                      : request.predicate;
  const MergeTargetSnapshot target_snapshot =
      BuildMergeTargetSnapshot(state,
                                table_uuid,
                                request.context,
                                base_merge_predicate,
                                source_table_uuid);
  if (target_snapshot.enabled) {
    result.evidence.push_back({"merge_target_snapshot", "prepared"});
    result.evidence.push_back({"merge_target_snapshot_column",
                               target_snapshot.column});
    result.evidence.push_back({"merge_target_snapshot_visible_rows",
                               std::to_string(target_snapshot.visible_rows.size())});
    result.evidence.push_back({"merge_target_snapshot_authority",
                               "candidate_stream_only"});
  }
  std::vector<MergeActionPartition> partitions;
  partitions.reserve(source_rows.size());
  EngineApiU64 matched_source_rows = 0;
  EngineApiU64 unmatched_source_rows = 0;
  bool repeated_full_scan = false;
  bool unique_conflict_proof_index_backed = false;
  for (std::size_t source_ordinal = 0; source_ordinal < source_rows.size(); ++source_ordinal) {
    const EngineRowValue& source_row = source_rows[source_ordinal];
    EnginePredicateEnvelope predicate = MergePredicateForRow(request, source_row);
    if (const auto* error = DmlRowIdentityPredicateError(predicate))
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context,
          "dml.merge_rows", MakeInvalidRequestDiagnostic("dml.merge_rows", error));
    if (predicate.predicate_kind == "column_equals" &&
        (predicate.canonical_predicate_envelope.empty() || predicate.bound_values.size() != 1))
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_equality_binding_incomplete"));
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
    const bool use_target_snapshot =
        target_snapshot.enabled &&
        plan.access_kind == DmlTargetAccessKind::table_scan;
    if (plan.access_kind == DmlTargetAccessKind::table_scan &&
        !use_target_snapshot) {
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
        use_target_snapshot
            ? FindMergeMatchesInSnapshot(target_snapshot, predicate, &result.evidence)
            : FindMergeMatchWithPlan(state,
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
    partition.matched_rows = lookup.rows;
    partition.plan = std::move(plan);
    if (!partition.matched_rows.empty()) {
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
  result.evidence.push_back({"merge_output_order", "source_ordinal_then_target_uuid"});
  result.evidence.push_back({"merge_action_execution", "source_ordered_action_batches"});
  result.evidence.push_back({"mga_visibility_recheck", "required"});
  result.evidence.push_back({"security_recheck", "required"});
  result.evidence.push_back({"mga_finality_authority", "engine_transaction_inventory"});
  result.evidence.push_back({"parser_or_reference_authority", "false"});

  std::vector<EngineRowValue> insert_rows;
  std::vector<std::size_t> insert_ordinals;
  std::map<std::size_t, MergeUpdateActionBatch> update_batches_by_source;
  std::map<std::size_t, MergeDeleteActionBatch> delete_batches_by_source;
  const std::string update_assignment_plan =
      MergeOptionValue(request, "assignment_plan:");
  if (!update_assignment_plan.empty()) {
    result.evidence.push_back({"merge_update_assignment_plan", "descriptor_bound"});
    result.evidence.push_back({"merge_update_batch_key", "source_occurrence"});
  }
  std::vector<MergeSourceClassification> source_classification;
  source_classification.reserve(partitions.size());
  for (const auto& partition : partitions) {
    MergeSourceClassification source;
    source.source_ordinal = partition.source_ordinal;
    source.unmatched_action = request.insert_when_not_matched ? MergeActionKind::insert : MergeActionKind::no_action;
    const auto matched_action = delete_branch_requested ? MergeActionKind::delete_row :
        request.update_when_matched ? MergeActionKind::update : MergeActionKind::no_action;
    for (const auto& target_row : partition.matched_rows)
      source.targets.push_back({target_row.row_uuid, matched_action});
    source_classification.push_back(std::move(source));
  }
  const auto classification = ClassifyMergeMatchSets(source_classification,
      merge_surface_variant == "upsert" ? MergeMatchPolicy::single_target : request.match_policy);
  if (!classification.ok)
    return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
        MakeInvalidRequestDiagnostic("dml.merge_rows", classification.error));
  result.matched_count = classification.matched_pairs;
  for (const auto& action : classification.actions) {
    const auto& partition = partitions[action.source_ordinal];
    if (action.action == MergeActionKind::delete_row) {
      delete_batches_by_source[action.source_ordinal].members.push_back(
          {action.source_ordinal, action.target_row_uuid, partition.source_row});
    } else if (action.action == MergeActionKind::update) {
      auto& batch = update_batches_by_source[action.source_ordinal];
      if (batch.members.empty())
        batch.assignments = !request.update_assignments.empty() ? request.update_assignments :
            (update_assignment_plan.empty() ? partition.source_row.fields :
                std::vector<std::pair<std::string, EngineTypedValue>>{});
      batch.members.push_back({action.source_ordinal, action.target_row_uuid, partition.source_row});
    } else if (action.action == MergeActionKind::insert) {
      insert_ordinals.push_back(action.source_ordinal);
      insert_rows.push_back(partition.source_row);
    }
  }

  MergeReturningRowsByOrdinal returning_rows_by_ordinal;
  EngineApiU64 update_batch_count = 0;
  EngineApiU64 insert_batch_count = 0;
  EngineApiU64 delete_batch_count = 0;
  const auto execute_update_batch = [&](const MergeUpdateActionBatch& batch)
      -> std::optional<EngineMergeRowsResult> {
    std::vector<EngineUuid> row_uuids;
    row_uuids.reserve(batch.members.size());
    for (const auto& member : batch.members) {
      row_uuids.push_back(member.matched_row_uuid);
    }
    EngineUpdateRowsRequest update;
    update.context = request.context;
    update.target_table = target;
    update.update_predicate = RowUuidSetPredicate(row_uuids);
    update.assignments = batch.assignments;
    AppendMergeUpdateOptions(request, &update);
    const auto updated = EngineUpdateRows(update);
    if (!updated.ok) {
      return MergeFailureFromUpdate(request, updated);
    }
    ++update_batch_count;
    result.updated_count += updated.updated_count;
    result.merged_count += updated.updated_count;
    std::unordered_map<EngineUuid, EngineRowValue, EngineUuidHash> rows_by_uuid;
    if (!AddRowsByUuid(updated.result_shape, &rows_by_uuid) ||
        updated.updated_count != batch.members.size() || rows_by_uuid.size() != batch.members.size())
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_update_effect_identity_mismatch"));
    for (const auto& member : batch.members) {
      const auto found = rows_by_uuid.find(member.matched_row_uuid);
      if (found != rows_by_uuid.end()) {
        returning_rows_by_ordinal[{member.source_ordinal, member.matched_row_uuid}] = found->second;
      } else return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_update_returned_foreign_target"));
    }
    AppendEvidence(&result.evidence, updated.evidence);
    AddDmlSummaryCounters(&result.dml_summary, updated.dml_summary);
    if (updated.updated_count != 0) {
      result.evidence.push_back({"merge_action", "update"});
    }
    result.evidence.push_back({"merge_action", "update_batch"});

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
          {"merge_fault_injection_parser_or_reference_authority", "false"});
      return interrupted;
    }
    return std::nullopt;
  };

  const auto execute_insert_batch = [&](std::span<const EngineRowValue> insert_rows,
                                        std::span<const std::size_t> insert_ordinals)
      -> std::optional<EngineMergeRowsResult> {
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
    std::unordered_map<EngineUuid, EngineRowValue, EngineUuidHash> inserted_by_uuid;
    if (!AddRowsByUuid(inserted.result_shape, &inserted_by_uuid) ||
        inserted.inserted_count != insert_ordinals.size() || inserted_by_uuid.size() != insert_ordinals.size())
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_insert_effect_identity_mismatch"));
    for (std::size_t index = 0;
         index < insert_ordinals.size() && index < inserted.result_shape.rows.size();
         ++index) {
      returning_rows_by_ordinal[{insert_ordinals[index], inserted.result_shape.rows[index].requested_row_uuid}] =
          inserted.result_shape.rows[index];
    }
    AppendEvidence(&result.evidence, inserted.evidence);
    AddDmlSummaryCounters(&result.dml_summary, inserted.dml_summary);
    if (inserted.inserted_count != 0) {
      result.evidence.push_back({"merge_action", "insert"});
    }
    result.evidence.push_back({"merge_action", "insert_batch"});
    ++insert_batch_count;
    return std::nullopt;
  };

  const auto execute_delete_batch = [&](const MergeDeleteActionBatch& delete_batch)
      -> std::optional<EngineMergeRowsResult> {
    std::vector<EngineUuid> row_uuids;
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
    result.deleted_count += deleted.deleted_count;
    result.merged_count += deleted.deleted_count;
    std::unordered_map<EngineUuid, EngineRowValue, EngineUuidHash> rows_by_uuid;
    if (!AddRowsByUuid(deleted.result_shape, &rows_by_uuid) ||
        deleted.deleted_count != delete_batch.members.size() || rows_by_uuid.size() != delete_batch.members.size())
      return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_delete_effect_identity_mismatch"));
    for (const auto& member : delete_batch.members) {
      const auto found = rows_by_uuid.find(member.matched_row_uuid);
      if (found != rows_by_uuid.end()) {
        returning_rows_by_ordinal[{member.source_ordinal, member.matched_row_uuid}] = found->second;
      } else return MakeCrudDiagnosticResult<EngineMergeRowsResult>(request.context, "dml.merge_rows",
          MakeInvalidRequestDiagnostic("dml.merge_rows", "merge_delete_returned_foreign_target"));
    }
    AppendEvidence(&result.evidence, deleted.evidence);
    AddDmlSummaryCounters(&result.dml_summary, deleted.dml_summary);
    if (deleted.deleted_count != 0) {
      result.evidence.push_back({"merge_action", "delete"});
    }
    result.evidence.push_back({"merge_action", "delete_batch"});
    ++delete_batch_count;
    return std::nullopt;
  };

  // Classification is complete before the first call. Physical batches never
  // move an action across another source occurrence.
  std::size_t next_insert = 0;
  for (std::size_t ordinal = 0; ordinal < partitions.size(); ++ordinal) {
    if (const auto found = update_batches_by_source.find(ordinal); found != update_batches_by_source.end())
      if (auto failure = execute_update_batch(found->second)) return std::move(*failure);
    if (const auto found = delete_batches_by_source.find(ordinal); found != delete_batches_by_source.end())
      if (auto failure = execute_delete_batch(found->second)) return std::move(*failure);
    if (next_insert < insert_ordinals.size() && insert_ordinals[next_insert] == ordinal) {
      if (auto failure = execute_insert_batch(
          std::span<const EngineRowValue>(insert_rows.data() + next_insert, 1),
          std::span<const std::size_t>(insert_ordinals.data() + next_insert, 1)))
        return std::move(*failure);
      ++next_insert;
    }
  }

  std::vector<EngineRowValue> affected_rows;
  affected_rows.reserve(returning_rows_by_ordinal.size());
  for (auto& [source_ordinal, row] : returning_rows_by_ordinal) {
    (void)source_ordinal;
    affected_rows.push_back(std::move(row));
  }
  result.evidence.push_back({"merge_update_batch_count", std::to_string(update_batch_count)});
  result.evidence.push_back({"merge_insert_batch_count", std::to_string(insert_batch_count)});
  result.evidence.push_back({"merge_delete_batch_count", std::to_string(delete_batch_count)});
  result.dml_summary.rows_changed = result.merged_count;
  result.result_shape.result_kind = "dml_affected_rows";
  result.result_shape.rows = std::move(affected_rows);
  result.evidence.push_back({"merge_surface",
                             merge_surface_variant == "upsert"
                                 ? "upsert_matched_update_or_insert"
                                 : (delete_branch_requested
                                        ? "matched_delete_or_not_matched_insert"
                                        : "matched_update_or_not_matched_insert")});
  result.evidence.push_back({"dml_returning", "affected_rows"});
  RecordMergeMetric("matched", static_cast<double>(result.matched_count));
  RecordMergeMetric("inserted", static_cast<double>(result.inserted_count));
  RecordMergeMetric("updated", static_cast<double>(result.updated_count));
  return result;
}

}  // namespace scratchbird::engine::internal_api
