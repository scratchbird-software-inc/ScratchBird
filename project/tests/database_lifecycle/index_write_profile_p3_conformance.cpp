// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "dml/insert_batch.hpp"
#include "dml/update_batch.hpp"
#include "index_family_registry.hpp"
#include "index_management.hpp"
#include "index_metrics.hpp"
#include "metric_registry.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace index_api = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid TypedUuid(platform::UuidKind kind, unsigned char salt) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[1] = 0x9e;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  uuid.value.bytes[15] = salt;
  return uuid;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.request_id = "p3-write-profile";
  context.database_uuid.canonical = "database-p3-write-profile";
  context.principal_uuid.canonical = "principal-p3-write-profile";
  context.transaction_uuid.canonical = "transaction-p3-write-profile";
  context.local_transaction_id = 42;
  context.snapshot_visible_through_local_transaction_id = 42;
  context.security_context_present = true;
  return context;
}

api::CrudTableRecord Table() {
  api::CrudTableRecord table;
  table.creator_tx = 42;
  table.table_uuid = "table-p3-write-profile";
  table.default_name = "profile_table";
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(std::string uuid,
                           std::string column,
                           std::string family,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = 42;
  index.index_uuid = std::move(uuid);
  index.table_uuid = "table-p3-write-profile";
  index.column_name = std::move(column);
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

api::CrudState State() {
  api::CrudState state;
  state.transactions[42] = "active";
  state.tables.push_back(Table());
  api::CrudRowVersionRecord row;
  row.creator_tx = 42;
  row.table_uuid = "table-p3-write-profile";
  row.row_uuid = "row-p3-write-profile";
  row.version_uuid = "version-p3-write-profile";
  row.values.push_back({"id", "1"});
  row.values.push_back({"name", "alpha"});
  row.deleted = false;
  state.row_versions.push_back(std::move(row));
  return state;
}

api::EngineRowValue InputRow(std::string id, std::string name) {
  api::EngineRowValue row;
  api::EngineTypedValue id_value;
  id_value.descriptor.canonical_type_name = "int64";
  id_value.encoded_value = std::move(id);
  row.fields.push_back({"id", id_value});
  api::EngineTypedValue name_value;
  name_value.descriptor.canonical_type_name = "character";
  name_value.encoded_value = std::move(name);
  row.fields.push_back({"name", name_value});
  return row;
}

api::EngineInsertRowsRequest InsertRequest() {
  api::EngineInsertRowsRequest request;
  request.context = Context();
  request.target_table.uuid.canonical = "table-p3-write-profile";
  request.target_schema.uuid.canonical = "schema-p3-write-profile";
  request.estimated_row_count = 2;
  request.input_rows.push_back(InputRow("1", "alpha"));
  request.input_rows.push_back(InputRow("2", "beta"));
  return request;
}

api::EngineUpdateRowsRequest UpdateRequest() {
  api::EngineUpdateRowsRequest request;
  request.context = Context();
  request.target_table.uuid.canonical = "table-p3-write-profile";
  request.update_predicate.predicate_kind = "column_eq";
  request.update_predicate.canonical_predicate_envelope = "name";
  api::EngineTypedValue value;
  value.descriptor.canonical_type_name = "character";
  value.encoded_value = "bravo";
  request.assignments.push_back({"name", value});
  return request;
}

bool HasInsertAction(const api::IndexMaintenancePlan& plan,
                     api::InsertIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.action == action) { return true; }
  }
  return false;
}

bool HasUpdateAction(const api::UpdateIndexMaintenancePlan& plan,
                     api::UpdateIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.action == action) { return true; }
  }
  return false;
}

bool HasStep(const index_api::IndexManagementPlan& plan, std::string_view step) {
  return std::find(plan.steps.begin(), plan.steps.end(), step) != plan.steps.end();
}

void TestIndexFamilyAndManagementMatrix() {
  std::set<std::string> expected = {
      "btree", "unique_btree", "expression", "partial", "covering", "hash",
      "bitmap", "brin_zone", "bloom", "full_text", "gin", "inverted",
      "ngram", "sparse_wand", "spatial", "rtree", "gist", "spgist",
      "vector_exact", "vector_hnsw", "vector_ivf", "columnar_zone",
      "document_path", "graph", "temporary_work", "in_memory",
      "reference_emulated", "advanced_vector_policy_blocked"};

  for (const auto& descriptor : index_api::BuiltinIndexFamilyDescriptors()) {
    Require(expected.erase(descriptor.id) == 1, "unexpected or duplicate index family descriptor");
    Require(descriptor.family_uuid.valid(), "index family UUID is invalid");
    Require(!descriptor.metrics_prefix.empty(), "index family metrics prefix is missing");
    Require(!descriptor.diagnostics_prefix.empty(), "index family diagnostics prefix is missing");
    if (descriptor.family != index_api::IndexFamily::policy_blocked) {
      Require(descriptor.requires_mga_recheck, "index family does not require MGA recheck");
      Require(!descriptor.packet_path.empty(), "index family packet path is missing");
    }
  }
  Require(expected.empty(), "index family registry is missing expected families");
  Require(index_api::IsPolicyBlockedIndexFamily(index_api::IndexFamily::policy_blocked),
          "policy-blocked index family did not fail closed");

  index_api::IndexManagementRequest invalid;
  invalid.operation = index_api::IndexManagementOperation::create;
  invalid.family = index_api::IndexFamily::btree;
  invalid.policy_allows_mutation = true;
  auto plan = index_api::PlanIndexManagementOperation(invalid);
  Require(!plan.ok(), "invalid index management request was admitted");
  Require(plan.diagnostic.diagnostic_code == "SB-INDEX-MANAGEMENT-INVALID-REQUEST",
          "invalid index management diagnostic mismatch");

  index_api::IndexManagementRequest create;
  create.operation = index_api::IndexManagementOperation::create;
  create.family = index_api::IndexFamily::btree;
  create.index_uuid = TypedUuid(platform::UuidKind::object, 0x51);
  plan = index_api::PlanIndexManagementOperation(create);
  Require(!plan.ok(), "index create was admitted without mutation policy");
  Require(plan.diagnostic.diagnostic_code == "SB-INDEX-MANAGEMENT-MUTATION-REFUSED",
          "index mutation refusal diagnostic mismatch");

  create.policy_allows_mutation = true;
  plan = index_api::PlanIndexManagementOperation(create);
  Require(plan.ok(), "index create was not admitted with mutation policy");
  Require(HasStep(plan, "write_catalog_evidence_before_success"),
          "index create omitted catalog evidence step");
  Require(HasStep(plan, "publish_index_resource_epoch"),
          "index create omitted resource epoch publication step");
}

void TestInsertWriteProfiles() {
  const auto table = Table();
  const auto state = State();
  const std::vector<api::CrudIndexRecord> indexes = {
      Index("index-name", "name", api::kCrudIndexFamilyBtree, false),
      Index("index-id-unique", "id", api::kCrudIndexFamilyBtree, true)};

  auto unsafe_request = InsertRequest();
  unsafe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  auto unsafe_context = api::BeginInsertBatchContext(unsafe_request, state, table, indexes);
  Require(unsafe_context.accepted, "insert context with unsafe delta option should still use exact path");
  Require(!unsafe_context.delta_ledger_policy.enabled,
          "insert delta ledger became enabled without MGA safety proofs");
  Require(!HasInsertAction(unsafe_context.index_plan, api::InsertIndexMaintenanceAction::committed_delta_ledger),
          "insert selected delta ledger without reader/cleanup/recovery proofs");
  Require(HasInsertAction(unsafe_context.index_plan,
                          api::InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert),
          "insert unique index preflight was not selected");

  auto safe_request = InsertRequest();
  safe_request.option_envelopes.push_back("runtime.deferred_secondary_index=enabled");
  safe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.reader_overlay=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.cleanup_horizon_bound=true");
  safe_request.option_envelopes.push_back("delta_ledger.recovery_classifiable=true");
  safe_request.option_envelopes.push_back("policy_snapshot_uuid=policy-p3-insert");
  auto safe_context = api::BeginInsertBatchContext(safe_request, state, table, indexes);
  Require(safe_context.accepted, "insert context with safe delta proofs was refused");
  Require(safe_context.delta_ledger_policy.enabled, "insert delta ledger proofs were not accepted");
  Require(HasInsertAction(safe_context.index_plan, api::InsertIndexMaintenanceAction::committed_delta_ledger),
          "insert did not select committed delta ledger after proofs");
  Require(safe_context.policy_snapshot_uuid == "policy-p3-insert", "insert policy snapshot was not bound");

  auto bulk_request = InsertRequest();
  bulk_request.strict_bulk_load_requested = true;
  auto bulk_context = api::BeginInsertBatchContext(bulk_request, state, table, indexes);
  Require(!bulk_context.accepted, "strict bulk load was admitted without policy");
  Require(bulk_context.fallback_reason == "strict_bulk_load_policy_not_enabled",
          "strict bulk load fallback reason mismatch");

  safe_context.memory_policy.context_budget_bytes = 1;
  const auto memory = api::ValidateInsertBatchMemoryBudget(safe_context, 1024);
  Require(memory.error, "insert memory budget overflow was not refused");
  Require(memory.detail == "dml.insert_rows:insert_batch_memory_budget_exceeded",
          "insert memory budget diagnostic mismatch");
}

void TestUpdateWriteProfiles() {
  const auto table = Table();
  const auto state = State();
  const std::vector<api::CrudIndexRecord> indexes = {
      Index("index-name", "name", api::kCrudIndexFamilyBtree, false),
      Index("index-id-unique", "id", api::kCrudIndexFamilyBtree, true)};

  auto unsafe_request = UpdateRequest();
  unsafe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  auto unsafe_context = api::BuildUpdateBatchContext(unsafe_request, state, table, indexes);
  Require(unsafe_context.accepted, "update context with unsafe delta option should still use exact path");
  Require(!unsafe_context.delta_ledger_policy.enabled,
          "update delta ledger became enabled without MGA safety proofs");
  Require(!HasUpdateAction(unsafe_context.index_plan, api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "update selected delta ledger without reader/cleanup/recovery proofs");
  Require(HasUpdateAction(unsafe_context.index_plan,
                          api::UpdateIndexMaintenanceAction::synchronous_exact_rewrite),
          "update exact rewrite was not selected for affected non-unique index");
  Require(HasUpdateAction(unsafe_context.index_plan,
                          api::UpdateIndexMaintenanceAction::unaffected),
          "update did not leave unaffected unique index alone");

  auto safe_request = UpdateRequest();
  safe_request.option_envelopes.push_back("runtime.deferred_secondary_index=enabled");
  safe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.reader_overlay=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.cleanup_horizon_bound=true");
  safe_request.option_envelopes.push_back("delta_ledger.recovery_classifiable=true");
  safe_request.option_envelopes.push_back("policy_snapshot_uuid=policy-p3-update");
  auto safe_context = api::BuildUpdateBatchContext(safe_request, state, table, indexes);
  Require(safe_context.accepted, "update context with safe delta proofs was refused");
  Require(safe_context.delta_ledger_policy.enabled, "update delta ledger proofs were not accepted");
  Require(HasUpdateAction(safe_context.index_plan, api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "update did not select committed delta ledger after proofs");
  Require(safe_context.policy_snapshot_uuid == "policy-p3-update", "update policy snapshot was not bound");

  auto disabled_page_request = UpdateRequest();
  disabled_page_request.option_envelopes.push_back("feature.page_reservation=disabled");
  const auto disabled_page_context = api::BuildUpdateBatchContext(disabled_page_request, state, table, indexes);
  Require(!disabled_page_context.accepted, "update admitted with disabled page reservation");
  Require(disabled_page_context.fallback_reason == "page_reservation_disabled",
          "update page reservation fallback reason mismatch");

  auto tiny_memory_request = UpdateRequest();
  tiny_memory_request.option_envelopes.push_back("memory.context_budget_bytes=1");
  auto tiny_memory_context = api::BuildUpdateBatchContext(tiny_memory_request, state, table, indexes);
  const auto memory = api::ValidateUpdateBatchMemoryBudget(tiny_memory_context, 1024);
  Require(memory.error, "update memory budget overflow was not refused");
  Require(memory.detail == "dml.update_rows:update_batch_memory_budget_exceeded",
          "update memory budget diagnostic mismatch");
}

void TestIndexMetrics() {
  const auto descriptors = index_api::EnsureIndexMetricDescriptors();
  Require(descriptors.ok, "index metric descriptors did not register");

  index_api::IndexMetricIdentity identity;
  identity.index_uuid = "index-p3-metrics";
  identity.index_family = "btree";
  identity.semantic_profile_id = "sbsql_v3";
  identity.operation = "lookup";
  identity.result = "ok";
  identity.filespace_uuid = "filespace-p3";

  index_api::IndexLogicalMetricDelta logical;
  logical.candidates = 10;
  logical.visible = 9;
  logical.rechecks = 10;
  auto published = index_api::PublishIndexLogicalMetrics(identity, logical);
  Require(published.ok, "index logical metrics publish failed");

  index_api::IndexPhysicalMetricDelta physical;
  physical.pages_read = 2;
  physical.pages_written = 1;
  physical.depth = 3;
  physical.density_ratio = 0.75;
  published = index_api::PublishIndexPhysicalMetrics(identity, physical);
  Require(published.ok, "index physical metrics publish failed");

  index_api::IndexMaintenanceMetricDelta maintenance;
  maintenance.operations = 1;
  maintenance.progress_percent = 100;
  published = index_api::PublishIndexMaintenanceMetrics(identity, maintenance);
  Require(published.ok, "index maintenance metrics publish failed");

  bool saw_candidates = false;
  bool saw_depth = false;
  bool saw_maintenance = false;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    saw_candidates = saw_candidates || value.family == "sb_index_candidates_total";
    saw_depth = saw_depth || value.family == "sb_index_depth";
    saw_maintenance = saw_maintenance || value.family == "sb_index_maintenance_operations_total";
  }
  Require(saw_candidates, "index candidates metric snapshot missing");
  Require(saw_depth, "index depth metric snapshot missing");
  Require(saw_maintenance, "index maintenance metric snapshot missing");
}

}  // namespace

int main() {
  TestIndexFamilyAndManagementMatrix();
  TestInsertWriteProfiles();
  TestUpdateWriteProfiles();
  TestIndexMetrics();
  return EXIT_SUCCESS;
}
