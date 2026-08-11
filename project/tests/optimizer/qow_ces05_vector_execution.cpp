// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"
#include "canonical_query_execute.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "logical_plan.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "model_family_coordinator.hpp"
#include "model_family_executor.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace hash = scratchbird::core::hash;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kExpectedDigest =
    "ac049f43ba1ec2ffe8112566f55679284aa29b2f3e6d577bd8e9d656cd244250";
constexpr std::string_view kExpectedCapability =
    "ac049f43-ba1e-82ff-a811-2566f5567928";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "RCP-077: " << detail << '\n';
  return condition;
}

std::string TestUuid(const std::uint64_t value) {
  std::ostringstream out;
  out << "70000000-0000-4000-8000-" << std::hex << std::setw(12)
      << std::setfill('0') << value;
  return out.str();
}

exec::PhysicalMgaStatementContext VectorMga() {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = TestUuid(1);
  context.statement_timestamp = "2026-08-11T01:02:03Z";
  context.owning_transaction_uuid = TestUuid(2);
  context.statement_snapshot_uuid = TestUuid(3);
  context.statement_metadata_snapshot_uuid = TestUuid(4);
  context.owning_local_transaction_id = 5;
  context.visible_committed_high_watermark = 20;
  context.oldest_active_transaction_id = 2;
  context.oldest_interesting_transaction_id = 3;
  context.oldest_snapshot_transaction_id = 3;
  context.retention_horizon_transaction_id = 3;
  context.active_excluded_local_transaction_ids = {5, 9};
  context.in_doubt_excluded_local_transaction_ids = {8};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 30;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

api::EngineDescriptor VectorDescriptor(const std::uint64_t identity,
                                       const std::string_view type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = TestUuid(identity);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor =
      "type_uuid=" + TestUuid(identity + 100) + ";nullability=non_null";
  return descriptor;
}

api::EngineTypedValue VectorValue(const api::EngineDescriptor& descriptor,
                                  std::string value) {
  return {descriptor, std::move(value), false};
}

plan::CanonicalLogicalRelationalGraph VectorLogicalGraph() {
  const auto physical = VectorMga();
  plan::CanonicalMgaStatementContext mga;
  mga.statement_uuid = physical.statement_uuid;
  mga.statement_timestamp = physical.statement_timestamp;
  mga.owning_transaction_uuid = physical.owning_transaction_uuid;
  mga.statement_snapshot_uuid = physical.statement_snapshot_uuid;
  mga.statement_metadata_snapshot_uuid =
      physical.statement_metadata_snapshot_uuid;
  mga.owning_local_transaction_id = physical.owning_local_transaction_id;
  mga.visible_committed_high_watermark =
      physical.visible_committed_high_watermark;
  mga.oldest_active_transaction_id = physical.oldest_active_transaction_id;
  mga.oldest_interesting_transaction_id =
      physical.oldest_interesting_transaction_id;
  mga.oldest_snapshot_transaction_id =
      physical.oldest_snapshot_transaction_id;
  mga.retention_horizon_transaction_id =
      physical.retention_horizon_transaction_id;
  mga.active_excluded_local_transaction_ids =
      physical.active_excluded_local_transaction_ids;
  mga.in_doubt_excluded_local_transaction_ids =
      physical.in_doubt_excluded_local_transaction_ids;
  mga.snapshot_kind = physical.snapshot_kind;
  mga.publication_inventory_next_local_transaction_id =
      physical.publication_inventory_next_local_transaction_id;
  mga.inventory_authoritative = physical.inventory_authoritative;
  mga.complete = physical.complete;
  mga.current = physical.current;

  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = 1;
  node.node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  node.output_descriptor_ids = {101, 102, 103};
  node.bound_expression_ids = {201, 202, 203, 204, 205, 206, 207, 208};
  node.origin_relational_node_ids = {1};
  node.required_object_uuids = {TestUuid(77)};
  node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  node.model_family_identity =
      plan::CanonicalLogicalModelFamilyIdentity::kVector;

  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = TestUuid(10);
  graph.catalog_epoch_uuid = TestUuid(11);
  graph.security_context_uuid = TestUuid(12);
  graph.local_transaction_id = mga.owning_local_transaction_id;
  graph.statement_snapshot_id = mga.visible_committed_high_watermark;
  graph.mga_statement_context = std::move(mga);
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {101, 102, 103};
  graph.nodes = {std::move(node)};
  return graph;
}

opt::ModelFamilyCoordinatorRequestV1 VectorPlanningRequest() {
  opt::ModelFamilyCoordinatorRequestV1 request;
  request.family_id = "vector";
  request.operation_id = "VECTOR_ANN_SEARCH";
  request.logical_operator_id = "LOGICAL_VECTOR_SOURCE_V1";
  request.logical_node_id = 1;
  request.object_uuid = TestUuid(77);
  request.output_descriptor_ids = {101, 102, 103};
  request.mga_statement_context = VectorMga();
  request.bound_sblr_tree_uuid = TestUuid(10);
  request.catalog_epoch_uuid = TestUuid(11);
  request.security_context_uuid = TestUuid(12);
  request.capability_snapshot_uuid = TestUuid(13);
  request.resource_snapshot_uuid = TestUuid(14);
  request.statistics_snapshot_uuid = TestUuid(15);
  request.route_snapshot_uuid = TestUuid(16);
  request.catalog_generation = 7;
  request.current_catalog_generation = 7;
  request.security_epoch = 8;
  request.policy_epoch = 9;
  request.resource_epoch = 10;
  request.statistics_generation = 11;
  request.route_epoch = 12;
  request.route_generation = 13;
  request.memory_budget_bytes = 1U << 20U;
  opt::ModelFamilyCandidateV1 candidate;
  candidate.alternative_uuid = TestUuid(20);
  candidate.provider_uuid = TestUuid(21);
  candidate.capability_uuid = std::string(kExpectedCapability);
  candidate.implementation_id = "physical_vector_search_v1";
  candidate.provider_generation = 7;
  candidate.available = true;
  candidate.exact = true;
  candidate.residual_recheck_required = true;
  candidate.base_row_mga_recheck_required = true;
  candidate.security_recheck_required = true;
  candidate.cost.cost_vector_uuid = TestUuid(22);
  candidate.cost.cpu_units = 3;
  candidate.cost.sequential_read_units = 2;
  candidate.cost.random_read_units = 1;
  candidate.cost.memory_bytes_required = 4096;
  request.candidates = {candidate};
  return request;
}

struct VectorExecutionFixture {
  exec::ModelFamilyExecutionRequestV1 request;
  std::shared_ptr<std::uint32_t> cleanup_count;
};

VectorExecutionFixture VectorExecutionRequest() {
  VectorExecutionFixture fixture;
  fixture.cleanup_count = std::make_shared<std::uint32_t>(0);
  auto& request = fixture.request;
  auto& input = request.input;
  input.family_id = "vector";
  input.operation_id = "VECTOR_ANN_SEARCH";
  input.object_uuid = TestUuid(77);
  input.physical_node_id = 1;
  input.selected_alternative_uuid = TestUuid(20);
  input.capability_uuid = std::string(kExpectedCapability);
  input.provider_uuid = TestUuid(21);
  input.provider_generation = 7;
  input.result_handle_uuid = TestUuid(23);
  input.causal_counter_id = 1;
  input.output_descriptor_ids = {101, 102, 103};
  input.mga_statement_context = VectorMga();
  input.catalog_epoch_uuid = TestUuid(11);
  input.security_context_uuid = TestUuid(12);
  input.policy_snapshot_uuid = TestUuid(24);
  input.resource_contract_uuid = TestUuid(25);
  input.catalog_generation = 7;
  input.descriptor_generation = 8;
  input.security_generation = 9;
  input.policy_generation = 10;
  input.resource_generation = 11;
  input.maximum_rows = 2;
  input.maximum_cells = 6;
  input.maximum_memory_bytes = 1U << 20U;
  request.capability.capability_uuid = input.capability_uuid;
  request.capability.family_id = input.family_id;
  request.capability.provider_uuid = input.provider_uuid;
  request.capability.provider_generation = input.provider_generation;
  request.capability.available = true;
  request.capability.exact = true;
  request.capability.exact_collection_fallback_available = true;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.current_catalog_generation = input.catalog_generation;
  request.current_descriptor_generation = input.descriptor_generation;
  request.current_security_generation = input.security_generation;
  request.current_policy_generation = input.policy_generation;
  request.current_resource_generation = input.resource_generation;
  request.current_provider_generation = input.provider_generation;
  request.current_mga_statement_context = input.mga_statement_context;
  request.security_admitted = true;
  request.cancellation_requested = [] { return false; };
  request.cleanup_provider = [count = fixture.cleanup_count] { ++*count; };
  request.execute_provider = [](const auto& selected) {
    exec::ModelProviderExecutionResultV1 result;
    result.ok = true;
    result.data_access_observed = true;
    result.rows_examined = 2;
    auto& batch = result.provider_batch;
    batch.provider_uuid = selected.provider_uuid;
    batch.provider_generation = selected.provider_generation;
    batch.selected_alternative_uuid = selected.selected_alternative_uuid;
    batch.capability_uuid = selected.capability_uuid;
    batch.exact_fallback_selected = selected.exact_fallback_selected;
    batch.result_handle_uuid = selected.result_handle_uuid;
    batch.causal_counter_id = selected.causal_counter_id;
    batch.output_descriptor_ids = selected.output_descriptor_ids;
    const auto row_uuid = VectorDescriptor(31, "uuid");
    const auto distance = VectorDescriptor(32, "real64");
    const auto score = VectorDescriptor(33, "real64");
    batch.batch.columns = {{"row_uuid", row_uuid, false, 101},
                           {"distance", distance, false, 102},
                           {"score", score, false, 103}};
    batch.batch.rows = {
        {{VectorValue(row_uuid, TestUuid(40)),
          VectorValue(distance, "0.25"), VectorValue(score, "1")}},
        {{VectorValue(row_uuid, TestUuid(41)),
          VectorValue(distance, "0.5"), VectorValue(score, "0.5")}},
    };
    exec::ModelProviderRowIdentityV1 first;
    first.row_uuid = TestUuid(40);
    first.vector_distance = "0.25";
    first.vector_score = "1";
    exec::ModelProviderRowIdentityV1 second;
    second.row_uuid = TestUuid(41);
    second.vector_distance = "0.5";
    second.vector_score = "0.5";
    batch.ordered_row_identities = {std::move(first), std::move(second)};
    batch.properties.property_uuid = TestUuid(26);
    batch.properties.ordering_id =
        "vector_distance_row_uuid_ascending_v1";
    batch.properties.partitioning_id = "single_local_partition";
    batch.properties.uniqueness_id = "row_uuid";
    batch.properties.exact = true;
    batch.properties.residual_recheck_complete = true;
    batch.properties.base_row_mga_recheck_complete = true;
    batch.properties.security_recheck_complete = true;
    batch.mga_statement_context = selected.mga_statement_context;
    batch.security_receipt_uuid = TestUuid(27);
    batch.residual_recheck_complete = true;
    batch.base_row_mga_recheck_complete = true;
    batch.security_recheck_complete = true;
    return result;
  };
  return fixture;
}

bool CanonicalVectorSpine() {
  bool passed = true;
  const auto logical =
      plan::ValidateCanonicalLogicalRelationalGraph(VectorLogicalGraph());
  passed &= Require(logical.accepted && logical.validated_node_count == 1,
                    "canonical vector logical identity was refused");
  auto logical_mutation = VectorLogicalGraph();
  logical_mutation.nodes.front().bound_expression_ids.pop_back();
  passed &= Require(
      !plan::ValidateCanonicalLogicalRelationalGraph(logical_mutation).accepted,
      "partial vector logical attachment was accepted");

  const auto planned =
      opt::CoordinateModelFamilySourceV1(VectorPlanningRequest());
  passed &= Require(
      planned.accepted && planned.selected && planned.deterministic &&
          planned.data_access_allowed &&
          planned.logical_operator_id == "LOGICAL_VECTOR_SOURCE_V1" &&
          planned.physical_operator_id == "PHYSICAL_VECTOR_SEARCH_V1" &&
      planned.physical_dag.nodes.size() == 1 &&
          planned.physical_dag.nodes.front().implementation_id ==
              "physical_vector_search_v1",
      "canonical vector physical alternative was not selected: " +
          planned.diagnostic_id + " " + planned.detail);
  auto bad_planning = VectorPlanningRequest();
  bad_planning.mga_statement_context.statement_timestamp.clear();
  passed &= Require(
      !opt::CoordinateModelFamilySourceV1(bad_planning).accepted,
      "timestamp-free vector planning request was accepted");

  auto fixture = VectorExecutionRequest();
  const auto executed = exec::ExecuteModelFamilySourceV1(fixture.request);
  passed &= Require(
      executed.accepted && executed.execution_started &&
          executed.data_access_observed && executed.root_published &&
          executed.cleanup_complete && executed.cleanup_count == 1 &&
          *fixture.cleanup_count == 1 && executed.rows_examined == 2 &&
          executed.output.batch.rows.size() == 2 &&
          executed.output.ordered_row_identities.size() == 2,
      "canonical vector provider/exchange/executor leg did not publish: " +
          executed.diagnostic_id + " " + executed.detail);

  auto duplicate_output = VectorExecutionRequest();
  duplicate_output.request.input.output_descriptor_ids = {101, 102, 102};
  const auto duplicate_result =
      exec::ExecuteModelFamilySourceV1(duplicate_output.request);
  passed &= Require(!duplicate_result.accepted &&
                        !duplicate_result.execution_started &&
                        *duplicate_output.cleanup_count == 0,
                    "duplicate vector output handles reached provider access");

  auto cancellation = VectorExecutionRequest();
  cancellation.request.cancellation_requested = [] { return true; };
  const auto cancelled =
      exec::ExecuteModelFamilySourceV1(cancellation.request);
  passed &= Require(!cancelled.accepted && !cancelled.execution_started &&
                        !cancelled.data_access_observed &&
                        *cancellation.cleanup_count == 0 &&
                        cancelled.diagnostic_id ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1",
                    "pre-access vector cancellation did not fail closed");
  return passed;
}

std::uint64_t ProjectionNowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct RelationGenerationFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::string relation_uuid;
  std::string empty_relation_uuid;
  std::string chain_relation_uuid;
  std::string cross_relation_uuid;
  std::string other_relation_uuid;
  std::uint64_t uuid_salt = 0;

  ~RelationGenerationFixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

std::string ProjectionUuid(const platform::UuidKind kind,
                           const std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, ProjectionNowMillis() + salt);
  if (!generated.ok()) return {};
  return uuid::UuidToString(generated.value.value);
}

api::EngineRequestContext ProjectionBaseContext(
    const RelationGenerationFixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

bool ProjectionBegin(const RelationGenerationFixture& fixture,
                     std::string request_id,
                     api::EngineRequestContext* context) {
  if (context == nullptr) return false;
  api::EngineBeginTransactionRequest request;
  request.context = ProjectionBaseContext(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) return false;
  *context = request.context;
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool ProjectionCommit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return api::EngineCommitTransaction(request).ok;
}

bool ProjectionRollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return api::EngineRollbackTransaction(request).ok;
}

bool ProjectionPublishSnapshot(api::EngineRequestContext* context,
                               const std::uint64_t salt) {
  if (context == nullptr) return false;
  context->statement_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, salt);
  api::EnginePublishStatementSnapshotRequest request;
  request.context = *context;
  const auto published = api::EnginePublishStatementSnapshot(request);
  if (!published.ok) return false;
  context->statement_snapshot_uuid = published.statement_snapshot_uuid;
  context->snapshot_visible_through_local_transaction_id =
      published.snapshot_vector.visible_committed_high_watermark;
  return true;
}

bool ProjectionPersistRelation(const api::EngineRequestContext& context,
                               const std::string& relation_uuid) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = relation_uuid;
  table.default_name = "rcp077_generation_projection";
  table.columns = {{"value",
                    "canonical=int64;type_uuid=" + TestUuid(0x7700) +
                        ";nullable=true"}};
  if (api::AppendMgaTableMetadata(context, table).error) return false;
  api::MgaRelationStorageDescriptor descriptor;
  return !api::EnsureMgaRelationStorageDescriptor(context, table, {},
                                                   &descriptor)
              .error;
}

api::CrudRowVersionRecord ProjectionRow(
    const api::EngineRequestContext& context, const std::string& relation_uuid,
    const std::string& row_uuid, const std::string& version_uuid,
    const std::string& value) {
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = relation_uuid;
  row.row_uuid = row_uuid;
  row.version_uuid = version_uuid;
  row.values = {{"value", value}};
  return row;
}

bool ProjectionAppendRow(const api::EngineRequestContext& context,
                         const api::CrudRowVersionRecord& row,
                         std::uint64_t* event_sequence) {
  return !api::AppendMgaRowVersion(context, row, event_sequence).error;
}

api::MgaVisibleHeapRelationReadRequest ProjectionReadRequest(
    const std::string& relation_uuid) {
  api::MgaVisibleHeapRelationReadRequest request;
  request.relation_uuid = relation_uuid;
  request.maximum_scanned_row_versions = 64;
  request.maximum_decoded_bytes = 1U << 20U;
  request.maximum_output_rows = 16;
  request.cancellation_requested = [] { return false; };
  return request;
}

std::string ProjectionEvidence(
    const api::MgaVisibleHeapRelationReadResult& result,
    const std::string_view key) {
  const auto found = std::find_if(result.evidence.begin(),
                                  result.evidence.end(), [&](const auto& item) {
                                    return item.evidence_kind == key;
                                  });
  return found == result.evidence.end() ? std::string{}
                                        : found->evidence_id;
}

bool RelationBaseGenerationProjection() {
  bool passed = true;
  RelationGenerationFixture fixture;
  fixture.uuid_salt = ProjectionNowMillis() % 1'000'000;
  fixture.directory =
      std::filesystem::temp_directory_path() /
      ("scratchbird_rcp077_relation_generation_" +
       std::to_string(fixture.uuid_salt));
  std::error_code filesystem_error;
  std::filesystem::create_directories(fixture.directory, filesystem_error);
  if (!Require(!filesystem_error,
               "relation generation fixture directory creation failed")) {
    return false;
  }
  fixture.database_path = fixture.directory / "projection.sbdb";

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database,
      ProjectionNowMillis() + fixture.uuid_salt + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace,
      ProjectionNowMillis() + fixture.uuid_salt + 2);
  if (!Require(database_uuid.ok() && filespace_uuid.ok(),
               "relation generation fixture identity creation failed")) {
    return false;
  }
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = ProjectionNowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  if (!Require(db::CreateDatabaseFile(create).ok(),
               "relation generation database creation failed")) {
    return false;
  }

  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace_uuid.value.value);
  fixture.schema_uuid = ProjectionUuid(platform::UuidKind::object,
                                       fixture.uuid_salt + 10);
  fixture.principal_uuid = ProjectionUuid(platform::UuidKind::principal,
                                          fixture.uuid_salt + 11);
  fixture.session_uuid = ProjectionUuid(platform::UuidKind::object,
                                        fixture.uuid_salt + 12);
  fixture.relation_uuid = ProjectionUuid(platform::UuidKind::object,
                                         fixture.uuid_salt + 20);
  fixture.empty_relation_uuid = ProjectionUuid(platform::UuidKind::object,
                                               fixture.uuid_salt + 21);
  fixture.chain_relation_uuid = ProjectionUuid(platform::UuidKind::object,
                                               fixture.uuid_salt + 22);
  fixture.cross_relation_uuid = ProjectionUuid(platform::UuidKind::object,
                                               fixture.uuid_salt + 23);
  fixture.other_relation_uuid = ProjectionUuid(platform::UuidKind::object,
                                               fixture.uuid_salt + 24);

  api::EngineRequestContext metadata;
  if (!Require(ProjectionBegin(fixture, "rcp077-generation-metadata",
                               &metadata),
               "relation generation metadata transaction failed")) {
    return false;
  }
  for (const auto* relation :
       {&fixture.relation_uuid, &fixture.empty_relation_uuid,
        &fixture.chain_relation_uuid, &fixture.cross_relation_uuid,
        &fixture.other_relation_uuid}) {
    passed &= Require(ProjectionPersistRelation(metadata, *relation),
                      "relation generation descriptor persistence failed");
  }
  if (!passed ||
      !Require(ProjectionCommit(metadata),
               "relation generation metadata commit failed")) {
    return false;
  }

  api::EngineRequestContext writer;
  if (!Require(ProjectionBegin(fixture, "rcp077-generation-writer", &writer),
               "relation generation writer transaction failed")) {
    return false;
  }
  const auto live_row_uuid =
      ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 30);
  const auto live_v1_uuid =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 31);
  const auto live_v2_uuid =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 32);
  auto live_v1 = ProjectionRow(writer, fixture.relation_uuid, live_row_uuid,
                               live_v1_uuid, "10");
  std::uint64_t live_v1_generation = 0;
  passed &= Require(ProjectionAppendRow(writer, live_v1,
                                        &live_v1_generation),
                    "first live relation version append failed");
  auto live_v2 = ProjectionRow(writer, fixture.relation_uuid, live_row_uuid,
                               live_v2_uuid, "20");
  live_v2.previous_version_uuid = live_v1_uuid;
  live_v2.previous_sequence = live_v1_generation;
  std::uint64_t live_v2_generation = 0;
  passed &= Require(ProjectionAppendRow(writer, live_v2,
                                        &live_v2_generation),
                    "superseding live relation version append failed");

  const auto deleted_row_uuid =
      ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 33);
  const auto deleted_v1_uuid =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 34);
  auto deleted_v1 = ProjectionRow(writer, fixture.relation_uuid,
                                  deleted_row_uuid, deleted_v1_uuid, "30");
  std::uint64_t deleted_v1_generation = 0;
  passed &= Require(ProjectionAppendRow(writer, deleted_v1,
                                        &deleted_v1_generation),
                    "pre-tombstone relation version append failed");
  auto tombstone = ProjectionRow(
      writer, fixture.relation_uuid, deleted_row_uuid,
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 35), "");
  tombstone.previous_version_uuid = deleted_v1_uuid;
  tombstone.previous_sequence = deleted_v1_generation;
  tombstone.deleted = true;
  tombstone.values.clear();
  std::uint64_t tombstone_generation = 0;
  passed &= Require(ProjectionAppendRow(writer, tombstone,
                                        &tombstone_generation),
                    "newest tombstone append failed");
  if (!passed ||
      !Require(ProjectionCommit(writer),
               "relation generation writer commit failed")) {
    return false;
  }

  api::EngineRequestContext first_reader;
  passed &= Require(
      ProjectionBegin(fixture, "rcp077-generation-first-reader",
                      &first_reader) &&
          ProjectionPublishSnapshot(&first_reader, fixture.uuid_salt + 40),
      "relation generation first statement snapshot failed");
  if (!passed) return false;
  const auto live_read = api::ReadVisibleMgaHeapRelation(
      first_reader, ProjectionReadRequest(fixture.relation_uuid));
  passed &= Require(
      live_read.ok && live_read.visible_rows.size() == 1 &&
          live_read.visible_rows.front().version_uuid == live_v2_uuid &&
          live_read.tombstone_row_count == 1 &&
          live_read.current_relation_base_generation == tombstone_generation &&
          ProjectionEvidence(live_read,
                             "mga_heap_read_relation_base_generation") ==
              std::to_string(tombstone_generation),
      "live, superseded, and tombstoned base generation projection drifted");
  const auto empty_read = api::ReadVisibleMgaHeapRelation(
      first_reader, ProjectionReadRequest(fixture.empty_relation_uuid));
  passed &= Require(
      empty_read.ok && empty_read.visible_rows.empty() &&
          empty_read.current_relation_base_generation > 0 &&
          ProjectionEvidence(empty_read,
                             "mga_heap_read_relation_base_generation") ==
              std::to_string(empty_read.current_relation_base_generation),
      "metadata-only empty relation generation was not projected");
  passed &= Require(ProjectionRollback(first_reader),
                    "first relation generation reader rollback failed");
  if (!passed) return false;

  api::EngineRequestContext invisible_writer;
  if (!Require(ProjectionBegin(fixture, "rcp077-generation-invisible-writer",
                               &invisible_writer),
               "invisible relation generation writer begin failed")) {
    return false;
  }
  auto invisible = ProjectionRow(
      invisible_writer, fixture.relation_uuid,
      ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 41),
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 42),
      "40");
  std::uint64_t invisible_generation = 0;
  passed &= Require(ProjectionAppendRow(invisible_writer, invisible,
                                        &invisible_generation),
                    "invisible durable relation version append failed");

  api::EngineRequestContext reader;
  passed &= Require(
      ProjectionBegin(fixture, "rcp077-generation-reader", &reader) &&
          ProjectionPublishSnapshot(&reader, fixture.uuid_salt + 43),
      "relation generation statement snapshot failed");
  if (!passed) return false;
  const auto invisible_read = api::ReadVisibleMgaHeapRelation(
      reader, ProjectionReadRequest(fixture.relation_uuid));
  passed &= Require(
      invisible_read.ok && invisible_read.visible_rows.size() == 1 &&
          invisible_read.invisible_row_version_count >= 1 &&
          invisible_read.current_relation_base_generation ==
              invisible_generation,
      "invisible durable relation event was omitted from base generation");

  passed &= Require(!api::CreateMgaSavepointMarker(reader, "rcp077_sp").error,
                    "relation generation savepoint creation failed");
  auto rolled_back = ProjectionRow(
      reader, fixture.relation_uuid,
      ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 44),
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 45),
      "50");
  std::uint64_t rolled_back_generation = 0;
  passed &= Require(ProjectionAppendRow(reader, rolled_back,
                                        &rolled_back_generation),
                    "savepoint-private relation event append failed");
  passed &= Require(
      !api::RollbackToMgaSavepointMarker(reader, "rcp077_sp").error,
      "relation generation savepoint rollback failed");
  const auto savepoint_read = api::ReadVisibleMgaHeapRelation(
      reader, ProjectionReadRequest(fixture.relation_uuid));
  passed &= Require(
      savepoint_read.ok && rolled_back_generation > invisible_generation &&
          savepoint_read.current_relation_base_generation ==
              invisible_generation &&
          savepoint_read.visible_rows.size() == 1,
      "savepoint-rolled-back event changed the base generation");

  const auto require_zero_refusal = [&](const auto& result,
                                        const std::string_view detail) {
    return Require(!result.ok && result.current_relation_base_generation == 0 &&
                       result.visible_rows.empty(),
                   detail);
  };
  auto refused_request = ProjectionReadRequest(fixture.relation_uuid);
  refused_request.cancellation_requested = [] { return true; };
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(reader, refused_request),
      "cancelled relation read exposed a base generation");
  refused_request = ProjectionReadRequest(fixture.relation_uuid);
  refused_request.maximum_scanned_row_versions = 1;
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(reader, refused_request),
      "scan-bounded relation refusal exposed a base generation");
  refused_request = ProjectionReadRequest(fixture.relation_uuid);
  refused_request.maximum_decoded_bytes = 1;
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(reader, refused_request),
      "byte-bounded relation refusal exposed a base generation");
  refused_request = ProjectionReadRequest(fixture.relation_uuid);
  refused_request.maximum_output_rows = 0;
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(reader, refused_request),
      "output-bounded relation refusal exposed a base generation");
  refused_request = ProjectionReadRequest(
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 46));
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(reader, refused_request),
      "missing-descriptor relation refusal exposed a base generation");

  auto chain_base = ProjectionRow(
      reader, fixture.chain_relation_uuid,
      ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 47),
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 48),
      "60");
  std::uint64_t chain_base_generation = 0;
  passed &= Require(ProjectionAppendRow(reader, chain_base,
                                        &chain_base_generation),
                    "chain base relation event append failed");
  auto broken_chain = ProjectionRow(
      reader, fixture.chain_relation_uuid, chain_base.row_uuid,
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 49),
      "61");
  broken_chain.previous_version_uuid =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 50);
  broken_chain.previous_sequence = chain_base_generation;
  std::uint64_t broken_chain_generation = 0;
  passed &= Require(ProjectionAppendRow(reader, broken_chain,
                                        &broken_chain_generation),
                    "broken chain relation event append failed");
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(
          reader, ProjectionReadRequest(fixture.chain_relation_uuid)),
      "chain-invalid relation refusal exposed a base generation");

  auto foreign_row = ProjectionRow(
      reader, fixture.other_relation_uuid,
      ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 51),
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 52),
      "70");
  std::uint64_t foreign_generation = 0;
  passed &= Require(ProjectionAppendRow(reader, foreign_row,
                                        &foreign_generation),
                    "foreign relation event append failed");
  const auto scoped_root =
      fixture.database_path.string() + ".sb.mga_relation_scope/";
  const auto foreign_path = scoped_root + fixture.other_relation_uuid + ".rows";
  const auto cross_path = scoped_root + fixture.cross_relation_uuid + ".rows";
  std::ifstream foreign_in(foreign_path, std::ios::binary);
  const std::string foreign_bytes{std::istreambuf_iterator<char>(foreign_in),
                                  std::istreambuf_iterator<char>()};
  std::ofstream cross_out(cross_path, std::ios::binary | std::ios::app);
  cross_out.write(foreign_bytes.data(),
                  static_cast<std::streamsize>(foreign_bytes.size()));
  cross_out.flush();
  passed &= Require(!foreign_bytes.empty() && static_cast<bool>(cross_out),
                    "cross-relation refusal fixture injection failed");
  cross_out.close();
  passed &= require_zero_refusal(
      api::ReadVisibleMgaHeapRelation(
          reader, ProjectionReadRequest(fixture.cross_relation_uuid)),
      "cross-relation identity refusal exposed a base generation");

  passed &= Require(ProjectionRollback(reader),
                    "relation generation reader rollback failed");
  passed &= Require(ProjectionRollback(invisible_writer),
                    "invisible relation generation writer rollback failed");
  return passed;
}

std::string ProductionCoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : uuid::UuidToString(found->descriptor_uuid.value);
}

void AddProductionAuthorization(api::EngineRequestContext* context,
                                const std::string& object_uuid) {
  auto& authorization = context->authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, ProjectionNowMillis() + 301);
  authorization.principal_uuid = context->principal_uuid;
  authorization.security_epoch = context->security_epoch;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = context->catalog_generation_id;
  authorization.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, ProjectionNowMillis() + 302);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = object_uuid;
  grant.right = "SELECT";
  grant.security_epoch = context->security_epoch;
  authorization.grants.push_back(std::move(grant));
}

api::RelationalTypeDescriptor ProductionDagDescriptor(
    const std::uint32_t descriptor_id, std::string descriptor_uuid,
    std::string type_uuid, const bool width_three = false) {
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid = std::move(descriptor_uuid);
  descriptor.type_uuid = std::move(type_uuid);
  descriptor.nullability = api::RelationalNullability::kNonNull;
  if (width_three) descriptor.width = 3;
  return descriptor;
}

api::TypedRelationalDag ProductionVectorDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  const auto encoded_type_uuid = [](const std::string_view encoded) {
    constexpr std::string_view prefix = "type_uuid=";
    const auto begin = encoded.find(prefix);
    if (begin == std::string_view::npos) return std::string{};
    const auto value_begin = begin + prefix.size();
    const auto end = encoded.find(';', value_begin);
    return std::string(encoded.substr(
        value_begin, end == std::string_view::npos
                         ? encoded.size() - value_begin
                         : end - value_begin));
  };
  const auto uuid_type = ProductionCoreTypeUuid("uuid");
  const auto real64_type = ProductionCoreTypeUuid("real64");
  const auto uint64_type = ProductionCoreTypeUuid("uint64");
  const auto boolean_type = ProductionCoreTypeUuid("boolean");
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProjectionUuid(platform::UuidKind::object, ProjectionNowMillis() + 310);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;
  dag.descriptors = {
      ProductionDagDescriptor(101, uuid_type, uuid_type),
      ProductionDagDescriptor(
          102,
          ProjectionUuid(platform::UuidKind::object,
                         ProjectionNowMillis() + 311),
          real64_type),
      ProductionDagDescriptor(
          103,
          ProjectionUuid(platform::UuidKind::object,
                         ProjectionNowMillis() + 312),
          real64_type),
      ProductionDagDescriptor(
          104,
          storage.columns[0].value_descriptor.descriptor_uuid.canonical,
          encoded_type_uuid(
              storage.columns[0].value_descriptor.encoded_descriptor),
          true),
      ProductionDagDescriptor(
          105,
          storage.columns[1].value_descriptor.descriptor_uuid.canonical,
          encoded_type_uuid(
              storage.columns[1].value_descriptor.encoded_descriptor)),
      ProductionDagDescriptor(106, uint64_type, uint64_type),
      ProductionDagDescriptor(107, boolean_type, boolean_type),
  };
  static constexpr std::array<std::string_view, 3> kNames{
      "row_uuid", "distance", "score"};
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    api::RelationalExpressionRecord expression;
    expression.expression_id = static_cast<std::uint32_t>(ordinal + 1);
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    expression.bound_name_uuid = storage.relation_uuid.canonical;
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1,
         static_cast<std::uint32_t>(ordinal + 1), std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord alias;
  alias.expression_id = 4;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 104;
  alias.bound_name_uuid = storage.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  api::RelationalExpressionRecord query;
  query.expression_id = 5;
  query.expression_kind = api::RelationalExpressionKind::kLiteral;
  query.result_descriptor_id = 104;
  query.literal_kind = api::RelationalLiteralKind::kVector;
  query.literal_or_parameter_ref = "[1,0,0]";
  dag.expressions.push_back(std::move(query));
  api::RelationalExpressionRecord metric;
  metric.expression_id = 6;
  metric.expression_kind = api::RelationalExpressionKind::kLiteral;
  metric.result_descriptor_id = 105;
  metric.literal_kind = api::RelationalLiteralKind::kString;
  metric.literal_or_parameter_ref = "L2_SQUARED";
  dag.expressions.push_back(std::move(metric));
  api::RelationalExpressionRecord top_k;
  top_k.expression_id = 7;
  top_k.expression_kind = api::RelationalExpressionKind::kLiteral;
  top_k.result_descriptor_id = 106;
  top_k.literal_kind = api::RelationalLiteralKind::kNumeric;
  top_k.literal_or_parameter_ref = "2";
  dag.expressions.push_back(std::move(top_k));
  api::RelationalExpressionRecord nearest;
  nearest.expression_id = 8;
  nearest.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  nearest.child_expression_ids = {4, 5, 6, 7};
  nearest.result_descriptor_id = 107;
  nearest.operator_name = "VECTOR_NEAREST";
  dag.expressions.push_back(std::move(nearest));
  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  source.output_descriptor_ids = {101, 102, 103};
  source.bound_expression_ids = {1, 2, 3, 4, 5, 6, 7, 8};
  source.required_object_uuids = {storage.relation_uuid.canonical};
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

bool ProductionVectorRoute() {
  RelationGenerationFixture fixture;
  fixture.uuid_salt = ProjectionNowMillis() % 1'000'000;
  fixture.directory =
      std::filesystem::temp_directory_path() /
      ("scratchbird_rcp077_vector_production_" +
       std::to_string(fixture.uuid_salt));
  std::error_code filesystem_error;
  std::filesystem::create_directories(fixture.directory, filesystem_error);
  if (!Require(!filesystem_error,
               "production vector fixture directory creation failed")) {
    return false;
  }
  fixture.database_path = fixture.directory / "vector.sbdb";
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database,
      ProjectionNowMillis() + fixture.uuid_salt + 401);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace,
      ProjectionNowMillis() + fixture.uuid_salt + 402);
  if (!Require(database_uuid.ok() && filespace_uuid.ok(),
               "production vector database identity creation failed")) {
    return false;
  }
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = ProjectionNowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  if (!Require(db::CreateDatabaseFile(create).ok(),
               "production vector database creation failed")) {
    return false;
  }
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace_uuid.value.value);
  fixture.schema_uuid = ProjectionUuid(platform::UuidKind::object,
                                       fixture.uuid_salt + 410);
  fixture.principal_uuid = ProjectionUuid(platform::UuidKind::principal,
                                          fixture.uuid_salt + 411);
  fixture.session_uuid = ProjectionUuid(platform::UuidKind::object,
                                        fixture.uuid_salt + 412);
  fixture.relation_uuid = ProjectionUuid(platform::UuidKind::object,
                                         fixture.uuid_salt + 413);
  const auto dense_vector_type = ProductionCoreTypeUuid("dense_vector");
  const auto text_type = ProductionCoreTypeUuid("character");
  if (!Require(!dense_vector_type.empty() && !text_type.empty(),
               "production vector core type UUIDs are unavailable")) {
    return false;
  }

  api::EngineRequestContext metadata;
  if (!Require(ProjectionBegin(fixture, "rcp077-vector-production-metadata",
                               &metadata),
               "production vector metadata transaction failed")) {
    return false;
  }
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = fixture.relation_uuid;
  table.default_name = "rcp077_vector_production";
  table.columns = {
      {"embedding",
       "canonical=dense_vector;type_uuid=" + dense_vector_type +
           ";nullable=false;dimension=3;element_type=real32"},
      {"metadata", "canonical=text;type_uuid=" + text_type +
                       ";nullable=false"},
  };
  api::MgaRelationStorageDescriptor storage;
  if (!Require(!api::AppendMgaTableMetadata(metadata, table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(metadata, table, {},
                                                            &storage)
                        .error &&
                   ProjectionCommit(metadata),
               "production vector storage descriptor persistence failed")) {
    return false;
  }

  api::EngineRequestContext writer;
  if (!Require(ProjectionBegin(fixture, "rcp077-vector-production-writer",
                               &writer),
               "production vector writer transaction failed")) {
    return false;
  }
  struct VectorSeed {
    std::string embedding;
    std::string metadata;
    std::string row_uuid;
  };
  const std::array<VectorSeed, 3> seeds{{
      {"[1,0,0]", "{\"group\":\"a\"}",
       ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 420)},
      {"[0,1,0]", "{\"group\":\"b\"}",
       ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 421)},
      {"[0.5,0,0]", "{\"group\":\"a\"}",
       ProjectionUuid(platform::UuidKind::row, fixture.uuid_salt + 422)},
  }};
  for (std::size_t ordinal = 0; ordinal < seeds.size(); ++ordinal) {
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = fixture.relation_uuid;
    row.row_uuid = seeds[ordinal].row_uuid;
    row.version_uuid = ProjectionUuid(platform::UuidKind::object,
                                      fixture.uuid_salt + 430 + ordinal);
    row.values = {{"embedding", seeds[ordinal].embedding},
                  {"metadata", seeds[ordinal].metadata}};
    std::uint64_t event_sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &event_sequence).error &&
                     event_sequence != 0,
                 "production vector row persistence failed")) {
      return false;
    }
  }
  if (!Require(ProjectionCommit(writer),
               "production vector writer commit failed")) {
    return false;
  }

  api::EngineRequestContext reader;
  if (!Require(ProjectionBegin(fixture, "rcp077-vector-production-reader",
                               &reader),
               "production vector reader transaction failed")) {
    return false;
  }
  reader.statement_timestamp = "2026-08-11T01:02:03Z";
  if (!Require(ProjectionPublishSnapshot(&reader, fixture.uuid_salt + 440),
               "production vector statement snapshot failed")) {
    return false;
  }
  reader.statement_metadata_snapshot_engine_owned = true;
  reader.statement_metadata_snapshot_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 441);
  reader.statement_metadata_snapshot_visible_through_local_transaction_id =
      reader.snapshot_visible_through_local_transaction_id;
  reader.catalog_epoch_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 442);
  reader.optimizer_capability_snapshot_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 443);
  reader.optimizer_resource_snapshot_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 444);
  reader.optimizer_route_snapshot_uuid.canonical =
      ProjectionUuid(platform::UuidKind::object, fixture.uuid_salt + 445);
  reader.optimizer_route_epoch = 1;
  reader.optimizer_route_generation = 1;
  reader.optimizer_memory_budget_bytes = 16 * 1024 * 1024;
  reader.optimizer_maximum_candidate_count = 4096;
  reader.optimizer_maximum_memo_groups = 4096;
  reader.optimizer_maximum_search_steps = 16384;
  reader.optimizer_maximum_planning_time_ns = 1'000'000'000;
  reader.current_monotonic_ns = std::to_string(ProjectionNowMillis());
  reader.query_cancellation_requested = [] { return false; };
  AddProductionAuthorization(&reader, fixture.relation_uuid);
  const auto execution = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, ProductionVectorDag(reader, storage)});
  const bool exact =
      execution.profile_matched && execution.optimizer_admitted &&
      execution.optimizer_selected && execution.physical_dag_published &&
      execution.physical_dag_executed && execution.runtime_actuals_attached &&
      execution.canonical_result_published && execution.api_result.ok &&
      execution.physical_node_count == 1 &&
      execution.canonical_result_column_count == 3 &&
      execution.canonical_result_row_count == 2 &&
      execution.api_result.result_shape.rows.size() == 2 &&
      execution.api_result.result_shape.rows[0].fields.size() == 3 &&
      execution.api_result.result_shape.rows[1].fields.size() == 3 &&
      execution.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
          seeds[0].row_uuid &&
      execution.api_result.result_shape.rows[0].fields[1].second.encoded_value ==
          "0" &&
      execution.api_result.result_shape.rows[1].fields[0].second.encoded_value ==
          seeds[2].row_uuid &&
      execution.api_result.result_shape.rows[1].fields[1].second.encoded_value ==
          "0.25";
  const auto diagnostic = execution.api_result.diagnostics.empty()
                              ? std::string{}
                              : execution.api_result.diagnostics.front().code +
                                    ":" + execution.api_result.diagnostics.front().detail;
  const bool rolled_back = ProjectionRollback(reader);
  return Require(exact,
                 "production canonical vector route drifted: " + diagnostic) &&
         Require(rolled_back,
                 "production canonical vector reader rollback failed");
}

api::EngineNoSqlProviderGenerationMetadata KatMetadata() {
  api::EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = api::EngineNoSqlProviderFamily::kVector;
  metadata.provider_id = "60000000-0000-4000-8000-000000000100";
  metadata.database_identity = "database-identity-kat-v1";
  metadata.database_uuid = "60000000-0000-4000-8000-000000000101";
  metadata.collection_uuid = "60000000-0000-4000-8000-000000000077";
  metadata.generation_uuid = "60000000-0000-4000-8000-000000000102";
  metadata.generation_id = 7;
  metadata.descriptor_epoch = 11;
  metadata.security_epoch = 13;
  metadata.redaction_epoch = 13;
  metadata.catalog_epoch = 17;
  metadata.publish_state = "published";
  metadata.validation_state = "validated";
  metadata.backup_metadata_ref = "kat-backup-v1";
  metadata.restore_metadata_ref = "kat-restore-v1";
  metadata.repair_metadata_ref = "kat-repair-v1";
  metadata.support_bundle_evidence_id = "kat-support-bundle-v1";
  metadata.vector_ann_candidate_present = true;
  metadata.vector_ann_capability_uuid = std::string(kExpectedCapability);
  metadata.vector_ann_index_uuid = "60000000-0000-4000-8000-000000000103";
  metadata.vector_ann_base_relation_uuid = metadata.collection_uuid;
  metadata.vector_ann_base_relation_generation = 19;
  metadata.vector_ann_relation_descriptor_uuid =
      "60000000-0000-4000-8000-000000000104";
  metadata.vector_ann_relation_descriptor_generation = 23;
  metadata.vector_ann_embedding_column_uuid =
      "60000000-0000-4000-8000-000000000105";
  metadata.vector_ann_embedding_descriptor_uuid =
      "60000000-0000-4000-8000-000000000106";
  metadata.vector_ann_embedding_type_uuid =
      "60000000-0000-4000-8000-000000000107";
  metadata.vector_ann_dimension = 3;
  metadata.vector_ann_element_profile = "real32";
  metadata.vector_ann_metric_id = "L2_SQUARED";
  metadata.vector_ann_algorithm_id = "hnsw";
  metadata.vector_ann_publish_attestation_state =
      "VECTOR_ANN_SECTION_8_FULL_BASE_EXACT_V1";
  metadata.vector_ann_checksum_valid = true;
  metadata.vector_ann_sealed_generation = true;
  metadata.vector_ann_recall_attestation_present = true;
  metadata.vector_ann_recall_contract_top_k = 10;
  metadata.vector_ann_recall_sample_rows = 100;
  metadata.vector_ann_required_recall_ppm = 950000;
  metadata.vector_ann_observed_recall_ppm = 970000;
  metadata.vector_ann_recall_sample_deterministic = true;
  metadata.vector_ann_recall_evidence_uuid =
      "60000000-0000-4000-8000-000000000108";
  metadata.vector_ann_statement_uuid =
      "60000000-0000-4000-8000-000000000700";
  metadata.vector_ann_statement_snapshot_uuid =
      "60000000-0000-4000-8000-000000000701";
  metadata.vector_ann_statement_metadata_snapshot_uuid =
      "60000000-0000-4000-8000-000000000702";
  metadata.vector_ann_owning_transaction_uuid =
      "60000000-0000-4000-8000-000000000703";
  metadata.vector_ann_local_transaction_id = 800;
  metadata.vector_ann_snapshot_visible_through_local_transaction_id = 900;
  metadata.vector_ann_security_context_uuid =
      "60000000-0000-4000-8000-000000000704";
  metadata.vector_ann_catalog_epoch_uuid =
      "60000000-0000-4000-8000-000000000705";
  metadata.vector_ann_exact_fallback_available = true;
  metadata.vector_ann_full_base_exact_recheck_required = true;
  metadata.vector_ann_base_row_mga_recheck_required = true;
  metadata.vector_ann_security_recheck_required = true;
  return metadata;
}

void AppendLengthPrefixed(const std::string_view value, std::string* out) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string LocalKatSeed(
    const api::EngineNoSqlProviderGenerationMetadata& metadata) {
  std::string seed;
  const auto field = [&](const std::string_view name,
                         const std::string_view value) {
    AppendLengthPrefixed(name, &seed);
    AppendLengthPrefixed(value, &seed);
  };
  const auto number = [&](const std::string_view name, const std::uint64_t value) {
    field(name, std::to_string(value));
  };
  const auto boolean = [&](const std::string_view name, const bool value) {
    field(name, value ? "true" : "false");
  };
  AppendLengthPrefixed("SCRATCHBIRD.VECTOR_ANN_CAPABILITY_BINDING.V1", &seed);
  field("family", api::EngineNoSqlProviderFamilyName(metadata.family));
  field("provider_id", metadata.provider_id);
  field("database_identity", metadata.database_identity);
  field("database_uuid", metadata.database_uuid);
  field("collection_uuid", metadata.collection_uuid);
  field("generation_uuid", metadata.generation_uuid);
  number("generation_id", metadata.generation_id);
  number("descriptor_epoch", metadata.descriptor_epoch);
  number("security_epoch", metadata.security_epoch);
  number("redaction_epoch", metadata.redaction_epoch);
  number("catalog_epoch", metadata.catalog_epoch);
  field("publish_state", metadata.publish_state);
  field("validation_state", metadata.validation_state);
  boolean("provider_claims_transaction_finality_authority",
          metadata.provider_claims_transaction_finality_authority);
  boolean("provider_claims_visibility_authority",
          metadata.provider_claims_visibility_authority);
  boolean("vector_ann_candidate_present", metadata.vector_ann_candidate_present);
  field("vector_ann_index_uuid", metadata.vector_ann_index_uuid);
  field("vector_ann_base_relation_uuid", metadata.vector_ann_base_relation_uuid);
  number("vector_ann_base_relation_generation",
         metadata.vector_ann_base_relation_generation);
  field("vector_ann_relation_descriptor_uuid",
        metadata.vector_ann_relation_descriptor_uuid);
  number("vector_ann_relation_descriptor_generation",
         metadata.vector_ann_relation_descriptor_generation);
  field("vector_ann_embedding_column_uuid",
        metadata.vector_ann_embedding_column_uuid);
  field("vector_ann_embedding_descriptor_uuid",
        metadata.vector_ann_embedding_descriptor_uuid);
  field("vector_ann_embedding_type_uuid",
        metadata.vector_ann_embedding_type_uuid);
  number("vector_ann_dimension", metadata.vector_ann_dimension);
  field("vector_ann_element_profile", metadata.vector_ann_element_profile);
  field("vector_ann_metric_id", metadata.vector_ann_metric_id);
  field("vector_ann_algorithm_id", metadata.vector_ann_algorithm_id);
  field("vector_ann_publish_attestation_state",
        metadata.vector_ann_publish_attestation_state);
  boolean("vector_ann_checksum_valid", metadata.vector_ann_checksum_valid);
  boolean("vector_ann_sealed_generation", metadata.vector_ann_sealed_generation);
  boolean("vector_ann_recall_attestation_present",
          metadata.vector_ann_recall_attestation_present);
  number("vector_ann_recall_contract_top_k",
         metadata.vector_ann_recall_contract_top_k);
  number("vector_ann_recall_sample_rows", metadata.vector_ann_recall_sample_rows);
  number("vector_ann_required_recall_ppm",
         metadata.vector_ann_required_recall_ppm);
  number("vector_ann_observed_recall_ppm",
         metadata.vector_ann_observed_recall_ppm);
  boolean("vector_ann_recall_sample_deterministic",
          metadata.vector_ann_recall_sample_deterministic);
  field("vector_ann_recall_evidence_uuid",
        metadata.vector_ann_recall_evidence_uuid);
  field("vector_ann_statement_uuid", metadata.vector_ann_statement_uuid);
  field("vector_ann_statement_snapshot_uuid",
        metadata.vector_ann_statement_snapshot_uuid);
  field("vector_ann_statement_metadata_snapshot_uuid",
        metadata.vector_ann_statement_metadata_snapshot_uuid);
  field("vector_ann_owning_transaction_uuid",
        metadata.vector_ann_owning_transaction_uuid);
  number("vector_ann_local_transaction_id",
         metadata.vector_ann_local_transaction_id);
  number("vector_ann_snapshot_visible_through_local_transaction_id",
         metadata.vector_ann_snapshot_visible_through_local_transaction_id);
  field("vector_ann_security_context_uuid",
        metadata.vector_ann_security_context_uuid);
  field("vector_ann_catalog_epoch_uuid", metadata.vector_ann_catalog_epoch_uuid);
  boolean("vector_ann_exact_fallback_available",
          metadata.vector_ann_exact_fallback_available);
  boolean("vector_ann_full_base_exact_recheck_required",
          metadata.vector_ann_full_base_exact_recheck_required);
  boolean("vector_ann_base_row_mga_recheck_required",
          metadata.vector_ann_base_row_mga_recheck_required);
  boolean("vector_ann_security_recheck_required",
          metadata.vector_ann_security_recheck_required);
  boolean("vector_ann_index_claims_visibility_authority",
          metadata.vector_ann_index_claims_visibility_authority);
  boolean("vector_ann_index_claims_transaction_finality_authority",
          metadata.vector_ann_index_claims_transaction_finality_authority);
  boolean("vector_ann_parser_claims_visibility_authority",
          metadata.vector_ann_parser_claims_visibility_authority);
  boolean("vector_ann_parser_claims_transaction_finality_authority",
          metadata.vector_ann_parser_claims_transaction_finality_authority);
  boolean("vector_ann_client_claims_visibility_authority",
          metadata.vector_ann_client_claims_visibility_authority);
  boolean("vector_ann_client_claims_transaction_finality_authority",
          metadata.vector_ann_client_claims_transaction_finality_authority);
  boolean("vector_ann_reference_claims_visibility_authority",
          metadata.vector_ann_reference_claims_visibility_authority);
  boolean("vector_ann_reference_claims_transaction_finality_authority",
          metadata.vector_ann_reference_claims_transaction_finality_authority);
  boolean("vector_ann_wal_claims_visibility_authority",
          metadata.vector_ann_wal_claims_visibility_authority);
  boolean("vector_ann_wal_claims_transaction_finality_authority",
          metadata.vector_ann_wal_claims_transaction_finality_authority);
  return seed;
}

std::string LocalCapabilityUuid(const hash::Digest256& digest) {
  std::array<std::uint8_t, 16> bytes{};
  std::copy_n(digest.begin(), bytes.size(), bytes.begin());
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x80U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

bool CarrierKat() {
  const auto metadata = KatMetadata();
  const auto seed = LocalKatSeed(metadata);
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const platform::byte*>(seed.data()), seed.size());
  if (!Require(seed.size() == 3105, "KAT seed length drifted") ||
      !Require(digest.ok(), "KAT seed hash failed") ||
      !Require(hash::HexLower(digest.digest) == kExpectedDigest,
               "KAT SHA-256 drifted") ||
      !Require(LocalCapabilityUuid(digest.digest) == kExpectedCapability,
               "test-local KAT UUID drifted") ||
      !Require(api::DeriveVectorAnnCapabilityUuidV1(metadata) ==
                   kExpectedCapability,
               "production KAT UUID drifted") ||
      !Require(api::ValidateVectorAnnCapabilityBindingV1(metadata),
               "production KAT binding was refused")) {
    return false;
  }
  auto corrupt = metadata;
  corrupt.vector_ann_capability_uuid.back() = '9';
  return Require(!api::ValidateVectorAnnCapabilityBindingV1(corrupt),
                 "corrupt capability binding was accepted");
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

std::string PairValue(const Pairs& pairs, const std::string_view key) {
  const auto found = std::find_if(pairs.begin(), pairs.end(),
                                  [&](const auto& pair) {
                                    return pair.first == key;
                                  });
  return found == pairs.end() ? std::string{} : found->second;
}

bool SetPair(Pairs* pairs, const std::string_view key,
             const std::string_view value) {
  if (pairs == nullptr) return false;
  const auto found = std::find_if(pairs->begin(), pairs->end(),
                                  [&](const auto& pair) {
                                    return pair.first == key;
                                  });
  if (found == pairs->end()) return false;
  found->second = value;
  return true;
}

bool Contains(const std::vector<std::string_view>& fields,
              const std::string_view field) {
  return std::find(fields.begin(), fields.end(), field) != fields.end();
}

const std::vector<std::string_view>& VectorBoolFields() {
  static const std::vector<std::string_view> fields{
      "vector_ann_candidate_present",
      "vector_ann_checksum_valid",
      "vector_ann_sealed_generation",
      "vector_ann_recall_attestation_present",
      "vector_ann_recall_sample_deterministic",
      "vector_ann_exact_fallback_available",
      "vector_ann_full_base_exact_recheck_required",
      "vector_ann_base_row_mga_recheck_required",
      "vector_ann_security_recheck_required",
      "vector_ann_index_claims_visibility_authority",
      "vector_ann_index_claims_transaction_finality_authority",
      "vector_ann_parser_claims_visibility_authority",
      "vector_ann_parser_claims_transaction_finality_authority",
      "vector_ann_client_claims_visibility_authority",
      "vector_ann_client_claims_transaction_finality_authority",
      "vector_ann_reference_claims_visibility_authority",
      "vector_ann_reference_claims_transaction_finality_authority",
      "vector_ann_wal_claims_visibility_authority",
      "vector_ann_wal_claims_transaction_finality_authority",
  };
  return fields;
}

const std::vector<std::string_view>& NumericSeedFields() {
  static const std::vector<std::string_view> fields{
      "generation_id",
      "descriptor_epoch",
      "security_epoch",
      "redaction_epoch",
      "catalog_epoch",
      "vector_ann_base_relation_generation",
      "vector_ann_relation_descriptor_generation",
      "vector_ann_dimension",
      "vector_ann_recall_contract_top_k",
      "vector_ann_recall_sample_rows",
      "vector_ann_required_recall_ppm",
      "vector_ann_observed_recall_ppm",
      "vector_ann_local_transaction_id",
      "vector_ann_snapshot_visible_through_local_transaction_id",
  };
  return fields;
}

const std::vector<std::string_view>& SeedFields() {
  static const std::vector<std::string_view> fields{
      "family",
      "provider_id",
      "database_identity",
      "database_uuid",
      "collection_uuid",
      "generation_uuid",
      "generation_id",
      "descriptor_epoch",
      "security_epoch",
      "redaction_epoch",
      "catalog_epoch",
      "publish_state",
      "validation_state",
      "provider_claims_transaction_finality_authority",
      "provider_claims_visibility_authority",
      "vector_ann_candidate_present",
      "vector_ann_index_uuid",
      "vector_ann_base_relation_uuid",
      "vector_ann_base_relation_generation",
      "vector_ann_relation_descriptor_uuid",
      "vector_ann_relation_descriptor_generation",
      "vector_ann_embedding_column_uuid",
      "vector_ann_embedding_descriptor_uuid",
      "vector_ann_embedding_type_uuid",
      "vector_ann_dimension",
      "vector_ann_element_profile",
      "vector_ann_metric_id",
      "vector_ann_algorithm_id",
      "vector_ann_publish_attestation_state",
      "vector_ann_checksum_valid",
      "vector_ann_sealed_generation",
      "vector_ann_recall_attestation_present",
      "vector_ann_recall_contract_top_k",
      "vector_ann_recall_sample_rows",
      "vector_ann_required_recall_ppm",
      "vector_ann_observed_recall_ppm",
      "vector_ann_recall_sample_deterministic",
      "vector_ann_recall_evidence_uuid",
      "vector_ann_statement_uuid",
      "vector_ann_statement_snapshot_uuid",
      "vector_ann_statement_metadata_snapshot_uuid",
      "vector_ann_owning_transaction_uuid",
      "vector_ann_local_transaction_id",
      "vector_ann_snapshot_visible_through_local_transaction_id",
      "vector_ann_security_context_uuid",
      "vector_ann_catalog_epoch_uuid",
      "vector_ann_exact_fallback_available",
      "vector_ann_full_base_exact_recheck_required",
      "vector_ann_base_row_mga_recheck_required",
      "vector_ann_security_recheck_required",
      "vector_ann_index_claims_visibility_authority",
      "vector_ann_index_claims_transaction_finality_authority",
      "vector_ann_parser_claims_visibility_authority",
      "vector_ann_parser_claims_transaction_finality_authority",
      "vector_ann_client_claims_visibility_authority",
      "vector_ann_client_claims_transaction_finality_authority",
      "vector_ann_reference_claims_visibility_authority",
      "vector_ann_reference_claims_transaction_finality_authority",
      "vector_ann_wal_claims_visibility_authority",
      "vector_ann_wal_claims_transaction_finality_authority",
  };
  return fields;
}

const std::vector<std::string_view>& VectorCarrierFields() {
  static const std::vector<std::string_view> fields{
      "vector_ann_candidate_present",
      "vector_ann_capability_uuid",
      "vector_ann_index_uuid",
      "vector_ann_base_relation_uuid",
      "vector_ann_base_relation_generation",
      "vector_ann_relation_descriptor_uuid",
      "vector_ann_relation_descriptor_generation",
      "vector_ann_embedding_column_uuid",
      "vector_ann_embedding_descriptor_uuid",
      "vector_ann_embedding_type_uuid",
      "vector_ann_dimension",
      "vector_ann_element_profile",
      "vector_ann_metric_id",
      "vector_ann_algorithm_id",
      "vector_ann_publish_attestation_state",
      "vector_ann_checksum_valid",
      "vector_ann_sealed_generation",
      "vector_ann_recall_attestation_present",
      "vector_ann_recall_contract_top_k",
      "vector_ann_recall_sample_rows",
      "vector_ann_required_recall_ppm",
      "vector_ann_observed_recall_ppm",
      "vector_ann_recall_sample_deterministic",
      "vector_ann_recall_evidence_uuid",
      "vector_ann_statement_uuid",
      "vector_ann_statement_snapshot_uuid",
      "vector_ann_statement_metadata_snapshot_uuid",
      "vector_ann_owning_transaction_uuid",
      "vector_ann_local_transaction_id",
      "vector_ann_snapshot_visible_through_local_transaction_id",
      "vector_ann_security_context_uuid",
      "vector_ann_catalog_epoch_uuid",
      "vector_ann_exact_fallback_available",
      "vector_ann_full_base_exact_recheck_required",
      "vector_ann_base_row_mga_recheck_required",
      "vector_ann_security_recheck_required",
      "vector_ann_index_claims_visibility_authority",
      "vector_ann_index_claims_transaction_finality_authority",
      "vector_ann_parser_claims_visibility_authority",
      "vector_ann_parser_claims_transaction_finality_authority",
      "vector_ann_client_claims_visibility_authority",
      "vector_ann_client_claims_transaction_finality_authority",
      "vector_ann_reference_claims_visibility_authority",
      "vector_ann_reference_claims_transaction_finality_authority",
      "vector_ann_wal_claims_visibility_authority",
      "vector_ann_wal_claims_transaction_finality_authority",
  };
  return fields;
}

const std::vector<std::string_view>& ActiveRequiredFields() {
  static const std::vector<std::string_view> fields{
      "vector_ann_capability_uuid",
      "vector_ann_index_uuid",
      "vector_ann_base_relation_uuid",
      "vector_ann_base_relation_generation",
      "vector_ann_relation_descriptor_uuid",
      "vector_ann_relation_descriptor_generation",
      "vector_ann_embedding_column_uuid",
      "vector_ann_embedding_descriptor_uuid",
      "vector_ann_embedding_type_uuid",
      "vector_ann_dimension",
      "vector_ann_element_profile",
      "vector_ann_metric_id",
      "vector_ann_algorithm_id",
      "vector_ann_publish_attestation_state",
      "vector_ann_checksum_valid",
      "vector_ann_sealed_generation",
      "vector_ann_recall_attestation_present",
      "vector_ann_recall_contract_top_k",
      "vector_ann_recall_sample_rows",
      "vector_ann_required_recall_ppm",
      "vector_ann_observed_recall_ppm",
      "vector_ann_recall_sample_deterministic",
      "vector_ann_recall_evidence_uuid",
      "vector_ann_statement_uuid",
      "vector_ann_statement_snapshot_uuid",
      "vector_ann_statement_metadata_snapshot_uuid",
      "vector_ann_owning_transaction_uuid",
      "vector_ann_local_transaction_id",
      "vector_ann_snapshot_visible_through_local_transaction_id",
      "vector_ann_security_context_uuid",
      "vector_ann_catalog_epoch_uuid",
      "vector_ann_exact_fallback_available",
      "vector_ann_full_base_exact_recheck_required",
      "vector_ann_base_row_mga_recheck_required",
      "vector_ann_security_recheck_required",
  };
  return fields;
}

std::string DefaultVectorValue(const std::string_view field) {
  if (Contains(VectorBoolFields(), field)) return "false";
  if (Contains(NumericSeedFields(), field)) return "0";
  return {};
}

std::string DistinctSeedValue(const std::string_view field,
                              const std::string& current) {
  if (field == "provider_claims_transaction_finality_authority" ||
      field == "provider_claims_visibility_authority" ||
      Contains(VectorBoolFields(), field)) {
    return current == "true" ? "false" : "true";
  }
  if (Contains(NumericSeedFields(), field)) {
    return std::to_string(std::stoull(current) + 1);
  }
  if (field == "family") return "document";
  if (current.size() == 36 && current[8] == '-' && current[13] == '-' &&
      current[18] == '-' && current[23] == '-') {
    auto changed = current;
    changed.back() = changed.back() == 'f' ? 'e' : 'f';
    return changed;
  }
  return current + ".mutated";
}

bool RawPersistenceMutationMatrix() {
  if (!Require(SeedFields().size() == 60, "seed mutation inventory drifted") ||
      !Require(VectorCarrierFields().size() == 46,
               "carrier field inventory drifted") ||
      !Require(ActiveRequiredFields().size() == 35,
               "required-active inventory drifted")) {
    return false;
  }

  const auto original_directory = std::filesystem::current_path();
  const auto unique = std::to_string(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("scratchbird_rcp077_vector_kat_" + unique);
  std::error_code filesystem_error;
  std::filesystem::create_directories(scratch, filesystem_error);
  if (!Require(!filesystem_error, "could not create mutation scratch")) {
    return false;
  }
  std::filesystem::current_path(scratch, filesystem_error);
  if (!Require(!filesystem_error, "could not enter mutation scratch")) {
    std::filesystem::remove_all(scratch, filesystem_error);
    return false;
  }

  api::EngineRequestContext context;
  context.request_id = "RCP-077-VECTOR-ANN-CARRIER-KAT-V1";
  context.database_path = "database-identity-kat-v1";
  context.database_uuid.canonical =
      "60000000-0000-4000-8000-000000000101";
  const auto store_path = std::filesystem::path(
      context.database_path + ".sb.nosql_provider_generations");
  const auto metadata = KatMetadata();

  const auto restore_directory = [&]() {
    (void)api::CleanupNoSqlProviderGenerations(context, true);
    std::filesystem::current_path(original_directory, filesystem_error);
    std::filesystem::remove_all(scratch, filesystem_error);
  };
  const auto published = api::PublishNoSqlProviderGeneration(context, metadata);
  if (!Require(published.ok, "KAT carrier publication failed")) {
    restore_directory();
    return false;
  }

  std::string baseline_bytes;
  {
    std::ifstream in(store_path, std::ios::binary);
    baseline_bytes.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
  }
  const auto first_tab = baseline_bytes.find('\t');
  const auto second_tab = baseline_bytes.find('\t', first_tab + 1);
  const auto newline = baseline_bytes.find('\n', second_tab + 1);
  if (!Require(first_tab != std::string::npos &&
                   second_tab != std::string::npos &&
                   newline != std::string::npos,
               "KAT persistence envelope malformed")) {
    restore_directory();
    return false;
  }
  const auto baseline_pairs = api::DecodeCrudPairs(
      baseline_bytes.substr(second_tab + 1, newline - second_tab - 1));
  const auto write_bytes = [&](const std::string& bytes) {
    std::ofstream out(store_path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
  };
  const auto write_pairs = [&](const Pairs& pairs) {
    return write_bytes("SBNOSQLPG1\tGENERATION\t" +
                       api::EncodeCrudPairs(pairs) + "\n");
  };
  const auto evict = [&]() {
    return api::CleanupNoSqlProviderGenerations(context, false).ok;
  };
  const auto load = [&]() {
    return api::LoadNoSqlProviderGeneration(
        context, api::EngineNoSqlProviderFamily::kVector,
        metadata.provider_id, metadata.collection_uuid);
  };
  const auto baseline_valid = [&]() {
    const auto loaded = load();
    return loaded.ok &&
           api::ValidateVectorAnnCapabilityBindingV1(loaded.metadata) &&
           loaded.metadata.vector_ann_capability_uuid == kExpectedCapability;
  };
  const auto run_case = [&](const std::string_view label, const Pairs& pairs) {
    if (!write_bytes(baseline_bytes) || !evict() || !baseline_valid() ||
        !write_pairs(pairs) || !evict()) {
      return Require(false, std::string(label) + " fixture transition failed");
    }
    const auto refused = !load().ok;
    const bool restored = write_bytes(baseline_bytes) && evict() &&
                          baseline_valid();
    return Require(refused && restored,
                   std::string(label) + " did not fail closed and restore");
  };

  bool exact = evict() && baseline_valid();
  std::size_t mutation_count = 0;
  for (const auto field : SeedFields()) {
    auto pairs = baseline_pairs;
    ++mutation_count;
    std::ostringstream id;
    id << "KAT-SEED-MUT-" << std::setw(3) << std::setfill('0')
       << mutation_count;
    const bool prepared = SetPair(
        &pairs, field, DistinctSeedValue(field, PairValue(pairs, field)));
    const bool ran = run_case(id.str(), pairs);
    exact = prepared && ran && exact;
  }
  {
    auto pairs = baseline_pairs;
    auto capability = PairValue(pairs, "vector_ann_capability_uuid");
    capability.back() = capability.back() == '9' ? '8' : '9';
    const bool prepared =
        SetPair(&pairs, "vector_ann_capability_uuid", capability);
    const bool ran = run_case("KAT-CAPABILITY-MUT-061", pairs);
    exact = prepared && ran && exact;
    ++mutation_count;
  }

  auto inactive_pairs = baseline_pairs;
  for (const auto field : VectorCarrierFields()) {
    const bool prepared =
        SetPair(&inactive_pairs, field, DefaultVectorValue(field));
    exact = prepared && exact;
  }
  for (const auto field : VectorCarrierFields()) {
    if (field == std::string_view("vector_ann_candidate_present")) continue;
    auto pairs = inactive_pairs;
    auto nondefault = PairValue(baseline_pairs, field);
    if (nondefault == DefaultVectorValue(field)) nondefault = "true";
    ++mutation_count;
    std::ostringstream id;
    id << "KAT-INACTIVE-PARTIAL-" << std::setw(3) << std::setfill('0')
       << mutation_count;
    const bool prepared = SetPair(&pairs, field, nondefault);
    const bool ran = run_case(id.str(), pairs);
    exact = prepared && ran && exact;
  }

  for (const auto field : ActiveRequiredFields()) {
    auto pairs = baseline_pairs;
    ++mutation_count;
    std::ostringstream id;
    id << "KAT-ACTIVE-MISSING-" << std::setw(3) << std::setfill('0')
       << mutation_count;
    const bool prepared =
        SetPair(&pairs, field, DefaultVectorValue(field));
    const bool ran = run_case(id.str(), pairs);
    exact = prepared && ran && exact;
  }

  exact = exact && mutation_count == 141;

  struct SupplementalRawProbe {
    std::string_view id;
    std::string_view field;
    std::string_view raw_value;
  };
  for (const auto& probe : std::array{
           SupplementalRawProbe{"KAT-RAW-NORMALIZATION-FAMILY-ALIAS",
                                "family", "nosql.vector"},
           SupplementalRawProbe{"KAT-RAW-NORMALIZATION-BOOL-UPPERCASE",
                                "vector_ann_checksum_valid", "TRUE"},
           SupplementalRawProbe{"KAT-RAW-NORMALIZATION-U64-LEADING-ZERO",
                                "generation_id", "007"},
       }) {
    auto pairs = baseline_pairs;
    const bool prepared = SetPair(&pairs, probe.field, probe.raw_value);
    const bool ran = run_case(probe.id, pairs);
    exact = prepared && ran && exact;
  }
  auto duplicated_key_pairs = baseline_pairs;
  duplicated_key_pairs.emplace_back("vector_ann_capability_uuid",
                                    std::string(kExpectedCapability));
  const bool duplicated_key_ran = run_case(
      "KAT-RAW-NORMALIZATION-DUPLICATE-IDENTICAL-KEY",
      duplicated_key_pairs);
  exact = duplicated_key_ran && exact;

  // Raw duplicate active carriers must survive loading as a cohort and be
  // refused.  A malformed DROP must also remain corruption evidence rather
  // than erase the current generation into benign absence.
  const bool duplicate_written = write_bytes(baseline_bytes + baseline_bytes);
  const bool duplicate_evicted = evict();
  const bool duplicate_refused = !load().ok;
  exact = duplicate_written && duplicate_evicted && duplicate_refused && exact;
  auto malformed_drop_pairs = baseline_pairs;
  malformed_drop_pairs.erase(
      std::remove_if(malformed_drop_pairs.begin(), malformed_drop_pairs.end(),
                     [](const auto& pair) {
                       return pair.first == "vector_ann_metric_id";
                     }),
      malformed_drop_pairs.end());
  const bool malformed_drop_written = write_bytes(
      baseline_bytes + "SBNOSQLPG1\tDROP\t" +
      api::EncodeCrudPairs(malformed_drop_pairs) + "\n");
  const bool malformed_drop_evicted = evict();
  const bool malformed_drop_refused = !load().ok;
  exact = malformed_drop_written && malformed_drop_evicted &&
          malformed_drop_refused && exact;

  restore_directory();
  return Require(exact, "141-case raw persistence matrix drifted");
}
}  // namespace

int main() {
  if (!CarrierKat() || !CanonicalVectorSpine() ||
      !RelationBaseGenerationProjection() ||
      !RawPersistenceMutationMatrix()
#if defined(SB_CES05_VECTOR_PRODUCTION_QUERY_ROUTE)
      || !ProductionVectorRoute()
#endif
  ) {
    return 1;
  }
  std::cout << "RCP-077 vector carrier, MGA base generation, canonical spine, "
               "and 141 raw mutations: PASS\n";
  return 0;
}
