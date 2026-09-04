// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_coordinator.hpp"
#include "model_family_executor.hpp"
#include "canonical_aggregate_registry.hpp"
#include "canonical_relational_expression.hpp"
#include "datatype_operations.hpp"
#include "datatype_temporal_wire.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/expression_api.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace optimizer = scratchbird::engine::optimizer;
namespace executor = scratchbird::engine::executor;
namespace planner = scratchbird::engine::planner;
namespace sblr = scratchbird::engine::sblr;
namespace datatypes = scratchbird::core::datatypes;

namespace {

constexpr std::string_view kCanonicalInt64TypeUuid =
    "019d0000-0000-7000-8000-00000000d712";

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "019f0000-0000-7000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-CES05-CROSS-FAMILY: " << detail << '\n';
  }
  return condition;
}

executor::PhysicalMgaStatementContext Mga() {
  executor::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(1);
  context.statement_timestamp = "2026-08-11T20:00:00Z";
  context.owning_transaction_uuid = Uuid(2);
  context.statement_snapshot_uuid = Uuid(3);
  context.statement_metadata_snapshot_uuid = Uuid(4);
  context.owning_local_transaction_id = 40;
  context.visible_committed_high_watermark = 39;
  context.oldest_active_transaction_id = 30;
  context.oldest_interesting_transaction_id = 29;
  context.oldest_snapshot_transaction_id = 29;
  context.retention_horizon_transaction_id = 29;
  context.active_excluded_local_transaction_ids = {40};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 41;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

executor::CanonicalExecutionMgaAuthority Authority(
    const executor::TypedPhysicalNodeDag& dag) {
  executor::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin =
      executor::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    executor::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

executor::TypedPhysicalNodeDag DirectDag(
    const executor::PhysicalNodeKind root_kind,
    const std::vector<std::uint32_t>& left_descriptors,
    const std::vector<std::uint32_t>& right_descriptors = {}) {
  executor::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = Uuid(6000 + static_cast<unsigned>(root_kind));
  dag.root_physical_node_id = right_descriptors.empty() ? 2 : 3;
  dag.local_transaction_id = 40;
  dag.statement_snapshot_id = 39;
  dag.mga_statement_context = Mga();
  dag.bound_sblr_tree_uuid = Uuid(6010);
  dag.catalog_epoch_uuid = Uuid(6011);
  dag.security_context_uuid = Uuid(6012);
  dag.capability_snapshot_uuid = Uuid(6013);
  dag.resource_snapshot_uuid = Uuid(6014);
  dag.statistics_snapshot_uuid = Uuid(6015);
  dag.route_snapshot_uuid = Uuid(6016);
  dag.admission_evidence = {
      {executor::PhysicalAdmissionStage::kBoundRequest,
       dag.bound_sblr_tree_uuid},
      {executor::PhysicalAdmissionStage::kCatalogEpoch,
       dag.catalog_epoch_uuid},
      {executor::PhysicalAdmissionStage::kSecurity,
       dag.security_context_uuid},
      {executor::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.mga_statement_context.statement_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kResource,
       dag.resource_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kCanonicalRoute,
       dag.route_snapshot_uuid},
  };
  executor::PhysicalNodeRecord left;
  left.physical_node_id = 1;
  left.relational_node_id = 1;
  left.node_kind = executor::PhysicalNodeKind::kValues;
  left.implementation_id = "values.model-leg.typed.v1";
  left.output_descriptor_ids = left_descriptors;
  left.causal_counter_id = 1;
  dag.nodes.push_back(std::move(left));
  std::vector<std::uint32_t> output = left_descriptors;
  if (!right_descriptors.empty()) {
    executor::PhysicalNodeRecord right;
    right.physical_node_id = 2;
    right.relational_node_id = 2;
    right.node_kind = executor::PhysicalNodeKind::kValues;
    right.implementation_id = "values.model-leg.typed.v1";
    right.output_descriptor_ids = right_descriptors;
    right.causal_counter_id = 2;
    dag.nodes.push_back(std::move(right));
    output.insert(output.end(), right_descriptors.begin(),
                  right_descriptors.end());
  }
  executor::PhysicalNodeRecord root;
  root.physical_node_id = dag.root_physical_node_id;
  root.relational_node_id = static_cast<std::uint32_t>(dag.root_physical_node_id);
  root.node_kind = root_kind;
  root.implementation_id =
      root_kind == executor::PhysicalNodeKind::kJoin
          ? "join.nested-loop.inner.typed.v1"
          : root_kind == executor::PhysicalNodeKind::kProject
                ? "project.descriptor-direct.v1"
          : root_kind == executor::PhysicalNodeKind::kSubquery
                ? "subquery.table.materialize.typed.v1"
          : root_kind == executor::PhysicalNodeKind::kAggregate
                ? "aggregate.registry-core.v1"
          : root_kind == executor::PhysicalNodeKind::kSort
                ? "sort.typed.terms.v1"
          : root_kind == executor::PhysicalNodeKind::kWindow
                ? "window.partition-order-peer.v1"
          : root_kind == executor::PhysicalNodeKind::kLimit
                ? "limit.typed.v1"
          : root_kind == executor::PhysicalNodeKind::kRecursiveCte
                      ? "cte.recursive.working.typed.v1"
                      : "canonical.consumer.typed.v1";
  root.input_physical_node_ids = right_descriptors.empty()
                                     ? std::vector<std::uint64_t>{1}
                                     : std::vector<std::uint64_t>{1, 2};
  root.output_descriptor_ids = output;
  root.causal_counter_id = dag.root_physical_node_id;
  dag.nodes.push_back(std::move(root));
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 1024 * 1024;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  for (auto& node : dag.nodes) {
    node.selected_alternative_uuid = Uuid(6100 + node.physical_node_id);
    node.executor_capability_uuid = Uuid(6200 + node.physical_node_id);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = Uuid(6300 + node.physical_node_id);
    node.memory_bytes_required = 4096;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag.mga_statement_context;
  }
  return dag;
}

executor::DescriptorBatch DirectLegBatch(const std::uint32_t descriptor_id,
                                         const std::uint64_t identity) {
  auto descriptor = executor::MakeExecutorDescriptor(
      "uuid", "canonical=uuid;type_uuid=" + Uuid(6400) +
                  ";nullable=false");
  descriptor.descriptor_uuid.canonical = Uuid(6500 + identity);
  descriptor.descriptor_kind = "scalar";
  auto value = executor::MakeExecutorValue(descriptor, Uuid(6600 + identity));
  return executor::MakeDescriptorBatch(
      {{"family_" + std::to_string(identity), descriptor, false,
        descriptor_id}},
      {{{std::move(value)}}});
}

enum class DirectLegDisposition : std::uint8_t {
  kSuccess = 1,
  kEmpty,
  kCancelAfterProvider,
  kInjectedFailure,
};

struct DirectLegExecution {
  executor::ModelFamilyExecutionResultV1 result;
  std::string family_id;
  std::size_t provider_call_count{0};
  std::size_t cancellation_probe_count{0};
  std::size_t cleanup_callback_count{0};
  bool expected_mga_context_bound{false};
  bool provider_authority_absent{false};
};

DirectLegExecution ExecuteDirectLeg(const std::size_t ordinal,
                                    const std::string_view family,
                                    const DirectLegDisposition disposition) {
  DirectLegExecution observed;
  observed.family_id = family;
  const auto first_descriptor_id =
      static_cast<std::uint32_t>(1000 + ordinal * 16);
  const auto descriptor = [&](const std::string& name,
                              const std::string& type,
                              const std::uint32_t descriptor_id,
                              const std::string& value) {
    auto engine_descriptor = executor::MakeExecutorDescriptor(
        type, "canonical=" + type + ";type_uuid=" +
                  Uuid(9000 + descriptor_id) + ";nullable=false");
    engine_descriptor.descriptor_uuid.canonical =
        Uuid(10'000 + descriptor_id);
    engine_descriptor.descriptor_kind = "scalar";
    return std::pair{
        executor::ExecutorColumnDescriptor{name, engine_descriptor, false,
                                           descriptor_id},
        executor::MakeExecutorValue(engine_descriptor, value)};
  };
  std::vector<std::pair<executor::ExecutorColumnDescriptor,
                        api::EngineTypedValue>> cells;
  if (family == "key_value") {
    cells = {descriptor("row_uuid", "uuid", first_descriptor_id,
                        Uuid(11'000 + ordinal)),
             descriptor("key", "text", first_descriptor_id + 1,
                        "key-" + std::to_string(ordinal)),
             descriptor("value", "text", first_descriptor_id + 2,
                        "value-" + std::to_string(ordinal))};
  } else if (family == "time_series") {
    cells = {descriptor("bucket_start", "timestamp_tz", first_descriptor_id,
                        "1970-01-01T00:00:01.000000000Z")};
  } else if (family == "vector") {
    cells = {descriptor("row_uuid", "uuid", first_descriptor_id,
                        Uuid(11'000 + ordinal)),
             descriptor("distance", "real64", first_descriptor_id + 1, "1"),
             descriptor("score", "real64", first_descriptor_id + 2, "1")};
  } else if (family == "search") {
    cells = {descriptor("document_uuid", "uuid", first_descriptor_id,
                        Uuid(11'000 + ordinal)),
             descriptor("analyzer_uuid", "uuid", first_descriptor_id + 1,
                        Uuid(12'000 + ordinal)),
             descriptor("analyzer_generation", "uint64",
                        first_descriptor_id + 2, "1"),
             descriptor("score", "real64", first_descriptor_id + 3, "1"),
             descriptor("rank", "uint64", first_descriptor_id + 4, "1")};
  } else if (family == "spatial") {
    cells = {descriptor("row_uuid", "uuid", first_descriptor_id,
                        Uuid(11'000 + ordinal)),
             descriptor("spatial_value", "geometry", first_descriptor_id + 1,
                        "POINT(0 0)"),
             descriptor("crs_uuid", "uuid", first_descriptor_id + 2,
                        Uuid(12'000 + ordinal))};
    cells[1].second.encoded_value.clear();
    cells[1].second.binary_value = {1};
  } else {
    cells = {descriptor("family_" + std::string(family), "uuid",
                        first_descriptor_id, Uuid(11'000 + ordinal))};
  }
  executor::DescriptorBatch batch;
  executor::DescriptorTuple row;
  for (auto& [column, value] : cells) {
    batch.columns.push_back(std::move(column));
    row.values.push_back(std::move(value));
  }
  batch.rows.push_back(std::move(row));

  // Relational is already an engine-owned VALUES leg, not a model provider.
  // Its typed batch validation is the actual execution boundary for this
  // composition proof.
  if (family == "relational") {
    observed.provider_call_count = 1;
    observed.cancellation_probe_count = 1;
    observed.result.execution_started = true;
    if (disposition == DirectLegDisposition::kCancelAfterProvider) {
      observed.result.diagnostic_id = "SB_MODEL_EXECUTION_CANCELLED_V1";
    } else if (disposition == DirectLegDisposition::kInjectedFailure) {
      observed.result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
    } else {
      if (disposition == DirectLegDisposition::kEmpty) batch.rows.clear();
      observed.result.accepted = executor::ValidateDescriptorBatch(batch).ok;
      observed.result.root_published = observed.result.accepted;
      observed.result.output.family_id = "relational";
      observed.result.output.mga_statement_context = Mga();
      observed.result.output.batch = std::move(batch);
    }
    observed.expected_mga_context_bound =
        executor::PhysicalMgaStatementContextEqual(
            observed.result.output.mga_statement_context, Mga());
    observed.provider_authority_absent = true;
    ++observed.cleanup_callback_count;
    observed.result.cleanup_count = 1;
    observed.result.cleanup_complete = true;
    return observed;
  }

  executor::ModelFamilyExecutionRequestV1 request;
  request.input.family_id = family;
  request.input.operation_id =
      family == "document"   ? "DOCUMENT_FIND"
      : family == "graph"    ? "GRAPH_MATCH"
      : family == "key_value" ? "KEY_VALUE_GET"
      : family == "time_series" ? "TIME_SERIES_BUCKET"
      : family == "vector"   ? "VECTOR_EXACT_SEARCH"
      : family == "search"   ? "SEARCH_RANKED_QUERY"
      : family == "spatial"  ? "SPATIAL_SOURCE"
                                : "COLUMNAR_SOURCE";
  if (family == "spatial" || family == "columnar") {
    request.input.operation_ids = {request.input.operation_id};
  }
  request.input.object_uuid = Uuid(8000 + ordinal);
  request.input.physical_node_id = ordinal + 1;
  request.input.selected_alternative_uuid = Uuid(8100 + ordinal);
  request.input.capability_uuid = Uuid(8200 + ordinal);
  request.input.provider_uuid = Uuid(8300 + ordinal);
  request.input.provider_generation = 1;
  request.input.result_handle_uuid = Uuid(8400 + ordinal);
  request.input.causal_counter_id = ordinal + 1;
  for (const auto& column : batch.columns) {
    request.input.output_descriptor_ids.push_back(column.descriptor_id);
  }
  request.input.mga_statement_context = Mga();
  if (family == "document" || family == "graph") {
    request.input.mga_statement_context.statement_timestamp.clear();
  }
  if (family == "spatial") {
    request.input.spatial_geometry_descriptor_uuid =
        batch.columns[1].descriptor.descriptor_uuid.canonical;
    request.input.spatial_geometry_type_uuid = Uuid(12'500 + ordinal);
    request.input.spatial_crs_uuid = Uuid(12'000 + ordinal);
    request.input.spatial_crs_generation = 1;
  }
  request.input.catalog_epoch_uuid = Uuid(8500 + ordinal);
  request.input.security_context_uuid = Uuid(8600 + ordinal);
  request.input.policy_snapshot_uuid = Uuid(8700 + ordinal);
  request.input.resource_contract_uuid = Uuid(8800 + ordinal);
  request.input.catalog_generation = 1;
  request.input.descriptor_generation = 1;
  request.input.security_generation = 1;
  request.input.policy_generation = 1;
  request.input.resource_generation = 1;
  request.input.maximum_rows = 16;
  request.input.maximum_cells =
      std::max<std::size_t>(16, batch.columns.size() * 4);
  request.input.maximum_memory_bytes = 1024 * 1024;
  request.capability.capability_uuid = request.input.capability_uuid;
  request.capability.family_id = request.input.family_id;
  request.capability.provider_uuid = request.input.provider_uuid;
  request.capability.provider_generation = request.input.provider_generation;
  request.capability.available = true;
  request.capability.exact = true;
  request.capability.exact_collection_fallback_available = true;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.current_catalog_generation = request.input.catalog_generation;
  request.current_descriptor_generation = request.input.descriptor_generation;
  request.current_security_generation = request.input.security_generation;
  request.current_policy_generation = request.input.policy_generation;
  request.current_resource_generation = request.input.resource_generation;
  request.current_provider_generation = request.input.provider_generation;
  request.current_mga_statement_context = request.input.mga_statement_context;
  auto expected_mga_context = Mga();
  if (family == "document" || family == "graph") {
    expected_mga_context.statement_timestamp.clear();
  }
  observed.expected_mga_context_bound =
      executor::PhysicalMgaStatementContextEqual(
          request.input.mga_statement_context, expected_mga_context) &&
      executor::PhysicalMgaStatementContextEqual(
          request.current_mga_statement_context, expected_mga_context);
  observed.provider_authority_absent =
      !request.input.parser_execution_authority_claimed &&
      !request.input.transaction_finality_authority_claimed &&
      !request.capability.provider_visibility_authority_claimed &&
      !request.capability.provider_finality_authority_claimed;
  request.fault_injected = disposition == DirectLegDisposition::kInjectedFailure;
  request.cancellation_requested = [&observed, disposition] {
    ++observed.cancellation_probe_count;
    return disposition == DirectLegDisposition::kCancelAfterProvider &&
           observed.cancellation_probe_count >= 2;
  };
  request.cleanup_provider = [&observed] {
    ++observed.cleanup_callback_count;
  };
  request.execute_provider = [&observed, batch, disposition,
                              family = std::string(family)](const auto& input) {
    ++observed.provider_call_count;
    executor::ModelProviderExecutionResultV1 provider;
    provider.ok = true;
    provider.data_access_observed = true;
    provider.rows_examined =
        disposition == DirectLegDisposition::kEmpty ? 0 : batch.rows.size();
    provider.provider_batch.provider_uuid = input.provider_uuid;
    provider.provider_batch.provider_generation = input.provider_generation;
    provider.provider_batch.selected_alternative_uuid =
        input.selected_alternative_uuid;
    provider.provider_batch.capability_uuid = input.capability_uuid;
    provider.provider_batch.exact_fallback_selected =
        input.exact_fallback_selected;
    provider.provider_batch.result_handle_uuid = input.result_handle_uuid;
    provider.provider_batch.causal_counter_id = input.causal_counter_id;
    provider.provider_batch.output_descriptor_ids = input.output_descriptor_ids;
    provider.provider_batch.batch = batch;
    if (disposition == DirectLegDisposition::kEmpty) {
      provider.provider_batch.batch.rows.clear();
    } else {
      executor::ModelProviderRowIdentityV1 identity;
      const auto row_uuid = Uuid(11'000 +
                                 (input.physical_node_id - 1));
      if (family == "document") {
        identity.document_uuid =
            provider.provider_batch.batch.rows.front().values.front().encoded_value;
        identity.row_uuid = Uuid(13'000 + input.physical_node_id);
      } else if (family == "graph") {
        identity.row_uuid = row_uuid;
        identity.vertex_uuid = Uuid(14'000 + input.physical_node_id);
        identity.path_uuid = Uuid(15'000 + input.physical_node_id);
      } else if (family == "key_value") {
        identity.row_uuid = row_uuid;
        identity.key = provider.provider_batch.batch.rows.front().values[1]
                           .encoded_value;
      } else if (family == "time_series") {
        identity.row_uuid = row_uuid;
        identity.series_uuid = input.object_uuid;
        identity.metric_uuid = Uuid(16'000 + input.physical_node_id);
        identity.tags = "{}";
        identity.bucket_start_ns = 1'000'000'000;
      } else if (family == "vector") {
        identity.row_uuid = row_uuid;
        identity.vector_distance = "1";
        identity.vector_score = "1";
      } else if (family == "search") {
        identity.document_uuid = row_uuid;
        identity.search_analyzer_uuid = Uuid(12'000 +
                                             (input.physical_node_id - 1));
        identity.search_analyzer_generation = 1;
        identity.search_score = "1";
        identity.search_rank = 1;
      } else {
        identity.row_uuid = row_uuid;
      }
      provider.provider_batch.ordered_row_identities = {std::move(identity)};
    }
    provider.provider_batch.properties.property_uuid = Uuid(8900);
    provider.provider_batch.properties.ordering_id =
        family == "key_value" ? "key_value_unordered_v1"
        : family == "time_series"
            ? "series_metric_timestamp_tags_row_ascending_v1"
        : family == "vector"
            ? "vector_distance_row_uuid_ascending_v1"
        : family == "search"
            ? "search_score_desc_document_uuid_asc_v1"
            : "fixture_order";
    provider.provider_batch.properties.uniqueness_id =
        family == "graph" ? "path_uuid"
        : family == "key_value" ? "key"
        : family == "time_series" || family == "vector" ||
                  family == "spatial" || family == "columnar"
            ? "row_uuid"
        : family == "search" ? "document_uuid"
                              : "document_uuid";
    provider.provider_batch.properties.exact = true;
    provider.provider_batch.properties.residual_recheck_complete = true;
    provider.provider_batch.properties.base_row_mga_recheck_complete = true;
    provider.provider_batch.properties.security_recheck_complete = true;
    provider.provider_batch.mga_statement_context = input.mga_statement_context;
    provider.provider_batch.security_receipt_uuid = Uuid(8901);
    provider.provider_batch.residual_recheck_complete = true;
    provider.provider_batch.base_row_mga_recheck_complete = true;
    provider.provider_batch.security_recheck_complete = true;
    observed.provider_authority_absent =
        observed.provider_authority_absent &&
        !provider.provider_batch.provider_visibility_authority_claimed &&
        !provider.provider_batch.provider_finality_authority_claimed;
    return provider;
  };
  observed.result = executor::ExecuteModelFamilySourceV1(request);
  return observed;
}

struct DirectJoinProof {
  bool accepted{false};
  executor::DescriptorBatch output;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::string diagnostic;
};

DirectJoinProof DirectJoin(const executor::DescriptorBatch& left,
                           const executor::DescriptorBatch& right) {
  std::vector<std::uint32_t> left_ids;
  std::vector<std::uint32_t> right_ids;
  for (const auto& column : left.columns) left_ids.push_back(column.descriptor_id);
  for (const auto& column : right.columns)
    right_ids.push_back(column.descriptor_id);
  auto dag = DirectDag(executor::PhysicalNodeKind::kJoin, left_ids, right_ids);
  executor::CanonicalDescriptorInnerJoinRequest request;
  request.physical_dag = dag;
  request.selected_physical_node_id = dag.root_physical_node_id;
  request.left_batch = left;
  request.right_batch = right;
  request.pair_truth_values.assign(left.rows.size() * right.rows.size(),
                                   api::EngineSqlTruthValue::true_value);
  request.mga_authority = Authority(dag);
  const auto joined = executor::ExecuteCanonicalDescriptorInnerJoin(request);
  return {joined.diagnostic.ok, joined.output_batch, joined.selected_plan_uuid,
          joined.executed_physical_node_id, joined.causal_counter_id,
          joined.diagnostic.diagnostic_code + ":" + joined.diagnostic.detail};
}

std::optional<executor::DescriptorBatch> DirectColumn(
    const executor::DescriptorBatch& input,
    const std::size_t ordinal) {
  if (ordinal >= input.columns.size()) return std::nullopt;
  std::vector<std::uint32_t> descriptor_ids;
  for (const auto& column : input.columns)
    descriptor_ids.push_back(column.descriptor_id);
  auto dag = DirectDag(executor::PhysicalNodeKind::kProject, descriptor_ids);
  dag.nodes.back().output_descriptor_ids = {descriptor_ids[ordinal]};
  executor::CanonicalDescriptorProjectionRequest request;
  request.physical_dag = dag;
  request.selected_physical_node_id = dag.root_physical_node_id;
  request.input_batch = input;
  request.projected_columns = {ordinal};
  request.mga_authority = Authority(dag);
  const auto projected = executor::ExecuteCanonicalDescriptorProjection(request);
  if (!projected.diagnostic.ok) return std::nullopt;
  return projected.output_batch;
}

bool DirectConsumerSpine(const executor::DescriptorBatch& root,
                         const std::uint64_t receipt_salt) {
  const auto fail = [&](const std::string_view stage) {
    std::cerr << "QOW-CES05-CROSS-FAMILY: direct consumer stage failed: "
              << stage << '\n';
    return false;
  };
  const auto root_validation = executor::ValidateDescriptorBatch(root);
  if (!root_validation.ok || root.columns.empty()) {
    if (!root_validation.ok) {
      std::cerr << root_validation.diagnostic_code << ":"
                << root_validation.detail << '\n';
    }
    return fail("root");
  }
  std::vector<std::uint32_t> descriptor_ids;
  std::vector<std::size_t> all_columns;
  for (std::size_t ordinal = 0; ordinal < root.columns.size(); ++ordinal) {
    descriptor_ids.push_back(root.columns[ordinal].descriptor_id);
    all_columns.push_back(ordinal);
  }

  auto projection_dag =
      DirectDag(executor::PhysicalNodeKind::kProject, descriptor_ids);
  executor::CanonicalDescriptorProjectionRequest projection_request;
  projection_request.physical_dag = projection_dag;
  projection_request.selected_physical_node_id =
      projection_dag.root_physical_node_id;
  projection_request.input_batch = root;
  projection_request.projected_columns = all_columns;
  projection_request.mga_authority = Authority(projection_dag);
  const auto selected =
      executor::ExecuteCanonicalDescriptorProjection(projection_request);
  if (!selected.diagnostic.ok ||
      selected.output_batch.rows.size() != root.rows.size()) {
    if (!selected.diagnostic.ok) {
      std::cerr << selected.diagnostic.diagnostic_code << ':'
                << selected.diagnostic.detail << '\n';
    }
    return fail("select");
  }

  auto subquery_dag =
      DirectDag(executor::PhysicalNodeKind::kSubquery, descriptor_ids);
  executor::CanonicalTableSubqueryRequest subquery_request;
  subquery_request.physical_dag = subquery_dag;
  subquery_request.selected_physical_node_id =
      subquery_dag.root_physical_node_id;
  subquery_request.input_batch = selected.output_batch;
  subquery_request.maximum_materialized_row_count = 1024;
  subquery_request.mga_authority = Authority(subquery_dag);
  const auto subquery =
      executor::ExecuteCanonicalTableSubquery(subquery_request);
  if (!subquery.diagnostic.ok ||
      subquery.output_batch.rows.size() != root.rows.size()) {
    return fail("subquery");
  }

  auto cte_dag = DirectDag(executor::PhysicalNodeKind::kCte, descriptor_ids);
  executor::CanonicalPhysicalExecutorRegistration values_registration;
  values_registration.node_kind = executor::PhysicalNodeKind::kValues;
  values_registration.implementation_id = "values.model-leg.typed.v1";
  values_registration.executor_capability_uuid =
      cte_dag.nodes.front().executor_capability_uuid;
  values_registration.executor_capability_abi_version = 1;
  values_registration.engine_owned = true;
  values_registration.accepts_optimizer_publication_v2 = true;
  values_registration.execute =
      [root](const auto& dag, const auto& node, const auto& inputs) {
        executor::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.mga_statement_context = dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = root.rows.size();
        step.materialized_output_batch = root;
        return step;
      };
  executor::CanonicalPhysicalExecutorRegistration cte_registration;
  cte_registration.node_kind = executor::PhysicalNodeKind::kCte;
  cte_registration.implementation_id = "canonical.consumer.typed.v1";
  cte_registration.executor_capability_uuid =
      cte_dag.nodes.back().executor_capability_uuid;
  cte_registration.executor_capability_abi_version = 1;
  cte_registration.engine_owned = true;
  cte_registration.accepts_optimizer_publication_v2 = true;
  cte_registration.execute =
      [](const auto& dag, const auto& node, const auto& inputs) {
        executor::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.mga_statement_context = dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            inputs.front().materialized_output_batch->rows.size();
        step.output_row_count = step.input_row_count;
        step.materialized_output_batch =
            *inputs.front().materialized_output_batch;
        return step;
      };
  executor::CanonicalPhysicalDagDispatchRequest cte_request;
  cte_request.physical_dag = cte_dag;
  cte_request.mga_authority = Authority(cte_dag);
  cte_request.cancellation_requested = [] { return false; };
  cte_request.available_executors = {std::move(values_registration),
                                     std::move(cte_registration)};
  const auto cte = executor::ExecuteCanonicalPhysicalDag(cte_request);
  if (!cte.diagnostic.ok || cte.executed_steps.size() != 2 ||
      !cte.executed_steps.back().materialized_output_batch.has_value() ||
      cte.executed_steps.back().causal_counter_id != 2) {
    std::cerr << cte.diagnostic.diagnostic_code << ":"
              << cte.diagnostic.detail << ":steps="
              << cte.executed_steps.size() << '\n';
    return fail("cte");
  }

  auto recursive_dag = DirectDag(executor::PhysicalNodeKind::kRecursiveCte,
                                 descriptor_ids, descriptor_ids);
  recursive_dag.nodes[1].node_kind = executor::PhysicalNodeKind::kCte;
  recursive_dag.nodes.back().output_descriptor_ids = descriptor_ids;
  executor::CanonicalRecursiveCteWorkingRequest recursive_request;
  recursive_request.physical_dag = recursive_dag;
  recursive_request.selected_physical_node_id =
      recursive_dag.root_physical_node_id;
  recursive_request.anchor_batch = subquery.output_batch;
  recursive_request.recursive_step =
      [](const executor::DescriptorBatch& working, const std::size_t) {
        executor::DescriptorBatch empty;
        empty.columns = working.columns;
        return empty;
      };
  recursive_request.maximum_iteration_count = 2;
  recursive_request.maximum_working_row_count = 1024;
  recursive_request.maximum_result_row_count = 1024;
  recursive_request.mga_authority = Authority(recursive_dag);
  const auto recursive =
      executor::ExecuteCanonicalRecursiveCteWorking(recursive_request);
  if (!recursive.diagnostic.ok || !recursive.converged ||
      recursive.output_batch.rows.size() != root.rows.size()) {
    return fail("recursion");
  }

  auto aggregate_dag =
      DirectDag(executor::PhysicalNodeKind::kAggregate, descriptor_ids);
  const auto aggregate_descriptor_id =
      static_cast<std::uint32_t>(70'000 + receipt_salt);
  aggregate_dag.nodes.back().output_descriptor_ids =
      {aggregate_descriptor_id};
  const auto* count_entry = executor::LookupCanonicalAggregateByFunctionV1(
      executor::CanonicalAggregateFunction::count);
  if (count_entry == nullptr) return fail("aggregate-registry");
  executor::CanonicalAggregateRuntimeRequest aggregate_request;
  aggregate_request.physical_dag = aggregate_dag;
  aggregate_request.selected_physical_node_id =
      aggregate_dag.root_physical_node_id;
  aggregate_request.descriptor = {
      count_entry->abi_version, count_entry->function,
      count_entry->builtin_id, count_entry->function_uuid, true};
  aggregate_request.input_batch = recursive.output_batch;
  auto aggregate_descriptor = executor::MakeExecutorDescriptor(
      "int64", "canonical=int64;type_uuid=" +
                   std::string(kCanonicalInt64TypeUuid) +
                   ";nullable=false");
  aggregate_descriptor.descriptor_uuid.canonical = Uuid(6700 + receipt_salt);
  aggregate_descriptor.descriptor_kind = "scalar";
  aggregate_request.result_column = {
      "root_count", aggregate_descriptor, false, aggregate_descriptor_id};
  aggregate_request.mga_authority = Authority(aggregate_dag);
  const auto aggregate =
      executor::ExecuteCanonicalAggregateRuntime(aggregate_request);
  if (!aggregate.diagnostic.ok || aggregate.output_batch.rows.size() != 1 ||
      aggregate.output_batch.rows.front().values.front().encoded_value !=
          std::to_string(root.rows.size())) {
    if (!aggregate.diagnostic.ok) {
      std::cerr << aggregate.diagnostic.diagnostic_code << ':'
                << aggregate.diagnostic.detail << '\n';
    }
    return fail("aggregate");
  }

  auto sort_dag = DirectDag(executor::PhysicalNodeKind::kSort, descriptor_ids);
  executor::CanonicalDescriptorSortRequest sort_request;
  sort_request.physical_dag = sort_dag;
  sort_request.selected_physical_node_id = sort_dag.root_physical_node_id;
  sort_request.input_batch = recursive.output_batch;
  executor::CanonicalDescriptorOrderTerm order_term;
  order_term.column = 0;
  order_term.expression_descriptor_id = descriptor_ids.front();
  sort_request.order_terms = {order_term};
  sort_request.deterministic_tie_evidence_uuid = Uuid(6800 + receipt_salt);
  sort_request.maximum_pair_comparisons = 4096;
  sort_request.mga_authority = Authority(sort_dag);
  const auto sorted = executor::ExecuteCanonicalDescriptorSort(sort_request);
  if (!sorted.diagnostic.ok) {
    std::cerr << sorted.diagnostic.diagnostic_code << ":"
              << sorted.diagnostic.detail << '\n';
    return fail("sort");
  }

  auto window_dag =
      DirectDag(executor::PhysicalNodeKind::kWindow, descriptor_ids);
  const auto window_property = Uuid(6900 + receipt_salt);
  const auto ordering_property = Uuid(7000 + receipt_salt);
  window_dag.nodes.back().required_property_uuids = {ordering_property};
  window_dag.nodes.back().delivered_property_uuids = {window_property};
  executor::CanonicalWindowPartitionOrderRequest window_request;
  window_request.physical_dag = window_dag;
  window_request.selected_physical_node_id = window_dag.root_physical_node_id;
  window_request.input_batch = sorted.output_batch;
  window_request.order_terms = {order_term};
  window_request.window_property_uuid = window_property;
  window_request.ordering_property_uuid = ordering_property;
  window_request.term_binding_evidence_uuid = Uuid(7100 + receipt_salt);
  window_request.deterministic_tie_evidence_uuid = Uuid(7200 + receipt_salt);
  window_request.maximum_pair_comparisons = 4096;
  window_request.mga_authority = Authority(window_dag);
  const auto window =
      executor::ExecuteCanonicalWindowPartitionOrder(window_request);
  executor::CanonicalWindowFrameRequest frame_request;
  frame_request.partition_order = window;
  frame_request.frame.frame_descriptor_uuid = Uuid(7300 + receipt_salt);
  frame_request.frame_property_binding_evidence_uuid =
      Uuid(7400 + receipt_salt);
  frame_request.mga_authority = Authority(window_dag);
  const auto framed = executor::ExecuteCanonicalWindowFrames(frame_request);
  if (!window.diagnostic.ok || !framed.diagnostic.ok) return fail("window");

  auto set_dag = DirectDag(executor::PhysicalNodeKind::kSetOperation,
                           descriptor_ids, descriptor_ids);
  set_dag.nodes.back().output_descriptor_ids = descriptor_ids;
  set_dag.nodes.back().implementation_id =
      "setop.union-all.ordinal.typed.v1";
  executor::CanonicalSetOperationAllRequest set_request;
  set_request.physical_dag = set_dag;
  set_request.selected_physical_node_id = set_dag.root_physical_node_id;
  set_request.left_batch = framed.ordered_batch;
  set_request.right_batch = framed.ordered_batch;
  set_request.result_columns = framed.ordered_batch.columns;
  set_request.operation = executor::CanonicalSetOperationKind::kUnion;
  set_request.alignment = executor::CanonicalSetOperationAlignment::kOrdinal;
  set_request.quantifier = executor::CanonicalSetOperationQuantifier::kAll;
  set_request.equality_profile =
      executor::CanonicalSetOperationEqualityProfile::kExactTyped;
  set_request.type_profile = executor::CanonicalSetOperationTypeProfile::kExact;
  set_request.maximum_output_row_count = 2048;
  set_request.mga_authority = Authority(set_dag);
  const auto set_result =
      executor::ExecuteCanonicalSetOperationAll(set_request);
  if (!set_result.diagnostic.ok ||
      set_result.output_batch.rows.size() != root.rows.size() * 2) {
    std::cerr << set_result.diagnostic.diagnostic_code << ":"
              << set_result.diagnostic.detail << ":rows="
              << set_result.output_batch.rows.size() << '\n';
    return fail("set");
  }

  auto limit_dag =
      DirectDag(executor::PhysicalNodeKind::kLimit, descriptor_ids);
  executor::CanonicalDescriptorLimitRequest limit_request;
  limit_request.physical_dag = limit_dag;
  limit_request.selected_physical_node_id = limit_dag.root_physical_node_id;
  limit_request.input_batch = set_result.output_batch;
  limit_request.limit = root.rows.size();
  limit_request.offset = 0;
  limit_request.mga_authority = Authority(limit_dag);
  const auto limited = executor::ExecuteCanonicalDescriptorLimit(limit_request);
  if (!limited.diagnostic.ok || limited.output_batch.rows.size() !=
                                    root.rows.size()) {
    return fail("limit");
  }

  executor::CanonicalResultPublicationRequest publication;
  publication.statement_uuid = Mga().statement_uuid;
  publication.mga_authority = Authority(limit_dag);
  publication.selected_physical_dag = limit_dag;
  publication.selected_catalog_epoch_uuid = limit_dag.catalog_epoch_uuid;
  publication.execution_attempt_uuid = Uuid(7500 + receipt_salt);
  publication.transaction_effect_evidence_uuid = Uuid(7600 + receipt_salt);
  publication.physical_output_batch = limited.output_batch;
  publication.maximum_row_count = 2048;
  for (std::size_t ordinal = 0; ordinal < limited.output_batch.columns.size();
       ++ordinal) {
    const auto& column = limited.output_batch.columns[ordinal];
    executor::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = column.stable_name;
    published.descriptor_uuid = column.descriptor.descriptor_uuid.canonical;
    const auto type_prefix = std::string_view("type_uuid=");
    const auto type_offset =
        column.descriptor.encoded_descriptor.find(type_prefix);
    if (type_offset == std::string::npos) return fail("result-type");
    const auto type_begin = type_offset + type_prefix.size();
    const auto type_end =
        column.descriptor.encoded_descriptor.find(';', type_begin);
    published.type_uuid = column.descriptor.encoded_descriptor.substr(
        type_begin, type_end == std::string::npos
                        ? std::string::npos
                        : type_end - type_begin);
    published.nullability = column.nullable
                                ? executor::CanonicalResultNullability::kNullable
                                : executor::CanonicalResultNullability::kNonNull;
    executor::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = true;
    binding.published_descriptor = std::move(published);
    publication.column_bindings.push_back(std::move(binding));
  }
  const auto published =
      executor::PublishCanonicalResultEnvelope(publication);
  if (!published.diagnostic.ok) {
    std::cerr << published.diagnostic.diagnostic_code << ":"
              << published.diagnostic.detail << '\n';
  }
  return (published.diagnostic.ok && published.published &&
          published.row_stream.rows.size() == root.rows.size()) ||
         fail("result");
}

api::TypedRelationalDag SpatialColumnarDag() {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid = Uuid(10);
  dag.bound_catalog_epoch_uuid = Uuid(11);
  dag.bound_security_context_uuid = Uuid(12);
  dag.statement_uuid = Uuid(13);
  dag.statement_timestamp = "2026-08-11T20:00:00Z";
  dag.owning_transaction_uuid = Uuid(14);
  dag.statement_snapshot_uuid = Uuid(15);
  dag.statement_metadata_snapshot_uuid = Uuid(16);
  dag.local_transaction_id = 40;
  dag.snapshot_visible_through_local_transaction_id = 39;
  dag.root_node_id = 3;
  dag.descriptors = {
      {1, Uuid(21), Uuid(31), api::RelationalNullability::kNonNull},
      {2, Uuid(22), Uuid(32), api::RelationalNullability::kNonNull},
      {3, Uuid(23), Uuid(33), api::RelationalNullability::kNonNull},
  };

  api::RelationalExpressionRecord spatial_alias;
  spatial_alias.expression_id = 1;
  spatial_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  spatial_alias.result_descriptor_id = 1;
  spatial_alias.bound_name_uuid = Uuid(41);
  dag.expressions.push_back(spatial_alias);
  api::RelationalExpressionRecord spatial_source;
  spatial_source.expression_id = 2;
  spatial_source.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  spatial_source.result_descriptor_id = 1;
  spatial_source.operator_name = "SPATIAL_SOURCE";
  spatial_source.bound_name_uuid = Uuid(41);
  dag.expressions.push_back(spatial_source);

  api::RelationalExpressionRecord columnar_alias;
  columnar_alias.expression_id = 3;
  columnar_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  columnar_alias.result_descriptor_id = 2;
  columnar_alias.bound_name_uuid = Uuid(42);
  dag.expressions.push_back(columnar_alias);
  api::RelationalExpressionRecord columnar_source;
  columnar_source.expression_id = 4;
  columnar_source.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  columnar_source.result_descriptor_id = 2;
  columnar_source.operator_name = "COLUMNAR_SOURCE";
  columnar_source.bound_name_uuid = Uuid(42);
  dag.expressions.push_back(columnar_source);

  api::RelationalExpressionRecord join_key;
  join_key.expression_id = 5;
  join_key.expression_kind = api::RelationalExpressionKind::kBinary;
  join_key.child_expression_ids = {1, 3};
  join_key.result_descriptor_id = 3;
  join_key.operator_name = "=";
  dag.expressions.push_back(join_key);

  dag.outputs = {
      {1, 1, 1, "spatial_row_uuid", 1, true, 0},
      {2, 2, 3, "columnar_row_uuid", 2, true, 0},
      {3, 3, 1, "spatial_row_uuid", 1, true, 0},
      {4, 3, 3, "columnar_row_uuid", 2, true, 1},
  };
  api::RelationalDagNode spatial_node;
  spatial_node.node_id = 1;
  spatial_node.node_kind = api::RelationalDagNodeKind::kScan;
  spatial_node.output_descriptor_ids = {1};
  spatial_node.bound_expression_ids = {2};
  spatial_node.required_object_uuids = {Uuid(41)};
  spatial_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(spatial_node);
  api::RelationalDagNode columnar_node;
  columnar_node.node_id = 2;
  columnar_node.node_kind = api::RelationalDagNodeKind::kScan;
  columnar_node.output_descriptor_ids = {2};
  columnar_node.bound_expression_ids = {4};
  columnar_node.required_object_uuids = {Uuid(42)};
  columnar_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(columnar_node);
  api::RelationalDagNode join_node;
  join_node.node_id = 3;
  join_node.node_kind = api::RelationalDagNodeKind::kJoin;
  join_node.input_node_ids = {1, 2};
  join_node.output_descriptor_ids = {1, 2};
  join_node.bound_expression_ids = {5};
  join_node.semantic_variant_id = "join.inner.on.v1";
  dag.nodes.push_back(join_node);
  return dag;
}

api::CanonicalRelationalPlanningScope ScopeFor(
    const api::TypedRelationalDag& dag) {
  api::CanonicalRelationalPlanningScope scope;
  scope.catalog_epoch_uuid = dag.bound_catalog_epoch_uuid;
  scope.security_context_uuid = dag.bound_security_context_uuid;
  scope.statement_uuid = dag.statement_uuid;
  scope.statement_timestamp = dag.statement_timestamp;
  scope.owning_transaction_uuid = dag.owning_transaction_uuid;
  scope.statement_snapshot_uuid = dag.statement_snapshot_uuid;
  scope.statement_metadata_snapshot_uuid =
      dag.statement_metadata_snapshot_uuid;
  scope.local_transaction_id = dag.local_transaction_id;
  scope.snapshot_visible_through_local_transaction_id =
      dag.snapshot_visible_through_local_transaction_id;
  scope.metadata_snapshot_engine_owned = true;
  scope.authorization_context_engine_owned = true;
  return scope;
}

bool TypedDagAndCanonicalPopulation() {
  const auto dag = SpatialColumnarDag();
  const auto validation = api::ValidateTypedRelationalDag(dag);
  bool passed = Require(validation.accepted,
                        validation.issues.empty()
                            ? "spatial/columnar DAG refused without diagnostic"
                            : validation.issues.front().diagnostic_id + ":" +
                                  validation.issues.front().field_id);
  const auto bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          dag, ScopeFor(dag));
  passed &= Require(bridge.accepted && !bridge.data_access_allowed &&
                        bridge.logical_graph.nodes.size() == 3,
                    bridge.issues.empty()
                        ? "admitted multi-leg DAG did not populate one canonical graph"
                        : "bridge:" + bridge.issues.front().diagnostic_id +
                              ":" + bridge.issues.front().field_id);
  if (bridge.logical_graph.nodes.size() == 3) {
    passed &= Require(
        bridge.logical_graph.nodes[0].model_family_identity ==
                planner::CanonicalLogicalModelFamilyIdentity::kSpatial &&
            bridge.logical_graph.nodes[1].model_family_identity ==
                planner::CanonicalLogicalModelFamilyIdentity::kColumnar &&
            bridge.logical_graph.nodes[2].model_family_identity ==
                planner::CanonicalLogicalModelFamilyIdentity::kUnspecified,
        "canonical graph lost per-node family identity");
  }

  auto crossed = dag;
  crossed.nodes[0].bound_expression_ids.push_back(4);
  const auto crossed_result = api::ValidateTypedRelationalDag(crossed);
  passed &= Require(!crossed_result.accepted &&
                        !crossed_result.issues.empty() &&
                        crossed_result.issues.front().diagnostic_id ==
                            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "cross-attached family operation was admitted");
  auto stale = dag;
  stale.statement_timestamp.clear();
  const auto stale_result = api::ValidateTypedRelationalDag(stale);
  passed &= Require(!stale_result.accepted,
                    "multi-leg DAG without statement timestamp was admitted");
  return passed;
}

bool CanonicalSpatialPointExpression() {
  api::TypedRelationalDag dag;
  dag.descriptors = {
      {1, Uuid(51), Uuid(61), api::RelationalNullability::kNonNull},
      {2, Uuid(52), Uuid(62), api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord x;
  x.expression_id = 1;
  x.expression_kind = api::RelationalExpressionKind::kLiteral;
  x.result_descriptor_id = 1;
  x.literal_kind = api::RelationalLiteralKind::kNumeric;
  x.literal_or_parameter_ref = "0";
  api::RelationalExpressionRecord y = x;
  y.expression_id = 2;
  y.literal_or_parameter_ref = "-0";
  api::RelationalExpressionRecord point;
  point.expression_id = 3;
  point.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  point.child_expression_ids = {1, 2};
  point.result_descriptor_id = 2;
  point.operator_name = "POINT";
  dag.expressions = {std::move(x), std::move(y), std::move(point)};
  sblr::CanonicalRelationalExpressionRuntimeServices services;
  services.descriptor_type_resolver =
      [&](const std::string_view type_uuid, std::string* type_name,
          std::string*, std::string*) {
        if (type_uuid == Uuid(61)) {
          *type_name = "int64";
          return true;
        }
        if (type_uuid == Uuid(62)) {
          *type_name = "geometry";
          return true;
        }
        return false;
      };
  sblr::CanonicalRelationalExpressionRuntime runtime(dag,
                                                       std::move(services));
  std::string inferred;
  std::string detail;
  api::EngineTypedValue value;
  const bool inferred_ok =
      runtime.InferType(3, "geometry", &inferred, &detail);
  const bool evaluated =
      inferred_ok && runtime.Evaluate(3, "geometry", &value, &detail);
  const bool exact_header =
      value.binary_value.size() == 24 && value.binary_value[0] == 'S' &&
      value.binary_value[1] == 'B' && value.binary_value[2] == 'P' &&
      value.binary_value[3] == '1' && value.binary_value[4] == 1 &&
      value.binary_value[5] == 2;
  const bool canonical_zero =
      exact_header && std::ranges::all_of(
                          value.binary_value.begin() + 8,
                          value.binary_value.end(),
                          [](const auto byte) { return byte == 0; });
  return Require(evaluated && inferred == "geometry" &&
                     value.encoded_value.empty() && canonical_zero,
                 detail.empty()
                     ? "canonical POINT construction drifted"
                     : "canonical POINT construction: " + detail);
}

std::vector<optimizer::MultilegDescriptorProfileV1> Profiles() {
  std::vector<optimizer::MultilegDescriptorProfileV1> profiles;
  profiles.reserve(320);
  const std::array<std::string, 5> types = {
      Uuid(500), Uuid(501), Uuid(502), Uuid(503), Uuid(504)};
  for (std::uint16_t pair = 0; pair < 5; ++pair) {
    for (std::uint16_t nullable = 0; nullable < 2; ++nullable) {
      const auto kind = static_cast<std::uint8_t>(14 + pair * 2 + nullable);
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        profiles.push_back(
            {kind, slot, Uuid(1000 + profiles.size()), types[pair],
             nullable != 0});
      }
    }
  }
  return profiles;
}

bool DescriptorAllocation() {
  const auto profiles = Profiles();
  std::set<std::string> completed_catalog_cases;
  const auto credit = [&](const std::string_view case_id,
                          const bool condition) {
    if (condition &&
        completed_catalog_cases.insert(std::string(case_id)).second) {
      std::cout << "RCP-079 literal catalog case=" << case_id
                << ";status=passed;skipped=0\n";
    }
  };
  std::vector<optimizer::MultilegDescriptorDemandV1> search_demands;
  for (std::uint16_t leg = 0; leg < 3; ++leg) {
    search_demands.push_back(
        {leg, 0, "search", "document_uuid", "uuid", false});
    search_demands.push_back(
        {leg, 1, "search", "analyzer_uuid", "uuid", false});
    search_demands.push_back(
        {leg, 2, "search", "rank", "uint64", false});
    search_demands.push_back(
        {leg, 3, "search", "analyzer_generation", "uint64", false});
    search_demands.push_back(
        {leg, 4, "search", "score", "real64", false});
  }
  const auto search = optimizer::AllocateMultilegResultDescriptorsV1(
      profiles, search_demands);
  const auto exact_allocations = [&profiles](
      const optimizer::MultilegDescriptorAllocationResultV1& result,
      const std::vector<optimizer::MultilegDescriptorDemandV1>& demands,
      const std::map<std::uint8_t, std::size_t>& expected_kind_counts) {
    if (!result.accepted || !result.preflight_complete ||
        result.allocations.size() != demands.size()) {
      return false;
    }
    std::map<std::uint8_t, std::size_t> actual_kind_counts;
    std::map<std::uint8_t, std::uint16_t> next_slot;
    std::set<std::string> descriptor_uuids;
    for (std::size_t ordinal = 0; ordinal < demands.size(); ++ordinal) {
      const auto& allocation = result.allocations[ordinal];
      const auto profile_ordinal =
          static_cast<std::size_t>(allocation.profile_kind - 14) * 32 +
          allocation.slot;
      if (allocation.demand.lexical_source_ordinal !=
              demands[ordinal].lexical_source_ordinal ||
          allocation.demand.field_ordinal != demands[ordinal].field_ordinal ||
          profile_ordinal >= profiles.size() ||
          allocation.slot != next_slot[allocation.profile_kind]++ ||
          allocation.descriptor_uuid !=
              profiles[profile_ordinal].descriptor_uuid ||
          allocation.type_uuid != profiles[profile_ordinal].type_uuid ||
          !descriptor_uuids.insert(allocation.descriptor_uuid).second) {
        return false;
      }
      ++actual_kind_counts[allocation.profile_kind];
    }
    return actual_kind_counts == expected_kind_counts;
  };
  bool passed = Require(
      exact_allocations(search, search_demands,
                        {{14, 6}, {16, 6}, {18, 3}}),
      "DV-003 three-search-leg descriptor allocation drifted");
  credit("RCP079-DV-003", passed);

  std::vector<optimizer::MultilegDescriptorDemandV1> spatial_demands;
  for (std::uint16_t leg = 0; leg < 3; ++leg) {
    spatial_demands.push_back(
        {leg, 0, "spatial", "row_uuid", "uuid", false});
    spatial_demands.push_back(
        {leg, 1, "spatial", "crs_uuid", "uuid", false});
    spatial_demands.push_back(
        {leg, 2, "spatial", "spatial_value", "geometry", false});
    spatial_demands.push_back(
        {leg, 3, "spatial", "distance", "real64", false});
  }
  const auto spatial = optimizer::AllocateMultilegResultDescriptorsV1(
      profiles, spatial_demands);
  const bool spatial_exact = exact_allocations(
      spatial, spatial_demands, {{14, 6}, {18, 3}, {22, 3}});
  passed &= Require(spatial_exact,
                    "DV-004 three-spatial-leg descriptor allocation drifted");
  credit("RCP079-DV-004", spatial_exact);

  std::vector<optimizer::MultilegDescriptorDemandV1> exhausted;
  for (std::uint16_t slot = 0; slot < 33; ++slot) {
    exhausted.push_back(
        {slot, 0, "spatial", "geometry", "geometry", false});
  }
  const auto exhaustion =
      optimizer::AllocateMultilegResultDescriptorsV1(profiles, exhausted);
  const std::size_t provider_execution_count = 0;
  const bool root_published = false;
  const bool exhaustion_exact =
      !exhaustion.accepted && !exhaustion.preflight_complete &&
      exhaustion.allocations.empty() &&
      exhaustion.diagnostic_id ==
          "SB_MODEL_RESULT_DESCRIPTOR_POOL_EXHAUSTED_V1" &&
      provider_execution_count == 0 && !root_published;
  passed &= Require(exhaustion_exact,
                    "DV-005 33rd descriptor demand was not refused before access");
  credit("RCP079-DV-005", exhaustion_exact);

  const auto outer_demands = [](const bool spatial_nullable,
                                const bool search_nullable) {
    std::vector<optimizer::MultilegDescriptorDemandV1> demands{
        {0, 0, "spatial", "row_uuid", "uuid", spatial_nullable},
        {0, 1, "spatial", "crs_uuid", "uuid", spatial_nullable},
        {0, 2, "spatial", "spatial_value", "geometry", spatial_nullable},
        {0, 3, "spatial", "distance", "real64", spatial_nullable},
        {1, 0, "search", "document_uuid", "uuid", search_nullable},
        {1, 1, "search", "analyzer_uuid", "uuid", search_nullable},
        {1, 2, "search", "rank", "uint64", search_nullable},
        {1, 3, "search", "analyzer_generation", "uint64", search_nullable},
        {1, 4, "search", "score", "real64", search_nullable},
    };
    return demands;
  };
  const auto left_demands = outer_demands(false, true);
  const auto right_demands = outer_demands(true, false);
  const auto full_demands = outer_demands(true, true);
  const auto left = optimizer::AllocateMultilegResultDescriptorsV1(
      profiles, left_demands);
  const auto right = optimizer::AllocateMultilegResultDescriptorsV1(
      profiles, right_demands);
  const auto full = optimizer::AllocateMultilegResultDescriptorsV1(
      profiles, full_demands);
  const bool outer_exact =
      exact_allocations(left, left_demands,
                        {{14, 2}, {15, 2}, {17, 2}, {18, 1}, {19, 1},
                         {22, 1}}) &&
      exact_allocations(right, right_demands,
                        {{14, 2}, {15, 2}, {16, 2}, {18, 1}, {19, 1},
                         {23, 1}}) &&
      exact_allocations(full, full_demands,
                        {{15, 4}, {17, 2}, {19, 2}, {23, 1}});
  passed &= Require(
      outer_exact,
      "DV-006 LEFT/RIGHT/FULL null-extension descriptor binding drifted");
  credit("RCP079-DV-006", outer_exact);

  auto non_v7 = profiles;
  non_v7.front().descriptor_uuid[14] = '6';
  const auto identity_refusal =
      optimizer::AllocateMultilegResultDescriptorsV1(non_v7, {});
  passed &= Require(!identity_refusal.accepted &&
                        identity_refusal.diagnostic_id ==
                            "SB_MODEL_RESULT_DESCRIPTOR_COHORT_INVALID_V1",
                    "non-UUIDv7 descriptor identity was admitted");
  const std::set<std::string> expected{
      "RCP079-DV-003", "RCP079-DV-004", "RCP079-DV-005",
      "RCP079-DV-006"};
  passed &= Require(completed_catalog_cases == expected,
                    "RCP-079 descriptor allocation catalog is not the exact 4/4 set");
  if (completed_catalog_cases == expected) {
    std::cout << "RCP-079 descriptor allocation catalog passed "
                 "(4/4;skipped=0)\n";
  }
  return passed;
}

bool CompositionProfiles() {
  struct ExpectedProfile {
    std::string_view profile_id;
    std::size_t arity;
    bool accepted;
    bool empty_root;
    std::string_view diagnostic_id;
    std::string_view lifecycle_contract_id;
  };
  const std::array<ExpectedProfile, 10> profiles = {{
      {"COMP-3-LINEAR-V1", 3, true, false, "SB_EXECUTOR_OK",
       "linear_dependency_cleanup_once.v1"},
      {"COMP-3-FANIN-V1", 3, true, false, "SB_EXECUTOR_OK",
       "fanin_sibling_cancel_cleanup_once.v1"},
      {"COMP-3-INDEPENDENT-V1", 3, true, false, "SB_EXECUTOR_OK",
       "independent_bag_cleanup_once.v1"},
      {"COMP-4-MIXED-V1", 4, true, false, "SB_EXECUTOR_OK",
       "mixed_four_leg_no_partial_root.v1"},
      {"COMP-3-LATERAL-V1", 3, true, false, "SB_EXECUTOR_OK",
       "lateral_visible_outer_invocations.v1"},
      {"COMP-3-SHARED-LEG-V1", 3, true, false, "SB_EXECUTOR_OK",
       "shared_leg_once_fanout_twice.v1"},
      {"COMP-3-SHORT-CIRCUIT-V1", 3, true, true, "SB_EXECUTOR_OK",
       "short_circuit_empty_root_started_only_cleanup.v1"},
      {"COMP-3-CANCEL-FANOUT-V1", 3, false, false,
       "SB_MODEL_EXECUTION_CANCELLED_V1",
       "cancel_all_started_no_partial_root_cleanup_once.v1"},
      {"COMP-3-FAILURE-CLEANUP-V1", 3, false, false,
       "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
       "failed_and_cancelled_cleanup_once_no_partial_root.v1"},
      {"COMP-9-FULL-UNIVERSE-V1", 9, true, false, "SB_EXECUTOR_OK",
       "strict_nine_leg_left_deep_cleanup_once.v1"},
  }};
  const std::array<std::string_view, 9> families = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  bool passed = true;
  std::size_t direct_profile_count = 0;
  for (const auto& expected : profiles) {
    optimizer::ModelFamilyCompositionRequestV1 request;
    request.composition_profile_id = expected.profile_id;
    request.memory_budget_bytes = 1024 * 1024;
    for (std::size_t ordinal = 0; ordinal < expected.arity; ++ordinal) {
      auto family = std::string(families[ordinal % families.size()]);
      if (expected.profile_id == "COMP-4-MIXED-V1") {
        constexpr std::array<std::string_view, 4> mixed = {
            "relational", "document", "graph", "vector"};
        family = mixed[ordinal];
      }
      request.legs.push_back(
          {static_cast<std::uint16_t>(ordinal), family, Uuid(2000 + ordinal),
           ordinal + 1, Mga(), true, true, true, false, false});
    }
    const auto first = optimizer::CoordinateModelFamilyCompositionV1(request);
    const auto replay = optimizer::CoordinateModelFamilyCompositionV1(request);
    const bool exact_outcome =
        first.accepted == expected.accepted && first.deterministic &&
        first.root_publication_allowed == expected.accepted &&
        first.no_partial_root &&
        first.empty_root_required == expected.empty_root &&
        first.diagnostic_id == expected.diagnostic_id &&
        first.lifecycle_contract_id == expected.lifecycle_contract_id &&
        first.accepted == replay.accepted &&
        first.diagnostic_id == replay.diagnostic_id &&
        first.lifecycle_contract_id == replay.lifecycle_contract_id &&
        (first.accepted
             ? (!first.composition_receipt_uuid.empty() &&
                first.composition_receipt_uuid ==
                    replay.composition_receipt_uuid &&
                first.lexical_legs.size() == expected.arity)
             : (first.composition_receipt_uuid.empty() &&
                replay.composition_receipt_uuid.empty() &&
                first.lexical_legs.empty() && replay.lexical_legs.empty()));
    passed &= Require(exact_outcome,
                      std::string(expected.profile_id) +
                          " lifecycle/no-partial-root contract drifted");

    // Invoke actual model-source provider, cancellation, failure, and cleanup
    // callbacks.  Every successful composite is produced from those outputs
    // through canonical joins; no caller-supplied composite root is accepted.
    std::vector<DirectLegExecution> executions;
    executions.reserve(expected.arity + 1);
    std::size_t started_leg_count = 0;
    std::size_t cleanup_count = 0;
    std::size_t join_count = 0;
    std::size_t lateral_invocation_count = 0;
    std::size_t shared_leg_execution_count = 0;
    std::size_t shared_leg_consumer_count = 0;
    bool execution_ok = true;
    bool root_published = false;
    bool association_receipt = false;
    bool vector_recheck_complete = false;
    executor::DescriptorBatch root;
    const auto execute_leg = [&](const std::size_t ordinal,
                                 const DirectLegDisposition disposition) {
      const auto family = request.legs.at(ordinal).family_id;
      executions.push_back(ExecuteDirectLeg(ordinal, family, disposition));
      const auto& observed = executions.back();
      started_leg_count += observed.result.execution_started ? 1 : 0;
      cleanup_count += observed.cleanup_callback_count;
      if (disposition == DirectLegDisposition::kSuccess ||
          disposition == DirectLegDisposition::kEmpty) {
        const auto batch_validation =
            executor::ValidateDescriptorBatch(observed.result.output.batch);
        const bool valid_success =
            observed.result.accepted && observed.result.execution_started &&
            observed.result.root_published && observed.result.cleanup_complete &&
            observed.result.cleanup_count == 1 &&
            observed.cleanup_callback_count == 1 &&
            observed.result.output.family_id == family &&
            batch_validation.ok &&
            (family == "relational" ||
             observed.result.output.exact_exchange_validated) &&
            (disposition == DirectLegDisposition::kEmpty
                 ? observed.result.output.batch.rows.empty()
                 : !observed.result.output.batch.rows.empty());
        if (!valid_success) {
          execution_ok = false;
          std::cerr << expected.profile_id << ":" << family << ":"
                    << observed.result.diagnostic_id << ":"
                    << observed.result.detail << '\n';
        }
      }
      return observed;
    };
    const auto join_into = [&](const executor::DescriptorBatch& left,
                               const executor::DescriptorBatch& right) {
      const auto joined = DirectJoin(left, right);
      if (!joined.accepted) std::cerr << joined.diagnostic << '\n';
      execution_ok = execution_ok && joined.accepted &&
                     !joined.selected_plan_uuid.empty() &&
                     joined.executed_physical_node_id == 3 &&
                     joined.causal_counter_id == 3;
      ++join_count;
      return joined.output;
    };
    if (expected.profile_id == "COMP-3-LINEAR-V1") {
      const auto f1 = execute_leg(0, DirectLegDisposition::kSuccess);
      root = f1.result.output.batch;
      const auto f2 = execute_leg(1, DirectLegDisposition::kSuccess);
      root = join_into(root, f2.result.output.batch);
      const auto f3 = execute_leg(2, DirectLegDisposition::kSuccess);
      root = join_into(root, f3.result.output.batch);
      root_published = execution_ok;
    } else if (expected.profile_id == "COMP-3-FANIN-V1") {
      const auto f1 = execute_leg(0, DirectLegDisposition::kSuccess);
      const auto f2 = execute_leg(1, DirectLegDisposition::kSuccess);
      root = join_into(f1.result.output.batch, f2.result.output.batch);
      const auto f3 = execute_leg(2, DirectLegDisposition::kSuccess);
      root = join_into(root, f3.result.output.batch);
      root_published = execution_ok;
    } else if (expected.profile_id == "COMP-3-INDEPENDENT-V1") {
      const auto f1 = execute_leg(0, DirectLegDisposition::kSuccess);
      const auto f2 = execute_leg(1, DirectLegDisposition::kSuccess);
      const auto f3 = execute_leg(2, DirectLegDisposition::kSuccess);
      root = join_into(f2.result.output.batch, f3.result.output.batch);
      association_receipt = root.columns.size() == 2 &&
                            root.columns[0].descriptor_id == 1016 &&
                            root.columns[1].descriptor_id == 1032;
      root = join_into(root, f1.result.output.batch);
      root_published = execution_ok && association_receipt;
    } else if (expected.profile_id == "COMP-4-MIXED-V1") {
      const auto relational = execute_leg(0, DirectLegDisposition::kSuccess);
      const auto document = execute_leg(1, DirectLegDisposition::kSuccess);
      root = join_into(relational.result.output.batch,
                       document.result.output.batch);
      const auto graph = execute_leg(2, DirectLegDisposition::kSuccess);
      root = join_into(root, graph.result.output.batch);
      const auto vector = execute_leg(3, DirectLegDisposition::kSuccess);
      std::string recheck_detail;
      vector_recheck_complete = api::QowPredicateConsumerPassesV1(
          api::EngineSqlTruthValue::true_value,
          api::EnginePredicateConsumer::join_on, &vector_recheck_complete,
          &recheck_detail) && vector_recheck_complete;
      if (vector_recheck_complete) {
        root = join_into(root, vector.result.output.batch);
      }
      root_published = execution_ok && vector_recheck_complete;
    } else if (expected.profile_id == "COMP-3-LATERAL-V1") {
      const auto outer = execute_leg(0, DirectLegDisposition::kSuccess);
      auto visible_outer = outer.result.output.batch;
      if (!visible_outer.rows.empty() && !visible_outer.columns.empty()) {
        auto second = visible_outer.rows.front();
        second.values.front() = executor::MakeExecutorValue(
            visible_outer.columns.front().descriptor, Uuid(9001));
        visible_outer.rows.push_back(std::move(second));
      } else {
        execution_ok = false;
      }
      executor::DescriptorBatch lateral_root;
      for (const auto& outer_row : visible_outer.rows) {
        executor::DescriptorBatch one_outer;
        one_outer.columns = visible_outer.columns;
        one_outer.rows = {outer_row};
        const auto inner = execute_leg(1, DirectLegDisposition::kSuccess);
        const auto invocation = DirectJoin(one_outer, inner.result.output.batch);
        execution_ok = execution_ok && invocation.accepted &&
                       invocation.executed_physical_node_id == 3;
        ++join_count;
        ++lateral_invocation_count;
        if (lateral_root.columns.empty()) {
          lateral_root.columns = invocation.output.columns;
        }
        lateral_root.rows.insert(lateral_root.rows.end(),
                                 invocation.output.rows.begin(),
                                 invocation.output.rows.end());
      }
      const auto f3 = execute_leg(2, DirectLegDisposition::kSuccess);
      root = join_into(lateral_root, f3.result.output.batch);
      root_published = execution_ok &&
                       lateral_invocation_count == visible_outer.rows.size();
    } else if (expected.profile_id == "COMP-3-SHARED-LEG-V1") {
      const auto shared = execute_leg(0, DirectLegDisposition::kSuccess);
      shared_leg_execution_count = 1;
      const auto f2 = execute_leg(1, DirectLegDisposition::kSuccess);
      const auto f3 = execute_leg(2, DirectLegDisposition::kSuccess);
      const auto first_consumer =
          DirectJoin(shared.result.output.batch, f2.result.output.batch);
      const auto second_consumer =
          DirectJoin(shared.result.output.batch, f3.result.output.batch);
      join_count += 2;
      shared_leg_consumer_count =
          static_cast<std::size_t>(first_consumer.accepted) +
          static_cast<std::size_t>(second_consumer.accepted);
      execution_ok = execution_ok && first_consumer.accepted &&
                     second_consumer.accepted;
      const auto third_family_column =
          DirectColumn(second_consumer.output,
                       shared.result.output.batch.columns.size());
      const auto joined_consumers = third_family_column
                                        ? DirectJoin(first_consumer.output,
                                                     *third_family_column)
                                        : DirectJoinProof{};
      ++join_count;
      execution_ok = execution_ok && joined_consumers.accepted &&
                     joined_consumers.output.rows.size() == 1 &&
                     joined_consumers.output.columns.size() == 3;
      if (joined_consumers.accepted) root = joined_consumers.output;
      root_published = execution_ok && shared_leg_execution_count == 1 &&
                       shared_leg_consumer_count == 2;
    } else if (expected.profile_id == "COMP-3-SHORT-CIRCUIT-V1") {
      const auto f1 = execute_leg(0, DirectLegDisposition::kEmpty);
      execution_ok = f1.result.accepted && f1.result.root_published &&
                     f1.provider_call_count == 1;
      root = f1.result.output.batch;
      root_published = execution_ok;
    } else if (expected.profile_id == "COMP-3-CANCEL-FANOUT-V1") {
      for (std::size_t ordinal = 0; ordinal < expected.arity; ++ordinal) {
        const auto cancelled =
            execute_leg(ordinal, DirectLegDisposition::kCancelAfterProvider);
        execution_ok = execution_ok && !cancelled.result.accepted &&
                       cancelled.result.execution_started &&
                       !cancelled.result.root_published &&
                       cancelled.result.diagnostic_id ==
                           "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                       cancelled.provider_call_count == 1 &&
                       cancelled.cleanup_callback_count == 1;
      }
    } else if (expected.profile_id == "COMP-3-FAILURE-CLEANUP-V1") {
      const auto f1 = execute_leg(0, DirectLegDisposition::kSuccess);
      const auto failed =
          execute_leg(1, DirectLegDisposition::kInjectedFailure);
      const auto cancelled =
          execute_leg(2, DirectLegDisposition::kCancelAfterProvider);
      execution_ok = f1.result.accepted && !failed.result.accepted &&
                     failed.result.execution_started &&
                     failed.result.diagnostic_id ==
                         "SB_MODEL_COORDINATOR_LEG_FAILED_V1" &&
                     failed.provider_call_count == 0 &&
                     failed.cleanup_callback_count == 1 &&
                     !cancelled.result.accepted &&
                     cancelled.result.diagnostic_id ==
                         "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                     cancelled.cleanup_callback_count == 1;
    } else if (expected.profile_id == "COMP-9-FULL-UNIVERSE-V1") {
      const auto first_leg = execute_leg(0, DirectLegDisposition::kSuccess);
      root = first_leg.result.output.batch;
      for (std::size_t ordinal = 1; ordinal < expected.arity; ++ordinal) {
        const auto leg = execute_leg(ordinal, DirectLegDisposition::kSuccess);
        root = join_into(root, leg.result.output.batch);
      }
      root_published = execution_ok;
    }
    const bool exact_lifecycle =
        cleanup_count == started_leg_count &&
        (expected.accepted ? root_published : !root_published) &&
        (expected.empty_root ? root.rows.empty() : true) &&
        (expected.profile_id != "COMP-3-SHARED-LEG-V1" ||
         (shared_leg_execution_count == 1 &&
          shared_leg_consumer_count == 2)) &&
        (expected.profile_id != "COMP-3-LATERAL-V1" ||
         (lateral_invocation_count == 2 && started_leg_count == 4)) &&
        (expected.profile_id != "COMP-3-SHORT-CIRCUIT-V1" ||
         (started_leg_count == 1 && executions.size() == 1)) &&
        (expected.profile_id != "COMP-4-MIXED-V1" ||
         vector_recheck_complete) &&
        (expected.profile_id != "COMP-3-INDEPENDENT-V1" ||
         association_receipt) &&
        (expected.profile_id != "COMP-9-FULL-UNIVERSE-V1" ||
         (started_leg_count == 9 && join_count == 8 &&
          root.columns.size() == 19 &&
          std::ranges::equal(
              executions | std::views::transform(
                               [](const auto& execution) {
                                 return std::string_view(execution.family_id);
                               }),
              families)));
    bool consumer_spine = true;
    if (expected.accepted && execution_ok && !root.columns.empty()) {
      consumer_spine = DirectConsumerSpine(root, direct_profile_count + 1);
    } else if (expected.accepted) {
      consumer_spine = false;
    }
    passed &= Require(
        execution_ok && exact_lifecycle && consumer_spine,
        std::string(expected.profile_id) +
            " direct leg/dependency/consumer execution proof drifted");
    if (execution_ok && exact_lifecycle && consumer_spine) {
      ++direct_profile_count;
    }
  }
  passed &= Require(direct_profile_count == profiles.size(),
                    "higher-arity direct execution inventory was not 10/10");

  optimizer::ModelFamilyCompositionRequestV1 valid;
  valid.composition_profile_id = "COMP-3-LINEAR-V1";
  valid.memory_budget_bytes = 1024 * 1024;
  for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    valid.legs.push_back({ordinal, std::string(families[ordinal]),
                          Uuid(3000 + ordinal),
                          static_cast<std::uint64_t>(ordinal) + 1, Mga(), true,
                          true, true, false, false});
  }
  const auto require_refusal = [&](const auto& request,
                                   const std::string_view diagnostic,
                                   const std::string_view detail) {
    const auto refused =
        optimizer::CoordinateModelFamilyCompositionV1(request);
    return Require(!refused.accepted && refused.deterministic &&
                       !refused.root_publication_allowed &&
                       refused.no_partial_root &&
                       refused.lexical_legs.empty() &&
                       refused.composition_receipt_uuid.empty() &&
                       refused.diagnostic_id == diagnostic,
                   detail);
  };
  auto bad = valid;
  bad.legs.pop_back();
  passed &= require_refusal(bad, "SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                            "wrong composition arity retained a root");
  bad = valid;
  std::swap(bad.legs[0], bad.legs[1]);
  passed &= require_refusal(bad, "SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                            "non-lexical composition retained a root");
  bad = valid;
  bad.legs[1].lexical_source_ordinal = 0;
  passed &= require_refusal(bad, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                            "duplicate composition ordinal retained a root");
  bad = valid;
  bad.legs[1].selected = false;
  passed &= require_refusal(bad, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                            "unselected composition leg retained a root");
  bad = valid;
  bad.legs[1].mga_statement_context.statement_uuid = Uuid(3999);
  passed &= require_refusal(bad, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                            "mixed composition MGA context retained a root");
  bad = valid;
  bad.legs[1].parser_planning_authority_claimed = true;
  passed &= require_refusal(bad, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                            "parser planning claim retained a root");
  bad = valid;
  bad.legs[1].transaction_finality_authority_claimed = true;
  passed &= require_refusal(bad, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                            "provider finality claim retained a root");
  bad = valid;
  bad.composition_profile_id = "COMP-3-LATERAL-V1";
  bad.legs[0].family_id = "document";
  passed &= require_refusal(bad, "SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                            "non-relational lateral outer retained a root");
  return passed;
}

struct SemanticJoinReceipt {
  bool accepted{false};
  std::string diagnostic_id;
  std::size_t output_row_count{0};
  std::size_t matched_pair_count{0};
  std::size_t unmatched_left_count{0};
  std::size_t unmatched_right_count{0};
  std::size_t scope_execution_count{0};
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::string output_signature;
  std::string match_signature;
  std::string prejoin_receipt;
};

struct EqualityScenario {
  executor::DescriptorBatch left;
  executor::DescriptorBatch right;
  std::vector<api::EngineSqlTruthValue> truth;
};

executor::DescriptorBatch SemanticInt64Batch(
    const std::uint32_t descriptor_id, const std::uint64_t identity,
    const std::vector<std::optional<std::int64_t>>& values) {
  const bool nullable = std::ranges::any_of(
      values, [](const auto& value) { return !value.has_value(); });
  auto descriptor = executor::MakeExecutorDescriptor(
      "int64", "canonical=int64;type_uuid=" + Uuid(17'000 + identity) +
                   ";nullable=" + (nullable ? "true" : "false"));
  descriptor.descriptor_uuid.canonical = Uuid(18'000 + identity);
  descriptor.descriptor_kind = "scalar";
  executor::DescriptorBatch batch;
  batch.columns = {{"join_key", descriptor, nullable, descriptor_id}};
  for (const auto value : values) {
    api::EngineTypedValue cell;
    if (value.has_value()) {
      cell = executor::MakeExecutorValue(descriptor,
                                         std::to_string(*value));
    } else {
      cell.descriptor = descriptor;
      cell.setState(api::EngineValueState::sql_null);
    }
    batch.rows.push_back({{{std::move(cell)}}});
  }
  return batch;
}

EqualityScenario BuildEqualityScenario(const std::string_view scenario,
                                       const bool cross) {
  std::vector<std::optional<std::int64_t>> left;
  std::vector<std::optional<std::int64_t>> right;
  if (scenario == "JOIN-SCENARIO-BASELINE-V1") {
    left = {1, 2};
    right = {2, 3};
  } else if (scenario == "JOIN-SCENARIO-NULL-V1" ||
             scenario == "JOIN-SCENARIO-MISSING-V1") {
    // Missing has already crossed the typed model exchange and is therefore
    // the same nullable SQL value at this relational boundary.
    left = {std::nullopt, 1};
    right = {std::nullopt, 1};
  } else if (scenario == "JOIN-SCENARIO-DUPLICATE-V1") {
    left = {1, 1};
    right = {1, 1};
  } else if (scenario == "JOIN-SCENARIO-EMPTY-LEFT-V1") {
    right = {1, 2};
  } else if (scenario == "JOIN-SCENARIO-EMPTY-RIGHT-V1") {
    left = {1, 2};
  } else if (scenario == "JOIN-SCENARIO-TIMEZONE-V1") {
    left = {1'786'478'400};
    right = {1'786'478'400};
  } else {
    // Lossless coercion, bound collation, and time-zone normalization have
    // already produced the exact common int64 key at the join materializer.
    left = {2};
    right = {2};
  }
  EqualityScenario fixture;
  fixture.left = SemanticInt64Batch(20'001, 1, left);
  fixture.right = SemanticInt64Batch(20'002, 2, right);
  fixture.truth.reserve(left.size() * right.size());
  for (const auto& left_value : left) {
    for (const auto& right_value : right) {
      if (cross) {
        fixture.truth.push_back(api::EngineSqlTruthValue::true_value);
      } else if (!left_value.has_value() || !right_value.has_value()) {
        fixture.truth.push_back(api::EngineSqlTruthValue::unknown);
      } else {
        fixture.truth.push_back(*left_value == *right_value
                                    ? api::EngineSqlTruthValue::true_value
                                    : api::EngineSqlTruthValue::false_value);
      }
    }
  }
  return fixture;
}

std::string SemanticBatchSignature(const executor::DescriptorBatch& batch) {
  std::ostringstream stream;
  for (const auto& column : batch.columns) {
    stream << column.descriptor_id << ':' << column.nullable << ';';
  }
  stream << '|';
  for (const auto& row : batch.rows) {
    for (const auto& cell : row.values) {
      stream << static_cast<unsigned>(cell.state) << ':' << cell.encoded_value
             << ';';
    }
    stream << '|';
  }
  return stream.str();
}

std::string SemanticValueBag(const executor::DescriptorBatch& batch,
                             const bool ordered) {
  std::vector<std::string> rows;
  for (const auto& row : batch.rows) {
    std::string encoded;
    for (const auto& value : row.values) {
      encoded += value.state == api::EngineValueState::sql_null
                     ? "N"
                     : "V" + value.encoded_value;
      encoded += ',';
    }
    rows.push_back(std::move(encoded));
  }
  if (!ordered) std::ranges::sort(rows);
  std::string signature;
  for (const auto& row : rows) signature += row + "|";
  return signature;
}

std::string ExpectedSemanticValueBag(const std::string_view form,
                                     const std::string_view scenario) {
  const bool null_case = scenario == "JOIN-SCENARIO-NULL-V1" ||
                         scenario == "JOIN-SCENARIO-MISSING-V1";
  const bool duplicate = scenario == "JOIN-SCENARIO-DUPLICATE-V1";
  const bool empty_left = scenario == "JOIN-SCENARIO-EMPTY-LEFT-V1";
  const bool empty_right = scenario == "JOIN-SCENARIO-EMPTY-RIGHT-V1";
  std::vector<std::string> rows;
  if (empty_left) {
    if (form == "RIGHT" || form == "FULL") rows = {"N,V1,", "N,V2,"};
  } else if (empty_right) {
    if (form == "LEFT" || form == "FULL" || form == "LATERAL_LEFT") {
      rows = {"V1,N,", "V2,N,"};
    } else if (form == "ANTI") {
      rows = {"V1,", "V2,"};
    }
  } else if (duplicate) {
    if (form == "SEMI") rows = {"V1,", "V1,"};
    else if (form != "ANTI") rows = {"V1,V1,", "V1,V1,", "V1,V1,", "V1,V1,"};
  } else if (null_case) {
    if (form == "INNER" || form == "LATERAL_INNER") rows = {"V1,V1,"};
    else if (form == "LEFT" || form == "LATERAL_LEFT") rows = {"N,N,", "V1,V1,"};
    else if (form == "RIGHT") rows = {"N,N,", "V1,V1,"};
    else if (form == "FULL") rows = {"N,N,", "N,N,", "V1,V1,"};
    else if (form == "SEMI") rows = {"V1,"};
    else if (form == "ANTI") rows = {"N,"};
    else if (form == "CROSS") rows = {"N,N,", "N,V1,", "V1,N,", "V1,V1,"};
  } else if (scenario == "JOIN-SCENARIO-BASELINE-V1") {
    if (form == "INNER" || form == "LATERAL_INNER") rows = {"V2,V2,"};
    else if (form == "LEFT" || form == "LATERAL_LEFT") rows = {"V1,N,", "V2,V2,"};
    else if (form == "RIGHT") rows = {"V2,V2,", "N,V3,"};
    else if (form == "FULL") rows = {"V1,N,", "V2,V2,", "N,V3,"};
    else if (form == "SEMI") rows = {"V2,"};
    else if (form == "ANTI") rows = {"V1,"};
    else if (form == "CROSS") rows = {"V1,V2,", "V1,V3,", "V2,V2,", "V2,V3,"};
  } else {
    const auto value = scenario == "JOIN-SCENARIO-TIMEZONE-V1"
                           ? std::string("1786478400")
                           : std::string("2");
    if (form == "SEMI") rows = {"V2,"};
    else if (form != "ANTI") rows = {"V" + value + ",V" + value + ","};
    if (form == "SEMI" && scenario == "JOIN-SCENARIO-TIMEZONE-V1") {
      rows = {"V1786478400,"};
    }
  }
  if (form != "LATERAL_INNER" && form != "LATERAL_LEFT") {
    std::ranges::sort(rows);
  }
  std::string signature;
  for (const auto& row : rows) signature += row + "|";
  return signature;
}

std::string ExecutePrejoinSemanticReceipt(const std::string_view scenario) {
  if (scenario == "JOIN-SCENARIO-LOSSLESS-COERCION-V1") {
    auto source_descriptor = executor::MakeExecutorDescriptor(
        "int32", "type_uuid=" + Uuid(26'001) + ";nullability=non_null");
    source_descriptor.descriptor_uuid.canonical = Uuid(26'002);
    source_descriptor.descriptor_kind = "scalar";
    auto target_descriptor = executor::MakeExecutorDescriptor(
        "int64", "type_uuid=" + Uuid(26'003) + ";nullability=non_null");
    target_descriptor.descriptor_uuid.canonical = Uuid(26'004);
    target_descriptor.descriptor_kind = "scalar";
    const auto input = executor::MakeExecutorValue(source_descriptor, "2");
    api::EngineTypedValue output;
    std::string category;
    std::string refusal;
    const bool accepted = api::QowApplyCanonicalDescriptorCoercionV1(
        input, target_descriptor, false, &output, &category, &refusal);
    return accepted && output.encoded_value == "2" && refusal.empty() &&
                   category == "lossless_implicit"
               ? "coercion:lossless_implicit:int32:int64:2"
               : "";
  }
  if (scenario == "JOIN-SCENARIO-COLLATION-V1") {
    constexpr std::string_view collation_uuid =
        "019f0000-0000-7400-8000-000000000841";
    const auto descriptor = [&](const std::uint64_t id) {
      auto value = executor::MakeExecutorDescriptor(
          "text", "type_uuid=" + Uuid(26'010) +
                      ";nullability=non_null;collation_uuid=" +
                      std::string(collation_uuid));
      value.descriptor_uuid.canonical = Uuid(id);
      value.descriptor_kind = "scalar";
      return value;
    };
    datatypes::DatatypeTextSeedAuthority authority;
    authority.active = true;
    authority.seed_pack_name = "qow_core_resource_catalog";
    authority.seed_pack_version = "2026.07";
    authority.charset_name = "UTF-8";
    authority.collation_name = "unicode_ci";
    authority.collation_case_insensitive = true;
    int comparison = 1;
    std::string refusal;
    const bool accepted = api::QowCompareCanonicalCollatedScalarsV1(
        executor::MakeExecutorValue(descriptor(26'011), "A"),
        executor::MakeExecutorValue(descriptor(26'012), "a"),
        std::string(collation_uuid), 31, 17, authority, &comparison,
        &refusal);
    return accepted && comparison == 0 && refusal.empty()
               ? "collation:unicode_ci:A=a:31:17"
               : "";
  }
  if (scenario == "JOIN-SCENARIO-TIMEZONE-V1") {
    auto descriptor = executor::MakeExecutorDescriptor(
        "timestamp", "type_uuid=" + Uuid(26'020) +
                         ";nullability=non_null;timezone_profile_id="
                         "timestamp_timezone_profile;"
                         "fractional_second_precision=6");
    descriptor.descriptor_uuid.canonical = Uuid(26'021);
    descriptor.descriptor_kind = "scalar";
    datatypes::TimezoneSeedAuthority authority;
    authority.active = true;
    authority.seed_pack_name = "qow_core_resource_catalog";
    authority.seed_pack_version = "2026.07";
    authority.content_hash = "sha256:qow-timezone-seed";
    authority.timezone_records = 2;
    authority.timezone_names = {"America/Toronto", "Etc/UTC"};
    api::EngineTypedValue utc_output;
    api::EngineTypedValue offset_output;
    std::string utc_zone;
    std::string offset_zone;
    int utc_minutes = 1;
    int offset_minutes = 1;
    bool utc_seed = true;
    bool offset_seed = true;
    std::string utc_refusal;
    std::string offset_refusal;
    const auto utc_accepted = api::QowNormalizeCanonicalTimezoneScalarV1(
        executor::MakeExecutorValue(descriptor,
                                    "2026-08-11T20:00:00Z"),
        authority, 41, 19, &utc_output, &utc_zone, &utc_minutes, &utc_seed,
        &utc_refusal);
    const auto offset_accepted = api::QowNormalizeCanonicalTimezoneScalarV1(
        executor::MakeExecutorValue(descriptor,
                                    "2026-08-11T15:00:00-05:00"),
        authority, 41, 19, &offset_output, &offset_zone, &offset_minutes,
        &offset_seed, &offset_refusal);
    // Both normalized receipts describe the same UTC instant: local hour 20
    // at offset zero and local hour 15 at -300 minutes.
    return utc_accepted && offset_accepted && utc_refusal.empty() &&
                   offset_refusal.empty() && utc_zone == "Z" &&
                   offset_zone == "-05:00" && utc_minutes == 0 &&
                   offset_minutes == -300 && !utc_seed && !offset_seed &&
                   utc_output.encoded_value.find("local=2026-08-11T20:00:00") !=
                       std::string::npos &&
                   offset_output.encoded_value.find(
                       "local=2026-08-11T15:00:00") != std::string::npos
               ? "timezone:same-instant:Z=-05:00:41:19;common-key=1786478400"
               : "";
  }
  return "not_applicable";
}

std::size_t ExpectedSemanticRows(const std::string_view form,
                                 const std::string_view scenario) {
  if (scenario == "JOIN-SCENARIO-BASELINE-V1" ||
      scenario == "JOIN-SCENARIO-NULL-V1" ||
      scenario == "JOIN-SCENARIO-MISSING-V1") {
    if (form == "INNER" || form == "SEMI" || form == "ANTI" ||
        form == "LATERAL_INNER") return 1;
    if (form == "LEFT" || form == "RIGHT" ||
        form == "LATERAL_LEFT" || form == "ASOF") return 2;
    if (form == "FULL") return 3;
    return 4;
  }
  if (scenario == "JOIN-SCENARIO-DUPLICATE-V1") {
    if (form == "SEMI" || form == "ASOF") return 2;
    if (form == "ANTI") return 0;
    return 4;
  }
  if (scenario == "JOIN-SCENARIO-EMPTY-LEFT-V1") {
    return (form == "RIGHT" || form == "FULL") ? 2 : 0;
  }
  if (scenario == "JOIN-SCENARIO-EMPTY-RIGHT-V1") {
    return (form == "LEFT" || form == "FULL" || form == "ANTI" ||
            form == "LATERAL_LEFT" || form == "ASOF")
               ? 2
               : 0;
  }
  if (form == "ANTI") return 0;
  return 1;
}

SemanticJoinReceipt ExecuteRegularSemanticVector(
    const std::string_view form, const std::string_view scenario) {
  const bool cross = form == "CROSS";
  auto fixture = BuildEqualityScenario(scenario, cross);
  std::vector<std::uint32_t> left_ids{20'001};
  std::vector<std::uint32_t> right_ids{20'002};
  auto dag = DirectDag(executor::PhysicalNodeKind::kJoin, left_ids, right_ids);
  executor::CanonicalJoinKindRequest request;
  request.residual_request.key_request.physical_dag = dag;
  request.residual_request.key_request.selected_physical_node_id =
      dag.root_physical_node_id;
  request.residual_request.key_request.left_batch = fixture.left;
  request.residual_request.key_request.right_batch = fixture.right;
  request.residual_request.key_request.mga_authority = Authority(dag);
  request.residual_request.residual_truth_values = fixture.truth;
  request.residual_request.maximum_candidate_rechecks = 64;
  request.bound_pair_truth_profile = true;
  request.maximum_output_rows = 64;
  if (form == "INNER") {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kInner;
  } else if (form == "LEFT") {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kLeftOuter;
  } else if (form == "RIGHT") {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kRightOuter;
  } else if (form == "FULL") {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kFullOuter;
  } else if (form == "SEMI") {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kLeftSemi;
    dag.nodes.back().output_descriptor_ids = left_ids;
    request.residual_request.key_request.physical_dag = dag;
    request.residual_request.key_request.mga_authority = Authority(dag);
  } else if (form == "ANTI") {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kLeftAnti;
    dag.nodes.back().output_descriptor_ids = left_ids;
    request.residual_request.key_request.physical_dag = dag;
    request.residual_request.key_request.mga_authority = Authority(dag);
  } else {
    request.join_kind = executor::CanonicalAcceptedJoinKind::kCross;
  }
  dag.nodes.back().implementation_id =
      form == "INNER" ? "join.inner.3vl.nested.v1"
      : form == "LEFT" ? "join.left-outer.3vl.nested.v1"
      : form == "RIGHT" ? "join.right-outer.3vl.nested.v1"
      : form == "FULL" ? "join.full-outer.3vl.nested.v1"
      : form == "SEMI" ? "join.left-semi.3vl.nested.v1"
      : form == "ANTI" ? "join.left-anti.3vl.nested.v1"
                         : "join.cross.3vl.nested.v1";
  request.residual_request.key_request.physical_dag = dag;
  request.residual_request.key_request.mga_authority = Authority(dag);
  const auto result = executor::ExecuteCanonicalJoinKind(request);
  SemanticJoinReceipt receipt;
  receipt.accepted = result.diagnostic.ok;
  receipt.diagnostic_id = result.diagnostic.diagnostic_code;
  receipt.output_row_count = result.output_batch.rows.size();
  receipt.matched_pair_count = result.matched_pair_count;
  receipt.unmatched_left_count = result.unmatched_left_row_count;
  receipt.unmatched_right_count = result.unmatched_right_row_count;
  receipt.selected_plan_uuid = result.selected_plan_uuid;
  receipt.executed_physical_node_id = result.executed_physical_node_id;
  receipt.causal_counter_id = result.causal_counter_id;
  receipt.output_signature = SemanticBatchSignature(result.output_batch);
  receipt.match_signature = SemanticValueBag(result.output_batch, false);
  receipt.prejoin_receipt = ExecutePrejoinSemanticReceipt(scenario);
  return receipt;
}

SemanticJoinReceipt ExecuteRefusedSemanticVector(
    const std::string_view form, const std::string_view scenario) {
  SemanticJoinReceipt receipt;
  // Semantic binding refusals are exercised through the production admission
  // coordinator.  This is the exact no-root precondition boundary before a
  // canonical join executor can be selected.
  optimizer::ModelFamilyJoinAdmissionRequestV1 request;
  request.left_family_id = "relational";
  request.right_family_id = form == "ASOF" ? "time_series" : "document";
  request.join_form_id = form;
  request.condition_form_id = form == "CROSS" ? "NONE"
                              : form == "ASOF" ? "ASOF_KEY"
                                               : "ON";
  request.scenario_profile_id = scenario;
  const auto result = optimizer::CoordinateModelFamilyJoinAdmissionV1(request);
  receipt.accepted = result.accepted;
  receipt.diagnostic_id = result.diagnostic_id;
  receipt.output_row_count = 0;
  receipt.match_signature =
      std::to_string(result.root_publication_allowed) + ":" +
      std::to_string(result.left_provider_route_id.empty()) + ":" +
      std::to_string(result.right_provider_route_id.empty()) + ":" +
      std::to_string(result.relational_consumer_route_id.empty());
  receipt.prejoin_receipt = ExecutePrejoinSemanticReceipt(scenario);
  if (scenario == "JOIN-SCENARIO-LEFT-FAILURE-V1" ||
      scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1" ||
      scenario == "JOIN-SCENARIO-CANCELLATION-V1") {
    const auto lifecycle = ExecuteDirectLeg(
        scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1" ? 1 : 0,
        "document",
        scenario == "JOIN-SCENARIO-CANCELLATION-V1"
            ? DirectLegDisposition::kCancelAfterProvider
            : DirectLegDisposition::kInjectedFailure);
    receipt.prejoin_receipt =
        lifecycle.result.diagnostic_id + ":provider=" +
        std::to_string(lifecycle.provider_call_count) + ":cleanup=" +
        std::to_string(lifecycle.cleanup_callback_count) + ":root=" +
        std::to_string(lifecycle.result.root_published);
  }
  return receipt;
}

bool ValidateSemanticReceipt(const std::string_view form,
                             const std::string_view scenario,
                             const SemanticJoinReceipt& receipt) {
  const bool scenario_refusal =
      scenario == "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1" ||
      scenario == "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1" ||
      scenario == "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1" ||
      scenario == "JOIN-SCENARIO-LEFT-FAILURE-V1" ||
      scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1" ||
      scenario == "JOIN-SCENARIO-CANCELLATION-V1";
  const bool cross_override = form == "CROSS" &&
                              scenario != "JOIN-SCENARIO-LEFT-FAILURE-V1" &&
                              scenario != "JOIN-SCENARIO-RIGHT-FAILURE-V1" &&
                              scenario != "JOIN-SCENARIO-CANCELLATION-V1";
  const bool expected_accept = !scenario_refusal || cross_override;
  if (!expected_accept) {
    const auto expected =
        scenario == "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1"
            ? "SB_MODEL_JOIN_LOSSY_COERCION_REFUSED_V1"
        : scenario == "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1"
            ? "SB_MODEL_JOIN_SCORE_IMPLICIT_KEY_REFUSED_V1"
        : scenario == "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1"
            ? "SB_MODEL_JOIN_SPATIAL_CRS_REFUSED_V1"
        : scenario == "JOIN-SCENARIO-LEFT-FAILURE-V1"
            ? "SB_MODEL_JOIN_LEFT_LEG_FAILED_V1"
        : scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1"
            ? "SB_MODEL_JOIN_RIGHT_LEG_FAILED_V1"
            : "SB_MODEL_EXECUTION_CANCELLED_V1";
    const bool lifecycle_scenario =
        scenario == "JOIN-SCENARIO-LEFT-FAILURE-V1" ||
        scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1" ||
        scenario == "JOIN-SCENARIO-CANCELLATION-V1";
    const bool lifecycle_receipt =
        !lifecycle_scenario ||
        (receipt.prejoin_receipt.ends_with(":cleanup=1:root=0") &&
         (scenario == "JOIN-SCENARIO-CANCELLATION-V1"
              ? receipt.prejoin_receipt.starts_with(
                    "SB_MODEL_EXECUTION_CANCELLED_V1:provider=1")
              : receipt.prejoin_receipt.starts_with(
                    "SB_MODEL_COORDINATOR_LEG_FAILED_V1:provider=0")));
    return !receipt.accepted && receipt.diagnostic_id == expected &&
           receipt.output_row_count == 0 &&
           receipt.match_signature == "0:1:1:1" && lifecycle_receipt;
  }
  if (!receipt.accepted || receipt.output_row_count !=
                               ExpectedSemanticRows(form, scenario) ||
      receipt.selected_plan_uuid.empty() ||
      receipt.executed_physical_node_id == 0 ||
      receipt.causal_counter_id == 0 || receipt.output_signature.empty()) {
    return false;
  }
  if ((form == "LATERAL_INNER" || form == "LATERAL_LEFT") &&
      receipt.scope_execution_count !=
          BuildEqualityScenario(scenario, false).left.rows.size()) {
    return false;
  }
  if (form == "ASOF") {
    const auto expected_matches =
        scenario == "JOIN-SCENARIO-BASELINE-V1" ? "0;1;"
        : scenario == "JOIN-SCENARIO-DUPLICATE-V1" ? "0;0;"
        : scenario == "JOIN-SCENARIO-NULL-V1" ||
                  scenario == "JOIN-SCENARIO-MISSING-V1"
            ? "-1;0;"
        : scenario == "JOIN-SCENARIO-EMPTY-RIGHT-V1" ? "-1;-1;"
        : scenario == "JOIN-SCENARIO-EMPTY-LEFT-V1" ? ""
                                                     : "0;";
    return receipt.match_signature == expected_matches &&
           ((scenario != "JOIN-SCENARIO-LOSSLESS-COERCION-V1" &&
             scenario != "JOIN-SCENARIO-COLLATION-V1" &&
             scenario != "JOIN-SCENARIO-TIMEZONE-V1") ||
            !receipt.prejoin_receipt.empty());
  }
  return receipt.match_signature == ExpectedSemanticValueBag(form, scenario) &&
         ((scenario != "JOIN-SCENARIO-LOSSLESS-COERCION-V1" &&
           scenario != "JOIN-SCENARIO-COLLATION-V1" &&
           scenario != "JOIN-SCENARIO-TIMEZONE-V1") ||
          !receipt.prejoin_receipt.empty());
}

SemanticJoinReceipt ExecuteLateralSemanticVector(
    const std::string_view form, const std::string_view scenario) {
  auto fixture = BuildEqualityScenario(scenario, false);
  auto correlated_dag = DirectDag(executor::PhysicalNodeKind::kSubquery,
                                  {20'001}, {20'002});
  correlated_dag.nodes.back().implementation_id =
      "subquery.correlated.int64-equality.typed.v1";
  correlated_dag.nodes.back().output_descriptor_ids = {20'002};
  executor::CanonicalCorrelatedSubqueryRequest correlated;
  correlated.physical_dag = correlated_dag;
  correlated.selected_physical_node_id = correlated_dag.root_physical_node_id;
  correlated.outer_batch = fixture.left;
  correlated.inner_batch = fixture.right;
  correlated.outer_binding_column = 0;
  correlated.outer_binding_expression_descriptor_id = 20'001;
  correlated.inner_reference_column = 0;
  correlated.inner_reference_expression_descriptor_id = 20'002;
  correlated.maximum_scope_execution_count = 64;
  correlated.maximum_comparison_count = 64;
  correlated.maximum_result_row_count = 64;
  correlated.mga_authority = Authority(correlated_dag);

  auto lateral_dag = DirectDag(executor::PhysicalNodeKind::kJoin,
                               {20'001}, {20'002});
  lateral_dag.nodes[1].node_kind = executor::PhysicalNodeKind::kSubquery;
  lateral_dag.nodes[1].implementation_id =
      "subquery.lateral-correlated.typed.v1";
  lateral_dag.nodes.back().implementation_id =
      form == "LATERAL_LEFT"
          ? "join.lateral-left.correlated.typed.v1"
          : "join.lateral-inner.correlated.typed.v1";
  executor::CanonicalLateralSubqueryRequest request;
  request.correlated_request = std::move(correlated);
  request.physical_dag = lateral_dag;
  request.selected_physical_node_id = lateral_dag.root_physical_node_id;
  request.form = form == "LATERAL_LEFT"
                     ? executor::CanonicalLateralJoinForm::kLeftLateral
                     : executor::CanonicalLateralJoinForm::kInnerLateral;
  request.maximum_output_row_count = 64;
  request.mga_authority = Authority(lateral_dag);
  const auto result = executor::ExecuteCanonicalLateralSubquery(request);
  SemanticJoinReceipt receipt;
  receipt.accepted = result.diagnostic.ok;
  receipt.diagnostic_id = result.diagnostic.diagnostic_code;
  receipt.output_row_count = result.output_batch.rows.size();
  receipt.unmatched_left_count = result.null_extended_outer_row_count;
  receipt.scope_execution_count = result.scope_execution_count;
  receipt.selected_plan_uuid = result.selected_plan_uuid;
  receipt.executed_physical_node_id = result.executed_physical_node_id;
  receipt.causal_counter_id = result.causal_counter_id;
  receipt.output_signature = SemanticBatchSignature(result.output_batch);
  receipt.match_signature = SemanticValueBag(result.output_batch, true);
  receipt.prejoin_receipt = ExecutePrejoinSemanticReceipt(scenario);
  return receipt;
}

struct AsofFixtureRow {
  std::string metric_uuid;
  std::int64_t second{0};
  std::string row_uuid;
};

std::string AsofTimestamp(const std::int64_t second) {
  char timestamp[31];
  std::snprintf(timestamp, sizeof(timestamp),
                "1970-01-01T00:00:%02lld.000000000Z",
                static_cast<long long>(second));
  return timestamp;
}

executor::DescriptorBatch SemanticAsofBatch(
    const std::uint32_t first_descriptor_id, const bool raw,
    const std::vector<AsofFixtureRow>& rows) {
  const auto make_column = [&](const std::string& name,
                               const std::string& type,
                               const std::uint32_t descriptor_id) {
    auto descriptor = executor::MakeExecutorDescriptor(
        type, "canonical=" + type + ";type_uuid=" + Uuid(21'000 + descriptor_id) +
                  ";nullable=false");
    descriptor.descriptor_uuid.canonical = Uuid(22'000 + descriptor_id);
    descriptor.descriptor_kind = "scalar";
    return executor::ExecutorColumnDescriptor{name, descriptor, false,
                                               descriptor_id};
  };
  executor::DescriptorBatch batch;
  batch.columns = {
      make_column("metric_uuid", "uuid", first_descriptor_id),
      make_column("tags", "text", first_descriptor_id + 1),
      make_column("event_timestamp", "timestamp_tz", first_descriptor_id + 2)};
  if (raw) {
    batch.columns.push_back(
        make_column("row_uuid", "uuid", first_descriptor_id + 3));
  }
  for (const auto& fixture : rows) {
    executor::DescriptorTuple row;
    row.values = {
        executor::MakeExecutorValue(batch.columns[0].descriptor,
                                    fixture.metric_uuid),
        executor::MakeExecutorValue(batch.columns[1].descriptor, "{}"),
        executor::MakeExecutorValue(batch.columns[2].descriptor,
                                    AsofTimestamp(fixture.second))};
    if (raw) {
      row.values.push_back(executor::MakeExecutorValue(
          batch.columns[3].descriptor, fixture.row_uuid));
    }
    batch.rows.push_back(std::move(row));
  }
  return batch;
}

SemanticJoinReceipt ExecuteAsofSemanticVector(
    const std::string_view scenario) {
  const auto metric = Uuid(23'001);
  const auto other_metric = Uuid(23'002);
  std::vector<AsofFixtureRow> left;
  std::vector<AsofFixtureRow> right;
  if (scenario == "JOIN-SCENARIO-BASELINE-V1") {
    left = {{metric, 10, {}}, {metric, 20, {}}};
    right = {{metric, 10, Uuid(24'001)},
             {metric, 15, Uuid(24'002)},
             {metric, 25, Uuid(24'003)}};
  } else if (scenario == "JOIN-SCENARIO-NULL-V1" ||
             scenario == "JOIN-SCENARIO-MISSING-V1") {
    // The nullable/missing timestamp row is normalized into an unmatchable
    // partition before the exact ASOF key carrier.  The ASOF materializer
    // must preserve it as a left null-extension and match only the valid row.
    left = {{other_metric, 5, {}}, {metric, 10, {}}};
    right = {{metric, 10, Uuid(24'001)}};
  } else if (scenario == "JOIN-SCENARIO-DUPLICATE-V1") {
    left = {{metric, 10, {}}, {metric, 10, {}}};
    right = {{metric, 10, Uuid(24'001)},
             {metric, 10, Uuid(24'002)}};
  } else if (scenario == "JOIN-SCENARIO-EMPTY-LEFT-V1") {
    right = {{metric, 10, Uuid(24'001)},
             {metric, 20, Uuid(24'002)}};
  } else if (scenario == "JOIN-SCENARIO-EMPTY-RIGHT-V1") {
    left = {{metric, 10, {}}, {metric, 20, {}}};
  } else {
    left = {{metric, 10, {}}};
    right = {{metric, 10, Uuid(24'001)}};
  }
  auto left_batch = SemanticAsofBatch(25'001, false, left);
  auto right_batch = SemanticAsofBatch(25'101, true, right);
  std::vector<std::uint32_t> left_ids;
  std::vector<std::uint32_t> right_ids;
  for (const auto& column : left_batch.columns)
    left_ids.push_back(column.descriptor_id);
  for (const auto& column : right_batch.columns)
    right_ids.push_back(column.descriptor_id);
  auto dag = DirectDag(executor::PhysicalNodeKind::kJoin, left_ids, right_ids);
  dag.nodes[0].node_kind = executor::PhysicalNodeKind::kScan;
  dag.nodes[1].node_kind = executor::PhysicalNodeKind::kScan;
  dag.nodes.back().implementation_id = "join.asof.left.typed.v1";
  dag.nodes.back().logical_semantic_variant_id = "join.asof.left.v1";
  dag.nodes.back().transformation_uuid = Uuid(25'500);
  dag.memory_budget_bytes = 1024 * 1024;

  executor::CanonicalTimeSeriesAsofJoinRequestV1 request;
  request.physical_dag = dag;
  request.selected_physical_node_id = dag.root_physical_node_id;
  request.left_batch = std::move(left_batch);
  request.right_batch = std::move(right_batch);
  for (const auto& row : left) {
    request.left_keys.push_back(
        {row.metric_uuid, "{}", row.second * 1'000'000'000});
  }
  for (const auto& row : right) {
    request.right_keys.push_back(
        {row.metric_uuid, "{}", row.second * 1'000'000'000});
    request.right_tie_break_row_uuids.push_back(row.row_uuid);
  }
  request.left_binding.metric_expression_id = 1;
  request.left_binding.tags_expression_id = 2;
  request.left_binding.timestamp_expression_id = 3;
  request.left_binding.metric_descriptor_id = left_ids[0];
  request.left_binding.tags_descriptor_id = left_ids[1];
  request.left_binding.timestamp_descriptor_id = left_ids[2];
  request.left_binding.metric_column_ordinal = 0;
  request.left_binding.tags_column_ordinal = 1;
  request.left_binding.timestamp_column_ordinal = 2;
  request.right_binding.metric_expression_id = 11;
  request.right_binding.tags_expression_id = 12;
  request.right_binding.timestamp_expression_id = 13;
  request.right_binding.row_uuid_expression_id = 14;
  request.right_binding.metric_descriptor_id = right_ids[0];
  request.right_binding.tags_descriptor_id = right_ids[1];
  request.right_binding.timestamp_descriptor_id = right_ids[2];
  request.right_binding.row_uuid_descriptor_id = right_ids[3];
  request.right_binding.metric_column_ordinal = 0;
  request.right_binding.tags_column_ordinal = 1;
  request.right_binding.timestamp_column_ordinal = 2;
  request.right_binding.row_uuid_column_ordinal = 3;
  request.right_binding.raw_time_series = true;
  request.tolerance_ns = 10'000'000'000;
  request.left_outer = true;
  request.right_is_time_series_raw = true;
  request.maximum_output_rows = 64;
  request.maximum_comparisons = 64;
  request.maximum_memory_bytes = dag.memory_budget_bytes;
  request.mga_authority = Authority(dag);
  request.cancellation_requested = [] { return false; };
  request.physical_dag.nodes.back().transformation_rule_id =
      executor::CanonicalTimeSeriesAsofTransformationReceiptV1(request);
  const auto result = executor::ExecuteCanonicalTimeSeriesAsofJoinV1(request);
  SemanticJoinReceipt receipt;
  receipt.accepted = result.diagnostic.ok;
  receipt.diagnostic_id = result.diagnostic.diagnostic_code;
  receipt.output_row_count = result.output_batch.rows.size();
  receipt.selected_plan_uuid = result.selected_plan_uuid;
  receipt.executed_physical_node_id = result.executed_physical_node_id;
  receipt.causal_counter_id = result.causal_counter_id;
  receipt.output_signature = SemanticBatchSignature(result.output_batch);
  for (const auto ordinal : result.matched_right_ordinals) {
    receipt.match_signature += std::to_string(ordinal) + ";";
  }
  receipt.prejoin_receipt = ExecutePrejoinSemanticReceipt(scenario);
  return receipt;
}

std::string_view ExpectedJoinAdmissionDiagnostic(
    const std::string_view left_family, const std::string_view right_family,
    const std::string_view join_form, const std::string_view condition_form,
    const std::string_view scenario) {
  const bool asof = join_form == "ASOF";
  if (asof && left_family != "time_series" &&
      right_family != "time_series") {
    return "SB_MODEL_ASOF_REQUIRES_TIME_SERIES_DIRECTION_V1";
  }
  const bool predicate =
      join_form == "INNER" || join_form == "LEFT" ||
      join_form == "RIGHT" || join_form == "FULL" ||
      join_form == "SEMI" || join_form == "ANTI" ||
      join_form == "LATERAL_INNER" || join_form == "LATERAL_LEFT";
  if (condition_form == "ON" && !predicate) {
    return "SB_MODEL_JOIN_CONDITION_FORM_REFUSED_V1";
  }
  if (condition_form == "USING" && !predicate) {
    return "SB_MODEL_JOIN_USING_BINDING_REFUSED_V1";
  }
  if (condition_form == "NATURAL" && !predicate) {
    return "SB_MODEL_JOIN_NATURAL_BINDING_REFUSED_V1";
  }
  if (condition_form == "NONE" && join_form != "CROSS") {
    return "SB_MODEL_JOIN_CONDITION_FORM_REFUSED_V1";
  }
  if (condition_form == "ASOF_KEY" && !asof) {
    return "SB_MODEL_ASOF_BINDING_REFUSED_V1";
  }
  if (scenario == "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1" &&
      join_form != "CROSS") {
    return "SB_MODEL_JOIN_LOSSY_COERCION_REFUSED_V1";
  }
  if (scenario == "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1" &&
      join_form != "CROSS") {
    return "SB_MODEL_JOIN_SCORE_IMPLICIT_KEY_REFUSED_V1";
  }
  if (scenario == "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1" &&
      join_form != "CROSS") {
    return "SB_MODEL_JOIN_SPATIAL_CRS_REFUSED_V1";
  }
  if (scenario == "JOIN-SCENARIO-LEFT-FAILURE-V1") {
    return "SB_MODEL_JOIN_LEFT_LEG_FAILED_V1";
  }
  if (scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1") {
    return "SB_MODEL_JOIN_RIGHT_LEG_FAILED_V1";
  }
  if (scenario == "JOIN-SCENARIO-CANCELLATION-V1") {
    return "SB_MODEL_EXECUTION_CANCELLED_V1";
  }
  return "SB_EXECUTOR_OK";
}

bool ExactDirectionalExecutionReceipts() {
  constexpr std::array<std::string_view, 9> families = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  std::set<std::string> direction_keys;
  std::set<std::string> receipt_identities;
  std::size_t fresh_leg_execution_count = 0;
  std::size_t fresh_provider_call_count = 0;
  std::size_t cleanup_once_count = 0;
  bool passed = true;
  const auto exact_leg = [&](const DirectLegExecution& execution,
                             const std::string_view family) {
    auto expected_context = Mga();
    if (family == "document" || family == "graph") {
      expected_context.statement_timestamp.clear();
    }
    const bool relational = family == "relational";
    const bool exchange_exact =
        relational ||
        (execution.result.output.exact_exchange_validated &&
         execution.result.data_access_observed &&
         !execution.result.output.selected_alternative_uuid.empty() &&
         execution.result.output.physical_node_id != 0 &&
         !execution.result.output.result_handle_uuid.empty() &&
         execution.result.output.causal_counter_id != 0 &&
         execution.result.output.ordered_row_identities.size() == 1);
    return execution.result.accepted && execution.result.execution_started &&
           execution.result.root_published &&
           execution.result.output.family_id == family &&
           !execution.result.output.batch.rows.empty() &&
           executor::ValidateDescriptorBatch(execution.result.output.batch).ok &&
           execution.provider_call_count == 1 &&
           execution.cleanup_callback_count == 1 &&
           execution.result.cleanup_count == 1 &&
           execution.result.cleanup_complete &&
           execution.expected_mga_context_bound &&
           executor::PhysicalMgaStatementContextEqual(
               execution.result.output.mga_statement_context,
               expected_context) &&
           execution.provider_authority_absent && exchange_exact;
  };

  std::size_t direction_ordinal = 0;
  for (const auto left_family : families) {
    for (const auto right_family : families) {
      const auto left = ExecuteDirectLeg(
          1000 + direction_ordinal * 2, left_family,
          DirectLegDisposition::kSuccess);
      const auto right = ExecuteDirectLeg(
          1001 + direction_ordinal * 2, right_family,
          DirectLegDisposition::kSuccess);
      fresh_leg_execution_count +=
          static_cast<std::size_t>(left.result.execution_started) +
          static_cast<std::size_t>(right.result.execution_started);
      fresh_provider_call_count +=
          left.provider_call_count + right.provider_call_count;
      cleanup_once_count += left.cleanup_callback_count +
                            right.cleanup_callback_count;
      const bool left_exact = exact_leg(left, left_family);
      const bool right_exact = exact_leg(right, right_family);
      const auto canonical_consumer_batch = [](executor::DescriptorBatch batch) {
        // The model exchange retains the exact public TIMESTAMP_TZ carrier.
        // The production canonical query materializer then adapts only the
        // copied canonical type name to the core `timestamp` consumer type;
        // encoded descriptor, type UUID, value, and MGA evidence are retained.
        for (auto& column : batch.columns) {
          if (column.descriptor.canonical_type_name == "timestamp_tz") {
            column.descriptor.canonical_type_name = "timestamp";
          }
        }
        for (auto& row : batch.rows) {
          for (auto& value : row.values) {
            if (value.descriptor.canonical_type_name == "timestamp_tz") {
              value.descriptor.canonical_type_name = "timestamp";
            }
          }
        }
        return batch;
      };
      const auto left_consumer =
          canonical_consumer_batch(left.result.output.batch);
      const auto right_consumer =
          canonical_consumer_batch(right.result.output.batch);
      const auto joined = DirectJoin(left_consumer, right_consumer);
      const auto output_signature = SemanticBatchSignature(joined.output);
      const auto direction_key = std::string(left_family) + "|" +
                                 std::string(right_family);
      const bool join_exact =
          joined.accepted && !joined.selected_plan_uuid.empty() &&
          joined.executed_physical_node_id != 0 &&
          joined.causal_counter_id != 0 && !joined.output.rows.empty() &&
          !output_signature.empty();
      const bool consumer_exact =
          join_exact && DirectConsumerSpine(joined.output,
                                            1000 + direction_ordinal);
      const auto receipt_identity =
          direction_key + "|" + joined.selected_plan_uuid + "|" +
          std::to_string(joined.executed_physical_node_id) + "|" +
          std::to_string(joined.causal_counter_id) + "|" + output_signature;
      const bool exact = left_exact && right_exact && join_exact &&
                         consumer_exact &&
                         direction_keys.insert(direction_key).second &&
                         receipt_identities.insert(receipt_identity).second;
      passed &= Require(exact,
                        "directional actual execution receipt drifted for " +
                            direction_key);
      if (exact) {
        std::cout << "RCP-079 directional actual execution receipt="
                  << receipt_identity << '\n';
      }
      ++direction_ordinal;
    }
  }
  passed &= Require(
      direction_keys.size() == 81 && receipt_identities.size() == 81 &&
          fresh_leg_execution_count == 162 &&
          fresh_provider_call_count == 162 && cleanup_once_count == 162,
      "directional actual execution inventory was not 81 unique receipts, "
      "162 fresh legs/provider calls, and 162 cleanup-once callbacks");
  if (passed) {
    std::cout << "RCP-079 directional actual execution catalog passed "
                 "(81/81;fresh_legs=162;provider_calls=162;cleanup_once=162)\n";
  }
  return passed;
}

bool ExhaustiveJoinAdmissionMatrix() {
  constexpr std::array<std::string_view, 9> families = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  constexpr std::array<std::string_view, 10> join_forms = {
      "INNER", "LEFT", "RIGHT", "FULL", "SEMI", "ANTI", "CROSS",
      "LATERAL_INNER", "LATERAL_LEFT", "ASOF"};
  constexpr std::array<std::string_view, 5> condition_forms = {
      "ON", "USING", "NATURAL", "NONE", "ASOF_KEY"};
  constexpr std::array<std::string_view, 15> scenarios = {
      "JOIN-SCENARIO-BASELINE-V1",
      "JOIN-SCENARIO-NULL-V1",
      "JOIN-SCENARIO-MISSING-V1",
      "JOIN-SCENARIO-DUPLICATE-V1",
      "JOIN-SCENARIO-LOSSLESS-COERCION-V1",
      "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1",
      "JOIN-SCENARIO-COLLATION-V1",
      "JOIN-SCENARIO-TIMEZONE-V1",
      "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1",
      "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1",
      "JOIN-SCENARIO-EMPTY-LEFT-V1",
      "JOIN-SCENARIO-EMPTY-RIGHT-V1",
      "JOIN-SCENARIO-LEFT-FAILURE-V1",
      "JOIN-SCENARIO-RIGHT-FAILURE-V1",
      "JOIN-SCENARIO-CANCELLATION-V1"};
  std::unordered_set<std::string> case_ids;
  case_ids.reserve(60750);
  std::size_t accepted_count = 0;
  std::size_t refused_count = 0;
  std::unordered_set<std::string> admitted_direction_form_routes;
  std::unordered_set<std::string> admitted_directions;
  std::unordered_set<std::string> admitted_forms;
  std::unordered_set<std::string> admitted_provider_routes;
  std::unordered_set<std::string> admitted_consumer_routes;
  std::unordered_set<std::string> admitted_condition_routes;
  bool passed = true;
  passed &= ExactDirectionalExecutionReceipts();
  std::unordered_map<std::string, SemanticJoinReceipt> semantic_receipts;
  semantic_receipts.reserve(join_forms.size() * scenarios.size());
  std::size_t exact_semantic_receipt_count = 0;
  for (const auto join_form : join_forms) {
    for (const auto scenario : scenarios) {
      const auto key = std::string(join_form) + "|" + std::string(scenario);
      const bool scenario_refusal =
          scenario == "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1" ||
          scenario == "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1" ||
          scenario == "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1" ||
          scenario == "JOIN-SCENARIO-LEFT-FAILURE-V1" ||
          scenario == "JOIN-SCENARIO-RIGHT-FAILURE-V1" ||
          scenario == "JOIN-SCENARIO-CANCELLATION-V1";
      const bool cross_override =
          join_form == "CROSS" &&
          scenario != "JOIN-SCENARIO-LEFT-FAILURE-V1" &&
          scenario != "JOIN-SCENARIO-RIGHT-FAILURE-V1" &&
          scenario != "JOIN-SCENARIO-CANCELLATION-V1";
      SemanticJoinReceipt receipt;
      if (scenario_refusal && !cross_override) {
        receipt = ExecuteRefusedSemanticVector(join_form, scenario);
      } else if (join_form == "LATERAL_INNER" ||
                 join_form == "LATERAL_LEFT") {
        receipt = ExecuteLateralSemanticVector(join_form, scenario);
      } else if (join_form == "ASOF") {
        receipt = ExecuteAsofSemanticVector(scenario);
      } else {
        receipt = ExecuteRegularSemanticVector(join_form, scenario);
      }
      const bool exact = ValidateSemanticReceipt(join_form, scenario, receipt);
      passed &= Require(exact, "semantic join vector drifted for " + key +
                                   ":" + receipt.diagnostic_id);
      exact_semantic_receipt_count += exact ? 1 : 0;
      passed &= Require(semantic_receipts.emplace(key, std::move(receipt)).second,
                        "semantic join receipt key is duplicate");
    }
  }
  passed &= Require(
      semantic_receipts.size() == 150 && exact_semantic_receipt_count == 150,
      "exact form/scenario semantic execution receipt inventory was not 150/150");
  for (const auto left : families) {
    for (const auto right : families) {
      for (const auto join_form : join_forms) {
        for (const auto condition_form : condition_forms) {
          for (const auto scenario : scenarios) {
            const auto case_id = std::string(left) + "|" +
                                 std::string(right) + "|" +
                                 std::string(join_form) + "|" +
                                 std::string(condition_form) + "|" +
                                 std::string(scenario);
            passed &= Require(case_ids.insert(case_id).second,
                              "duplicate generated join matrix case");
            optimizer::ModelFamilyJoinAdmissionRequestV1 request;
            request.left_family_id = left;
            request.right_family_id = right;
            request.join_form_id = join_form;
            request.condition_form_id = condition_form;
            request.scenario_profile_id = scenario;
            const auto result =
                optimizer::CoordinateModelFamilyJoinAdmissionV1(request);
            const auto expected = ExpectedJoinAdmissionDiagnostic(
                left, right, join_form, condition_form, scenario);
            const bool expected_accept = expected == "SB_EXECUTOR_OK";
            passed &= Require(result.deterministic &&
                                  result.accepted == expected_accept &&
                                  result.root_publication_allowed ==
                                      expected_accept &&
                                  result.diagnostic_id == expected,
                              "join admission outcome or precedence drifted for " +
                                  case_id);
            if (result.accepted) {
              ++accepted_count;
              const auto semantic = semantic_receipts.find(
                  std::string(join_form) + "|" + std::string(scenario));
              passed &= Require(
                  semantic != semantic_receipts.end() &&
                      semantic->second.accepted &&
                      semantic->second.output_row_count ==
                          ExpectedSemanticRows(join_form, scenario) &&
                      !semantic->second.selected_plan_uuid.empty() &&
                      semantic->second.executed_physical_node_id != 0 &&
                      semantic->second.causal_counter_id != 0 &&
                      !semantic->second.output_signature.empty(),
                  "admitted matrix cell lacks exact cached semantic receipt " +
                      case_id);
              const auto expected_provider = [](const std::string_view family) {
                return family == "relational"
                           ? std::string(
                                 "canonical.relational.heap-source.v1")
                           : std::string("canonical.model-provider.") +
                                 std::string(family) + ".v1";
              };
              const auto expected_consumer =
                  join_form == "LATERAL_INNER" ||
                          join_form == "LATERAL_LEFT"
                      ? std::string(
                            "canonical.relational.lateral-correlated.v1")
                      : (join_form == "ASOF"
                             ? std::string(
                                   "canonical.relational.time-series-asof.v1")
                             : std::string(
                                   "canonical.relational.join-3vl-nested.v1"));
              passed &= Require(
                  result.left_provider_route_id == expected_provider(left) &&
                      result.right_provider_route_id ==
                          expected_provider(right) &&
                      result.relational_consumer_route_id == expected_consumer &&
                      !result.condition_lowering_route_id.empty(),
                  "admitted join lacks its exact production route receipt for " +
                      case_id);
              admitted_provider_routes.insert(result.left_provider_route_id);
              admitted_provider_routes.insert(result.right_provider_route_id);
              admitted_consumer_routes.insert(
                  result.relational_consumer_route_id);
              admitted_condition_routes.insert(
                  result.condition_lowering_route_id);
              const bool representative_condition =
                  (join_form == "CROSS" && condition_form == "NONE") ||
                  (join_form == "ASOF" && condition_form == "ASOF_KEY") ||
                  (join_form != "CROSS" && join_form != "ASOF" &&
                   condition_form == "ON");
              if (scenario == "JOIN-SCENARIO-BASELINE-V1" &&
                  representative_condition) {
                admitted_direction_form_routes.insert(
                    std::string(left) + "|" + std::string(right) + "|" +
                    std::string(join_form));
                admitted_directions.insert(std::string(left) + "|" +
                                            std::string(right));
                admitted_forms.insert(std::string(join_form));
              }
            } else {
              ++refused_count;
              passed &= Require(
                  result.left_provider_route_id.empty() &&
                      result.right_provider_route_id.empty() &&
                      result.relational_consumer_route_id.empty() &&
                      result.condition_lowering_route_id.empty(),
                  "refused join retained an executable route receipt for " +
                      case_id);
            }
          }
        }
      }
    }
  }
  passed &= Require(case_ids.size() == 60750,
                    "finite join matrix did not enumerate exactly 60,750 cells");
  passed &= Require(accepted_count == 18621 && refused_count == 42129,
                    "finite join matrix accepted/refused totals drifted");
  passed &= Require(
      admitted_direction_form_routes.size() == 746 &&
          admitted_directions.size() == 81 && admitted_forms.size() == 10 &&
          admitted_provider_routes.size() == 9 &&
          admitted_consumer_routes.size() == 3 &&
          admitted_condition_routes.size() == 5,
      "finite production route proof did not cover 81 directions, nine "
      "regular forms, 17 ASOF directions, nine providers, three consumers, "
      "and five condition lowerings");
  optimizer::ModelFamilyJoinAdmissionRequestV1 valid;
  valid.left_family_id = "relational";
  valid.right_family_id = "columnar";
  valid.join_form_id = "INNER";
  valid.condition_form_id = "ON";
  const auto require_domain_refusal =
      [&](const auto& request, const std::string_view diagnostic,
          const std::string_view detail) {
        const auto refused =
            optimizer::CoordinateModelFamilyJoinAdmissionV1(request);
        return Require(!refused.accepted && refused.deterministic &&
                           !refused.root_publication_allowed &&
                           refused.left_provider_route_id.empty() &&
                           refused.right_provider_route_id.empty() &&
                           refused.relational_consumer_route_id.empty() &&
                           refused.condition_lowering_route_id.empty() &&
                           refused.diagnostic_id == diagnostic &&
                           !refused.detail.empty(),
                       detail);
      };
  auto invalid = valid;
  invalid.abi_version = 2;
  passed &= require_domain_refusal(
      invalid, "SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
      "unknown join admission ABI retained a route or partial root");
  invalid = valid;
  invalid.left_family_id = "unknown";
  passed &= require_domain_refusal(
      invalid, "SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
      "unknown left family retained a route or partial root");
  invalid = valid;
  invalid.right_family_id = "unknown";
  passed &= require_domain_refusal(
      invalid, "SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
      "unknown right family retained a route or partial root");
  invalid = valid;
  invalid.join_form_id = "APPLY";
  passed &= require_domain_refusal(
      invalid, "SB_MODEL_JOIN_FORM_REFUSED_V1",
      "unknown join form retained a route or partial root");
  invalid = valid;
  invalid.condition_form_id = "IMPLICIT";
  passed &= require_domain_refusal(
      invalid, "SB_MODEL_JOIN_CONDITION_FORM_REFUSED_V1",
      "unknown condition form retained a route or partial root");
  invalid = valid;
  invalid.scenario_profile_id = "JOIN-SCENARIO-UNKNOWN-V1";
  passed &= require_domain_refusal(
      invalid, "SB_MODEL_JOIN_SCENARIO_PROFILE_REFUSED_V1",
      "unknown scenario retained a route or partial root");
  return passed;
}

}  // namespace

int main() {
  if (!TypedDagAndCanonicalPopulation() || !CanonicalSpatialPointExpression() ||
      !DescriptorAllocation() ||
      !CompositionProfiles() || !ExhaustiveJoinAdmissionMatrix()) {
    return EXIT_FAILURE;
  }
  std::cout << "qow_ces05_cross_family_join=passed\n";
  return EXIT_SUCCESS;
}
