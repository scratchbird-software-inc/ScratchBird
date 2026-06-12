// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "index_btree_page.hpp"
#include "index_key_encoding.hpp"
#include "logical_plan.hpp"
#include "optimizer_catalog_backed_planning.hpp"
#include "optimizer_correctness_oracle.hpp"
#include "optimizer_explain.hpp"
#include "optimizer_feedback.hpp"
#include "optimizer_feedback_stability.hpp"
#include "optimizer_plan_cache.hpp"
#include "physical_plan.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace page = scratchbird::storage::page;
namespace plan = scratchbird::engine::planner;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "engine_listener_optimizer_integrated_route_conformance: "
            << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

bool Contains(const std::vector<std::string>& values,
              std::string_view expected) {
  return std::find(values.begin(), values.end(), std::string(expected)) !=
         values.end();
}

bool ContainsText(const std::vector<std::string>& values,
                  std::string_view expected) {
  return std::any_of(values.begin(),
                     values.end(),
                     [&](const auto& value) {
                       return value.find(expected) != std::string::npos;
                     });
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view value) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id.find(value) !=
                                  std::string::npos;
                     });
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

std::string Sha(std::string_view label) {
  return "sha256:eler041-" + std::string(label);
}

struct FixtureIds {
  std::string database_uuid =
      UuidText(platform::UuidKind::database, 1771400000000ull, 0x41);
  std::string relation_uuid =
      UuidText(platform::UuidKind::object, 1771400001000ull, 0x42);
  std::string index_uuid =
      UuidText(platform::UuidKind::object, 1771400002000ull, 0x43);
  std::string function_uuid =
      UuidText(platform::UuidKind::object, 1771400003000ull, 0x44);
  std::string filespace_uuid =
      UuidText(platform::UuidKind::object, 1771400004000ull, 0x45);
  std::string column_uuid =
      UuidText(platform::UuidKind::object, 1771400005000ull, 0x46);
  std::string row_uuid =
      UuidText(platform::UuidKind::row, 1771400006000ull, 0x47);
  std::string version_uuid =
      UuidText(platform::UuidKind::row, 1771400007000ull, 0x48);
  std::string descriptor_digest = Sha("descriptor-customer");
  std::string sblr_digest = Sha("sblr-customer-lookup");
  std::string operation_id = "engine.operation.eler041.customer_lookup";
};

opt::OptimizerStatsIdentity Identity(const FixtureIds& ids,
                                     std::string statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = ids.relation_uuid;
  identity.statistic_uuid = std::move(statistic_uuid);
  identity.stats_epoch = 4101;
  identity.catalog_epoch = 4100;
  identity.transaction_visibility_epoch = 4099;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats(const FixtureIds& ids) {
  opt::TableCardinalityStats stats;
  stats.identity = Identity(ids, UuidText(platform::UuidKind::object,
                                          1771400010000ull,
                                          0x51));
  stats.row_count = 10000;
  stats.visible_row_count = 9900;
  stats.page_count = 96;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats IndexStats(const FixtureIds& ids) {
  opt::IndexStats index;
  index.identity = Identity(ids, UuidText(platform::UuidKind::object,
                                          1771400011000ull,
                                          0x52));
  index.index_uuid = ids.index_uuid;
  index.relation_uuid = ids.relation_uuid;
  index.index_family = "btree";
  index.descriptor_digest = ids.descriptor_digest;
  index.collation_identity = Sha("collation-binary");
  index.key_column_uuids = {ids.column_uuid};
  index.height = 2;
  index.leaf_pages = 24;
  index.distinct_keys = 9000;
  index.visibility_coverage = 1.0;
  index.equality_lookup_supported = true;
  index.ordered_range_supported = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

plan::OptimizerPolicyMetadata SafePolicy() {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 4104;
  policy.normalized_controls.plan_profile_id = "eler041.profile.catalog";
  policy.normalized_controls.join_search_policy_id =
      "eler041.join_search.bounded";
  policy.normalized_controls.memory_policy_id = "eler041.memory.governed";
  policy.normalized_controls.spill_policy_id = "eler041.spill.bounded";
  policy.normalized_controls.parallelism_policy_id =
      "eler041.parallelism.single_node";
  policy.normalized_controls.what_if_policy_id = "eler041.what_if.disabled";
  policy.normalized_controls.safe_control_ids = {
      "catalog_stats:required",
      "mga_recheck:preserved",
      "physical_plan:required"};
  policy.safe_control_ids = {
      "security_recheck:preserved",
      "redaction_digest:required"};
  return policy;
}

plan::LogicalPlan LogicalPlan(const FixtureIds& ids) {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "eler041.logical_plan.customer_lookup";
  logical.optimizer_policy = SafePolicy();
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kNone,
                                        ids.operation_id,
                                        "engine.customer_lookup");
  node.required_object_uuids.push_back(ids.relation_uuid);
  node.required_descriptors.push_back(ids.descriptor_digest);
  logical.nodes.push_back(std::move(node));
  return logical;
}

opt::AccessPathPlanningRequest AccessRequest(const FixtureIds& ids) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = ids.relation_uuid;
  request.predicate_kind = "scalar_eq";
  request.descriptor_digest = ids.descriptor_digest;
  request.collation_identity = Sha("collation-binary");
  request.projected_column_uuids = {ids.column_uuid};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.table_stats = TableStats(ids);
  request.candidate_indexes = {IndexStats(ids)};
  return request;
}

opt::BoundOptimizerRequest BoundRequest(const FixtureIds& ids) {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid =
      UuidText(platform::UuidKind::object, 1771400012000ull, 0x53);
  request.context.operation_id = ids.operation_id;
  request.context.sblr_digest = ids.sblr_digest;
  request.context.descriptor_set_digest = ids.descriptor_digest;
  request.context.statistics_snapshot_id = Sha("catalog-stats-snapshot");
  request.context.metric_snapshot_id = Sha("runtime-metrics-snapshot");
  request.context.executor_capability_set_id =
      "executor.capability.scalar_btree.lookup";
  request.context.catalog_epoch = 4100;
  request.context.stats_epoch = 4101;
  request.context.security_epoch = 4102;
  request.context.redaction_epoch = 4103;
  request.context.policy_epoch = 4104;
  request.context.resource_epoch = 4105;
  request.context.name_resolution_epoch = 4106;
  request.context.memory_policy_epoch = 4107;
  request.context.memory_feedback_generation = 4108;
  request.context.route_epoch = 4109;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = LogicalPlan(ids);
  request.catalog_access_path_request = AccessRequest(ids);
  return request;
}

opt::OptimizerProductionPlanCacheKeyRequest ProductionKeyRequest(
    const FixtureIds& ids,
    const opt::BoundOptimizerRequest& bound_request) {
  opt::OptimizerProductionPlanCacheKeyRequest request;
  request.bound_request = bound_request;
  request.catalog_stats_digest = Sha("catalog-stats-digest");
  request.cost_profile_id = "cost_profile.eler041.catalog";
  request.route_capability_digest = Sha("route-capability-local-index");
  request.security_policy_digest = Sha("security-policy-reader");
  request.redaction_route_digest = Sha("redaction-route-customer");
  request.parameter_shape_digest = Sha("parameter-shape-customer-id");
  request.memory_grant_class = "memory_grant_class.eler041.small";
  request.memory_grant_digest = Sha("memory-grant-small");
  request.compatibility_epoch = 4110;
  request.format_compatibility_epoch = 4111;
  request.object_uuids = {ids.relation_uuid};
  request.function_uuids = {ids.function_uuid};
  request.index_uuids = {ids.index_uuid};
  request.filespace_uuids = {ids.filespace_uuid};
  request.dependency_digests = {
      Sha("dep-relation"),
      Sha("dep-index"),
      Sha("dep-function"),
      Sha("dep-filespace"),
      Sha("dep-route"),
      Sha("dep-memory-feedback")};
  return request;
}

std::vector<platform::byte> EncodedKey(const std::string& index_uuid,
                                       std::string_view key) {
  const auto descriptor_uuid =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(descriptor_uuid.ok(), "index UUID parse failed");
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor_uuid.value;
  component.payload.assign(key.begin(), key.end());
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "test key encoding failed");
  return encoded.encoded;
}

page::IndexBtreeCell Cell(const FixtureIds& ids, std::string_view key) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(ids.index_uuid, key);
  const auto parsed_row =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           ids.row_uuid);
  const auto parsed_version =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           ids.version_uuid);
  Require(parsed_row.ok() && parsed_version.ok(),
          "row/version UUID parse failed");
  cell.row_uuid = parsed_row.value;
  cell.version_uuid = parsed_version.value;
  return cell;
}

page::IndexBtreePhysicalTree PhysicalTreeWithRow(const FixtureIds& ids,
                                                 std::string_view key) {
  const auto parsed_index =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           ids.index_uuid);
  Require(parsed_index.ok(), "physical index UUID parse failed");
  auto initialized = page::InitializeIndexBtreePhysicalTree(parsed_index.value,
                                                           4096);
  Require(initialized.ok(), "physical index tree initialization failed");
  page::IndexBtreePhysicalInsertRequest insert;
  insert.cell = Cell(ids, key);
  const auto inserted =
      page::InsertIndexBtreeCell(&initialized.tree, insert);
  Require(inserted.ok(), "physical index seed insert failed");
  return std::move(initialized.tree);
}

api::DmlTargetAccessPlanRequest DmlAccessRequest(const FixtureIds& ids,
                                                 const opt::PhysicalPlanNode& physical) {
  api::DmlTargetAccessPlanRequest request;
  request.mutation_kind = "dml.target_rows.update";
  request.database_uuid = ids.database_uuid;
  request.relation_uuid = ids.relation_uuid;
  request.predicate_kind = "scalar_eq";
  request.predicate_descriptor_digest = ids.descriptor_digest;
  request.index_uuid = ids.index_uuid;
  request.index_family = "btree";
  request.security_policy_digest = Sha("security-policy-reader");
  request.redaction_policy_digest = Sha("redaction-route-customer");
  request.access_policy_digest = Sha("access-policy-customer");
  request.collation_profile_digest = Sha("collation-binary");
  request.access_descriptor_present = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.grants_proven = true;
  request.security_context_present = true;
  request.parser_or_reference_authority = false;
  request.observed_catalog_epoch = 4100;
  request.current_catalog_epoch = 4100;
  request.observed_security_epoch = 4102;
  request.current_security_epoch = 4102;
  request.observed_policy_epoch = 4104;
  request.current_policy_epoch = 4104;
  request.observed_stats_epoch = 4101;
  request.current_stats_epoch = 4101;
  request.index_epoch = 4109;
  request.object_epoch = 4100;
  request.compatibility_epoch = 4110;
  request.local_transaction_id = 4100001;
  request.estimated_rows = physical.estimated_rows == 0
                               ? 1
                               : physical.estimated_rows;
  return request;
}

api::DmlRowLocatorStreamResult ConsumePhysicalLocator(
    const FixtureIds& ids,
    const api::DmlTargetAccessPlan& access_plan,
    const page::IndexBtreePhysicalTree& tree,
    std::string_view key) {
  api::DmlRowLocatorStreamRequest request;
  request.consumer = api::DmlRowLocatorStreamConsumer::update;
  request.access_plan = access_plan;
  request.access_plan_engine_authority_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.parser_or_reference_authority = false;
  request.index_or_cache_finality_authority = false;
  request.index_unique = false;
  request.physical_tree = &tree;
  request.encoded_point_key = EncodedKey(ids.index_uuid, key);
  return api::BuildDmlRowLocatorStream(request);
}

opt::OptimizerResultOracleEvidence OracleSide(std::string producer,
                                              const std::string& route_hash) {
  opt::OptimizerResultOracleEvidence result;
  result.producer_label = std::move(producer);
  result.result_contract_hash = Sha("oracle-result-contract");
  result.result_hash = Sha("oracle-result-rowset");
  result.result_row_count = 1;
  result.result_row_count_observed = true;
  result.ordering_contract_hash = Sha("oracle-ordering");
  result.null_semantics_hash = Sha("oracle-null-semantics");
  result.error_diagnostic_code = "SB_OK";
  result.diagnostic_digest = Sha("oracle-diagnostics");
  result.row_locator_contract_hash = route_hash;
  result.recheck_evidence_digest = Sha("oracle-recheck");
  result.accepted = true;
  result.live_route_executed = true;
  result.synthetic_result = false;
  result.exact_recheck_proven = true;
  result.mga_recheck_proven = true;
  result.security_recheck_proven = true;
  result.row_locator_mga_snapshot_proven = true;
  return result;
}

opt::OptimizerCorrectnessOracleCase OracleCase(
    const FixtureIds& ids,
    const std::string& physical_plan_hash,
    const std::string& route_hash) {
  opt::OptimizerCorrectnessOracleCase oracle_case;
  oracle_case.case_id = "eler041-dml-locator-integrated-route";
  oracle_case.correctness_class = opt::OptimizerCorrectnessClass::kDmlLocator;
  oracle_case.route_kind = "embedded";
  oracle_case.route_label = "eler041/optimizer/executor/dml_locator";
  oracle_case.dataset_schema_digest = Sha("dataset-schema");
  oracle_case.sblr_digest = ids.sblr_digest;
  oracle_case.logical_plan_hash = Sha("logical-plan-customer-lookup");
  oracle_case.baseline_plan_hash = Sha("baseline-row-uuid-plan");
  oracle_case.optimized_plan_hash = physical_plan_hash;
  oracle_case.equivalence_contract_hash = Sha("equivalence-contract");
  oracle_case.catalog_epoch = 4100;
  oracle_case.security_epoch = 4102;
  oracle_case.redaction_epoch = 4103;
  oracle_case.statistics_epoch = 4101;
  oracle_case.provider_generation = 4109;
  oracle_case.baseline = OracleSide("engine_baseline_row_uuid_route",
                                    route_hash);
  oracle_case.optimized = OracleSide("optimized_physical_btree_route",
                                     route_hash);
  oracle_case.production_correctness_claim = true;
  oracle_case.evidence_only = true;
  oracle_case.baseline_is_engine_reference_route = true;
  oracle_case.optimized_route_consumed = true;
  oracle_case.exact_recheck_required = true;
  oracle_case.mga_recheck_required = true;
  oracle_case.security_recheck_required = true;
  oracle_case.reference_reference_only = true;
  return oracle_case;
}

opt::OptimizerRuntimeFeedback RuntimeFeedback(
    const opt::CatalogBackedProductionPlanningResult& result) {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "physical_btree_point_lookup";
  feedback.plan_shape = result.optimized_plan.physical_root.node_id;
  feedback.cost_profile_id = "cost_profile.eler041.catalog";
  feedback.estimated_rows = 1;
  feedback.actual_rows = 4;
  feedback.actual_rows_examined = 4;
  feedback.actual_rows_filtered = 3;
  feedback.loop_count = 1;
  feedback.estimated_pages = 1;
  feedback.actual_pages = 2;
  feedback.estimated_io_operations = 1;
  feedback.actual_io_operations = 2;
  feedback.estimated_visibility_recheck_rows = 1;
  feedback.actual_visibility_recheck_rows = 1;
  feedback.estimated_spill_bytes = 0;
  feedback.actual_spill_bytes = 4096;
  feedback.memory_grant_bytes = 4096;
  feedback.peak_memory_bytes = 8192;
  feedback.estimated_latency_microseconds = 100;
  feedback.actual_latency_microseconds = 450;
  feedback.estimated_resource_units = 10;
  feedback.actual_resource_units = 40;
  feedback.freshness_microseconds = 1000;
  feedback.policy_allowed = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.parser_or_reference_authority = false;
  return feedback;
}

void ProveFailClosedAuthorityBoundaries(
    const FixtureIds& ids,
    const opt::BoundOptimizerRequest& bound_request,
    const opt::OptimizerProductionPlanCacheKeyRequest& key_request,
    const api::DmlTargetAccessPlan& access_plan,
    const page::IndexBtreePhysicalTree& tree) {
  auto parser_bound = bound_request;
  parser_bound.context.parser_owned_claims_present = true;
  auto parser_key_request = key_request;
  parser_key_request.bound_request = parser_bound;
  parser_key_request.parser_or_reference_authority_claimed = true;
  const auto parser_key =
      opt::BuildProductionOptimizerPlanCacheKeyInput(parser_key_request);
  Require(!parser_key.ok &&
              Contains(parser_key.evidence,
                       "production_plan_cache_parser_or_reference_authority_refused"),
          "parser authority was admitted to production plan cache key");

  auto placeholder_key_request = key_request;
  placeholder_key_request.memory_grant_digest = "memory:default";
  const auto placeholder_key =
      opt::BuildProductionOptimizerPlanCacheKeyInput(placeholder_key_request);
  Require(!placeholder_key.ok &&
              ContainsText(placeholder_key.evidence,
                           "enterprise_plan_cache_key_missing_or_placeholder"),
          "placeholder production plan cache key was admitted");

  auto cluster_key_request = key_request;
  cluster_key_request.cluster_route_requested = true;
  const auto cluster_key =
      opt::BuildProductionOptimizerPlanCacheKeyInput(cluster_key_request);
  Require(!cluster_key.ok &&
              Contains(cluster_key.evidence,
                       "production_plan_cache_cluster_route_requires_external_provider"),
          "cluster route was admitted without external provider");

  auto unsafe_access = DmlAccessRequest(ids,
                                       opt::PhysicalPlanNode{});
  unsafe_access.parser_or_reference_authority = true;
  const auto refused_access = api::BuildDmlTargetAccessPlan(unsafe_access);
  Require(!refused_access.ok &&
              Contains(refused_access.diagnostics,
                       "unsafe parser/reference authority"),
          "DML access plan admitted parser/reference authority");

  auto unsafe_stream = api::DmlRowLocatorStreamRequest{};
  unsafe_stream.consumer = api::DmlRowLocatorStreamConsumer::update;
  unsafe_stream.access_plan = access_plan;
  unsafe_stream.access_plan_engine_authority_proof = true;
  unsafe_stream.durable_mga_inventory_proof = true;
  unsafe_stream.parser_or_reference_authority = true;
  unsafe_stream.physical_tree = &tree;
  unsafe_stream.encoded_point_key = EncodedKey(ids.index_uuid, "customer-42");
  const auto refused_stream = api::BuildDmlRowLocatorStream(unsafe_stream);
  Require(!refused_stream.ok &&
              HasEvidence(refused_stream.evidence,
                          "dml_row_locator_stream_refusal",
                          "parser_or_reference_authority_forbidden"),
          "row locator stream admitted parser/reference authority");
}

std::string Csv(std::string_view value) {
  if (value.find_first_of(",\"\n") == std::string_view::npos) {
    return std::string(value);
  }
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out += ch;
    }
  }
  out += '"';
  return out;
}

void WriteMatrix(const std::filesystem::path& path,
                 const std::vector<std::vector<std::string>>& rows) {
  if (path.empty()) { return; }
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "could not open ELER-041 matrix");
  out << "proof,status,evidence\n";
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i != 0) { out << ','; }
      out << Csv(row[i]);
    }
    out << '\n';
  }
  out.close();
  Require(static_cast<bool>(out), "could not write ELER-041 matrix");
}

}  // namespace

int main(int argc, char** argv) {
  const FixtureIds ids;
  const auto bound_request = BoundRequest(ids);
  const auto key_request = ProductionKeyRequest(ids, bound_request);
  const auto key_result =
      opt::BuildProductionOptimizerPlanCacheKeyInput(key_request);
  Require(key_result.ok, "production plan-cache key was refused");
  Require(Contains(key_result.evidence,
                   "production_plan_cache_parser_or_reference_authority=false"),
          "production plan-cache key missing authority evidence");

  opt::CatalogBackedProductionPlanningRequest production;
  production.bound_request = bound_request;
  production.plan_cache_key_input = key_result.input;
  production.production_build = true;
  production.require_index_stats = true;
  const auto result = opt::OptimizeCatalogBackedProductionPlan(production);
  Require(result.validation.ok && result.validation.catalog_backed &&
              result.validation.benchmark_clean_ready,
          "catalog-backed production optimizer route was not admitted");
  Require(result.bound_result.ok && result.optimized_plan.ok &&
              result.optimized_plan.has_physical_plan,
          "optimized production plan missing physical route");

  const auto physical_validation =
      opt::ValidatePhysicalPlanNode(result.optimized_plan.physical_root);
  Require(physical_validation.ok, "physical optimizer plan validation failed");
  Require(result.optimized_plan.physical_root.storage_backed,
          "optimizer selected route is not storage backed");
  Require(result.optimized_plan.physical_root.access_kind ==
              plan::PhysicalAccessKind::kScalarBtreeLookup,
          std::string("optimizer did not select scalar B-tree lookup route: ") +
              plan::PhysicalAccessKindName(
                  result.optimized_plan.physical_root.access_kind));

  auto tree = PhysicalTreeWithRow(ids, "customer-42");
  const auto access_request =
      DmlAccessRequest(ids, result.optimized_plan.physical_root);
  const auto access_plan = api::BuildDmlTargetAccessPlan(access_request);
  Require(access_plan.ok &&
              access_plan.access_kind ==
                  api::DmlTargetAccessKind::nonunique_index_lookup,
          "DML access plan did not admit optimizer-selected index lookup");
  Require(access_plan.executor_capability ==
              result.optimized_plan.physical_root.executor_capability_id,
          "DML access plan executor capability drifted from optimizer plan");

  const auto stream =
      ConsumePhysicalLocator(ids, access_plan, tree, "customer-42");
  Require(stream.ok &&
              stream.source ==
                  api::DmlRowLocatorStreamSource::physical_btree_point,
          "executor row-locator stream did not consume physical B-tree route");
  Require(stream.locators.size() == 1 &&
              stream.locators.front().row_uuid == ids.row_uuid,
          "executor row-locator stream returned wrong row");
  Require(HasEvidence(stream.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "row-locator stream missing MGA finality authority evidence");
  Require(HasEvidence(stream.evidence,
                      "parser_or_reference_authority",
                      "false"),
          "row-locator stream missing parser/reference refusal evidence");

  opt::CachedOptimizerPlan cached;
  cached.key_input = key_result.input;
  cached.cache_key = opt::BuildOptimizerPlanCacheKey(cached.key_input);
  cached.result = result.bound_result;
  cached.created_epoch = key_result.input.catalog_epoch;
  cached.metadata_only = true;
  cached.mga_visibility_recheck_required = true;
  cached.security_recheck_required = true;
  cached.parser_or_reference_finality_authority = false;
  opt::OptimizerPlanCache cache;
  const auto put = cache.PutEnterprise(cached);
  Require(put.ok &&
              Contains(put.evidence,
                       "enterprise_plan_cache_plan_safe_for_reuse=true"),
          std::string("enterprise plan-cache put was refused: ") +
              put.diagnostic_code + " evidence=" +
              (put.evidence.empty() ? std::string("<none>")
                                    : put.evidence.front()));
  const auto lookup = cache.LookupEnterprise(key_result.input);
  Require(lookup.hit &&
              Contains(lookup.evidence, "OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE") &&
              Contains(lookup.evidence, "mga_visibility_recheck=preserved") &&
              Contains(lookup.evidence,
                       "security_authorization_recheck=preserved"),
          "enterprise plan-cache lookup did not preserve route evidence");

  const std::string physical_json =
      opt::SerializePhysicalPlanNodeToJson(result.optimized_plan.physical_root);
  const std::string physical_plan_hash =
      Sha("physical-plan-" + result.optimized_plan.physical_root.node_id);
  const std::string route_hash = Sha("row-locator-" + ids.row_uuid);
  Require(physical_json.find(result.optimized_plan.physical_root.node_id) !=
              std::string::npos,
          "serialized physical plan omitted selected node");
  const auto oracle =
      opt::ValidateOptimizerCorrectnessOracleCase(
          OracleCase(ids, physical_plan_hash, route_hash));
  Require(oracle.ok && oracle.correctness_proven,
          "DML locator correctness oracle did not validate live route");

  const auto feedback = RuntimeFeedback(result);
  const auto feedback_status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
  Require(feedback_status.ok && feedback_status.applied &&
              Contains(feedback_status.evidence,
                       "feedback_advisory_only=true") &&
              Contains(feedback_status.evidence,
                       "mga_finality_authority=engine_transaction_inventory"),
          "runtime feedback was not accepted as advisory evidence");

  opt::OptimizerFeedbackStabilityRequest stability_request;
  stability_request.observations = {
      {"eler041.baseline",
       Sha("baseline-plan"),
       Sha("oracle-result-rowset"),
       cached.cache_key,
       "SB_OPTIMIZER_PLAN_CACHE_MISS",
       4107,
       4108,
       4101,
       4100,
       true,
       true,
       true},
      {"eler041.optimized",
       physical_plan_hash,
       Sha("oracle-result-rowset"),
       cached.cache_key,
       lookup.diagnostic_code,
       4109,
       4110,
       4101,
       4100,
       true,
       true,
       true}};
  stability_request.latest_feedback_generation = 4109;
  stability_request.latest_memory_feedback_generation = 4110;
  stability_request.latest_stats_epoch = 4101;
  stability_request.latest_catalog_epoch = 4100;
  stability_request.max_plan_switches = 1;
  stability_request.require_benchmark_clean = true;
  const auto stability =
      opt::EvaluateOptimizerFeedbackStability(stability_request);
  Require(stability.accepted && stability.stable &&
              stability.benchmark_clean_permitted &&
              Contains(stability.evidence,
                       "feedback_stability.parser_or_reference_authority=false"),
          "feedback stability did not preserve route correctness contract");

  const auto explain =
      opt::BuildOptimizerExplainDocument(bound_request, result.bound_result);
  Require(!explain.plan_hash.empty() &&
              !explain.selected_candidate_id.empty() &&
              ContainsText(explain.invalidation_dependencies,
                           "memory_feedback_generation=4108") &&
              ContainsText(explain.invalidation_dependencies,
                           "stats_epoch=4101"),
          "optimizer explain lineage is incomplete");
  const auto explain_json = opt::RenderOptimizerExplainJson(explain);
  Require(explain_json.find("\"plan_hash\"") != std::string::npos &&
              explain_json.find("\"executor_capability_evidence\"") !=
                  std::string::npos,
          "rendered explain JSON omitted route lineage");

  ProveFailClosedAuthorityBoundaries(ids,
                                     bound_request,
                                     key_request,
                                     access_plan,
                                     tree);

  std::vector<std::vector<std::string>> matrix = {
      {"production_plan_cache_key", "complete", key_result.diagnostic_code},
      {"catalog_backed_optimizer_route", "complete",
       result.validation.diagnostic_code},
      {"physical_plan_validation", "complete",
       result.optimized_plan.physical_root.executor_capability_id},
      {"executor_row_locator_consumption", "complete",
       api::DmlRowLocatorStreamSourceName(stream.source)},
      {"enterprise_plan_cache_reuse", "complete", lookup.diagnostic_code},
      {"correctness_oracle", "complete", oracle.diagnostic_code},
      {"feedback_and_explain_lineage", "complete",
       feedback_status.diagnostic_code},
      {"fail_closed_authority_boundaries", "complete",
       "parser_reference_placeholder_cluster_refused"}};
  if (argc == 2) { WriteMatrix(argv[1], matrix); }

  std::cout << "engine_listener_optimizer_integrated_route_conformance=passed"
            << " cache_key=" << cached.cache_key
            << " plan_node=" << result.optimized_plan.physical_root.node_id
            << " locators=" << stream.locators.size() << '\n';
  return EXIT_SUCCESS;
}
