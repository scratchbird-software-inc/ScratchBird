// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "descriptor_value_runtime.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/plan_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kTypeUuid =
    "019f0000-0000-7300-8000-000000420001";

template <typename T>
concept HasCallerCandidates = requires(T value) { value.candidates; };

template <typename T>
concept HasCallerRelationUuid = requires(T value) { value.relation_uuid; };

template <typename T>
concept HasCallerExecutorRegistry = requires(T value) {
  value.available_executors;
};

template <typename T>
concept HasCallerPhysicalBatch = requires(T value) {
  value.physical_output_batch;
};

template <typename T>
concept HasCallerColumnBindings = requires(T value) {
  value.column_bindings;
};

template <typename T>
concept HasCallerRelationPointer = requires(T value) {
  value.relation_uuid;
};

template <typename T>
concept HasCallerColumnUuid = requires(T value) {
  value.column_uuid;
};

template <typename T>
concept HasCallerCatalogObjects = requires(T value) {
  value.catalog_object_uuids;
};

template <typename T>
concept HasCallerAuthorizationObjects = requires(T value) {
  value.authorized_object_uuids;
};

template <typename T>
concept HasCallerStatistics = requires(T value) { value.statistics; };

template <typename T>
concept HasCallerPhysicalDag = requires(T value) { value.physical_dag; };

template <typename T>
concept HasCallerSql = requires(T value) { value.sql; };

template <typename T>
concept HasCallerTransactionFinality = requires(T value) {
  value.transaction_finality_claimed;
};

template <typename T>
concept HasCallerRecoveryAuthority = requires(T value) {
  value.recovery_authority_claimed;
};

template <typename T>
concept HasCallerWalAuthority = requires(T value) {
  value.wal_is_transaction_or_recovery_authority;
};

static_assert(!HasCallerCandidates<exec::CanonicalHeapRelationAcquisitionRequest>);
static_assert(!HasCallerRelationUuid<exec::CanonicalHeapRelationAcquisitionRequest>);
static_assert(!HasCallerCandidates<exec::CanonicalHeapPhysicalDagDispatchRequest>);
static_assert(!HasCallerRelationUuid<exec::CanonicalHeapPhysicalDagDispatchRequest>);
static_assert(
    !HasCallerExecutorRegistry<exec::CanonicalHeapPhysicalDagDispatchRequest>);
static_assert(!HasCallerExecutorRegistry<
              api::CanonicalHeapOptimizerSelectedExecutionRequest>);
static_assert(!HasCallerPhysicalBatch<
              api::CanonicalHeapOptimizerSelectedExecutionRequest>);
static_assert(!HasCallerColumnBindings<
              api::CanonicalHeapOptimizerSelectedExecutionRequest>);
static_assert(!HasCallerCandidates<
              api::CanonicalHeapOptimizerSelectedExecutionRequest>);
static_assert(!HasCallerRelationPointer<
              api::CanonicalHeapOptimizerSelectedExecutionRequest>);
static_assert(!HasCallerColumnUuid<
              api::CanonicalHeapOptimizerSelectedExecutionRequest>);
static_assert(!HasCallerCatalogObjects<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerAuthorizationObjects<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerStatistics<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerPhysicalDag<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerExecutorRegistry<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerPhysicalBatch<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerSql<api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerTransactionFinality<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerRecoveryAuthority<
              api::CanonicalHeapOptimizerAdmissionRequest>);
static_assert(!HasCallerWalAuthority<
              api::CanonicalHeapOptimizerAdmissionRequest>);

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "QOW-TEST-QRY-004-HEAP-MGA-V1: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(const bool condition, std::string_view detail) {
  if (!condition) { Fail(detail); }
}

struct FixtureFileImage {
  std::string path;
  std::string bytes;

  bool operator==(const FixtureFileImage&) const = default;
};

std::vector<FixtureFileImage> CaptureFixtureFiles(
    const std::filesystem::path& directory) {
  std::vector<FixtureFileImage> images;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream input(entry.path(), std::ios::binary);
    Require(input.good(), "fixture file snapshot open failed");
    images.push_back(
        {std::filesystem::relative(entry.path(), directory).generic_string(),
         std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>())});
  }
  std::ranges::sort(images, {}, &FixtureFileImage::path);
  return images;
}

void AppendCompleteRelationDescriptorRecord(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  std::ofstream output(context.database_path + ".sb.mga_relation_descriptors",
                       std::ios::binary | std::ios::app);
  Require(output.good(), "relation descriptor fixture append open failed");
  output << "SBMGADESC1\tRELATION\t" << descriptor.relation_uuid.canonical
         << '\t'
         << api::EncodeCrudPairs(
                api::SerializeMgaRelationStorageDescriptor(descriptor))
         << '\n';
  output.flush();
  Require(output.good(), "complete relation descriptor fixture append failed");
}

std::string NativeAdmissionFingerprint(
    const opt::CanonicalNativeAdmissionBuildResult& built) {
  std::ostringstream output;
  const auto number = [&](const auto value) { output << value << ';'; };
  const auto flag = [&](const bool value) { output << (value ? "1;" : "0;"); };
  const auto text = [&](const std::string_view value) {
    output << value.size() << ':' << value << ';';
  };
  const auto numbers = [&](const auto& values) {
    number(values.size());
    for (const auto value : values) number(value);
  };
  const auto texts = [&](const auto& values) {
    number(values.size());
    for (const auto& value : values) text(value);
  };
  const auto graph = [&](const auto& value) {
    number(value.abi_version);
    text(value.bound_sblr_tree_uuid);
    text(value.catalog_epoch_uuid);
    text(value.security_context_uuid);
    number(value.local_transaction_id);
    number(value.statement_snapshot_id);
    number(value.root_logical_node_id);
    numbers(value.result_descriptor_ids);
    number(value.nodes.size());
    for (const auto& node : value.nodes) {
      number(node.logical_node_id);
      number(static_cast<std::uint32_t>(node.node_kind));
      numbers(node.input_logical_node_ids);
      numbers(node.output_descriptor_ids);
      numbers(node.bound_expression_ids);
      numbers(node.origin_relational_node_ids);
      texts(node.required_object_uuids);
      text(node.semantic_variant_id);
      flag(node.shareable);
      texts(node.required_property_uuids);
      texts(node.delivered_property_uuids);
    }
    flag(value.raw_sql_text_present);
    flag(value.parser_execution_authority_claimed);
    flag(value.transaction_finality_authority_claimed);
  };
  const auto properties = [&](const auto& value) {
    number(value.abi_version);
    text(value.bound_sblr_tree_uuid);
    text(value.catalog_epoch_uuid);
    text(value.security_context_uuid);
    number(value.local_transaction_id);
    number(value.statement_snapshot_id);
    number(value.properties.size());
    for (const auto& property : value.properties) {
      text(property.property_uuid);
      number(static_cast<std::uint32_t>(property.property_kind));
      number(property.origin_logical_node_id);
      numbers(property.expression_ids);
      number(property.ordering_terms.size());
      for (const auto& term : property.ordering_terms) {
        number(term.expression_id);
        number(static_cast<std::uint32_t>(term.direction));
        number(static_cast<std::uint32_t>(term.null_placement));
        text(term.collation_uuid);
      }
      texts(property.dependency_property_uuids);
      text(property.window_frame_descriptor_uuid);
      flag(property.populated_from_bound_sblr);
    }
    flag(value.raw_sql_text_present);
    flag(value.parser_execution_authority_claimed);
    flag(value.transaction_finality_authority_claimed);
  };
  const auto request = [&](const auto& value) {
    number(value.abi_version);
    graph(value.logical_graph);
    properties(value.logical_properties);
    text(value.catalog.snapshot_uuid);
    text(value.catalog.catalog_epoch_uuid);
    number(value.catalog.catalog_generation);
    texts(value.catalog.object_uuids);
    numbers(value.catalog.descriptor_ids);
    flag(value.catalog.engine_owned);
    text(value.security.security_context_uuid);
    number(value.security.security_epoch);
    number(value.security.policy_epoch);
    number(value.security.catalog_generation);
    texts(value.security.authorized_object_uuids);
    flag(value.security.engine_owned);
    number(value.mga.local_transaction_id);
    number(value.mga.statement_snapshot_id);
    text(value.mga.metadata_snapshot_uuid);
    flag(value.mga.transaction_active);
    flag(value.mga.statement_snapshot_fixed);
    flag(value.mga.engine_owned);
    flag(value.mga.finality_authority_claimed);
    text(value.policy_capability.policy_snapshot_uuid);
    number(value.policy_capability.policy_epoch);
    text(value.policy_capability.capability_snapshot_uuid);
    number(value.policy_capability.capability_abi_version);
    number(value.policy_capability.supported_node_kinds.size());
    for (const auto kind : value.policy_capability.supported_node_kinds) {
      number(static_cast<std::uint32_t>(kind));
    }
    flag(value.policy_capability.engine_owned);
    flag(value.policy_capability.cluster_capability_claimed);
    text(value.resource.resource_snapshot_uuid);
    number(value.resource.resource_epoch);
    number(value.resource.memory_budget_bytes);
    number(value.resource.maximum_candidate_count);
    number(value.resource.maximum_memo_groups);
    number(value.resource.maximum_search_steps);
    number(value.resource.maximum_planning_time_ns);
    flag(value.resource.spill_allowed);
    flag(value.resource.engine_owned);
    number(value.statistics.abi_version);
    text(value.statistics.statistics_snapshot_uuid);
    text(value.statistics.catalog_epoch_uuid);
    number(value.statistics.statistics_generation);
    number(value.statistics.admitted_at_monotonic_ns);
    number(value.statistics.node_estimates.size());
    for (const auto& estimate : value.statistics.node_estimates) {
      number(estimate.logical_node_id);
      text(estimate.object_uuid);
      number(static_cast<std::uint32_t>(estimate.state));
      number(static_cast<std::uint32_t>(estimate.source));
      text(estimate.catalog_epoch_uuid);
      text(estimate.statistics_snapshot_uuid);
      number(estimate.statistics_generation);
      number(estimate.collected_at_monotonic_ns);
      number(estimate.admitted_at_monotonic_ns);
      number(estimate.maximum_age_ns);
      number(static_cast<std::uint32_t>(estimate.confidence));
      flag(estimate.row_count_present);
      number(estimate.row_count);
      flag(estimate.page_count_present);
      number(estimate.page_count);
      flag(estimate.derived_from_runtime_actuals);
      flag(estimate.benchmark_clean_authority_claimed);
    }
    flag(value.statistics.captured_before_data_access);
    flag(value.statistics.data_access_observed);
    flag(value.statistics.runtime_actuals_present);
    flag(value.statistics.parser_statistics_authority_claimed);
    text(value.route.route_snapshot_uuid);
    number(value.route.route_epoch);
    number(value.route.route_generation);
    text(value.route.operation_id);
    text(value.route.route_id);
    flag(value.route.native_local_route);
    flag(value.route.engine_owned);
    flag(value.route.cluster_route_claimed);
    flag(value.populated_from_admitted_typed_sblr);
    flag(value.data_access_observed);
    flag(value.parser_planning_authority_claimed);
  };
  const auto admission = [&](const auto& value) {
    flag(value.admitted);
    flag(value.planning_allowed);
    flag(value.degraded_for_unknown_statistics);
    flag(value.benchmark_clean_ready);
    flag(value.data_access_allowed);
    text(value.bound_sblr_tree_uuid);
    text(value.catalog_epoch_uuid);
    text(value.security_context_uuid);
    text(value.capability_snapshot_uuid);
    text(value.resource_snapshot_uuid);
    text(value.statistics_snapshot_uuid);
    text(value.route_snapshot_uuid);
    number(value.local_transaction_id);
    number(value.statement_snapshot_id);
    number(value.catalog_generation);
    number(value.security_epoch);
    number(value.policy_epoch);
    number(value.resource_epoch);
    number(value.statistics_generation);
    number(value.route_epoch);
    number(value.route_generation);
    number(value.evidence.size());
    for (const auto& evidence : value.evidence) {
      number(static_cast<std::uint32_t>(evidence.stage));
      text(evidence.evidence_id);
    }
    number(value.issues.size());
    for (const auto& issue : value.issues) {
      number(static_cast<std::uint32_t>(issue.stage));
      text(issue.diagnostic_id);
      text(issue.field_id);
    }
  };
  flag(built.built);
  request(built.request);
  admission(built.admission);
  text(built.diagnostic_id);
  text(built.field_id);
  return output.str();
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view detail) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(detail);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(const platform::UuidKind kind,
                            const platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "UUID generation failed");
  return generated.value;
}

std::string NewUuidText(const platform::UuidKind kind,
                        const platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::string main_table_uuid;
  std::string empty_table_uuid;
  std::string full_width_table_uuid;
  std::string empty_full_width_table_uuid;
  std::string missing_later_column_table_uuid;
  std::string duplicate_later_column_table_uuid;
  std::string malformed_later_column_table_uuid;
  std::string malformed_table_uuid;
  std::string temporary_table_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) {
      std::filesystem::remove_all(directory, ignored);
    }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
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

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "rollback failed");
}

api::EngineRequestContext QueryContext(api::EngineRequestContext context,
                                       const std::string& table_uuid,
                                       const platform::u64 salt) {
  context.statement_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 1);
  context.statement_metadata_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 2);
  context.statement_metadata_snapshot_engine_owned = true;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 3);
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid;
  subject.subject_kind = "principal";
  context.authorization_context.effective_subjects.push_back(subject);
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 4);
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = table_uuid;
  grant.right = "SELECT";
  grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));
  context.optimizer_capability_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 5);
  context.optimizer_resource_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 6);
  context.optimizer_route_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 7);
  context.optimizer_route_epoch = 1;
  context.optimizer_route_generation = 1;
  context.optimizer_memory_budget_bytes = 8 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 1024;
  context.optimizer_maximum_memo_groups = 32;
  context.optimizer_maximum_search_steps = 128;
  context.optimizer_maximum_planning_time_ns = 1'000'000;
  context.current_monotonic_ns = "1";
  return context;
}

std::string EncodedInt64Descriptor() {
  return "canonical=int64;type_uuid=" + std::string(kTypeUuid) +
         ";nullable=true";
}

api::EngineDescriptor InputDescriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor = EncodedInt64Descriptor();
  return descriptor;
}

api::EngineRowValue Int64Row(const std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = InputDescriptor();
  typed.encoded_value = std::to_string(value);
  api::EngineRowValue row;
  row.fields.push_back({"value", std::move(typed)});
  return row;
}

api::EngineRowValue NullRow() {
  api::EngineTypedValue typed;
  typed.descriptor = InputDescriptor();
  typed.is_null = true;
  typed.state = api::EngineValueState::sql_null;
  api::EngineRowValue row;
  row.fields.push_back({"value", std::move(typed)});
  return row;
}

api::EngineRowValue FullWidthRow(const std::int64_t first,
                                 const std::optional<std::int64_t> second) {
  api::EngineTypedValue first_value;
  first_value.descriptor = InputDescriptor();
  first_value.descriptor.encoded_descriptor =
      "canonical=int64;type_uuid=" + std::string(kTypeUuid) +
      ";nullable=false";
  first_value.encoded_value = std::to_string(first);
  api::EngineTypedValue second_value;
  second_value.descriptor = InputDescriptor();
  if (second.has_value()) {
    second_value.encoded_value = std::to_string(*second);
  } else {
    second_value.is_null = true;
    second_value.state = api::EngineValueState::sql_null;
  }
  api::EngineRowValue row;
  row.fields.push_back({"required_value", std::move(first_value)});
  row.fields.push_back({"nullable_value", std::move(second_value)});
  return row;
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           const std::string& table_uuid,
                           const bool temporary = false,
                           const bool full_width = false) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = table_uuid;
  table.default_name = "qow_heap_" + table_uuid.substr(table_uuid.size() - 6);
  if (full_width) {
    table.columns.push_back(
        {"required_value",
         "canonical=int64;type_uuid=" + std::string(kTypeUuid) +
             ";nullable=false"});
    table.columns.push_back({"nullable_value", EncodedInt64Descriptor()});
  } else {
    table.columns.push_back({"value", EncodedInt64Descriptor()});
  }
  table.temporary = temporary;
  if (temporary) {
    table.temporary_scope = "session";
    table.temporary_session_uuid = fixture.session_uuid;
    table.on_commit_action = "preserve_rows";
  }
  return table;
}

void PersistTable(const Fixture& fixture,
                  const api::EngineRequestContext& context,
                  const std::string& table_uuid,
                  const bool temporary = false,
                  const bool full_width = false) {
  const auto table = Table(fixture, context, table_uuid, temporary, full_width);
  const auto appended = api::AppendMgaTableMetadata(context, table);
  Require(!appended.error, "table metadata append failed");
  api::MgaRelationStorageDescriptor descriptor;
  const auto ensured =
      api::EnsureMgaRelationStorageDescriptor(context, table, {}, &descriptor);
  Require(!ensured.error, "persisted relation descriptor creation failed");
}

void InsertRows(const Fixture& fixture,
                const api::EngineRequestContext& context,
                const std::string& table_uuid,
                std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.target_object = request.target_table;
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.estimated_row_count = rows.size();
  request.input_rows = std::move(rows);
  RequireOk(api::EngineInsertRows(request), "fixture row insert failed");
}

void DeleteRow(const api::EngineRequestContext& context,
               const std::string& table_uuid,
               const std::string& row_uuid) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.delete_predicate.canonical_predicate_envelope = row_uuid;
  request.tombstone_only = true;
  const auto deleted = api::EngineDeleteRows(request);
  RequireOk(deleted, "fixture row deletion failed");
  Require(deleted.deleted_count == 1, "fixture deletion did not tombstone one row");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = NowMillis() % 1'000'000;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_qow_heap_mga_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "qow_heap_mga.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewUuid(platform::UuidKind::database, fixture.salt + 10);
  create.filespace_uuid =
      NewUuid(platform::UuidKind::filespace, fixture.salt + 11);
  create.creation_unix_epoch_millis = NowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.filespace_uuid = uuid::UuidToString(create.filespace_uuid.value);
  fixture.schema_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 20);
  fixture.principal_uuid =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 21);
  fixture.session_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 22);
  fixture.main_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 23);
  fixture.empty_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 24);
  fixture.full_width_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 27);
  fixture.empty_full_width_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 28);
  fixture.missing_later_column_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 34);
  fixture.duplicate_later_column_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 35);
  fixture.malformed_later_column_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 36);
  fixture.malformed_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 25);
  fixture.temporary_table_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 26);

  auto metadata = Begin(fixture, "qow-heap-metadata");
  PersistTable(fixture, metadata, fixture.main_table_uuid);
  PersistTable(fixture, metadata, fixture.empty_table_uuid);
  PersistTable(fixture, metadata, fixture.full_width_table_uuid, false, true);
  PersistTable(fixture, metadata, fixture.empty_full_width_table_uuid, false,
               true);
  PersistTable(fixture, metadata, fixture.missing_later_column_table_uuid,
               false, true);
  PersistTable(fixture, metadata, fixture.duplicate_later_column_table_uuid,
               false, true);
  PersistTable(fixture, metadata, fixture.malformed_later_column_table_uuid,
               false, true);
  PersistTable(fixture, metadata, fixture.malformed_table_uuid);
  PersistTable(fixture, metadata, fixture.temporary_table_uuid, true);
  Commit(metadata);

  auto writer = Begin(fixture, "qow-heap-main-writer");
  InsertRows(fixture,
             writer,
             fixture.main_table_uuid,
             {Int64Row(10), Int64Row(20), NullRow()});
  Commit(writer);

  auto full_width_writer = Begin(fixture, "qow-heap-full-width-writer");
  InsertRows(fixture,
             full_width_writer,
             fixture.full_width_table_uuid,
             {FullWidthRow(1, std::nullopt), FullWidthRow(2, 22)});
  Commit(full_width_writer);

  auto malformed_width_writer = Begin(
      fixture, "qow-heap-malformed-full-width-writer");
  const auto append_full_width_row = [&](const std::string& table_uuid,
                                         const platform::u64 row_salt,
                                         auto values) {
    api::CrudRowVersionRecord row;
    row.creator_tx = malformed_width_writer.local_transaction_id;
    row.table_uuid = table_uuid;
    row.row_uuid = NewUuidText(platform::UuidKind::row, row_salt);
    row.version_uuid =
        NewUuidText(platform::UuidKind::object, row_salt + 1);
    row.values = std::move(values);
    std::uint64_t ignored_sequence = 0;
    const auto appended = api::AppendMgaRowVersion(
        malformed_width_writer, row, &ignored_sequence);
    Require(!appended.error, "malformed full-width row append failed");
  };
  append_full_width_row(fixture.missing_later_column_table_uuid,
                        fixture.salt + 37,
                        std::vector<std::pair<std::string, std::string>>{
                            {"required_value", "1"}});
  append_full_width_row(fixture.duplicate_later_column_table_uuid,
                        fixture.salt + 39,
                        std::vector<std::pair<std::string, std::string>>{
                            {"required_value", "1"},
                            {"nullable_value", "2"},
                            {"nullable_value", "3"}});
  append_full_width_row(fixture.malformed_later_column_table_uuid,
                        fixture.salt + 41,
                        std::vector<std::pair<std::string, std::string>>{
                            {"required_value", "1"},
                            {"nullable_value", "not-an-int64"}});
  Commit(malformed_width_writer);

  auto malformed_writer = Begin(fixture, "qow-heap-malformed-writer");
  api::CrudRowVersionRecord valid;
  valid.creator_tx = malformed_writer.local_transaction_id;
  valid.table_uuid = fixture.malformed_table_uuid;
  valid.row_uuid =
      NewUuidText(platform::UuidKind::row, fixture.salt + 30);
  valid.version_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 31);
  valid.values = {{"value", "7"}};
  std::uint64_t ignored_sequence = 0;
  auto append =
      api::AppendMgaRowVersion(malformed_writer, valid, &ignored_sequence);
  Require(!append.error, "valid malformed-fixture row append failed");
  api::CrudRowVersionRecord malformed = valid;
  malformed.row_uuid =
      NewUuidText(platform::UuidKind::row, fixture.salt + 32);
  malformed.version_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 33);
  malformed.values = {{"value", "not-an-int64"}};
  append = api::AppendMgaRowVersion(
      malformed_writer, malformed, &ignored_sequence);
  Require(!append.error, "malformed fixture row append failed");
  Commit(malformed_writer);
  return fixture;
}

exec::CanonicalHeapRelationAcquisitionRequest BoundRequest(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor,
    const platform::u64 salt) {
  Require(!descriptor.columns.empty(),
          "fixture descriptor does not have persisted columns");
  auto* relational = new api::TypedRelationalDag();
  relational->wire_version = 2;
  relational->bound_sblr_tree_uuid =
      NewUuidText(platform::UuidKind::object, salt + 1);
  relational->bound_catalog_epoch_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  relational->bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  relational->root_node_id = 1;
  api::RelationalDagNode node;
  node.node_id = 1;
  node.node_kind = api::RelationalDagNodeKind::kScan;
  for (std::size_t ordinal = 0; ordinal < descriptor.columns.size();
       ++ordinal) {
    const auto id = static_cast<std::uint32_t>(ordinal + 1);
    const auto& column = descriptor.columns[ordinal];
    Require(column.ordinal == ordinal,
            "fixture descriptor columns are not in persisted ordinal order");
    api::RelationalTypeDescriptor type;
    type.descriptor_id = id;
    type.descriptor_uuid = column.value_descriptor.descriptor_uuid.canonical;
    type.type_uuid = std::string(kTypeUuid);
    type.nullability = column.nullable
                           ? api::RelationalNullability::kNullable
                           : api::RelationalNullability::kNonNull;
    relational->descriptors.push_back(std::move(type));
    api::RelationalExpressionRecord expression;
    expression.expression_id = id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = id;
    expression.bound_name_uuid = column.column_uuid.canonical;
    relational->expressions.push_back(std::move(expression));
    api::RelationalOutputRecord output;
    output.output_id = id;
    output.relation_node_id = 1;
    output.expression_id = id;
    output.output_name_utf8 = column.canonical_name_key;
    output.descriptor_id = id;
    output.visible = true;
    output.ordinal = ordinal;
    relational->outputs.push_back(std::move(output));
    node.output_descriptor_ids.push_back(id);
    node.bound_expression_ids.push_back(id);
  }
  node.required_object_uuids = {descriptor.relation_uuid.canonical};
  node.semantic_variant_id = "relation.source.v1";
  relational->nodes.push_back(node);

  exec::CanonicalHeapRelationAcquisitionRequest request;
  request.context = &context;
  request.relational_dag = relational;
  request.physical_dag.abi_version = 2;
  request.physical_dag.selected_plan_uuid =
      NewUuidText(platform::UuidKind::object, salt + 2);
  request.physical_dag.root_physical_node_id = 11;
  request.physical_dag.local_transaction_id = context.local_transaction_id;
  request.physical_dag.statement_snapshot_id =
      context.snapshot_visible_through_local_transaction_id;
  request.physical_dag.bound_sblr_tree_uuid =
      relational->bound_sblr_tree_uuid;
  request.physical_dag.catalog_epoch_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  request.physical_dag.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  request.physical_dag.capability_snapshot_uuid =
      context.optimizer_capability_snapshot_uuid.canonical;
  request.physical_dag.resource_snapshot_uuid =
      context.optimizer_resource_snapshot_uuid.canonical;
  request.physical_dag.statistics_snapshot_uuid =
      NewUuidText(platform::UuidKind::object, salt + 3);
  request.physical_dag.route_snapshot_uuid =
      context.optimizer_route_snapshot_uuid.canonical;
  request.physical_dag.catalog_generation = context.catalog_generation_id;
  request.physical_dag.security_epoch = context.security_epoch;
  request.physical_dag.policy_epoch =
      context.authorization_context.policy_epoch;
  request.physical_dag.resource_epoch = context.resource_epoch;
  request.physical_dag.statistics_generation = 1;
  request.physical_dag.route_epoch = context.optimizer_route_epoch;
  request.physical_dag.route_generation = context.optimizer_route_generation;
  request.physical_dag.memory_budget_bytes =
      context.optimizer_memory_budget_bytes;
  request.physical_dag.optimizer_published = true;
  request.physical_dag.immutable_node_identity_validated = true;
  request.physical_dag.capability_validated_before_access = true;
  const std::string capability =
      context.optimizer_capability_snapshot_uuid.canonical;
  const std::vector<std::string> evidence{
      relational->bound_sblr_tree_uuid,
      context.statement_metadata_snapshot_uuid.canonical,
      context.authorization_context.authority_uuid.canonical,
      context.statement_metadata_snapshot_uuid.canonical,
      capability,
      context.optimizer_resource_snapshot_uuid.canonical,
      request.physical_dag.statistics_snapshot_uuid,
      context.optimizer_route_snapshot_uuid.canonical};
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    request.physical_dag.admission_evidence.push_back(
        {static_cast<exec::PhysicalAdmissionStage>(index + 1),
         evidence[index]});
  }
  exec::PhysicalNodeRecord physical;
  physical.physical_node_id = 11;
  physical.relational_node_id = 1;
  physical.node_kind = exec::PhysicalNodeKind::kScan;
  physical.implementation_id = "scan.heap.v1";
  physical.output_descriptor_ids = node.output_descriptor_ids;
  physical.causal_counter_id = 101;
  physical.selected_alternative_uuid =
      NewUuidText(platform::UuidKind::object, salt + 4);
  physical.executor_capability_uuid = capability;
  physical.executor_capability_abi_version = 1;
  physical.cost_vector_uuid =
      NewUuidText(platform::UuidKind::object, salt + 5);
  physical.memory_bytes_required = 1024;
  physical.engine_capability_validated = true;
  request.physical_dag.nodes.push_back(physical);
  request.selected_physical_node_id = 11;
  request.maximum_scanned_row_versions = 1024;
  request.maximum_decoded_bytes = 4 * 1024 * 1024;
  request.maximum_output_rows = 1024;
  request.maximum_output_columns = descriptor.columns.size();
  request.maximum_output_cells = 1024 * descriptor.columns.size();
  request.cancellation_requested = [] { return false; };
  return request;
}

void ReleaseRequest(exec::CanonicalHeapRelationAcquisitionRequest* request) {
  if (request == nullptr) { return; }
  delete request->relational_dag;
  request->relational_dag = nullptr;
}

api::CanonicalHeapOptimizerAdmissionRequest AdmissionRequestFor(
    const exec::CanonicalHeapRelationAcquisitionRequest& acquisition) {
  api::CanonicalHeapOptimizerAdmissionRequest request;
  request.context = *acquisition.context;
  request.relational_dag = *acquisition.relational_dag;
  return request;
}

exec::CanonicalHeapRelationAcquisitionRequest RequestFor(
    const api::EngineRequestContext& context,
    const std::string& relation_uuid,
    const platform::u64 salt) {
  const auto descriptor =
      api::LoadMgaRelationStorageDescriptor(context, relation_uuid);
  Require(descriptor.ok, "fixture descriptor load failed");
  return BoundRequest(context, descriptor.descriptor, salt);
}

exec::CanonicalHeapPhysicalDagDispatchRequest DispatchRequestFor(
    const exec::CanonicalHeapRelationAcquisitionRequest& acquisition) {
  exec::CanonicalHeapPhysicalDagDispatchRequest request;
  request.context = acquisition.context;
  request.relational_dag = acquisition.relational_dag;
  request.physical_dag = acquisition.physical_dag;
  request.maximum_scanned_row_versions =
      acquisition.maximum_scanned_row_versions;
  request.maximum_decoded_bytes = acquisition.maximum_decoded_bytes;
  request.maximum_output_rows = acquisition.maximum_output_rows;
  request.maximum_output_columns = acquisition.maximum_output_columns;
  request.maximum_output_cells = acquisition.maximum_output_cells;
  request.cancellation_requested = acquisition.cancellation_requested;
  return request;
}

api::CanonicalHeapOptimizerSelectedExecutionRequest SelectedRequestFor(
    const exec::CanonicalHeapRelationAcquisitionRequest& acquisition,
    const platform::u64 salt) {
  api::CanonicalHeapOptimizerSelectedExecutionRequest request;
  request.context = *acquisition.context;
  request.relational_dag = *acquisition.relational_dag;
  request.selected_physical_dag = acquisition.physical_dag;
  request.maximum_scanned_row_versions =
      acquisition.maximum_scanned_row_versions;
  request.maximum_decoded_bytes = acquisition.maximum_decoded_bytes;
  request.maximum_output_rows = acquisition.maximum_output_rows;
  request.maximum_output_columns = acquisition.maximum_output_columns;
  request.maximum_output_cells = acquisition.maximum_output_cells;
  request.cancellation_requested = acquisition.cancellation_requested;
  request.execution_attempt_uuid =
      NewUuidText(platform::UuidKind::object, salt + 1);
  request.transaction_effect_evidence_uuid =
      NewUuidText(platform::UuidKind::object, salt + 2);
  return request;
}

void RequireAtomicFailure(
    const exec::CanonicalHeapRelationAcquisitionResult& result,
    std::string_view detail) {
  Require(!result.diagnostic.ok && result.output_batch.columns.empty() &&
              result.output_batch.rows.empty() &&
              result.emitted_record_uuids.empty() &&
              result.emitted_row_version_uuids.empty() &&
              result.column_uuids.empty() &&
              result.counters.emitted_row_count == 0 &&
              result.counters.output_column_count == 0 &&
              result.counters.materialized_cell_count == 0,
          detail);
}

void RequireAtomicDispatchFailure(
    const exec::CanonicalPhysicalDagDispatchResult& result,
    std::string_view detail) {
  Require(!result.diagnostic.ok && result.executed_steps.empty() &&
              result.root_result_handle_id == 0 &&
              result.root_output_descriptor_ids.empty() &&
              result.selected_plan_uuid.empty() &&
              result.executed_root_physical_node_id == 0 &&
              result.root_causal_counter_id == 0,
          detail);
}

void RequireAtomicSelectedFailure(
    const api::CanonicalOptimizerSelectedExecutionResult& result,
    std::string_view detail) {
  Require(!result.accepted && !result.canonical_result_published &&
              !result.result_publication.published &&
              result.result_publication.envelope.column_descriptors.empty() &&
              result.result_publication.row_stream.columns.empty() &&
              result.result_publication.row_stream.rows.empty() &&
              result.result_publication.delivery_records.empty() &&
              result.result_publication.canonical_envelope_bytes.empty(),
          detail);
}

void RequireAtomicAdmissionFailure(
    const api::CanonicalHeapOptimizerAdmissionResult& result,
    std::string_view detail) {
  Require(!result.built && !result.issue.diagnostic_id.empty() &&
              result.request.logical_graph.nodes.empty() &&
              result.request.logical_properties.properties.empty() &&
              result.request.catalog.object_uuids.empty() &&
              result.request.catalog.descriptor_ids.empty() &&
              result.request.security.authorized_object_uuids.empty() &&
              result.request.statistics.node_estimates.empty() &&
              !result.admission.admitted &&
              result.admission.evidence.empty() &&
              result.admission.issues.empty() &&
              result.current_relation_descriptor_uuid.empty() &&
              result.current_relation_descriptor_generation == 0,
          detail);
}

void RequireAtomicObjectAdmissionBuildFailure(
    const opt::CanonicalNativeAdmissionBuildResult& result,
    const std::string_view diagnostic_id,
    const std::string_view field_id,
    const std::string_view detail) {
  Require(!result.built && result.diagnostic_id == diagnostic_id &&
              result.field_id == field_id &&
              result.request.logical_graph.nodes.empty() &&
              result.request.logical_properties.properties.empty() &&
              result.request.catalog.object_uuids.empty() &&
              result.request.catalog.descriptor_ids.empty() &&
              result.request.security.authorized_object_uuids.empty() &&
              result.request.statistics.node_estimates.empty() &&
              !result.admission.admitted &&
              result.admission.evidence.empty() &&
              result.admission.issues.empty(),
          detail);
}

bool SameDescriptor(const api::EngineDescriptor& left,
                    const api::EngineDescriptor& right) {
  return left.descriptor_uuid.canonical == right.descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

bool PreservesPersistedDescriptorFields(
    const api::EngineDescriptor& runtime,
    const api::EngineDescriptor& persisted) {
  return runtime.descriptor_kind == "scalar" &&
         runtime.descriptor_uuid.canonical ==
             persisted.descriptor_uuid.canonical &&
         runtime.canonical_type_name == persisted.canonical_type_name &&
         runtime.encoded_descriptor == persisted.encoded_descriptor;
}

void ValidatePositiveAndVisibilityMatrix(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-reader"),
                             fixture.main_table_uuid,
                             fixture.salt + 100);
  auto request = RequestFor(reader, fixture.main_table_uuid,
                            fixture.salt + 120);
  const auto persisted =
      api::LoadMgaRelationStorageDescriptor(reader, fixture.main_table_uuid);
  Require(persisted.ok && persisted.descriptor.columns.size() == 1,
          "current persisted descriptor was not available to the gate");
  const auto& persisted_value_descriptor =
      persisted.descriptor.columns.front().value_descriptor;
  const auto first = exec::ExecuteCanonicalHeapRelationAcquisition(request);
  Require(first.diagnostic.ok && first.output_batch.columns.size() == 1 &&
              first.output_batch.rows.size() == 3 &&
              first.counters.emitted_row_count == 3 &&
              first.authority.engine_catalog_descriptor_loaded &&
              first.authority.engine_mga_snapshot_bound &&
              first.authority.engine_authorization_rechecked &&
              first.authority.bounded_physical_read &&
              !first.authority.caller_candidates_consumed &&
              !first.authority.owns_transaction_finality &&
              !first.authority.owns_recovery &&
              !first.authority.owns_parser_execution &&
              !first.authority.wal_is_visibility_or_recovery_authority &&
              PreservesPersistedDescriptorFields(
                  first.output_batch.columns.front().descriptor,
                  persisted_value_descriptor) &&
              first.output_batch.columns.front().nullable ==
                  persisted.descriptor.columns.front().nullable &&
              first.output_batch.columns.front().stable_name ==
                  persisted.descriptor.columns.front().canonical_name_key &&
              first.column_uuids == std::vector<std::string>{
                  persisted.descriptor.columns.front().column_uuid.canonical} &&
              first.counters.output_column_count == 1 &&
              first.counters.materialized_cell_count == 3 &&
              persisted.descriptor.columns.front().ordinal == 0 &&
              request.relational_dag->outputs.front().ordinal == 0 &&
              persisted_value_descriptor.encoded_descriptor.find(
                  "type_uuid=" + std::string(kTypeUuid)) !=
                  std::string::npos,
          "committed heap rows were not acquired under bounded MGA authority");
  std::size_t null_count = 0;
  for (const auto& row : first.output_batch.rows) {
    Require(SameDescriptor(row.values.front().descriptor,
                           first.output_batch.columns.front().descriptor) &&
                PreservesPersistedDescriptorFields(
                    row.values.front().descriptor,
                    persisted_value_descriptor),
            "runtime carrier rewrote a persisted descriptor field");
    if (row.values.front().state == api::EngineValueState::sql_null) {
      ++null_count;
    }
  }
  Require(null_count == 1, "typed SQL NULL was not preserved");
  const auto repeated = exec::ExecuteCanonicalHeapRelationAcquisition(request);
  Require(repeated.diagnostic.ok &&
              repeated.emitted_record_uuids == first.emitted_record_uuids &&
              repeated.emitted_row_version_uuids ==
                  first.emitted_row_version_uuids,
          "same-snapshot heap acquisition was unstable");

  auto uncommitted_writer = Begin(fixture, "qow-heap-uncommitted-writer");
  InsertRows(fixture,
             uncommitted_writer,
             fixture.main_table_uuid,
             {Int64Row(30)});
  const auto while_uncommitted =
      exec::ExecuteCanonicalHeapRelationAcquisition(request);
  Require(while_uncommitted.diagnostic.ok &&
              while_uncommitted.output_batch.rows.size() == 3,
          "other transaction's uncommitted row became visible");
  Commit(uncommitted_writer);
  const auto after_snapshot_commit =
      exec::ExecuteCanonicalHeapRelationAcquisition(request);
  Require(after_snapshot_commit.diagnostic.ok &&
              after_snapshot_commit.output_batch.rows.size() == 3,
          "post-snapshot commit became visible in repeatable-read snapshot");
  ReleaseRequest(&request);
  Rollback(reader);

  auto current_reader = QueryContext(Begin(fixture, "qow-heap-current-reader"),
                                     fixture.main_table_uuid,
                                     fixture.salt + 140);
  auto current = RequestFor(current_reader,
                            fixture.main_table_uuid,
                            fixture.salt + 160);
  const auto current_result =
      exec::ExecuteCanonicalHeapRelationAcquisition(current);
  Require(current_result.diagnostic.ok &&
              current_result.output_batch.rows.size() == 4,
          "new snapshot did not observe committed row");
  const std::string row_to_delete = current_result.emitted_record_uuids.front();
  ReleaseRequest(&current);
  Rollback(current_reader);

  auto delete_writer = Begin(fixture, "qow-heap-delete-writer");
  DeleteRow(delete_writer, fixture.main_table_uuid, row_to_delete);
  Commit(delete_writer);
  auto delete_reader = QueryContext(Begin(fixture, "qow-heap-delete-reader"),
                                    fixture.main_table_uuid,
                                    fixture.salt + 170);
  auto after_delete = RequestFor(delete_reader,
                                 fixture.main_table_uuid,
                                 fixture.salt + 171);
  const auto after_delete_result =
      exec::ExecuteCanonicalHeapRelationAcquisition(after_delete);
  Require(after_delete_result.diagnostic.ok &&
              after_delete_result.output_batch.rows.size() == 3 &&
              after_delete_result.counters.tombstone_row_count >= 1,
          "committed tombstone did not suppress the prior row version");
  ReleaseRequest(&after_delete);
  Rollback(delete_reader);

  auto rollback_writer = Begin(fixture, "qow-heap-rollback-writer");
  InsertRows(fixture,
             rollback_writer,
             fixture.main_table_uuid,
             {Int64Row(40)});
  Rollback(rollback_writer);
  auto rollback_reader = QueryContext(Begin(fixture, "qow-heap-rollback-reader"),
                                      fixture.main_table_uuid,
                                      fixture.salt + 180);
  auto rolled_back = RequestFor(rollback_reader,
                                fixture.main_table_uuid,
                                fixture.salt + 200);
  const auto rolled_back_result =
      exec::ExecuteCanonicalHeapRelationAcquisition(rolled_back);
  Require(rolled_back_result.diagnostic.ok &&
              rolled_back_result.output_batch.rows.size() == 3 &&
              rolled_back_result.counters.invisible_row_version_count >= 1,
          "rolled-back row was visible or not visibility-rechecked");
  ReleaseRequest(&rolled_back);
  Rollback(rollback_reader);

  auto own_writer = QueryContext(Begin(fixture, "qow-heap-own-writer"),
                                 fixture.main_table_uuid,
                                 fixture.salt + 220);
  InsertRows(fixture, own_writer, fixture.main_table_uuid, {Int64Row(50)});
  auto own = RequestFor(own_writer,
                        fixture.main_table_uuid,
                        fixture.salt + 240);
  const auto own_result = exec::ExecuteCanonicalHeapRelationAcquisition(own);
  Require(own_result.diagnostic.ok && own_result.output_batch.rows.size() == 4,
          "reader's own active MGA write was not visible");
  ReleaseRequest(&own);
  Rollback(own_writer);

  auto empty_reader = QueryContext(Begin(fixture, "qow-heap-empty-reader"),
                                   fixture.empty_table_uuid,
                                   fixture.salt + 260);
  auto empty = RequestFor(empty_reader,
                          fixture.empty_table_uuid,
                          fixture.salt + 280);
  const auto empty_result =
      exec::ExecuteCanonicalHeapRelationAcquisition(empty);
  Require(empty_result.diagnostic.ok &&
              empty_result.output_batch.columns.size() == 1 &&
              empty_result.output_batch.rows.empty(),
          "empty relation did not produce a typed empty batch");
  ReleaseRequest(&empty);
  Rollback(empty_reader);
}

// QOW-TEST-QRY-004-HEAP-DISPATCH-V1
void ValidatePhysicalHeapDispatchMatrix(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-dispatch-reader"),
                             fixture.main_table_uuid,
                             fixture.salt + 281);
  auto acquisition = RequestFor(reader,
                                fixture.main_table_uuid,
                                fixture.salt + 282);
  const auto request = DispatchRequestFor(acquisition);
  const auto persisted =
      api::LoadMgaRelationStorageDescriptor(reader, fixture.main_table_uuid);
  Require(persisted.ok && persisted.descriptor.columns.size() == 1,
          "dispatch fixture descriptor load failed");

  const auto first = exec::ExecuteCanonicalHeapPhysicalDagDispatch(request);
  Require(first.diagnostic.ok && first.execution_started &&
              first.data_access_observed &&
              first.executed_steps.size() == 1 &&
              first.root_result_handle_id != 0 &&
              first.root_output_descriptor_ids ==
                  acquisition.physical_dag.nodes.front()
                      .output_descriptor_ids &&
              first.selected_plan_uuid ==
                  acquisition.physical_dag.selected_plan_uuid &&
              first.executed_root_physical_node_id ==
                  acquisition.physical_dag.nodes.front().physical_node_id &&
              first.root_causal_counter_id ==
                  acquisition.physical_dag.nodes.front().causal_counter_id &&
              first.authority.engine_mga_snapshot_bound &&
              !first.authority.owns_transaction_finality &&
              !first.authority.owns_recovery &&
              !first.authority.owns_parser_execution &&
              !first.authority.owns_visibility_outside_engine_mga &&
              !first.authority.wal_is_transaction_or_recovery_authority,
          "actual heap scan did not pass through canonical physical dispatch");
  const auto& step = first.executed_steps.front();
  Require(step.execution_ordinal == 1 && step.execution_started &&
              step.execution_finished &&
              step.counters_captured_after_finish &&
              step.data_access_observation_known &&
              step.data_access_observed &&
              step.input_row_count == 0 && step.output_row_count == 3 &&
              step.rows_examined >= step.output_row_count &&
              step.selected_plan_uuid == first.selected_plan_uuid &&
              step.executed_physical_node_id ==
                  first.executed_root_physical_node_id &&
              step.causal_counter_id == first.root_causal_counter_id &&
              step.result_handle_id == first.root_result_handle_id &&
              step.output_descriptor_ids ==
                  first.root_output_descriptor_ids &&
              step.authority.engine_mga_snapshot_bound &&
              step.heap_read_counters.has_value() &&
              step.heap_read_authority.has_value() &&
              step.heap_read_counters->emitted_row_count == 3 &&
              step.heap_read_counters->scanned_row_version_count ==
                  step.rows_examined &&
              step.heap_read_authority->engine_catalog_descriptor_loaded &&
              step.heap_read_authority->engine_mga_snapshot_bound &&
              step.heap_read_authority->engine_authorization_rechecked &&
              step.heap_read_authority->bounded_physical_read &&
              !step.heap_read_authority->caller_candidates_consumed &&
              !step.heap_read_authority->owns_transaction_finality &&
              !step.heap_read_authority->owns_recovery &&
              !step.heap_read_authority->owns_parser_execution &&
              !step.heap_read_authority
                   ->wal_is_visibility_or_recovery_authority &&
              step.current_relation_descriptor_uuid ==
                  persisted.descriptor.descriptor_uuid.canonical &&
              step.current_relation_descriptor_generation ==
                  persisted.descriptor.descriptor_generation &&
              step.materialized_output_batch.has_value() &&
              step.materialized_output_batch->columns.size() == 1 &&
              step.materialized_output_batch->rows.size() == 3,
          "heap dispatch step lost counter, authority, or causal evidence");
  const auto& batch = *step.materialized_output_batch;
  Require(PreservesPersistedDescriptorFields(
              batch.columns.front().descriptor,
              persisted.descriptor.columns.front().value_descriptor) &&
              batch.columns.front().nullable ==
                  persisted.descriptor.columns.front().nullable,
          "dispatch root rewrote the persisted descriptor identity");
  std::size_t null_count = 0;
  for (const auto& row : batch.rows) {
    Require(row.values.size() == 1 &&
                SameDescriptor(row.values.front().descriptor,
                               batch.columns.front().descriptor),
            "dispatch row lost its canonical descriptor");
    if (row.values.front().state == api::EngineValueState::sql_null) {
      ++null_count;
    }
  }
  Require(null_count == 1, "typed SQL NULL did not reach dispatch root");

  const auto repeated =
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(request);
  Require(repeated.diagnostic.ok && repeated.executed_steps.size() == 1 &&
              repeated.root_result_handle_id == first.root_result_handle_id &&
              repeated.root_output_descriptor_ids ==
                  first.root_output_descriptor_ids &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.executed_root_physical_node_id ==
                  first.executed_root_physical_node_id &&
              repeated.root_causal_counter_id ==
                  first.root_causal_counter_id &&
              repeated.executed_steps.front()
                      .current_relation_descriptor_uuid ==
                  step.current_relation_descriptor_uuid &&
              repeated.executed_steps.front()
                      .current_relation_descriptor_generation ==
                  step.current_relation_descriptor_generation,
          "repeated heap dispatch did not preserve deterministic identities");
  ReleaseRequest(&acquisition);
  Rollback(reader);

  auto empty_reader = QueryContext(Begin(fixture,
                                         "qow-heap-dispatch-empty-reader"),
                                   fixture.empty_table_uuid,
                                   fixture.salt + 283);
  auto empty_acquisition = RequestFor(empty_reader,
                                      fixture.empty_table_uuid,
                                      fixture.salt + 284);
  const auto empty = exec::ExecuteCanonicalHeapPhysicalDagDispatch(
      DispatchRequestFor(empty_acquisition));
  Require(empty.diagnostic.ok && empty.execution_started &&
              !empty.data_access_observed &&
              empty.executed_steps.size() == 1 &&
              empty.executed_steps.front().data_access_observation_known &&
              !empty.executed_steps.front().data_access_observed &&
              empty.executed_steps.front().materialized_output_batch.has_value() &&
              empty.executed_steps.front()
                  .materialized_output_batch->columns.size() == 1 &&
              empty.executed_steps.front()
                  .materialized_output_batch->rows.empty() &&
              empty.executed_steps.front().output_row_count == 0,
          "empty heap relation did not dispatch a typed empty root batch");
  ReleaseRequest(&empty_acquisition);
  Rollback(empty_reader);
}

void ValidatePhysicalHeapDispatchRefusals(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-dispatch-refusals"),
                             fixture.main_table_uuid,
                             fixture.salt + 285);
  auto acquisition = RequestFor(reader,
                                fixture.main_table_uuid,
                                fixture.salt + 286);
  const auto baseline = DispatchRequestFor(acquisition);

  auto mutated = baseline;
  mutated.context = nullptr;
  auto result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(result, "missing dispatch context was accepted");
  Require(!result.execution_started && !result.data_access_observed,
          "missing context reported physical execution or read");

  mutated = baseline;
  mutated.cancellation_requested = {};
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(result,
                               "missing cancellation probe was accepted");
  Require(!result.execution_started && !result.data_access_observed,
          "missing probe reached physical execution");

  mutated = baseline;
  mutated.maximum_scanned_row_versions = 0;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "zero scanned-version dispatch bound was accepted");
  mutated = baseline;
  mutated.maximum_decoded_bytes = 0;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "zero decoded-byte dispatch bound was accepted");
  mutated = baseline;
  mutated.maximum_output_rows = 0;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "zero output-row dispatch bound was accepted");
  mutated = baseline;
  mutated.maximum_output_columns = 0;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "zero output-column dispatch bound was accepted");
  mutated = baseline;
  mutated.maximum_output_cells = 0;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "zero output-cell dispatch bound was accepted");

  mutated = baseline;
  mutated.physical_dag.nodes.front().input_physical_node_ids = {11};
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(result,
                               "input edge entered leaf heap dispatch");
  Require(!result.execution_started && !result.data_access_observed,
          "input-edge refusal occurred after physical read");

  mutated = baseline;
  mutated.physical_dag.nodes.front().executor_capability_uuid.clear();
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "missing heap capability UUID was accepted");
  mutated = baseline;
  mutated.physical_dag.nodes.front().executor_capability_abi_version = 2;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "wrong heap capability ABI was accepted");

  mutated = baseline;
  auto second_heap = mutated.physical_dag.nodes.front();
  second_heap.physical_node_id = 12;
  second_heap.causal_counter_id = 102;
  second_heap.selected_alternative_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 288);
  second_heap.executor_capability_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 289);
  second_heap.cost_vector_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 290);
  auto project = mutated.physical_dag.nodes.front();
  project.physical_node_id = 13;
  project.node_kind = exec::PhysicalNodeKind::kProject;
  project.implementation_id = "project.typed.v1";
  project.input_physical_node_ids = {11, 12};
  project.causal_counter_id = 103;
  project.selected_alternative_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 291);
  project.executor_capability_uuid =
      reader.optimizer_capability_snapshot_uuid.canonical;
  project.cost_vector_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 292);
  mutated.physical_dag.nodes.push_back(std::move(second_heap));
  mutated.physical_dag.nodes.push_back(std::move(project));
  mutated.physical_dag.root_physical_node_id = 13;
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(
      result, "inconsistent duplicate heap capabilities were accepted");
  Require(!result.execution_started && !result.data_access_observed,
          "inconsistent heap capability reached physical execution");

  mutated = baseline;
  mutated.physical_dag.nodes.front().node_kind =
      exec::PhysicalNodeKind::kFilter;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "wrong heap physical kind was accepted");
  mutated = baseline;
  mutated.physical_dag.nodes.front().implementation_id = "scan.index.v1";
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "unavailable heap registration was accepted");
  mutated = baseline;
  mutated.physical_dag.root_physical_node_id = 999;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "wrong heap physical root was accepted");

  mutated = baseline;
  ++mutated.physical_dag.local_transaction_id;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "mismatched heap dispatch transaction was accepted");
  mutated = baseline;
  ++mutated.physical_dag.statement_snapshot_id;
  RequireAtomicDispatchFailure(
      exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated),
      "mismatched heap dispatch snapshot was accepted");

  mutated = baseline;
  mutated.cancellation_requested = [] { return true; };
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(result,
                               "pre-dispatch cancellation was ignored");
  Require(!result.execution_started && !result.data_access_observed,
          "pre-dispatch cancellation reported a physical read");

  auto denied_context = reader;
  denied_context.authorization_context.grants.front().deny = true;
  mutated = baseline;
  mutated.context = &denied_context;
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(result,
                               "denied heap dispatch published a result");
  Require(result.execution_started && !result.data_access_observed,
          "pre-read callback refusal retained generic read truth");

  std::size_t cancellation_probes = 0;
  mutated = baseline;
  mutated.cancellation_requested = [&] {
    return ++cancellation_probes >= 6;
  };
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(mutated);
  RequireAtomicDispatchFailure(result,
                               "mid-dispatch cancellation published a root");
  Require(result.execution_started,
          "mid-dispatch cancellation did not enter the engine callback");

  auto malformed_context = reader;
  malformed_context.authorization_context.grants.front()
      .target_uuid.canonical = fixture.malformed_table_uuid;
  auto malformed_acquisition = RequestFor(malformed_context,
                                          fixture.malformed_table_uuid,
                                          fixture.salt + 287);
  result = exec::ExecuteCanonicalHeapPhysicalDagDispatch(
      DispatchRequestFor(malformed_acquisition));
  RequireAtomicDispatchFailure(
      result, "malformed later heap row published a partial dispatch root");
  Require(result.execution_started && result.data_access_observed,
          "post-read heap refusal lost actual data-access truth");
  ReleaseRequest(&malformed_acquisition);

  ReleaseRequest(&acquisition);
  Rollback(reader);
}

exec::CanonicalResultPublicationResult PublishDescriptorCarrier(
    const std::string& encoded_descriptor,
    const bool physical_nullable,
    const exec::CanonicalResultNullability published_nullability,
    const platform::u64 salt) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      NewUuidText(platform::UuidKind::object, salt + 1);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor = encoded_descriptor;
  exec::CanonicalResultPublicationRequest request;
  request.statement_uuid =
      NewUuidText(platform::UuidKind::object, salt + 2);
  request.execution_attempt_uuid =
      NewUuidText(platform::UuidKind::object, salt + 3);
  request.transaction_effect_evidence_uuid =
      NewUuidText(platform::UuidKind::object, salt + 4);
  request.physical_output_batch.columns.push_back(
      {"value", descriptor, physical_nullable, 1});
  exec::CanonicalResultColumnDescriptor published;
  published.ordinal = 0;
  published.name_utf8 = "value";
  published.descriptor_uuid = descriptor.descriptor_uuid.canonical;
  published.type_uuid = std::string(kTypeUuid);
  published.nullability = published_nullability;
  request.column_bindings.push_back({0, true, published});
  return exec::PublishCanonicalResultEnvelope(request);
}

void ValidateStorageNullabilityCarrierMatrix(Fixture& fixture) {
  const std::string prefix =
      "canonical=int64;type_uuid=" + std::string(kTypeUuid) + ";";
  auto result = PublishDescriptorCarrier(
      prefix + "nullable=true", true,
      exec::CanonicalResultNullability::kNullable, fixture.salt + 400);
  Require(result.diagnostic.ok && result.published,
          "persisted nullable=true carrier was refused");
  result = PublishDescriptorCarrier(
      prefix + "nullable=false", false,
      exec::CanonicalResultNullability::kNonNull, fixture.salt + 410);
  Require(result.diagnostic.ok && result.published,
          "persisted nullable=false carrier was refused");
  result = PublishDescriptorCarrier(
      prefix + "nullability=nullable", true,
      exec::CanonicalResultNullability::kNullable, fixture.salt + 420);
  Require(result.diagnostic.ok && result.published,
          "canonical nullability carrier regressed");
  result = PublishDescriptorCarrier(
      prefix + "nullability=nullable;nullable=true", true,
      exec::CanonicalResultNullability::kNullable, fixture.salt + 430);
  Require(result.diagnostic.ok && result.published,
          "agreeing nullability carriers were refused");
  result = PublishDescriptorCarrier(
      prefix + "nullability=unknown", true,
      exec::CanonicalResultNullability::kUnknown, fixture.salt + 435);
  Require(result.diagnostic.ok && result.published,
          "canonical unknown nullability spelling regressed");

  const std::vector<std::string> refused{
      "canonical=int64;type_uuid=" + std::string(kTypeUuid),
      prefix + "nullable=True",
      prefix + "nullable=true;nullable=true",
      prefix + "nullability=nullable;nullable=false",
      prefix + "nullability=indeterminate"};
  for (std::size_t index = 0; index < refused.size(); ++index) {
    result = PublishDescriptorCarrier(
        refused[index], true, exec::CanonicalResultNullability::kNullable,
        fixture.salt + 440 + index * 10);
    Require(!result.diagnostic.ok && !result.published &&
                result.canonical_envelope_bytes.empty() &&
                result.delivery_records.empty(),
            "missing, malformed, duplicate, contradictory, or unknown "
            "nullability carrier was accepted");
  }
}

// QOW-TEST-QRY-004-HEAP-RESULT-V1
void ValidateOptimizerSelectedHeapResultMatrix(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-result-reader"),
                             fixture.main_table_uuid,
                             fixture.salt + 500);
  auto acquisition = RequestFor(reader, fixture.main_table_uuid,
                                fixture.salt + 510);
  const auto persisted =
      api::LoadMgaRelationStorageDescriptor(reader, fixture.main_table_uuid);
  Require(persisted.ok && persisted.descriptor.columns.size() == 1,
          "result fixture descriptor load failed");
  auto request = SelectedRequestFor(acquisition, fixture.salt + 520);
  const auto first = api::ExecuteCanonicalHeapOptimizerSelectedDag(request);
  Require(first.accepted && first.exact_selected_nodes_executed &&
              first.causal_counters_attached &&
              first.canonical_result_published &&
              first.data_access_observed && !first.replan_required &&
              first.issues.empty() && first.dispatch.diagnostic.ok &&
              first.dispatch.executed_steps.size() == 1 &&
              first.dispatch.data_access_observed &&
              first.dispatch.executed_steps.front()
                  .data_access_observation_known &&
              first.dispatch.executed_steps.front().data_access_observed &&
              first.runtime_actuals.accepted &&
              first.runtime_actuals.post_execution_actuals &&
              first.runtime_actuals.data_access_observed &&
              first.runtime_actuals.node_actuals.size() == 1,
          "actual database heap scan did not close the selected execution spine");
  const auto& publication = first.result_publication;
  Require(publication.diagnostic.ok && publication.published &&
              publication.envelope.statement_uuid ==
                  reader.statement_uuid.canonical &&
              publication.envelope.execution_attempt_uuid ==
                  request.execution_attempt_uuid &&
              publication.envelope.column_descriptors.size() == 1 &&
              publication.envelope.column_descriptors.front().ordinal == 0 &&
              publication.envelope.column_descriptors.front().name_utf8 ==
                  "value" &&
              publication.envelope.column_descriptors.front()
                      .descriptor_uuid ==
                  persisted.descriptor.columns.front()
                      .value_descriptor.descriptor_uuid.canonical &&
              publication.envelope.column_descriptors.front().type_uuid ==
                  kTypeUuid &&
              publication.envelope.column_descriptors.front().nullability ==
                  exec::CanonicalResultNullability::kNullable &&
              publication.envelope.row_count == 3 &&
              publication.row_stream.columns.size() == 1 &&
              publication.row_stream.rows.size() == 3 &&
              publication.delivery_records.size() == 4 &&
              publication.delivery_records.front().kind ==
                  exec::CanonicalResultDeliveryKind::kMetadata &&
              !publication.canonical_envelope_bytes.empty(),
          "selected heap result lost derived metadata, rows, or delivery order");
  for (std::size_t index = 1; index < publication.delivery_records.size();
       ++index) {
    Require(publication.delivery_records[index].kind ==
                exec::CanonicalResultDeliveryKind::kRow &&
                publication.delivery_records[index].row_ordinal == index - 1,
            "selected result did not deliver metadata before ordered rows");
  }
  Require(PreservesPersistedDescriptorFields(
              publication.row_stream.columns.front().descriptor,
              persisted.descriptor.columns.front().value_descriptor) &&
              publication.row_stream.columns.front().descriptor
                      .encoded_descriptor.find("nullable=true") !=
                  std::string::npos &&
              publication.row_stream.columns.front().descriptor
                      .encoded_descriptor.find("nullability=") ==
                  std::string::npos,
          "selected result rewrote the persisted boolean nullability carrier");
  std::size_t null_count = 0;
  for (const auto& row : publication.row_stream.rows) {
    if (row.values.front().state == api::EngineValueState::sql_null) {
      ++null_count;
    }
  }
  Require(null_count == 1, "selected result lost the persisted SQL NULL");

  const auto repeated =
      api::ExecuteCanonicalHeapOptimizerSelectedDag(request);
  Require(repeated.accepted &&
              repeated.result_publication.canonical_envelope_bytes ==
                  publication.canonical_envelope_bytes &&
              repeated.result_publication.row_stream.rows.size() == 3,
          "same-snapshot selected result bytes were unstable");
  ReleaseRequest(&acquisition);
  Rollback(reader);

  auto empty_reader = QueryContext(Begin(fixture, "qow-heap-result-empty"),
                                   fixture.empty_table_uuid,
                                   fixture.salt + 540);
  auto empty_acquisition = RequestFor(empty_reader, fixture.empty_table_uuid,
                                      fixture.salt + 550);
  const auto empty = api::ExecuteCanonicalHeapOptimizerSelectedDag(
      SelectedRequestFor(empty_acquisition, fixture.salt + 560));
  Require(empty.accepted && empty.canonical_result_published &&
              !empty.data_access_observed &&
              !empty.dispatch.data_access_observed &&
              empty.dispatch.executed_steps.size() == 1 &&
              empty.dispatch.executed_steps.front()
                  .data_access_observation_known &&
              !empty.dispatch.executed_steps.front().data_access_observed &&
              empty.runtime_actuals.accepted &&
              !empty.runtime_actuals.data_access_observed &&
              empty.result_publication.envelope.column_descriptors.size() == 1 &&
              empty.result_publication.envelope.row_count == 0 &&
              empty.result_publication.row_stream.columns.size() == 1 &&
              empty.result_publication.row_stream.rows.empty() &&
              empty.result_publication.delivery_records.size() == 1 &&
              empty.result_publication.delivery_records.front().kind ==
                  exec::CanonicalResultDeliveryKind::kMetadata,
          "typed empty heap result did not retain completed zero-read truth");
  ReleaseRequest(&empty_acquisition);
  Rollback(empty_reader);
}

// QOW-TEST-QRY-004-HEAP-FULL-WIDTH-V1
void ValidateFullWidthHeapMatrix(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-full-width-reader"),
                             fixture.full_width_table_uuid,
                             fixture.salt + 700);
  auto acquisition = RequestFor(reader, fixture.full_width_table_uuid,
                                fixture.salt + 710);
  const auto persisted = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.full_width_table_uuid);
  Require(persisted.ok && persisted.descriptor.columns.size() == 2 &&
              !persisted.descriptor.columns[0].nullable &&
              persisted.descriptor.columns[1].nullable,
          "mixed-nullability full-width descriptor was not persisted");

  const auto acquired =
      exec::ExecuteCanonicalHeapRelationAcquisition(acquisition);
  Require(acquired.diagnostic.ok && acquired.data_access_observed &&
              acquired.output_batch.columns.size() == 2 &&
              acquired.output_batch.rows.size() == 2 &&
              acquired.counters.emitted_row_count == 2 &&
              acquired.counters.output_column_count == 2 &&
              acquired.counters.materialized_cell_count == 4 &&
              acquired.column_uuids ==
                  std::vector<std::string>{
                      persisted.descriptor.columns[0].column_uuid.canonical,
                      persisted.descriptor.columns[1].column_uuid.canonical},
          "full-width acquisition lost exact shape or counter evidence");
  for (std::size_t ordinal = 0; ordinal < 2; ++ordinal) {
    Require(acquired.output_batch.columns[ordinal].stable_name ==
                persisted.descriptor.columns[ordinal].canonical_name_key &&
                acquired.output_batch.columns[ordinal].descriptor_id ==
                    ordinal + 1 &&
                PreservesPersistedDescriptorFields(
                    acquired.output_batch.columns[ordinal].descriptor,
                    persisted.descriptor.columns[ordinal].value_descriptor),
            "full-width acquisition changed ordered persisted metadata");
  }
  Require(!acquired.output_batch.rows[0].values[0].isSqlNull() &&
              acquired.output_batch.rows[0].values[1].isSqlNull() &&
              !acquired.output_batch.rows[1].values[0].isSqlNull() &&
              !acquired.output_batch.rows[1].values[1].isSqlNull(),
          "typed NULL outside ordinal zero was not preserved");

  const auto dispatched = exec::ExecuteCanonicalHeapPhysicalDagDispatch(
      DispatchRequestFor(acquisition));
  Require(dispatched.diagnostic.ok && dispatched.execution_started &&
              dispatched.data_access_observed &&
              dispatched.executed_steps.size() == 1 &&
              dispatched.root_output_descriptor_ids ==
                  std::vector<std::uint32_t>{1, 2} &&
              dispatched.executed_steps.front().heap_read_counters.has_value() &&
              dispatched.executed_steps.front()
                      .heap_read_counters->output_column_count == 2 &&
              dispatched.executed_steps.front()
                      .heap_read_counters->materialized_cell_count == 4 &&
              dispatched.executed_steps.front()
                  .materialized_output_batch.has_value() &&
              dispatched.executed_steps.front()
                      .materialized_output_batch->columns.size() == 2,
          "full-width dispatch lost shape, batch, or counter evidence");

  const auto selected_request =
      SelectedRequestFor(acquisition, fixture.salt + 720);
  const auto selected =
      api::ExecuteCanonicalHeapOptimizerSelectedDag(selected_request);
  Require(selected.accepted && selected.canonical_result_published &&
              selected.data_access_observed && selected.issues.empty() &&
              selected.result_publication.envelope.column_descriptors.size() ==
                  2 &&
              selected.result_publication.envelope.row_count == 2 &&
              selected.result_publication.row_stream.columns.size() == 2 &&
              selected.result_publication.row_stream.rows.size() == 2 &&
              selected.result_publication.delivery_records.size() == 3 &&
              selected.result_publication.delivery_records.front().kind ==
                  exec::CanonicalResultDeliveryKind::kMetadata &&
              selected.result_publication.delivery_records[1].kind ==
                  exec::CanonicalResultDeliveryKind::kRow &&
              selected.result_publication.delivery_records[2].kind ==
                  exec::CanonicalResultDeliveryKind::kRow,
          "full-width selected route lost metadata-first publication");
  for (std::size_t ordinal = 0; ordinal < 2; ++ordinal) {
    const auto& metadata =
        selected.result_publication.envelope.column_descriptors[ordinal];
    Require(metadata.ordinal == ordinal &&
                metadata.name_utf8 ==
                    persisted.descriptor.columns[ordinal].canonical_name_key &&
                metadata.descriptor_uuid == persisted.descriptor.columns[ordinal]
                                                .value_descriptor
                                                .descriptor_uuid.canonical &&
                metadata.type_uuid == kTypeUuid,
            "selected full-width metadata is not in persisted ordinal order");
  }
  const auto repeated =
      api::ExecuteCanonicalHeapOptimizerSelectedDag(selected_request);
  Require(repeated.accepted &&
              repeated.result_publication.canonical_envelope_bytes ==
                  selected.result_publication.canonical_envelope_bytes &&
              repeated.dispatch.root_output_descriptor_ids ==
                  selected.dispatch.root_output_descriptor_ids,
          "full-width result identity or envelope bytes were unstable");
  ReleaseRequest(&acquisition);
  Rollback(reader);

  auto empty_reader = QueryContext(
      Begin(fixture, "qow-heap-empty-full-width-reader"),
      fixture.empty_full_width_table_uuid, fixture.salt + 730);
  auto empty_acquisition = RequestFor(empty_reader,
                                      fixture.empty_full_width_table_uuid,
                                      fixture.salt + 740);
  const auto empty = api::ExecuteCanonicalHeapOptimizerSelectedDag(
      SelectedRequestFor(empty_acquisition, fixture.salt + 750));
  Require(empty.accepted && empty.canonical_result_published &&
              !empty.data_access_observed &&
              empty.result_publication.envelope.column_descriptors.size() == 2 &&
              empty.result_publication.envelope.row_count == 0 &&
              empty.result_publication.row_stream.columns.size() == 2 &&
              empty.result_publication.row_stream.rows.empty() &&
              empty.result_publication.delivery_records.size() == 1 &&
              empty.dispatch.executed_steps.front()
                      .heap_read_counters->output_column_count == 2 &&
              empty.dispatch.executed_steps.front()
                      .heap_read_counters->materialized_cell_count == 0,
          "empty full-width relation did not publish typed metadata only");
  ReleaseRequest(&empty_acquisition);
  Rollback(empty_reader);
}

void ValidateFullWidthHeapRefusals(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-full-width-refusals"),
                             fixture.full_width_table_uuid,
                             fixture.salt + 760);
  auto acquisition = RequestFor(reader, fixture.full_width_table_uuid,
                                fixture.salt + 770);
  const auto baseline = SelectedRequestFor(acquisition, fixture.salt + 780);
  const auto require_pre_read_refusal = [&](const auto& candidate,
                                             std::string_view detail) {
    const auto refused =
        api::ExecuteCanonicalHeapOptimizerSelectedDag(candidate);
    RequireAtomicSelectedFailure(refused, detail);
    Require(!refused.data_access_observed,
            "full-width pre-read refusal reported data access");
  };
  const auto require_post_read_refusal = [&](const auto& candidate,
                                              std::string_view detail) {
    const auto refused =
        api::ExecuteCanonicalHeapOptimizerSelectedDag(candidate);
    RequireAtomicSelectedFailure(refused, detail);
    Require(refused.data_access_observed && !refused.issues.empty() &&
                refused.dispatch.executed_steps.empty(),
            "full-width post-read refusal lost atomic read evidence");
  };

  auto mutated = baseline;
  mutated.maximum_output_columns = 0;
  require_pre_read_refusal(mutated, "zero column bound was accepted");
  mutated = baseline;
  mutated.maximum_output_columns = 1;
  require_pre_read_refusal(mutated, "too-small column bound was accepted");
  mutated = baseline;
  mutated.maximum_output_columns = 4097;
  require_pre_read_refusal(mutated, "above-ceiling column bound was accepted");
  mutated = baseline;
  mutated.maximum_output_cells = 0;
  require_pre_read_refusal(mutated, "zero cell bound was accepted");
  mutated = baseline;
  mutated.maximum_output_cells = 3;
  const auto exhausted_cells =
      api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(exhausted_cells,
                               "too-small cell bound was accepted");
  Require(exhausted_cells.data_access_observed &&
              !exhausted_cells.issues.empty() &&
              exhausted_cells.issues.front().diagnostic_id ==
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
              exhausted_cells.issues.front().field_id ==
                  "heap materialized cell bound would be exceeded" &&
              exhausted_cells.dispatch.executed_steps.empty(),
          "cell exhaustion used the wrong post-read atomic refusal path");

  mutated = baseline;
  mutated.relational_dag.outputs.push_back(mutated.relational_dag.outputs[1]);
  mutated.relational_dag.outputs.back().output_id = 3;
  mutated.relational_dag.outputs.back().ordinal = 2;
  require_pre_read_refusal(mutated, "extra output ID was accepted");
  mutated = baseline;
  mutated.relational_dag.outputs.pop_back();
  require_pre_read_refusal(mutated, "missing output ID was accepted");
  mutated = baseline;
  mutated.relational_dag.outputs[1].output_id = 1;
  require_pre_read_refusal(mutated, "duplicate output ID was accepted");
  mutated = baseline;
  std::swap(mutated.relational_dag.outputs[0],
            mutated.relational_dag.outputs[1]);
  require_pre_read_refusal(mutated, "reordered output IDs were accepted");

  mutated = baseline;
  mutated.relational_dag.expressions.push_back(
      mutated.relational_dag.expressions[1]);
  mutated.relational_dag.expressions.back().expression_id = 3;
  require_pre_read_refusal(mutated, "extra expression ID was accepted");
  mutated = baseline;
  mutated.relational_dag.expressions.pop_back();
  require_pre_read_refusal(mutated, "missing expression ID was accepted");
  mutated = baseline;
  mutated.relational_dag.expressions[1].expression_id = 1;
  require_pre_read_refusal(mutated, "duplicate expression ID was accepted");
  mutated = baseline;
  std::swap(mutated.relational_dag.expressions[0],
            mutated.relational_dag.expressions[1]);
  require_pre_read_refusal(mutated, "reordered expression IDs were accepted");

  mutated = baseline;
  mutated.relational_dag.descriptors.push_back(
      mutated.relational_dag.descriptors[1]);
  mutated.relational_dag.descriptors.back().descriptor_id = 3;
  require_pre_read_refusal(mutated, "extra descriptor ID was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors.pop_back();
  require_pre_read_refusal(mutated, "missing descriptor ID was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors[1].descriptor_id = 1;
  require_pre_read_refusal(mutated, "duplicate descriptor ID was accepted");
  mutated = baseline;
  std::swap(mutated.relational_dag.descriptors[0],
            mutated.relational_dag.descriptors[1]);
  require_pre_read_refusal(mutated, "reordered descriptor IDs were accepted");

  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().output_descriptor_ids.push_back(3);
  require_pre_read_refusal(mutated, "extra physical output ID was accepted");
  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().output_descriptor_ids.pop_back();
  require_pre_read_refusal(mutated, "missing physical output ID was accepted");
  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().output_descriptor_ids = {1, 1};
  require_pre_read_refusal(mutated,
                           "duplicate physical output ID was accepted");
  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().output_descriptor_ids = {2, 1};
  require_pre_read_refusal(mutated,
                           "reordered physical output IDs were accepted");

  mutated = baseline;
  std::swap(mutated.relational_dag.expressions[0].bound_name_uuid,
            mutated.relational_dag.expressions[1].bound_name_uuid);
  require_post_read_refusal(mutated, "swapped bound column UUIDs were accepted");
  mutated = baseline;
  std::swap(mutated.relational_dag.outputs[0].output_name_utf8,
            mutated.relational_dag.outputs[1].output_name_utf8);
  std::swap(mutated.relational_dag.expressions[0].bound_name_uuid,
            mutated.relational_dag.expressions[1].bound_name_uuid);
  std::swap(mutated.relational_dag.descriptors[0].descriptor_uuid,
            mutated.relational_dag.descriptors[1].descriptor_uuid);
  std::swap(mutated.relational_dag.descriptors[0].nullability,
            mutated.relational_dag.descriptors[1].nullability);
  require_post_read_refusal(mutated,
                            "persisted ordinal binding drift was accepted");
  mutated = baseline;
  mutated.relational_dag.outputs[1].output_name_utf8 = "drifted_name";
  require_post_read_refusal(mutated, "persisted name drift was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors[1].descriptor_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 789);
  require_post_read_refusal(mutated,
                            "persisted value-descriptor UUID drift was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors[1].type_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 790);
  require_post_read_refusal(mutated, "persisted type drift was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors[1].nullability =
      api::RelationalNullability::kNonNull;
  require_post_read_refusal(mutated,
                            "persisted nullability drift was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors[1].collation_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 791);
  require_post_read_refusal(mutated, "persisted collation drift was accepted");
  mutated = baseline;
  mutated.relational_dag.descriptors[1].timezone_profile_id = "UTC";
  require_post_read_refusal(mutated, "persisted timezone drift was accepted");

  auto overflow_context = reader;
  overflow_context.optimizer_maximum_candidate_count =
      std::numeric_limits<std::size_t>::max();
  auto overflow = acquisition;
  overflow.context = &overflow_context;
  overflow.maximum_output_rows = std::numeric_limits<std::size_t>::max();
  const auto overflow_result =
      exec::ExecuteCanonicalHeapRelationAcquisition(overflow);
  RequireAtomicFailure(overflow_result,
                       "checked row-by-width overflow was accepted");
  Require(!overflow_result.data_access_observed &&
              overflow_result.diagnostic.diagnostic_code ==
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
              overflow_result.diagnostic.detail ==
                  "admitted heap row-by-width shape overflows size_t",
          "checked row-by-width overflow used the wrong refusal path");

  ReleaseRequest(&acquisition);
  Rollback(reader);

  const std::vector<std::pair<std::string, std::string>> malformed_relations{
      {fixture.missing_later_column_table_uuid, "missing"},
      {fixture.duplicate_later_column_table_uuid, "duplicate"},
      {fixture.malformed_later_column_table_uuid, "malformed"}};
  for (std::size_t index = 0; index < malformed_relations.size(); ++index) {
    const auto& [relation_uuid, category] = malformed_relations[index];
    auto malformed_context = QueryContext(
        Begin(fixture, "qow-heap-full-width-" + category), relation_uuid,
        fixture.salt + 800 + index * 20);
    auto malformed_acquisition = RequestFor(
        malformed_context, relation_uuid, fixture.salt + 810 + index * 20);
    const auto refused = api::ExecuteCanonicalHeapOptimizerSelectedDag(
        SelectedRequestFor(malformed_acquisition,
                           fixture.salt + 820 + index * 20));
    RequireAtomicSelectedFailure(
        refused, "malformed later-column value published a partial result");
    Require(refused.data_access_observed && !refused.issues.empty(),
            "later-column refusal lost known physical read evidence");
    ReleaseRequest(&malformed_acquisition);
    Rollback(malformed_context);
  }
}

void ValidateOptimizerSelectedHeapResultRefusals(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-result-refusals"),
                             fixture.main_table_uuid,
                             fixture.salt + 600);
  auto acquisition = RequestFor(reader, fixture.main_table_uuid,
                                fixture.salt + 610);
  const auto baseline = SelectedRequestFor(acquisition, fixture.salt + 620);

  const auto require_preflight_refusal = [&](const auto& request,
                                              std::string_view detail) {
    const auto refused =
        api::ExecuteCanonicalHeapOptimizerSelectedDag(request);
    RequireAtomicSelectedFailure(refused, detail);
    Require(!refused.data_access_observed,
            "selected preflight refusal reported heap data");
  };

  auto mutated = baseline;
  mutated.context.statement_uuid.canonical.clear();
  require_preflight_refusal(mutated,
                            "missing context statement UUID was accepted");

  mutated = baseline;
  mutated.execution_attempt_uuid.clear();
  auto result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "missing execution attempt UUID was accepted");
  Require(!result.data_access_observed,
          "missing execution attempt UUID reached heap data");

  mutated = baseline;
  mutated.transaction_effect_evidence_uuid = "NOT-A-UUID";
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(
      result, "malformed transaction-effect evidence UUID was accepted");
  Require(!result.data_access_observed,
          "malformed transaction-effect UUID reached heap data");

  mutated = baseline;
  mutated.execution_attempt_uuid = mutated.context.statement_uuid.canonical;
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "equal statement/execution UUIDs were accepted");
  Require(!result.data_access_observed,
          "equal statement/execution UUIDs reached heap data");

  mutated = baseline;
  mutated.transaction_effect_evidence_uuid = mutated.execution_attempt_uuid;
  require_preflight_refusal(mutated,
                            "equal execution/effect UUIDs were accepted");

  mutated = baseline;
  mutated.maximum_scanned_row_versions = 0;
  require_preflight_refusal(mutated,
                            "zero selected scanned-row bound was accepted");
  mutated = baseline;
  mutated.maximum_decoded_bytes = 0;
  require_preflight_refusal(mutated,
                            "zero selected decoded-byte bound was accepted");
  mutated = baseline;
  mutated.maximum_output_rows = 0;
  require_preflight_refusal(mutated,
                            "zero selected output-row bound was accepted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().visible = false;
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "hidden sole result binding was accepted");
  Require(!result.data_access_observed,
          "hidden result binding reached heap data");

  mutated = baseline;
  mutated.relational_dag.outputs.front().ordinal = 1;
  require_preflight_refusal(mutated,
                            "nonzero sole result ordinal was accepted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().descriptor_id = 2;
  require_preflight_refusal(mutated,
                            "result descriptor identity drift was accepted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().expression_id = 2;
  require_preflight_refusal(mutated,
                            "result expression identity drift was accepted");

  mutated = baseline;
  mutated.relational_dag.expressions.front().bound_name_uuid = "bad-uuid";
  require_preflight_refusal(mutated,
                            "bound column UUID drift was accepted");

  mutated = baseline;
  mutated.relational_dag.descriptors.front().descriptor_uuid = "bad-uuid";
  require_preflight_refusal(mutated,
                            "result descriptor UUID drift was accepted");

  mutated = baseline;
  mutated.relational_dag.expressions.front().child_expression_ids = {1};
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "non-leaf identifier result was accepted");
  Require(!result.data_access_observed,
          "non-leaf identifier result reached heap data");

  mutated = baseline;
  auto extra_output = mutated.relational_dag.outputs.front();
  extra_output.output_id = 2;
  extra_output.ordinal = 1;
  mutated.relational_dag.outputs.push_back(extra_output);
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "multiple result bindings entered heap execution");
  Require(!result.data_access_observed,
          "multiple result bindings reached heap data");

  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().implementation_id =
      "scan.index.v1";
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "non-heap implementation entered result route");
  Require(!result.data_access_observed,
          "non-heap implementation reached heap data");

  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().executor_capability_uuid.clear();
  require_preflight_refusal(mutated,
                            "missing selected capability UUID was accepted");
  mutated = baseline;
  mutated.selected_physical_dag.nodes.front()
      .executor_capability_abi_version = 2;
  require_preflight_refusal(mutated,
                            "wrong selected capability ABI was accepted");
  mutated = baseline;
  mutated.selected_physical_dag.nodes.front().node_kind =
      exec::PhysicalNodeKind::kFilter;
  require_preflight_refusal(mutated,
                            "wrong selected physical kind was accepted");
  mutated = baseline;
  mutated.selected_physical_dag.root_physical_node_id = 999;
  require_preflight_refusal(mutated,
                            "wrong selected physical root was accepted");
  mutated = baseline;
  ++mutated.selected_physical_dag.local_transaction_id;
  require_preflight_refusal(mutated,
                            "selected transaction drift was accepted");
  mutated = baseline;
  ++mutated.selected_physical_dag.statement_snapshot_id;
  require_preflight_refusal(mutated,
                            "selected statement snapshot drift was accepted");

  mutated = baseline;
  mutated.cancellation_requested = [] { return true; };
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "pre-registration cancellation was ignored");
  Require(!result.data_access_observed,
          "pre-registration cancellation reported heap data");

  mutated = baseline;
  mutated.context.authorization_context.grants.front().deny = true;
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(result,
                               "denied selected heap scan published a result");
  Require(!result.data_access_observed && !result.issues.empty(),
          "pre-read selected refusal retained callback-started read truth");

  mutated = baseline;
  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 630);
  policy.subject_uuid = mutated.context.principal_uuid;
  policy.subject_kind = "principal";
  policy.target_uuid.canonical = fixture.main_table_uuid;
  policy.right = "SELECT";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = mutated.context.authorization_context.policy_epoch;
  mutated.context.authorization_context.policies.push_back(std::move(policy));
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(
      result, "indeterminate selected authorization published a result");
  Require(!result.data_access_observed,
          "indeterminate selected authorization reported heap data");

  mutated = baseline;
  mutated.maximum_output_rows = 1;
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(
      result, "post-read output-bound refusal published a partial result");
  Require(result.data_access_observed && !result.issues.empty() &&
              result.dispatch.executed_steps.empty(),
          "selected refusal did not preserve top-level known read truth "
          "while withholding partial dispatch details");

  mutated = baseline;
  mutated.relational_dag.outputs.front().output_name_utf8 = "wrong_name";
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(mutated);
  RequireAtomicSelectedFailure(
      result, "result name disagreement published an envelope");

  ReleaseRequest(&acquisition);
  Rollback(reader);

  auto malformed_context = QueryContext(
      Begin(fixture, "qow-heap-result-malformed"),
      fixture.malformed_table_uuid, fixture.salt + 640);
  auto malformed_acquisition = RequestFor(
      malformed_context, fixture.malformed_table_uuid, fixture.salt + 650);
  result = api::ExecuteCanonicalHeapOptimizerSelectedDag(
      SelectedRequestFor(malformed_acquisition, fixture.salt + 660));
  RequireAtomicSelectedFailure(
      result, "malformed stored row published a selected result");
  Require(result.data_access_observed && !result.issues.empty(),
          "post-read malformed-row refusal lost known read truth");
  ReleaseRequest(&malformed_acquisition);
  Rollback(malformed_context);
}

void ValidateBindingSecurityAndResourceRefusals(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-refusal-reader"),
                             fixture.main_table_uuid,
                             fixture.salt + 300);
  auto baseline = RequestFor(reader,
                             fixture.main_table_uuid,
                             fixture.salt + 320);
  const api::TypedRelationalDag original_relational = *baseline.relational_dag;

  auto mutated = baseline;
  mutated.physical_dag.catalog_generation = 2;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "catalog generation mismatch was accepted");

  mutated = baseline;
  mutated.physical_dag.abi_version = 1;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "non-v2 physical DAG was accepted");

  mutated = baseline;
  mutated.physical_dag.optimizer_published = false;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "non-published optimizer DAG was accepted");

  mutated = baseline;
  mutated.physical_dag.nodes.front().node_kind =
      exec::PhysicalNodeKind::kFilter;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "non-scan physical node was accepted");

  mutated = baseline;
  mutated.physical_dag.nodes.front().input_physical_node_ids = {11};
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "physical input edge entered the leaf scan profile");

  mutated = baseline;
  mutated.physical_dag.nodes.front().implementation_id = "scan.index.v1";
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "wrong scan implementation was accepted");

  mutated = baseline;
  mutated.selected_physical_node_id = 999;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "nonexistent selected physical node was accepted");

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)->wire_version = 1;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "non-v2 relational DAG was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->expressions.front()
      .bound_name_uuid.reset();
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "identifier without bound column UUID was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->nodes.front()
      .node_kind = api::RelationalDagNodeKind::kFilter;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "non-scan relational source was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->nodes.front()
      .input_node_ids = {1};
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "relational input edge entered the leaf scan profile");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->nodes.front()
      .required_object_uuids.clear();
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "missing relation UUID was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->nodes.front()
      .required_object_uuids.push_back(
          NewUuidText(platform::UuidKind::object, fixture.salt + 339));
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "multiple relation UUIDs were accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->outputs.front()
      .ordinal = 1;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "nonzero relation output ordinal was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->outputs.front()
      .output_name_utf8 = "not_value";
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "wrong persisted output name was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->expressions.front()
      .bound_name_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 340);
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "wrong bound column UUID was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->descriptors.front()
      .type_uuid = NewUuidText(platform::UuidKind::object,
                              fixture.salt + 341);
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "wrong bound type UUID was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->descriptors.front()
      .descriptor_uuid = NewUuidText(platform::UuidKind::object,
                                    fixture.salt + 342);
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "wrong bound descriptor UUID was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  auto missing_transaction_context = reader;
  missing_transaction_context.local_transaction_id = 0;
  missing_transaction_context.transaction_uuid.canonical.clear();
  mutated = baseline;
  mutated.context = &missing_transaction_context;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "missing context transaction was accepted");

  mutated = baseline;
  ++mutated.physical_dag.local_transaction_id;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "mismatched physical transaction was accepted");

  mutated = baseline;
  ++mutated.physical_dag.statement_snapshot_id;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "mismatched physical statement snapshot was accepted");

  auto wrong_snapshot_context = reader;
  wrong_snapshot_context.snapshot_visible_through_local_transaction_id += 1;
  mutated = baseline;
  mutated.context = &wrong_snapshot_context;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "mismatched statement snapshot was accepted");

  auto prepared_context = reader;
  prepared_context.prepared_metadata_required_object_uuid.canonical =
      fixture.main_table_uuid;
  prepared_context.prepared_metadata_required_executable_generation = 1;
  prepared_context.prepared_metadata_required_metadata_epoch = 1;
  mutated = baseline;
  mutated.context = &prepared_context;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "prepared descriptor marker was accepted");

  auto missing_authorization = reader;
  missing_authorization.authorization_context.present = false;
  mutated = baseline;
  mutated.context = &missing_authorization;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "missing authorization context was accepted");

  auto missing_grant = reader;
  missing_grant.authorization_context.grants.clear();
  mutated = baseline;
  mutated.context = &missing_grant;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "missing SELECT authorization was accepted");

  auto stale_authorization = reader;
  ++stale_authorization.authorization_context.security_epoch;
  mutated = baseline;
  mutated.context = &stale_authorization;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "stale authorization snapshot was accepted");

  auto denied_context = reader;
  denied_context.authorization_context.grants.front().deny = true;
  mutated = baseline;
  mutated.context = &denied_context;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "explicit SELECT denial was accepted");

  auto wrong_principal = reader;
  wrong_principal.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 343);
  mutated = baseline;
  mutated.context = &wrong_principal;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "authorization principal mismatch was accepted");

  auto policy_context = reader;
  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 344);
  policy.subject_uuid = policy_context.principal_uuid;
  policy.subject_kind = "principal";
  policy.target_uuid.canonical = fixture.main_table_uuid;
  policy.right = "SELECT";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = policy_context.authorization_context.policy_epoch;
  policy_context.authorization_context.policies.push_back(policy);
  mutated = baseline;
  mutated.context = &policy_context;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "indeterminate runtime policy recheck was accepted");

  const std::string invisible_relation_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 345);
  auto invisible_context = reader;
  invisible_context.authorization_context.grants.front()
      .target_uuid.canonical = invisible_relation_uuid;
  mutated = baseline;
  mutated.context = &invisible_context;
  const_cast<api::TypedRelationalDag*>(mutated.relational_dag)
      ->nodes.front()
      .required_object_uuids = {invisible_relation_uuid};
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "nonexistent authorized relation was accepted");
  *const_cast<api::TypedRelationalDag*>(mutated.relational_dag) =
      original_relational;

  mutated = baseline;
  mutated.maximum_scanned_row_versions = 0;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "zero row-version bound was accepted");
  mutated = baseline;
  mutated.maximum_decoded_bytes = 0;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "zero decoded-byte bound was accepted");
  mutated = baseline;
  mutated.maximum_output_rows = 0;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "zero output-row bound was accepted");
  mutated = baseline;
  mutated.maximum_output_columns = 0;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "zero output-column bound was accepted");
  mutated = baseline;
  mutated.maximum_output_cells = 0;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "zero output-cell bound was accepted");

  mutated = baseline;
  mutated.maximum_scanned_row_versions = 1;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "row-version bound was not enforced atomically");
  mutated = baseline;
  mutated.maximum_decoded_bytes = 1;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "decoded-byte bound was not enforced before read");
  mutated = baseline;
  mutated.maximum_output_rows = 1;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(mutated),
                       "output-row bound was not enforced atomically");

  std::size_t cancellation_probes = 0;
  mutated = baseline;
  mutated.cancellation_requested = [&] {
    return ++cancellation_probes >= 5;
  };
  const auto cancelled =
      exec::ExecuteCanonicalHeapRelationAcquisition(mutated);
  RequireAtomicFailure(cancelled, "mid-read cancellation published rows");
  Require(cancelled.cancellation_observed,
          "mid-read cancellation did not return cancellation evidence");

  auto temporary_context = reader;
  temporary_context.authorization_context.grants.front()
      .target_uuid.canonical = fixture.temporary_table_uuid;
  auto temporary = RequestFor(temporary_context,
                              fixture.temporary_table_uuid,
                              fixture.salt + 360);
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(temporary),
                       "temporary relation entered ordinary heap profile");
  auto other_session_context = temporary_context;
  other_session_context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 361);
  temporary.context = &other_session_context;
  RequireAtomicFailure(exec::ExecuteCanonicalHeapRelationAcquisition(temporary),
                       "other session acquired the temporary relation");
  ReleaseRequest(&temporary);

  auto malformed_context = reader;
  malformed_context.authorization_context.grants.front()
      .target_uuid.canonical = fixture.malformed_table_uuid;
  auto malformed = RequestFor(malformed_context,
                              fixture.malformed_table_uuid,
                              fixture.salt + 380);
  const auto malformed_result =
      exec::ExecuteCanonicalHeapRelationAcquisition(malformed);
  RequireAtomicFailure(malformed_result,
                       "malformed later stored row produced a partial batch");
  ReleaseRequest(&malformed);

  ReleaseRequest(&baseline);
  Rollback(reader);
}

// QOW-TEST-QRY-004-HEAP-OPTIMIZER-ADMISSION-V1
void ValidateHeapOptimizerAdmissionMatrix(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-admission-reader"),
                             fixture.full_width_table_uuid,
                             fixture.salt + 3300);
  auto acquisition = RequestFor(reader, fixture.full_width_table_uuid,
                                fixture.salt + 3320);
  const auto request = AdmissionRequestFor(acquisition);
  const auto before = CaptureFixtureFiles(fixture.directory);
  const auto result =
      api::BuildCanonicalCurrentHeapOptimizerAdmission(request);
  const auto after = CaptureFixtureFiles(fixture.directory);
  const auto persisted = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.full_width_table_uuid);
  Require(persisted.ok && persisted.descriptor.columns.size() == 2,
          "admission fixture descriptor was unavailable");
  if (!result.built) {
    std::cerr << result.issue.diagnostic_id << ':' << result.issue.field_id
              << '\n';
  }
  Require(result.built && result.issue.diagnostic_id.empty() &&
              result.admission.admitted &&
              result.admission.planning_allowed &&
              result.admission.degraded_for_unknown_statistics &&
              !result.admission.benchmark_clean_ready &&
              !result.admission.data_access_allowed &&
              result.admission.issues.empty(),
          "current heap object was not admitted as explicit unknown statistics");
  Require(before == after,
          "successful heap optimizer admission mutated database files");
  Require(result.current_relation_descriptor_uuid ==
                  persisted.descriptor.descriptor_uuid.canonical &&
              result.current_relation_descriptor_generation ==
                  persisted.descriptor.descriptor_generation,
          "admission lost the exact current descriptor identity");
  Require(result.request.logical_graph.nodes.size() == 1 &&
              result.request.logical_graph.nodes.front().required_object_uuids ==
                  std::vector<std::string>{fixture.full_width_table_uuid} &&
              result.request.logical_properties.properties.empty() &&
              result.request.catalog.object_uuids ==
                  std::vector<std::string>{fixture.full_width_table_uuid} &&
              result.request.security.authorized_object_uuids ==
                  std::vector<std::string>{fixture.full_width_table_uuid} &&
              result.request.catalog.descriptor_ids ==
                  std::vector<std::uint32_t>({1, 2}),
          "admission did not preserve exact object/descriptor coverage");
  Require(result.request.catalog.snapshot_uuid ==
                  reader.statement_metadata_snapshot_uuid.canonical &&
              result.request.catalog.catalog_epoch_uuid ==
                  reader.statement_metadata_snapshot_uuid.canonical &&
              result.request.catalog.catalog_generation ==
                  reader.catalog_generation_id &&
              result.request.catalog.engine_owned &&
              result.request.security.security_context_uuid ==
                  reader.authorization_context.authority_uuid.canonical &&
              result.request.security.security_epoch == reader.security_epoch &&
              result.request.security.policy_epoch ==
                  reader.authorization_context.policy_epoch &&
              result.request.security.catalog_generation ==
                  reader.authorization_context.catalog_generation_id &&
              result.request.security.engine_owned &&
              result.request.mga.local_transaction_id ==
                  reader.local_transaction_id &&
              result.request.mga.statement_snapshot_id ==
                  reader.snapshot_visible_through_local_transaction_id &&
              result.request.mga.metadata_snapshot_uuid ==
                  reader.statement_metadata_snapshot_uuid.canonical &&
              result.request.mga.transaction_active &&
              result.request.mga.statement_snapshot_fixed &&
              result.request.mga.engine_owned &&
              !result.request.mga.finality_authority_claimed &&
              result.request.policy_capability.policy_snapshot_uuid ==
                  reader.authorization_context.authority_uuid.canonical &&
              result.request.policy_capability.policy_epoch ==
                  reader.authorization_context.policy_epoch &&
              result.request.policy_capability.capability_snapshot_uuid ==
                  reader.optimizer_capability_snapshot_uuid.canonical &&
              result.request.policy_capability.capability_abi_version == 1 &&
              !result.request.policy_capability.supported_node_kinds.empty() &&
              result.request.policy_capability.engine_owned &&
              !result.request.policy_capability.cluster_capability_claimed &&
              result.request.resource.resource_snapshot_uuid ==
                  reader.optimizer_resource_snapshot_uuid.canonical &&
              result.request.resource.resource_epoch == reader.resource_epoch &&
              result.request.resource.memory_budget_bytes ==
                  reader.optimizer_memory_budget_bytes &&
              result.request.resource.maximum_candidate_count ==
                  reader.optimizer_maximum_candidate_count &&
              result.request.resource.maximum_memo_groups ==
                  reader.optimizer_maximum_memo_groups &&
              result.request.resource.maximum_search_steps ==
                  reader.optimizer_maximum_search_steps &&
              result.request.resource.maximum_planning_time_ns ==
                  reader.optimizer_maximum_planning_time_ns &&
              result.request.resource.spill_allowed ==
                  reader.optimizer_spill_allowed &&
              result.request.resource.engine_owned &&
              result.request.statistics.statistics_snapshot_uuid ==
                  reader.statement_uuid.canonical &&
              result.request.statistics.catalog_epoch_uuid ==
                  reader.statement_metadata_snapshot_uuid.canonical &&
              result.request.statistics.statistics_generation ==
                  reader.catalog_generation_id &&
              result.request.statistics.admitted_at_monotonic_ns == 1 &&
              result.request.route.route_snapshot_uuid ==
                  reader.optimizer_route_snapshot_uuid.canonical &&
              result.request.route.route_epoch ==
                  reader.optimizer_route_epoch &&
              result.request.route.route_generation ==
                  reader.optimizer_route_generation &&
              result.request.route.operation_id == "query.execute" &&
              result.request.route.route_id ==
                  "native.sblr.query.execute.v2" &&
              result.request.route.native_local_route &&
              result.request.route.engine_owned &&
              !result.request.route.cluster_route_claimed &&
              result.request.populated_from_admitted_typed_sblr &&
              !result.request.data_access_observed &&
              !result.request.parser_planning_authority_claimed &&
              result.admission.bound_sblr_tree_uuid ==
                  request.relational_dag.bound_sblr_tree_uuid &&
              result.admission.catalog_epoch_uuid ==
                  reader.statement_metadata_snapshot_uuid.canonical &&
              result.admission.security_context_uuid ==
                  reader.authorization_context.authority_uuid.canonical &&
              result.admission.capability_snapshot_uuid ==
                  reader.optimizer_capability_snapshot_uuid.canonical &&
              result.admission.resource_snapshot_uuid ==
                  reader.optimizer_resource_snapshot_uuid.canonical &&
              result.admission.statistics_snapshot_uuid ==
                  reader.statement_uuid.canonical &&
              result.admission.route_snapshot_uuid ==
                  reader.optimizer_route_snapshot_uuid.canonical &&
              result.admission.local_transaction_id ==
                  reader.local_transaction_id &&
              result.admission.statement_snapshot_id ==
                  reader.snapshot_visible_through_local_transaction_id &&
              result.admission.catalog_generation ==
                  reader.catalog_generation_id &&
              result.admission.security_epoch == reader.security_epoch &&
              result.admission.policy_epoch ==
                  reader.authorization_context.policy_epoch &&
              result.admission.resource_epoch == reader.resource_epoch &&
              result.admission.statistics_generation ==
                  reader.catalog_generation_id &&
              result.admission.route_epoch == reader.optimizer_route_epoch &&
              result.admission.route_generation ==
                  reader.optimizer_route_generation,
          "admission scope identities drifted from engine context");
  Require(result.admission.evidence.size() == 8,
          "admission did not retain all eight ordered stages");
  for (std::size_t index = 0; index < result.admission.evidence.size();
       ++index) {
    Require(result.admission.evidence[index].stage ==
                static_cast<scratchbird::engine::optimizer::
                                CanonicalOptimizerAdmissionStage>(index + 1),
            "optimizer admission stages are out of order");
  }
  Require(result.request.statistics.node_estimates.size() == 1,
          "heap admission did not produce exactly one source estimate");
  const auto& estimate = result.request.statistics.node_estimates.front();
  Require(estimate.logical_node_id == 1 &&
              estimate.object_uuid == fixture.full_width_table_uuid &&
              estimate.state == scratchbird::engine::optimizer::
                                    CanonicalOptimizerStatisticState::kUnknown &&
              estimate.source == scratchbird::engine::optimizer::
                                     CanonicalOptimizerStatisticSource::
                                         kUnavailable &&
              estimate.confidence ==
                  scratchbird::engine::optimizer::CostConfidence::kUnknown &&
              !estimate.row_count_present && !estimate.page_count_present &&
              estimate.row_count == 0 && estimate.page_count == 0 &&
              estimate.collected_at_monotonic_ns == 0 &&
              estimate.maximum_age_ns == 0 &&
              !estimate.derived_from_runtime_actuals &&
              !estimate.benchmark_clean_authority_claimed &&
              estimate.catalog_epoch_uuid ==
                  reader.statement_metadata_snapshot_uuid.canonical &&
              estimate.statistics_snapshot_uuid ==
                  reader.statement_uuid.canonical &&
              estimate.statistics_generation == reader.catalog_generation_id &&
              estimate.admitted_at_monotonic_ns == 1 &&
              result.request.statistics.captured_before_data_access &&
              !result.request.statistics.data_access_observed &&
              !result.request.statistics.runtime_actuals_present &&
              !result.request.statistics.parser_statistics_authority_claimed,
          "heap admission synthesized row/page statistics or later authority");

  auto object_free_graph = result.request.logical_graph;
  for (auto& logical_node : object_free_graph.nodes) {
    logical_node.required_object_uuids.clear();
  }
  opt::CanonicalNativeObjectFreeAdmissionContext object_free_context;
  object_free_context.statement_uuid = reader.statement_uuid.canonical;
  object_free_context.catalog_snapshot_uuid =
      reader.statement_metadata_snapshot_uuid.canonical;
  object_free_context.security_context_uuid =
      reader.authorization_context.authority_uuid.canonical;
  object_free_context.catalog_generation = reader.catalog_generation_id;
  object_free_context.authorization_catalog_generation =
      reader.authorization_context.catalog_generation_id;
  object_free_context.security_epoch = reader.authorization_context.security_epoch;
  object_free_context.policy_epoch = reader.authorization_context.policy_epoch;
  object_free_context.resource_epoch = reader.resource_epoch;
  object_free_context.capability_snapshot_uuid =
      reader.optimizer_capability_snapshot_uuid.canonical;
  object_free_context.resource_snapshot_uuid =
      reader.optimizer_resource_snapshot_uuid.canonical;
  object_free_context.route_snapshot_uuid =
      reader.optimizer_route_snapshot_uuid.canonical;
  object_free_context.route_epoch = reader.optimizer_route_epoch;
  object_free_context.route_generation = reader.optimizer_route_generation;
  object_free_context.memory_budget_bytes =
      reader.optimizer_memory_budget_bytes;
  object_free_context.maximum_candidate_count =
      reader.optimizer_maximum_candidate_count;
  object_free_context.maximum_memo_groups =
      reader.optimizer_maximum_memo_groups;
  object_free_context.maximum_search_steps =
      reader.optimizer_maximum_search_steps;
  object_free_context.maximum_planning_time_ns =
      reader.optimizer_maximum_planning_time_ns;
  object_free_context.spill_allowed = reader.optimizer_spill_allowed;
  object_free_context.local_transaction_id = reader.local_transaction_id;
  object_free_context.statement_snapshot_id =
      reader.snapshot_visible_through_local_transaction_id;
  object_free_context.admitted_at_monotonic_ns = 1;
  object_free_context.metadata_snapshot_engine_owned = true;
  object_free_context.authorization_context_engine_owned = true;
  const auto legacy_object_free =
      opt::BuildCanonicalObjectFreeNativeOptimizerAdmissionRequest(
          object_free_graph, result.request.logical_properties,
          object_free_context);
  opt::CanonicalNativeObjectAdmissionContext empty_object_context;
  static_cast<opt::CanonicalNativeObjectFreeAdmissionContext&>(
      empty_object_context) = object_free_context;
  const auto object_aware_empty =
      opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
          object_free_graph, result.request.logical_properties,
          empty_object_context);
  Require(legacy_object_free.built && object_aware_empty.built &&
              legacy_object_free.admission.admitted &&
              object_aware_empty.admission.admitted &&
              NativeAdmissionFingerprint(legacy_object_free) ==
                  NativeAdmissionFingerprint(object_aware_empty),
          "legacy object-free admission diverged field-for-field from the "
          "empty object-aware profile");

  constexpr std::string_view kCatalogObjectDiagnostic =
      "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-OBJECT-EVIDENCE-V1";
  constexpr std::string_view kCatalogObjectField =
      "required_object_snapshot";
  constexpr std::string_view kSecurityObjectDiagnostic =
      "QOW-DIAG-OPTIMIZER-ADMISSION-SECURITY-OBJECT-EVIDENCE-V1";
  constexpr std::string_view kSecurityObjectField =
      "authorized_object_snapshot";
  const auto require_object_refusal =
      [&](const auto& graph,
          const opt::CanonicalNativeObjectAdmissionContext& context,
          const std::string_view diagnostic_id,
          const std::string_view field_id,
          const std::string_view detail) {
        RequireAtomicObjectAdmissionBuildFailure(
            opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
                graph, result.request.logical_properties, context),
            diagnostic_id, field_id, detail);
      };

  const auto alternate_object_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3350);
  opt::CanonicalNativeObjectAdmissionContext object_context;
  object_context.catalog_object_uuids = {fixture.full_width_table_uuid};
  object_context.authorized_object_uuids = {fixture.full_width_table_uuid};
  object_context.catalog_object_evidence_engine_owned = true;
  object_context.authorization_object_evidence_engine_owned = true;

  auto object_mutation = object_context;
  object_mutation.catalog_object_uuids.clear();
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kCatalogObjectDiagnostic, kCatalogObjectField,
                         "missing catalog object evidence was accepted");

  object_mutation = object_context;
  object_mutation.catalog_object_uuids.push_back(
      fixture.full_width_table_uuid);
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kCatalogObjectDiagnostic, kCatalogObjectField,
                         "duplicate catalog object evidence was accepted");

  object_mutation = object_context;
  object_mutation.catalog_object_uuids = {alternate_object_uuid};
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kCatalogObjectDiagnostic, kCatalogObjectField,
                         "mismatched catalog object evidence was accepted");

  object_mutation = object_context;
  object_mutation.catalog_object_evidence_engine_owned = false;
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kCatalogObjectDiagnostic, kCatalogObjectField,
                         "caller-owned catalog object evidence was accepted");

  object_mutation = object_context;
  object_mutation.authorized_object_uuids.clear();
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kSecurityObjectDiagnostic, kSecurityObjectField,
                         "missing authorization object evidence was accepted");

  object_mutation = object_context;
  object_mutation.authorized_object_uuids.push_back(
      fixture.full_width_table_uuid);
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kSecurityObjectDiagnostic, kSecurityObjectField,
                         "duplicate authorization object evidence was accepted");

  object_mutation = object_context;
  object_mutation.authorized_object_uuids = {alternate_object_uuid};
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kSecurityObjectDiagnostic, kSecurityObjectField,
                         "mismatched authorization object evidence was accepted");

  object_mutation = object_context;
  object_mutation.authorization_object_evidence_engine_owned = false;
  require_object_refusal(result.request.logical_graph, object_mutation,
                         kSecurityObjectDiagnostic, kSecurityObjectField,
                         "caller-owned authorization object evidence was accepted");

  auto two_object_graph = result.request.logical_graph;
  two_object_graph.nodes.front().required_object_uuids = {
      fixture.full_width_table_uuid, alternate_object_uuid};
  std::vector<std::string> canonical_two_objects{
      fixture.full_width_table_uuid, alternate_object_uuid};
  std::ranges::sort(canonical_two_objects);
  object_context.catalog_object_uuids = canonical_two_objects;
  object_context.authorized_object_uuids = canonical_two_objects;

  object_mutation = object_context;
  std::ranges::reverse(object_mutation.catalog_object_uuids);
  require_object_refusal(two_object_graph, object_mutation,
                         kCatalogObjectDiagnostic, kCatalogObjectField,
                         "reordered catalog object evidence was accepted");

  object_mutation = object_context;
  std::ranges::reverse(object_mutation.authorized_object_uuids);
  require_object_refusal(
      two_object_graph, object_mutation, kSecurityObjectDiagnostic,
      kSecurityObjectField,
      "reordered authorization object evidence was accepted");

  ReleaseRequest(&acquisition);
  Rollback(reader);
}

void ValidateHeapOptimizerAdmissionRefusals(Fixture& fixture) {
  auto reader = QueryContext(Begin(fixture, "qow-heap-admission-refusals"),
                             fixture.full_width_table_uuid,
                             fixture.salt + 3400);
  auto acquisition = RequestFor(reader, fixture.full_width_table_uuid,
                                fixture.salt + 3420);
  const auto baseline = AdmissionRequestFor(acquisition);
  const auto expect_refusal = [&](const auto& candidate,
                                  const std::string_view detail) {
    const auto unchanged = CaptureFixtureFiles(fixture.directory);
    RequireAtomicAdmissionFailure(
        api::BuildCanonicalCurrentHeapOptimizerAdmission(candidate), detail);
    Require(CaptureFixtureFiles(fixture.directory) == unchanged,
            "heap optimizer admission refusal mutated database files");
  };

  auto mutated = baseline;
  mutated.relational_dag.nodes.push_back(mutated.relational_dag.nodes.front());
  mutated.relational_dag.nodes.back().node_id = 2;
  expect_refusal(mutated, "multi-node relation admission was not atomic");

  mutated = baseline;
  mutated.relational_dag.nodes.front().node_kind =
      api::RelationalDagNodeKind::kProject;
  expect_refusal(mutated, "non-scan relation admission was not atomic");

  mutated = baseline;
  mutated.relational_dag.nodes.front().required_object_uuids.push_back(
      NewUuidText(platform::UuidKind::object, fixture.salt + 3440));
  expect_refusal(mutated, "multiple relation objects were admitted");

  mutated = baseline;
  mutated.relational_dag.descriptors.pop_back();
  mutated.relational_dag.expressions.pop_back();
  mutated.relational_dag.outputs.pop_back();
  mutated.relational_dag.nodes.front().output_descriptor_ids.pop_back();
  mutated.relational_dag.nodes.front().bound_expression_ids.pop_back();
  expect_refusal(mutated, "partial persisted width was admitted");

  mutated = baseline;
  mutated.relational_dag.root_node_id = 999;
  expect_refusal(mutated, "nonexistent relation root was admitted");

  mutated = baseline;
  mutated.relational_dag.nodes.front().input_node_ids = {999};
  expect_refusal(mutated, "relation source input edge was admitted");

  mutated = baseline;
  mutated.relational_dag.nodes.front().semantic_variant_id =
      "relation.source.stale.v1";
  expect_refusal(mutated, "wrong relation source semantic variant was admitted");

  mutated = baseline;
  mutated.relational_dag.nodes.front().shareable = true;
  expect_refusal(mutated, "shareable relation source was admitted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().visible = false;
  expect_refusal(mutated, "invisible relation output was admitted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().ordinal = 9;
  expect_refusal(mutated, "noncontiguous relation output was admitted");

  mutated = baseline;
  api::RelationalValuesRowRecord values_row;
  values_row.row_id = 1;
  values_row.expression_ids = {1, 2};
  mutated.relational_dag.values_rows.push_back(std::move(values_row));
  mutated.relational_dag.nodes.front().values_row_ids = {1};
  expect_refusal(mutated, "relation source values carrier was admitted");

  mutated = baseline;
  api::RelationalGroupingSetRecord grouping_set;
  grouping_set.relation_node_id = 1;
  grouping_set.ordinal = 0;
  grouping_set.expression_ids = {1};
  mutated.relational_dag.grouping_sets.push_back(std::move(grouping_set));
  expect_refusal(mutated, "relation source grouping carrier was admitted");

  mutated = baseline;
  api::RelationalPropertyRecord property;
  property.property_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3447);
  property.property_kind = api::RelationalPropertyKind::kOrdering;
  property.origin_node_id = 1;
  property.expression_ids = {1};
  mutated.relational_dag.properties.push_back(property);
  mutated.relational_dag.nodes.front().required_property_uuids = {
      property.property_uuid};
  expect_refusal(mutated, "relation source logical property was admitted");

  mutated = baseline;
  mutated.relational_dag.expressions.front().result_descriptor_id = 2;
  expect_refusal(mutated, "expression/result descriptor mismatch was admitted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().expression_id = 2;
  expect_refusal(mutated, "output/expression linkage mismatch was admitted");

  mutated = baseline;
  mutated.relational_dag.expressions.front().function_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3448);
  expect_refusal(mutated, "identifier expression function carrier was admitted");

  mutated = baseline;
  mutated.relational_dag.nodes.front().required_object_uuids = {"not-a-uuid"};
  expect_refusal(mutated, "noncanonical relation identity was admitted");

  mutated = baseline;
  mutated.relational_dag.nodes.front().required_object_uuids = {
      NewUuidText(platform::UuidKind::object, fixture.salt + 3441)};
  expect_refusal(mutated, "missing relation identity was admitted");

  mutated = baseline;
  mutated.relational_dag.expressions.front().bound_name_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3442);
  expect_refusal(mutated, "stale column UUID binding was admitted");

  mutated = baseline;
  mutated.relational_dag.outputs.front().output_name_utf8 = "stale_name";
  expect_refusal(mutated, "stale column name binding was admitted");

  mutated = baseline;
  mutated.relational_dag.descriptors.front().descriptor_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3443);
  expect_refusal(mutated, "stale descriptor UUID was admitted");

  mutated = baseline;
  mutated.relational_dag.descriptors.front().type_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3444);
  expect_refusal(mutated, "stale type UUID was admitted");

  mutated = baseline;
  mutated.relational_dag.descriptors.front().nullability =
      api::RelationalNullability::kNullable;
  expect_refusal(mutated, "stale nullability was admitted");

  mutated = baseline;
  mutated.relational_dag.descriptors.front().collation_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3445);
  expect_refusal(mutated, "stale collation was admitted");

  mutated = baseline;
  mutated.relational_dag.descriptors.front().timezone_profile_id = "UTC";
  expect_refusal(mutated, "stale timezone profile was admitted");

  mutated = baseline;
  std::swap(mutated.relational_dag.outputs[0],
            mutated.relational_dag.outputs[1]);
  expect_refusal(mutated, "stale persisted output order was admitted");

  mutated = baseline;
  mutated.context.authorization_context.grants.clear();
  expect_refusal(mutated, "missing SELECT grant was admitted");

  mutated = baseline;
  mutated.context.authorization_context.grants.front().deny = true;
  expect_refusal(mutated, "denied SELECT grant was admitted");

  mutated = baseline;
  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3446);
  policy.subject_uuid = mutated.context.principal_uuid;
  policy.subject_kind = "principal";
  policy.target_uuid.canonical = fixture.full_width_table_uuid;
  policy.right = "SELECT";
  policy.policy_kind = "heap_admission_runtime_recheck";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = mutated.context.authorization_context.policy_epoch;
  mutated.context.authorization_context.policies.push_back(std::move(policy));
  expect_refusal(mutated, "runtime-recheck SELECT policy was admitted");

  struct AdmissionMutation {
    std::string_view detail;
    std::function<void(api::CanonicalHeapOptimizerAdmissionRequest&)> apply;
  };
  const std::vector<AdmissionMutation> missing_context_refusals{
      {"missing database path was admitted", [](auto& candidate) {
         candidate.context.database_path.clear();
       }},
      {"missing database UUID was admitted", [](auto& candidate) {
         candidate.context.database_uuid.canonical.clear();
       }},
      {"missing statement UUID was admitted", [](auto& candidate) {
         candidate.context.statement_uuid.canonical.clear();
       }},
      {"stale transaction UUID was admitted", [&](auto& candidate) {
         candidate.context.transaction_uuid.canonical =
             NewUuidText(platform::UuidKind::transaction,
                         fixture.salt + 3500);
       }},
      {"stale local transaction ID was admitted", [](auto& candidate) {
         ++candidate.context.local_transaction_id;
       }},
      {"caller-owned metadata snapshot was admitted", [](auto& candidate) {
         candidate.context.statement_metadata_snapshot_engine_owned = false;
       }},
      {"stale metadata snapshot UUID was admitted", [&](auto& candidate) {
         candidate.context.statement_metadata_snapshot_uuid.canonical =
             NewUuidText(platform::UuidKind::object, fixture.salt + 3501);
       }},
      {"missing security context was admitted", [](auto& candidate) {
         candidate.context.security_context_present = false;
       }},
      {"missing materialized authorization context was admitted",
       [](auto& candidate) {
         candidate.context.authorization_context.present = false;
       }},
      {"stale authorization authority was admitted", [&](auto& candidate) {
         candidate.context.authorization_context.authority_uuid.canonical =
             NewUuidText(platform::UuidKind::object, fixture.salt + 3502);
       }},
      {"authorization principal mismatch was admitted", [&](auto& candidate) {
         candidate.context.authorization_context.principal_uuid.canonical =
             NewUuidText(platform::UuidKind::principal,
                         fixture.salt + 3503);
       }},
      {"stale authorization catalog generation was admitted",
       [](auto& candidate) {
         ++candidate.context.authorization_context.catalog_generation_id;
       }},
      {"stale authorization security epoch was admitted", [](auto& candidate) {
         ++candidate.context.authorization_context.security_epoch;
       }},
      {"missing authorization policy epoch was admitted", [](auto& candidate) {
         candidate.context.authorization_context.policy_epoch = 0;
       }},
      {"missing resource epoch was admitted", [](auto& candidate) {
         candidate.context.resource_epoch = 0;
       }},
      {"missing resource snapshot was admitted", [](auto& candidate) {
         candidate.context.optimizer_resource_snapshot_uuid.canonical.clear();
       }},
      {"missing route snapshot was admitted", [](auto& candidate) {
         candidate.context.optimizer_route_snapshot_uuid.canonical.clear();
       }},
      {"missing route generation was admitted", [](auto& candidate) {
         candidate.context.optimizer_route_generation = 0;
       }},
      {"missing optimizer candidate bound was admitted", [](auto& candidate) {
         candidate.context.optimizer_maximum_candidate_count = 0;
       }},
      {"missing optimizer memo bound was admitted", [](auto& candidate) {
         candidate.context.optimizer_maximum_memo_groups = 0;
       }},
      {"missing optimizer search bound was admitted", [](auto& candidate) {
         candidate.context.optimizer_maximum_search_steps = 0;
       }},
      {"missing optimizer time bound was admitted", [](auto& candidate) {
         candidate.context.optimizer_maximum_planning_time_ns = 0;
       }},
      {"indeterminate authorization subject context was admitted",
       [](auto& candidate) {
         candidate.context.authorization_context.effective_subjects.clear();
       }},
  };
  for (const auto& context_refusal : missing_context_refusals) {
    mutated = baseline;
    context_refusal.apply(mutated);
    expect_refusal(mutated, context_refusal.detail);
  }

  mutated = baseline;
  ++mutated.context.catalog_generation_id;
  expect_refusal(mutated, "stale catalog generation was admitted");

  mutated = baseline;
  ++mutated.context.security_epoch;
  expect_refusal(mutated, "stale security epoch was admitted");

  mutated = baseline;
  mutated.context.snapshot_visible_through_local_transaction_id = 0;
  expect_refusal(mutated, "missing MGA snapshot was admitted");

  mutated = baseline;
  mutated.context.optimizer_memory_budget_bytes = 0;
  expect_refusal(mutated, "missing optimizer memory bound was admitted");

  mutated = baseline;
  mutated.context.optimizer_capability_snapshot_uuid.canonical.clear();
  expect_refusal(mutated, "missing capability snapshot was admitted");

  mutated = baseline;
  mutated.context.optimizer_route_epoch = 0;
  expect_refusal(mutated, "stale optimizer route was admitted");

  mutated = baseline;
  mutated.context.current_monotonic_ns = "0";
  expect_refusal(mutated, "missing monotonic admission time was admitted");

  mutated = baseline;
  mutated.context.prepared_metadata_required_object_uuid.canonical =
      fixture.full_width_table_uuid;
  expect_refusal(mutated, "prepared metadata object state was admitted");

  mutated = baseline;
  mutated.context.prepared_metadata_required_executable_generation = 1;
  expect_refusal(mutated,
                 "prepared metadata executable generation was admitted");

  mutated = baseline;
  mutated.context.prepared_metadata_required_metadata_epoch = 1;
  expect_refusal(mutated, "prepared metadata epoch was admitted");

  auto temporary_context = QueryContext(
      reader, fixture.temporary_table_uuid, fixture.salt + 3460);
  auto temporary_acquisition = RequestFor(
      temporary_context, fixture.temporary_table_uuid, fixture.salt + 3480);
  expect_refusal(AdmissionRequestFor(temporary_acquisition),
                 "temporary relation was admitted as a local heap source");
  ReleaseRequest(&temporary_acquisition);

  const auto current_descriptor = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.full_width_table_uuid);
  Require(current_descriptor.ok,
          "current descriptor was unavailable for stale-record refusals");
  auto unsupported_status = current_descriptor.descriptor;
  unsupported_status.descriptor_status = "stale_descriptor";
  AppendCompleteRelationDescriptorRecord(reader, unsupported_status);
  expect_refusal(baseline,
                 "unsupported persisted relation descriptor status was admitted");
  AppendCompleteRelationDescriptorRecord(reader, current_descriptor.descriptor);
  auto restored_descriptor = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.full_width_table_uuid);
  Require(restored_descriptor.ok &&
              restored_descriptor.descriptor.descriptor_status ==
                  current_descriptor.descriptor.descriptor_status,
          "persisted descriptor status fixture was not restored");

  auto zero_generation = current_descriptor.descriptor;
  zero_generation.descriptor_generation = 0;
  AppendCompleteRelationDescriptorRecord(reader, zero_generation);
  expect_refusal(baseline,
                 "zero/stale persisted relation descriptor generation was admitted");
  AppendCompleteRelationDescriptorRecord(reader, current_descriptor.descriptor);
  restored_descriptor = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.full_width_table_uuid);
  Require(restored_descriptor.ok &&
              restored_descriptor.descriptor.descriptor_generation ==
                  current_descriptor.descriptor.descriptor_generation,
          "persisted descriptor generation fixture was not restored");

  auto invisible_writer = Begin(fixture, "qow-heap-admission-invisible-writer");
  const auto invisible_relation_uuid =
      NewUuidText(platform::UuidKind::object, fixture.salt + 3520);
  PersistTable(fixture, invisible_writer, invisible_relation_uuid, false, true);
  const auto writer_descriptor = api::LoadMgaRelationStorageDescriptor(
      invisible_writer, invisible_relation_uuid);
  Require(writer_descriptor.ok && writer_descriptor.descriptor.columns.size() == 2,
          "writer could not load its uncommitted relation descriptor");
  auto invisible_reader = QueryContext(
      Begin(fixture, "qow-heap-admission-invisible-reader"),
      invisible_relation_uuid, fixture.salt + 3530);
  auto invisible_acquisition = BoundRequest(
      invisible_reader, writer_descriptor.descriptor, fixture.salt + 3540);
  expect_refusal(AdmissionRequestFor(invisible_acquisition),
                 "separately authorized reader admitted an uncommitted relation");
  ReleaseRequest(&invisible_acquisition);
  Rollback(invisible_reader);
  Rollback(invisible_writer);

  ReleaseRequest(&acquisition);
  Rollback(reader);
}

}  // namespace

int main() {
  auto fixture = MakeFixture();
  ValidateHeapOptimizerAdmissionMatrix(fixture);
  ValidateHeapOptimizerAdmissionRefusals(fixture);
  ValidateFullWidthHeapMatrix(fixture);
  ValidateFullWidthHeapRefusals(fixture);
  ValidateOptimizerSelectedHeapResultMatrix(fixture);
  ValidateOptimizerSelectedHeapResultRefusals(fixture);
  ValidateStorageNullabilityCarrierMatrix(fixture);
  ValidatePhysicalHeapDispatchMatrix(fixture);
  ValidatePhysicalHeapDispatchRefusals(fixture);
  ValidatePositiveAndVisibilityMatrix(fixture);
  ValidateBindingSecurityAndResourceRefusals(fixture);
  return EXIT_SUCCESS;
}
