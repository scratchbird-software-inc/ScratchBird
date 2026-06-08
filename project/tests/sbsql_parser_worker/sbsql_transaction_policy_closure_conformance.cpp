// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_cleanup.hpp"
#include "transaction_evidence.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"
#include "transaction_recovery.hpp"
#include "transaction_snapshot.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000070701";
constexpr std::string_view kDomainUuid = "019f0000-0000-7000-8000-000000070702";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath(std::string_view suffix) {
  return std::filesystem::temp_directory_path() /
         ("sbsql_transaction_policy_closure_" + std::string(suffix) + "_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810600000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810600001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810600002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "transaction policy closure database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-transaction-policy-closure";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000070711";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000070712";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("transaction_policy_closure");
  return context;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

std::string FirstDetail(const std::vector<api::EngineApiDiagnostic>& diagnostics) {
  return diagnostics.empty() ? std::string{} : diagnostics.front().detail;
}

api::EngineBeginTransactionResult BeginTransaction(const api::EngineRequestContext& context,
                                                   std::string isolation,
                                                   std::vector<std::string> options = {},
                                                   std::vector<std::string> profiles = {}) {
  api::EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = std::move(isolation);
  request.option_envelopes = std::move(options);
  request.transaction_policy_profile.encoded_profiles = std::move(profiles);
  return api::EngineBeginTransaction(request);
}

api::EngineCommitTransactionResult CommitTransaction(api::EngineRequestContext context,
                                                     const api::EngineBeginTransactionResult& begun) {
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return api::EngineCommitTransaction(request);
}

api::EngineRollbackTransactionResult RollbackTransaction(api::EngineRequestContext context,
                                                         const api::EngineBeginTransactionResult& begun) {
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return api::EngineRollbackTransaction(request);
}

mga::TransactionState LoadTransactionState(const std::filesystem::path& path,
                                           std::uint64_t local_transaction_id) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(path.string());
  Require(loaded.ok(), "transaction inventory load failed");
  const auto lookup = mga::LookupLocalTransaction(loaded.inventory,
                                                  mga::MakeLocalTransactionId(local_transaction_id));
  Require(lookup.ok(), "transaction inventory lookup failed");
  return lookup.entry.state;
}

mga::LocalTransactionInventory LoadInventory(const std::filesystem::path& path) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(path.string());
  Require(loaded.ok(), "transaction inventory load failed");
  return loaded.inventory;
}

void ExpectBeginFailure(const api::EngineRequestContext& context,
                        std::string isolation,
                        std::vector<std::string> options,
                        std::vector<std::string> profiles,
                        std::string_view expected_code,
                        std::string_view expected_detail) {
  const auto result = BeginTransaction(context,
                                       std::move(isolation),
                                       std::move(options),
                                       std::move(profiles));
  Require(!result.ok, "transaction.begin unexpectedly succeeded");
  Require(!result.diagnostics.empty(), "transaction.begin returned no diagnostic");
  Require(result.diagnostics.front().code == expected_code,
          "transaction.begin diagnostic code drifted");
  Require(result.diagnostics.front().detail == expected_detail,
          "transaction.begin diagnostic detail drifted");
}

void RequireSupportedBeginIsolations(const api::EngineRequestContext& context,
                                     const std::filesystem::path& path) {
  const std::vector<std::string> isolations = {
      "read_committed", "snapshot", "repeatable_read", "serializable", "read_consistency"};
  for (const auto& isolation : isolations) {
    const auto begun = BeginTransaction(context, isolation);
    Require(begun.ok, "supported transaction.begin isolation failed");
    Require(begun.isolation_level == isolation, "transaction.begin isolation evidence drifted");
    Require(begun.read_mode == "read_write", "transaction.begin default read mode drifted");
    Require(!begun.read_only, "transaction.begin defaulted to read-only");
    Require(HasEvidence(begun, "mga_authority", "durable_transaction_inventory"),
            "transaction.begin missing durable MGA evidence");
    Require(HasEvidence(begun, "transaction_state", "active"),
            "transaction.begin missing active state evidence");
    Require(HasEvidence(begun, "parser_finality", "false"),
            "transaction.begin did not prove parser non-finality");
    Require(LoadTransactionState(path, begun.local_transaction_id) == mga::TransactionState::active,
            "durable inventory did not record active transaction");
    const auto committed = CommitTransaction(context, begun);
    Require(committed.ok, "transaction.commit failed after supported begin isolation");
  }
}

void RequireReadOnlyMGAHelpers(const api::EngineRequestContext& context,
                               const std::filesystem::path& path,
                               const api::EngineBeginTransactionResult& read_only) {
  const auto inventory = LoadInventory(path);
  const auto local_id = mga::MakeLocalTransactionId(read_only.local_transaction_id);
  const auto lookup = mga::LookupLocalTransaction(inventory, local_id);
  Require(lookup.ok(), "read-only transaction inventory lookup failed");
  Require(lookup.entry.state == mga::TransactionState::read_only_active,
          "read-only transaction helper setup state drifted");

  const auto horizons = mga::ComputeLocalTransactionHorizons(inventory);
  Require(horizons.ok(), "read-only transaction horizons failed");
  Require(horizons.horizons.oldest_interesting_transaction.value == local_id.value,
          "read_only_active did not participate in oldest interesting transaction");
  Require(horizons.horizons.oldest_active_transaction.value == local_id.value,
          "read_only_active did not participate in oldest active transaction");

  const auto snapshot = mga::CreateLocalTransactionSnapshot(inventory, local_id);
  Require(snapshot.ok(), "read_only_active could not create local transaction snapshot");
  Require(snapshot.snapshot.reader_transaction.value == local_id.value,
          "read-only snapshot reader transaction drifted");
  Require(snapshot.snapshot.oldest_active_transaction.value == local_id.value,
          "read-only snapshot oldest active transaction drifted");

  const auto recovery = mga::ClassifyLocalTransactionForRecovery(lookup.entry);
  Require(recovery.action == mga::TransactionRecoveryAction::complete_rollback,
          "read_only_active recovery action did not classify as rollback");
  Require(recovery.stable_reason == "uncommitted_local_state",
          "read_only_active recovery reason drifted");
  const auto recovered = mga::ApplyLocalTransactionInventoryRecovery(inventory, CurrentUnixMillis());
  Require(recovered.ok(), "read_only_active recovery application failed");
  const auto recovered_lookup = mga::LookupLocalTransaction(recovered.recovered_inventory, local_id);
  Require(recovered_lookup.ok(), "read_only_active recovered lookup failed");
  Require(recovered_lookup.entry.state == mga::TransactionState::rolled_back,
          "read_only_active did not recover to rolled_back");

  const auto lineage = mga::BuildTransactionLineageEvidence(inventory, "schema_epoch_1", "snapshot_capsule_1");
  bool found_lineage = false;
  for (const auto& record : lineage) {
    if (record.local_id.value != local_id.value) { continue; }
    found_lineage = true;
    Require(record.observed_state == "read_only_active",
            "read_only_active lineage observed state drifted");
    Require(record.event_class == "read_only_begin",
            "read_only_active lineage event class drifted");
    Require(record.restore_classification == "restore_after_local_rollback_recovery",
            "read_only_active lineage restore classification drifted");
  }
  Require(found_lineage, "read_only_active lineage evidence missing");

  mga::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::row, 1779810600100).value;
  metadata.identity.creator_transaction = lookup.entry.identity;
  metadata.identity.version_sequence = 1;
  metadata.state = mga::RowVersionState::uncommitted;
  metadata.creator_transaction_state = mga::TransactionState::read_only_active;
  metadata.payload_present = true;
  const auto cleanup = mga::EvaluateLocalCleanupWithHorizons(metadata, horizons.horizons);
  Require(cleanup.diagnostic.message_key == "transaction.cleanup.blocked_by_unknown_outcome",
          "read_only_active cleanup outcome classification drifted");

  auto read_only_context = context;
  read_only_context.local_transaction_id = read_only.local_transaction_id;
  read_only_context.transaction_uuid = read_only.transaction_uuid;
  const auto crud_state = api::LoadCrudState(read_only_context);
  Require(!crud_state.ok, "read-only CRUD overlay unexpectedly admitted mutation authority");
  const auto crud_tx = crud_state.state.transactions.find(read_only.local_transaction_id);
  Require(crud_tx != crud_state.state.transactions.end(),
          "CRUD overlay did not expose read-only transaction");
  Require(crud_tx->second == "read_only_active",
          "CRUD overlay did not render read_only_active");
  Require(crud_state.diagnostic.detail == "crud.transaction_authority:active_local_transaction_required",
          "CRUD overlay read-only refusal diagnostic drifted");

  const auto mga_state = api::LoadMgaRelationStoreState(read_only_context);
  Require(!mga_state.ok, "read-only MGA overlay unexpectedly admitted mutation authority");
  const auto mga_tx = mga_state.state.crud_metadata.transactions.find(read_only.local_transaction_id);
  Require(mga_tx != mga_state.state.crud_metadata.transactions.end(),
          "MGA overlay did not expose read-only transaction");
  Require(mga_tx->second == "read_only_active",
          "MGA overlay did not render read_only_active");
  Require(mga_state.diagnostic.detail == "mga.transaction_authority:active_local_transaction_required",
          "MGA overlay read-only refusal diagnostic drifted");
}

void RequireBeginReadOnlyModes(const api::EngineRequestContext& context,
                               const std::filesystem::path& path) {
  const auto explicit_read_write =
      BeginTransaction(context, "read_committed", {"transaction_read_only:false"});
  Require(explicit_read_write.ok, "transaction.begin read_only:false failed");
  Require(explicit_read_write.read_mode == "read_write",
          "transaction.begin read_only:false read mode drifted");
  Require(!explicit_read_write.read_only, "transaction.begin read_only:false overclaimed read-only");
  Require(HasEvidence(explicit_read_write, "transaction_read_only", "false"),
          "transaction.begin read_only:false evidence missing");
  Require(LoadTransactionState(path, explicit_read_write.local_transaction_id) ==
              mga::TransactionState::active,
          "read_only:false did not persist active transaction state");
  Require(CommitTransaction(context, explicit_read_write).ok,
          "commit failed for read_only:false transaction");

  const auto read_only =
      BeginTransaction(context, "snapshot", {"transaction_read_mode:read_only"});
  Require(read_only.ok, "transaction.begin read_only mode failed");
  Require(read_only.read_mode == "read_only", "transaction.begin read-only read mode drifted");
  Require(read_only.read_only, "transaction.begin read-only flag missing");
  Require(HasEvidence(read_only, "transaction_state", "read_only_active"),
          "transaction.begin missing read_only_active evidence");
  Require(HasEvidence(read_only, "read_only_write_guard", "mga_transaction_state"),
          "transaction.begin missing read-only write guard evidence");
  Require(LoadTransactionState(path, read_only.local_transaction_id) ==
              mga::TransactionState::read_only_active,
          "durable inventory did not record read_only_active state");
  RequireReadOnlyMGAHelpers(context, path, read_only);

  auto read_only_context = context;
  read_only_context.local_transaction_id = read_only.local_transaction_id;
  read_only_context.transaction_uuid = read_only.transaction_uuid;
  api::EngineCreateDomainRequest create;
  create.context = read_only_context;
  create.target_schema.uuid.canonical = std::string(kSchemaUuid);
  create.target_schema.object_kind = "schema";
  create.target_object.uuid.canonical = std::string(kDomainUuid);
  create.target_object.object_kind = "domain";
  create.localized_names.push_back({"en", "primary", "", "readonly_refused_domain", true});
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "type=text";
  create.descriptors.push_back(descriptor);
  const auto create_result = api::EngineCreateDomain(create);
  Require(!create_result.ok, "read-only transaction unexpectedly allowed domain mutation");
  Require(FirstDetail(create_result.diagnostics) ==
              "crud.transaction_authority:active_local_transaction_required",
          "read-only transaction mutation guard diagnostic drifted");

  const auto committed = CommitTransaction(context, read_only);
  Require(committed.ok, "read-only transaction commit failed");
  Require(HasEvidence(committed, "transaction_state", "committed"),
          "read-only transaction commit evidence missing");

  const auto profile_read_only =
      BeginTransaction(context, "read_committed", {}, {"read_only:true"});
  Require(profile_read_only.ok, "transaction.begin read_only:true profile failed");
  Require(profile_read_only.read_only, "read_only:true profile did not set read-only");
  Require(LoadTransactionState(path, profile_read_only.local_transaction_id) ==
              mga::TransactionState::read_only_active,
          "read_only:true profile did not persist read_only_active state");
  Require(RollbackTransaction(context, profile_read_only).ok,
          "rollback failed for read_only:true profile transaction");
}

void RequireBeginRefusals(const api::EngineRequestContext& context) {
  ExpectBeginFailure(context,
                     "dirty_read",
                     {},
                     {},
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "transaction.begin:unsupported_isolation_level");
  ExpectBeginFailure(context,
                     "read_committed",
                     {"transaction_read_mode:analytics"},
                     {},
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "transaction.begin:unsupported_transaction_read_mode");
  ExpectBeginFailure(context,
                     "read_committed",
                     {"transaction_read_only:maybe"},
                     {},
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "transaction.begin:unsupported_transaction_read_only_value");
  ExpectBeginFailure(context,
                     "read_committed",
                     {"transaction_read_mode:read_only", "transaction_read_only:false"},
                     {},
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "transaction.begin:transaction_read_mode_conflict");
  ExpectBeginFailure(context,
                     "read_committed",
                     {},
                     {"dormant_reattach:true"},
                     "SB-SNTXN-DORMANT-UNSUPPORTED",
                     "dormant_reattach:true");
  ExpectBeginFailure(context,
                     "read_committed",
                     {},
                     {"abandoned_cleanup:true"},
                     "SB-SNTXN-ABANDONED-UNSUPPORTED",
                     "abandoned_cleanup:true");
}

api::EngineSetTransactionCharacteristicsResult SetCharacteristics(
    const api::EngineRequestContext& context,
    std::vector<std::string> options = {}) {
  api::EngineSetTransactionCharacteristicsRequest request;
  request.context = context;
  request.option_envelopes = std::move(options);
  return api::EngineSetTransactionCharacteristics(request);
}

void ExpectSetFailure(const api::EngineRequestContext& context,
                      std::vector<std::string> options,
                      std::string_view expected_code,
                      std::string_view expected_detail) {
  const auto result = SetCharacteristics(context, std::move(options));
  Require(!result.ok, "transaction.set_characteristics unexpectedly succeeded");
  Require(!result.diagnostics.empty(), "transaction.set_characteristics returned no diagnostic");
  Require(result.diagnostics.front().code == expected_code,
          "transaction.set_characteristics diagnostic code drifted");
  Require(result.diagnostics.front().detail == expected_detail,
          "transaction.set_characteristics diagnostic detail drifted");
}

void RequireReadOnlyContextCannotWiden(api::EngineRequestContext context) {
  context.read_only_mode = true;

  const auto default_begin = BeginTransaction(context, "read_committed");
  Require(default_begin.ok, "read-only context default begin failed");
  Require(default_begin.read_only, "read-only context default begin did not stay read-only");
  Require(default_begin.read_mode == "read_only",
          "read-only context default begin read mode drifted");
  Require(RollbackTransaction(context, default_begin).ok,
          "rollback failed for read-only context default begin");

  ExpectBeginFailure(context,
                     "read_committed",
                     {"transaction_read_mode:read_write"},
                     {},
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "transaction.begin:transaction_read_mode_conflict");
  ExpectBeginFailure(context,
                     "read_committed",
                     {"transaction_read_only:false"},
                     {},
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "transaction.begin:transaction_read_mode_conflict");

  const auto default_characteristics = SetCharacteristics(context);
  Require(default_characteristics.ok, "read-only context set-characteristics default failed");
  Require(HasEvidence(default_characteristics, "transaction_read_mode", "read_only"),
          "read-only context set-characteristics default read mode drifted");
  Require(HasEvidence(default_characteristics, "transaction_read_only", "true"),
          "read-only context set-characteristics default read-only evidence missing");
  ExpectSetFailure(context,
                   {"transaction_read_mode:read_write"},
                   "SB_ENGINE_API_INVALID_REQUEST",
                   "transaction.set_characteristics:transaction_read_mode_conflict");
  ExpectSetFailure(context,
                   {"transaction_read_only:false"},
                   "SB_ENGINE_API_INVALID_REQUEST",
                   "transaction.set_characteristics:transaction_read_mode_conflict");
}

void RequireSetCharacteristics(const api::EngineRequestContext& context,
                               const std::filesystem::path& path) {
  const std::vector<std::string> isolations = {
      "read_committed", "snapshot", "repeatable_read", "serializable", "read_consistency"};
  for (const auto& isolation : isolations) {
    const auto result =
        SetCharacteristics(context, {"transaction_isolation_level:" + isolation});
    Require(result.ok, "set-characteristics supported isolation failed");
    Require(HasEvidence(result, "transaction_isolation_level", isolation),
            "set-characteristics isolation evidence missing");
    Require(HasEvidence(result, "mga_authority", "session_default_only_no_finality"),
            "set-characteristics overclaimed finality");
    Require(HasEvidence(result, "parser_finality", "false"),
            "set-characteristics did not prove parser non-finality");
  }

  const auto read_write = SetCharacteristics(context, {"transaction_read_mode:read_write"});
  Require(read_write.ok, "set-characteristics read_write failed");
  Require(HasEvidence(read_write, "transaction_read_mode", "read_write"),
          "set-characteristics read_write evidence missing");

  const auto read_only = SetCharacteristics(context, {"transaction_read_mode:read_only"});
  Require(read_only.ok, "set-characteristics read_only failed");
  Require(HasEvidence(read_only, "transaction_read_mode", "read_only"),
          "set-characteristics read_only evidence missing");
  Require(HasEvidence(read_only, "transaction_read_only", "true"),
          "set-characteristics read_only bool evidence missing");

  const auto read_only_bool = SetCharacteristics(context, {"transaction_read_only:true"});
  Require(read_only_bool.ok, "set-characteristics transaction_read_only:true failed");
  Require(HasEvidence(read_only_bool, "transaction_read_only", "true"),
          "set-characteristics transaction_read_only:true evidence missing");

  const auto read_write_bool = SetCharacteristics(context, {"transaction_read_only:false"});
  Require(read_write_bool.ok, "set-characteristics transaction_read_only:false failed");
  Require(HasEvidence(read_write_bool, "transaction_read_only", "false"),
          "set-characteristics transaction_read_only:false evidence missing");

  ExpectSetFailure(context,
                   {"transaction_read_mode:analytics"},
                   "SB_ENGINE_API_INVALID_REQUEST",
                   "transaction.set_characteristics:unsupported_transaction_read_mode");
  ExpectSetFailure(context,
                   {"transaction_read_only:maybe"},
                   "SB_ENGINE_API_INVALID_REQUEST",
                   "transaction.set_characteristics:unsupported_transaction_read_only_value");
  ExpectSetFailure(context,
                   {"transaction_read_mode:read_only", "transaction_read_only:false"},
                   "SB_ENGINE_API_INVALID_REQUEST",
                   "transaction.set_characteristics:transaction_read_mode_conflict");
  ExpectSetFailure(context,
                   {"transaction_isolation_level:dirty_read"},
                   "SB_ENGINE_API_INVALID_REQUEST",
                   "transaction.set_characteristics:unsupported_isolation_level");

  const auto begun = BeginTransaction(context, "read_committed");
  Require(begun.ok, "begin for active set-characteristics refusal failed");
  auto active_context = context;
  active_context.local_transaction_id = begun.local_transaction_id;
  active_context.transaction_uuid = begun.transaction_uuid;
  ExpectSetFailure(active_context,
                   {},
                   "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
                   "active_transaction_already_bound");
  Require(LoadTransactionState(path, begun.local_transaction_id) ==
              mga::TransactionState::active,
          "active set-characteristics refusal setup did not persist active state");
  Require(RollbackTransaction(context, begun).ok,
          "rollback failed after active set-characteristics refusal");
}

}  // namespace

int main() {
  const auto path = TestDatabasePath("main");
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = EngineContext(path, database_uuid);

  RequireSupportedBeginIsolations(context, path);
  RequireBeginReadOnlyModes(context, path);
  RequireBeginRefusals(context);
  RequireSetCharacteristics(context, path);
  RequireReadOnlyContextCannotWiden(context);

  RemoveDatabaseArtifacts(path);
  std::cout << "sbsql_transaction_policy_closure_conformance=passed\n";
  return EXIT_SUCCESS;
}
