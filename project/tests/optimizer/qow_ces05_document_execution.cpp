// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_coordinator.hpp"
#include "model_family_executor.hpp"
#include "logical_plan.hpp"
#include "hash_digest.hpp"

#if defined(SB_CES05_PRODUCTION_QUERY_ROUTE)
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "ddl/create_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/document_api.hpp"
#include "canonical_query_execute.hpp"
#include "canonical_aggregate_registry.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#endif

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
#if defined(SB_CES05_PRODUCTION_QUERY_ROUTE)
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
#endif

struct FixtureSchemaField {
  const char* field_id;
  const char* type;
  bool nullable;
};

struct FixtureLiteralRow {
  const char* row_uuid;
  std::int64_t join_key;
  const char* payload;
};

struct FixtureGenerations {
  std::uint64_t catalog;
  std::uint64_t descriptor;
  std::uint64_t security;
  std::uint64_t policy;
  std::uint64_t provider;
  std::uint64_t route;
  std::uint64_t resource;
};

struct VectorPair {
  const char* vector_id;
  const char* family_id;
  const char* fixture_id;
  const char* statement_id;
  const char* case_class;
  const char* input_profile_id;
  const char* expected_outcome;
  const char* expected_diagnostic;
  const char* ordering_rule;
  const char* source_class;
  const char* subject_id;
  std::array<FixtureSchemaField, 3> typed_schema;
  std::array<FixtureLiteralRow, 2> literal_rows;
  const char* provider_state;
  const char* descriptor_state;
  const char* security_state;
  const char* mga_state;
  const char* resource_state;
  const char* statement_uuid;
  const char* qualified_object_reference;
  bool cancellation_requested;
  FixtureGenerations selected_generations;
  FixtureGenerations current_generations;
  const char* injected_mutation;
  const char* injected_fault;
  const char* expected_cleanup_state;
};

constexpr std::array<FixtureSchemaField, 3> kDocumentSchema{{
    {"row_uuid", "UUID", false},
    {"join_key", "INT64", true},
    {"payload", "TEXT", true},
}};

constexpr FixtureGenerations kGeneration7{7, 7, 7, 7, 7, 7, 7};
constexpr FixtureGenerations kCatalogGeneration8{8, 7, 7, 7, 7, 7, 7};
constexpr FixtureGenerations kDescriptorGeneration8{7, 8, 7, 7, 7, 7, 7};

constexpr VectorPair DocumentVector(
    const char* vector_id, const char* fixture_id, const char* statement_id,
    const char* case_class, const char* input_profile_id,
    const char* expected_outcome, const char* expected_diagnostic,
    const char* ordering_rule, const char* first_row_uuid,
    const char* second_row_uuid, const char* provider_state,
    const char* descriptor_state, const char* security_state,
    const char* mga_state, const char* resource_state,
    const char* statement_uuid, const bool cancellation_requested,
    const FixtureGenerations current_generations,
    const char* injected_mutation, const char* injected_fault,
    const char* expected_cleanup_state) {
  return {vector_id,
          "document",
          fixture_id,
          statement_id,
          case_class,
          input_profile_id,
          expected_outcome,
          expected_diagnostic,
          ordering_rule,
          "family",
          "document",
          kDocumentSchema,
          {{{first_row_uuid, 1, "document-one"},
            {second_row_uuid, 2, "document-two"}}},
          provider_state,
          descriptor_state,
          security_state,
          mga_state,
          resource_state,
          statement_uuid,
          "app.document_fixture",
          cancellation_requested,
          kGeneration7,
          current_generations,
          injected_mutation,
          injected_fault,
          expected_cleanup_state};
}

// Exact immutable RCP-072 catalog vectors required by RCP-073. Every signed
// vector and dataset field is represented here and drives the direct request.
constexpr std::array<VectorPair, 12> kVectors{{
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-01-V1", "FIX-FAMILY-DOCUMENT-POSITIVE-V1",
        "STMT-FAMILY-DOCUMENT-POSITIVE-V1", "positive", "INPUT-POSITIVE-V1",
        "success:row_count=2;typed_batch_valid=true;canonical_route=true;ordering=fixture_order",
        "not_applicable", "exact_fixture_order",
        "00000000-0000-4000-8000-000000000013",
        "00000000-0000-4000-8000-000000000014", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000013", false, kGeneration7,
        "none", "none", "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-02-V1", "FIX-FAMILY-DOCUMENT-BOUNDARY-V1",
        "STMT-FAMILY-DOCUMENT-BOUNDARY-V1", "boundary",
        "INPUT-BOUNDARY-V1",
        "success:row_count=0;typed_batch_valid=true;boundary_exact=true;ordering=empty",
        "not_applicable", "not_applicable",
        "00000000-0000-4000-8000-000000000014",
        "00000000-0000-4000-8000-000000000015", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000014", false, kGeneration7,
        "none", "none", "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-03-V1",
        "FIX-FAMILY-DOCUMENT-SEMANTIC-REFUSAL-V1",
        "STMT-FAMILY-DOCUMENT-SEMANTIC-REFUSAL-V1", "semantic_refusal",
        "INPUT-SEMANTIC-REFUSAL-V1",
        "refusal:SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1;row_count=0;execution_started=false",
        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000015",
        "00000000-0000-4000-8000-000000000016", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000015", false, kGeneration7,
        "none", "none", "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-04-V1", "FIX-FAMILY-DOCUMENT-CANCELLATION-V1",
        "STMT-FAMILY-DOCUMENT-CANCELLATION-V1", "cancellation",
        "INPUT-CANCELLATION-V1",
        "refusal:SB_MODEL_EXECUTION_CANCELLED_V1;row_count=0;root_absent=true;cleanup_once=true",
        "SB_MODEL_EXECUTION_CANCELLED_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000016",
        "00000000-0000-4000-8000-000000000017", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000016", true, kGeneration7,
        "none", "cancellation_after_admission",
        "all_started_components_cleaned_once_root_absent"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-05-V1", "FIX-FAMILY-DOCUMENT-FAULT-V1",
        "STMT-FAMILY-DOCUMENT-FAULT-V1", "fault", "INPUT-FAULT-V1",
        "refusal:SB_MODEL_COORDINATOR_LEG_FAILED_V1;row_count=0;root_absent=true;cleanup_once=true",
        "SB_MODEL_COORDINATOR_LEG_FAILED_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000017",
        "00000000-0000-4000-8000-000000000018", "fault_injected",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000017", false, kGeneration7,
        "none", "named_precondition_or_provider_failure",
        "all_started_components_cleaned_once_root_absent"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-06-V1",
        "FIX-FAMILY-DOCUMENT-STALE-GENERATION-V1",
        "STMT-FAMILY-DOCUMENT-STALE-GENERATION-V1", "stale_generation",
        "INPUT-STALE-GENERATION-V1",
        "refusal:SB_MODEL_CATALOG_GENERATION_STALE_V1;row_count=0;data_access=false",
        "SB_MODEL_CATALOG_GENERATION_STALE_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000018",
        "00000000-0000-4000-8000-000000000019", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000018", false,
        kCatalogGeneration8, "advance_catalog_generation", "none",
        "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-07-V1",
        "FIX-FAMILY-DOCUMENT-DESCRIPTOR-MISMATCH-V1",
        "STMT-FAMILY-DOCUMENT-DESCRIPTOR-MISMATCH-V1",
        "descriptor_mismatch", "INPUT-DESCRIPTOR-MISMATCH-V1",
        "refusal:SB_MODEL_TYPED_EXCHANGE_INVALID_V1;row_count=0;root_absent=true",
        "SB_MODEL_TYPED_EXCHANGE_INVALID_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000019",
        "00000000-0000-4000-8000-000000000020", "published_validated",
        "mismatched", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000019", false,
        kDescriptorGeneration8, "swap_output_descriptor", "none",
        "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-08-V1",
        "FIX-FAMILY-DOCUMENT-SECURITY-REDACTION-V1",
        "STMT-FAMILY-DOCUMENT-SECURITY-REDACTION-V1", "security_redaction",
        "INPUT-SECURITY-REDACTION-V1",
        "refusal:SB_MODEL_SECURITY_ADMISSION_REFUSED_V1;row_count=0;protected_identity_absent=true",
        "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000020",
        "00000000-0000-4000-8000-000000000021", "published_validated",
        "current", "redacted_or_denied", "bound_statement_context",
        "within_limit", "10000000-0000-4000-8000-000000000020", false,
        kGeneration7, "remove_object_disclosure", "none",
        "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-09-V1",
        "FIX-FAMILY-DOCUMENT-MGA-CONTEXT-SUBSTITUTION-V1",
        "STMT-FAMILY-DOCUMENT-MGA-CONTEXT-SUBSTITUTION-V1",
        "mga_context_substitution", "INPUT-MGA-CONTEXT-SUBSTITUTION-V1",
        "refusal:SB_MODEL_MGA_CONTEXT_MISMATCH_V1;row_count=0;data_access=false",
        "SB_MODEL_MGA_CONTEXT_MISMATCH_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000021",
        "00000000-0000-4000-8000-000000000022", "published_validated",
        "current", "authorized", "substituted", "within_limit",
        "10000000-0000-4000-8000-000000000021", false, kGeneration7,
        "swap_statement_context", "none", "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-10-V1",
        "FIX-FAMILY-DOCUMENT-RESOURCE-EXHAUSTION-V1",
        "STMT-FAMILY-DOCUMENT-RESOURCE-EXHAUSTION-V1",
        "resource_exhaustion", "INPUT-RESOURCE-EXHAUSTION-V1",
        "refusal:SB_MODEL_RESOURCE_MEMORY_REFUSED_V1;row_count=0;execution_started=false",
        "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1", "not_applicable",
        "00000000-0000-4000-8000-000000000022",
        "00000000-0000-4000-8000-000000000023", "published_validated",
        "current", "authorized", "bound_statement_context", "over_limit",
        "10000000-0000-4000-8000-000000000022", false, kGeneration7,
        "reduce_memory_budget", "none", "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-11-V1",
        "FIX-FAMILY-DOCUMENT-EXACT-FALLBACK-V1",
        "STMT-FAMILY-DOCUMENT-EXACT-FALLBACK-V1", "exact_fallback",
        "INPUT-EXACT-FALLBACK-V1",
        "success:row_count=2;selected_alternative=family_exact_fallback;exact_recheck_complete=true;unavailable_diagnostic=SB_MODEL_DOCUMENT_EXACT_FALLBACK_UNAVAILABLE_V1",
        "not_applicable", "exact_fixture_order",
        "00000000-0000-4000-8000-000000000023",
        "00000000-0000-4000-8000-000000000024", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000023", false, kGeneration7,
        "none", "none", "normal_completion_cleanup_once"),
    DocumentVector(
        "VEC-FAMILY-DOCUMENT-12-V1",
        "FIX-FAMILY-DOCUMENT-DETERMINISTIC-REPLAY-V1",
        "STMT-FAMILY-DOCUMENT-DETERMINISTIC-REPLAY-V1",
        "deterministic_replay", "INPUT-DETERMINISTIC-REPLAY-V1",
        "success:row_count=2;result_digest=fixture_expected_digest;plan_digest_equal=true;counter_digest_equal=true",
        "not_applicable", "exact_fixture_order",
        "00000000-0000-4000-8000-000000000024",
        "00000000-0000-4000-8000-000000000025", "published_validated",
        "current", "authorized", "bound_statement_context", "within_limit",
        "10000000-0000-4000-8000-000000000024", false, kGeneration7,
        "none", "none", "normal_completion_cleanup_once"),
}};

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "00000000-0000-4000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-DOCUMENT: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext Mga(const std::uint64_t identity = 1) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(100 + identity);
  context.owning_transaction_uuid = Uuid(200 + identity);
  context.statement_snapshot_uuid = Uuid(300 + identity);
  context.statement_metadata_snapshot_uuid = Uuid(400 + identity);
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

exec::PhysicalMgaStatementContext Mga(const VectorPair& vector) {
  auto context = Mga();
  context.statement_uuid = vector.statement_uuid;
  return context;
}

plan::CanonicalLogicalRelationalGraph LogicalModelGraph(
    std::string semantic_variant) {
  const auto physical_mga = Mga(73);
  plan::CanonicalMgaStatementContext logical_mga;
  logical_mga.statement_uuid = physical_mga.statement_uuid;
  logical_mga.owning_transaction_uuid =
      physical_mga.owning_transaction_uuid;
  logical_mga.statement_snapshot_uuid =
      physical_mga.statement_snapshot_uuid;
  logical_mga.statement_metadata_snapshot_uuid =
      physical_mga.statement_metadata_snapshot_uuid;
  logical_mga.owning_local_transaction_id =
      physical_mga.owning_local_transaction_id;
  logical_mga.visible_committed_high_watermark =
      physical_mga.visible_committed_high_watermark;
  logical_mga.oldest_active_transaction_id =
      physical_mga.oldest_active_transaction_id;
  logical_mga.oldest_interesting_transaction_id =
      physical_mga.oldest_interesting_transaction_id;
  logical_mga.oldest_snapshot_transaction_id =
      physical_mga.oldest_snapshot_transaction_id;
  logical_mga.retention_horizon_transaction_id =
      physical_mga.retention_horizon_transaction_id;
  logical_mga.active_excluded_local_transaction_ids =
      physical_mga.active_excluded_local_transaction_ids;
  logical_mga.in_doubt_excluded_local_transaction_ids =
      physical_mga.in_doubt_excluded_local_transaction_ids;
  logical_mga.snapshot_kind = physical_mga.snapshot_kind;
  logical_mga.publication_inventory_next_local_transaction_id =
      physical_mga.publication_inventory_next_local_transaction_id;
  logical_mga.inventory_authoritative = physical_mga.inventory_authoritative;
  logical_mga.complete = physical_mga.complete;
  logical_mga.current = physical_mga.current;

  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = 1;
  node.node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  node.output_descriptor_ids = {101};
  node.bound_expression_ids = {201};
  node.origin_relational_node_ids = {1};
  node.semantic_variant_id = std::move(semantic_variant);
  if (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1") {
    node.required_object_uuids = {Uuid(7301)};
  }

  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(7302);
  graph.catalog_epoch_uuid = Uuid(7303);
  graph.security_context_uuid = Uuid(7304);
  graph.local_transaction_id = logical_mga.owning_local_transaction_id;
  graph.statement_snapshot_id =
      logical_mga.visible_committed_high_watermark;
  graph.mga_statement_context = std::move(logical_mga);
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {101};
  graph.nodes = {std::move(node)};
  return graph;
}

bool HasLogicalIssue(
    const plan::CanonicalLogicalRelationalGraphValidationResult& result,
    const std::string_view field_id) {
  return std::ranges::any_of(result.issues, [&](const auto& issue) {
    return issue.diagnostic_id == "SBLR.PLAN_TREE.INVALID_HANDLE" &&
           issue.logical_node_id == 1 && issue.field_id == field_id;
  });
}

api::EngineDescriptor Descriptor(const std::uint64_t identity,
                                 const std::string& type,
                                 const bool nullable) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = Uuid(identity);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor =
      "type_uuid=" + Uuid(identity + 100) + ";nullability=" +
      (nullable ? "nullable" : "non_null");
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  return {descriptor, std::move(encoded), false};
}

std::string FixtureObjectUuid(const VectorPair& vector) {
  // The immutable catalog supplies a qualified name, while the coordinator
  // contract consumes its already-bound object UUID. This is the test
  // catalog's explicit deterministic name-to-object binding.
  return std::string_view(vector.qualified_object_reference) ==
                 "app.document_fixture"
             ? Uuid(1)
             : std::string{};
}

std::string FixtureEngineType(const FixtureSchemaField& field) {
  if (std::string_view(field.type) == "UUID") return "uuid";
  if (std::string_view(field.type) == "INT64") return "int64";
  if (std::string_view(field.type) == "TEXT") return "text";
  return {};
}

opt::ModelFamilyCoordinatorRequestV1 PlanningRequest(
    const VectorPair& vector = kVectors[0]) {
  opt::ModelFamilyCoordinatorRequestV1 request;
  request.family_id = vector.family_id;
  request.operation_id =
      std::string_view(vector.case_class) == "semantic_refusal"
          ? "DOCUMENT_WRITE"
          : "DOCUMENT_PATH";
  request.logical_operator_id = "LOGICAL_DOCUMENT_SOURCE_V1";
  request.logical_node_id = 1;
  request.object_uuid = FixtureObjectUuid(vector);
  request.output_descriptor_ids = {101, 102, 103};
  request.mga_statement_context = Mga(vector);
  request.bound_sblr_tree_uuid = Uuid(2);
  request.catalog_epoch_uuid = Uuid(3);
  request.security_context_uuid = Uuid(4);
  request.capability_snapshot_uuid = Uuid(5);
  request.resource_snapshot_uuid = Uuid(6);
  request.statistics_snapshot_uuid = Uuid(7);
  request.route_snapshot_uuid = Uuid(8);
  request.catalog_generation = vector.selected_generations.catalog;
  request.current_catalog_generation = vector.current_generations.catalog;
  request.security_epoch = vector.selected_generations.security;
  request.policy_epoch = vector.selected_generations.policy;
  request.resource_epoch = vector.selected_generations.resource;
  request.statistics_generation = 7;
  request.route_epoch = vector.selected_generations.route;
  request.route_generation = vector.current_generations.route;
  request.memory_budget_bytes =
      std::string_view(vector.resource_state) == "over_limit" ? 32 : 4096;
  request.security_admitted =
      std::string_view(vector.security_state) == "authorized";

  opt::ModelFamilyCandidateV1 candidate;
  candidate.alternative_uuid = Uuid(9);
  candidate.provider_uuid = Uuid(10);
  candidate.capability_uuid = Uuid(11);
  candidate.provider_generation = vector.selected_generations.provider;
  candidate.available = true;
  candidate.exact = true;
  candidate.exact_collection_fallback = true;
  candidate.cost.cost_vector_uuid = Uuid(12);
  candidate.cost.cpu_units = 1;
  candidate.cost.sequential_read_units = 2;
  candidate.cost.memory_bytes_required = 64;
  request.candidates.push_back(candidate);
  return request;
}

exec::ModelFamilyExecutionRequestV1 ExecutionRequest(
    const VectorPair& vector, const bool empty) {
  const auto row_uuid = Descriptor(
      501, FixtureEngineType(vector.typed_schema[0]),
      vector.typed_schema[0].nullable);
  const auto join_key =
      Descriptor(502, FixtureEngineType(vector.typed_schema[1]),
                 vector.typed_schema[1].nullable);
  const auto payload =
      Descriptor(503, FixtureEngineType(vector.typed_schema[2]),
                 vector.typed_schema[2].nullable);
  exec::ModelFamilyExecutionRequestV1 request;
  request.input.family_id = vector.family_id;
  request.input.operation_id = "DOCUMENT_PATH";
  request.input.object_uuid = FixtureObjectUuid(vector);
  request.input.physical_node_id = 1;
  request.input.selected_alternative_uuid = Uuid(9);
  request.input.capability_uuid = Uuid(11);
  request.input.provider_uuid = Uuid(10);
  request.input.provider_generation = vector.selected_generations.provider;
  request.input.result_handle_uuid = Uuid(13);
  request.input.causal_counter_id = 1;
  request.input.output_descriptor_ids = {101, 102, 103};
  request.input.mga_statement_context = Mga(vector);
  request.input.catalog_epoch_uuid = Uuid(3);
  request.input.security_context_uuid = Uuid(4);
  request.input.policy_snapshot_uuid = Uuid(14);
  request.input.resource_contract_uuid = Uuid(15);
  request.input.catalog_generation = vector.selected_generations.catalog;
  request.input.descriptor_generation = vector.selected_generations.descriptor;
  request.input.security_generation = vector.selected_generations.security;
  request.input.policy_generation = vector.selected_generations.policy;
  request.input.resource_generation = vector.selected_generations.resource;
  request.input.maximum_rows = 2;
  request.input.maximum_cells = 6;
  request.input.maximum_memory_bytes = 4096;
  request.capability.capability_uuid = Uuid(11);
  request.capability.family_id = vector.family_id;
  request.capability.provider_uuid = Uuid(10);
  request.capability.provider_generation = vector.selected_generations.provider;
  request.capability.available = true;
  request.capability.exact = true;
  request.capability.exact_collection_fallback_available = true;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.current_catalog_generation = vector.current_generations.catalog;
  request.current_descriptor_generation = vector.current_generations.descriptor;
  request.current_security_generation = vector.current_generations.security;
  request.current_policy_generation = vector.current_generations.policy;
  request.current_resource_generation = vector.current_generations.resource;
  request.current_provider_generation = vector.current_generations.provider;
  request.current_mga_statement_context = request.input.mga_statement_context;
  if (std::string_view(vector.mga_state) == "substituted") {
    request.current_mga_statement_context = Mga(2);
  }
  request.cancellation_requested =
      [requested = vector.cancellation_requested,
       probes = std::size_t{0}]() mutable {
        return requested && ++probes >= 2;
      };
  request.cleanup_provider = [] {};
  request.fault_injected =
      std::string_view(vector.injected_fault) ==
      "named_precondition_or_provider_failure";
  request.security_admitted =
      std::string_view(vector.security_state) == "authorized";
  request.execute_provider = [=](const auto& input) {
    exec::ModelProviderExecutionResultV1 result;
    result.ok = true;
    result.data_access_observed = true;
    result.rows_examined = empty ? 0 : 2;
    auto& batch = result.provider_batch;
    batch.provider_uuid = input.provider_uuid;
    batch.provider_generation = input.provider_generation;
    batch.result_handle_uuid = input.result_handle_uuid;
    batch.causal_counter_id = input.causal_counter_id;
    batch.output_descriptor_ids = input.output_descriptor_ids;
    batch.batch.columns = {
        {vector.typed_schema[0].field_id, row_uuid,
         vector.typed_schema[0].nullable, 101},
        {vector.typed_schema[1].field_id, join_key,
         vector.typed_schema[1].nullable, 102},
        {vector.typed_schema[2].field_id, payload,
         vector.typed_schema[2].nullable, 103}};
    if (!empty) {
      batch.batch.rows = {
          {{Value(row_uuid, vector.literal_rows[0].row_uuid),
            Value(join_key, std::to_string(vector.literal_rows[0].join_key)),
            Value(payload, vector.literal_rows[0].payload)}},
          {{Value(row_uuid, vector.literal_rows[1].row_uuid),
            Value(join_key, std::to_string(vector.literal_rows[1].join_key)),
            Value(payload, vector.literal_rows[1].payload)}},
      };
      batch.ordered_row_identities = {
          {Uuid(1301), vector.literal_rows[0].row_uuid},
          {Uuid(1302), vector.literal_rows[1].row_uuid},
      };
    }
    batch.properties.property_uuid = Uuid(16);
    batch.properties.exact = true;
    batch.properties.residual_recheck_complete = true;
    batch.properties.base_row_mga_recheck_complete = true;
    batch.properties.security_recheck_complete = true;
    batch.mga_statement_context = input.mga_statement_context;
    batch.security_receipt_uuid = Uuid(17);
    batch.residual_recheck_complete = true;
    batch.base_row_mga_recheck_complete = true;
    batch.security_recheck_complete = true;
    return result;
  };
  return request;
}

bool Successful(const VectorPair& vector, const bool empty,
                const bool exact_fallback) {
  auto planning_request = PlanningRequest(vector);
  planning_request.candidates.front().exact_collection_fallback = exact_fallback;
  const auto planning = opt::CoordinateDocumentFamilySourceV1(planning_request);
  bool passed = true;
  passed &= Require(planning.accepted && planning.selected &&
                        planning.data_access_allowed && planning.deterministic,
                    std::string(vector.vector_id) + " planning failed");
  passed &= Require(planning.logical_operator_id ==
                            "LOGICAL_DOCUMENT_SOURCE_V1" &&
                        planning.physical_operator_id ==
                            "PHYSICAL_DOCUMENT_PATH_SCAN_V1",
                    std::string(vector.vector_id) + " operator identity drifted");
  auto execution_request = ExecutionRequest(vector, empty);
  execution_request.exact_fallback_selected = exact_fallback;
  const auto execution = exec::ExecuteModelFamilySourceV1(execution_request);
  passed &= Require(execution.accepted && execution.execution_started &&
                        execution.root_published && execution.cleanup_complete &&
                        execution.cleanup_count == 1,
                    std::string(vector.vector_id) + " execution failed");
  passed &= Require(execution.output.batch.rows.size() == (empty ? 0 : 2) &&
                        execution.output.exact_exchange_validated,
                    std::string(vector.vector_id) + " row result drifted");
  return passed;
}

bool PositiveBoundaryFallback() {
  bool passed = true;
  passed &= Successful(kVectors[0], false, false);
  passed &= Successful(kVectors[1], true, false);
  passed &= Successful(kVectors[10], false, true);
  return passed;
}

bool RefusalVectors() {
  bool passed = true;

  auto semantic = PlanningRequest(kVectors[2]);
  const auto semantic_result = opt::CoordinateDocumentFamilySourceV1(semantic);
  passed &= Require(!semantic_result.accepted &&
                        !semantic_result.data_access_allowed &&
                        semantic_result.diagnostic_id ==
                            kVectors[2].expected_diagnostic,
                    "semantic refusal vector drifted");

  auto cancellation = ExecutionRequest(kVectors[3], false);
  std::size_t cancellation_provider_calls = 0;
  cancellation.cleanup_provider = [] {};
  const auto original_provider = cancellation.execute_provider;
  cancellation.execute_provider = [&](const auto& input) {
    ++cancellation_provider_calls;
    return original_provider(input);
  };
  const auto cancelled = exec::ExecuteModelFamilySourceV1(cancellation);
  passed &= Require(!cancelled.accepted && cancelled.execution_started &&
                        !cancelled.root_published &&
                        cancelled.diagnostic_id == kVectors[3].expected_diagnostic &&
                        cancelled.cleanup_count == 1 &&
                        cancelled.cleanup_complete &&
                        cancellation_provider_calls == 1,
                    "cancellation-after-admission vector drifted");

  auto fault = ExecutionRequest(kVectors[4], false);
  std::size_t fault_provider_calls = 0;
  fault.execute_provider = [&](const auto&) {
    ++fault_provider_calls;
    return exec::ModelProviderExecutionResultV1{};
  };
  const auto faulted = exec::ExecuteModelFamilySourceV1(fault);
  passed &= Require(!faulted.accepted && faulted.execution_started &&
                        !faulted.root_published &&
                        faulted.diagnostic_id == kVectors[4].expected_diagnostic &&
                        faulted.cleanup_count == 1 &&
                        fault_provider_calls == 0,
                    "fault vector drifted");

  auto stale = PlanningRequest(kVectors[5]);
  const auto stale_result = opt::CoordinateDocumentFamilySourceV1(stale);
  passed &= Require(!stale_result.accepted && !stale_result.data_access_allowed &&
                        stale_result.diagnostic_id ==
                            kVectors[5].expected_diagnostic,
                    "stale catalog vector accessed data");

  auto mismatch = ExecutionRequest(kVectors[6], false);
  const auto mismatched = exec::ExecuteModelFamilySourceV1(mismatch);
  passed &= Require(!mismatched.accepted && !mismatched.execution_started &&
                        !mismatched.root_published &&
                        mismatched.diagnostic_id == kVectors[6].expected_diagnostic &&
                        mismatched.cleanup_count == 0,
                    "descriptor mismatch vector drifted");

  auto security = PlanningRequest(kVectors[7]);
  const auto denied = opt::CoordinateDocumentFamilySourceV1(security);
  passed &= Require(!denied.accepted && !denied.data_access_allowed &&
                        denied.diagnostic_id == kVectors[7].expected_diagnostic &&
                        denied.detail.find(security.object_uuid) == std::string::npos,
                    "security redaction vector disclosed or accessed identity");

  auto substituted = ExecutionRequest(kVectors[8], false);
  std::size_t substituted_provider_calls = 0;
  substituted.execute_provider = [&](const auto&) {
    ++substituted_provider_calls;
    return exec::ModelProviderExecutionResultV1{};
  };
  const auto mga_result = exec::ExecuteModelFamilySourceV1(substituted);
  passed &= Require(!mga_result.accepted && !mga_result.execution_started &&
                        mga_result.diagnostic_id == kVectors[8].expected_diagnostic &&
                        substituted_provider_calls == 0,
                    "MGA substitution vector accessed provider");

  auto resource = PlanningRequest(kVectors[9]);
  const auto resource_result = opt::CoordinateDocumentFamilySourceV1(resource);
  passed &= Require(!resource_result.accepted &&
                        !resource_result.data_access_allowed &&
                        resource_result.diagnostic_id ==
                            kVectors[9].expected_diagnostic,
                    "resource exhaustion vector accessed data");
  return passed;
}

bool ExactIdentityAndPropertyRefusals() {
  bool passed = true;
  auto provider_stale = ExecutionRequest(kVectors[0], false);
  provider_stale.current_provider_generation = 8;
  const auto provider_stale_result =
      exec::ExecuteModelFamilySourceV1(provider_stale);
  passed &= Require(
      !provider_stale_result.accepted &&
          !provider_stale_result.execution_started &&
          provider_stale_result.diagnostic_id ==
              "SB_MODEL_PROVIDER_GENERATION_STALE_V1",
      "provider generation substitution was not distinct from catalog staleness");

  auto capability = ExecutionRequest(kVectors[0], false);
  capability.capability.capability_uuid = Uuid(1111);
  const auto capability_result = exec::ExecuteModelFamilySourceV1(capability);
  passed &= Require(!capability_result.accepted &&
                        !capability_result.execution_started &&
                        capability_result.diagnostic_id ==
                            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "selected capability substitution reached provider access");

  const auto expect_provider_refusal = [&](auto mutation,
                                            const std::string_view detail) {
    auto request = ExecutionRequest(kVectors[0], false);
    const auto provider = request.execute_provider;
    request.execute_provider = [provider, mutation](const auto& input) {
      auto result = provider(input);
      mutation(result.provider_batch);
      return result;
    };
    const auto refused = exec::ExecuteModelFamilySourceV1(request);
    return Require(!refused.accepted && refused.execution_started &&
                       !refused.root_published &&
                       refused.diagnostic_id ==
                           "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                   detail);
  };
  passed &= expect_provider_refusal(
      [](auto& batch) { batch.properties.ordering_id = "provider_order"; },
      "document ordering property substitution was admitted");
  passed &= expect_provider_refusal(
      [](auto& batch) {
        batch.properties.partitioning_id = "distributed_partition";
      },
      "document partitioning property substitution was admitted");
  passed &= expect_provider_refusal(
      [](auto& batch) { batch.properties.uniqueness_id = "row_uuid"; },
      "document uniqueness property substitution was admitted");
  passed &= expect_provider_refusal(
      [](auto& batch) { batch.ordered_row_identities.clear(); },
      "missing document row identity vector was admitted");
  passed &= expect_provider_refusal(
      [](auto& batch) {
        batch.ordered_row_identities[1].document_uuid =
            batch.ordered_row_identities[0].document_uuid;
      },
      "duplicate document uniqueness identity was admitted");
  passed &= expect_provider_refusal(
      [](auto& batch) {
        batch.ordered_row_identities[1].row_uuid =
            batch.ordered_row_identities[0].row_uuid;
      },
      "duplicate relational row identity was admitted");
  passed &= expect_provider_refusal(
      [](auto& batch) {
        std::swap(batch.output_descriptor_ids[1],
                  batch.output_descriptor_ids[2]);
      },
      "provider output descriptor substitution was admitted");

  auto bounded = ExecutionRequest(kVectors[9], false);
  bounded.input.maximum_memory_bytes = 1;
  const auto bounded_result = exec::ExecuteModelFamilySourceV1(bounded);
  passed &= Require(!bounded_result.accepted && bounded_result.execution_started &&
                        bounded_result.diagnostic_id ==
                            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "retained typed batch payload escaped its byte bound");
  return passed;
}

void AppendReplayField(std::string* bytes, const std::string_view value) {
  bytes->append(std::to_string(value.size()));
  bytes->push_back(':');
  bytes->append(value);
  bytes->push_back('|');
}

template <typename Integer>
  requires(std::is_integral_v<Integer> &&
           !std::is_same_v<std::remove_cv_t<Integer>, bool>)
void AppendReplayField(std::string* bytes, const Integer value) {
  AppendReplayField(bytes,
                    std::to_string(static_cast<std::uint64_t>(value)));
}

void AppendReplayField(std::string* bytes, const bool value) {
  AppendReplayField(bytes,
                    std::string_view(value ? "true" : "false"));
}

void AppendReplayMga(std::string* bytes,
                     const exec::PhysicalMgaStatementContext& mga) {
  AppendReplayField(bytes, mga.statement_uuid);
  AppendReplayField(bytes, mga.owning_transaction_uuid);
  AppendReplayField(bytes, mga.statement_snapshot_uuid);
  AppendReplayField(bytes, mga.statement_metadata_snapshot_uuid);
  AppendReplayField(bytes, mga.owning_local_transaction_id);
  AppendReplayField(bytes, mga.visible_committed_high_watermark);
  AppendReplayField(bytes, mga.oldest_active_transaction_id);
  AppendReplayField(bytes, mga.oldest_interesting_transaction_id);
  AppendReplayField(bytes, mga.oldest_snapshot_transaction_id);
  AppendReplayField(bytes, mga.retention_horizon_transaction_id);
  AppendReplayField(bytes,
                    static_cast<std::uint64_t>(
                        mga.active_excluded_local_transaction_ids.size()));
  for (const auto id : mga.active_excluded_local_transaction_ids) {
    AppendReplayField(bytes, id);
  }
  AppendReplayField(bytes,
                    static_cast<std::uint64_t>(
                        mga.in_doubt_excluded_local_transaction_ids.size()));
  for (const auto id : mga.in_doubt_excluded_local_transaction_ids) {
    AppendReplayField(bytes, id);
  }
  AppendReplayField(bytes, mga.snapshot_kind);
  AppendReplayField(bytes, mga.publication_inventory_next_local_transaction_id);
  AppendReplayField(bytes, mga.inventory_authoritative);
  AppendReplayField(bytes, mga.complete);
  AppendReplayField(bytes, mga.current);
}

void AppendReplayDescriptor(std::string* bytes,
                            const api::EngineDescriptor& descriptor) {
  AppendReplayField(bytes, descriptor.descriptor_uuid.canonical);
  AppendReplayField(bytes, descriptor.descriptor_kind);
  AppendReplayField(bytes, descriptor.canonical_type_name);
  AppendReplayField(bytes, descriptor.encoded_descriptor);
}

void AppendReplayValue(std::string* bytes,
                       const api::EngineTypedValue& value) {
  AppendReplayDescriptor(bytes, value.descriptor);
  AppendReplayField(bytes, value.encoded_value);
  AppendReplayField(bytes,
                    static_cast<std::uint64_t>(value.binary_value.size()));
  if (!value.binary_value.empty()) {
    AppendReplayField(
        bytes,
        std::string_view(
            reinterpret_cast<const char*>(value.binary_value.data()),
            value.binary_value.size()));
  } else {
    AppendReplayField(bytes, std::string_view{});
  }
  AppendReplayField(bytes, value.is_null);
  AppendReplayField(bytes, static_cast<std::uint64_t>(value.state));
}

void AppendReplayProperties(std::string* bytes,
                            const exec::ModelPropertyDescriptorV1& properties) {
  AppendReplayField(bytes, properties.abi_version);
  AppendReplayField(bytes, properties.property_descriptor_id);
  AppendReplayField(bytes, properties.property_uuid);
  AppendReplayField(bytes, properties.ordering_id);
  AppendReplayField(bytes, properties.partitioning_id);
  AppendReplayField(bytes, properties.uniqueness_id);
  AppendReplayField(bytes, properties.exact);
  AppendReplayField(bytes, properties.residual_recheck_complete);
  AppendReplayField(bytes, properties.base_row_mga_recheck_complete);
  AppendReplayField(bytes, properties.security_recheck_complete);
}

void AppendReplayOutput(std::string* bytes,
                        const exec::ModelSourceOutputDescriptorV1& output) {
  AppendReplayField(bytes, output.abi_version);
  AppendReplayField(bytes, output.output_descriptor_id);
  AppendReplayField(bytes, output.family_id);
  AppendReplayField(bytes, output.operation_id);
  AppendReplayField(bytes, output.object_uuid);
  AppendReplayField(bytes, output.physical_node_id);
  AppendReplayField(bytes, output.selected_alternative_uuid);
  AppendReplayField(bytes, output.capability_uuid);
  AppendReplayField(bytes, output.provider_uuid);
  AppendReplayField(bytes, output.provider_generation);
  AppendReplayField(bytes, output.result_handle_uuid);
  AppendReplayField(bytes, output.causal_counter_id);
  AppendReplayField(
      bytes, static_cast<std::uint64_t>(output.output_descriptor_ids.size()));
  for (const auto id : output.output_descriptor_ids) {
    AppendReplayField(bytes, id);
  }
  AppendReplayField(
      bytes, static_cast<std::uint64_t>(output.ordered_row_identities.size()));
  for (const auto& identity : output.ordered_row_identities) {
    AppendReplayField(bytes, identity.document_uuid);
    AppendReplayField(bytes, identity.row_uuid);
  }
  AppendReplayField(bytes,
                    static_cast<std::uint64_t>(output.batch.columns.size()));
  for (const auto& column : output.batch.columns) {
    AppendReplayField(bytes, column.stable_name);
    AppendReplayDescriptor(bytes, column.descriptor);
    AppendReplayField(bytes, column.nullable);
    AppendReplayField(bytes, column.descriptor_id);
  }
  AppendReplayField(bytes,
                    static_cast<std::uint64_t>(output.batch.rows.size()));
  for (const auto& row : output.batch.rows) {
    AppendReplayField(bytes,
                      static_cast<std::uint64_t>(row.values.size()));
    for (const auto& value : row.values) AppendReplayValue(bytes, value);
  }
  AppendReplayProperties(bytes, output.properties);
  AppendReplayMga(bytes, output.mga_statement_context);
  AppendReplayField(bytes, output.security_receipt_uuid);
  AppendReplayField(bytes, output.exact_exchange_validated);
}

void AppendReplayCost(std::string* bytes,
                      const opt::ModelFamilyCostVectorV1& cost) {
  AppendReplayField(bytes, cost.cost_vector_uuid);
  AppendReplayField(bytes, cost.provenance_uuid);
  AppendReplayField(bytes, cost.property_snapshot_uuid);
  AppendReplayField(bytes, cost.calibration_profile_uuid);
  AppendReplayField(bytes, cost.scalarization_policy_id);
  AppendReplayField(bytes, cost.provenance_generation);
  AppendReplayField(bytes, cost.confidence_basis_points);
  AppendReplayField(bytes, cost.scalar_score);
  AppendReplayField(bytes, cost.startup_units);
  AppendReplayField(bytes, cost.cpu_units);
  AppendReplayField(bytes, cost.sequential_read_units);
  AppendReplayField(bytes, cost.random_read_units);
  AppendReplayField(bytes, cost.page_write_units);
  AppendReplayField(bytes, cost.cache_units);
  AppendReplayField(bytes, cost.memory_bytes_required);
  AppendReplayField(bytes, cost.memory_grant_units);
  AppendReplayField(bytes, cost.spill_units);
  AppendReplayField(bytes, cost.network_units);
  AppendReplayField(bytes, cost.compression_units);
  AppendReplayField(bytes, cost.encryption_units);
  AppendReplayField(bytes, cost.predicate_evaluation_units);
  AppendReplayField(bytes, cost.vector_distance_units);
  AppendReplayField(bytes, cost.text_scoring_units);
  AppendReplayField(bytes, cost.spatial_evaluation_units);
  AppendReplayField(bytes, cost.udr_invocation_units);
  AppendReplayField(bytes, cost.mga_units);
  AppendReplayField(bytes, cost.index_maintenance_units);
  AppendReplayField(bytes, cost.cache_miss_units);
  AppendReplayField(bytes, cost.cache_residency_benefit_units);
  AppendReplayField(bytes, cost.memory_allocation_units);
  AppendReplayField(bytes, cost.memory_grant_opportunity_units);
  AppendReplayField(bytes, cost.spill_write_units);
  AppendReplayField(bytes, cost.spill_read_units);
  AppendReplayField(bytes, cost.temp_space_pressure_units);
  AppendReplayField(bytes, cost.decompression_units);
  AppendReplayField(bytes, cost.decryption_units);
  AppendReplayField(bytes, cost.expression_evaluation_units);
  AppendReplayField(bytes, cost.domain_cast_units);
  AppendReplayField(bytes, cost.datatype_conversion_units);
  AppendReplayField(bytes, cost.collation_comparison_units);
  AppendReplayField(bytes, cost.mga_version_traversal_units);
  AppendReplayField(bytes, cost.mga_visibility_check_units);
  AppendReplayField(bytes, cost.archive_fetch_units);
  AppendReplayField(bytes, cost.garbage_retention_pressure_units);
  AppendReplayField(bytes, cost.lock_latch_wait_risk_units);
  AppendReplayField(bytes, cost.network_latency_units);
  AppendReplayField(bytes, cost.network_bandwidth_units);
  AppendReplayField(bytes, cost.remote_execution_startup_units);
  AppendReplayField(bytes, cost.cluster_coordination_units);
  AppendReplayField(bytes, cost.repartition_units);
  AppendReplayField(bytes, cost.broadcast_units);
  AppendReplayField(bytes, cost.replica_staleness_risk_units);
  AppendReplayField(bytes, cost.quorum_availability_risk_units);
  AppendReplayField(bytes, cost.donor_compatibility_enforcement_units);
  AppendReplayField(bytes, cost.result_ordering_enforcement_units);
  AppendReplayField(bytes, cost.uncertainty_penalty);
  AppendReplayField(bytes, cost.risk_penalty);
  AppendReplayField(bytes, cost.plan_instability_penalty);
  AppendReplayField(bytes, cost.complete_dimension_vector);
}

void AppendReplayCandidate(std::string* bytes,
                           const opt::ModelFamilyCandidateV1& candidate) {
  AppendReplayField(bytes, candidate.alternative_uuid);
  AppendReplayField(bytes, candidate.provider_uuid);
  AppendReplayField(bytes, candidate.capability_uuid);
  AppendReplayField(bytes, candidate.implementation_id);
  AppendReplayField(bytes, candidate.provider_generation);
  AppendReplayField(bytes, candidate.available);
  AppendReplayField(bytes, candidate.exact);
  AppendReplayField(bytes, candidate.exact_collection_fallback);
  AppendReplayField(bytes, candidate.residual_recheck_required);
  AppendReplayField(bytes, candidate.base_row_mga_recheck_required);
  AppendReplayField(bytes, candidate.security_recheck_required);
  AppendReplayField(bytes, candidate.engine_owned);
  AppendReplayField(bytes, candidate.local_scope);
  AppendReplayField(bytes, candidate.parser_planning_authority_claimed);
  AppendReplayField(bytes, candidate.transaction_finality_authority_claimed);
  AppendReplayCost(bytes, candidate.cost);
}

void AppendReplayPhysicalCost(
    std::string* bytes, const exec::PhysicalCostVectorReceipt& cost) {
  AppendReplayField(bytes, cost.cost_vector_uuid);
  AppendReplayField(bytes, cost.calibration_profile_uuid);
  AppendReplayField(bytes, cost.scalarization_policy_id);
  AppendReplayField(bytes, cost.scalar_score);
  AppendReplayField(bytes, cost.cpu_units);
  AppendReplayField(bytes, cost.page_read_sequential_units);
  AppendReplayField(bytes, cost.page_read_random_units);
  AppendReplayField(bytes, cost.page_write_units);
  AppendReplayField(bytes, cost.memory_bytes_required);
  AppendReplayField(bytes, cost.spill_bytes_expected);
  AppendReplayField(bytes, cost.network_bytes_expected);
  AppendReplayField(bytes, cost.mga_visibility_checks_expected);
  AppendReplayField(bytes, cost.archive_fetches_expected);
  AppendReplayField(bytes, cost.uncertainty_penalty);
  AppendReplayField(bytes, cost.risk_penalty);
  AppendReplayField(bytes, cost.cache_units);
  AppendReplayField(bytes, cost.memory_grant_units);
  AppendReplayField(bytes, cost.spill_units);
  AppendReplayField(bytes, cost.network_units);
  AppendReplayField(bytes, cost.compression_units);
  AppendReplayField(bytes, cost.encryption_units);
  AppendReplayField(bytes, cost.predicate_evaluation_units);
  AppendReplayField(bytes, cost.vector_distance_units);
  AppendReplayField(bytes, cost.text_scoring_units);
  AppendReplayField(bytes, cost.spatial_evaluation_units);
  AppendReplayField(bytes, cost.udr_invocation_units);
  AppendReplayField(bytes, cost.mga_units);
  AppendReplayField(bytes, cost.index_maintenance_units);
  AppendReplayField(bytes, cost.cache_miss_units);
  AppendReplayField(bytes, cost.cache_residency_benefit_units);
  AppendReplayField(bytes, cost.memory_allocation_units);
  AppendReplayField(bytes, cost.memory_grant_opportunity_units);
  AppendReplayField(bytes, cost.spill_write_units);
  AppendReplayField(bytes, cost.spill_read_units);
  AppendReplayField(bytes, cost.temp_space_pressure_units);
  AppendReplayField(bytes, cost.decompression_units);
  AppendReplayField(bytes, cost.decryption_units);
  AppendReplayField(bytes, cost.expression_evaluation_units);
  AppendReplayField(bytes, cost.domain_cast_units);
  AppendReplayField(bytes, cost.datatype_conversion_units);
  AppendReplayField(bytes, cost.collation_comparison_units);
  AppendReplayField(bytes, cost.mga_version_traversal_units);
  AppendReplayField(bytes, cost.mga_visibility_check_units);
  AppendReplayField(bytes, cost.archive_fetch_units);
  AppendReplayField(bytes, cost.garbage_retention_pressure_units);
  AppendReplayField(bytes, cost.lock_latch_wait_risk_units);
  AppendReplayField(bytes, cost.network_latency_units);
  AppendReplayField(bytes, cost.network_bandwidth_units);
  AppendReplayField(bytes, cost.remote_execution_startup_units);
  AppendReplayField(bytes, cost.cluster_coordination_units);
  AppendReplayField(bytes, cost.repartition_units);
  AppendReplayField(bytes, cost.broadcast_units);
  AppendReplayField(bytes, cost.replica_staleness_risk_units);
  AppendReplayField(bytes, cost.quorum_availability_risk_units);
  AppendReplayField(bytes, cost.donor_compatibility_enforcement_units);
  AppendReplayField(bytes, cost.result_ordering_enforcement_units);
  AppendReplayField(bytes, cost.plan_instability_penalty);
  AppendReplayField(bytes, cost.complete_dimension_vector);
  AppendReplayField(bytes, cost.confidence);
}

void AppendReplayStringVector(std::string* bytes,
                              const std::vector<std::string>& values) {
  AppendReplayField(bytes, static_cast<std::uint64_t>(values.size()));
  for (const auto& value : values) AppendReplayField(bytes, value);
}

void AppendReplayDag(std::string* bytes,
                     const exec::TypedPhysicalNodeDag& dag) {
  AppendReplayField(bytes, dag.abi_version);
  AppendReplayField(bytes, dag.selected_plan_uuid);
  AppendReplayField(bytes, dag.root_physical_node_id);
  AppendReplayField(bytes, dag.local_transaction_id);
  AppendReplayField(bytes, dag.statement_snapshot_id);
  AppendReplayMga(bytes, dag.mga_statement_context);
  AppendReplayField(bytes,
                    static_cast<std::uint64_t>(dag.admission_evidence.size()));
  for (const auto& evidence : dag.admission_evidence) {
    AppendReplayField(bytes, static_cast<std::uint64_t>(evidence.stage));
    AppendReplayField(bytes, evidence.evidence_uuid);
  }
  AppendReplayField(bytes, static_cast<std::uint64_t>(dag.nodes.size()));
  for (const auto& node : dag.nodes) {
    AppendReplayField(bytes, node.physical_node_id);
    AppendReplayField(bytes, node.relational_node_id);
    AppendReplayField(bytes, static_cast<std::uint64_t>(node.node_kind));
    AppendReplayField(bytes, node.implementation_id);
    AppendReplayField(
        bytes, static_cast<std::uint64_t>(node.input_physical_node_ids.size()));
    for (const auto id : node.input_physical_node_ids) {
      AppendReplayField(bytes, id);
    }
    AppendReplayField(
        bytes, static_cast<std::uint64_t>(node.output_descriptor_ids.size()));
    for (const auto id : node.output_descriptor_ids) {
      AppendReplayField(bytes, id);
    }
    AppendReplayField(bytes, node.shareable);
    AppendReplayField(bytes, node.causal_counter_id);
    AppendReplayField(bytes, node.selected_alternative_uuid);
    AppendReplayField(bytes, node.executor_capability_uuid);
    AppendReplayField(bytes, node.executor_capability_abi_version);
    AppendReplayField(bytes, node.cost_vector_uuid);
    AppendReplayStringVector(bytes, node.required_property_uuids);
    AppendReplayStringVector(bytes, node.delivered_property_uuids);
    AppendReplayField(bytes, node.memory_bytes_required);
    AppendReplayField(bytes, node.spill_bytes_expected);
    AppendReplayField(bytes, node.engine_capability_validated);
    AppendReplayMga(bytes, node.mga_statement_context);
    AppendReplayField(bytes, node.logical_semantic_variant_id);
    AppendReplayField(bytes, node.publication_ordinal);
    AppendReplayField(bytes, node.transformation_uuid);
    AppendReplayField(bytes, node.transformation_rule_id);
    AppendReplayStringVector(bytes, node.enforced_property_uuids);
    AppendReplayPhysicalCost(bytes, node.retained_cost);
  }
  AppendReplayField(bytes, dag.bound_sblr_tree_uuid);
  AppendReplayField(bytes, dag.catalog_epoch_uuid);
  AppendReplayField(bytes, dag.security_context_uuid);
  AppendReplayField(bytes, dag.capability_snapshot_uuid);
  AppendReplayField(bytes, dag.resource_snapshot_uuid);
  AppendReplayField(bytes, dag.statistics_snapshot_uuid);
  AppendReplayField(bytes, dag.route_snapshot_uuid);
  AppendReplayField(bytes, dag.catalog_generation);
  AppendReplayField(bytes, dag.security_epoch);
  AppendReplayField(bytes, dag.policy_epoch);
  AppendReplayField(bytes, dag.resource_epoch);
  AppendReplayField(bytes, dag.statistics_generation);
  AppendReplayField(bytes, dag.route_epoch);
  AppendReplayField(bytes, dag.route_generation);
  AppendReplayField(bytes, dag.memory_budget_bytes);
  AppendReplayField(bytes, dag.spill_allowed);
  AppendReplayField(bytes, dag.optimizer_published);
  AppendReplayField(bytes, dag.immutable_node_identity_validated);
  AppendReplayField(bytes, dag.capability_validated_before_access);
  AppendReplayField(bytes, dag.data_access_observed);
  AppendReplayField(bytes, dag.parser_execution_authority_claimed);
  AppendReplayField(bytes, dag.transaction_finality_authority_claimed);
  AppendReplayField(bytes, dag.publication_contract_version);
  AppendReplayField(bytes, dag.selected_plan_signature);
  AppendReplayField(bytes, dag.selected_scalar_score);
  AppendReplayField(bytes, dag.published_node_count);
  AppendReplayField(bytes, dag.first_causal_counter_id);
  AppendReplayField(bytes, dag.complete_cost_vectors_retained);
  AppendReplayField(bytes, dag.descriptor_contract_validated);
  AppendReplayField(bytes, dag.property_contract_validated);
  AppendReplayField(bytes, dag.dependency_contract_validated);
  AppendReplayField(bytes, dag.resource_contract_validated);
  AppendReplayField(bytes, dag.mga_contract_validated);
  AppendReplayField(bytes, dag.causal_identity_validated);
}

std::string CanonicalReplayBytes(
    const opt::ModelFamilyCoordinatorResultV1& planning,
    const exec::ModelFamilyExecutionResultV1& execution) {
  std::string bytes;
  AppendReplayField(&bytes, planning.accepted);
  AppendReplayField(&bytes, planning.selected);
  AppendReplayField(&bytes, planning.data_access_allowed);
  AppendReplayField(&bytes, planning.deterministic);
  AppendReplayField(&bytes, planning.exact_fallback_selected);
  AppendReplayCandidate(&bytes, planning.selected_candidate);
  AppendReplayDag(&bytes, planning.physical_dag);
  AppendReplayField(&bytes, planning.logical_operator_id);
  AppendReplayField(&bytes, planning.physical_operator_id);
  AppendReplayField(&bytes, planning.diagnostic_id);
  AppendReplayField(&bytes, planning.detail);
  AppendReplayField(&bytes, execution.accepted);
  AppendReplayField(&bytes, execution.execution_started);
  AppendReplayField(&bytes, execution.data_access_observed);
  AppendReplayField(&bytes, execution.rows_examined);
  AppendReplayField(&bytes, execution.root_published);
  AppendReplayField(&bytes, execution.cleanup_complete);
  AppendReplayField(&bytes, execution.cleanup_count);
  AppendReplayOutput(&bytes, execution.output);
  AppendReplayField(&bytes, execution.diagnostic_id);
  AppendReplayField(&bytes, execution.detail);
  return bytes;
}

std::string ReplayDigest(const std::string& bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data()),
      bytes.size());
  return digest.ok() ? scratchbird::core::hash::HexLower(digest.digest)
                     : std::string{};
}

exec::ModelSourceOutputDescriptorV1 ExpectedFixtureOutput(
    const VectorPair& vector) {
  const auto row_uuid = Descriptor(
      501, FixtureEngineType(vector.typed_schema[0]),
      vector.typed_schema[0].nullable);
  const auto join_key = Descriptor(
      502, FixtureEngineType(vector.typed_schema[1]),
      vector.typed_schema[1].nullable);
  const auto payload = Descriptor(
      503, FixtureEngineType(vector.typed_schema[2]),
      vector.typed_schema[2].nullable);
  exec::ModelSourceOutputDescriptorV1 expected;
  expected.abi_version = 1;
  expected.output_descriptor_id = "SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1";
  expected.family_id = vector.family_id;
  expected.operation_id = "DOCUMENT_PATH";
  expected.object_uuid = FixtureObjectUuid(vector);
  expected.physical_node_id = 1;
  expected.selected_alternative_uuid = Uuid(9);
  expected.capability_uuid = Uuid(11);
  expected.provider_uuid = Uuid(10);
  expected.provider_generation = vector.selected_generations.provider;
  expected.result_handle_uuid = Uuid(13);
  expected.causal_counter_id = 1;
  expected.output_descriptor_ids = {101, 102, 103};
  expected.ordered_row_identities = {
      {Uuid(1301), vector.literal_rows[0].row_uuid},
      {Uuid(1302), vector.literal_rows[1].row_uuid},
  };
  expected.batch.columns = {
      {vector.typed_schema[0].field_id, row_uuid,
       vector.typed_schema[0].nullable, 101},
      {vector.typed_schema[1].field_id, join_key,
       vector.typed_schema[1].nullable, 102},
      {vector.typed_schema[2].field_id, payload,
       vector.typed_schema[2].nullable, 103},
  };
  expected.batch.rows = {
      {{Value(row_uuid, vector.literal_rows[0].row_uuid),
        Value(join_key, std::to_string(vector.literal_rows[0].join_key)),
        Value(payload, vector.literal_rows[0].payload)}},
      {{Value(row_uuid, vector.literal_rows[1].row_uuid),
        Value(join_key, std::to_string(vector.literal_rows[1].join_key)),
        Value(payload, vector.literal_rows[1].payload)}},
  };
  expected.properties.abi_version = 1;
  expected.properties.property_descriptor_id =
      "SB_MODEL_PROPERTY_DESCRIPTOR_V1";
  expected.properties.property_uuid = Uuid(16);
  expected.properties.ordering_id = "fixture_order";
  expected.properties.partitioning_id = "single_local_partition";
  expected.properties.uniqueness_id = "document_uuid";
  expected.properties.exact = true;
  expected.properties.residual_recheck_complete = true;
  expected.properties.base_row_mga_recheck_complete = true;
  expected.properties.security_recheck_complete = true;
  expected.mga_statement_context = Mga(vector);
  expected.security_receipt_uuid = Uuid(17);
  expected.exact_exchange_validated = true;
  return expected;
}

bool DeterministicReplay() {
  const auto planning_request = PlanningRequest(kVectors[11]);
  const auto first_plan = opt::CoordinateDocumentFamilySourceV1(planning_request);
  const auto second_plan = opt::CoordinateDocumentFamilySourceV1(planning_request);
  auto first_request = ExecutionRequest(kVectors[11], false);
  auto second_request = ExecutionRequest(kVectors[11], false);
  const auto first = exec::ExecuteModelFamilySourceV1(first_request);
  const auto second = exec::ExecuteModelFamilySourceV1(second_request);
  const auto first_bytes = CanonicalReplayBytes(first_plan, first);
  const auto second_bytes = CanonicalReplayBytes(second_plan, second);
  const auto first_digest = ReplayDigest(first_bytes);
  const auto second_digest = ReplayDigest(second_bytes);
  std::string first_output_bytes;
  AppendReplayOutput(&first_output_bytes, first.output);
  std::string expected_output_bytes;
  AppendReplayOutput(&expected_output_bytes,
                     ExpectedFixtureOutput(kVectors[11]));
  const auto first_output_digest = ReplayDigest(first_output_bytes);
  const auto expected_output_digest = ReplayDigest(expected_output_bytes);
  constexpr std::string_view kExpectedFixtureDigest =
      "73c6e90362edd3e0f12d03fb73544812ddfad7b8569c4b4d2473ea8750c491c4";
  return Require(
      first_plan.accepted && second_plan.accepted && first.accepted &&
          second.accepted && first_bytes == second_bytes &&
          !first_digest.empty() && first_digest == second_digest &&
          first_output_bytes == expected_output_bytes &&
          !expected_output_digest.empty() &&
          first_output_digest == expected_output_digest &&
          expected_output_digest == kExpectedFixtureDigest &&
          first.output.mga_statement_context.statement_uuid ==
              kVectors[11].statement_uuid &&
          first.output.ordered_row_identities.size() == 2 &&
          first.output.ordered_row_identities[0].row_uuid ==
              kVectors[11].literal_rows[0].row_uuid &&
          first.output.ordered_row_identities[1].row_uuid ==
              kVectors[11].literal_rows[1].row_uuid,
      "deterministic replay complete canonical digest drifted: full=" +
          first_digest + ";fixture=" + expected_output_digest);
}

bool MissingNullAndUnavailableFallback() {
  bool passed = true;
  auto missing = ExecutionRequest(kVectors[0], false);
  const auto missing_provider = missing.execute_provider;
  missing.execute_provider = [missing_provider](const auto& input) {
    auto provider = missing_provider(input);
    provider.provider_batch.batch.rows[0].values[0].encoded_value.clear();
    provider.provider_batch.batch.rows[0].values[0].setState(
        api::EngineValueState::missing);
    return provider;
  };
  const auto refused = exec::ExecuteModelFamilySourceV1(missing);
  passed &= Require(!refused.accepted &&
                        refused.diagnostic_id ==
                            "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1" &&
                        !refused.root_published,
                    "non-null descriptor accepted a missing document value");

  auto nullable = missing;
  nullable.execute_provider = [missing_provider](const auto& input) {
    auto provider = missing_provider(input);
    provider.provider_batch.batch.rows[0].values[2].encoded_value.clear();
    provider.provider_batch.batch.rows[0].values[2].setState(
        api::EngineValueState::missing);
    provider.provider_batch.batch.rows[1].values[2].encoded_value.clear();
    provider.provider_batch.batch.rows[1].values[2].setState(
        api::EngineValueState::sql_null);
    return provider;
  };
  const auto converted = exec::ExecuteModelFamilySourceV1(nullable);
  passed &= Require(converted.accepted &&
                        converted.output.batch.rows[0].values[2].state ==
                            api::EngineValueState::sql_null &&
                        converted.output.batch.rows[1].values[2].state ==
                            api::EngineValueState::sql_null,
                    "explicit nullable missing projection was not normalized");

  auto unavailable = ExecutionRequest(kVectors[10], false);
  unavailable.exact_fallback_selected = true;
  unavailable.capability.exact_collection_fallback_available = false;
  const auto unavailable_result = exec::ExecuteModelFamilySourceV1(unavailable);
  passed &= Require(!unavailable_result.accepted &&
                        !unavailable_result.execution_started &&
                        unavailable_result.diagnostic_id ==
                            "SB_MODEL_DOCUMENT_EXACT_FALLBACK_UNAVAILABLE_V1",
                    "unavailable exact fallback diagnostic drifted");
  return passed;
}

bool LogicalModelIdentityAdmission() {
  bool passed = true;
  const auto source = plan::ValidateCanonicalLogicalRelationalGraph(
      LogicalModelGraph("SBLR_MODEL_SOURCE_V1"));
  const auto expand = plan::ValidateCanonicalLogicalRelationalGraph(
      LogicalModelGraph("SBLR_MODEL_EXPAND_V1"));
  passed &= Require(source.accepted && source.validated_node_count == 1,
                    "complete SBLR_MODEL_SOURCE_V1 binding was refused");
  passed &= Require(expand.accepted && expand.validated_node_count == 1,
                    "complete SBLR_MODEL_EXPAND_V1 binding was refused");

  const auto expect_identity_refusal = [&](std::string semantic,
                                           const std::string_view detail) {
    const auto refused = plan::ValidateCanonicalLogicalRelationalGraph(
        LogicalModelGraph(std::move(semantic)));
    return Require(!refused.accepted && refused.validated_node_count == 0 &&
                       HasLogicalIssue(refused, "logical_node_record"),
                   detail);
  };
  passed &= expect_identity_refusal(
      "SBLR_MODEL_UNKNOWN_V1", "unknown uppercase model identity was admitted");
  passed &= expect_identity_refusal(
      "sblr_model_source_v1", "lowercase model-source alias was admitted");
  passed &= expect_identity_refusal(
      "sblr_model_expand_v1", "lowercase model-expand alias was admitted");

  const auto expect_shape_refusal = [&](auto mutation,
                                        const std::string_view detail) {
    auto graph = LogicalModelGraph("SBLR_MODEL_SOURCE_V1");
    mutation(graph.nodes.front());
    const auto refused =
        plan::ValidateCanonicalLogicalRelationalGraph(graph);
    return Require(
        !refused.accepted && refused.validated_node_count == 0 &&
            HasLogicalIssue(refused, "model_semantic_node_shape"),
        detail);
  };
  passed &= expect_shape_refusal(
      [](auto& node) {
        node.node_kind = plan::CanonicalLogicalRelationalNodeKind::kFilter;
      },
      "model identity with the wrong logical node kind was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.input_logical_node_ids = {2}; },
      "model identity with an input edge was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.bound_expression_ids.clear(); },
      "model identity without a bound expression was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.output_descriptor_ids.clear(); },
      "model identity without an output descriptor was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.output_descriptor_ids.push_back(102); },
      "model identity with mismatched expression/output width was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.required_object_uuids.clear(); },
      "model source without its required object UUID was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.required_object_uuids.push_back(Uuid(7305)); },
      "model source with an extra object UUID was admitted");
  passed &= expect_shape_refusal(
      [](auto& node) { node.required_object_uuids = {"not-a-uuid"}; },
      "model source with a malformed object UUID was admitted");

  auto expand_with_object = LogicalModelGraph("SBLR_MODEL_EXPAND_V1");
  expand_with_object.nodes.front().required_object_uuids = {Uuid(7306)};
  const auto refused_expand =
      plan::ValidateCanonicalLogicalRelationalGraph(expand_with_object);
  passed &= Require(
      !refused_expand.accepted && refused_expand.validated_node_count == 0 &&
          HasLogicalIssue(refused_expand, "model_semantic_node_shape"),
      "model expand with a required object UUID was admitted");

  auto parser_authority = LogicalModelGraph("SBLR_MODEL_SOURCE_V1");
  parser_authority.parser_execution_authority_claimed = true;
  const auto refused_authority =
      plan::ValidateCanonicalLogicalRelationalGraph(parser_authority);
  passed &= Require(
      !refused_authority.accepted &&
          std::ranges::any_of(refused_authority.issues,
                              [](const auto& issue) {
                                return issue.diagnostic_id ==
                                           "QOW-DIAG-LOGICAL-GRAPH-AUTHORITY-V1" &&
                                       issue.field_id ==
                                           "forbidden_authority_claim";
                              }),
      "model identity acquired parser execution authority");

  auto finality_authority = LogicalModelGraph("SBLR_MODEL_SOURCE_V1");
  finality_authority.transaction_finality_authority_claimed = true;
  const auto refused_finality =
      plan::ValidateCanonicalLogicalRelationalGraph(finality_authority);
  passed &= Require(
      !refused_finality.accepted &&
          std::ranges::any_of(refused_finality.issues,
                              [](const auto& issue) {
                                return issue.diagnostic_id ==
                                           "QOW-DIAG-LOGICAL-GRAPH-AUTHORITY-V1" &&
                                       issue.field_id ==
                                           "forbidden_authority_claim";
                              }),
      "model identity acquired transaction-finality authority");
  return passed;
}

#if defined(SB_CES05_PRODUCTION_QUERY_ROUTE)
std::uint64_t ProductionSeed() {
  static std::uint64_t ordinal = 0;
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         ++ordinal;
}

platform::TypedUuid ProductionUuid(const platform::UuidKind kind) {
  return uuid::GenerateEngineIdentityV7(kind, ProductionSeed()).value;
}

std::string ProductionUuidText(const platform::UuidKind kind) {
  return uuid::UuidToString(ProductionUuid(kind).value);
}

std::string ProductionExactCoreTypeUuid(const std::string_view stable_name) {
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (count != 1 || found == manifest.manifest.descriptor_rows.end() ||
      !found->descriptor_uuid.valid()) {
    return {};
  }
  const auto descriptor_uuid = uuid::UuidToString(found->descriptor_uuid.value);
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701",
      manifest.manifest.catalog_epoch, 1, descriptor_uuid,
      found->descriptor_epoch);
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

std::optional<std::string> ProductionDescriptorField(
    const api::EngineDescriptor& descriptor, const std::string_view key) {
  const auto prefix = std::string(key) + "=";
  std::optional<std::string> value;
  std::size_t begin = 0;
  while (begin <= descriptor.encoded_descriptor.size()) {
    const auto end = descriptor.encoded_descriptor.find(';', begin);
    const auto field = std::string_view(descriptor.encoded_descriptor).substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (field.starts_with(prefix)) {
      if (value.has_value()) return std::nullopt;
      value = std::string(field.substr(prefix.size()));
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return value;
}

struct ProductionFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string collection_uuid;
  std::string other_collection_uuid;
  api::MgaRelationStorageDescriptor collection_descriptor;
  api::MgaRelationStorageDescriptor other_collection_descriptor;

  ~ProductionFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

bool MakeProductionFixture(ProductionFixture* fixture) {
  fixture->directory =
      std::filesystem::temp_directory_path() /
      ("scratchbird_rcp073_document_" + std::to_string(ProductionSeed()));
  std::error_code error;
  if (!std::filesystem::create_directories(fixture->directory, error) || error) {
    return Require(false, "production route fixture directory creation failed");
  }
  fixture->database_path = fixture->directory / "document.sbdb";
  db::DatabaseCreateConfig create;
  create.path = fixture->database_path.string();
  create.database_uuid = ProductionUuid(platform::UuidKind::database);
  create.filespace_uuid = ProductionUuid(platform::UuidKind::filespace);
  create.creation_unix_epoch_millis = ProductionSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    return Require(false, "production route database creation failed");
  }
  fixture->database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture->schema_uuid = ProductionUuidText(platform::UuidKind::schema);
  fixture->collection_uuid = ProductionUuidText(platform::UuidKind::object);
  fixture->other_collection_uuid =
      ProductionUuidText(platform::UuidKind::object);
  return true;
}

api::EngineRequestContext ProductionBaseContext(
    const ProductionFixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      ProductionUuidText(platform::UuidKind::principal);
  context.session_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 73;
  context.security_epoch = 74;
  context.resource_epoch = 75;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.name_resolution_epoch = 76;
  return context;
}

bool BeginProductionTransaction(const ProductionFixture& fixture,
                                std::string request_id,
                                api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = ProductionBaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    return Require(false, "production route transaction begin failed");
  }
  *context = request.context;
  context->transaction_uuid = begun.transaction_uuid;
  context->local_transaction_id = begun.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool CommitProductionTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return Require(api::EngineCommitTransaction(request).ok,
                 "production route transaction commit failed");
}

void AddProductionAuthorization(api::EngineRequestContext* context,
                                const std::string& right,
                                const std::string& target_uuid) {
  if (!context->authorization_context.present) {
    context->authorization_context.present = true;
    context->authorization_context.authority_uuid.canonical =
        ProductionUuidText(platform::UuidKind::object);
    context->authorization_context.principal_uuid = context->principal_uuid;
    context->authorization_context.security_epoch = context->security_epoch;
    context->authorization_context.policy_epoch = 77;
    context->authorization_context.catalog_generation_id =
        context->catalog_generation_id;
    context->authorization_context.effective_subjects.push_back(
        {context->principal_uuid, "principal"});
  }
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = target_uuid;
  grant.right = right;
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

api::EngineTypedValue ProductionText(std::string value) {
  api::EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineLocalizedName ProductionName(std::string name) {
  return {"en", "primary", "", std::move(name), true};
}

api::EngineColumnDefinition ProductionTextColumn(
    const std::uint32_t ordinal, std::string name, const bool nullable) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  column.names.push_back(ProductionName(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "canonical=text";
  column.ordinal = ordinal;
  column.nullable = nullable;
  return column;
}

bool CreateProductionCollections(
    ProductionFixture* fixture,
    const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = fixture->schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(ProductionName("document_schema"));
  const auto schema_created = api::EngineCreateSchema(schema);
  if (!schema_created.ok) {
    for (const auto& diagnostic : schema_created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    return Require(false, "production document schema creation failed");
  }
  const auto create_collection = [&](const std::string& relation_uuid,
                                     const std::string& name,
                                     api::MgaRelationStorageDescriptor* out) {
    api::EngineCreateTableRequest table;
    table.context = context;
    table.context.current_schema_uuid.canonical.clear();
    table.target_schema.uuid.canonical = fixture->schema_uuid;
    table.target_schema.object_kind = "schema";
    table.requested_table_uuid.canonical = relation_uuid;
    table.table_names.push_back(ProductionName(name));
    table.table_columns.push_back(
        ProductionTextColumn(0, "payload", true));
    table.table_columns.push_back(
        ProductionTextColumn(1, "payload_shadow", true));
    table.table_columns.push_back(
        ProductionTextColumn(2, "required_shadow", false));
    const auto created = api::EngineCreateTable(table);
    if (!created.ok) {
      for (const auto& diagnostic : created.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                  << diagnostic.detail << '\n';
      }
      return false;
    }
    const auto loaded =
        api::LoadMgaRelationStorageDescriptor(context, relation_uuid);
    if (!loaded.ok || loaded.descriptor.columns.size() != 3 ||
        loaded.descriptor.columns[0].value_descriptor.canonical_type_name !=
            "text" ||
        loaded.descriptor.columns[1].value_descriptor.canonical_type_name !=
            "text") {
      std::cerr << loaded.diagnostic.code << ':'
                << loaded.diagnostic.message_key << ':'
                << loaded.diagnostic.detail << '\n';
      return false;
    }
    *out = loaded.descriptor;
    return true;
  };
  return Require(
      create_collection(fixture->collection_uuid, "documents_a",
                        &fixture->collection_descriptor) &&
          create_collection(fixture->other_collection_uuid, "documents_b",
                            &fixture->other_collection_descriptor),
      "production persisted document collections were not created");
}

bool InsertProductionDocument(const api::EngineRequestContext& context,
                              const std::string& collection_uuid,
                              const std::string& document_uuid,
                              const std::string& row_uuid,
                              std::string name,
                              std::string payload) {
  api::EngineDocumentInsertRequest request;
  request.context = context;
  request.collection_uuid = collection_uuid;
  request.document_uuid = document_uuid;
  request.row_uuid = row_uuid;
  request.target_object.uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  request.localized_names.push_back(
      {"en", "primary", "", std::move(name), true});
  request.assignments.push_back(
      {"payload", ProductionText(payload)});
  if (!Require(api::EngineDocumentInsert(request).ok,
               "production route document insert failed")) {
    return false;
  }
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = collection_uuid;
  row.row_uuid = row_uuid;
  row.version_uuid = ProductionUuidText(platform::UuidKind::object);
  row.values.push_back({"payload", std::move(payload)});
  std::uint64_t event_sequence = 0;
  const auto appended = api::AppendMgaRowVersion(
      context, row, &event_sequence);
  return Require(!appended.error && event_sequence != 0,
                 "production route document MGA row persistence failed");
}

void AppendLittleEndianU64(std::vector<std::uint8_t>* output,
                           const std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    output->push_back(
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffu));
  }
}

void AddProductionOperand(sblr::SblrOperationEnvelope* envelope,
                          std::string type,
                          std::string name,
                          std::string value) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.ordinal =
      static_cast<std::uint32_t>(envelope->operands.size() + 1);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.assign(16, 0);
  operand.value_body.front() = 0x73;
  AppendLittleEndianU64(&operand.value_body, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(),
                            value.end());
  envelope->operands.push_back(std::move(operand));
}

std::string ProductionOperandText(const sblr::SblrOperand& operand) {
  constexpr std::size_t kTypedLiteralPayloadOffset = 24;
  if (operand.value_body.size() < kTypedLiteralPayloadOffset) return {};
  return std::string(
      operand.value_body.begin() + kTypedLiteralPayloadOffset,
      operand.value_body.end());
}

void SetProductionOperandText(sblr::SblrOperand* operand,
                              const std::string_view value) {
  if (operand == nullptr) return;
  operand->value.clear();
  operand->value_kind = sblr::SblrValueKind::literal_typed;
  operand->value_body.assign(16, 0);
  operand->value_body.front() = 0x73;
  AppendLittleEndianU64(&operand->value_body, value.size());
  operand->value_body.insert(operand->value_body.end(), value.begin(),
                             value.end());
}

std::string ProductionHex(const std::string_view value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    encoded.push_back(kDigits[byte >> 4]);
    encoded.push_back(kDigits[byte & 0x0f]);
  }
  return encoded;
}

std::string ProductionDescriptorField(const std::string& encoded,
                                      const std::string_view key) {
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = std::string_view(encoded).substr(
        offset, end == std::string::npos ? std::string::npos
                                         : end - offset);
    const auto equal = field.find('=');
    if (equal != std::string_view::npos && field.substr(0, equal) == key) {
      return std::string(field.substr(equal + 1));
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return {};
}

sblr::SblrOperationEnvelope ProductionDocumentEnvelope(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& collection,
    const std::string_view projected_path = "payload") {
  const auto* operation = sblr::LookupSblrOperation("query.execute");
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "rcp073.document.production.query.execute");
  envelope.opcode_code = operation == nullptr ? 0 : operation->code;
  envelope.parser_package_uuid =
      "019f0730-0000-7000-8000-000000000001";
  envelope.registry_snapshot_uuid =
      "019f0730-0000-7000-8000-000000000002";
  envelope.result_shape = "query_execute_result";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;

  const auto bound_tree_uuid =
      ProductionUuidText(platform::UuidKind::object);
  const auto persisted_column = std::ranges::find_if(
      collection.columns, [&](const auto& column) {
        return column.canonical_name_key == projected_path;
      });
  const auto& descriptor_source =
      persisted_column == collection.columns.end()
          ? collection.columns.front()
          : *persisted_column;
  const auto descriptor_uuid =
      descriptor_source.value_descriptor.descriptor_uuid.canonical;
  const auto type_uuid = ProductionDescriptorField(
      descriptor_source.value_descriptor.encoded_descriptor, "type_uuid");
  const auto optional_transport = [&](const std::string_view field) {
    const auto value = ProductionDescriptorField(
        descriptor_source.value_descriptor.encoded_descriptor, field);
    return value.empty() ? std::string("-") : value;
  };
  AddProductionOperand(&envelope, "uint16", "relational_wire_version", "2");
  AddProductionOperand(&envelope, "uuid", "relational_bound_sblr_tree_uuid",
                       bound_tree_uuid);
  AddProductionOperand(&envelope, "uuid", "relational_catalog_epoch_uuid",
                       context.catalog_epoch_uuid.canonical);
  AddProductionOperand(&envelope, "uuid", "relational_security_context_uuid",
                       context.authorization_context.authority_uuid.canonical);
  AddProductionOperand(&envelope, "uuid", "relational_statement_uuid",
                       context.statement_uuid.canonical);
  AddProductionOperand(&envelope, "uuid",
                       "relational_owning_transaction_uuid",
                       context.transaction_uuid.canonical);
  AddProductionOperand(&envelope, "uuid",
                       "relational_statement_snapshot_uuid",
                       context.statement_snapshot_uuid.canonical);
  AddProductionOperand(&envelope, "uuid",
                       "relational_statement_metadata_snapshot_uuid",
                       context.statement_metadata_snapshot_uuid.canonical);
  AddProductionOperand(&envelope, "uint64",
                       "relational_local_transaction_id",
                       std::to_string(context.local_transaction_id));
  AddProductionOperand(
      &envelope, "uint64",
      "relational_snapshot_visible_through_local_transaction_id",
      std::to_string(
          context.snapshot_visible_through_local_transaction_id));
  AddProductionOperand(&envelope, "uint32", "relational_root_node_id", "1");
  AddProductionOperand(&envelope, "relational_descriptor_v1", "slot_1",
                       descriptor_uuid + "|" + type_uuid +
                           (descriptor_source.nullable ? "|2|" : "|1|") +
                           optional_transport("collation_uuid") + "|" +
                           optional_transport("timezone_profile_id") + "|" +
                           optional_transport("width") + "|" +
                           optional_transport("precision") + "|" +
                           optional_transport("scale"));
  std::uint32_t output_expression_id = 1;
  if (persisted_column != collection.columns.end()) {
    AddProductionOperand(
        &envelope, "relational_expression_v1", "slot_1",
        "3|-|1|-|" + persisted_column->column_uuid.canonical + "|-|-|-");
  } else {
    AddProductionOperand(
        &envelope, "relational_expression_v1", "slot_1",
        "3|-|1|-|" + collection.relation_uuid.canonical + "|-|-|-");
    AddProductionOperand(
        &envelope, "relational_expression_v1", "slot_2",
        "1|-|1|-|-|2|-|" + ProductionHex(projected_path));
    AddProductionOperand(
        &envelope, "relational_expression_v1", "slot_3",
        "4|1,2|1|-|-|-|" + ProductionHex("DOCUMENT_PATH") + "|-");
    output_expression_id = 3;
  }
  AddProductionOperand(
      &envelope, "relational_output_v1", "slot_1",
      "1|" + std::to_string(output_expression_id) + "|1|1|0|" +
          ProductionHex(projected_path));
  AddProductionOperand(&envelope, "relational_node_v1", "slot_1",
                       "1|0|-|1|-");
  AddProductionOperand(
      &envelope, "relational_node_binding_v1", "slot_1",
      "53424c525f4d4f44454c5f534f555243455f5631|" +
          std::to_string(output_expression_id) + "|" +
          collection.relation_uuid.canonical + "|-|-");
  return envelope;
}

enum class ProductionUnnestMutation {
  none,
  operator_case,
  function_uuid,
  source_semantic,
  wrong_node_kind,
  input_edge,
  object_uuid,
  extra_bound_root,
  missing_bound_root,
  reversed_children,
  duplicate_child,
  missing_child,
  wrong_path_kind,
  output_type,
  orphan_expression,
  output_expression,
};

sblr::SblrOperationEnvelope ProductionDocumentUnnestEnvelope(
    const api::EngineRequestContext& context,
    const std::string_view document = R"({"items":[3,1,2]})",
    const std::string_view path = "$.items[*]",
    const ProductionUnnestMutation mutation =
        ProductionUnnestMutation::none) {
  const auto* operation = sblr::LookupSblrOperation("query.execute");
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "rcp073.document.unnest.production.query.execute");
  envelope.opcode_code = operation == nullptr ? 0 : operation->code;
  envelope.parser_package_uuid =
      "019f0730-0000-7000-8000-000000000001";
  envelope.registry_snapshot_uuid =
      "019f0730-0000-7000-8000-000000000002";
  envelope.result_shape = "query_execute_result";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;

  const auto json_type_uuid = ProductionExactCoreTypeUuid("json_document");
  const auto character_type_uuid = ProductionExactCoreTypeUuid("character");
  const auto bound_tree_uuid =
      ProductionUuidText(platform::UuidKind::object);
  const auto document_descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  const auto output_descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  const auto path_descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  const auto mutation_uuid =
      ProductionUuidText(platform::UuidKind::object);
  AddProductionOperand(&envelope, "uint16", "relational_wire_version", "2");
  AddProductionOperand(&envelope, "uuid", "relational_bound_sblr_tree_uuid",
                       bound_tree_uuid);
  AddProductionOperand(&envelope, "uuid", "relational_catalog_epoch_uuid",
                       context.catalog_epoch_uuid.canonical);
  AddProductionOperand(&envelope, "uuid", "relational_security_context_uuid",
                       context.authorization_context.authority_uuid.canonical);
  AddProductionOperand(&envelope, "uuid", "relational_statement_uuid",
                       context.statement_uuid.canonical);
  AddProductionOperand(&envelope, "uuid",
                       "relational_owning_transaction_uuid",
                       context.transaction_uuid.canonical);
  AddProductionOperand(&envelope, "uuid",
                       "relational_statement_snapshot_uuid",
                       context.statement_snapshot_uuid.canonical);
  AddProductionOperand(&envelope, "uuid",
                       "relational_statement_metadata_snapshot_uuid",
                       context.statement_metadata_snapshot_uuid.canonical);
  AddProductionOperand(&envelope, "uint64",
                       "relational_local_transaction_id",
                       std::to_string(context.local_transaction_id));
  AddProductionOperand(
      &envelope, "uint64",
      "relational_snapshot_visible_through_local_transaction_id",
      std::to_string(
          context.snapshot_visible_through_local_transaction_id));
  AddProductionOperand(&envelope, "uint32", "relational_root_node_id", "1");
  AddProductionOperand(&envelope, "relational_descriptor_v1", "slot_1",
                       document_descriptor_uuid + "|" + json_type_uuid +
                           "|1|-|-|-|-|-");
  AddProductionOperand(&envelope, "relational_descriptor_v1", "slot_2",
                       output_descriptor_uuid + "|" +
                           (mutation == ProductionUnnestMutation::output_type
                                ? character_type_uuid
                                : json_type_uuid) +
                           "|2|-|-|-|-|-");
  AddProductionOperand(&envelope, "relational_descriptor_v1", "slot_3",
                       path_descriptor_uuid + "|" + character_type_uuid +
                           "|1|-|-|-|-|-");
  AddProductionOperand(
      &envelope, "relational_expression_v1", "slot_1",
      "1|-|1|-|-|9|-|" + ProductionHex(document));
  AddProductionOperand(
      &envelope, "relational_expression_v1", "slot_2",
      std::string("1|-|3|-|-|") +
          (mutation == ProductionUnnestMutation::wrong_path_kind ? "1" : "2") +
          "|-|" + ProductionHex(path));
  const auto root_children =
      mutation == ProductionUnnestMutation::reversed_children
          ? std::string("2,1")
          : mutation == ProductionUnnestMutation::duplicate_child
              ? std::string("1,1")
              : mutation == ProductionUnnestMutation::missing_child
                  ? std::string("1")
                  : std::string("1,2");
  AddProductionOperand(
      &envelope, "relational_expression_v1", "slot_3",
      "4|" + root_children + "|2|" +
          (mutation == ProductionUnnestMutation::function_uuid ? mutation_uuid
                                                                : "-") +
          "|-|-|" +
          ProductionHex(mutation == ProductionUnnestMutation::operator_case
                            ? "document_unnest"
                            : "DOCUMENT_UNNEST") +
          "|-");
  if (mutation == ProductionUnnestMutation::orphan_expression) {
    AddProductionOperand(&envelope, "relational_expression_v1", "slot_4",
                         "1|-|3|-|-|1|-|31");
  }
  AddProductionOperand(&envelope, "relational_output_v1", "slot_1",
                       "1|" +
                           std::string(
                               mutation ==
                                       ProductionUnnestMutation::output_expression
                                   ? "1"
                                   : "3") +
                           "|2|1|0|" + ProductionHex("item"));
  AddProductionOperand(&envelope, "relational_node_v1", "slot_1",
                       std::string(
                           mutation == ProductionUnnestMutation::wrong_node_kind
                               ? "2"
                               : "1") +
                           "|0|" +
                           (mutation == ProductionUnnestMutation::input_edge
                                ? "2"
                                : "-") +
                           "|2|-");
  AddProductionOperand(
      &envelope, "relational_node_binding_v1", "slot_1",
      ProductionHex(mutation == ProductionUnnestMutation::source_semantic
                        ? "SBLR_MODEL_SOURCE_V1"
                        : "SBLR_MODEL_EXPAND_V1") +
          "|" +
          (mutation == ProductionUnnestMutation::extra_bound_root
               ? "3,1"
               : mutation == ProductionUnnestMutation::missing_bound_root
                   ? "-"
                   : "3") +
          "|" +
          (mutation == ProductionUnnestMutation::object_uuid ? mutation_uuid
                                                              : "-") +
          "|-|-");
  return envelope;
}

enum class ProductionUnnestCompositionMutation {
  none,
  disconnected_limit,
  orphan_consumer,
  substituted_producer,
};

sblr::SblrOperationEnvelope ProductionDocumentUnnestFilterProjectLimitEnvelope(
    const api::EngineRequestContext& context,
    const ProductionUnnestCompositionMutation mutation =
        ProductionUnnestCompositionMutation::none) {
  auto envelope = ProductionDocumentUnnestEnvelope(context);
  const auto int64_type_uuid = ProductionExactCoreTypeUuid("int64");
  const auto boolean_type_uuid = ProductionExactCoreTypeUuid("boolean");
  std::string producer_descriptor_uuid;
  for (auto& operand : envelope.operands) {
    if (operand.type == "uint32" &&
        operand.name == "relational_root_node_id") {
      SetProductionOperandText(&operand, "4");
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "slot_2") {
      const auto encoded_descriptor = ProductionOperandText(operand);
      producer_descriptor_uuid =
          encoded_descriptor.substr(0, encoded_descriptor.find('|'));
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "slot_1" &&
               mutation ==
                   ProductionUnnestCompositionMutation::substituted_producer) {
      SetProductionOperandText(
          &operand,
          ProductionHex("SBLR_MODEL_SOURCE_V1") + "|3|-|-|-");
    }
  }
  AddProductionOperand(
      &envelope, "relational_descriptor_v1", "slot_4",
      ProductionUuidText(platform::UuidKind::object) + "|" + boolean_type_uuid +
          "|1|-|-|-|-|-");
  AddProductionOperand(
      &envelope, "relational_descriptor_v1", "slot_5",
      ProductionUuidText(platform::UuidKind::object) + "|" + int64_type_uuid +
          "|1|-|-|-|-|-");
  AddProductionOperand(&envelope, "relational_expression_v1", "slot_4",
                       "1|-|4|-|-|6|-|54525545");
  AddProductionOperand(
      &envelope, "relational_expression_v1", "slot_5",
      "3|-|2|-|" + producer_descriptor_uuid + "|-|-|-");
  AddProductionOperand(&envelope, "relational_expression_v1", "slot_6",
                       "1|-|5|-|-|1|-|32");
  AddProductionOperand(&envelope, "relational_output_v1", "slot_2",
                       "3|5|2|1|0|" + ProductionHex("item"));
  AddProductionOperand(&envelope, "relational_node_v1", "slot_2",
                       "2|0|1|2|-");
  AddProductionOperand(
      &envelope, "relational_node_binding_v1", "slot_2",
      ProductionHex("filter.where.v1") + "|4|-|-|-");
  AddProductionOperand(
      &envelope, "relational_node_v1", "slot_3",
      "3|0|2|2|-");
  AddProductionOperand(
      &envelope, "relational_node_binding_v1", "slot_3",
      ProductionHex("project.select-list.v1") + "|5|-|-|-");
  AddProductionOperand(
      &envelope, "relational_node_v1", "slot_4",
      mutation == ProductionUnnestCompositionMutation::disconnected_limit
          ? "7|0|1|2|-"
          : "7|0|3|2|-");
  AddProductionOperand(
      &envelope, "relational_node_binding_v1", "slot_4",
      ProductionHex("limit.bound-count.v1") + "|6|-|-|-");
  if (mutation == ProductionUnnestCompositionMutation::orphan_consumer) {
    AddProductionOperand(&envelope, "relational_node_v1", "slot_5",
                         "7|0|1|2|-");
    AddProductionOperand(
        &envelope, "relational_node_binding_v1", "slot_5",
        ProductionHex("limit.bound-count.v1") + "|6|-|-|-");
  }
  return envelope;
}

api::TypedRelationalDag ProductionDocumentUnnestSortLimitDag(
    const api::EngineRequestContext& context) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.package_root = api::RelationalPackageRoot::kQueryExecute;
  dag.bound_sblr_tree_uuid = ProductionUuidText(platform::UuidKind::object);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;

  const auto json_type_uuid = ProductionExactCoreTypeUuid("json_document");
  const auto character_type_uuid = ProductionExactCoreTypeUuid("character");
  const auto int64_type_uuid = ProductionExactCoreTypeUuid("int64");
  dag.descriptors = {
      {1, ProductionUuidText(platform::UuidKind::object), json_type_uuid,
       api::RelationalNullability::kNonNull},
      {2, ProductionUuidText(platform::UuidKind::object), json_type_uuid,
       api::RelationalNullability::kNullable},
      {3, ProductionUuidText(platform::UuidKind::object), character_type_uuid,
       api::RelationalNullability::kNonNull},
      {4, ProductionUuidText(platform::UuidKind::object), int64_type_uuid,
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord document;
  document.expression_id = 1;
  document.expression_kind = api::RelationalExpressionKind::kLiteral;
  document.result_descriptor_id = 1;
  document.literal_kind = api::RelationalLiteralKind::kDocument;
  document.literal_or_parameter_ref = R"({"items":[3,1,2]})";
  api::RelationalExpressionRecord path;
  path.expression_id = 2;
  path.expression_kind = api::RelationalExpressionKind::kLiteral;
  path.result_descriptor_id = 3;
  path.literal_kind = api::RelationalLiteralKind::kString;
  path.literal_or_parameter_ref = "$.items[*]";
  api::RelationalExpressionRecord unnest;
  unnest.expression_id = 3;
  unnest.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  unnest.child_expression_ids = {1, 2};
  unnest.result_descriptor_id = 2;
  unnest.operator_name = "DOCUMENT_UNNEST";
  api::RelationalExpressionRecord sort_key;
  sort_key.expression_id = 4;
  sort_key.expression_kind = api::RelationalExpressionKind::kLiteral;
  sort_key.result_descriptor_id = 4;
  sort_key.literal_kind = api::RelationalLiteralKind::kNumeric;
  sort_key.literal_or_parameter_ref = "0";
  api::RelationalExpressionRecord limit;
  limit.expression_id = 5;
  limit.expression_kind = api::RelationalExpressionKind::kLiteral;
  limit.result_descriptor_id = 4;
  limit.literal_kind = api::RelationalLiteralKind::kNumeric;
  limit.literal_or_parameter_ref = "2";
  dag.expressions = {std::move(document), std::move(path), std::move(unnest),
                     std::move(sort_key), std::move(limit)};
  dag.outputs = {{1, 1, 3, "item", 2, true, 0}};
  const auto property_uuid = ProductionUuidText(platform::UuidKind::object);
  api::RelationalPropertyRecord ordering;
  ordering.property_uuid = property_uuid;
  ordering.property_kind = api::RelationalPropertyKind::kOrdering;
  ordering.origin_node_id = 2;
  ordering.ordering_terms.push_back(
      {4, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}});
  dag.properties.push_back(std::move(ordering));
  api::RelationalDagNode producer;
  producer.node_id = 1;
  producer.node_kind = api::RelationalDagNodeKind::kScan;
  producer.output_descriptor_ids = {2};
  producer.bound_expression_ids = {3};
  producer.semantic_variant_id = "SBLR_MODEL_EXPAND_V1";
  api::RelationalDagNode sort;
  sort.node_id = 2;
  sort.node_kind = api::RelationalDagNodeKind::kSort;
  sort.input_node_ids = {1};
  sort.output_descriptor_ids = {2};
  sort.bound_expression_ids = {4};
  sort.semantic_variant_id = "sort.required-order.v1";
  sort.required_property_uuids = {property_uuid};
  sort.delivered_property_uuids = {property_uuid};
  api::RelationalDagNode limit_node;
  limit_node.node_id = 3;
  limit_node.node_kind = api::RelationalDagNodeKind::kLimit;
  limit_node.input_node_ids = {2};
  limit_node.output_descriptor_ids = {2};
  limit_node.bound_expression_ids = {5};
  limit_node.semantic_variant_id = "limit.bound-count.v1";
  dag.nodes = {std::move(producer), std::move(sort), std::move(limit_node)};
  return dag;
}

api::TypedRelationalDag ProductionDocumentUnnestCountStarDag(
    const api::EngineRequestContext& context) {
  auto dag = ProductionDocumentUnnestSortLimitDag(context);
  dag.root_node_id = 2;
  dag.properties.clear();
  dag.expressions.resize(3);
  dag.nodes.resize(1);
  const auto count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  api::RelationalExpressionRecord aggregate;
  aggregate.expression_id = 4;
  aggregate.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  aggregate.result_descriptor_id = 4;
  if (count != nullptr) aggregate.function_uuid = count->function_uuid;
  dag.expressions.push_back(std::move(aggregate));
  dag.outputs.push_back({2, 2, 4, "item_count", 4, true, 0});
  api::RelationalDagNode aggregate_node;
  aggregate_node.node_id = 2;
  aggregate_node.node_kind = api::RelationalDagNodeKind::kAggregate;
  aggregate_node.input_node_ids = {1};
  aggregate_node.output_descriptor_ids = {4};
  aggregate_node.bound_expression_ids = {4};
  aggregate_node.semantic_variant_id = "aggregate.global-count-star.v1";
  dag.nodes.push_back(std::move(aggregate_node));
  return dag;
}

enum class ProductionUnnestCteMutation {
  none,
  semantic,
  output_schema,
  mga_context,
};

api::TypedRelationalDag ProductionDocumentUnnestCteDag(
    const api::EngineRequestContext& context,
    const bool shareable,
    const ProductionUnnestCteMutation mutation =
        ProductionUnnestCteMutation::none) {
  auto dag = ProductionDocumentUnnestSortLimitDag(context);
  dag.root_node_id = 2;
  dag.properties.clear();
  dag.expressions.resize(3);
  dag.nodes.resize(1);
  api::RelationalDagNode cte;
  cte.node_id = 2;
  cte.node_kind = api::RelationalDagNodeKind::kCte;
  cte.shareable = shareable;
  cte.input_node_ids = {1};
  cte.output_descriptor_ids = {
      mutation == ProductionUnnestCteMutation::output_schema ? 1U : 2U};
  cte.semantic_variant_id =
      mutation == ProductionUnnestCteMutation::semantic
          ? "cte.substituted.v1"
          : "cte.bound.v1";
  dag.nodes.push_back(std::move(cte));
  if (mutation == ProductionUnnestCteMutation::mga_context) {
    dag.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
  }
  return dag;
}

enum class ProductionUnnestRecursiveMutation {
  none,
  term_semantic,
  root_semantic,
  output_schema,
  mga_context,
};

api::TypedRelationalDag ProductionDocumentUnnestRecursiveCteDag(
    const api::EngineRequestContext& context,
    const ProductionUnnestRecursiveMutation mutation =
        ProductionUnnestRecursiveMutation::none,
    const std::string_view upper_bound = "5") {
  auto dag = ProductionDocumentUnnestCountStarDag(context);
  dag.root_node_id = 4;
  api::RelationalExpressionRecord bound;
  bound.expression_id = 5;
  bound.expression_kind = api::RelationalExpressionKind::kLiteral;
  bound.result_descriptor_id = 4;
  bound.literal_kind = api::RelationalLiteralKind::kNumeric;
  bound.literal_or_parameter_ref = std::string(upper_bound);
  dag.expressions.push_back(std::move(bound));

  api::RelationalDagNode term;
  term.node_id = 3;
  term.node_kind = api::RelationalDagNodeKind::kCte;
  term.output_descriptor_ids = {
      mutation == ProductionUnnestRecursiveMutation::output_schema ? 2U
                                                                   : 4U};
  term.semantic_variant_id =
      mutation == ProductionUnnestRecursiveMutation::term_semantic
          ? "cte.recursive-term.substituted.v1"
          : "cte.recursive-term-int64-increment.v1";
  dag.nodes.push_back(std::move(term));

  api::RelationalDagNode recursive;
  recursive.node_id = 4;
  recursive.node_kind = api::RelationalDagNodeKind::kRecursiveCte;
  recursive.input_node_ids = {2, 3};
  recursive.output_descriptor_ids = {4};
  recursive.bound_expression_ids = {5};
  recursive.semantic_variant_id =
      mutation == ProductionUnnestRecursiveMutation::root_semantic
          ? "cte.recursive-substituted.v1"
          : "cte.recursive-union-all-int64-increment.v1";
  dag.nodes.push_back(std::move(recursive));
  if (mutation == ProductionUnnestRecursiveMutation::mga_context) {
    dag.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
  }
  return dag;
}

enum class ProductionUnnestSetMutation {
  none,
  root_semantic,
  values_semantic,
  output_schema,
  input_order,
  orphan_node,
  root_output_lineage,
  multi_row_values,
  malformed_json,
  literal_type,
  descriptor_type,
  nullability,
  mga_context,
};

api::TypedRelationalDag ProductionDocumentUnnestSetDag(
    const api::EngineRequestContext& context,
    const ProductionUnnestSetMutation mutation =
        ProductionUnnestSetMutation::none) {
  auto dag = ProductionDocumentUnnestSortLimitDag(context);
  dag.root_node_id = 3;
  dag.properties.clear();
  dag.expressions.resize(3);
  dag.nodes.resize(1);

  api::RelationalExpressionRecord literal;
  literal.expression_id = 4;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 2;
  literal.literal_kind =
      mutation == ProductionUnnestSetMutation::literal_type
          ? api::RelationalLiteralKind::kString
          : api::RelationalLiteralKind::kDocument;
  literal.literal_or_parameter_ref =
      mutation == ProductionUnnestSetMutation::malformed_json ? "{" : "4";
  dag.expressions.push_back(std::move(literal));
  dag.values_rows = {{1, {4}}};
  if (mutation == ProductionUnnestSetMutation::multi_row_values) {
    api::RelationalExpressionRecord extra_literal;
    extra_literal.expression_id = 5;
    extra_literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    extra_literal.result_descriptor_id = 2;
    extra_literal.literal_kind = api::RelationalLiteralKind::kDocument;
    extra_literal.literal_or_parameter_ref = "5";
    dag.expressions.push_back(std::move(extra_literal));
    dag.values_rows.push_back({2, {5}});
  }
  dag.outputs.push_back({2, 2, 4, "item", 2, true, 0});

  api::RelationalDagNode values;
  values.node_id = 2;
  values.node_kind = api::RelationalDagNodeKind::kValues;
  values.output_descriptor_ids = {2};
  values.values_row_ids =
      mutation == ProductionUnnestSetMutation::multi_row_values
          ? std::vector<std::uint32_t>{1, 2}
          : std::vector<std::uint32_t>{1};
  values.semantic_variant_id =
      mutation == ProductionUnnestSetMutation::values_semantic
          ? "values.substituted.v1"
          : "values.literal-table.v1";
  dag.nodes.push_back(std::move(values));

  api::RelationalDagNode set;
  set.node_id = 3;
  set.node_kind = api::RelationalDagNodeKind::kSetOperation;
  set.input_node_ids =
      mutation == ProductionUnnestSetMutation::input_order
          ? std::vector<std::uint32_t>{2, 1}
          : std::vector<std::uint32_t>{1, 2};
  set.output_descriptor_ids = {
      mutation == ProductionUnnestSetMutation::output_schema ? 1U : 2U};
  set.semantic_variant_id =
      mutation == ProductionUnnestSetMutation::root_semantic
          ? "set-operation.union-distinct.v1"
          : "set-operation.union-all.v1";
  dag.nodes.push_back(std::move(set));

  if (mutation == ProductionUnnestSetMutation::orphan_node) {
    api::RelationalDagNode orphan;
    orphan.node_id = 4;
    orphan.node_kind = api::RelationalDagNodeKind::kCte;
    orphan.output_descriptor_ids = {2};
    orphan.semantic_variant_id = "cte.bound.v1";
    dag.nodes.push_back(std::move(orphan));
  }
  if (mutation == ProductionUnnestSetMutation::root_output_lineage) {
    dag.outputs.push_back({3, 3, 4, "item", 2, true, 0});
  }
  if (mutation == ProductionUnnestSetMutation::mga_context) {
    dag.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
  }
  if (mutation == ProductionUnnestSetMutation::descriptor_type) {
    dag.descriptors[1].type_uuid = dag.descriptors[3].type_uuid;
  }
  if (mutation == ProductionUnnestSetMutation::nullability) {
    dag.descriptors[1].nullability =
        api::RelationalNullability::kNonNull;
  }
  return dag;
}

enum class ProductionUnnestWindowMutation {
  none,
  semantic,
  function_uuid,
  missing_order_property,
  window_property,
  output_schema,
  input_node,
  mga_context,
};

api::TypedRelationalDag ProductionDocumentUnnestRowNumberDag(
    const api::EngineRequestContext& context,
    const ProductionUnnestWindowMutation mutation =
        ProductionUnnestWindowMutation::none) {
  auto dag = ProductionDocumentUnnestSortLimitDag(context);
  dag.root_node_id = 3;
  dag.expressions.resize(4);
  dag.nodes.resize(2);

  constexpr std::string_view kRowNumberFunctionUuid =
      "019de5fc-2400-7539-bcce-00eef3ae7220";
  const auto function_uuid =
      mutation == ProductionUnnestWindowMutation::function_uuid
          ? ProductionUuidText(platform::UuidKind::object)
          : std::string(kRowNumberFunctionUuid);
  api::RelationalExpressionRecord row_number;
  row_number.expression_id = 5;
  row_number.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  row_number.result_descriptor_id = 4;
  row_number.function_uuid = function_uuid;
  dag.expressions.push_back(std::move(row_number));
  dag.outputs.push_back({2, 3, 3, "item", 2, true, 0});
  dag.outputs.push_back({3, 3, 5, "row_number", 4, true, 1});

  const auto ordering_property_uuid =
      dag.properties.front().property_uuid;
  const auto window_property_uuid =
      ProductionUuidText(platform::UuidKind::object);
  api::RelationalPropertyRecord window_property;
  window_property.property_uuid = window_property_uuid;
  window_property.property_kind = api::RelationalPropertyKind::kWindow;
  window_property.origin_node_id = 3;
  if (mutation != ProductionUnnestWindowMutation::window_property) {
    window_property.dependency_property_uuids = {ordering_property_uuid};
  }
  window_property.window_frame_descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  dag.properties.push_back(std::move(window_property));

  api::RelationalDagNode window;
  window.node_id = 3;
  window.node_kind = api::RelationalDagNodeKind::kWindow;
  window.input_node_ids =
      mutation == ProductionUnnestWindowMutation::input_node
          ? std::vector<std::uint32_t>{1}
          : std::vector<std::uint32_t>{2};
  window.output_descriptor_ids = {2, 4};
  window.bound_expression_ids = {4, 5};
  window.semantic_variant_id =
      mutation == ProductionUnnestWindowMutation::semantic
          ? "window.rank.v1"
          : "window.row-number.v1";
  if (mutation != ProductionUnnestWindowMutation::missing_order_property) {
    window.required_property_uuids = {ordering_property_uuid};
  }
  window.delivered_property_uuids = {ordering_property_uuid,
                                     window_property_uuid};
  dag.nodes.push_back(std::move(window));

  api::RelationalWindowDefinitionRecord definition;
  definition.window_id = 1;
  definition.relation_node_id = 3;
  definition.ordering_terms = {
      {4, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}}};
  dag.window_definitions.push_back(std::move(definition));
  api::RelationalWindowInvocationRecord invocation;
  invocation.invocation_id = 1;
  invocation.relation_node_id = 3;
  invocation.function_expression_id = 5;
  invocation.window_definition_id = 1;
  invocation.function_abi_version = 1;
  invocation.builtin_id = "sb.window.row_number";
  invocation.function_uuid = function_uuid;
  invocation.result_descriptor_id = 4;
  invocation.output_name_utf8 = "row_number";
  dag.window_invocations.push_back(std::move(invocation));

  if (mutation == ProductionUnnestWindowMutation::output_schema) {
    dag.descriptors[3].nullability = api::RelationalNullability::kNullable;
  }
  if (mutation == ProductionUnnestWindowMutation::mga_context) {
    dag.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
  }
  return dag;
}

exec::CanonicalRecursiveCteWorkingRequest
ProductionRecursiveAggregateAnchorRequest() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d711";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019d0000-0000-7000-8000-00000000d712;"
      "nullability=non_null";
  api::EngineTypedValue anchor_value;
  anchor_value.descriptor = descriptor;
  anchor_value.encoded_value = "3";
  anchor_value.setState(api::EngineValueState::value);

  exec::CanonicalRecursiveCteWorkingRequest request;
  auto& dag = request.physical_dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid =
      "019f0730-0000-7000-8000-000000000303";
  dag.root_physical_node_id = 303;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0730-0000-7000-8000-000000000311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0730-0000-7000-8000-000000000312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0730-0000-7000-8000-000000000313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0730-0000-7000-8000-000000000314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0730-0000-7000-8000-000000000315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0730-0000-7000-8000-000000000316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0730-0000-7000-8000-000000000317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0730-0000-7000-8000-000000000318"},
  };
  dag.bound_sblr_tree_uuid = dag.admission_evidence[0].evidence_uuid;
  dag.catalog_epoch_uuid = dag.admission_evidence[1].evidence_uuid;
  dag.security_context_uuid = dag.admission_evidence[2].evidence_uuid;
  dag.capability_snapshot_uuid = dag.admission_evidence[4].evidence_uuid;
  dag.resource_snapshot_uuid = dag.admission_evidence[5].evidence_uuid;
  dag.statistics_snapshot_uuid = dag.admission_evidence[6].evidence_uuid;
  dag.route_snapshot_uuid = dag.admission_evidence[7].evidence_uuid;
  dag.local_transaction_id = 12;
  dag.statement_snapshot_id = 7;
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 4096;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;

  exec::PhysicalMgaStatementContext context;
  context.statement_uuid =
      "019f0730-0000-7000-8000-000000000321";
  context.owning_transaction_uuid =
      "019f0730-0000-7000-8000-000000000322";
  context.statement_snapshot_uuid = dag.admission_evidence[3].evidence_uuid;
  context.statement_metadata_snapshot_uuid =
      "019f0730-0000-7000-8000-000000000323";
  context.owning_local_transaction_id = 12;
  context.visible_committed_high_watermark = 7;
  context.oldest_active_transaction_id = 10;
  context.oldest_interesting_transaction_id = 8;
  context.oldest_snapshot_transaction_id = 8;
  context.retention_horizon_transaction_id = 8;
  context.active_excluded_local_transaction_ids = {10, 12};
  context.in_doubt_excluded_local_transaction_ids = {11};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 20;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  dag.mga_statement_context = context;

  dag.nodes = {
      {.physical_node_id = 300,
       .relational_node_id = 300,
       .node_kind = exec::PhysicalNodeKind::kScan,
       .implementation_id = "physical_document_path_scan_v1",
       .output_descriptor_ids = {300},
       .causal_counter_id = 3000},
      {.physical_node_id = 301,
       .relational_node_id = 301,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.count-star.v1",
       .input_physical_node_ids = {300},
       .output_descriptor_ids = {301},
       .causal_counter_id = 3001,
       .logical_semantic_variant_id =
           "aggregate.global-count-star.v1"},
      {.physical_node_id = 302,
       .relational_node_id = 302,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id =
           "cte.recursive-term.int64-increment.typed.v1",
       .output_descriptor_ids = {301},
       .causal_counter_id = 3002,
       .logical_semantic_variant_id =
           "cte.recursive-term-int64-increment.v1"},
      {.physical_node_id = 303,
       .relational_node_id = 303,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = "cte.recursive.working.typed.v1",
       .input_physical_node_ids = {301, 302},
       .output_descriptor_ids = {301},
       .causal_counter_id = 3003},
  };
  for (auto& node : dag.nodes) {
    node.selected_alternative_uuid =
        "019f0730-0000-7000-8000-000000000324";
    node.executor_capability_uuid =
        "019f0730-0000-7000-8000-000000000325";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0730-0000-7000-8000-000000000326";
    node.memory_bytes_required = 1;
    node.engine_capability_validated = true;
    node.mga_statement_context = context;
  }
  request.selected_physical_node_id = 303;
  request.anchor_batch = exec::MakeDescriptorBatch(
      {{"item_count", descriptor, false, 301}},
      {{{std::move(anchor_value)}}});
  request.recursive_step = [descriptor](
                               const exec::DescriptorBatch& working,
                               const std::size_t) {
    exec::DescriptorBatch next;
    next.columns = working.columns;
    for (const auto& row : working.rows) {
      const auto value = std::stoll(row.values.front().encoded_value);
      if (value >= 5) continue;
      api::EngineTypedValue incremented;
      incremented.descriptor = descriptor;
      incremented.encoded_value = std::to_string(value + 1);
      incremented.setState(api::EngineValueState::value);
      next.rows.push_back({{std::move(incremented)}});
    }
    return next;
  };
  request.maximum_iteration_count = 6;
  request.maximum_working_row_count = 1;
  request.maximum_result_row_count = 6;
  request.mga_authority.statement_context = context;
  request.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  request.mga_authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    return current;
  };
  return request;
}

bool ValidateProductionRecursiveAggregateAnchorAdmission() {
  bool passed = true;
  const auto refused = [&](auto mutation, const std::string_view detail) {
    auto request = ProductionRecursiveAggregateAnchorRequest();
    mutation(request);
    const auto result = exec::ExecuteCanonicalRecursiveCteWorking(request);
    return Require(!result.diagnostic.ok && !result.converged &&
                       result.output_batch.rows.empty() &&
                       result.executed_physical_node_id == 0,
                   detail);
  };

  const auto positive = exec::ExecuteCanonicalRecursiveCteWorking(
      ProductionRecursiveAggregateAnchorRequest());
  passed &= Require(
      positive.diagnostic.ok && positive.converged &&
          positive.output_batch.rows.size() == 3 &&
          positive.output_batch.rows[0].values[0].encoded_value == "3" &&
          positive.output_batch.rows[2].values[0].encoded_value == "5" &&
          positive.executed_physical_node_id == 303,
      "direct materialized COUNT(*) recursive anchor was not admitted exactly: " +
          positive.diagnostic.diagnostic_code + ":" +
          positive.diagnostic.detail);
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[1].node_kind =
            exec::PhysicalNodeKind::kScan;
      },
      "non-aggregate recursive anchor kind was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[1].implementation_id =
            "aggregate.substituted.v1";
      },
      "substituted recursive aggregate implementation was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[1].logical_semantic_variant_id =
            "aggregate.substituted.v1";
      },
      "substituted recursive aggregate semantic was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[1].input_physical_node_ids.clear();
      },
      "wrong recursive aggregate input arity was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[1].engine_capability_validated = false;
      },
      "unvalidated recursive aggregate capability was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[1].output_descriptor_ids.push_back(302);
      },
      "wide recursive aggregate output was admitted");
  passed &= refused(
      [](auto& request) {
        request.anchor_batch.columns[0].descriptor.canonical_type_name =
            "text";
        request.anchor_batch.rows[0].values[0].descriptor =
            request.anchor_batch.columns[0].descriptor;
      },
      "non-int64 recursive aggregate anchor was admitted");
  passed &= refused(
      [](auto& request) {
        request.anchor_batch.rows[0].values[0].setState(
            api::EngineValueState::sql_null);
      },
      "NULL recursive COUNT(*) anchor was admitted");
  passed &= refused(
      [](auto& request) {
        std::ranges::reverse(
            request.physical_dag.nodes[3].input_physical_node_ids);
      },
      "reordered recursive aggregate/term inputs were admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[2].input_physical_node_ids = {300};
      },
      "nonempty recursive term binding was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[2].implementation_id =
            "cte.recursive-term.substituted.v1";
      },
      "substituted recursive term implementation was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[2].logical_semantic_variant_id =
            "cte.recursive-term.substituted.v1";
      },
      "substituted recursive term semantic was admitted");
  passed &= refused(
      [](auto& request) {
        request.physical_dag.nodes[2].engine_capability_validated = false;
      },
      "unvalidated recursive term capability was admitted");
  return passed;
}

bool HasProductionRouteEvidence(const api::EngineApiResult& result) {
  return std::ranges::any_of(result.evidence, [](const auto& evidence) {
    return evidence.evidence_kind == "canonical.model_route" &&
           evidence.evidence_id ==
               "SBSQL_DOCUMENT_SOURCE_TO_SBLR_MODEL_SOURCE_TO_DOCUMENT_PATH_SCAN_TO_TYPED_BATCH_V1";
  });
}

bool HasProductionEvidence(const api::EngineApiResult& result,
                           const std::string_view kind,
                           const std::string_view id) {
  return std::ranges::any_of(result.evidence, [&](const auto& evidence) {
    return evidence.evidence_kind == kind && evidence.evidence_id == id;
  });
}

bool HasProductionDiagnostic(const api::EngineApiResult& result,
                             const std::string_view code) {
  return std::ranges::any_of(result.diagnostics, [&](const auto& diagnostic) {
    return diagnostic.code == code;
  });
}

bool ProductionCanonicalQueryExecuteRoute() {
  ProductionFixture fixture;
  if (!MakeProductionFixture(&fixture)) return false;

  api::EngineRequestContext writer;
  if (!BeginProductionTransaction(fixture, "rcp073-document-writer",
                                  &writer)) {
    return false;
  }
  if (!CreateProductionCollections(&fixture, writer)) return false;
  AddProductionAuthorization(&writer, "INSERT", fixture.collection_uuid);
  AddProductionAuthorization(&writer, "INSERT",
                             fixture.other_collection_uuid);
  if (!InsertProductionDocument(
          writer, fixture.collection_uuid,
          ProductionUuidText(platform::UuidKind::object),
          ProductionUuidText(platform::UuidKind::row), "document-one",
          "document-one") ||
      !InsertProductionDocument(
          writer, fixture.collection_uuid,
          ProductionUuidText(platform::UuidKind::object),
          ProductionUuidText(platform::UuidKind::row), "document-two",
          "document-two") ||
      !InsertProductionDocument(
          writer, fixture.other_collection_uuid,
          ProductionUuidText(platform::UuidKind::object),
          ProductionUuidText(platform::UuidKind::row), "document-other",
          "other-collection-document") ||
      !CommitProductionTransaction(writer)) {
    return false;
  }

  api::EngineRequestContext context;
  if (!BeginProductionTransaction(fixture, "rcp073-document-reader",
                                  &context)) {
    return false;
  }
  context.statement_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  if (!snapshot.ok) {
    return Require(false, "production route statement snapshot publish failed");
  }
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  context.catalog_epoch_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 77;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      {context.principal_uuid, "principal"});
  AddProductionAuthorization(&context, "SELECT", fixture.collection_uuid);
  AddProductionAuthorization(&context, "SELECT",
                             fixture.other_collection_uuid);
  context.optimizer_capability_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_resource_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_route_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_route_epoch = 78;
  context.optimizer_route_generation = 79;
  context.optimizer_memory_budget_bytes = 1024 * 1024;
  context.optimizer_maximum_candidate_count = 1024;
  context.optimizer_maximum_memo_groups = 1024;
  context.optimizer_maximum_search_steps = 4096;
  context.optimizer_maximum_planning_time_ns = 1'000'000'000;
  context.current_monotonic_ns = std::to_string(ProductionSeed());
  context.query_cancellation_requested = [] { return false; };

  const auto direct_find = [&context](
                               const api::MgaRelationStorageDescriptor& collection,
                               const std::string& path) {
    const auto column = std::ranges::find_if(
        collection.columns, [&](const auto& candidate) {
          return candidate.canonical_name_key == path;
        });
    api::EngineDocumentFindRequest request;
    request.context = context;
    request.target_object.uuid.canonical = collection.relation_uuid.canonical;
    request.expected_descriptor_uuid =
        collection.descriptor_uuid.canonical;
    request.expected_descriptor_generation =
        collection.descriptor_generation;
    request.exact_collection_fallback = true;
    request.maximum_rows = 16;
    request.maximum_cells = 16;
    request.maximum_memory_bytes = 1024 * 1024;
    request.maximum_scanned_row_versions = 16;
    request.maximum_decoded_bytes = 1024 * 1024;
    request.projected_paths = {path};
    if (column != collection.columns.end()) {
      const auto type_uuid =
          ProductionDescriptorField(column->value_descriptor, "type_uuid");
      if (!type_uuid.has_value()) return api::EngineDocumentFindResult{};
      auto runtime_descriptor = column->value_descriptor;
      runtime_descriptor.descriptor_kind = "scalar";
      runtime_descriptor.encoded_descriptor =
          "type_uuid=" + *type_uuid + ";nullability=" +
          (column->nullable ? "nullable" : "non_null");
      const auto append_optional_field = [&](const std::string_view key) {
        const auto value =
            ProductionDescriptorField(column->value_descriptor, key);
        if (value.has_value()) {
          runtime_descriptor.encoded_descriptor +=
              ";" + std::string(key) + "=" + *value;
        }
      };
      append_optional_field("collation_uuid");
      append_optional_field("timezone_profile_id");
      append_optional_field("width");
      append_optional_field("precision");
      append_optional_field("scale");
      request.projected_column_uuids = {column->column_uuid.canonical};
      request.projected_path_nullable = {column->nullable};
      request.descriptors = {std::move(runtime_descriptor)};
    }
    return api::EngineDocumentFind(request);
  };
  bool passed = true;
  const auto collection_a =
      direct_find(fixture.collection_descriptor, "payload");
  const auto collection_b =
      direct_find(fixture.other_collection_descriptor, "payload");
  passed &= Require(collection_a.ok && collection_a.typed_rows.size() == 2 &&
                        collection_b.ok &&
                        collection_b.typed_rows.size() == 1 &&
                        collection_b.typed_rows.front().values.front()
                                .value.encoded_value ==
                            "other-collection-document",
                    "exact fallback crossed its bound collection UUID");
  const auto direct_nullable_missing =
      direct_find(fixture.collection_descriptor, "payload_shadow");
  passed &= Require(
      direct_nullable_missing.ok &&
          direct_nullable_missing.typed_rows.size() == 2 &&
          std::ranges::all_of(direct_nullable_missing.typed_rows,
                              [](const auto& row) {
                                return row.values.size() == 1 &&
                                       row.values.front().value.state ==
                                           api::EngineValueState::missing;
                              }),
      "direct nullable missing projection was not normalized per column");
  const auto direct_non_nullable_missing =
      direct_find(fixture.collection_descriptor, "required_shadow");
  passed &= Require(
      !direct_non_nullable_missing.ok &&
          HasProductionDiagnostic(
              direct_non_nullable_missing,
              "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1"),
      "direct non-null missing projection did not fail closed exactly");
  if (std::getenv("SB_CES05_API_ONLY") != nullptr) {
    passed &= CommitProductionTransaction(context);
    return passed;
  }

  const auto envelope =
      ProductionDocumentEnvelope(context, fixture.collection_descriptor);
  const auto dispatched =
      sblr::DispatchSblrOperation({context, envelope, api::EngineApiRequest{}});
  if (!dispatched.api_result.ok && !dispatched.api_result.diagnostics.empty()) {
    const auto& diagnostic = dispatched.api_result.diagnostics.front();
    std::cerr << "QOW-CES05-DOCUMENT production route: " << diagnostic.code
              << ' ' << diagnostic.detail << '\n';
  }
  passed &= Require(dispatched.envelope_validated && dispatched.accepted &&
                        dispatched.dispatched_to_api &&
                        dispatched.logical_graph_populated &&
                        dispatched.optimizer_admitted &&
                        dispatched.optimizer_selected &&
                        dispatched.physical_dag_published &&
                        dispatched.physical_dag_executed &&
                        dispatched.canonical_result_published &&
                        dispatched.api_result.ok,
                    "normal query.execute document route did not complete");
  passed &= Require(dispatched.logical_node_count == 1 &&
                        dispatched.physical_node_count == 1 &&
                        dispatched.optimizer_admission_stage_count == 8 &&
                        dispatched.canonical_result_column_count == 1 &&
                        dispatched.canonical_result_row_count == 2 &&
                        !dispatched.selected_plan_uuid.empty() &&
                        !dispatched.canonical_result_bytes.empty(),
                    "normal query.execute route receipts drifted");
  passed &= Require(HasProductionRouteEvidence(dispatched.api_result),
                    "normal query.execute route evidence is absent");
  passed &= Require(
      HasProductionEvidence(dispatched.api_result,
                            "canonical.model_search_family",
                            "document.local.v1") &&
          HasProductionEvidence(dispatched.api_result,
                                "canonical.physical_abi", "2") &&
          HasProductionEvidence(dispatched.api_result,
                                "canonical.physical_dispatch",
                                "generic.selected-dag.v1") &&
          HasProductionEvidence(
              dispatched.api_result, "canonical.provider_generation",
              std::to_string(
                  fixture.collection_descriptor.descriptor_generation)) &&
          HasProductionEvidence(
              dispatched.api_result, "canonical.document_properties",
              "fixture_order|single_local_partition|document_uuid") &&
          HasProductionEvidence(dispatched.api_result,
                                "canonical.document_row_identity_count", "2"),
      "canonical search/physical/provider/property evidence drifted");
  std::vector<std::string> payloads;
  for (const auto& row : dispatched.api_result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == "payload") payloads.push_back(value.encoded_value);
    }
  }
  passed &= Require(payloads ==
                        std::vector<std::string>{"document-one",
                                                 "document-two"},
                    "normal query.execute route returned the wrong rows");

  const auto unnest = sblr::DispatchSblrOperation(
      {context, ProductionDocumentUnnestEnvelope(context),
       api::EngineApiRequest{}});
  if (!unnest.api_result.ok && !unnest.api_result.diagnostics.empty()) {
    const auto& diagnostic = unnest.api_result.diagnostics.front();
    std::cerr << "QOW-CES05-DOCUMENT unnest route: " << diagnostic.code
              << ' ' << diagnostic.detail << '\n';
  }
  passed &= Require(
      unnest.envelope_validated && unnest.accepted &&
          unnest.dispatched_to_api && unnest.logical_graph_populated &&
          unnest.optimizer_admitted && unnest.optimizer_selected &&
          unnest.physical_dag_published && unnest.physical_dag_executed &&
          unnest.canonical_result_published && unnest.api_result.ok &&
          unnest.logical_node_count == 1 && unnest.physical_node_count == 1 &&
          unnest.optimizer_admission_stage_count == 8 &&
          unnest.canonical_result_column_count == 1 &&
          unnest.canonical_result_row_count == 3,
      "object-free DOCUMENT_UNNEST did not complete the canonical spine");
  std::vector<std::string> unnested_items;
  for (const auto& row : unnest.api_result.result_shape.rows) {
    const auto item = std::ranges::find_if(row.fields, [](const auto& field) {
      return field.first == "item";
    });
    if (item != row.fields.end()) {
      unnested_items.push_back(item->second.encoded_value);
    }
  }
  passed &= Require(
      unnested_items == std::vector<std::string>{"3", "1", "2"},
      "DOCUMENT_UNNEST did not preserve wildcard source ordinal order");
  passed &= Require(
      HasProductionEvidence(
          unnest.api_result, "canonical.model_route",
          "SBSQL_DOCUMENT_UNNEST_TO_SBLR_MODEL_EXPAND_TO_DOCUMENT_PATH_SCAN_TO_TYPED_BATCH_V1") &&
          HasProductionEvidence(unnest.api_result,
                                "canonical.document_input",
                                "bound_expression_no_storage") &&
          HasProductionEvidence(
              unnest.api_result,
              "canonical.document_exact_collection_fallback", "false") &&
          HasProductionEvidence(unnest.api_result,
                                "canonical.document_row_identity_count", "3"),
      "DOCUMENT_UNNEST route, zero-storage, fallback, or identity evidence drifted");

  const auto composed_unnest = sblr::DispatchSblrOperation(
      {context, ProductionDocumentUnnestFilterProjectLimitEnvelope(context),
       api::EngineApiRequest{}});
  if (!composed_unnest.api_result.ok &&
      !composed_unnest.api_result.diagnostics.empty()) {
    const auto& diagnostic = composed_unnest.api_result.diagnostics.front();
    std::cerr << "QOW-CES05-DOCUMENT composed unnest route: "
              << diagnostic.code << ' ' << diagnostic.detail << '\n';
    for (const auto& envelope_diagnostic : composed_unnest.diagnostics) {
      std::cerr << "QOW-CES05-DOCUMENT composed envelope: "
                << envelope_diagnostic.code << ' '
                << envelope_diagnostic.message << '\n';
    }
  }
  std::vector<std::string> composed_items;
  for (const auto& row : composed_unnest.api_result.result_shape.rows) {
    const auto item = std::ranges::find_if(row.fields, [](const auto& field) {
      return field.first == "item";
    });
    if (item != row.fields.end()) {
      composed_items.push_back(item->second.encoded_value);
    }
  }
  passed &= Require(
      composed_unnest.envelope_validated && composed_unnest.accepted &&
          composed_unnest.dispatched_to_api &&
          composed_unnest.logical_graph_populated &&
          composed_unnest.optimizer_admitted &&
          composed_unnest.optimizer_selected &&
          composed_unnest.physical_dag_published &&
          composed_unnest.physical_dag_executed &&
          composed_unnest.canonical_result_published &&
          composed_unnest.api_result.ok &&
          composed_unnest.logical_node_count == 4 &&
          composed_unnest.physical_node_count == 4 &&
          composed_unnest.canonical_result_row_count == 2 &&
          composed_items == std::vector<std::string>{"3", "1"},
      "DOCUMENT_UNNEST FILTER+PROJECT+LIMIT did not execute and publish at the actual selected-DAG root");

  const auto sorted_unnest = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, ProductionDocumentUnnestSortLimitDag(context)});
  if (!sorted_unnest.api_result.ok &&
      !sorted_unnest.api_result.diagnostics.empty()) {
    const auto& diagnostic = sorted_unnest.api_result.diagnostics.front();
    std::cerr << "QOW-CES05-DOCUMENT sorted unnest route: "
              << diagnostic.code << ' ' << diagnostic.detail << '\n';
  }
  std::vector<std::string> sorted_items;
  for (const auto& row : sorted_unnest.api_result.result_shape.rows) {
    const auto item = std::ranges::find_if(row.fields, [](const auto& field) {
      return field.first == "item";
    });
    if (item != row.fields.end()) {
      sorted_items.push_back(item->second.encoded_value);
    }
  }
  passed &= Require(
      sorted_unnest.profile_matched && sorted_unnest.optimizer_admitted &&
          sorted_unnest.optimizer_selected &&
          sorted_unnest.physical_dag_published &&
          sorted_unnest.physical_dag_executed &&
          sorted_unnest.runtime_actuals_attached &&
          sorted_unnest.canonical_result_published &&
          sorted_unnest.api_result.ok &&
          sorted_unnest.physical_node_count == 3 &&
          sorted_unnest.canonical_result_row_count == 2 &&
          sorted_items == std::vector<std::string>{"3", "1"},
      "DOCUMENT_UNNEST expression SORT+LIMIT did not execute through one selected DAG");

  const auto count_dag = ProductionDocumentUnnestCountStarDag(context);
  const auto counted_unnest = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, count_dag});
  const auto replayed_count = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, count_dag});
  std::string count_value;
  if (counted_unnest.api_result.result_shape.rows.size() == 1) {
    const auto field = std::ranges::find_if(
        counted_unnest.api_result.result_shape.rows.front().fields,
        [](const auto& candidate) {
          return candidate.first == "item_count";
        });
    if (field != counted_unnest.api_result.result_shape.rows.front().fields.end()) {
      count_value = field->second.encoded_value;
    }
  }
  passed &= Require(
      counted_unnest.profile_matched && counted_unnest.optimizer_admitted &&
          counted_unnest.optimizer_selected &&
          counted_unnest.physical_dag_published &&
          counted_unnest.physical_dag_executed &&
          counted_unnest.runtime_actuals_attached &&
          counted_unnest.canonical_result_published &&
          counted_unnest.api_result.ok &&
          counted_unnest.physical_node_count == 2 &&
          counted_unnest.canonical_result_column_count == 1 &&
          counted_unnest.canonical_result_row_count == 1 &&
          count_value == "3" && replayed_count.api_result.ok &&
          replayed_count.canonical_result_bytes ==
              counted_unnest.canonical_result_bytes,
      "DOCUMENT_UNNEST global COUNT(*) did not publish one deterministic aggregate-root result");

  auto cancelled_count_context = context;
  cancelled_count_context.query_cancellation_requested = [] { return true; };
  const auto cancelled_count = sblr::ExecuteCanonicalCurrentHeapQuery(
      {cancelled_count_context,
       ProductionDocumentUnnestCountStarDag(cancelled_count_context)});
  passed &= Require(
      !cancelled_count.api_result.ok &&
          !cancelled_count.physical_dag_executed &&
          !cancelled_count.canonical_result_published,
      "cancelled DOCUMENT_UNNEST COUNT(*) partially executed or published");

  auto substituted_count_dag = count_dag;
  substituted_count_dag.descriptors.back().type_uuid =
      substituted_count_dag.descriptors.front().type_uuid;
  const auto substituted_count = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, std::move(substituted_count_dag)});
  passed &= Require(
      !substituted_count.api_result.ok &&
          !substituted_count.physical_dag_executed &&
          !substituted_count.canonical_result_published,
      "DOCUMENT_UNNEST COUNT(*) output descriptor substitution was admitted");

  const auto inline_cte_dag =
      ProductionDocumentUnnestCteDag(context, false);
  auto materialized_cte_dag = inline_cte_dag;
  materialized_cte_dag.nodes.back().shareable = true;
  const auto inline_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, inline_cte_dag});
  const auto replayed_inline_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, inline_cte_dag});
  const auto materialized_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, materialized_cte_dag});
  const auto replayed_materialized_cte =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {context, materialized_cte_dag});
  const auto cte_items = [](const auto& execution) {
    std::vector<std::string> items;
    for (const auto& row : execution.api_result.result_shape.rows) {
      const auto item = std::ranges::find_if(
          row.fields, [](const auto& field) {
            return field.first == "item";
          });
      if (item != row.fields.end()) {
        items.push_back(item->second.encoded_value);
      }
    }
    return items;
  };
  const auto cte_completed = [&](const auto& execution) {
    return execution.profile_matched && execution.optimizer_admitted &&
           execution.optimizer_selected &&
           execution.physical_dag_published &&
           execution.physical_dag_executed &&
           execution.runtime_actuals_attached &&
           execution.canonical_result_published && execution.api_result.ok &&
           execution.physical_node_count == 2 &&
           execution.canonical_result_column_count == 1 &&
           execution.canonical_result_row_count == 3 &&
           cte_items(execution) ==
               std::vector<std::string>{"3", "1", "2"};
  };
  if (!cte_completed(inline_cte) || !cte_completed(materialized_cte) ||
      !replayed_inline_cte.api_result.ok ||
      replayed_inline_cte.canonical_result_bytes !=
          inline_cte.canonical_result_bytes ||
      !replayed_materialized_cte.api_result.ok ||
      replayed_materialized_cte.canonical_result_bytes !=
          materialized_cte.canonical_result_bytes ||
      !HasProductionEvidence(
          inline_cte.api_result, "canonical.document_cte_implementation",
          "cte.bound.inline.typed.v1") ||
      !HasProductionEvidence(
          inline_cte.api_result, "canonical.document_cte_auxiliary_memory",
          "none") ||
      !HasProductionEvidence(
          materialized_cte.api_result,
          "canonical.document_cte_implementation",
          "cte.bound.materialize.typed.v1") ||
      !HasProductionEvidence(
          materialized_cte.api_result,
          "canonical.document_cte_auxiliary_memory",
          "runtime_input_batch")) {
    const auto print_cte = [](const std::string_view name,
                              const auto& execution) {
      std::cerr << "QOW-CES05-DOCUMENT " << name
                << " ok=" << execution.api_result.ok
                << " admitted=" << execution.optimizer_admitted
                << " selected=" << execution.optimizer_selected
                << " published=" << execution.physical_dag_published
                << " executed=" << execution.physical_dag_executed
                << " result=" << execution.canonical_result_published
                << " nodes=" << execution.physical_node_count
                << " rows=" << execution.canonical_result_row_count
                << " plan=" << execution.selected_plan_uuid << '\n';
      if (!execution.api_result.diagnostics.empty()) {
        std::cerr << "QOW-CES05-DOCUMENT " << name << " diagnostic: "
                  << execution.api_result.diagnostics.front().code << ' '
                  << execution.api_result.diagnostics.front().detail << '\n';
      }
    };
    print_cte("inline CTE", inline_cte);
    print_cte("materialized CTE", materialized_cte);
  }
  passed &= Require(
      cte_completed(inline_cte) && cte_completed(materialized_cte) &&
          replayed_inline_cte.api_result.ok &&
          replayed_inline_cte.canonical_result_bytes ==
              inline_cte.canonical_result_bytes &&
          replayed_materialized_cte.api_result.ok &&
          replayed_materialized_cte.canonical_result_bytes ==
              materialized_cte.canonical_result_bytes &&
          HasProductionEvidence(
              inline_cte.api_result,
              "canonical.document_cte_implementation",
              "cte.bound.inline.typed.v1") &&
          HasProductionEvidence(
              inline_cte.api_result,
              "canonical.document_cte_auxiliary_memory", "none") &&
          HasProductionEvidence(
              materialized_cte.api_result,
              "canonical.document_cte_implementation",
              "cte.bound.materialize.typed.v1") &&
          HasProductionEvidence(
              materialized_cte.api_result,
              "canonical.document_cte_auxiliary_memory",
              "runtime_input_batch"),
      "DOCUMENT_UNNEST inline/materialized nonrecursive CTE receipts or actual-root publication drifted");

  constexpr std::array<ProductionUnnestCteMutation, 3> kCteMutations{{
      ProductionUnnestCteMutation::semantic,
      ProductionUnnestCteMutation::output_schema,
      ProductionUnnestCteMutation::mga_context,
  }};
  for (const auto mutation : kCteMutations) {
    const auto malformed_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
        {context, ProductionDocumentUnnestCteDag(context, false, mutation)});
    passed &= Require(
        !malformed_cte.api_result.ok &&
            !malformed_cte.physical_dag_executed &&
            !malformed_cte.canonical_result_published,
        "substituted DOCUMENT_UNNEST CTE semantic/schema/MGA context was admitted");
  }

  auto cancelled_cte_context = context;
  cancelled_cte_context.query_cancellation_requested = [] { return true; };
  const auto cancelled_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {cancelled_cte_context,
       ProductionDocumentUnnestCteDag(cancelled_cte_context, true)});
  passed &= Require(
      !cancelled_cte.api_result.ok && !cancelled_cte.physical_dag_executed &&
          !cancelled_cte.canonical_result_published,
      "cancelled DOCUMENT_UNNEST materialized CTE partially executed or published");

  const auto recursive_cte_dag =
      ProductionDocumentUnnestRecursiveCteDag(context);
  const auto recursive_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, recursive_cte_dag});
  const auto replayed_recursive_cte =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {context, recursive_cte_dag});
  std::vector<std::string> recursive_values;
  for (const auto& row : recursive_cte.api_result.result_shape.rows) {
    const auto value = std::ranges::find_if(
        row.fields, [](const auto& field) {
          return field.first == "item_count";
        });
    if (value != row.fields.end()) {
      recursive_values.push_back(value->second.encoded_value);
    }
  }
  if (!recursive_cte.api_result.ok ||
      recursive_cte.physical_node_count != 4 ||
      recursive_values != std::vector<std::string>{"3", "4", "5"}) {
    std::cerr << "QOW-CES05-DOCUMENT recursive CTE ok="
              << recursive_cte.api_result.ok
              << " admitted=" << recursive_cte.optimizer_admitted
              << " selected=" << recursive_cte.optimizer_selected
              << " published=" << recursive_cte.physical_dag_published
              << " executed=" << recursive_cte.physical_dag_executed
              << " result=" << recursive_cte.canonical_result_published
              << " nodes=" << recursive_cte.physical_node_count
              << " rows=" << recursive_cte.canonical_result_row_count
              << " values=";
    for (const auto& value : recursive_values) std::cerr << value << ',';
    std::cerr << '\n';
    if (!recursive_cte.api_result.diagnostics.empty()) {
      std::cerr << "QOW-CES05-DOCUMENT recursive CTE diagnostic: "
                << recursive_cte.api_result.diagnostics.front().code << ' '
                << recursive_cte.api_result.diagnostics.front().detail
                << '\n';
    }
  }
  passed &= Require(
      recursive_cte.profile_matched && recursive_cte.optimizer_admitted &&
          recursive_cte.optimizer_selected &&
          recursive_cte.physical_dag_published &&
          recursive_cte.physical_dag_executed &&
          recursive_cte.runtime_actuals_attached &&
          recursive_cte.canonical_result_published &&
          recursive_cte.api_result.ok &&
          recursive_cte.physical_node_count == 4 &&
          recursive_cte.canonical_result_column_count == 1 &&
          recursive_cte.canonical_result_row_count == 3 &&
          recursive_values == std::vector<std::string>{"3", "4", "5"} &&
          replayed_recursive_cte.api_result.ok &&
          replayed_recursive_cte.canonical_result_bytes ==
              recursive_cte.canonical_result_bytes &&
          HasProductionEvidence(
              recursive_cte.api_result,
              "canonical.document_recursive_cte_implementation",
              "cte.recursive.union-all.typed.v1") &&
          HasProductionEvidence(
              recursive_cte.api_result,
              "canonical.document_recursive_cte_bound", "5") &&
          HasProductionEvidence(
              recursive_cte.api_result,
              "canonical.document_recursive_cte_work_bound", "12"),
      "DOCUMENT_UNNEST recursive CTE did not execute its exact bounded 4-node selected DAG and publish once at the recursive root");

  constexpr std::array<ProductionUnnestRecursiveMutation, 4>
      kRecursiveMutations{{
          ProductionUnnestRecursiveMutation::term_semantic,
          ProductionUnnestRecursiveMutation::root_semantic,
          ProductionUnnestRecursiveMutation::output_schema,
          ProductionUnnestRecursiveMutation::mga_context,
      }};
  for (const auto mutation : kRecursiveMutations) {
    const auto malformed_recursive =
        sblr::ExecuteCanonicalCurrentHeapQuery(
            {context,
             ProductionDocumentUnnestRecursiveCteDag(context, mutation)});
    passed &= Require(
        !malformed_recursive.api_result.ok &&
            !malformed_recursive.physical_dag_executed &&
            !malformed_recursive.canonical_result_published,
        "substituted DOCUMENT_UNNEST recursive CTE term/root/schema/MGA context was admitted");
  }

  auto cancelled_recursive_context = context;
  cancelled_recursive_context.query_cancellation_requested = [] {
    return true;
  };
  const auto cancelled_recursive = sblr::ExecuteCanonicalCurrentHeapQuery(
      {cancelled_recursive_context,
       ProductionDocumentUnnestRecursiveCteDag(
           cancelled_recursive_context)});
  passed &= Require(
      !cancelled_recursive.api_result.ok &&
          !cancelled_recursive.physical_dag_executed &&
          !cancelled_recursive.canonical_result_published,
      "cancelled DOCUMENT_UNNEST recursive CTE partially executed or published");

  auto bounded_recursive_context = context;
  bounded_recursive_context.optimizer_maximum_candidate_count = 2;
  const auto bounded_recursive = sblr::ExecuteCanonicalCurrentHeapQuery(
      {bounded_recursive_context,
       ProductionDocumentUnnestRecursiveCteDag(
           bounded_recursive_context)});
  passed &= Require(
      !bounded_recursive.api_result.ok &&
          !bounded_recursive.physical_dag_executed &&
          !bounded_recursive.canonical_result_published,
      "DOCUMENT_UNNEST recursive CTE exceeded its admitted row/work bound");

  const auto set_dag = ProductionDocumentUnnestSetDag(context);
  const auto set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_dag});
  const auto replayed_set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_dag});
  const auto set_items = cte_items(set_union);
  if (!set_union.api_result.ok || set_union.physical_node_count != 3 ||
      set_items != std::vector<std::string>{"3", "1", "2", "4"}) {
    std::cerr << "QOW-CES05-DOCUMENT set UNION ALL ok="
              << set_union.api_result.ok
              << " admitted=" << set_union.optimizer_admitted
              << " selected=" << set_union.optimizer_selected
              << " published=" << set_union.physical_dag_published
              << " executed=" << set_union.physical_dag_executed
              << " result=" << set_union.canonical_result_published
              << " nodes=" << set_union.physical_node_count
              << " rows=" << set_union.canonical_result_row_count
              << " values=";
    for (const auto& value : set_items) std::cerr << value << ',';
    std::cerr << '\n';
    if (!set_union.api_result.diagnostics.empty()) {
      std::cerr << "QOW-CES05-DOCUMENT set UNION ALL diagnostic: "
                << set_union.api_result.diagnostics.front().code << ' '
                << set_union.api_result.diagnostics.front().detail << '\n';
    }
  }
  passed &= Require(
      set_union.profile_matched && set_union.optimizer_admitted &&
          set_union.optimizer_selected &&
          set_union.physical_dag_published &&
          set_union.physical_dag_executed &&
          set_union.runtime_actuals_attached &&
          set_union.canonical_result_published && set_union.api_result.ok &&
          set_union.physical_node_count == 3 &&
          set_union.canonical_result_column_count == 1 &&
          set_union.canonical_result_row_count == 4 &&
          set_items == std::vector<std::string>{"3", "1", "2", "4"} &&
          replayed_set_union.api_result.ok &&
          replayed_set_union.canonical_result_bytes ==
              set_union.canonical_result_bytes &&
          HasProductionEvidence(
              set_union.api_result,
              "canonical.document_set_implementation",
              "setop.union-all.ordinal.typed.v1") &&
          HasProductionEvidence(
              set_union.api_result, "canonical.document_set_semantics",
              "union-all.ordinal.left-then-right.bag.v1"),
      "DOCUMENT_UNNEST UNION ALL did not execute the exact 3-node selected DAG with deterministic bag/order semantics");

  constexpr std::array<ProductionUnnestSetMutation, 12> kSetMutations{{
      ProductionUnnestSetMutation::root_semantic,
      ProductionUnnestSetMutation::values_semantic,
      ProductionUnnestSetMutation::output_schema,
      ProductionUnnestSetMutation::input_order,
      ProductionUnnestSetMutation::orphan_node,
      ProductionUnnestSetMutation::root_output_lineage,
      ProductionUnnestSetMutation::multi_row_values,
      ProductionUnnestSetMutation::malformed_json,
      ProductionUnnestSetMutation::literal_type,
      ProductionUnnestSetMutation::descriptor_type,
      ProductionUnnestSetMutation::nullability,
      ProductionUnnestSetMutation::mga_context,
  }};
  for (const auto mutation : kSetMutations) {
    const auto malformed_set = sblr::ExecuteCanonicalCurrentHeapQuery(
        {context, ProductionDocumentUnnestSetDag(context, mutation)});
    passed &= Require(
        !malformed_set.api_result.ok &&
            !malformed_set.physical_dag_executed &&
            !malformed_set.canonical_result_published,
        "substituted DOCUMENT_UNNEST UNION ALL semantic/schema/order/lineage/cardinality/MGA shape was admitted");
  }

  auto cancelled_set_context = context;
  cancelled_set_context.query_cancellation_requested = [] { return true; };
  const auto cancelled_set = sblr::ExecuteCanonicalCurrentHeapQuery(
      {cancelled_set_context,
       ProductionDocumentUnnestSetDag(cancelled_set_context)});
  passed &= Require(
      !cancelled_set.api_result.ok &&
          !cancelled_set.physical_dag_executed &&
          !cancelled_set.canonical_result_published,
      "cancelled DOCUMENT_UNNEST UNION ALL partially executed or published");

  auto bounded_set_context = context;
  bounded_set_context.optimizer_maximum_candidate_count = 3;
  const auto bounded_set = sblr::ExecuteCanonicalCurrentHeapQuery(
      {bounded_set_context,
       ProductionDocumentUnnestSetDag(bounded_set_context)});
  passed &= Require(
      !bounded_set.api_result.ok && !bounded_set.physical_dag_executed &&
          !bounded_set.canonical_result_published,
      "DOCUMENT_UNNEST UNION ALL exceeded its split source/set row bound");

  const auto row_number_dag =
      ProductionDocumentUnnestRowNumberDag(context);
  const auto row_number_window = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, row_number_dag});
  const auto replayed_row_number_window =
      sblr::ExecuteCanonicalCurrentHeapQuery({context, row_number_dag});
  std::vector<std::string> window_items;
  std::vector<std::string> row_numbers;
  for (const auto& row : row_number_window.api_result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == "item") {
        window_items.push_back(field.second.encoded_value);
      } else if (field.first == "row_number") {
        row_numbers.push_back(field.second.encoded_value);
      }
    }
  }
  if (!row_number_window.api_result.ok ||
      row_number_window.physical_node_count != 3 ||
      window_items != std::vector<std::string>{"3", "1", "2"} ||
      row_numbers != std::vector<std::string>{"1", "2", "3"}) {
    std::cerr << "QOW-CES05-DOCUMENT ROW_NUMBER ok="
              << row_number_window.api_result.ok
              << " admitted=" << row_number_window.optimizer_admitted
              << " selected=" << row_number_window.optimizer_selected
              << " published=" << row_number_window.physical_dag_published
              << " executed=" << row_number_window.physical_dag_executed
              << " result=" << row_number_window.canonical_result_published
              << " nodes=" << row_number_window.physical_node_count
              << " rows=" << row_number_window.canonical_result_row_count
              << '\n';
    if (!row_number_window.api_result.diagnostics.empty()) {
      std::cerr << "QOW-CES05-DOCUMENT ROW_NUMBER diagnostic: "
                << row_number_window.api_result.diagnostics.front().code << ' '
                << row_number_window.api_result.diagnostics.front().detail
                << '\n';
    }
  }
  passed &= Require(
      row_number_window.profile_matched &&
          row_number_window.optimizer_admitted &&
          row_number_window.optimizer_selected &&
          row_number_window.physical_dag_published &&
          row_number_window.physical_dag_executed &&
          row_number_window.runtime_actuals_attached &&
          row_number_window.canonical_result_published &&
          row_number_window.api_result.ok &&
          row_number_window.physical_node_count == 3 &&
          row_number_window.canonical_result_column_count == 2 &&
          row_number_window.canonical_result_row_count == 3 &&
          window_items == std::vector<std::string>{"3", "1", "2"} &&
          row_numbers == std::vector<std::string>{"1", "2", "3"} &&
          replayed_row_number_window.api_result.ok &&
          replayed_row_number_window.canonical_result_bytes ==
              row_number_window.canonical_result_bytes &&
          HasProductionEvidence(
              row_number_window.api_result,
              "canonical.document_window_implementation",
              "window.row-number.v1") &&
          HasProductionEvidence(
              row_number_window.api_result,
              "canonical.document_window_root", "selected-dag-root.v1"),
      "DOCUMENT_UNNEST SORT to ROW_NUMBER did not execute and publish its deterministic window root");

  constexpr std::array<ProductionUnnestWindowMutation, 7>
      kWindowMutations{{
          ProductionUnnestWindowMutation::semantic,
          ProductionUnnestWindowMutation::function_uuid,
          ProductionUnnestWindowMutation::missing_order_property,
          ProductionUnnestWindowMutation::window_property,
          ProductionUnnestWindowMutation::output_schema,
          ProductionUnnestWindowMutation::input_node,
          ProductionUnnestWindowMutation::mga_context,
      }};
  for (const auto mutation : kWindowMutations) {
    const auto malformed_window = sblr::ExecuteCanonicalCurrentHeapQuery(
        {context,
         ProductionDocumentUnnestRowNumberDag(context, mutation)});
    passed &= Require(
        !malformed_window.api_result.ok &&
            !malformed_window.physical_dag_executed &&
            !malformed_window.canonical_result_published,
        "substituted DOCUMENT_UNNEST ROW_NUMBER semantic/function/property/schema/input/MGA shape was admitted");
  }

  auto cancelled_window_context = context;
  cancelled_window_context.query_cancellation_requested = [] {
    return true;
  };
  const auto cancelled_window = sblr::ExecuteCanonicalCurrentHeapQuery(
      {cancelled_window_context,
       ProductionDocumentUnnestRowNumberDag(cancelled_window_context)});
  passed &= Require(
      !cancelled_window.api_result.ok &&
          !cancelled_window.physical_dag_executed &&
          !cancelled_window.canonical_result_published,
      "cancelled DOCUMENT_UNNEST ROW_NUMBER partially executed or published");

  auto bounded_window_context = context;
  bounded_window_context.optimizer_maximum_candidate_count = 2;
  const auto bounded_window = sblr::ExecuteCanonicalCurrentHeapQuery(
      {bounded_window_context,
       ProductionDocumentUnnestRowNumberDag(bounded_window_context)});
  passed &= Require(
      !bounded_window.api_result.ok &&
          !bounded_window.physical_dag_executed &&
          !bounded_window.canonical_result_published,
      "DOCUMENT_UNNEST ROW_NUMBER exceeded its admitted row bound");

  constexpr std::array<ProductionUnnestCompositionMutation, 3>
      kCompositionMutations{{
          ProductionUnnestCompositionMutation::disconnected_limit,
          ProductionUnnestCompositionMutation::orphan_consumer,
          ProductionUnnestCompositionMutation::substituted_producer,
      }};
  for (const auto mutation : kCompositionMutations) {
    const auto malformed = sblr::DispatchSblrOperation(
        {context,
         ProductionDocumentUnnestFilterProjectLimitEnvelope(context, mutation),
         api::EngineApiRequest{}});
    passed &= Require(
        !malformed.api_result.ok && !malformed.physical_dag_executed &&
            !malformed.canonical_result_published,
        "disconnected, orphaned, or substituted composed producer escaped fail-closed validation");
  }

  auto cancelled_composition_context = context;
  cancelled_composition_context.query_cancellation_requested = [] {
    return true;
  };
  const auto cancelled_composition = sblr::DispatchSblrOperation(
      {cancelled_composition_context,
       ProductionDocumentUnnestFilterProjectLimitEnvelope(
           cancelled_composition_context),
       api::EngineApiRequest{}});
  passed &= Require(
      !cancelled_composition.api_result.ok &&
          !cancelled_composition.physical_dag_executed &&
          !cancelled_composition.canonical_result_published,
      "cancelled DOCUMENT_UNNEST composition partially executed or published");

  const auto empty_unnest = sblr::DispatchSblrOperation(
      {context, ProductionDocumentUnnestEnvelope(context, R"({"items":[]})"),
       api::EngineApiRequest{}});
  passed &= Require(empty_unnest.api_result.ok &&
                        empty_unnest.canonical_result_published &&
                        empty_unnest.canonical_result_row_count == 0,
                    "empty DOCUMENT_UNNEST boundary was not published exactly");

  const auto invalid_path_unnest = sblr::DispatchSblrOperation(
      {context,
       ProductionDocumentUnnestEnvelope(context, R"({"items":[1]})",
                                        "$.items"),
       api::EngineApiRequest{}});
  passed &= Require(
      !invalid_path_unnest.api_result.ok &&
          !invalid_path_unnest.optimizer_admitted &&
          !invalid_path_unnest.physical_dag_executed &&
          !invalid_path_unnest.canonical_result_published &&
          HasProductionDiagnostic(invalid_path_unnest.api_result,
                                  "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
      "invalid DOCUMENT_UNNEST wildcard performed provider access");

  constexpr std::array<ProductionUnnestMutation, 15> kUnnestMutations{{
      ProductionUnnestMutation::operator_case,
      ProductionUnnestMutation::function_uuid,
      ProductionUnnestMutation::source_semantic,
      ProductionUnnestMutation::wrong_node_kind,
      ProductionUnnestMutation::input_edge,
      ProductionUnnestMutation::object_uuid,
      ProductionUnnestMutation::extra_bound_root,
      ProductionUnnestMutation::missing_bound_root,
      ProductionUnnestMutation::reversed_children,
      ProductionUnnestMutation::duplicate_child,
      ProductionUnnestMutation::missing_child,
      ProductionUnnestMutation::wrong_path_kind,
      ProductionUnnestMutation::output_type,
      ProductionUnnestMutation::orphan_expression,
      ProductionUnnestMutation::output_expression,
  }};
  for (const auto mutation : kUnnestMutations) {
    const auto malformed = sblr::DispatchSblrOperation(
        {context,
         ProductionDocumentUnnestEnvelope(
             context, R"({"items":[1]})", "$.items[*]", mutation),
         api::EngineApiRequest{}});
    passed &= Require(
        !malformed.api_result.ok && !malformed.optimizer_admitted &&
            !malformed.physical_dag_executed &&
            !malformed.canonical_result_published &&
            HasProductionDiagnostic(malformed.api_result,
                                    "SBLR.PLAN_TREE.INVALID_HANDLE"),
        "malformed functionless DOCUMENT_UNNEST shape escaped typed-DAG validation");
  }

  auto cancelled_unnest_context = context;
  cancelled_unnest_context.query_cancellation_requested = [] { return true; };
  const auto cancelled_unnest = sblr::DispatchSblrOperation(
      {cancelled_unnest_context,
       ProductionDocumentUnnestEnvelope(cancelled_unnest_context),
       api::EngineApiRequest{}});
  passed &= Require(
      !cancelled_unnest.api_result.ok &&
          !cancelled_unnest.physical_dag_executed &&
          !cancelled_unnest.canonical_result_published &&
          (HasProductionDiagnostic(
               cancelled_unnest.api_result,
               "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1") ||
           HasProductionDiagnostic(cancelled_unnest.api_result,
                                   "SB_MODEL_EXECUTION_CANCELLED_V1")),
      "cancelled DOCUMENT_UNNEST did not fail before input consumption");

  auto bounded_unnest_context = context;
  bounded_unnest_context.optimizer_memory_budget_bytes = 1024;
  const auto bounded_unnest = sblr::DispatchSblrOperation(
      {bounded_unnest_context,
       ProductionDocumentUnnestEnvelope(bounded_unnest_context),
       api::EngineApiRequest{}});
  passed &= Require(
      !bounded_unnest.api_result.ok &&
          !bounded_unnest.canonical_result_published &&
          HasProductionDiagnostic(bounded_unnest.api_result,
                                  "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"),
      "DOCUMENT_UNNEST combined canonical-input/expansion/batch peak escaped its bound");

  const auto nullable_missing = sblr::DispatchSblrOperation(
      {context,
       ProductionDocumentEnvelope(context, fixture.collection_descriptor,
                                  "payload_shadow"),
       api::EngineApiRequest{}});
  bool nullable_rows = nullable_missing.api_result.result_shape.rows.size() == 2;
  for (const auto& row : nullable_missing.api_result.result_shape.rows) {
    const auto value = std::ranges::find_if(
        row.fields, [](const auto& field) {
          return field.first == "payload_shadow";
        });
    nullable_rows = nullable_rows && value != row.fields.end() &&
                    value->second.state == api::EngineValueState::sql_null;
  }
  passed &= Require(nullable_missing.api_result.ok &&
                        nullable_missing.canonical_result_published &&
                        nullable_missing.canonical_result_row_count == 2 &&
                        nullable_rows,
                    "nullable missing document projection was not published as SQL NULL");

  const auto non_nullable_missing = sblr::DispatchSblrOperation(
      {context,
       ProductionDocumentEnvelope(context, fixture.collection_descriptor,
                                  "required_shadow"),
       api::EngineApiRequest{}});
  passed &= Require(!non_nullable_missing.api_result.ok &&
                        !non_nullable_missing.canonical_result_published &&
                        HasProductionDiagnostic(
                            non_nullable_missing.api_result,
                            "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1"),
                    "non-null missing document projection did not fail closed exactly");

  const auto expect_descriptor_substitution_refusal =
      [&](auto mutation, const std::string_view detail) {
        auto substituted = fixture.collection_descriptor;
        mutation(substituted.columns.front());
        const auto refused = sblr::DispatchSblrOperation(
            {context, ProductionDocumentEnvelope(context, substituted),
             api::EngineApiRequest{}});
        return Require(
            !refused.api_result.ok &&
                !refused.canonical_result_published &&
                HasProductionDiagnostic(refused.api_result,
                                        "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"),
            detail);
      };
  passed &= expect_descriptor_substitution_refusal(
      [](auto& column) {
        column.column_uuid.canonical =
            ProductionUuidText(platform::UuidKind::object);
      },
      "persisted document column UUID substitution was admitted");
  passed &= expect_descriptor_substitution_refusal(
      [](auto& column) {
        column.value_descriptor.descriptor_uuid.canonical =
            ProductionUuidText(platform::UuidKind::object);
      },
      "persisted document descriptor UUID substitution was admitted");
  passed &= expect_descriptor_substitution_refusal(
      [](auto& column) {
        const auto prior = ProductionDescriptorField(
            column.value_descriptor.encoded_descriptor, "type_uuid");
        const auto replacement =
            ProductionUuidText(platform::UuidKind::object);
        column.value_descriptor.encoded_descriptor.replace(
            column.value_descriptor.encoded_descriptor.find(prior),
            prior.size(), replacement);
      },
      "persisted document type UUID substitution was admitted");
  passed &= expect_descriptor_substitution_refusal(
      [](auto& column) { column.nullable = !column.nullable; },
      "persisted document nullability substitution was admitted");
  passed &= expect_descriptor_substitution_refusal(
      [](auto& column) {
        column.value_descriptor.encoded_descriptor += ";width=1";
      },
      "persisted document optional descriptor substitution was admitted");

  auto denied_context = context;
  std::erase_if(denied_context.authorization_context.grants,
                [&](const auto& grant) {
                  return grant.right == "SELECT" &&
                         grant.target_uuid.canonical ==
                             fixture.collection_uuid;
                });
  const auto denied = sblr::DispatchSblrOperation(
      {denied_context,
       ProductionDocumentEnvelope(denied_context,
                                  fixture.collection_descriptor),
       api::EngineApiRequest{}});
  bool redacted = true;
  for (const auto& diagnostic : denied.api_result.diagnostics) {
    redacted = redacted &&
               diagnostic.detail.find(fixture.collection_uuid) ==
                   std::string::npos;
  }
  for (const auto& evidence : denied.api_result.evidence) {
    redacted = redacted &&
               evidence.evidence_id.find(fixture.collection_uuid) ==
                   std::string::npos;
  }
  passed &= Require(
      !denied.api_result.ok && !denied.canonical_result_published &&
          HasProductionDiagnostic(denied.api_result,
                                  "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1") &&
          redacted,
      "object-exact SELECT denial was not fail-closed and redacted");
  passed &= CommitProductionTransaction(context);
  return passed;
}
#endif

bool CatalogIdentity() {
  bool passed = Require(kVectors.size() == 12, "immutable vector count drifted");
  for (std::size_t index = 0; index < kVectors.size(); ++index) {
    const auto& vector = kVectors[index];
    std::string case_token = vector.case_class;
    for (auto& ch : case_token) {
      ch = ch == '_' ? '-' : static_cast<char>(
                              std::toupper(static_cast<unsigned char>(ch)));
    }
    const auto normal_setup = index != 4;
    const auto normal_descriptor = index != 6;
    const auto normal_security = index != 7;
    const auto normal_mga = index != 8;
    const auto normal_resource = index != 9;
    const auto special_cleanup = index == 3 || index == 4;
    const auto success = index == 0 || index == 1 || index == 10 ||
                         index == 11;
    const auto exact_order = index == 0 || index == 10 || index == 11;
    const auto expected_mutation =
        index == 5   ? "advance_catalog_generation"
        : index == 6 ? "swap_output_descriptor"
        : index == 7 ? "remove_object_disclosure"
        : index == 8 ? "swap_statement_context"
        : index == 9 ? "reduce_memory_budget"
                     : "none";
    const auto expected_fault =
        index == 3
            ? "cancellation_after_admission"
            : (index == 4 ? "named_precondition_or_provider_failure"
                          : "none");
    const auto generation_is = [](const FixtureGenerations& generations,
                                  const std::uint64_t catalog,
                                  const std::uint64_t descriptor) {
      return generations.catalog == catalog &&
             generations.descriptor == descriptor &&
             generations.security == 7 && generations.policy == 7 &&
             generations.provider == 7 && generations.route == 7 &&
             generations.resource == 7;
    };

    passed &= Require(std::string_view(vector.vector_id).ends_with(
                          (index + 1 < 10 ? "0" + std::to_string(index + 1) + "-V1"
                                          : std::to_string(index + 1) + "-V1")),
                      "immutable vector ordering drifted");
    passed &= Require(
        vector.family_id == std::string_view("document") &&
            vector.source_class == std::string_view("family") &&
            vector.subject_id == std::string_view("document") &&
            vector.fixture_id ==
                "FIX-FAMILY-DOCUMENT-" + case_token + "-V1" &&
            vector.statement_id ==
                "STMT-FAMILY-DOCUMENT-" + case_token + "-V1" &&
            vector.input_profile_id == "INPUT-" + case_token + "-V1" &&
            vector.qualified_object_reference ==
                std::string_view("app.document_fixture") &&
            FixtureObjectUuid(vector) == Uuid(1),
        "immutable fixture/vector/name binding drifted");
    passed &= Require(
        vector.typed_schema[0].field_id == std::string_view("row_uuid") &&
            vector.typed_schema[0].type == std::string_view("UUID") &&
            !vector.typed_schema[0].nullable &&
            vector.typed_schema[1].field_id == std::string_view("join_key") &&
            vector.typed_schema[1].type == std::string_view("INT64") &&
            vector.typed_schema[1].nullable &&
            vector.typed_schema[2].field_id == std::string_view("payload") &&
            vector.typed_schema[2].type == std::string_view("TEXT") &&
            vector.typed_schema[2].nullable &&
            vector.literal_rows[0].join_key == 1 &&
            vector.literal_rows[1].join_key == 2 &&
            vector.literal_rows[0].payload ==
                std::string_view("document-one") &&
            vector.literal_rows[1].payload ==
                std::string_view("document-two"),
        "immutable typed schema or literal row payload drifted");
    passed &= Require(
        vector.statement_uuid ==
            "10000000" + std::string(vector.literal_rows[0].row_uuid).substr(8) &&
            generation_is(vector.selected_generations, 7, 7) &&
            generation_is(vector.current_generations, index == 5 ? 8 : 7,
                          index == 6 ? 8 : 7),
        "immutable statement or generation tuple drifted");
    passed &= Require(
        vector.provider_state ==
                std::string_view(normal_setup ? "published_validated"
                                              : "fault_injected") &&
            vector.descriptor_state ==
                std::string_view(normal_descriptor ? "current"
                                                   : "mismatched") &&
            vector.security_state ==
                std::string_view(normal_security ? "authorized"
                                                 : "redacted_or_denied") &&
            vector.mga_state ==
                std::string_view(normal_mga ? "bound_statement_context"
                                            : "substituted") &&
            vector.resource_state ==
                std::string_view(normal_resource ? "within_limit"
                                                 : "over_limit") &&
            vector.cancellation_requested == (index == 3) &&
            vector.injected_mutation == std::string_view(expected_mutation) &&
            vector.injected_fault == std::string_view(expected_fault) &&
            vector.expected_cleanup_state ==
                std::string_view(
                    special_cleanup
                        ? "all_started_components_cleaned_once_root_absent"
                        : "normal_completion_cleanup_once"),
        "immutable setup/mutation/fault/cleanup tuple drifted");
    passed &= Require(
        std::string_view(vector.expected_outcome).starts_with(
            success ? "success:" : "refusal:") &&
            (success
                 ? vector.expected_diagnostic ==
                       std::string_view("not_applicable")
                 : std::string_view(vector.expected_outcome)
                       .contains(vector.expected_diagnostic)) &&
            vector.ordering_rule ==
                std::string_view(exact_order ? "exact_fixture_order"
                                             : "not_applicable"),
        "immutable expected outcome/diagnostic/order tuple drifted");

    const auto planning = PlanningRequest(vector);
    auto execution = ExecutionRequest(vector, false);
    const auto provider = execution.execute_provider(execution.input);
    passed &= Require(
        planning.family_id == vector.family_id &&
            planning.object_uuid == FixtureObjectUuid(vector) &&
            planning.mga_statement_context.statement_uuid ==
                vector.statement_uuid &&
            planning.catalog_generation == vector.selected_generations.catalog &&
            planning.current_catalog_generation ==
                vector.current_generations.catalog &&
            planning.security_epoch == vector.selected_generations.security &&
            planning.policy_epoch == vector.selected_generations.policy &&
            planning.resource_epoch == vector.selected_generations.resource &&
            planning.route_epoch == vector.selected_generations.route &&
            planning.route_generation == vector.current_generations.route &&
            planning.security_admitted == normal_security &&
            (planning.memory_budget_bytes == 4096) == normal_resource &&
            planning.candidates.size() == 1 &&
            planning.candidates[0].provider_generation ==
                vector.selected_generations.provider,
        "signed vector did not drive the complete planning request");
    passed &= Require(
        execution.input.family_id == vector.family_id &&
            execution.input.object_uuid == FixtureObjectUuid(vector) &&
            execution.input.mga_statement_context.statement_uuid ==
                vector.statement_uuid &&
            execution.input.catalog_generation ==
                vector.selected_generations.catalog &&
            execution.input.descriptor_generation ==
                vector.selected_generations.descriptor &&
            execution.input.security_generation ==
                vector.selected_generations.security &&
            execution.input.policy_generation ==
                vector.selected_generations.policy &&
            execution.input.resource_generation ==
                vector.selected_generations.resource &&
            execution.current_catalog_generation ==
                vector.current_generations.catalog &&
            execution.current_descriptor_generation ==
                vector.current_generations.descriptor &&
            execution.current_security_generation ==
                vector.current_generations.security &&
            execution.current_policy_generation ==
                vector.current_generations.policy &&
            execution.current_resource_generation ==
                vector.current_generations.resource &&
            execution.current_provider_generation ==
                vector.current_generations.provider &&
            execution.fault_injected == !normal_setup &&
            execution.security_admitted == normal_security && provider.ok &&
            provider.provider_batch.batch.columns.size() == 3 &&
            provider.provider_batch.batch.rows.size() == 2,
        "signed vector did not drive the complete execution request");
    for (std::size_t column = 0; column < vector.typed_schema.size(); ++column) {
      passed &= Require(
          provider.provider_batch.batch.columns[column].stable_name ==
                  vector.typed_schema[column].field_id &&
              provider.provider_batch.batch.columns[column]
                      .descriptor.canonical_type_name ==
                  FixtureEngineType(vector.typed_schema[column]) &&
              provider.provider_batch.batch.columns[column].nullable ==
                  vector.typed_schema[column].nullable,
          "signed typed schema did not drive provider column descriptors");
    }
    for (std::size_t row = 0; row < vector.literal_rows.size(); ++row) {
      passed &= Require(
          provider.provider_batch.batch.rows[row].values[0].encoded_value ==
                  vector.literal_rows[row].row_uuid &&
              provider.provider_batch.batch.rows[row].values[1].encoded_value ==
                  std::to_string(vector.literal_rows[row].join_key) &&
              provider.provider_batch.batch.rows[row].values[2].encoded_value ==
                  vector.literal_rows[row].payload &&
              provider.provider_batch.ordered_row_identities[row].row_uuid ==
                  vector.literal_rows[row].row_uuid,
          "signed literal row/order did not drive the provider batch");
    }
    const auto first_cancellation_probe = execution.cancellation_requested();
    const auto second_cancellation_probe = execution.cancellation_requested();
    passed &= Require(!first_cancellation_probe &&
                          second_cancellation_probe ==
                              vector.cancellation_requested,
                      "signed cancellation state did not drive its probe");
  }
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= CatalogIdentity();
  passed &= PositiveBoundaryFallback();
  passed &= RefusalVectors();
  passed &= ExactIdentityAndPropertyRefusals();
  passed &= DeterministicReplay();
  passed &= MissingNullAndUnavailableFallback();
  passed &= LogicalModelIdentityAdmission();
#if defined(SB_CES05_PRODUCTION_QUERY_ROUTE)
  passed &= ValidateProductionRecursiveAggregateAnchorAdmission();
  passed &= ProductionCanonicalQueryExecuteRoute();
#endif
  return passed ? 0 : 1;
}
