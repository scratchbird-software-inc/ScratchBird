// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/delete_batch.hpp"
#include "dml/insert_batch.hpp"
#include "dml/update_batch.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr std::string_view kFeatureFlagSearchKey =
    "DPC_DEFERRED_INDEX_FEATURE_FLAG";
constexpr std::string_view kGateSearchKey =
    "DPC_DEFERRED_INDEX_FEATURE_FLAG_GATE";
constexpr const char* kRuntimeOption = "runtime.deferred_secondary_index=enabled";
constexpr const char* kRuntimeOffOption = "runtime.deferred_secondary_index=disabled";
constexpr const char* kFeatureOption = "feature.secondary_index_delta_ledger=enabled";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

api::EngineRequestContext Context(std::string request_id) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_uuid.canonical = "database-dpc-020";
  context.principal_uuid.canonical = "principal-dpc-020";
  context.transaction_uuid.canonical = "transaction-dpc-020";
  context.local_transaction_id = 42;
  context.snapshot_visible_through_local_transaction_id = 42;
  context.security_context_present = true;
  return context;
}

api::CrudTableRecord Table() {
  api::CrudTableRecord table;
  table.creator_tx = 42;
  table.table_uuid = "table-dpc-020";
  table.default_name = "dpc020_table";
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
  index.table_uuid = "table-dpc-020";
  index.column_name = std::move(column);
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

std::vector<api::CrudIndexRecord> Indexes() {
  return {
      Index("index-dpc-020-name", "name", api::kCrudIndexFamilyBtree, false),
      Index("index-dpc-020-id-unique", "id", api::kCrudIndexFamilyBtree, true)};
}

api::CrudState State() {
  api::CrudState state;
  state.transactions[42] = "active";
  state.tables.push_back(Table());
  api::CrudRowVersionRecord row;
  row.creator_tx = 42;
  row.table_uuid = "table-dpc-020";
  row.row_uuid = "row-dpc-020";
  row.version_uuid = "version-dpc-020";
  row.values.push_back({"id", "1"});
  row.values.push_back({"name", "alpha"});
  row.values.push_back({"payload", "payload-1"});
  state.row_versions.push_back(std::move(row));
  return state;
}

api::EngineTypedValue Value(std::string type, std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor.canonical_type_name = std::move(type);
  value.encoded_value = std::move(encoded);
  return value;
}

api::EngineRowValue InputRow() {
  api::EngineRowValue row;
  row.fields.push_back({"id", Value("int64", "2")});
  row.fields.push_back({"name", Value("character", "bravo")});
  row.fields.push_back({"payload", Value("character", "payload-2")});
  return row;
}

api::EngineInsertRowsRequest InsertRequest(std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = Context("dpc-020-insert");
  request.target_table.uuid.canonical = "table-dpc-020";
  request.target_schema.uuid.canonical = "schema-dpc-020";
  request.target_object.uuid.canonical = "table-dpc-020";
  request.estimated_row_count = 1;
  request.input_rows.push_back(InputRow());
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineUpdateRowsRequest UpdateRequest(std::vector<std::string> options = {},
                                           std::string column = "name") {
  api::EngineUpdateRowsRequest request;
  request.context = Context("dpc-020-update");
  request.target_table.uuid.canonical = "table-dpc-020";
  request.update_predicate.predicate_kind = "column_eq";
  request.update_predicate.canonical_predicate_envelope = "name";
  request.assignments.push_back({std::move(column), Value("character", "charlie")});
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineDeleteRowsRequest DeleteRequest(std::vector<std::string> options = {}) {
  api::EngineDeleteRowsRequest request;
  request.context = Context("dpc-020-delete");
  request.target_table.uuid.canonical = "table-dpc-020";
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.tombstone_only = true;
  request.option_envelopes = std::move(options);
  return request;
}

std::vector<std::string> ProofOptions() {
  return {
      kFeatureOption,
      "delta_ledger.reader_overlay=enabled",
      "delta_ledger.cleanup_horizon_bound=true",
      "delta_ledger.recovery_classifiable=true"};
}

std::vector<std::string> RuntimeProofOptions() {
  auto options = ProofOptions();
  options.insert(options.begin(), kRuntimeOption);
  return options;
}

bool HasInsertAction(const api::IndexMaintenancePlan& plan,
                     const std::string& index_uuid,
                     api::InsertIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.index.index_uuid == index_uuid && entry.action == action) {
      return true;
    }
  }
  return false;
}

bool HasUpdateAction(const api::UpdateIndexMaintenancePlan& plan,
                     const std::string& index_uuid,
                     api::UpdateIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.index.index_uuid == index_uuid && entry.action == action) {
      return true;
    }
  }
  return false;
}

bool HasDeleteAction(const api::DeleteIndexMaintenancePlan& plan,
                     const std::string& index_uuid,
                     api::DeleteIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.index.index_uuid == index_uuid && entry.action == action) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && entry.evidence_id == id) {
      return true;
    }
  }
  return false;
}

void RequireInsertSynchronous(const api::InsertBatchContext& context,
                              std::string_view reason) {
  Require(context.accepted, "DPC-020 insert context was refused");
  Require(!context.delta_ledger_policy.enabled,
          "DPC-020 insert enabled deferred maintenance without runtime admission");
  Require(context.delta_ledger_policy.synchronous_fallback_required,
          "DPC-020 insert did not record synchronous fallback");
  Require(context.delta_ledger_policy.fallback_reason == reason,
          "DPC-020 insert fallback reason mismatch");
  Require(!HasInsertAction(context.index_plan,
                           "index-dpc-020-name",
                           api::InsertIndexMaintenanceAction::committed_delta_ledger),
          "DPC-020 insert selected committed delta ledger while runtime gate was closed");
  Require(HasInsertAction(context.index_plan,
                          "index-dpc-020-name",
                          api::InsertIndexMaintenanceAction::synchronous_exact_insert),
          "DPC-020 insert did not use synchronous non-unique maintenance");
  Require(HasInsertAction(context.index_plan,
                          "index-dpc-020-id-unique",
                          api::InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert),
          "DPC-020 insert did not preserve unique preflight");

  api::EngineApiResult result;
  api::AddInsertBatchEvidenceToResult(context, &result);
  Require(HasEvidence(result.evidence, "insert_delta_ledger_policy", "synchronous_fallback"),
          "DPC-020 insert result evidence omitted synchronous fallback");
  Require(HasEvidence(result.evidence, "insert_deferred_secondary_index_fallback_reason", reason),
          "DPC-020 insert result evidence omitted fallback reason");
}

void RequireUpdateSynchronous(const api::UpdateBatchContext& context,
                              std::string_view reason) {
  Require(context.accepted, "DPC-020 update context was refused");
  Require(!context.delta_ledger_policy.enabled,
          "DPC-020 update enabled deferred maintenance without runtime admission");
  Require(context.delta_ledger_policy.synchronous_fallback_required,
          "DPC-020 update did not record synchronous fallback");
  Require(context.delta_ledger_policy.fallback_reason == reason,
          "DPC-020 update fallback reason mismatch");
  Require(!HasUpdateAction(context.index_plan,
                           "index-dpc-020-name",
                           api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "DPC-020 update selected committed delta ledger while runtime gate was closed");
  Require(HasUpdateAction(context.index_plan,
                          "index-dpc-020-name",
                          api::UpdateIndexMaintenanceAction::synchronous_exact_rewrite),
          "DPC-020 update did not use synchronous non-unique rewrite");

  api::EngineApiResult result;
  api::AddUpdateBatchEvidenceToResult(context, &result);
  Require(HasEvidence(result.evidence, "update_delta_ledger_policy", "synchronous_fallback"),
          "DPC-020 update result evidence omitted synchronous fallback");
  Require(HasEvidence(result.evidence, "update_deferred_secondary_index_fallback_reason", reason),
          "DPC-020 update result evidence omitted fallback reason");
}

void RequireDeleteSynchronous(const api::DeleteBatchContext& context,
                              std::string_view reason) {
  Require(context.accepted, "DPC-020 delete context was refused");
  Require(!context.delta_ledger_policy.enabled,
          "DPC-020 delete enabled deferred maintenance without runtime admission");
  Require(context.delta_ledger_policy.synchronous_fallback_required,
          "DPC-020 delete did not record synchronous fallback");
  Require(context.delta_ledger_policy.fallback_reason == reason,
          "DPC-020 delete fallback reason mismatch");
  Require(!HasDeleteAction(context.index_plan,
                           "index-dpc-020-name",
                           api::DeleteIndexMaintenanceAction::tombstone_delta_ledger),
          "DPC-020 delete selected tombstone delta ledger while runtime gate was closed");
  Require(HasDeleteAction(context.index_plan,
                          "index-dpc-020-name",
                          api::DeleteIndexMaintenanceAction::visibility_recheck_only),
          "DPC-020 delete did not use visibility recheck fallback");
  Require(HasDeleteAction(context.index_plan,
                          "index-dpc-020-id-unique",
                          api::DeleteIndexMaintenanceAction::visibility_recheck_only),
          "DPC-020 delete did not keep unique index synchronous-safe");

  api::EngineApiResult result;
  api::AddDeleteBatchEvidenceToResult(context, &result);
  Require(HasEvidence(result.evidence, "delete_delta_ledger_policy", "synchronous_fallback"),
          "DPC-020 delete result evidence omitted synchronous fallback");
  Require(HasEvidence(result.evidence, "delete_deferred_secondary_index_fallback_reason", reason),
          "DPC-020 delete result evidence omitted fallback reason");
}

void ValidateDefaultOff() {
  const auto table = Table();
  const auto state = State();
  const auto indexes = Indexes();
  RequireInsertSynchronous(api::BeginInsertBatchContext(InsertRequest(), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
  RequireUpdateSynchronous(api::BuildUpdateBatchContext(UpdateRequest(), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
  RequireDeleteSynchronous(api::BuildDeleteBatchContext(DeleteRequest(), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
}

void ValidateExplicitOffAndProofsWithoutRuntime() {
  const auto table = Table();
  const auto state = State();
  const auto indexes = Indexes();
  auto explicit_off = ProofOptions();
  explicit_off.insert(explicit_off.begin(), kRuntimeOffOption);

  RequireInsertSynchronous(api::BeginInsertBatchContext(InsertRequest(explicit_off), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
  RequireUpdateSynchronous(api::BuildUpdateBatchContext(UpdateRequest(explicit_off), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
  RequireDeleteSynchronous(api::BuildDeleteBatchContext(DeleteRequest(explicit_off), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");

  const auto proofs_without_runtime = ProofOptions();
  RequireInsertSynchronous(api::BeginInsertBatchContext(InsertRequest(proofs_without_runtime), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
  RequireUpdateSynchronous(api::BuildUpdateBatchContext(UpdateRequest(proofs_without_runtime), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
  RequireDeleteSynchronous(api::BuildDeleteBatchContext(DeleteRequest(proofs_without_runtime), state, table, indexes),
                           "runtime_deferred_secondary_index_disabled");
}

void ValidateRuntimeOnStillNeedsProofs() {
  const auto table = Table();
  const auto state = State();
  const auto indexes = Indexes();
  const std::vector<std::string> runtime_without_proofs = {kRuntimeOption, kFeatureOption};

  RequireInsertSynchronous(api::BeginInsertBatchContext(InsertRequest(runtime_without_proofs), state, table, indexes),
                           "secondary_index_delta_ledger_safety_proofs_incomplete");
  RequireUpdateSynchronous(api::BuildUpdateBatchContext(UpdateRequest(runtime_without_proofs), state, table, indexes),
                           "secondary_index_delta_ledger_safety_proofs_incomplete");
  RequireDeleteSynchronous(api::BuildDeleteBatchContext(DeleteRequest(runtime_without_proofs), state, table, indexes),
                           "secondary_index_delta_ledger_safety_proofs_incomplete");
}

void ValidateRuntimeOnWithProofsAdmitsNonUniqueOnly() {
  const auto table = Table();
  const auto state = State();
  const auto indexes = Indexes();
  const auto options = RuntimeProofOptions();

  const auto insert = api::BeginInsertBatchContext(InsertRequest(options), state, table, indexes);
  Require(insert.accepted, "DPC-020 insert runtime-on context was refused");
  Require(insert.delta_ledger_policy.enabled, "DPC-020 insert did not admit runtime-on proofs");
  Require(HasInsertAction(insert.index_plan,
                          "index-dpc-020-name",
                          api::InsertIndexMaintenanceAction::committed_delta_ledger),
          "DPC-020 insert did not select non-unique committed delta ledger");
  Require(HasInsertAction(insert.index_plan,
                          "index-dpc-020-id-unique",
                          api::InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert),
          "DPC-020 insert bypassed DPC-019 unique preflight");

  const auto update = api::BuildUpdateBatchContext(UpdateRequest(options), state, table, indexes);
  Require(update.accepted, "DPC-020 update runtime-on context was refused");
  Require(update.delta_ledger_policy.enabled, "DPC-020 update did not admit runtime-on proofs");
  Require(HasUpdateAction(update.index_plan,
                          "index-dpc-020-name",
                          api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "DPC-020 update did not select non-unique committed delta ledger");
  Require(HasUpdateAction(update.index_plan,
                          "index-dpc-020-id-unique",
                          api::UpdateIndexMaintenanceAction::unaffected),
          "DPC-020 update incorrectly touched the unique index");

  const auto unique_update = api::BuildUpdateBatchContext(UpdateRequest(options, "id"),
                                                         state,
                                                         table,
                                                         indexes);
  Require(unique_update.accepted, "DPC-020 unique-key update context was refused");
  Require(!HasUpdateAction(unique_update.index_plan,
                           "index-dpc-020-id-unique",
                           api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "DPC-020 update selected delta ledger for unique index");
  Require(HasUpdateAction(unique_update.index_plan,
                          "index-dpc-020-id-unique",
                          api::UpdateIndexMaintenanceAction::synchronous_exact_probe_then_rewrite),
          "DPC-020 update bypassed DPC-019 unique rewrite preflight");

  const auto delete_context = api::BuildDeleteBatchContext(DeleteRequest(options), state, table, indexes);
  Require(delete_context.accepted, "DPC-020 delete runtime-on context was refused");
  Require(delete_context.delta_ledger_policy.enabled, "DPC-020 delete did not admit runtime-on proofs");
  Require(HasDeleteAction(delete_context.index_plan,
                          "index-dpc-020-name",
                          api::DeleteIndexMaintenanceAction::tombstone_delta_ledger),
          "DPC-020 delete did not select non-unique tombstone delta ledger");
  Require(!HasDeleteAction(delete_context.index_plan,
                           "index-dpc-020-id-unique",
                           api::DeleteIndexMaintenanceAction::tombstone_delta_ledger),
          "DPC-020 delete selected tombstone delta ledger for unique index");
  Require(HasDeleteAction(delete_context.index_plan,
                          "index-dpc-020-id-unique",
                          api::DeleteIndexMaintenanceAction::visibility_recheck_only),
          "DPC-020 delete did not preserve unique visibility recheck");
}

}  // namespace

int main() {
  Require(!kFeatureFlagSearchKey.empty(), "DPC-020 feature flag search key missing");
  Require(!kGateSearchKey.empty(), "DPC-020 gate search key missing");

  ValidateDefaultOff();
  ValidateExplicitOffAndProofsWithoutRuntime();
  ValidateRuntimeOnStillNeedsProofs();
  ValidateRuntimeOnWithProofsAdmitsNonUniqueOnly();

  return EXIT_SUCCESS;
}
