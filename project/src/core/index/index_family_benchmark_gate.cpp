// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_family_benchmark_gate.hpp"

#include "candidate_set.hpp"
#include "full_text_runtime.hpp"
#include "gin_physical_provider.hpp"
#include "gist_physical_provider.hpp"
#include "graph_adjacency_physical_provider.hpp"
#include "in_memory_index_runtime.hpp"
#include "index_btree_page.hpp"
#include "index_hash_page.hpp"
#include "index_key_encoding.hpp"
#include "index_optimizer_integration.hpp"
#include "index_text_document_access.hpp"
#include "ngram_physical_provider.hpp"
#include "physical_bloom_filter.hpp"
#include "physical_columnar_zone.hpp"
#include "physical_zone_summary.hpp"
#include "spatial_rtree_physical_provider.hpp"
#include "spgist_physical_provider.hpp"
#include "temporary_work_index_runtime.hpp"
#include "text_inverted_segment.hpp"
#include "vector_exact_physical_provider.hpp"
#include "vector_hnsw_physical_provider.hpp"
#include "vector_ivf_pq_physical_provider.hpp"
#include "index_validation_repair_tooling.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::core::index {
namespace {

namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace page = scratchbird::storage::page;

using platform::Severity;
using platform::StatusCode;
using platform::Subsystem;

struct WorkloadSpec {
  const char* workload;
  const char* route_operation;
  bool fallback_disabled;
  const char* cache_classification;
};

struct OperationSample {
  bool ok = false;
  std::string diagnostic_code;
  std::string message_key;
  u64 operation_count = 1;
  u64 rows_examined = 0;
  u64 rows_returned = 0;
  u64 rows_materialized = 0;
  u64 pages_or_containers_touched = 0;
  std::vector<std::string> evidence;
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::engine};
}

IndexReadinessPlanAdmissionEvidence BenchmarkReadinessEvidence(
    IndexFamily family,
    IndexRouteKind route,
    const IndexRouteCapabilityState& route_state) {
  IndexReadinessPlanAdmissionEvidence evidence;
  evidence.family = family;
  evidence.route = route;
  evidence.manifest_epoch = 42;
  evidence.registry_epoch = 42;
  evidence.route_proof_epoch = 42;
  evidence.source_evidence_digest =
      "sha256:ceic042-index-readiness-benchmark-consumption";
  evidence.generated_by =
      "project/tools/ceic_index_readiness_manifest.py#CEIC_030_INDEX_READINESS_MANIFEST_TOOL";
  evidence.generated_manifest_present = true;
  evidence.generated_manifest_current = true;
  evidence.generated_manifest_validated = true;
  evidence.source_digest_matches = true;
  evidence.runtime_registry_family_matches = true;
  evidence.runtime_registry_route_matches = true;
  evidence.runtime_family_available = true;
  evidence.runtime_route_complete = route_state.route_complete();
  evidence.supports_read = route_state.supports_read;
  evidence.supports_equality_lookup = route_state.supports_equality_lookup;
  evidence.supports_ordered_range = route_state.supports_ordered_range;
  evidence.supports_negative_prune = route_state.supports_negative_prune;
  evidence.supports_summary_segment_prune =
      route_state.supports_summary_segment_prune;
  evidence.produces_candidate_set = route_state.produces_candidate_set;
  evidence.approximate_candidate_source =
      route_state.approximate_candidate_source;
  evidence.requires_exact_recheck = route_state.requires_exact_recheck;
  evidence.requires_mga_recheck = route_state.requires_mga_recheck;
  evidence.requires_security_recheck = route_state.requires_security_recheck;
  evidence.requires_exact_rerank = route_state.requires_exact_rerank;
  evidence.exact_recheck_proven = true;
  evidence.mga_recheck_proven = true;
  evidence.security_recheck_proven = true;
  evidence.exact_rerank_proven = route_state.requires_exact_rerank;
  evidence.operation_metrics_producer_proven = true;
  evidence.support_bundle_producer_proven = true;
  evidence.crash_reopen_proven = true;
  evidence.corruption_cleanup_proven = true;
  evidence.cleanup_horizon_proven = true;
  evidence.storage_integration_proven = true;
  evidence.external_cluster_provider_only = true;
  return evidence;
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string key,
                            std::string detail = {}) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.message_key = std::move(key);
  if (!detail.empty()) {
    diagnostic.arguments.push_back({"detail", std::move(detail)});
  }
  diagnostic.source_component = "core.index.family_benchmark_gate";
  return diagnostic;
}

std::string GeneratedUuidText(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateDurableEngineIdentityV7(kind, 1810008000000ull + salt);
  return generated.ok() ? uuid::UuidToString(generated.value.value) : "";
}

std::vector<WorkloadSpec> Workloads(bool include_fallback_disabled) {
  std::vector<WorkloadSpec> workloads = {
      {"point_lookup", "family_runtime.point_lookup", false, "warm"},
      {"range_lookup", "family_runtime.range_lookup", false, "warm"},
      {"bulk_build", "family_runtime.bulk_build", false, "cold"},
      {"update_delete_maintenance",
       "family_runtime.update_delete_maintenance",
       false,
       "warm"},
      {"crash_reopen", "family_runtime.crash_reopen", false, "cold"},
      {"validate", "family_runtime.validate", false, "warm"},
      {"repair_rebuild", "family_runtime.repair_rebuild", false, "cold"},
      {"cold_cache", "family_runtime.cold_cache", false, "cold"},
      {"warm_cache", "family_runtime.warm_cache", false, "warm"}};
  if (include_fallback_disabled) {
    workloads.push_back({"fallback_disabled",
                         "family_runtime.fallback_disabled",
                         true,
                         "warm"});
  }
  return workloads;
}

u32 IterationCount(u32 requested) {
  if (requested == 0) return 1;
  return std::min<u32>(requested, 9);
}

u64 Percentile(std::vector<u64> samples, u32 pct) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  const std::size_t index =
      std::min<std::size_t>(samples.size() - 1,
                            ((samples.size() * pct + 99) / 100) - 1);
  return samples[index];
}

bool TokenSearchFamily(IndexFamily family) {
  return family == IndexFamily::full_text ||
         family == IndexFamily::gin ||
         family == IndexFamily::inverted ||
         family == IndexFamily::ngram ||
         family == IndexFamily::sparse_wand;
}

bool VectorFamily(IndexFamily family) {
  return family == IndexFamily::vector_exact ||
         family == IndexFamily::vector_hnsw ||
         family == IndexFamily::vector_ivf;
}

IndexRouteKind DefaultQueryRouteForFamily(IndexFamily family) {
  if (family == IndexFamily::document_path) {
    return IndexRouteKind::nosql_document;
  }
  if (family == IndexFamily::graph) {
    return IndexRouteKind::nosql_graph;
  }
  if (VectorFamily(family)) {
    return IndexRouteKind::nosql_vector;
  }
  if (TokenSearchFamily(family)) {
    return IndexRouteKind::nosql_search;
  }
  return IndexRouteKind::sql_select;
}

IndexRouteKind RouteForWorkload(IndexFamily family,
                                const WorkloadSpec& workload) {
  const std::string_view name(workload.workload);
  if (name == "bulk_build") {
    return IndexRouteKind::bulk_build;
  }
  if (name == "update_delete_maintenance" || name == "crash_reopen") {
    return IndexRouteKind::maintenance;
  }
  if (name == "validate" || name == "repair_rebuild") {
    return IndexRouteKind::validate_repair;
  }
  return DefaultQueryRouteForFamily(family);
}

IndexPlanCategory QueryCategoryForFamily(IndexFamily family) {
  if (family == IndexFamily::bitmap) {
    return IndexPlanCategory::bitmap_combine;
  }
  if (family == IndexFamily::brin_zone ||
      family == IndexFamily::columnar_zone) {
    return IndexPlanCategory::summary_prune;
  }
  if (family == IndexFamily::bloom) {
    return IndexPlanCategory::fallback_full_scan;
  }
  if (family == IndexFamily::temporary_work ||
      family == IndexFamily::in_memory) {
    return IndexPlanCategory::fallback_full_scan;
  }
  if (family == IndexFamily::graph) {
    return IndexPlanCategory::graph_search;
  }
  if (VectorFamily(family)) {
    return IndexPlanCategory::vector_search;
  }
  if (family == IndexFamily::spatial ||
      family == IndexFamily::rtree ||
      family == IndexFamily::gist ||
      family == IndexFamily::spgist) {
    return IndexPlanCategory::spatial_search;
  }
  if (TokenSearchFamily(family)) {
    return IndexPlanCategory::inverted_search;
  }
  return IndexPlanCategory::point_lookup;
}

void AddEvidence(IndexFamilyBenchmarkEvidenceRow* row,
                 std::string key,
                 std::string value) {
  row->evidence.push_back(std::move(key) + "=" + std::move(value));
}

IndexFamilyBenchmarkEvidenceRow BaseRow(
    const IndexFamilyDescriptor& descriptor,
    const IndexFamilyPhysicalCapabilityState& state,
    const WorkloadSpec& workload) {
  IndexFamilyBenchmarkEvidenceRow row;
  row.family_id = descriptor.id;
  row.workload = workload.workload;
  row.route_operation = workload.route_operation;
  row.route_kind = RouteForWorkload(descriptor.family, workload);
  row.route_capability_kind = IndexRouteKindName(row.route_kind);
  row.runtime_available = state.runtime_available;
  row.benchmark_clean_admissible = state.benchmark_clean;
  row.fallback_disabled = workload.fallback_disabled;
  row.cache_classification = workload.cache_classification;
  row.blocker = IndexFamilyPhysicalCapabilityBlockerName(state.blocker);
  row.diagnostic_code = state.blocker == IndexFamilyPhysicalCapabilityBlocker::none
                            ? "INDEX.BENCHMARK.OK"
                            : state.blocker_diagnostic_code;
  row.message_key = state.blocker == IndexFamilyPhysicalCapabilityBlocker::none
                        ? "index.benchmark.ok"
                        : state.blocker_message_key;
  AddEvidence(&row, "family_id", row.family_id);
  AddEvidence(&row, "workload", row.workload);
  AddEvidence(&row, "route_operation", row.route_operation);
  AddEvidence(&row, "route_kind", IndexRouteKindName(row.route_kind));
  AddEvidence(&row, "runtime_available",
              row.runtime_available ? "true" : "false");
  AddEvidence(&row, "benchmark_clean_admissible",
              row.benchmark_clean_admissible ? "true" : "false");
  AddEvidence(&row, "fallback_disabled",
              row.fallback_disabled ? "true" : "false");
  AddEvidence(&row, "observational_only", "true");
  AddEvidence(&row, "catalog_authority", "false");
  AddEvidence(&row, "execution_authority", "false");
  AddEvidence(&row, "parser_authority", "false");
  AddEvidence(&row, "donor_authority", "false");
  AddEvidence(&row, "provider_authority", "false");
  AddEvidence(&row, "transaction_finality_authority", "false");
  AddEvidence(&row, "visibility_authority", "false");
  AddEvidence(&row, "security_authority", "false");
  AddEvidence(&row, "recovery_authority", "false");
  return row;
}

IndexFamilyBenchmarkEvidenceRow RefusedRow(
    const IndexFamilyDescriptor& descriptor,
    const IndexFamilyPhysicalCapabilityState& state,
    const WorkloadSpec& workload) {
  auto row = BaseRow(descriptor, state, workload);
  row.fail_closed = true;
  row.concrete_runtime_consumed = false;
  row.diagnostic_code =
      state.blocker_diagnostic_code.empty()
          ? "INDEX.CAPABILITY.UNKNOWN_FAMILY"
          : state.blocker_diagnostic_code;
  row.message_key = state.blocker_message_key.empty()
                        ? "index.capability.unknown_family"
                        : state.blocker_message_key;
  AddEvidence(&row, "capability_blocker", row.blocker);
  AddEvidence(&row, "fail_closed", "true");
  AddEvidence(&row, "planner_runtime_promotion", "false");
  AddEvidence(&row, "standalone_provider_gate", "false");
  AddEvidence(&row, "optimizer_selected_gate", "false");
  AddEvidence(&row, "route_consumed_gate", "false");
  AddEvidence(&row, "driver_visible_gate", "false");
  return row;
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind, u64 salt);

void ApplyRouteProof(IndexFamilyBenchmarkEvidenceRow* row,
                     IndexFamily family) {
  const auto* route_state =
      FindBuiltinIndexRouteCapabilityState(row->route_kind, family);
  if (route_state == nullptr) {
    row->fail_closed = true;
    row->concrete_runtime_consumed = false;
    row->diagnostic_code = "INDEX.ROUTE_CAPABILITY.UNKNOWN_ROUTE";
    row->message_key = "index.route_capability.unknown_route";
    AddEvidence(row, "route_capability_state", "missing");
    return;
  }

  row->route_consumed_gate = route_state->route_complete();
  row->dml_route_consumed = route_state->route_complete() &&
                            (row->route_kind == IndexRouteKind::dml_insert ||
                             row->route_kind == IndexRouteKind::dml_update ||
                             row->route_kind == IndexRouteKind::dml_delete);
  row->sql_query_route_consumed = route_state->route_complete() &&
                                  row->route_kind == IndexRouteKind::sql_select;
  row->nosql_route_consumed = route_state->route_complete() &&
                              (row->route_kind == IndexRouteKind::nosql_document ||
                               row->route_kind == IndexRouteKind::nosql_graph ||
                               row->route_kind == IndexRouteKind::nosql_vector ||
                               row->route_kind == IndexRouteKind::nosql_search);
  row->maintenance_route_consumed =
      route_state->route_complete() &&
      (row->route_kind == IndexRouteKind::maintenance ||
       row->route_kind == IndexRouteKind::validate_repair);
  AddEvidence(row,
              "route_capability_state",
              route_state->route_complete() ? "complete" : "refused");
  AddEvidence(row,
              "route_consumed_gate",
              row->route_consumed_gate ? "true" : "false");
  AddEvidence(row,
              "dml_route_consumed",
              row->dml_route_consumed ? "true" : "false");
  AddEvidence(row,
              "sql_query_route_consumed",
              row->sql_query_route_consumed ? "true" : "false");
  AddEvidence(row,
              "nosql_route_consumed",
              row->nosql_route_consumed ? "true" : "false");
  AddEvidence(row,
              "maintenance_route_consumed",
              row->maintenance_route_consumed ? "true" : "false");
  for (const auto& evidence : route_state->evidence) {
    row->evidence.push_back(evidence);
  }

  if (!route_state->route_complete()) {
    row->fail_closed = true;
    row->concrete_runtime_consumed = false;
    row->diagnostic_code = route_state->route_diagnostic_code;
    row->message_key = route_state->route_message_key;
    return;
  }

  row->standalone_provider_gate = true;
  AddEvidence(row, "standalone_provider_gate", "true");
  if (route_state->supports_read) {
    IndexOptimizerRequest optimizer;
    optimizer.index_uuid = GeneratedUuid(platform::UuidKind::object, 922);
    optimizer.family = family;
    optimizer.route = row->route_kind;
    optimizer.category = QueryCategoryForFamily(family);
    optimizer.requires_candidate_set = route_state->produces_candidate_set;
    optimizer.requires_negative_prune = route_state->supports_negative_prune;
    optimizer.requires_summary_segment_prune =
        route_state->supports_summary_segment_prune;
    optimizer.exact_rerank_available = route_state->requires_exact_rerank;
    optimizer.approximate = route_state->approximate_candidate_source;
    auto readiness =
        BenchmarkReadinessEvidence(family, row->route_kind, *route_state);
    optimizer.readiness_evidence = &readiness;
    const auto plan = PlanIndexOptimizerPath(optimizer);
    row->optimizer_selected_gate = plan.ok();
    AddEvidence(row,
                "optimizer_selected_gate",
                row->optimizer_selected_gate ? "true" : "false");
    if (!plan.ok()) {
      row->fail_closed = true;
      row->concrete_runtime_consumed = false;
      row->diagnostic_code = plan.diagnostic.diagnostic_code;
      row->message_key = plan.diagnostic.message_key;
    }
  } else {
    AddEvidence(row, "optimizer_selected_gate", "false");
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind, u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1780000000000ULL + salt);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

IndexValidationRepairTarget ValidationTarget(IndexFamily family, u64 salt) {
  IndexValidationRepairTarget target;
  target.database_uuid = GeneratedUuid(platform::UuidKind::database, salt + 1);
  target.table_uuid = GeneratedUuid(platform::UuidKind::object, salt + 2);
  target.index_uuid = GeneratedUuid(platform::UuidKind::object, salt + 3);
  target.generation_uuid = GeneratedUuid(platform::UuidKind::object, salt + 4);
  target.physical_family = family;
  return target;
}

IndexFamilyValidationRepairProof ValidationProof(
    const IndexFamilyDescriptor& descriptor) {
  IndexFamilyValidationRepairProof proof;
  proof.catalog_uuid_binding_proven = true;
  proof.exact_base_table_source_present = true;
  proof.physical_generation_present = true;
  proof.physical_generation_checksum_valid = true;
  proof.runtime_provider_attached = true;
  proof.runtime_epoch_current = true;
  proof.cold_start_source_present =
      descriptor.persistence ==
      IndexPersistenceClass::memory_primary_persisted_cold_start;
  proof.cold_start_checksum_valid = proof.cold_start_source_present;
  proof.repair_output_validated = true;
  proof.rebuild_output_validated = true;
  proof.proof_token = "index-family-benchmark-proof";
  return proof;
}

OperationSample ValidationSample(const IndexFamilyDescriptor& descriptor,
                                 bool mutating,
                                 u64 salt) {
  IndexFamilyValidationRepairRequest request;
  request.operation = mutating ? IndexValidationRepairOperation::rebuild
                               : IndexValidationRepairOperation::validate;
  request.family = descriptor.family;
  request.target = ValidationTarget(descriptor.family, salt);
  request.proof = ValidationProof(descriptor);
  request.policy_allows_mutation = mutating;
  const auto result = ExecuteIndexFamilyValidationRepairOperation(request);
  OperationSample sample;
  sample.ok = result.ok();
  sample.diagnostic_code = result.diagnostic.diagnostic_code;
  sample.message_key = result.diagnostic.message_key;
  sample.operation_count = 1;
  sample.rows_examined = 1;
  sample.rows_returned = result.planner_visible ? 1 : 0;
  sample.pages_or_containers_touched = 1;
  sample.evidence.push_back("validation_repair_route_consumed=true");
  sample.evidence.push_back(std::string("validation_state=") +
                            result.validation_state);
  sample.evidence.push_back(std::string("repair_state=") +
                            result.repair_state);
  return sample;
}

std::vector<platform::byte> SortableI64Payload(std::int64_t value) {
  const auto sortable =
      static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<platform::byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<platform::byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

IndexKeyEncodingResult EncodeBenchKeyBytes(std::vector<platform::byte> payload,
                                           u64 type_salt) {
  IndexKeyEncodingComponent component;
  component.kind = IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid =
      GeneratedUuid(platform::UuidKind::object, type_salt);
  component.sort_direction = IndexKeySortDirection::ascending;
  component.null_placement = IndexKeyNullPlacement::nulls_last;
  component.payload = std::move(payload);
  return EncodeIndexKey({component}, {});
}

IndexKeyEncodingResult EncodeBenchIntKey(std::int64_t value, u64 type_salt) {
  return EncodeBenchKeyBytes(SortableI64Payload(value), type_salt);
}

IndexKeyEncodingResult EncodeBenchTextKey(std::string_view value,
                                          u64 type_salt) {
  return EncodeBenchKeyBytes({value.begin(), value.end()}, type_salt);
}

std::string EncodedString(const IndexKeyEncodingResult& encoded) {
  return std::string(reinterpret_cast<const char*>(encoded.encoded.data()),
                     encoded.encoded.size());
}

CandidateSetAuthorityContext CandidateAuthority() {
  CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

bool WorkloadIsValidation(std::string_view workload) {
  return workload == "validate";
}

bool WorkloadIsRepair(std::string_view workload) {
  return workload == "repair_rebuild";
}

bool WorkloadIsReopen(std::string_view workload) {
  return workload == "crash_reopen" || workload == "cold_cache";
}

bool WorkloadIsMaintenance(std::string_view workload) {
  return workload == "update_delete_maintenance";
}

void SetSampleDiagnostic(OperationSample* sample,
                         const DiagnosticRecord& diagnostic) {
  sample->diagnostic_code = diagnostic.diagnostic_code;
  sample->message_key = diagnostic.message_key;
}

page::IndexBtreeCell BtreeCell(u64 salt,
                               std::string_view key,
                               platform::byte suffix) {
  page::IndexBtreeCell cell;
  const auto encoded = EncodeBenchTextKey(key, salt + 900);
  cell.encoded_key = encoded.encoded;
  cell.row_uuid = GeneratedUuid(platform::UuidKind::row, salt + suffix);
  cell.version_uuid =
      GeneratedUuid(platform::UuidKind::row, salt + 100 + suffix);
  return cell;
}

std::vector<page::IndexBtreeCell> BtreeCells(u64 salt) {
  return {BtreeCell(salt, "alpha", 1),
          BtreeCell(salt, "alpine", 2),
          BtreeCell(salt, "bravo", 3),
          BtreeCell(salt, "charlie", 4),
          BtreeCell(salt, "delta", 5),
          BtreeCell(salt, "echo", 6)};
}

OperationSample BtreeSample(const IndexFamilyDescriptor& descriptor,
                            const WorkloadSpec& workload,
                            u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }

  OperationSample sample;
  sample.operation_count = 1;
  auto cells = BtreeCells(salt);
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, salt + 70);
  if (workload.workload == std::string("bulk_build")) {
    page::IndexBtreePhysicalBulkBuildRequest request;
    request.index_uuid = index_uuid;
    request.page_size = 512;
    request.leaf_entry_capacity = 3;
    request.internal_entry_capacity = 3;
    request.sorted_cells = cells;
    request.sorted_order_proof_valid = true;
    const auto built = page::BuildIndexBtreePhysicalBulkLoadedTree(request);
    sample.ok = built.ok();
    SetSampleDiagnostic(&sample, built.diagnostic);
    sample.rows_examined = cells.size();
    sample.rows_returned = built.report.tuple_live_entry_estimate;
    sample.pages_or_containers_touched = built.report.page_count;
    sample.evidence.push_back("btree_bulk_leaf_branch_route_consumed=true");
    return sample;
  }

  auto initialized =
      page::InitializeIndexBtreePhysicalTree(index_uuid, 512);
  sample.ok = initialized.ok();
  SetSampleDiagnostic(&sample, initialized.diagnostic);
  if (!sample.ok) return sample;
  auto tree = std::move(initialized.tree);
  for (const auto& cell : cells) {
    if (descriptor.family == IndexFamily::unique_btree) {
      page::IndexBtreePhysicalUniqueInsertRequest request;
      request.cell = cell;
      const auto inserted = page::InsertUniqueIndexBtreeCell(&tree, request);
      sample.ok = inserted.ok() && inserted.inserted;
      SetSampleDiagnostic(&sample, inserted.diagnostic);
    } else {
      const auto inserted = page::InsertIndexBtreeCell(&tree, {cell});
      sample.ok = inserted.ok() && inserted.inserted;
      SetSampleDiagnostic(&sample, inserted.diagnostic);
    }
    if (!sample.ok) return sample;
  }

  if (WorkloadIsMaintenance(workload.workload)) {
    const auto deleted = page::DeleteIndexBtreeCell(&tree, {cells[2]});
    sample.ok = deleted.ok() && deleted.deleted;
    SetSampleDiagnostic(&sample, deleted.diagnostic);
    sample.operation_count += 1;
    sample.rows_examined = cells.size();
    sample.rows_returned = sample.ok ? cells.size() - 1 : 0;
    sample.pages_or_containers_touched = tree.pages.size();
    sample.evidence.push_back("btree_delete_rebalance_route_consumed=true");
    return sample;
  }

  if (WorkloadIsReopen(workload.workload)) {
    const auto exported = page::ExportIndexBtreePhysicalTreeImage(tree);
    sample.ok = exported.ok();
    SetSampleDiagnostic(&sample, exported.diagnostic);
    if (!sample.ok) return sample;
    const auto imported = page::ImportIndexBtreePhysicalTreeImage(exported.image);
    sample.ok = imported.ok();
    SetSampleDiagnostic(&sample, imported.diagnostic);
    sample.rows_examined = cells.size();
    sample.rows_returned = imported.ok() ? cells.size() : 0;
    sample.pages_or_containers_touched = exported.image.pages.size();
    sample.evidence.push_back("btree_crash_reopen_route_consumed=true");
    return sample;
  }

  page::IndexBtreePhysicalScanResult scanned;
  if (workload.workload == std::string("point_lookup")) {
    scanned = page::PointLookupIndexBtreePhysicalTree(tree, cells[2].encoded_key);
    sample.evidence.push_back("btree_point_lookup_route_consumed=true");
  } else if (workload.workload == std::string("range_lookup")) {
    IndexKeyEncodingComponent component;
    component.kind = IndexKeyComponentKind::scalar;
    component.ordinal = 0;
    component.type_descriptor_uuid =
        GeneratedUuid(platform::UuidKind::object, salt + 900);
    component.sort_direction = IndexKeySortDirection::ascending;
    component.null_placement = IndexKeyNullPlacement::nulls_last;
    component.payload = {platform::byte('a'), platform::byte('l')};
    const auto prefix = BuildEncodedPrefixMatcher({component}, {});
    if (!prefix.ok()) {
      sample.ok = false;
      SetSampleDiagnostic(&sample, prefix.diagnostic);
      return sample;
    }
    scanned = page::PrefixScanIndexBtreePhysicalTree(tree,
                                                     prefix.matcher_prefix);
    sample.evidence.push_back("btree_prefix_scan_route_consumed=true");
  } else {
    scanned = page::OrderedScanIndexBtreePhysicalTree(tree);
    sample.evidence.push_back("btree_ordered_scan_route_consumed=true");
  }
  sample.ok = scanned.ok();
  SetSampleDiagnostic(&sample, scanned.diagnostic);
  sample.rows_examined = scanned.visited_leaf_pages == 0
                             ? cells.size()
                             : scanned.visited_leaf_pages;
  sample.rows_returned = scanned.locators.size();
  sample.pages_or_containers_touched = scanned.visited_leaf_pages;
  return sample;
}

std::vector<platform::byte> HashKey(std::string_view key) {
  return {key.begin(), key.end()};
}

page::IndexHashPhysicalInsertResult HashInsert(
    page::IndexHashPhysicalIndex* index,
    const std::vector<platform::byte>& key,
    u64 salt) {
  const auto route = page::LocateIndexHashBucket(*index, key);
  if (!route.ok()) return {};
  auto latch = page::AcquireIndexHashBucketExclusiveLatch(
      index, route.bucket_page_number);
  page::IndexHashPhysicalInsertRequest request;
  request.encoded_key = key;
  request.row_uuid = GeneratedUuid(platform::UuidKind::row, salt + 1);
  request.version_uuid = GeneratedUuid(platform::UuidKind::row, salt + 2);
  request.latch_evidence = latch.evidence();
  return page::InsertIndexHashEntry(index, request);
}

page::IndexHashPhysicalProbeResult HashProbe(
    const page::IndexHashPhysicalIndex& index,
    const std::vector<platform::byte>& key) {
  const auto route = page::LocateIndexHashBucket(index, key);
  if (!route.ok()) return {};
  auto latch = page::AcquireIndexHashBucketSharedLatch(index,
                                                       route.bucket_page_number);
  page::IndexHashPhysicalProbeRequest request;
  request.encoded_key = key;
  request.latch_evidence = latch.evidence();
  return page::ProbeIndexHashBucket(index, request);
}

OperationSample HashSample(const IndexFamilyDescriptor& descriptor,
                           const WorkloadSpec& workload,
                           u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const platform::u16 algorithm_version =
      workload.fallback_disabled
          ? page::kIndexHashHighAssuranceAlgorithmVersion
          : page::kIndexHashProductionDefaultAlgorithmVersion;
  auto initialized = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(platform::UuidKind::object, salt + 10),
      2048,
      0,
      algorithm_version,
      2);
  sample.ok = initialized.ok();
  SetSampleDiagnostic(&sample, initialized.diagnostic);
  if (!sample.ok) return sample;
  auto index = std::move(initialized.index);
  const std::vector<std::string> keys = {
      "alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto inserted = HashInsert(&index, HashKey(keys[i]), salt + i * 10);
    sample.ok = inserted.ok() && inserted.inserted;
    SetSampleDiagnostic(&sample, inserted.diagnostic);
    if (!sample.ok) return sample;
    sample.evidence.insert(sample.evidence.end(),
                           inserted.evidence.begin(),
                           inserted.evidence.end());
  }
  if (WorkloadIsReopen(workload.workload)) {
    const auto image = page::ExportIndexHashPhysicalIndexImage(index);
    sample.ok = image.ok();
    SetSampleDiagnostic(&sample, image.diagnostic);
    if (!sample.ok) return sample;
    const auto imported = page::ImportIndexHashPhysicalIndexImage(image.image);
    sample.ok = imported.ok();
    SetSampleDiagnostic(&sample, imported.diagnostic);
    sample.rows_examined = keys.size();
    sample.rows_returned = imported.ok() ? keys.size() : 0;
    sample.pages_or_containers_touched = image.image.pages.size();
    sample.evidence.push_back("hash_crash_reopen_route_consumed=true");
    sample.evidence.insert(sample.evidence.end(),
                           imported.evidence.begin(),
                           imported.evidence.end());
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    const auto key = HashKey("bravo");
    const auto probe = HashProbe(index, key);
    sample.ok = probe.ok() && !probe.locators.empty();
    SetSampleDiagnostic(&sample, probe.diagnostic);
    if (!sample.ok) return sample;
    const auto route = page::LocateIndexHashBucket(index, key);
    sample.ok = route.ok();
    SetSampleDiagnostic(&sample, route.diagnostic);
    if (!sample.ok) return sample;
    auto latch = page::AcquireIndexHashBucketExclusiveLatch(
        &index, route.bucket_page_number);
    page::IndexHashPhysicalDeleteRequest request;
    request.encoded_key = key;
    request.row_uuid = probe.locators.front().row_uuid;
    request.version_uuid = probe.locators.front().version_uuid;
    request.latch_evidence = latch.evidence();
    const auto deleted = page::DeleteIndexHashEntry(&index, request);
    sample.ok = deleted.ok() && deleted.tombstone_marked;
    SetSampleDiagnostic(&sample, deleted.diagnostic);
    sample.rows_examined = probe.collision_entries_traversed;
    sample.rows_returned = deleted.deleted ? 1 : 0;
    sample.pages_or_containers_touched = probe.pages_traversed;
    sample.evidence.push_back("hash_delete_tombstone_route_consumed=true");
    sample.evidence.insert(sample.evidence.end(),
                           deleted.evidence.begin(),
                           deleted.evidence.end());
    return sample;
  }
  const auto probe = HashProbe(index, HashKey("bravo"));
  sample.ok = probe.ok();
  SetSampleDiagnostic(&sample, probe.diagnostic);
  sample.operation_count = 1;
  sample.rows_examined = probe.collision_entries_traversed;
  sample.rows_returned = probe.locators.size();
  sample.pages_or_containers_touched = probe.pages_traversed;
  sample.evidence.push_back("hash_probe_route_consumed=true");
  sample.evidence.insert(sample.evidence.end(),
                         probe.evidence.begin(),
                         probe.evidence.end());
  return sample;
}

OperationSample BitmapSample(const IndexFamilyDescriptor& descriptor,
                             const WorkloadSpec& workload,
                             u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto authority = CandidateAuthority();
  auto left = MakeCompressedBitmapCandidateSetFromRowOrdinals(
      {1, 2, 3, 64, 65, 66, 1024, 2048}, authority, true);
  auto right = MakeCompressedBitmapCandidateSetFromRowOrdinals(
      {3, 4, 5, 65, 4096}, authority, true);
  sample.ok = left.ok() && right.ok();
  if (!sample.ok) {
    SetSampleDiagnostic(&sample,
                        left.ok() ? right.diagnostic : left.diagnostic);
    return sample;
  }
  CandidateSetResult result;
  if (workload.workload == std::string("point_lookup")) {
    result = CandidateSetPopcount(left.output, authority).ok()
                 ? TopKCandidateSet(left.output, 3, authority)
                 : CandidateSetResult{};
    sample.evidence.push_back("bitmap_topk_popcount_route_consumed=true");
  } else if (workload.workload == std::string("range_lookup") ||
             workload.workload == std::string("warm_cache") ||
             workload.workload == std::string("fallback_disabled")) {
    result = IntersectCandidateSets(left.output, right.output, authority);
    sample.evidence.push_back("bitmap_intersection_route_consumed=true");
  } else if (WorkloadIsMaintenance(workload.workload)) {
    result = SubtractCandidateSets(left.output, right.output, authority);
    sample.evidence.push_back("bitmap_deleted_overlay_subtract_route_consumed=true");
  } else if (WorkloadIsReopen(workload.workload)) {
    const auto serialized =
        SerializeCompressedBitmapCandidateSet(left.output);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    result = DeserializeCompressedBitmapCandidateSet(serialized.serialized,
                                                     authority);
    sample.evidence.push_back("bitmap_serialize_reopen_route_consumed=true");
  } else {
    result = UnionCandidateSets(left.output, right.output, authority);
    sample.evidence.push_back("bitmap_union_route_consumed=true");
  }
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.operation_count = 1;
  sample.rows_examined = left.output.compressed_bitmap_cardinality +
                         right.output.compressed_bitmap_cardinality;
  sample.rows_returned = result.output.compressed_bitmap_cardinality;
  sample.pages_or_containers_touched =
      result.output.compressed_bitmap_containers.size();
  sample.evidence.push_back("bitmap_physical_container_route_executed=true");
  return sample;
}

PhysicalBloomEncodedKeyEvidence BloomKey(std::int64_t value, u64 salt) {
  PhysicalBloomEncodedKeyEvidence key;
  key.row_uuid = GeneratedUuidText(platform::UuidKind::row, salt + value);
  key.version_uuid =
      GeneratedUuidText(platform::UuidKind::row, salt + 1000 + value);
  key.encoded_key = EncodedString(EncodeBenchIntKey(value, salt + 800));
  return key;
}

PhysicalBloomAbsentProbeEvidence BloomAbsent(std::int64_t value, u64 salt) {
  PhysicalBloomAbsentProbeEvidence absent;
  absent.encoded_key = EncodedString(EncodeBenchIntKey(value, salt + 800));
  return absent;
}

PhysicalBloomFilterBuildRequest BloomBuildRequest(u64 salt) {
  PhysicalBloomFilterBuildRequest request;
  request.relation_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 1);
  request.index_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 2);
  request.segment_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 3);
  request.base_generation = 7;
  request.filter_generation = 11;
  request.seed = 0x123456789abcdef0ull;
  request.seed_version = 1;
  request.hash_count = 4;
  request.bit_count = 4096;
  request.fpr_target = 0.05;
  request.absent_probe_sample_required_for_benchmark_clean = true;
  request.authoritative_keys = {BloomKey(10, salt),
                                BloomKey(20, salt),
                                BloomKey(30, salt),
                                BloomKey(40, salt)};
  request.absent_probe_sample = {BloomAbsent(1000, salt),
                                 BloomAbsent(1001, salt),
                                 BloomAbsent(1002, salt),
                                 BloomAbsent(1003, salt)};
  return request;
}

OperationSample BloomSample(const IndexFamilyDescriptor& descriptor,
                            const WorkloadSpec& workload,
                            u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built =
      BuildPhysicalBloomFilterFromEncodedKeyEvidence(BloomBuildRequest(salt));
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = built.page.inserted_key_count;
  sample.pages_or_containers_touched = built.page.layout.block_count;
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializePhysicalBloomFilterPage(built.page);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    PhysicalBloomFilterOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = built.page.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = built.page.index_uuid;
    open.expected_segment_uuid_present = true;
    open.expected_segment_uuid = built.page.segment_uuid;
    open.expected_base_generation_present = true;
    open.expected_base_generation = built.page.base_generation;
    const auto opened = OpenPhysicalBloomFilterPage(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.page.inserted_key_count : 0;
    sample.evidence.push_back("bloom_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    PhysicalBloomFilterMutation mutation;
    mutation.kind = PhysicalBloomMutationKind::append_key;
    mutation.after_key_present = true;
    mutation.after_key = BloomKey(50, salt);
    const auto mutated = ApplyPhysicalBloomFilterMutation(built.page, mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.page.inserted_key_count;
    sample.evidence.push_back("bloom_mutation_route_consumed=true");
    return sample;
  }
  const auto probe = ProbePhysicalBloomFilter(
      {built.page, BloomAbsent(1000, salt).encoded_key});
  sample.ok = probe.ok();
  SetSampleDiagnostic(&sample, probe.diagnostic);
  sample.rows_returned = probe.can_prune ? 1 : 0;
  sample.evidence.push_back("bloom_negative_prune_probe_route_consumed=true");
  return sample;
}

PhysicalZoneColumnValueEvidence ZoneIntColumn(std::int64_t value, u64 salt) {
  PhysicalZoneColumnValueEvidence column;
  column.column_ordinal = 0;
  column.scalar_type_key = "sb.i64";
  column.encoded_scalar = EncodedString(EncodeBenchIntKey(value, salt + 810));
  return column;
}

PhysicalZoneColumnValueEvidence ZoneTextColumn(std::string_view value,
                                               u64 salt) {
  PhysicalZoneColumnValueEvidence column;
  column.column_ordinal = 1;
  column.scalar_type_key = "sb.text";
  column.encoded_scalar = EncodedString(EncodeBenchTextKey(value, salt + 820));
  return column;
}

PhysicalZoneRowEvidence ZoneRow(u64 page,
                                u64 generation,
                                std::vector<PhysicalZoneColumnValueEvidence> columns) {
  PhysicalZoneRowEvidence row;
  row.page_id = page;
  row.extent_id = page / 2;
  row.base_generation = generation;
  row.columns = std::move(columns);
  return row;
}

std::vector<PhysicalZoneRowEvidence> ZoneRows(u64 salt) {
  return {ZoneRow(0, 7, {ZoneIntColumn(10, salt), ZoneTextColumn("blue", salt)}),
          ZoneRow(1, 7, {ZoneIntColumn(20, salt), ZoneTextColumn("red", salt)}),
          ZoneRow(2, 7, {ZoneIntColumn(30, salt), ZoneTextColumn("green", salt)}),
          ZoneRow(3, 7, {ZoneIntColumn(40, salt), ZoneTextColumn("green", salt)})};
}

PhysicalZoneSummaryBuildRequest ZoneBuildRequest(u64 salt) {
  PhysicalZoneSummaryBuildRequest request;
  request.relation_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 11);
  request.summary_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 12);
  request.range_sizing.min_pages_per_range = 1;
  request.range_sizing.target_pages_per_range = 2;
  request.range_sizing.max_pages_per_range = 4;
  request.range_sizing.base_generation = 7;
  request.range_sizing.summary_generation = 11;
  request.range_sizing.adaptive = true;
  request.small_set_limit = 3;
  request.base_page_rows = ZoneRows(salt);
  return request;
}

OperationSample ZoneSummarySample(const IndexFamilyDescriptor& descriptor,
                                  const WorkloadSpec& workload,
                                  u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built =
      BuildPhysicalZoneSummaryFromBasePageEvidence(ZoneBuildRequest(salt));
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = ZoneRows(salt).size();
  sample.pages_or_containers_touched = built.page.ranges.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializePhysicalZoneSummaryPage(built.page);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    PhysicalZoneSummaryOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_base_generation_present = true;
    open.expected_base_generation = built.page.range_sizing.base_generation;
    const auto opened = OpenPhysicalZoneSummaryPage(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.page.ranges.size() : 0;
    sample.evidence.push_back("zone_summary_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    PhysicalZoneSummaryMutation mutation;
    mutation.kind = PhysicalZoneSummaryMutationKind::append_row;
    mutation.after_row_present = true;
    mutation.after_row =
        ZoneRow(4, 7, {ZoneIntColumn(50, salt), ZoneTextColumn("orange", salt)});
    const auto mutated = ApplyPhysicalZoneSummaryMutation(built.page, mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.page.ranges.size();
    sample.evidence.push_back("zone_summary_resummarize_route_consumed=true");
    return sample;
  }
  PhysicalZonePredicate predicate;
  predicate.column_ordinal = 0;
  predicate.scalar_type_key = "sb.i64";
  predicate.lower_present = true;
  predicate.upper_present = true;
  predicate.encoded_lower = EncodedString(EncodeBenchIntKey(25, salt + 810));
  predicate.encoded_upper = EncodedString(EncodeBenchIntKey(35, salt + 810));
  const auto pruned = PrunePhysicalZoneSummaryRanges({built.page, {predicate}});
  sample.ok = pruned.ok();
  SetSampleDiagnostic(&sample, pruned.diagnostic);
  sample.rows_returned = pruned.ranges.size();
  sample.pages_or_containers_touched = pruned.ranges.size();
  sample.evidence.push_back("zone_summary_prune_route_consumed=true");
  return sample;
}

PhysicalColumnarZoneColumnValueEvidence ColumnarIntColumn(std::int64_t value,
                                                          u64 salt) {
  PhysicalColumnarZoneColumnValueEvidence column;
  column.column_ordinal = 0;
  column.scalar_type_key = "sb.i64";
  column.encoded_scalar = EncodedString(EncodeBenchIntKey(value, salt + 830));
  return column;
}

PhysicalColumnarZoneColumnValueEvidence ColumnarTextColumn(std::string_view value,
                                                           u64 salt) {
  PhysicalColumnarZoneColumnValueEvidence column;
  column.column_ordinal = 1;
  column.scalar_type_key = "sb.text";
  column.encoded_scalar = EncodedString(EncodeBenchTextKey(value, salt + 840));
  return column;
}

PhysicalColumnarZoneRowEvidence ColumnarRow(
    u64 page,
    u64 group,
    u64 ordinal,
    std::vector<PhysicalColumnarZoneColumnValueEvidence> columns) {
  PhysicalColumnarZoneRowEvidence row;
  row.page_id = page;
  row.row_group_id = group;
  row.row_ordinal = ordinal;
  row.base_generation = 7;
  row.columns = std::move(columns);
  return row;
}

std::vector<PhysicalColumnarZoneRowEvidence> ColumnarRows(u64 salt) {
  return {ColumnarRow(0, 10, 0, {ColumnarIntColumn(10, salt), ColumnarTextColumn("blue", salt)}),
          ColumnarRow(1, 10, 1, {ColumnarIntColumn(20, salt), ColumnarTextColumn("red", salt)}),
          ColumnarRow(2, 11, 10, {ColumnarIntColumn(30, salt), ColumnarTextColumn("green", salt)}),
          ColumnarRow(3, 11, 11, {ColumnarIntColumn(40, salt), ColumnarTextColumn("orange", salt)})};
}

std::vector<PhysicalColumnarZoneCompressionPolicy> ColumnarPolicies() {
  PhysicalColumnarZoneCompressionPolicy i64;
  i64.column_ordinal = 0;
  i64.scalar_type_key = "sb.i64";
  i64.codec_id = "delta_bitpack_v1";
  i64.uncompressed_bytes = 800;
  i64.compressed_bytes = 240;
  PhysicalColumnarZoneCompressionPolicy text;
  text.column_ordinal = 1;
  text.scalar_type_key = "sb.text";
  text.codec_id = "dictionary_lz4_v1";
  text.uncompressed_bytes = 1200;
  text.compressed_bytes = 360;
  return {i64, text};
}

PhysicalColumnarZoneBuildRequest ColumnarBuildRequest(u64 salt) {
  PhysicalColumnarZoneBuildRequest request;
  request.relation_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 21);
  request.index_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 22);
  request.segment_uuid = GeneratedUuidText(platform::UuidKind::object, salt + 23);
  request.base_generation = 7;
  request.summary_generation = 11;
  request.dictionary_limit = 3;
  request.rows = ColumnarRows(salt);
  request.compression_policies = ColumnarPolicies();
  return request;
}

OperationSample ColumnarZoneSample(const IndexFamilyDescriptor& descriptor,
                                   const WorkloadSpec& workload,
                                   u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built =
      BuildPhysicalColumnarZoneFromPageEvidence(ColumnarBuildRequest(salt));
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = ColumnarRows(salt).size();
  sample.pages_or_containers_touched = built.segment.row_groups.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializePhysicalColumnarZoneSegment(built.segment);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    PhysicalColumnarZoneOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = built.segment.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = built.segment.index_uuid;
    open.expected_segment_uuid_present = true;
    open.expected_segment_uuid = built.segment.segment_uuid;
    const auto opened = OpenPhysicalColumnarZoneSegment(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.segment.row_groups.size() : 0;
    sample.evidence.push_back("columnar_zone_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    PhysicalColumnarZoneMutation mutation;
    mutation.kind = PhysicalColumnarZoneMutationKind::append_row;
    mutation.after_row_present = true;
    mutation.after_row =
        ColumnarRow(4, 12, 20, {ColumnarIntColumn(50, salt),
                                ColumnarTextColumn("purple", salt)});
    const auto mutated =
        ApplyPhysicalColumnarZoneMutation(built.segment, mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.segment.row_groups.size();
    sample.evidence.push_back("columnar_zone_mutation_route_consumed=true");
    return sample;
  }
  PhysicalColumnarZonePredicate predicate;
  predicate.column_ordinal = 0;
  predicate.scalar_type_key = "sb.i64";
  predicate.lower_present = true;
  predicate.upper_present = true;
  predicate.encoded_lower = EncodedString(EncodeBenchIntKey(15, salt + 830));
  predicate.encoded_upper = EncodedString(EncodeBenchIntKey(45, salt + 830));
  const auto pruned = PrunePhysicalColumnarZone({built.segment, {predicate}});
  sample.ok = pruned.ok();
  SetSampleDiagnostic(&sample, pruned.diagnostic);
  if (!sample.ok) return sample;
  const auto stream = OpenPhysicalColumnarZoneCandidateStream(
      {built.segment, pruned, {0, 1}});
  sample.ok = stream.ok();
  SetSampleDiagnostic(&sample, stream.diagnostic);
  sample.rows_returned = stream.candidate_row_ordinals.size();
  sample.pages_or_containers_touched = pruned.groups.size();
  sample.evidence.push_back("columnar_zone_late_materialization_route_consumed=true");
  return sample;
}

std::string UuidWithSuffix(const char* prefix, u64 suffix) {
  std::ostringstream out;
  out << prefix << std::setw(12) << std::setfill('0') << suffix;
  return out.str();
}

TextInvertedRowLocator Locator(u64 row) {
  TextInvertedRowLocator locator;
  locator.row_ordinal = row;
  locator.row_uuid = UuidWithSuffix("14141414-1414-7414-8414-", row);
  locator.version_uuid = UuidWithSuffix("15151515-1515-7515-8515-", row);
  return locator;
}

TextInvertedExactRecheckProof TextProof() {
  TextInvertedExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.source_recheck_required = true;
  proof.evidence_ref = "benchmark_text_exact_mga_security_recheck";
  return proof;
}

TextInvertedDocumentInput TextDoc(u64 row,
                                  std::vector<std::string> terms) {
  TextInvertedDocumentInput doc;
  doc.locator = Locator(row);
  doc.normalized_terms = std::move(terms);
  doc.exact_source_recheck_evidence_ref =
      "text_exact_source_recheck_" + std::to_string(row);
  return doc;
}

std::vector<TextInvertedDocumentInput> TextDocumentsForBench() {
  return {TextDoc(10, {"alpha", "beta", "gamma"}),
          TextDoc(20, {"alpha", "gamma"}),
          TextDoc(30, {"alpha", "beta"}),
          TextDoc(40, {"omega"})};
}

TextInvertedSegmentBuildRequest TextBuildRequest(u64 generation) {
  TextInvertedSegmentBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.segment_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.segment_generation = generation;
  request.analyzer_epoch = 13;
  request.resource_epoch = 17;
  request.block_posting_target = 2;
  request.recheck_proof = TextProof();
  return request;
}

OperationSample TextRuntimeSample(const IndexFamilyDescriptor& descriptor,
                                  const WorkloadSpec& workload,
                                  u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto sealed =
      BuildTextInvertedSegmentFromDocuments(TextBuildRequest(11),
                                            TextDocumentsForBench());
  sample.ok = sealed.ok();
  SetSampleDiagnostic(&sample, sealed.diagnostic);
  sample.rows_examined = TextDocumentsForBench().size();
  sample.pages_or_containers_touched = sealed.segment.posting_blocks.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized =
        SerializeTextInvertedSegmentArtifact(sealed.segment);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    TextInvertedSegmentOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = sealed.segment.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = sealed.segment.index_uuid;
    open.expected_segment_uuid_present = true;
    open.expected_segment_uuid = sealed.segment.segment_uuid;
    open.expected_base_generation_present = true;
    open.expected_base_generation = sealed.segment.base_generation;
    open.expected_segment_generation_present = true;
    open.expected_segment_generation = sealed.segment.segment_generation;
    open.expected_analyzer_epoch_present = true;
    open.expected_analyzer_epoch = sealed.segment.analyzer_epoch;
    open.expected_resource_epoch_present = true;
    open.expected_resource_epoch = sealed.segment.resource_epoch;
    open.recheck_proof = TextProof();
    const auto opened = OpenTextInvertedSegmentArtifact(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.segment.documents.size() : 0;
    sample.evidence.push_back("text_segment_crash_reopen_route_consumed=true");
    return sample;
  }
  if (descriptor.family == IndexFamily::full_text ||
      descriptor.family == IndexFamily::sparse_wand) {
    FullTextRuntimeRequest request;
    request.segments.emplace_back(sealed.segment);
    request.kind = workload.workload == std::string("range_lookup")
                       ? FullTextRuntimeQueryKind::phrase
                       : FullTextRuntimeQueryKind::all_terms;
    request.terms = {"alpha", "beta"};
    request.top_k = 3;
    request.candidate_recheck_proof = TextProof();
    request.exact_rerank_proof.proof_supplied = true;
    request.exact_rerank_proof.exact_source_rows_available = true;
    request.exact_rerank_proof.mga_visibility_recheck_available = true;
    request.exact_rerank_proof.security_authorization_recheck_available = true;
    request.exact_rerank_proof.score_order_recheck_available = true;
    request.exact_rerank_proof.evidence_ref =
        "full_text_exact_rerank_contract";
    const auto result = ExecuteFullTextRuntime(request);
    sample.ok = result.ok();
    SetSampleDiagnostic(&sample, result.diagnostic);
    sample.rows_returned = result.candidates.size();
    sample.pages_or_containers_touched = result.scanned_block_count;
    sample.evidence.push_back("full_text_bm25_wand_route_consumed=true");
    return sample;
  }
  TextInvertedQueryRequest query;
  query.segment = sealed.segment;
  query.kind = workload.workload == std::string("range_lookup")
                   ? TextInvertedQueryKind::all_terms
                   : TextInvertedQueryKind::term;
  query.terms = {"alpha", "beta"};
  query.recheck_proof = TextProof();
  const auto result = ProbeTextInvertedSegment(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.scanned_block_count;
  sample.evidence.push_back("text_inverted_probe_route_consumed=true");
  return sample;
}

GinExactRecheckProof GinProof() {
  GinExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "gin_exact_source_mga_security_recheck_contract";
  return proof;
}

GinOpclassDescriptor GinOpclassForBench() {
  GinOpclassDescriptor opclass;
  opclass.opclass_name = "text_token_array_ops";
  opclass.opclass_epoch = 13;
  opclass.resource_epoch = 17;
  opclass.deterministic = true;
  opclass.immutable = true;
  opclass.safe = true;
  opclass.tri_consistent_supported = true;
  return opclass;
}

std::vector<std::string> SplitBenchTokens(std::string value) {
  std::vector<std::string> tokens;
  std::string current;
  for (char ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch == ' ') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) tokens.push_back(current);
  std::sort(tokens.begin(), tokens.end());
  tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  return tokens;
}

GinOpclassExtractor GinExtractorForBench() {
  return [](const GinOpclassExtractorInput& input) {
    GinOpclassExtractorOutput output;
    output.keys = SplitBenchTokens(input.exact_source_value);
    output.deterministic = true;
    output.exact_source_recheck_evidence_present = true;
    output.evidence_ref =
        "gin_opclass_extractor_source_recheck_" +
        std::to_string(input.locator.row_ordinal);
    return output;
  };
}

std::vector<GinSourceRow> GinRowsForBench() {
  return {{Locator(10), "alpha beta"},
          {Locator(20), "alpha gamma"},
          {Locator(30), "alpha beta"},
          {Locator(40), "alpha beta"},
          {Locator(50), "omega"}};
}

GinPhysicalBuildRequest GinBuildRequestForBench() {
  GinPhysicalBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.opclass = GinOpclassForBench();
  request.pending_flush_threshold = 2;
  request.posting_list_limit = 2;
  request.recheck_proof = GinProof();
  request.rows = GinRowsForBench();
  request.extractor = GinExtractorForBench();
  return request;
}

OperationSample GinSample(const IndexFamilyDescriptor& descriptor,
                          const WorkloadSpec& workload,
                          u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built = BuildGinPhysicalProvider(GinBuildRequestForBench());
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = GinRowsForBench().size();
  sample.pages_or_containers_touched = built.provider.entries.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeGinPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    GinPhysicalOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = built.provider.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = built.provider.index_uuid;
    open.expected_provider_uuid_present = true;
    open.expected_provider_uuid = built.provider.provider_uuid;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_opclass_epoch_present = true;
    open.expected_opclass_epoch = built.provider.opclass.opclass_epoch;
    open.expected_resource_epoch_present = true;
    open.expected_resource_epoch = built.provider.opclass.resource_epoch;
    open.recheck_proof = GinProof();
    const auto opened = OpenGinPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.entries.size() : 0;
    sample.evidence.push_back("gin_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    GinPhysicalMutation mutation;
    mutation.kind = GinMutationKind::insert_row;
    mutation.after_row_present = true;
    mutation.after_row = {Locator(60), "alpha delta"};
    mutation.recheck_proof = GinProof();
    mutation.extractor = GinExtractorForBench();
    const auto mutated = ApplyGinPhysicalMutation(built.provider, mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.provider.entries.size();
    sample.evidence.push_back("gin_pending_posting_tree_mutation_route_consumed=true");
    return sample;
  }
  GinTriConsistentRequest query;
  query.provider = built.provider;
  query.strategy = GinQueryStrategy::contains_all;
  query.query_keys = {"alpha", "beta"};
  query.recheck_proof = GinProof();
  const auto result = ExecuteGinTriConsistent(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.posting_list_probe_count +
                                      result.posting_tree_probe_count +
                                      result.pending_list_probe_count;
  sample.evidence.push_back("gin_tri_consistent_route_consumed=true");
  return sample;
}

NgramExactRecheckProof NgramProof() {
  NgramExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_batch_available = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "ngram_exact_source_batch_mga_security_recheck_contract";
  return proof;
}

NgramTokenizerDescriptor NgramTokenizerForBench() {
  NgramTokenizerDescriptor tokenizer;
  tokenizer.tokenizer_epoch = 19;
  tokenizer.charset_epoch = 23;
  tokenizer.resource_epoch = 29;
  tokenizer.deterministic = true;
  tokenizer.tokenizer_safe = true;
  tokenizer.charset_safe = true;
  tokenizer.unicode_boundary_safe = true;
  return tokenizer;
}

std::vector<NgramSourceRow> NgramRowsForBench() {
  return {{Locator(101), "alpha beta"},
          {Locator(102), "alphabet"},
          {Locator(103), "beta alpha"},
          {Locator(104), "gamma alphabetic"},
          {Locator(105), "alpine beta"}};
}

NgramPhysicalBuildRequest NgramBuildRequestForBench() {
  NgramPhysicalBuildRequest request;
  request.relation_uuid = "44444444-4444-7444-8444-444444444444";
  request.index_uuid = "55555555-5555-7555-8555-555555555555";
  request.provider_uuid = "66666666-6666-7666-8666-666666666666";
  request.base_generation = 7;
  request.provider_generation = 12;
  request.tokenizer = NgramTokenizerForBench();
  request.gram_width = 3;
  request.recheck_proof = NgramProof();
  request.rows = NgramRowsForBench();
  return request;
}

OperationSample NgramSample(const IndexFamilyDescriptor& descriptor,
                            const WorkloadSpec& workload,
                            u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built = BuildNgramPhysicalProvider(NgramBuildRequestForBench());
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = NgramRowsForBench().size();
  sample.pages_or_containers_touched = built.provider.postings.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeNgramPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    NgramPhysicalOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = built.provider.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = built.provider.index_uuid;
    open.expected_provider_uuid_present = true;
    open.expected_provider_uuid = built.provider.provider_uuid;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_tokenizer_epoch_present = true;
    open.expected_tokenizer_epoch = built.provider.tokenizer.tokenizer_epoch;
    open.expected_charset_epoch_present = true;
    open.expected_charset_epoch = built.provider.tokenizer.charset_epoch;
    open.expected_resource_epoch_present = true;
    open.expected_resource_epoch = built.provider.tokenizer.resource_epoch;
    open.recheck_proof = NgramProof();
    const auto opened = OpenNgramPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.postings.size() : 0;
    sample.evidence.push_back("ngram_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    NgramPhysicalMutation mutation;
    mutation.kind = NgramMutationKind::insert_row;
    mutation.after_row_present = true;
    mutation.after_row = {Locator(106), "alphabet soup"};
    mutation.recheck_proof = NgramProof();
    const auto mutated = ApplyNgramPhysicalMutation(built.provider, mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.provider.postings.size();
    sample.evidence.push_back("ngram_mutation_route_consumed=true");
    return sample;
  }
  NgramQueryRequest query;
  query.provider = built.provider;
  query.kind = workload.workload == std::string("range_lookup")
                   ? NgramQueryKind::contains
                   : NgramQueryKind::prefix;
  query.pattern = workload.workload == std::string("range_lookup")
                      ? "pha"
                      : "alp";
  query.recheck_proof = NgramProof();
  const auto result = QueryNgramPhysicalProvider(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.qgram_probe_count;
  sample.evidence.push_back("ngram_query_route_consumed=true");
  return sample;
}

OperationSample DocumentPathSample(const IndexFamilyDescriptor& descriptor,
                                   const WorkloadSpec& workload,
                                   u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  DocumentPathAccessRequest request;
  request.raw_path = workload.workload == std::string("range_lookup")
                         ? "$.items.*.sku"
                         : "$.customer.status";
  request.wildcard_requested = workload.workload == std::string("range_lookup");
  request.wildcard_allowed = true;
  request.exact_source_recheck_available = true;
  request.missing_policy = DocumentValuePolicy::index_presence;
  request.array_policy = DocumentValuePolicy::expand_elements;
  const auto decision = DecideDocumentPathAccess(request);
  sample.ok = decision.ok();
  SetSampleDiagnostic(&sample, decision.diagnostic);
  sample.operation_count = 1;
  sample.rows_examined = 1;
  sample.rows_returned = decision.ok() ? 1 : 0;
  sample.pages_or_containers_touched = decision.ok() ? 1 : 0;
  sample.evidence.push_back("document_path_normalized_access_route_consumed=true");
  sample.evidence.push_back(std::string("document_path_wildcard_scope=") +
                            (decision.wildcard_scope ? "true" : "false"));
  sample.evidence.push_back(std::string("document_path_array_expansion=") +
                            (decision.expand_array_elements ? "true" : "false"));
  return sample;
}

SpatialRTreeMbr SpatialMbr(double min_x,
                           double min_y,
                           double max_x,
                           double max_y,
                           u32 srid = 4326) {
  SpatialRTreeMbr mbr;
  mbr.dimensions = 2;
  mbr.srid = srid;
  mbr.min = {min_x, min_y};
  mbr.max = {max_x, max_y};
  return mbr;
}

SpatialRTreeRecheckProof SpatialProof() {
  SpatialRTreeRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_geometry_available = true;
  proof.exact_predicate_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "spatial_exact_source_mga_security_recheck_contract";
  return proof;
}

SpatialRTreeDescriptor SpatialDescriptorForBench(u64 epoch = 31) {
  SpatialRTreeDescriptor descriptor;
  descriptor.dimensions = 2;
  descriptor.descriptor_epoch = epoch;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.supports_point = true;
  descriptor.supports_mbr = true;
  return descriptor;
}

SpatialRTreeSridResource SpatialSridForBench(u64 epoch = 37) {
  SpatialRTreeSridResource resource;
  resource.resource_uuid = "39393939-3939-7939-8939-393939393939";
  resource.srid = 4326;
  resource.resource_epoch = epoch;
  resource.coordinate_order = "xy";
  resource.deterministic = true;
  resource.safe = true;
  resource.cache_present = true;
  return resource;
}

std::vector<SpatialRTreeSourceRow> SpatialRowsForBench() {
  return {{Locator(10), SpatialMbr(0.0, 0.0, 1.0, 1.0), "src10"},
          {Locator(20), SpatialMbr(2.0, 2.0, 4.0, 4.0), "src20"},
          {Locator(30), SpatialMbr(5.0, 5.0, 6.0, 6.0), "src30"},
          {Locator(40), SpatialMbr(-1.0, -1.0, 0.5, 0.5), "src40"},
          {Locator(50), SpatialMbr(3.0, 0.0, 5.0, 1.0), "src50"},
          {Locator(60), SpatialMbr(8.0, 8.0, 9.0, 9.0), "src60"}};
}

SpatialRTreeBuildRequest SpatialBuildRequestForBench(
    SpatialRTreeBuildMode mode = SpatialRTreeBuildMode::incremental_insert) {
  SpatialRTreeBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = SpatialDescriptorForBench();
  request.srid_resource = SpatialSridForBench();
  request.recheck_proof = SpatialProof();
  request.build_mode = mode;
  request.max_entries = 3;
  request.min_entries = 1;
  request.rows = SpatialRowsForBench();
  return request;
}

SpatialRTreeQueryResult SpatialQueryForBench(
    const SpatialRTreePhysicalProvider& provider,
    SpatialRTreeQueryKind kind,
    SpatialRTreeMbr mbr) {
  SpatialRTreeQueryRequest request;
  request.provider = provider;
  request.kind = kind;
  request.query_mbr = std::move(mbr);
  request.recheck_proof = SpatialProof();
  return QuerySpatialRTreePhysicalProvider(request);
}

OperationSample SpatialRtreeSample(const IndexFamilyDescriptor& descriptor,
                                   const WorkloadSpec& workload,
                                   u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built = BuildSpatialRTreePhysicalProvider(
      SpatialBuildRequestForBench(workload.workload == std::string("bulk_build")
                                      ? SpatialRTreeBuildMode::str_bulk
                                      : SpatialRTreeBuildMode::incremental_insert));
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = SpatialRowsForBench().size();
  sample.pages_or_containers_touched = built.provider.nodes.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeSpatialRTreePhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    SpatialRTreeOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = built.provider.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = built.provider.index_uuid;
    open.expected_provider_uuid_present = true;
    open.expected_provider_uuid = built.provider.provider_uuid;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    open.expected_srid_resource_epoch_present = true;
    open.expected_srid_resource_epoch =
        built.provider.srid_resource.resource_epoch;
    open.recheck_proof = SpatialProof();
    const auto opened = OpenSpatialRTreePhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.rows.size() : 0;
    sample.evidence.push_back("spatial_rtree_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    SpatialRTreeMutation mutation;
    mutation.kind = SpatialRTreeMutationKind::insert_row;
    mutation.expected_provider_generation_present = true;
    mutation.expected_provider_generation = built.provider.provider_generation;
    mutation.expected_descriptor_epoch_present = true;
    mutation.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    mutation.expected_srid_resource_epoch_present = true;
    mutation.expected_srid_resource_epoch =
        built.provider.srid_resource.resource_epoch;
    mutation.after_row_present = true;
    mutation.after_row = {Locator(70), SpatialMbr(7.0, 7.0, 7.5, 7.5), "src70"};
    mutation.recheck_proof = SpatialProof();
    const auto mutated = ApplySpatialRTreePhysicalMutation(built.provider,
                                                           mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.provider.rows.size();
    sample.evidence.push_back("spatial_rtree_mutation_route_consumed=true");
    return sample;
  }
  const auto result = SpatialQueryForBench(
      built.provider,
      workload.workload == std::string("point_lookup")
          ? SpatialRTreeQueryKind::point
          : SpatialRTreeQueryKind::intersects,
      workload.workload == std::string("point_lookup")
          ? SpatialMbr(0.25, 0.25, 0.25, 0.25)
          : SpatialMbr(3.5, 0.5, 5.5, 5.5));
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.nodes_visited;
  sample.evidence.push_back("spatial_rtree_query_route_consumed=true");
  return sample;
}

GistExactRecheckProof GistProof() {
  GistExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_geometry_available = true;
  proof.exact_predicate_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "gist_exact_source_mga_security_recheck_contract";
  return proof;
}

std::vector<GistSourceRow> GistRowsForBench() {
  std::vector<GistSourceRow> rows;
  for (const auto& row : SpatialRowsForBench()) {
    rows.push_back({row.locator, row.mbr, row.exact_source_recheck_evidence_ref});
  }
  return rows;
}

GistOpclassRuntime GistOpclassForBench() {
  return MakeSpatialMbrGistOpclass(47, 53, 2, 4326);
}

GistBuildRequest GistBuildRequestForBench(
    SpatialRTreeBuildMode mode = SpatialRTreeBuildMode::incremental_insert) {
  GistBuildRequest request;
  request.relation_uuid = "71717171-7171-7171-8171-717171717171";
  request.index_uuid = "72727272-7272-7272-8272-727272727272";
  request.provider_uuid = "73737373-7373-7373-8373-737373737373";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.spatial_descriptor = SpatialDescriptorForBench(41);
  request.srid_resource = SpatialSridForBench(43);
  request.opclass = GistOpclassForBench();
  request.recheck_proof = GistProof();
  request.build_mode = mode;
  request.max_entries = 3;
  request.min_entries = 1;
  request.rows = GistRowsForBench();
  return request;
}

OperationSample GistSample(const IndexFamilyDescriptor& descriptor,
                           const WorkloadSpec& workload,
                           u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built = BuildGistPhysicalProvider(
      GistBuildRequestForBench(workload.workload == std::string("bulk_build")
                                   ? SpatialRTreeBuildMode::str_bulk
                                   : SpatialRTreeBuildMode::incremental_insert));
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = GistRowsForBench().size();
  sample.pages_or_containers_touched = built.provider.physical_tree.nodes.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeGistPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    GistOpenRequest open;
    open.bytes = serialized.bytes;
    open.opclass = GistOpclassForBench();
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_opclass_epoch_present = true;
    open.expected_opclass_epoch = built.provider.opclass.opclass_epoch;
    open.expected_resource_epoch_present = true;
    open.expected_resource_epoch = built.provider.opclass.resource_epoch;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch =
        built.provider.physical_tree.descriptor.descriptor_epoch;
    open.expected_srid_resource_epoch_present = true;
    open.expected_srid_resource_epoch =
        built.provider.physical_tree.srid_resource.resource_epoch;
    open.recheck_proof = GistProof();
    const auto opened = OpenGistPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.physical_tree.rows.size() : 0;
    sample.evidence.push_back("gist_crash_reopen_route_consumed=true");
    return sample;
  }
  GistQueryRequest query;
  query.provider = built.provider;
  query.opclass = GistOpclassForBench();
  query.strategy = workload.workload == std::string("point_lookup")
                       ? GistPredicateStrategy::point
                       : GistPredicateStrategy::intersects;
  query.query_mbr = workload.workload == std::string("point_lookup")
                        ? SpatialMbr(0.25, 0.25, 0.25, 0.25)
                        : SpatialMbr(3.5, 0.5, 5.5, 5.5);
  query.recheck_proof = GistProof();
  const auto result = QueryGistPhysicalProvider(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.nodes_visited;
  sample.evidence.push_back("gist_opclass_spatial_route_consumed=true");
  return sample;
}

SpGistExactRecheckProof SpGistProof() {
  SpGistExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_geometry_available = true;
  proof.exact_predicate_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "spgist_exact_source_mga_security_recheck_contract";
  return proof;
}

std::vector<SpGistSourceRow> SpGistRowsForBench() {
  return {{Locator(10), SpatialMbr(0.0, 0.0, 1.0, 1.0), "src10"},
          {Locator(20), SpatialMbr(2.0, 2.0, 4.0, 4.0), "src20"},
          {Locator(30), SpatialMbr(5.0, 5.0, 6.0, 6.0), "src30"},
          {Locator(40), SpatialMbr(-1.0, -1.0, 0.5, 0.5), "src40"},
          {Locator(50), SpatialMbr(3.0, 0.0, 5.0, 1.0), "src50"},
          {Locator(60), SpatialMbr(8.0, 8.0, 9.0, 9.0), "src60"},
          {Locator(70), SpatialMbr(-6.0, 7.0, -5.0, 8.0), "src70"},
          {Locator(80), SpatialMbr(7.0, -6.0, 8.0, -5.0), "src80"}};
}

SpGistOpclassRuntime SpGistOpclassForBench() {
  return MakeSpatialQuadMbrSpGistOpclass(71, 73, 4326);
}

SpGistBuildRequest SpGistBuildRequestForBench() {
  SpGistBuildRequest request;
  request.relation_uuid = "b1b1b1b1-b1b1-71b1-81b1-b1b1b1b1b1b1";
  request.index_uuid = "b2b2b2b2-b2b2-72b2-82b2-b2b2b2b2b2b2";
  request.provider_uuid = "b3b3b3b3-b3b3-73b3-83b3-b3b3b3b3b3b3";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.spatial_descriptor = SpatialDescriptorForBench(61);
  request.srid_resource = SpatialSridForBench(67);
  request.opclass = SpGistOpclassForBench();
  request.recheck_proof = SpGistProof();
  request.leaf_capacity = 2;
  request.max_depth = 12;
  request.rows = SpGistRowsForBench();
  return request;
}

OperationSample SpGistSample(const IndexFamilyDescriptor& descriptor,
                             const WorkloadSpec& workload,
                             u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  const auto built = BuildSpGistPhysicalProvider(SpGistBuildRequestForBench());
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = SpGistRowsForBench().size();
  sample.pages_or_containers_touched = built.provider.nodes.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeSpGistPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    SpGistOpenRequest open;
    open.bytes = serialized.bytes;
    open.opclass = SpGistOpclassForBench();
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_opclass_epoch_present = true;
    open.expected_opclass_epoch = built.provider.opclass.opclass_epoch;
    open.expected_resource_epoch_present = true;
    open.expected_resource_epoch = built.provider.opclass.resource_epoch;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch = built.provider.spatial_descriptor.descriptor_epoch;
    open.expected_srid_resource_epoch_present = true;
    open.expected_srid_resource_epoch =
        built.provider.srid_resource.resource_epoch;
    open.recheck_proof = SpGistProof();
    const auto opened = OpenSpGistPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.rows.size() : 0;
    sample.evidence.push_back("spgist_crash_reopen_route_consumed=true");
    return sample;
  }
  SpGistQueryRequest query;
  query.provider = built.provider;
  query.opclass = SpGistOpclassForBench();
  query.strategy = workload.workload == std::string("point_lookup")
                       ? SpGistPredicateStrategy::point
                       : SpGistPredicateStrategy::intersects;
  query.query_mbr = workload.workload == std::string("point_lookup")
                        ? SpatialMbr(0.25, 0.25, 0.25, 0.25)
                        : SpatialMbr(3.5, 0.5, 5.5, 5.5);
  query.recheck_proof = SpGistProof();
  const auto result = QuerySpGistPhysicalProvider(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.nodes_visited;
  sample.evidence.push_back("spgist_partitioned_tree_route_consumed=true");
  return sample;
}

VectorExactRecheckProof VectorProof() {
  VectorExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_vector_available = true;
  proof.exact_rerank_proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "vector_source_mga_security_rerank_contract";
  return proof;
}

VectorExactDescriptor VectorDescriptorForBench(
    VectorExactElementProfile profile = VectorExactElementProfile::fp32,
    u64 epoch = 31) {
  VectorExactDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = profile;
  descriptor.descriptor_epoch = epoch;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.int8_scale = 0.25;
  descriptor.int8_zero_point = 0;
  return descriptor;
}

VectorExactMetricResource VectorMetricForBench(
    VectorExactMetricKind kind = VectorExactMetricKind::l2,
    u64 epoch = 37) {
  VectorExactMetricResource metric;
  metric.metric_resource_uuid = "99999999-9999-7999-8999-999999999999";
  metric.metric_resource_epoch = epoch;
  metric.metric_kind = kind;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

std::vector<VectorExactSourceRow> VectorRowsForBench() {
  return {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(40), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(50), {-1.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(70), {3.0F, 3.0F, 3.0F, 3.0F}},
          {Locator(80), {0.0F, 2.0F, 0.0F, 0.0F}}};
}

OperationSample VectorExactSample(const IndexFamilyDescriptor& descriptor,
                                  const WorkloadSpec& workload,
                                  u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  VectorExactBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = VectorDescriptorForBench();
  request.metric = VectorMetricForBench();
  request.recheck_proof = VectorProof();
  request.rows = VectorRowsForBench();
  const auto built = BuildVectorExactPhysicalProvider(request);
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = request.rows.size();
  sample.pages_or_containers_touched = built.provider.rows.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeVectorExactPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    VectorExactOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    open.expected_metric_resource_epoch_present = true;
    open.expected_metric_resource_epoch =
        built.provider.metric.metric_resource_epoch;
    open.expected_dimensions_present = true;
    open.expected_dimensions = built.provider.descriptor.dimensions;
    open.recheck_proof = VectorProof();
    const auto opened = OpenVectorExactPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.rows.size() : 0;
    sample.evidence.push_back("vector_exact_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    VectorExactMutation mutation;
    mutation.kind = VectorExactMutationKind::insert_row;
    mutation.expected_provider_generation_present = true;
    mutation.expected_provider_generation = built.provider.provider_generation;
    mutation.expected_descriptor_epoch_present = true;
    mutation.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    mutation.expected_metric_resource_epoch_present = true;
    mutation.expected_metric_resource_epoch =
        built.provider.metric.metric_resource_epoch;
    mutation.after_row_present = true;
    mutation.after_row = {Locator(90), {1.0F, 1.0F, 1.0F, 1.0F}};
    mutation.recheck_proof = VectorProof();
    const auto mutated = ApplyVectorExactPhysicalMutation(built.provider,
                                                          mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.provider.rows.size();
    sample.evidence.push_back("vector_exact_mutation_route_consumed=true");
    return sample;
  }
  VectorExactQueryRequest query;
  query.provider = built.provider;
  query.recheck_proof = VectorProof();
  VectorExactQuery first;
  first.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  first.top_k = 3;
  VectorExactQuery second;
  second.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  second.top_k = 2;
  second.candidate_set = {Locator(10), Locator(30), Locator(50)};
  second.metadata_prefilter = [](const TextInvertedRowLocator& locator) {
    return locator.row_ordinal != 50;
  };
  query.queries = {first, second};
  const auto result = QueryVectorExactPhysicalProvider(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.batch_results.empty()
                             ? 0
                             : result.batch_results.front().candidates.size();
  sample.pages_or_containers_touched =
      result.batch_results.empty()
          ? 0
          : result.batch_results.front().decoded_vector_count;
  sample.evidence.push_back("vector_exact_batched_query_route_consumed=true");
  return sample;
}

OperationSample VectorHnswSample(const IndexFamilyDescriptor& descriptor,
                                 const WorkloadSpec& workload,
                                 u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  VectorHnswBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = VectorDescriptorForBench(VectorExactElementProfile::fp32, 41);
  request.metric = VectorMetricForBench(VectorExactMetricKind::l2, 43);
  request.profile.m = 4;
  request.profile.ef_construction = 16;
  request.profile.ef_search = 16;
  request.profile.max_level = 5;
  request.profile.compaction_tombstone_ratio = 0.10;
  request.recheck_proof = VectorProof();
  request.rows = VectorRowsForBench();
  const auto built = BuildVectorHnswPhysicalProvider(request);
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = request.rows.size();
  sample.pages_or_containers_touched = built.provider.nodes.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeVectorHnswPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    VectorHnswOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_training_generation_present = true;
    open.expected_training_generation = built.provider.training_generation;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    open.expected_metric_resource_epoch_present = true;
    open.expected_metric_resource_epoch =
        built.provider.metric.metric_resource_epoch;
    open.expected_dimensions_present = true;
    open.expected_dimensions = built.provider.descriptor.dimensions;
    open.recheck_proof = VectorProof();
    const auto opened = OpenVectorHnswPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.live_node_count : 0;
    sample.evidence.push_back("vector_hnsw_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    VectorHnswMutation mutation;
    mutation.kind = VectorHnswMutationKind::insert_row;
    mutation.expected_provider_generation_present = true;
    mutation.expected_provider_generation = built.provider.provider_generation;
    mutation.expected_descriptor_epoch_present = true;
    mutation.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    mutation.expected_metric_resource_epoch_present = true;
    mutation.expected_metric_resource_epoch =
        built.provider.metric.metric_resource_epoch;
    mutation.after_row_present = true;
    mutation.after_row = {Locator(90), {1.0F, 1.0F, 1.0F, 1.0F}};
    mutation.recheck_proof = VectorProof();
    const auto mutated = ApplyVectorHnswPhysicalMutation(built.provider,
                                                         mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.provider.live_node_count;
    sample.evidence.push_back("vector_hnsw_mutation_route_consumed=true");
    return sample;
  }
  VectorHnswQueryRequest query;
  query.provider = built.provider;
  query.recheck_proof = VectorProof();
  VectorHnswQuery first;
  first.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  first.top_k = 3;
  first.ef_search = 32;
  VectorHnswQuery second;
  second.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  second.top_k = 2;
  second.ef_search = 32;
  second.candidate_set = {Locator(10), Locator(30), Locator(50)};
  second.metadata_prefilter = [](const TextInvertedRowLocator& locator) {
    return locator.row_ordinal != 50;
  };
  query.queries = {first, second};
  const auto result = QueryVectorHnswPhysicalProvider(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.batch_results.empty()
                             ? 0
                             : result.batch_results.front().candidates.size();
  sample.pages_or_containers_touched =
      result.batch_results.empty()
          ? 0
          : result.batch_results.front().graph_nodes_visited;
  sample.evidence.push_back("vector_hnsw_ef_search_rerank_route_consumed=true");
  return sample;
}

OperationSample VectorIvfSample(const IndexFamilyDescriptor& descriptor,
                                const WorkloadSpec& workload,
                                u64 salt) {
  if (WorkloadIsValidation(workload.workload)) {
    return ValidationSample(descriptor, false, salt);
  }
  if (WorkloadIsRepair(workload.workload)) {
    return ValidationSample(descriptor, true, salt);
  }
  OperationSample sample;
  VectorIvfPqBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = VectorDescriptorForBench(VectorExactElementProfile::fp32, 51);
  request.metric = VectorMetricForBench(VectorExactMetricKind::l2, 53);
  request.profile.compression = workload.workload == std::string("range_lookup")
                                    ? VectorIvfPqCompression::pq
                                    : VectorIvfPqCompression::ivf_flat;
  request.profile.centroid_count = 3;
  request.profile.nprobe = 3;
  request.profile.training_iterations = 5;
  request.profile.max_training_rows = 32;
  request.profile.pq_subspaces = 2;
  request.profile.pq_codewords = 4;
  request.recheck_proof = VectorProof();
  request.rows = VectorRowsForBench();
  const auto built = BuildVectorIvfPqPhysicalProvider(request);
  sample.ok = built.ok();
  SetSampleDiagnostic(&sample, built.diagnostic);
  sample.rows_examined = request.rows.size();
  sample.pages_or_containers_touched = built.provider.lists.size();
  if (!sample.ok) return sample;
  if (WorkloadIsReopen(workload.workload)) {
    const auto serialized = SerializeVectorIvfPqPhysicalProvider(built.provider);
    sample.ok = serialized.ok();
    SetSampleDiagnostic(&sample, serialized.diagnostic);
    if (!sample.ok) return sample;
    VectorIvfPqOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_training_generation_present = true;
    open.expected_training_generation = built.provider.training_generation;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    open.expected_metric_resource_epoch_present = true;
    open.expected_metric_resource_epoch =
        built.provider.metric.metric_resource_epoch;
    open.expected_dimensions_present = true;
    open.expected_dimensions = built.provider.descriptor.dimensions;
    open.recheck_proof = VectorProof();
    const auto opened = OpenVectorIvfPqPhysicalProvider(open);
    sample.ok = opened.ok();
    SetSampleDiagnostic(&sample, opened.diagnostic);
    sample.rows_returned = opened.ok() ? opened.provider.live_vector_count : 0;
    sample.evidence.push_back("vector_ivf_crash_reopen_route_consumed=true");
    return sample;
  }
  if (WorkloadIsMaintenance(workload.workload)) {
    VectorIvfPqMutation mutation;
    mutation.kind = VectorIvfPqMutationKind::insert_row;
    mutation.expected_provider_generation_present = true;
    mutation.expected_provider_generation = built.provider.provider_generation;
    mutation.expected_training_generation_present = true;
    mutation.expected_training_generation = built.provider.training_generation;
    mutation.expected_descriptor_epoch_present = true;
    mutation.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    mutation.expected_metric_resource_epoch_present = true;
    mutation.expected_metric_resource_epoch =
        built.provider.metric.metric_resource_epoch;
    mutation.after_row_present = true;
    mutation.after_row = {Locator(90), {1.0F, 1.0F, 1.0F, 1.0F}};
    mutation.recheck_proof = VectorProof();
    const auto mutated = ApplyVectorIvfPqPhysicalMutation(built.provider,
                                                          mutation);
    sample.ok = mutated.ok();
    SetSampleDiagnostic(&sample, mutated.diagnostic);
    sample.rows_returned = mutated.provider.live_vector_count;
    sample.evidence.push_back("vector_ivf_mutation_route_consumed=true");
    return sample;
  }
  VectorIvfPqQueryRequest query;
  query.provider = built.provider;
  query.recheck_proof = VectorProof();
  VectorIvfPqQuery first;
  first.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  first.top_k = 3;
  first.nprobe = 3;
  VectorIvfPqQuery second;
  second.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  second.top_k = 2;
  second.nprobe = 3;
  second.candidate_set = {Locator(10), Locator(30), Locator(50)};
  second.metadata_prefilter = [](const TextInvertedRowLocator& locator) {
    return locator.row_ordinal != 50;
  };
  query.queries = {first, second};
  const auto result = QueryVectorIvfPqPhysicalProvider(query);
  sample.ok = result.ok();
  SetSampleDiagnostic(&sample, result.diagnostic);
  sample.rows_returned = result.batch_results.empty()
                             ? 0
                             : result.batch_results.front().candidates.size();
  sample.pages_or_containers_touched =
      result.batch_results.empty()
          ? 0
          : result.batch_results.front().selected_list_ids.size();
  sample.evidence.push_back("vector_ivf_nprobe_rerank_route_consumed=true");
  return sample;
}

GraphPropertyValue GraphProp(std::string key,
                             std::string type,
                             std::string value) {
  return {std::move(key), std::move(type), std::move(value)};
}

GraphRecheckProof GraphProof() {
  GraphRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_recheck_required = true;
  proof.exact_source_available = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "graph_exact_source_mga_security_recheck";
  return proof;
}

GraphDescriptor GraphDescriptorForBench() {
  GraphDescriptor descriptor;
  descriptor.descriptor_epoch = 182;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

GraphVertexInput GraphVertex(std::string id,
                             u64 row,
                             std::vector<std::string> labels,
                             std::vector<GraphPropertyValue> props) {
  return {std::move(id),
          Locator(row),
          std::move(labels),
          std::move(props),
          "graph_vertex_exact_source_" + std::to_string(row)};
}

GraphEdgeInput GraphEdge(std::string id,
                         std::string source,
                         std::string target,
                         std::string label,
                         u64 row) {
  return {std::move(id),
          std::move(source),
          std::move(target),
          std::move(label),
          Locator(row),
          {GraphProp("weight", "int64", std::to_string(row))},
          "graph_edge_exact_source_" + std::to_string(row)};
}

GraphBuildRequest GraphBuildRequestForBench() {
  GraphBuildRequest request;
  request.relation_uuid =
      GeneratedUuidText(platform::UuidKind::object, 301);
  request.index_uuid =
      GeneratedUuidText(platform::UuidKind::object, 302);
  request.provider_uuid =
      GeneratedUuidText(platform::UuidKind::object, 303);
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = GraphDescriptorForBench();
  request.recheck_proof = GraphProof();
  request.vertices = {
      GraphVertex("A", 10, {"Person"}, {GraphProp("color", "string", "blue")}),
      GraphVertex("B", 20, {"Person"}, {GraphProp("color", "string", "red")}),
      GraphVertex("C", 30, {"Company"}, {GraphProp("tier", "int64", "2")})};
  request.edges = {GraphEdge("e1", "A", "B", "KNOWS", 100),
                   GraphEdge("e2", "B", "C", "LIKES", 110),
                   GraphEdge("e3", "A", "C", "KNOWS", 120)};
  return request;
}

GraphBuildResult BuildGraphForBench() {
  return BuildGraphAdjacencyPhysicalProvider(GraphBuildRequestForBench());
}

OperationSample GraphSample(const IndexFamilyDescriptor& descriptor,
                            const WorkloadSpec& workload,
                            u64 salt) {
  if (workload.workload == std::string("validate")) {
    return ValidationSample(descriptor, false, salt);
  }
  if (workload.workload == std::string("repair_rebuild")) {
    return ValidationSample(descriptor, true, salt);
  }

  const auto built = BuildGraphForBench();
  OperationSample sample;
  sample.diagnostic_code = built.diagnostic.diagnostic_code;
  sample.message_key = built.diagnostic.message_key;
  sample.operation_count = 1;
  sample.rows_examined = 3;
  sample.pages_or_containers_touched = 1;
  if (!built.ok()) return sample;

  if (workload.workload == std::string("bulk_build") ||
      workload.workload == std::string("cold_cache")) {
    sample.ok = true;
    sample.rows_returned = built.provider.vertex_id_index.size();
    sample.pages_or_containers_touched = built.provider.vertex_id_index.size() +
                                         built.provider.edge_source_adjacency.size();
    sample.evidence.push_back("graph_bulk_provider_built=true");
    return sample;
  }

  if (workload.workload == std::string("crash_reopen")) {
    const auto serialized =
        SerializeGraphAdjacencyPhysicalProvider(built.provider);
    GraphOpenRequest open;
    open.bytes = serialized.bytes;
    open.expected_relation_uuid_present = true;
    open.expected_relation_uuid = built.provider.relation_uuid;
    open.expected_index_uuid_present = true;
    open.expected_index_uuid = built.provider.index_uuid;
    open.expected_provider_uuid_present = true;
    open.expected_provider_uuid = built.provider.provider_uuid;
    open.expected_provider_generation_present = true;
    open.expected_provider_generation = built.provider.provider_generation;
    open.expected_descriptor_epoch_present = true;
    open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    open.recheck_proof = GraphProof();
    const auto opened = OpenGraphAdjacencyPhysicalProvider(open);
    sample.ok = opened.ok();
    sample.diagnostic_code = opened.diagnostic.diagnostic_code;
    sample.message_key = opened.diagnostic.message_key;
    sample.rows_returned = opened.ok() ? opened.provider.vertices.size() : 0;
    sample.pages_or_containers_touched = serialized.bytes.empty() ? 0 : 1;
    sample.evidence.push_back("graph_crash_reopen_route_consumed=true");
    return sample;
  }

  if (workload.workload == std::string("update_delete_maintenance")) {
    GraphMutation mutation;
    mutation.kind = GraphMutationKind::delete_vertex;
    mutation.expected_provider_generation_present = true;
    mutation.expected_provider_generation = built.provider.provider_generation;
    mutation.expected_descriptor_epoch_present = true;
    mutation.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
    mutation.before_vertex_present = true;
    mutation.before_vertex = built.provider.vertices.front().row;
    mutation.recheck_proof = GraphProof();
    const auto mutated =
        ApplyGraphAdjacencyPhysicalMutation(built.provider, mutation);
    sample.ok = mutated.ok() && mutated.tombstone_written;
    sample.diagnostic_code = mutated.diagnostic.diagnostic_code;
    sample.message_key = mutated.diagnostic.message_key;
    sample.operation_count = 2;
    sample.rows_examined = built.provider.vertices.size();
    sample.rows_returned = sample.ok ? mutated.provider.vertices.size() : 0;
    sample.pages_or_containers_touched = mutated.provider.edge_source_adjacency.size();
    sample.evidence.push_back("graph_update_delete_maintenance_consumed=true");
    return sample;
  }

  if (workload.workload == std::string("range_lookup") ||
      workload.workload == std::string("warm_cache") ||
      workload.workload == std::string("fallback_disabled")) {
    GraphAdjacencyLookupRequest request;
    request.provider = built.provider;
    request.recheck_proof = GraphProof();
    request.vertex_id = "A";
    request.direction = GraphAdjacencyDirection::outgoing;
    const auto result = QueryGraphAdjacencyIndex(request);
    sample.ok = result.ok();
    sample.diagnostic_code = result.diagnostic.diagnostic_code;
    sample.message_key = result.diagnostic.message_key;
    sample.rows_examined = result.index_entries_examined;
    sample.rows_returned = result.candidates.size();
    sample.pages_or_containers_touched = result.index_entries_examined;
    sample.evidence.push_back("graph_adjacency_route_consumed=true");
    return sample;
  }

  GraphVertexLookupRequest request;
  request.provider = built.provider;
  request.recheck_proof = GraphProof();
  request.vertex_id = "A";
  const auto result = QueryGraphVertexIdIndex(request);
  sample.ok = result.ok();
  sample.diagnostic_code = result.diagnostic.diagnostic_code;
  sample.message_key = result.diagnostic.message_key;
  sample.rows_examined = result.index_entries_examined;
  sample.rows_returned = result.candidates.size();
  sample.pages_or_containers_touched = result.index_entries_examined;
  sample.evidence.push_back("graph_vertex_route_consumed=true");
  return sample;
}

TemporaryWorkAuthorityProof TemporaryProof() {
  TemporaryWorkAuthorityProof proof;
  proof.proof_supplied = true;
  proof.exact_recheck_required = true;
  proof.exact_recheck_available = true;
  proof.mga_visibility_recheck_required = true;
  proof.mga_visibility_recheck_available = true;
  proof.security_recheck_required = true;
  proof.security_context_bound = true;
  proof.evidence_ref = "temporary_exact_mga_security_recheck";
  return proof;
}

TemporaryWorkRuntimeState TemporaryRuntime(u64 salt) {
  TemporaryWorkRuntimeOptions options;
  options.runtime_generation = 1000 + salt;
  options.memory_quota_bytes = 1000000;
  options.artifact_prefix = "index_family_benchmark";
  return CreateTemporaryWorkRuntime(std::move(options));
}

std::vector<TemporaryWorkRecord> TemporaryRows() {
  return {{"k3", "payload-3", 3},
          {"k1", "payload-1", 1},
          {"k2", "payload-2", 2},
          {"k4", "payload-4", 4}};
}

OperationSample TemporarySample(const IndexFamilyDescriptor& descriptor,
                                const WorkloadSpec& workload,
                                u64 salt) {
  if (workload.workload == std::string("validate")) {
    return ValidationSample(descriptor, false, salt);
  }
  if (workload.workload == std::string("repair_rebuild")) {
    return ValidationSample(descriptor, true, salt);
  }

  auto runtime = TemporaryRuntime(salt);
  const auto built =
      BuildTemporarySortRun(&runtime, TemporaryRows(), TemporaryProof(), true);
  OperationSample sample;
  sample.diagnostic_code = built.diagnostic.diagnostic_code;
  sample.message_key = built.diagnostic.message_key;
  sample.operation_count = 1;
  sample.rows_examined = TemporaryRows().size();
  sample.rows_returned = built.sorted_rows.size();
  sample.pages_or_containers_touched = 1;
  if (!built.ok()) return sample;

  if (workload.workload == std::string("update_delete_maintenance")) {
    const auto cleaned =
        CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
    sample.ok = cleaned.ok();
    sample.diagnostic_code = cleaned.diagnostic.diagnostic_code;
    sample.message_key = cleaned.diagnostic.message_key;
    sample.operation_count = 2;
    sample.rows_materialized = 0;
    sample.evidence.push_back("temporary_cleanup_route_consumed=true");
    return sample;
  }

  if (workload.workload == std::string("crash_reopen") ||
      workload.workload == std::string("point_lookup") ||
      workload.workload == std::string("range_lookup") ||
      workload.workload == std::string("warm_cache") ||
      workload.workload == std::string("fallback_disabled")) {
    const auto opened =
        OpenTemporaryWorkArtifact(&runtime, built.descriptor,
                                  TemporaryWorkFamily::sort_run,
                                  TemporaryProof());
    sample.ok = opened.ok();
    sample.diagnostic_code = opened.diagnostic.diagnostic_code;
    sample.message_key = opened.diagnostic.message_key;
    sample.rows_examined = opened.sorted_rows.size();
    sample.rows_returned = opened.sorted_rows.size();
    sample.pages_or_containers_touched = opened.descriptor.artifact.empty() ? 0 : 1;
    sample.evidence.push_back("temporary_open_route_consumed=true");
    return sample;
  }

  sample.ok = true;
  sample.evidence.push_back("temporary_bulk_build_route_consumed=true");
  return sample;
}

InMemoryIndexAuthorityProof InMemoryProof(u64 epoch = 182) {
  InMemoryIndexAuthorityProof proof;
  proof.proof_supplied = true;
  proof.exact_source_recheck_required = true;
  proof.exact_source_available = true;
  proof.mga_visibility_recheck_required = true;
  proof.mga_visibility_recheck_available = true;
  proof.security_recheck_required = true;
  proof.security_context_bound = true;
  proof.snapshot_proof_supplied = true;
  proof.snapshot_still_valid = true;
  proof.runtime_epoch = epoch;
  proof.mga_snapshot_epoch = 10;
  proof.catalog_snapshot_epoch = 20;
  proof.security_snapshot_epoch = 30;
  proof.evidence_token = "in_memory_exact_mga_security_snapshot_recheck";
  return proof;
}

InMemoryIndexRuntimeState InMemoryRuntime(u64 epoch = 182) {
  InMemoryIndexRuntimeOptions options;
  options.runtime_epoch = epoch;
  options.memory_quota_bytes = 1000000;
  options.relation_uuid = "rel-index-family-benchmark";
  options.index_uuid = "idx-index-family-benchmark";
  return CreateInMemoryIndexRuntime(std::move(options));
}

InMemoryIndexColdSourceDescriptor InMemoryColdSource() {
  InMemoryIndexColdSourceDescriptor descriptor;
  descriptor.relation_uuid = "rel-index-family-benchmark";
  descriptor.index_uuid = "idx-index-family-benchmark";
  descriptor.descriptor_epoch = 401;
  descriptor.persisted_generation = 902;
  descriptor.cold_source_supplied = true;
  descriptor.deterministic_order = true;
  descriptor.candidate_entries_only = true;
  descriptor.exact_recheck_required = true;
  descriptor.mga_recheck_required = true;
  descriptor.security_recheck_required = true;
  descriptor.entries = {{"alpha:001", "payload-a1", 1, "source-a1"},
                        {"alpha:002", "payload-a2", 2, "source-a2"},
                        {"beta:001", "payload-b1", 3, "source-b1"},
                        {"beta:002", "payload-b2", 4, "source-b2"}};
  return descriptor;
}

OperationSample InMemorySample(const IndexFamilyDescriptor& descriptor,
                               const WorkloadSpec& workload,
                               u64 salt) {
  if (workload.workload == std::string("validate")) {
    return ValidationSample(descriptor, false, salt);
  }
  if (workload.workload == std::string("repair_rebuild")) {
    return ValidationSample(descriptor, true, salt);
  }

  auto runtime = InMemoryRuntime();
  const auto rebuilt = RebuildInMemoryIndexFromColdSource(
      &runtime, InMemoryColdSource(), InMemoryProof());
  OperationSample sample;
  sample.diagnostic_code = rebuilt.diagnostic.diagnostic_code;
  sample.message_key = rebuilt.diagnostic.message_key;
  sample.operation_count = 1;
  sample.rows_examined = InMemoryColdSource().entries.size();
  sample.rows_returned = rebuilt.generation ? rebuilt.generation->entries_by_key.size() : 0;
  sample.pages_or_containers_touched =
      rebuilt.generation ? rebuilt.generation->entries_by_key.size() : 0;
  if (!rebuilt.ok()) return sample;

  if (workload.workload == std::string("bulk_build") ||
      workload.workload == std::string("cold_cache")) {
    sample.ok = true;
    sample.evidence.push_back("in_memory_cold_rebuild_route_consumed=true");
    return sample;
  }

  if (workload.workload == std::string("update_delete_maintenance")) {
    InMemoryIndexMutation insert;
    insert.kind = InMemoryIndexMutationKind::insert_entry;
    insert.entry = {"delta:001", "payload-d1", 5, "source-d1"};
    insert.runtime_epoch = 182;
    insert.proof = InMemoryProof();
    const auto inserted = ApplyInMemoryIndexMutation(&runtime, insert);
    InMemoryIndexMutation erase = insert;
    erase.kind = InMemoryIndexMutationKind::delete_entry;
    const auto deleted = ApplyInMemoryIndexMutation(&runtime, erase);
    sample.ok = inserted.ok() && deleted.ok();
    sample.diagnostic_code = deleted.diagnostic.diagnostic_code;
    sample.message_key = deleted.diagnostic.message_key;
    sample.operation_count = 3;
    sample.rows_examined = 2;
    sample.rows_returned = deleted.generation ? deleted.generation->entries_by_key.size() : 0;
    sample.pages_or_containers_touched = sample.rows_returned;
    sample.evidence.push_back("in_memory_mutation_route_consumed=true");
    return sample;
  }

  InMemoryIndexLookupRequest lookup;
  lookup.runtime_epoch = 182;
  lookup.proof = InMemoryProof();
  if (workload.workload == std::string("range_lookup") ||
      workload.workload == std::string("warm_cache") ||
      workload.workload == std::string("fallback_disabled")) {
    lookup.mode = InMemoryIndexLookupMode::range;
    lookup.lower_key = "alpha:001";
    lookup.upper_key = "beta:002";
  } else {
    lookup.mode = InMemoryIndexLookupMode::point;
    lookup.key = "beta:001";
  }
  const auto found = LookupInMemoryIndex(&runtime, lookup);
  sample.ok = found.ok();
  sample.diagnostic_code = found.diagnostic.diagnostic_code;
  sample.message_key = found.diagnostic.message_key;
  sample.rows_examined = rebuilt.generation->entries_by_key.size();
  sample.rows_returned = found.candidates.size();
  sample.pages_or_containers_touched = found.candidates.size();
  sample.evidence.push_back("in_memory_lookup_route_consumed=true");
  return sample;
}

OperationSample SampleForFamily(const IndexFamilyDescriptor& descriptor,
                                const WorkloadSpec& workload,
                                u64 salt) {
  switch (descriptor.family) {
    case IndexFamily::btree:
    case IndexFamily::unique_btree:
    case IndexFamily::expression:
    case IndexFamily::partial:
    case IndexFamily::covering:
      return BtreeSample(descriptor, workload, salt);
    case IndexFamily::hash:
      return HashSample(descriptor, workload, salt);
    case IndexFamily::bitmap:
      return BitmapSample(descriptor, workload, salt);
    case IndexFamily::brin_zone:
      return ZoneSummarySample(descriptor, workload, salt);
    case IndexFamily::bloom:
      return BloomSample(descriptor, workload, salt);
    case IndexFamily::full_text:
    case IndexFamily::inverted:
    case IndexFamily::sparse_wand:
      return TextRuntimeSample(descriptor, workload, salt);
    case IndexFamily::gin:
      return GinSample(descriptor, workload, salt);
    case IndexFamily::ngram:
      return NgramSample(descriptor, workload, salt);
    case IndexFamily::spatial:
    case IndexFamily::rtree:
      return SpatialRtreeSample(descriptor, workload, salt);
    case IndexFamily::gist:
      return GistSample(descriptor, workload, salt);
    case IndexFamily::spgist:
      return SpGistSample(descriptor, workload, salt);
    case IndexFamily::vector_exact:
      return VectorExactSample(descriptor, workload, salt);
    case IndexFamily::vector_hnsw:
      return VectorHnswSample(descriptor, workload, salt);
    case IndexFamily::vector_ivf:
      return VectorIvfSample(descriptor, workload, salt);
    case IndexFamily::columnar_zone:
      return ColumnarZoneSample(descriptor, workload, salt);
    case IndexFamily::document_path:
      return DocumentPathSample(descriptor, workload, salt);
    case IndexFamily::graph:
      return GraphSample(descriptor, workload, salt);
    case IndexFamily::temporary_work:
      return TemporarySample(descriptor, workload, salt);
    case IndexFamily::in_memory:
      return InMemorySample(descriptor, workload, salt);
    default:
      return ValidationSample(descriptor, false, salt);
  }
}

IndexFamilyBenchmarkEvidenceRow CompleteRow(
    const IndexFamilyDescriptor& descriptor,
    const IndexFamilyPhysicalCapabilityState& state,
    const WorkloadSpec& workload,
    u32 sample_iterations,
    u64 salt) {
  auto row = BaseRow(descriptor, state, workload);
  row.concrete_runtime_consumed = true;
  row.fail_closed = false;
  row.diagnostic_code = "INDEX.BENCHMARK.OK";
  row.message_key = "index.benchmark.ok";
  ApplyRouteProof(&row, descriptor.family);
  if (row.fail_closed) {
    return row;
  }

  std::vector<u64> latencies;
  latencies.reserve(sample_iterations);
  for (u32 i = 0; i < sample_iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto sample = SampleForFamily(descriptor, workload, salt + i);
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();
    latencies.push_back(
        static_cast<u64>(std::max<long long>(1, elapsed)));
    if (!sample.ok) {
      row.fail_closed = true;
      row.concrete_runtime_consumed = false;
      row.diagnostic_code = sample.diagnostic_code.empty()
                                ? "INDEX.BENCHMARK.RUNTIME_REFUSED"
                                : sample.diagnostic_code;
      row.message_key = sample.message_key.empty()
                            ? "index.benchmark.runtime_refused"
                            : sample.message_key;
      break;
    }
    row.operation_count += sample.operation_count;
    row.rows_examined += sample.rows_examined;
    row.rows_returned += sample.rows_returned;
    row.rows_materialized += sample.rows_materialized;
    row.pages_or_containers_touched += sample.pages_or_containers_touched;
    row.evidence.insert(row.evidence.end(), sample.evidence.begin(),
                        sample.evidence.end());
  }

  row.p50_microseconds = Percentile(latencies, 50);
  row.p95_microseconds = Percentile(latencies, 95);
  row.p99_microseconds = Percentile(latencies, 99);
  AddEvidence(&row, "sample_count", std::to_string(latencies.size()));
  AddEvidence(&row, "concrete_runtime_consumed",
              row.concrete_runtime_consumed ? "true" : "false");
  AddEvidence(&row, "fail_closed", row.fail_closed ? "true" : "false");
  return row;
}

}  // namespace

IndexFamilyBenchmarkGateResult BuildIndexFamilyBenchmarkEvidence(
    const IndexFamilyBenchmarkGateRequest& request) {
  IndexFamilyBenchmarkGateResult result;
  result.status = OkStatus();
  result.diagnostic = Diagnostic(result.status,
                                 "INDEX.BENCHMARK_GATE.OK",
                                 "index.benchmark_gate.ok");

  const auto iterations = IterationCount(request.sample_iterations);
  const auto workloads = Workloads(request.include_fallback_disabled_rows);
  u64 salt = 182000;
  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    if (state == nullptr) {
      result.status = ErrorStatus();
      result.diagnostic =
          Diagnostic(result.status,
                     "INDEX.BENCHMARK_GATE.MISSING_CAPABILITY_STATE",
                     "index.benchmark_gate.missing_capability_state",
                     descriptor.id);
      continue;
    }

    for (const auto& workload : workloads) {
      salt += 100;
      if (!state->runtime_available || !state->benchmark_clean ||
          state->blocker != IndexFamilyPhysicalCapabilityBlocker::none) {
        result.rows.push_back(RefusedRow(descriptor, *state, workload));
      } else {
        result.rows.push_back(
            CompleteRow(descriptor, *state, workload, iterations, salt));
      }
    }
  }
  return result;
}

}  // namespace scratchbird::core::index
