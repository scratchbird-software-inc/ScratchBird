// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "batch_point_lookup_executor.hpp"
#include "candidate_set_executor.hpp"
#include "covering_index_payload.hpp"
#include "executor_batch_join_dml.hpp"
#include "executor_batch_relational.hpp"
#include "indexed_physical_operator.hpp"
#include "index_key_encoding.hpp"
#include "index_route_capability.hpp"
#include "join_planner_full.hpp"
#include "late_materialization_covering_scan_runtime.hpp"
#include "late_materialization_executor.hpp"
#include "parallel_physical_pipeline.hpp"
#include "runtime_consumption_evidence.hpp"
#include "runtime_filter_executor.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agents = scratchbird::core::agents;
namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace plan = scratchbird::engine::planner;
namespace uuid = scratchbird::core::uuid;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-220/221/222 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
}

bool HasEvidenceContaining(const std::vector<std::string>& evidence,
                           const std::string& expected) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(expected) != std::string::npos;
  });
}

bool HasEngineEvidence(
    const std::vector<scratchbird::engine::internal_api::EngineEvidenceReference>&
        evidence,
    const std::string& kind,
    const std::string& id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id.find(id) != std::string::npos;
                     });
}

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::engine};
}

platform::TypedUuid RowUuid(platform::byte suffix) {
  platform::TypedUuid uuid;
  uuid.kind = platform::UuidKind::row;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  uuid.value.bytes[15] = suffix;
  return uuid;
}

platform::TypedUuid ObjectUuid(platform::UuidKind kind, platform::byte suffix) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x02;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  uuid.value.bytes[15] = suffix;
  return uuid;
}

std::string UuidText(const platform::TypedUuid& typed) {
  return uuid::UuidToString(typed.value);
}

std::vector<platform::byte> Bytes(std::string_view text) {
  return std::vector<platform::byte>(text.begin(), text.end());
}

std::vector<platform::byte> EncodedKey(
    const platform::TypedUuid& index_uuid,
    std::string_view key) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = index_uuid;
  component.payload = Bytes(key);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "ORH-220 index key encoding failed");
  return encoded.encoded;
}

page::IndexBtreePhysicalScanBound Bound(
    const platform::TypedUuid& index_uuid,
    std::string_view key,
    bool inclusive = true) {
  page::IndexBtreePhysicalScanBound bound;
  bound.unbounded = false;
  bound.inclusive = inclusive;
  bound.encoded_key = EncodedKey(index_uuid, key);
  return bound;
}

page::IndexBtreePhysicalTree MakePhysicalTree(
    const platform::TypedUuid& index_uuid) {
  auto initialized = page::InitializeIndexBtreePhysicalTree(index_uuid, 768);
  Require(initialized.ok(), "ORH-220 physical btree init failed");
  return std::move(initialized.tree);
}

page::IndexBtreeCell PhysicalCell(const platform::TypedUuid& index_uuid,
                                  std::string_view key,
                                  const platform::TypedUuid& row_uuid,
                                  const platform::TypedUuid& version_uuid) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(index_uuid, key);
  cell.row_uuid = row_uuid;
  cell.version_uuid = version_uuid;
  return cell;
}

void InsertPhysicalCell(page::IndexBtreePhysicalTree* tree,
                        const page::IndexBtreeCell& cell) {
  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  const auto inserted = page::InsertIndexBtreeCell(tree, request);
  Require(inserted.ok(), "ORH-220 physical index insert failed");
}

exec::IndexedPhysicalOperatorRequest IndexedRequest(
    exec::IndexedPhysicalOperatorKind kind,
    const page::IndexBtreePhysicalTree* tree) {
  exec::IndexedPhysicalOperatorRequest request;
  request.kind = kind;
  request.physical_tree = tree;
  request.plan_safe = true;
  request.physical_tree_available = true;
  request.encoded_key_proof = true;
  request.encoded_bounds_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  return request;
}

struct PhysicalIndexFixture {
  platform::TypedUuid index_uuid = ObjectUuid(platform::UuidKind::object, 0x30);
  platform::TypedUuid row_alpha = RowUuid(0x31);
  platform::TypedUuid row_bravo = RowUuid(0x32);
  platform::TypedUuid row_charlie = RowUuid(0x33);
  platform::TypedUuid row_delta = RowUuid(0x34);
  platform::TypedUuid version_alpha = RowUuid(0x91);
  platform::TypedUuid version_bravo = RowUuid(0x92);
  platform::TypedUuid version_charlie = RowUuid(0x93);
  platform::TypedUuid version_delta = RowUuid(0x94);
  page::IndexBtreePhysicalTree tree = MakePhysicalTree(index_uuid);

  PhysicalIndexFixture() {
    InsertPhysicalCell(&tree,
                       PhysicalCell(index_uuid,
                                    "alpha",
                                    row_alpha,
                                    version_alpha));
    InsertPhysicalCell(&tree,
                       PhysicalCell(index_uuid,
                                    "bravo",
                                    row_bravo,
                                    version_bravo));
    InsertPhysicalCell(&tree,
                       PhysicalCell(index_uuid,
                                    "charlie",
                                    row_charlie,
                                    version_charlie));
    InsertPhysicalCell(&tree,
                       PhysicalCell(index_uuid,
                                    "delta",
                                    row_delta,
                                    version_delta));
  }
};

void RequireIndexedOperatorEvidence(
    const exec::IndexedPhysicalOperatorResult& result,
    std::string_view operator_name) {
  Require(result.ok, "ORH-220 indexed physical operator refused");
  Require(result.runtime_route_capability,
          "ORH-220 indexed physical route capability missing");
  Require(!result.benchmark_clean,
          "ORH-220 indexed operator claimed standalone benchmark-clean");
  Require(!result.table_scan_consumed,
          "ORH-220 indexed operator consumed table scan fallback");
  Require(HasEngineEvidence(result.evidence,
                            "indexed_physical_operator",
                            std::string(operator_name)),
          "ORH-220 indexed operator shape evidence missing");
  Require(HasEngineEvidence(result.evidence,
                            "indexed_physical_operator_physical_scan_consumed",
                            "index_btree_physical_tree"),
          "ORH-220 physical index scan consumption evidence missing");
  Require(HasEngineEvidence(result.evidence,
                            "mga_visibility_recheck",
                            "required") &&
              HasEngineEvidence(result.evidence,
                                "security_recheck",
                                "required") &&
              HasEngineEvidence(result.evidence,
                                "parser_or_donor_authority",
                                "false") &&
              HasEngineEvidence(result.evidence,
                                "index_or_cache_finality_authority",
                                "false"),
          "ORH-220 MGA/security/non-authority evidence missing");
}

idx::CandidateSetRow Candidate(platform::byte suffix, double score) {
  idx::CandidateSetRow row;
  row.row_uuid = RowUuid(suffix);
  row.score = score;
  row.exact_predicate_match = true;
  row.mga_visible = true;
  row.security_authorized = true;
  row.exact_payload_available = true;
  row.source = "orh220-runtime-gate";
  return row;
}

idx::CandidateSetAuthorityContext SafeAuthority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

exec::ExecutorBatchRequest BatchRequest(std::size_t max_rows = 64) {
  exec::ExecutorBatchRequest request;
  request.requested_mode = exec::ExecutorBatchRequestMode::kPreferBatch;
  request.node_supports_batch = true;
  request.preserve_input_order = true;
  request.limits.max_batch_rows = max_rows;
  request.limits.max_materialized_cells = max_rows * 8;
  request.limits.max_materialized_bytes = max_rows * 8 * sizeof(std::int64_t);
  return request;
}

opt::RuntimeOptimizedPathEvidence ExactIndexBlockerEvidence(
    std::string selected_path,
    std::string diagnostic_code,
    std::string fallback_reason) {
  auto evidence = opt::MakeSelectionOnlyRuntimeEvidence(
      std::move(selected_path), "embedded", std::move(diagnostic_code),
      std::move(fallback_reason));
  evidence.transaction_snapshot_class = "engine.mga.snapshot";
  evidence.result_contract_hash = "hash:orh220-index-dependent-blocker";
  evidence.contract_only = true;
  return evidence;
}

opt::RuntimeFilterDescriptor JoinRuntimeFilterDescriptor() {
  opt::RuntimeFilterDescriptor descriptor;
  descriptor.family = opt::RuntimeFilterFamily::kJoin;
  descriptor.route = opt::RuntimeFilterRoute::kProvider;
  descriptor.filter_id = "orh220.join.runtime_filter";
  descriptor.plan_node_id = "join-node-1";
  descriptor.provider_id = "physical-join-provider";
  descriptor.predicate_digest = "left.customer_id=right.customer_id";
  descriptor.descriptor_generation = 7;
  descriptor.required_descriptor_generation = 7;
  descriptor.input_rows = 100;
  descriptor.estimated_candidate_rows = 2;
  descriptor.estimated_pruned_rows = 98;
  descriptor.baseline_cost_units = 1000;
  descriptor.filter_cost_units = 10;
  descriptor.exact_recheck_cost_units = 20;
  descriptor.plan_shape_supported = true;
  descriptor.provider_supports_runtime_filters = true;
  descriptor.candidate_set_available = true;
  descriptor.security_context_present = true;
  descriptor.security_snapshot_bound = true;
  descriptor.grants_proven = true;
  descriptor.engine_mga_authoritative = true;
  descriptor.exact_recheck_available = true;
  descriptor.exact_fallback_available = true;
  descriptor.mga_visibility_recheck_required = true;
  descriptor.security_authorization_recheck_required = true;
  return descriptor;
}

// SEARCH_KEY: ORH_PHYSICAL_JOIN_OPERATOR_SUITE
void PhysicalJoinSuiteConsumesNonIndexOperatorsAndBlocksIndexClaims() {
  const auto left = exec::MakeBatch(
      "left", {{{1, 10}}, {{2, 20}}, {{2, 21}}, {{4, 40}}});
  const auto right = exec::MakeBatch(
      "right", {{{2, 200}}, {{2, 201}}, {{3, 300}}, {{4, 400}}});

  exec::ExecutorBatchJoinRequest join_request;
  join_request.batch_request = BatchRequest();
  join_request.left_column = 0;
  join_request.right_column = 0;
  join_request.output_descriptor_digest = "joined";
  const auto hash_join = exec::ExecuteBatchedJoinEqual(left, right, join_request);
  Require(hash_join.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kBatch,
          "hash join did not consume batch mode");
  Require(hash_join.evidence.hash_route_used,
          "hash join route was not consumed");
  Require(hash_join.output.rows.size() == 5,
          "hash join produced unexpected row count");
  Require(hash_join.evidence.counters.hash_join_left_probes == left.rows.size(),
          "hash join probe counter did not match left rows");

  const auto left_sorted = exec::SortByColumn(left, 0, true);
  const auto right_sorted = exec::SortByColumn(right, 0, true);
  const auto merge_join =
      exec::MergeJoinEqual(left_sorted, right_sorted, 0, 0);
  const auto nested_loop =
      exec::NestedLoopJoinEqual(left_sorted, right_sorted, 0, 0);
  Require(exec::DeterministicBatchSignature(merge_join) ==
              exec::DeterministicBatchSignature(nested_loop),
          "merge join did not preserve sorted nested-loop equality semantics");
  Require(merge_join.rows.size() == 5,
          "merge join produced unexpected row count");

  const auto graph = opt::BuildJoinGraph(
      {{"rel.orders", 1000, false},
       {"rel.customers", 100, false},
       {"rel.countries", 10, true}},
      {{"rel.orders", "rel.customers", "orders.customer_id=customers.id",
        opt::JoinSemanticKind::kInner, true, false, false, false, false,
        false, false, 0.02},
       {"rel.customers", "rel.countries", "customers.country_id=countries.id",
        opt::JoinSemanticKind::kInner, true, false, false, false, false,
        false, false, 0.05}},
      false, false);
  const auto dp = opt::EnumerateDeterministicJoinOrder(graph, 1024 * 1024);
  Require(dp.ok, "join-order DP rejected safe inner equi-join graph");
  Require(dp.bounded_enumeration_applied,
          "join-order DP did not record bounded enumeration");
  Require(dp.transitions_considered > 0,
          "join-order DP did not consider connected transitions");
  Require(dp.method == plan::PhysicalAccessKind::kJoinHash ||
              dp.method == plan::PhysicalAccessKind::kJoinMerge,
          "join-order DP did not select a physical hash or merge join method");

  opt::RuntimeFilterPushdownRequest pushdown;
  pushdown.plan_id = "orh220-plan";
  pushdown.candidates = {JoinRuntimeFilterDescriptor()};
  const auto filter_decision = opt::EvaluateRuntimeFilterPushdown(pushdown);
  Require(filter_decision.ok, "optimizer refused safe runtime filter");
  Require(filter_decision.selected_filters.size() == 1,
          "optimizer did not select runtime filter");

  exec::RuntimeFilterProviderSet providers;
  providers.physical_provider =
      [](const exec::RuntimeFilterProviderRequest& request) {
        exec::RuntimeFilterProviderResult result;
        result.status = OkStatus();
        result.exact_recheck_evidence_present = true;
        result.mga_recheck_evidence_present = true;
        result.security_recheck_evidence_present = true;
        result.candidate_rows = {Candidate(1, 10.0), Candidate(2, 9.0)};
        result.evidence = {"runtime_filter.provider=physical_join_hash_table",
                           "runtime_filter.provider_candidate_rows_only=true",
                           "runtime_filter.request_filter_id=" +
                               request.descriptor.filter_id};
        return result;
      };
  const auto filter_runtime = exec::ExecuteRuntimeFilterPushdown(
      filter_decision.selected_filters, SafeAuthority(), providers);
  Require(filter_runtime.ok(), "executor refused selected runtime filter");
  Require(filter_runtime.final_row_uuids.size() == 2,
          "runtime filter did not finalize row-id stream");
  Require(filter_runtime.counters.exact_recheck_count == 2,
          "runtime filter did not perform exact recheck");

  const auto row_stream = idx::MakeExactRowUuidOrderedCandidateSet(
      {Candidate(1, 10.0), Candidate(2, 9.0)}, SafeAuthority(), false);
  Require(row_stream.ok(), "row-id stream construction failed");
  const auto finalized =
      exec::FinalizeCandidateSetForExecutor(row_stream.output, SafeAuthority());
  Require(finalized.ok(), "executor did not consume row-id stream");
  Require(finalized.final_row_uuids.size() == 2,
          "executor row-id stream finalization count mismatch");

  const auto* btree_select = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select,
      idx::IndexFamily::btree);
  Require(btree_select != nullptr && btree_select->route_complete() &&
              btree_select->supports_ordered_range &&
              btree_select->requires_mga_recheck &&
              btree_select->requires_security_recheck,
          "ORH-220 corrected btree select route capability missing");
  const auto* hash_select = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select,
      idx::IndexFamily::hash);
  Require(hash_select != nullptr && hash_select->supports_equality_lookup &&
              !hash_select->supports_ordered_range &&
              hash_select->requires_exact_recheck,
          "ORH-220 hash route capability drifted");
  const auto* donor_select = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select,
      idx::IndexFamily::donor_emulated);
  const auto* policy_select = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select,
      idx::IndexFamily::policy_blocked);
  Require(donor_select != nullptr && !donor_select->route_complete() &&
              policy_select != nullptr && !policy_select->route_complete(),
          "ORH-220 donor/policy route capabilities became authoritative");

  PhysicalIndexFixture physical;
  auto nested = IndexedRequest(
      exec::IndexedPhysicalOperatorKind::indexed_nested_loop,
      &physical.tree);
  exec::IndexedPhysicalOuterProbe point_probe;
  point_probe.outer_ordinal = 7;
  point_probe.encoded_key = EncodedKey(physical.index_uuid, "alpha");
  nested.outer_probes.push_back(point_probe);
  exec::IndexedPhysicalOuterProbe range_probe;
  range_probe.outer_ordinal = 8;
  range_probe.range_probe = true;
  range_probe.lower_bound = Bound(physical.index_uuid, "charlie");
  range_probe.upper_bound = Bound(physical.index_uuid, "delta");
  nested.outer_probes.push_back(range_probe);
  const auto nested_result = exec::ExecuteIndexedPhysicalOperator(nested);
  RequireIndexedOperatorEvidence(nested_result, "indexed_nested_loop");
  Require(nested_result.locators.size() == 3,
          "ORH-220 indexed nested-loop locator count mismatch");
  Require(nested_result.locators[0].row_uuid == UuidText(physical.row_alpha) &&
              nested_result.locators[1].row_uuid ==
                  UuidText(physical.row_charlie) &&
              nested_result.locators[2].row_uuid ==
                  UuidText(physical.row_delta),
          "ORH-220 indexed nested-loop locators drifted");
  Require(HasEngineEvidence(nested_result.evidence,
                            "indexed_nested_loop_outer_ordinal",
                            "7") &&
              HasEngineEvidence(nested_result.evidence,
                                "indexed_nested_loop_outer_ordinal",
                                "8"),
          "ORH-220 indexed nested-loop probe evidence missing");

  auto physical_filter =
      IndexedRequest(exec::IndexedPhysicalOperatorKind::runtime_filter,
                     &physical.tree);
  physical_filter.runtime_filter_keys.push_back(
      EncodedKey(physical.index_uuid, "bravo"));
  physical_filter.runtime_filter_keys.push_back(
      EncodedKey(physical.index_uuid, "delta"));
  const auto physical_filter_result =
      exec::ExecuteIndexedPhysicalOperator(physical_filter);
  RequireIndexedOperatorEvidence(physical_filter_result, "runtime_filter");
  Require(physical_filter_result.locators.size() == 2,
          "ORH-220 physical runtime filter locator count mismatch");
  Require(HasEngineEvidence(physical_filter_result.evidence,
                            "runtime_filter_exact_recheck",
                            "mga_visibility_and_security_required_per_candidate"),
          "ORH-220 physical runtime filter exact recheck evidence missing");

  auto right_tree = MakePhysicalTree(physical.index_uuid);
  InsertPhysicalCell(&right_tree,
                     PhysicalCell(physical.index_uuid,
                                  "alpha",
                                  RowUuid(0x41),
                                  RowUuid(0xa1)));
  InsertPhysicalCell(&right_tree,
                     PhysicalCell(physical.index_uuid,
                                  "charlie",
                                  RowUuid(0x42),
                                  RowUuid(0xa2)));
  InsertPhysicalCell(&right_tree,
                     PhysicalCell(physical.index_uuid,
                                  "echo",
                                  RowUuid(0x43),
                                  RowUuid(0xa3)));
  auto merge_ordered = IndexedRequest(
      exec::IndexedPhysicalOperatorKind::merge_ordered_input,
      &physical.tree);
  merge_ordered.right_physical_tree = &right_tree;
  const auto physical_merge =
      exec::ExecuteIndexedPhysicalOperator(merge_ordered);
  RequireIndexedOperatorEvidence(physical_merge, "merge_ordered_input");
  Require(physical_merge.merge_pairs.size() == 2,
          "ORH-220 physical merge ordered pair count mismatch");
  Require(HasEngineEvidence(physical_merge.evidence,
                            "merge_ordered_left_stream_evidence",
                            "indexed_physical_operator_scan_kind=merge_ordered_left") &&
              HasEngineEvidence(physical_merge.evidence,
                                "merge_ordered_right_stream_evidence",
                                "indexed_physical_operator_scan_kind=merge_ordered_right"),
          "ORH-220 physical merge stream evidence missing");

  auto missing_mga = IndexedRequest(
      exec::IndexedPhysicalOperatorKind::indexed_nested_loop,
      &physical.tree);
  missing_mga.outer_probes.push_back(point_probe);
  missing_mga.durable_mga_inventory_proof = false;
  const auto missing_mga_result =
      exec::ExecuteIndexedPhysicalOperator(missing_mga);
  Require(!missing_mga_result.ok &&
              missing_mga_result.diagnostic_code ==
                  "SB-IRC060-MGA-PROOF-REQUIRED",
          "ORH-220 missing MGA proof did not fail closed");
  auto unsafe_donor = IndexedRequest(
      exec::IndexedPhysicalOperatorKind::indexed_nested_loop,
      &physical.tree);
  unsafe_donor.outer_probes.push_back(point_probe);
  unsafe_donor.parser_or_donor_authority = true;
  const auto donor_result = exec::ExecuteIndexedPhysicalOperator(unsafe_donor);
  Require(!donor_result.ok &&
              donor_result.diagnostic_code ==
                  "SB-IRC060-PARSER-DONOR-AUTHORITY-FORBIDDEN",
          "ORH-220 parser/donor authority did not fail closed");
}

agents::ResourceGovernanceQuotaVector Quotas(std::int64_t value) {
  agents::ResourceGovernanceQuotaVector quotas;
  quotas.memory_bytes = value;
  quotas.device_memory_bytes = value;
  quotas.pinned_memory_bytes = value;
  quotas.io_bytes = value;
  quotas.io_ops = value;
  quotas.worker_threads = 8;
  quotas.backlog_items = value;
  quotas.candidate_rows = value;
  quotas.cache_entries = value;
  quotas.batch_rows = value;
  quotas.fragments = value;
  quotas.lanes = 8;
  quotas.time_budget_microseconds = value;
  return quotas;
}

agents::ResourceGovernanceAdmissionRequest GovernanceRequest() {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = "orh221.parallel.pipeline";
  request.expected_family =
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline;
  request.descriptor.descriptor_id = "runtime.parallel.pipeline.orh221";
  request.descriptor.family =
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime_policy:orh221";
  request.descriptor.descriptor_generation = 9;
  request.descriptor.expected_generation = 9;
  request.descriptor.limits = Quotas(1000000);
  request.descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested = Quotas(2);
  request.requested.memory_bytes = 4096;
  request.requested.io_bytes = 1024;
  request.requested.worker_threads = 2;
  request.requested.candidate_rows = 2;
  request.requested.batch_rows = 8;
  request.requested.fragments = 2;
  request.requested.lanes = 2;
  return request;
}

exec::ParallelPhysicalSnapshotToken SnapshotToken() {
  exec::ParallelPhysicalSnapshotToken snapshot;
  snapshot.token_id = "orh221-shared-mga-snapshot";
  snapshot.snapshot_generation = 17;
  snapshot.transaction_number = 19;
  snapshot.visibility_high_water_mark = 23;
  snapshot.catalog_epoch = 29;
  snapshot.security_epoch = 31;
  snapshot.policy_epoch = 37;
  snapshot.engine_mga_snapshot = true;
  snapshot.transaction_inventory_bound = true;
  snapshot.catalog_security_policy_epochs_bound = true;
  return snapshot;
}

exec::ParallelPhysicalWorkerLane Lane(platform::u32 worker_id,
                                      platform::byte row_suffix) {
  exec::ParallelPhysicalWorkerLane lane;
  lane.worker_id = worker_id;
  lane.received_snapshot_token_id = "orh221-shared-mga-snapshot";
  lane.received_snapshot_generation = 17;
  lane.fragment_count = 1;
  lane.byte_count = 128;
  lane.candidate_rows = {Candidate(row_suffix, 10.0 - row_suffix)};
  return lane;
}

// SEARCH_KEY: ORH_VECTORIZED_PARALLEL_PIPELINE
void VectorizedAndParallelPipelineUsesBatchesWorkersAndDeterministicOutput() {
  const auto input = exec::MakeBatch(
      "scan", {{{1, 10, 100}}, {{1, 20, 200}}, {{2, 5, 50}},
               {{2, 7, 70}}, {{3, 1, 10}}});

  exec::ExecutorBatchScanFilterProjectionRequest scan_request;
  scan_request.batch_request = BatchRequest();
  scan_request.filter.enabled = true;
  scan_request.filter.column = 1;
  scan_request.filter.op = exec::Int64ComparisonOperator::kGreaterThanOrEqual;
  scan_request.filter.value = 7;
  scan_request.projection_columns = {0, 2};
  scan_request.output_descriptor_digest = "scan.projected";
  const auto scan =
      exec::ExecuteBatchedScanFilterProjection(input, scan_request);
  Require(scan.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
          "scan/filter/projection did not consume bounded batch mode");
  Require(scan.evidence.rows_processed_in_batch == input.rows.size(),
          "scan/filter/projection did not process rows in batch");
  Require(scan.evidence.counters.rows_filter_passed == 3,
          "scan/filter/projection filter pass count mismatch");
  Require(scan.evidence.authority.owns_transaction_finality == false &&
              scan.evidence.authority.owns_visibility == false,
          "scan/filter/projection claimed MGA authority");

  exec::ExecutorBatchAggregateSumRequest aggregate_request;
  aggregate_request.batch_request = BatchRequest();
  aggregate_request.key_column = 0;
  aggregate_request.value_column = 1;
  aggregate_request.output_descriptor_digest = "aggregate.sum";
  const auto aggregate =
      exec::ExecuteBatchedAggregateSumByKey(scan.output, aggregate_request);
  Require(aggregate.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kBatch,
          "aggregate did not consume bounded batch mode");
  Require(aggregate.evidence.aggregate_group_order_deterministic,
          "aggregate did not record deterministic group order");
  Require(aggregate.output.rows.size() == 2,
          "aggregate produced unexpected groups");

  exec::ExecutorBatchJoinRequest join_request;
  join_request.batch_request = BatchRequest();
  join_request.left_column = 0;
  join_request.right_column = 0;
  const auto joined =
      exec::ExecuteBatchedJoinEqual(aggregate.output, aggregate.output,
                                    join_request);
  Require(joined.evidence.hash_route_used,
          "batched join pipeline did not consume hash route");
  Require(!joined.evidence.deterministic_result_signature.empty(),
          "batched join pipeline did not record deterministic signature");

  exec::ParallelPhysicalPipelineRequest parallel;
  parallel.family = exec::ParallelPhysicalPipelineFamily::kPageScan;
  parallel.snapshot = SnapshotToken();
  parallel.authority = SafeAuthority();
  parallel.resource_governance = GovernanceRequest();
  parallel.quotas.max_workers = 2;
  parallel.quotas.max_fragments = 4;
  parallel.quotas.max_candidate_rows = 4;
  parallel.quotas.max_bytes = 1024;
  parallel.quotas.max_worker_pressure_bytes = 512;
  parallel.worker_lanes = {Lane(1, 1), Lane(2, 2)};
  parallel.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& request) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        exec::ParallelPhysicalWorkerExecutionResult result;
        result.status = OkStatus();
        result.worker_id = request.lane.worker_id;
        result.snapshot_token_id = request.snapshot.token_id;
        result.snapshot_generation = request.snapshot.snapshot_generation;
        result.fragment_count = request.lane.fragment_count;
        result.byte_count = request.lane.byte_count;
        result.candidate_rows = request.lane.candidate_rows;
        result.worker_thread_id = std::this_thread::get_id();
        result.evidence = {"parallel_worker.real_async_provider=true"};
        return result;
      };
  const auto parallel_result = exec::ExecuteParallelPhysicalPipeline(parallel);
  Require(parallel_result.ok(), "parallel physical pipeline failed");
  Require(parallel_result.counters.workers_admitted == 2,
          "parallel pipeline did not admit both workers");
  Require(parallel_result.counters.actual_worker_threads >= 2,
          "parallel pipeline did not use distinct worker threads");
  Require(parallel_result.final_row_uuids.size() == 2,
          "parallel pipeline did not finalize candidate rows");
  Require(HasEvidence(parallel_result.evidence,
                      "parallel_pipeline.deterministic_row_uuid_ordering=true"),
          "parallel pipeline did not record deterministic ordering evidence");
}

page::LargePayloadDescriptor Descriptor(platform::byte suffix,
                                        platform::u64 byte_count) {
  page::LargePayloadDescriptor descriptor;
  descriptor.payload_uuid = ObjectUuid(platform::UuidKind::object, suffix);
  descriptor.owner_object_uuid = ObjectUuid(platform::UuidKind::object, 80);
  descriptor.generation_scope_uuid = ObjectUuid(platform::UuidKind::object, 81);
  descriptor.filespace_uuid = ObjectUuid(platform::UuidKind::filespace, 82);
  descriptor.overflow_value_uuid = ObjectUuid(platform::UuidKind::object, 83);
  descriptor.family = page::LargePayloadFamily::blob;
  descriptor.generation = 1;
  descriptor.creator_local_transaction_id = 7;
  descriptor.byte_count = byte_count;
  descriptor.content_hash = "hash:" + std::to_string(suffix);
  descriptor.filespace_class = "primary";
  descriptor.page_family = "large_payload";
  return descriptor;
}

page::LatePayloadReference Reference(platform::byte row_suffix,
                                     platform::byte payload_suffix,
                                     bool redacted) {
  page::LatePayloadReference reference;
  reference.row_uuid = RowUuid(row_suffix);
  reference.descriptor = Descriptor(payload_suffix, redacted ? 0 : 4);
  reference.observer_snapshot_visible_through_local_transaction_id = 42;
  reference.descriptor_evidence_present = true;
  reference.descriptor_fresh = true;
  reference.exact_predicate_rechecked_by_engine = true;
  reference.mga_visibility_rechecked_by_engine = true;
  reference.security_authorized_by_engine = true;
  reference.security_snapshot_bound = true;
  reference.redaction_policy_bound = true;
  reference.redaction_required = redacted;
  reference.unredacted_payload_authorized_by_security = !redacted;
  reference.redaction_reason = redacted ? "policy.redacted" : "";
  return reference;
}

idx::CoveringIndexPayloadColumnRef CoveringColumn(platform::byte seed,
                                                  platform::u32 ordinal) {
  idx::CoveringIndexPayloadColumnRef column;
  column.column_uuid = ObjectUuid(platform::UuidKind::object, seed);
  column.type_descriptor_uuid =
      ObjectUuid(platform::UuidKind::object,
                 static_cast<platform::byte>(seed + 40));
  column.projection_ordinal = ordinal;
  column.required = true;
  return column;
}

idx::CoveringIndexPayloadColumnValue CoveringValue(
    const idx::CoveringIndexPayloadColumnRef& column,
    std::string_view value) {
  idx::CoveringIndexPayloadColumnValue out;
  out.column_uuid = column.column_uuid;
  out.projection_ordinal = column.projection_ordinal;
  out.kind = idx::CoveringIndexPayloadValueKind::inline_value;
  out.encoded_value = Bytes(value);
  out.binary_result_frame_compatible = true;
  out.redaction_safe = true;
  out.unredacted_authorized = true;
  return out;
}

idx::CoveringIndexPayloadAssemblyResult AssembleCoveringPayload(
    const PhysicalIndexFixture& fixture,
    const exec::IndexedPhysicalOperatorLocator& locator,
    std::string_view first,
    std::string_view second) {
  const auto c1 = CoveringColumn(1, 0);
  const auto c2 = CoveringColumn(2, 1);
  idx::CoveringIndexPayloadAssemblyRequest request;
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = ObjectUuid(platform::UuidKind::object, 0x44);
  request.row_uuid = locator.from_physical_index
                         ? uuid::ParseDurableEngineIdentityUuid(
                               platform::UuidKind::row,
                               locator.row_uuid)
                               .value
                         : fixture.row_alpha;
  request.version_uuid = uuid::ParseDurableEngineIdentityUuid(
                             platform::UuidKind::row,
                             locator.version_uuid)
                             .value;
  request.descriptor_result_contract_hash = "contract:orh222:v1";
  request.payload_generation = 10;
  request.redaction_policy_epoch = 20;
  request.security_policy_epoch = 30;
  request.freshness_generation = 40;
  request.descriptor_columns = {c1, c2};
  request.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  request.values = {CoveringValue(c1, first), CoveringValue(c2, second)};
  request.projection_only = true;
  request.result_contract_bound = true;
  const auto assembled = idx::AssembleCoveringIndexPayload(request);
  Require(assembled.ok(), "ORH-222 covering payload assembly failed");
  return assembled;
}

idx::CoveringIndexPayloadAdmission AdmitCoveringPayload(
    const idx::CoveringIndexPayloadRecord& record) {
  const auto c1 = CoveringColumn(1, 0);
  const auto c2 = CoveringColumn(2, 1);
  idx::CoveringIndexPayloadValidationRequest request;
  request.record = record;
  request.locator.encoded_key = Bytes("orh222-encoded-key");
  request.locator.row_uuid = record.row_uuid;
  request.locator.version_uuid = record.version_uuid;
  request.locator.leaf_page_number = 7;
  request.locator.cell_ordinal = 1;
  request.locator.physical_btree_locator_scan = true;
  request.required_columns = {c1, c2};
  request.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  request.expected_descriptor_result_contract_hash = "contract:orh222:v1";
  request.expected_payload_generation = 10;
  request.expected_redaction_policy_epoch = 20;
  request.expected_security_policy_epoch = 30;
  request.expected_freshness_generation = 40;
  request.descriptor_epoch_current = true;
  request.result_contract_current = true;
  request.redaction_epoch_current = true;
  request.security_epoch_current = true;
  request.freshness_current = true;
  request.result_frame_contract_proven = true;
  request.redaction_policy_safe = true;
  request.exact_predicate_recheck_planned = true;
  request.mga_visibility_recheck_planned = true;
  request.security_authorization_recheck_planned = true;
  request.exact_predicate_rechecked_by_engine = true;
  request.mga_visibility_rechecked_by_engine = true;
  request.security_authorized_by_engine = true;
  request.base_row_recheck_available = true;
  request.allow_index_only = true;
  const auto admission = idx::ValidateCoveringIndexPayloadForLocator(request);
  Require(admission.ok(), "ORH-222 covering payload admission failed");
  return admission;
}

exec::IndexedPhysicalOperatorResult FullPhysicalIndexStream(
    const PhysicalIndexFixture& fixture) {
  auto request =
      IndexedRequest(exec::IndexedPhysicalOperatorKind::range_scan,
                     &fixture.tree);
  request.lower_bound.unbounded = true;
  request.upper_bound.unbounded = true;
  const auto stream = exec::ExecuteIndexedPhysicalOperator(request);
  RequireIndexedOperatorEvidence(stream, "range_scan");
  Require(stream.locators.size() == 4,
          "ORH-222 full physical index stream locator count mismatch");
  return stream;
}

// SEARCH_KEY: ORH_LATE_MATERIALIZATION_COVERING_INDEX
void LateMaterializationAndCoveringPathUseRowIdStreamsWithExactBlockers() {
  const auto authority = SafeAuthority();
  const auto candidates = idx::MakeExactRowUuidOrderedCandidateSet(
      {Candidate(1, 10.0), Candidate(2, 9.0), Candidate(3, 1.0)},
      authority, false);
  Require(candidates.ok(), "late materialization candidate stream failed");

  exec::LateMaterializationPlan late_plan;
  late_plan.plan_id = "orh222.late_materialization";
  late_plan.candidate_intersection = candidates.output;
  late_plan.authority = authority;
  late_plan.payload_references = {Reference(1, 11, false),
                                  Reference(2, 12, true),
                                  Reference(3, 13, false)};
  late_plan.top_k_limit = 2;
  late_plan.candidate_intersection_proven = true;
  late_plan.exact_predicate_recheck_required = true;
  late_plan.mga_visibility_recheck_required = true;
  late_plan.security_authorization_recheck_required = true;
  late_plan.redaction_gate_required = true;
  late_plan.top_k_pruning_required = true;

  const auto materialized = exec::ExecuteLateMaterialization(
      late_plan, [](const page::LatePayloadFetchRequest& request) {
        page::LatePayloadFetchResult result;
        result.status = OkStatus();
        result.row_uuid = request.reference.row_uuid;
        result.descriptor = request.reference.descriptor;
        result.redacted = request.reference.redaction_required;
        result.fetched = !result.redacted;
        if (result.fetched) {
          result.payload_bytes = {1, 2, 3, 4};
        }
        result.evidence = {
            "late_payload_fetch.test_fetcher=true",
            "late_payload_fetch.final_authorized_and_pruned=" +
                std::string(request.requester_final_authorized_and_pruned
                                ? "true"
                                : "false")};
        return result;
      });
  Require(materialized.ok(), "late materialization failed");
  Require(materialized.rows.size() == 2,
          "late materialization did not apply top-K after recheck");
  Require(materialized.counters.payload_fetcher_invocation_count == 2,
          "late materialization fetched rows outside pruned row-id stream");
  Require(materialized.counters.redacted_payload_count == 1,
          "late materialization redaction gate did not run");
  Require(HasEvidence(materialized.evidence,
                      "late_materialization.fetch_after_top_k_pruning=true"),
          "late materialization missing top-K fetch evidence");

  idx::BatchPointLookupPlan covering_plan;
  covering_plan.purpose = idx::BatchPointLookupPurpose::key_value;
  covering_plan.plan_id = "orh222.covering.point_lookup";
  covering_plan.keys = {{"customer:1", 0}, {"customer:2", 1},
                        {"customer:1", 2}};
  covering_plan.caller_evidence = {
      "covering_index_projection_payload=provider_payload",
      "covering_index_order=encoded_key"};
  const auto covering = exec::ExecuteBatchPointLookupForExecutor(
      covering_plan, authority,
      [](const idx::BatchPointLookupProviderRequest& request) {
        idx::BatchPointLookupProviderResult result;
        result.status = OkStatus();
        for (const auto& key : request.ordered_unique_keys) {
          idx::BatchPointLookupProviderRow row;
          row.encoded_key = key.encoded_key;
          row.candidate = key.encoded_key == "customer:1"
                              ? Candidate(1, 10.0)
                              : Candidate(2, 9.0);
          row.payload = "covered:" + key.encoded_key;
          row.attributes = {{"projection", "covered"},
                            {"order", "encoded_key"}};
          row.exact_key_match = true;
          row.exact_row_uuid = true;
          row.ordered_point_lookup = true;
          result.rows.push_back(std::move(row));
        }
        result.evidence = {"batch_point_lookup.provider=covering_index_probe",
                           "batch_point_lookup.covering_payload=true"};
        return result;
      });
  Require(covering.ok(), "covering-style batch point lookup failed");
  Require(covering.rows.size() == 3,
          "covering lookup did not preserve duplicate key occurrences");
  Require(covering.lookup.provider_batch_executed,
          "covering lookup did not execute provider batch");
  Require(HasEvidence(covering.evidence,
                      "executor.batch_point_lookup.requires_mga_recheck=true"),
          "covering lookup did not preserve MGA recheck evidence");

  const PhysicalIndexFixture fixture;
  const auto physical_stream = FullPhysicalIndexStream(fixture);
  const auto late_indexed =
      exec::ConsumeIndexedRowIdStreamForLateMaterialization(
          physical_stream,
          {},
          [](const exec::IndexedPhysicalOperatorLocator& locator) {
            exec::LateMaterializationIndexedProviderResult out;
            out.ok = true;
            out.row.row_uuid = locator.row_uuid;
            out.row.version_uuid = locator.version_uuid;
            out.row.projected_values = {"base:" + locator.row_uuid};
            out.row.evidence = {"orh222.base_row_exact_recheck=true"};
            out.evidence = {"orh222.provider.physical_locator_consumed=true"};
            return out;
          });
  Require(late_indexed.ok,
          "ORH-222 indexed row-id late materialization failed");
  Require(late_indexed.runtime_route_capability &&
              !late_indexed.full_table_scan_or_materialization,
          "ORH-222 late materialization did not consume row-id stream route");
  Require(late_indexed.rows.size() == physical_stream.locators.size(),
          "ORH-222 late materialization row-id count mismatch");
  Require(HasEvidence(late_indexed.evidence,
                      "irc061.late_materialization.row_id_stream_only=true") &&
              HasEvidence(late_indexed.evidence,
                          "irc061.mga_visibility_recheck.engine_owned=true") &&
              HasEvidence(late_indexed.evidence,
                          "irc061.security_authorization_recheck.engine_owned=true") &&
              HasEvidence(late_indexed.evidence,
                          "irc061.index_payload_finality_authority=false"),
          "ORH-222 late materialization authority evidence missing");

  std::vector<idx::CoveringIndexPayloadAdmission> admissions;
  admissions.push_back(AdmitCoveringPayload(
      AssembleCoveringPayload(fixture,
                              physical_stream.locators[0],
                              "alpha-name",
                              "alpha-city")
          .record));
  admissions.push_back(AdmitCoveringPayload(
      AssembleCoveringPayload(fixture,
                              physical_stream.locators[1],
                              "bravo-name",
                              "bravo-city")
          .record));
  admissions.push_back(AdmitCoveringPayload(
      AssembleCoveringPayload(fixture,
                              physical_stream.locators[2],
                              "charlie-name",
                              "charlie-city")
          .record));
  admissions.push_back(AdmitCoveringPayload(
      AssembleCoveringPayload(fixture,
                              physical_stream.locators[3],
                              "delta-name",
                              "delta-city")
          .record));
  exec::CoveringProjectionOnlyScanRequest covering_scan;
  covering_scan.physical_stream = &physical_stream;
  for (const auto& admission : admissions) {
    covering_scan.admissions.push_back(&admission);
  }
  const auto covering_scan_result =
      exec::ExecuteCoveringProjectionOnlyScan(covering_scan);
  Require(covering_scan_result.ok,
          "ORH-222 full covering projection scan failed");
  Require(covering_scan_result.runtime_route_capability &&
              covering_scan_result.projection_only &&
              !covering_scan_result.full_table_scan_or_materialization,
          "ORH-222 covering projection did not consume physical route");
  Require(covering_scan_result.rows.size() == physical_stream.locators.size(),
          "ORH-222 covering projection row count mismatch");
  Require(HasEvidence(covering_scan_result.evidence,
                      "irc061.covering_projection_only_scan=true") &&
              HasEvidence(covering_scan_result.evidence,
                          "irc061.covering_full_table_scan_used=false") &&
              HasEvidence(covering_scan_result.evidence,
                          "irc061.covering_base_row_materialization_used=false") &&
              HasEvidenceContaining(
                  covering_scan_result.evidence,
                  "covering_payload.required_rechecks_proven=true"),
          "ORH-222 covering projection-only evidence missing");

  exec::IndexPlanShapeRegressionGuardRequest guard;
  guard.route_name = "orh222.covering_projection_only";
  guard.required_path =
      exec::IndexPlanShapeRequiredPath::covering_projection_only;
  guard.physical_result = &physical_stream;
  guard.covering_projection_result = &covering_scan_result;
  const auto guard_result =
      exec::EvaluateIndexPlanShapeRegressionGuard(guard);
  Require(guard_result.ok && guard_result.physical_route_consumed,
          "ORH-222 shape guard did not accept covering projection route");

  exec::CoveringProjectionOnlyScanRequest unsafe_covering = covering_scan;
  unsafe_covering.proof.security_authorized_by_engine = false;
  const auto unsafe_covering_result =
      exec::ExecuteCoveringProjectionOnlyScan(unsafe_covering);
  Require(!unsafe_covering_result.ok &&
              unsafe_covering_result.diagnostic_detail ==
                  "security_authorization_recheck_required",
          "ORH-222 unsafe covering route did not fail closed");

  exec::IndexPlanShapeRegressionGuardRequest scan_regression = guard;
  scan_regression.table_scan_fallback = true;
  const auto scan_regression_result =
      exec::EvaluateIndexPlanShapeRegressionGuard(scan_regression);
  Require(!scan_regression_result.ok &&
              scan_regression_result.diagnostic_code ==
                  "SB-IRC062-TABLE-SCAN-FALLBACK-REGRESSION",
          "ORH-222 table-scan regression was not refused");
}

}  // namespace

int main() {
  PhysicalJoinSuiteConsumesNonIndexOperatorsAndBlocksIndexClaims();
  VectorizedAndParallelPipelineUsesBatchesWorkersAndDeterministicOutput();
  LateMaterializationAndCoveringPathUseRowIdStreamsWithExactBlockers();
  return 0;
}
