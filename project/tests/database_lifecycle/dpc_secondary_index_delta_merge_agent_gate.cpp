// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_evidence_fixture.hpp"
#include "../support/published_mga_table_fixture.hpp"
#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "memory.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "security/security_model.hpp"
#include <memory>
#include "secondary_index_delta_ledger.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kMergeSearchKey =
    "DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "DPC-024 generated UUID creation failed");
  return generated.value;
}

api::EngineUuid NewIdentity(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string name) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue("note")});
  return row;
}

std::vector<std::string> DeferredOptions() {
  return {"runtime.deferred_secondary_index=enabled",
          "feature.secondary_index_delta_ledger=enabled",
          "delta_ledger.reader_overlay=enabled",
          "delta_ledger.cleanup_horizon_bound=true",
          "delta_ledger.recovery_classifiable=true"};
}

class ProbeSession {
 public:
  explicit ProbeSession(const api::EngineRequestContext& context) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = context.database_path.data();
    open.database_path_size = context.database_path.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Check(sb_engine_open(&open, &engine_, nullptr), nullptr, "engine open");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    std::copy(context.principal_uuid.bytes.begin(), context.principal_uuid.bytes.end(),
              begin.effective_user_uuid.bytes);
    std::copy(context.session_uuid.bytes.begin(), context.session_uuid.bytes.end(),
              begin.session_uuid.bytes);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Check(sb_engine_session_begin(engine_, &begin, &session_, nullptr), nullptr,
          "session begin");
  }
  ~ProbeSession() {
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    (void)sb_engine_session_end(session_, &end, nullptr);
    (void)sb_engine_close(engine_, nullptr);
  }
  ProbeSession(const ProbeSession&) = delete;
  ProbeSession& operator=(const ProbeSession&) = delete;
  sb_engine_session_t get() const { return session_; }
  static void Check(sb_engine_status_t status, sb_engine_result_t result,
                    const char* phase) {
    if (status != SB_ENGINE_STATUS_OK && result != nullptr) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK) {
        for (std::size_t i = 0; i < diagnostics.diagnostic_count; ++i) {
          const auto& d = diagnostics.diagnostics[i];
          for (const auto text : {d.symbolic_code, d.message_key, d.safe_detail}) {
            if (text.data) std::cerr.write(text.data, text.size_bytes);
            std::cerr << ':';
          }
          std::cerr << '\n';
        }
      }
    }
    if (result) sb_engine_result_release(result);
    if (status != SB_ENGINE_STATUS_OK)
      Fail(std::string("unique probe fixture ") + phase + " failed");
  }
 private:
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

class ProbeStatement {
 public:
  ProbeStatement(const ProbeSession& session, const api::EngineRequestContext& base) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base;
    request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(
        session.get(), &request, &receipt_, &view, &result);
    ProbeSession::Check(status, result, "statement acquisition");
    result = nullptr;
    const auto copied = bridge::CopyStatementContextEngineContextV1(
        receipt_, &context, &result);
    ProbeSession::Check(copied, result, "statement context copy");
  }
  ~ProbeStatement() {
    (void)scratchbird::server_engine_bridge::ReleaseStatementContextReceipt(receipt_);
  }
  ProbeStatement(const ProbeStatement&) = delete;
  ProbeStatement& operator=(const ProbeStatement&) = delete;
  api::EngineRequestContext context;
 private:
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt_;
};

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineUuid filespace_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<ProbeSession> session;
  api::EngineUuid table_uuid;
  api::EngineUuid non_unique_index_uuid;
  api::EngineUuid unique_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.default_root_uuid = fixture.filespace_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "DPC-024 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-024 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-024 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc024_delta_merge_agent";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           api::EngineUuid index_uuid,
                           std::string column,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  // Deferred insertion currently admits non-unique hash indexes. Keep the
  // unique B-tree on its exact synchronous maintenance path.
  index.family = unique ? api::kCrudIndexFamilyBtree : api::kCrudIndexFamilyHash;
  index.profile = unique ? api::kCrudIndexProfileRowStoreScalarBtreeV1 : std::string{};
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc024_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc024.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = true;
  create.resource_seed_pack_root =
      (std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path()
          .parent_path() / "resources/seed-packs/initial-resource-pack").string();
  create.bootstrap_principal_name = "dpc_merge_owner";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DPC-024 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.filespace_uuid = create.filespace_uuid.value;
  auto& owner = fixture.owner_context;
  owner.database_uuid = fixture.database_uuid;
  owner.database_path = fixture.database_path.string();
  owner.default_root_uuid = created.state.filespace_uuid.value;
  owner.session_uuid = NewIdentity(platform::UuidKind::object, 0);
  owner.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  owner.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  owner.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(owner.database_path);
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "IPAR unique-probe durable bootstrap principal unavailable");
  owner.principal_uuid = bootstrap.state.principal_uuid.value;
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(owner);
  Require(loaded.ok, "IPAR unique-probe durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  api::DurableAuthorizationState authority;
  authority.authority_uuid = owner.database_uuid;
  authority.security_context_generation = lifecycle.security_context_generation;
  authority.security_epoch = lifecycle.security_generation;
  authority.policy_epoch = lifecycle.policy_generation;
  authority.catalog_generation_id = 1;
  authority.engine_owned_sysarch_role_uuid = bootstrap.state.sysarch_role_uuid.value;
  for (const auto& principal : lifecycle.principals)
    if (!principal.deleted && principal.lifecycle_state == "active")
      authority.principals.push_back({principal.principal_uuid, "principal", true,
                                     authority.security_epoch});
  for (const auto& role : lifecycle.roles)
    if (!role.deleted && role.lifecycle_state == "active")
      authority.roles.push_back({role.role_uuid, true, authority.security_epoch});
  for (const auto& membership : lifecycle.memberships)
    if (!membership.revoked)
      authority.memberships.push_back({membership.member_principal_uuid, "principal",
          membership.container_uuid, membership.container_kind, true, authority.security_epoch});
  for (const auto& grant : lifecycle.grants)
    if (!grant.revoked)
      authority.grants.push_back({grant.grant_uuid, grant.grantee_uuid, grant.grantee_kind,
          grant.target_object_uuid, grant.privilege, grant.grant_effect == "deny", true,
          authority.security_epoch});
  const auto materialized = api::MaterializeDurableAuthorizationContext(authority,
      {owner.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  Require(materialized.ok, "IPAR unique-probe durable authorization unavailable");
  owner.authorization_context = materialized.context;

  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewIdentity(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewIdentity(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc024-metadata");
  RequireDiagnosticOk(scratchbird::tests::PublishMgaTableFixture(
      context, Table(fixture, context), {"character", "character", "character"},
      {Index(fixture, context, fixture.non_unique_index_uuid, "name", false),
       Index(fixture, context, fixture.unique_index_uuid, "id", true)}),
                      "DPC-024 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid,
                                "name", false)),
                      "DPC-024 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid,
                                "id", true)),
                      "DPC-024 unique index metadata append failed");
  Commit(context);
  fixture.owner_context.current_schema_uuid = context.current_schema_uuid;
  fixture.session = std::make_shared<ProbeSession>(BaseContext(fixture, "merge-session"));
  return fixture;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string name,
                                      std::vector<std::string> options = {}) {
  ProbeStatement statement(*fixture.session, context);
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(name)));
  request.estimated_row_count = 1;
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

api::EngineSelectRowsResult SelectEquals(const Fixture& fixture,
                                         const api::EngineRequestContext& context,
                                         std::string column,
                                         std::string value) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = std::move(column);
  request.select_predicate.bound_values.push_back(TextValue(std::move(value)));
  return api::EngineSelectRows(request);
}

api::EngineUuid SeedCommittedRow(Fixture& fixture,
                             std::string id,
                             std::string name,
                             bool deferred) {
  auto context = Begin(fixture, "dpc024-seed");
  const auto inserted = InsertRow(fixture, context, std::move(id), std::move(name),
                                  deferred ? DeferredOptions() : std::vector<std::string>{});
  RequireOk(inserted, "DPC-024 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "DPC-024 seed row UUID missing");
  if (deferred) {
    const auto ledger = api::LoadMgaSecondaryIndexDeltaLedger(context);
    Require(ledger.ok && !ledger.ledger.records.empty(),
            "DPC-024 deferred seed did not publish an index delta");
  }
  Commit(context);
  return inserted.row_uuids.front();
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) { return true; }
  }
  return false;
}

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) { return scratchbird::tests::EvidenceTextFields(entry.evidence_id); }
  }
  return {};
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(BaseContext(fixture, "dpc024-load-ledger"));
  Require(loaded.ok, "DPC-024 ledger load failed");
  return loaded.ledger;
}

platform::u64 MaxLedgerLocalTransactionId(
    const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  platform::u64 max_tx = 0;
  for (const auto& record : ledger.records) {
    max_tx = std::max(max_tx, record.delta.local_transaction_id);
  }
  return max_tx;
}

api::MgaSecondaryIndexDeltaMergeAgentRequest MergeRequest(
    const Fixture& fixture,
    platform::u64 horizon,
    bool authoritative = true) {
  api::MgaSecondaryIndexDeltaMergeAgentRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  request.cleanup_horizon_authoritative = authoritative;
  request.max_records_to_scan = 16;
  request.max_records_to_merge = 16;
  return request;
}

bool HasMergedCleanedRecord(const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  for (const auto& record : ledger.records) {
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned) {
      return true;
    }
  }
  return false;
}

void ValidateAuthoritativeMergeDrainsIntoBase() {
  auto fixture = MakeFixture("authoritative_merge", 1000);
  (void)SeedCommittedRow(fixture, "id-base", "alpha", false);
  const auto delta_row = SeedCommittedRow(fixture, "id-delta", "alpha", true);
  auto active_sync = Begin(fixture, "dpc024-active-sync");
  RequireOk(InsertRow(fixture, active_sync, "id-active-sync", "bravo", {}),
            "DPC-024 active synchronous insert failed");

  auto before_reader = Begin(fixture, "dpc024-before-reader");
  const auto before = SelectEquals(fixture, before_reader, "name", "alpha");
  RequireOk(before, "DPC-024 pre-merge select failed");
  Require(before.visible_count == 2, "DPC-024 pre-merge overlay did not see both rows");
  if (!HasEvidence(before.evidence, "mga_secondary_index_delta_overlay_used"))
    for (const auto& item : before.evidence)
      std::cerr << item.evidence_kind << ":"
                << scratchbird::tests::EvidenceTextFields(item.evidence_id) << "\n";
  Require(HasEvidence(before.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-024 pre-merge overlay evidence missing");
  Rollback(before_reader);

  const auto count_delta_base_entries = [&]() {
    const auto state = api::LoadMgaRelationStoreState(
        BaseContext(fixture, "dpc024-base-entry-check"));
    Require(state.ok, "DPC-024 base entry verification failed");
    std::size_t count = 0;
    for (const auto& entry : state.state.index_entries) {
      if (entry.index_uuid == fixture.non_unique_index_uuid && entry.row_uuid == delta_row)
        ++count;
    }
    return count;
  };
  Require(count_delta_base_entries() == 1,
          "DPC-024 auxiliary delta bypassed the mandatory transactional base entry");
  const auto ledger_before = LoadLedger(fixture);
  const auto merge = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-merge"),
      MergeRequest(fixture, MaxLedgerLocalTransactionId(ledger_before)));
  Require(merge.ok, "DPC-024 authoritative merge failed");
  Require(merge.merged_count == 1, "DPC-024 merge count changed");
  Require(merge.cleaned_count == 1, "DPC-024 cleaned count changed");
  Require(EvidenceValue(merge.evidence, "mga_secondary_index_delta_merge_result") ==
              "successful_merge",
          "DPC-024 successful merge evidence missing");
  Require(HasMergedCleanedRecord(LoadLedger(fixture)),
          "DPC-024 merged ledger state was not retained deterministically");
  Require(count_delta_base_entries() == 1,
          "DPC-024 idempotent merge duplicated or dropped the existing base entry");
  const auto repeated = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-repeated-merge"),
      MergeRequest(fixture, MaxLedgerLocalTransactionId(ledger_before)));
  Require(repeated.ok && repeated.merged_count == 0 && repeated.cleaned_count == 0 &&
              count_delta_base_entries() == 1,
          "DPC-024 repeated merge re-counted cleaned records or changed the base entry");
  Commit(active_sync);

  auto after_reader = Begin(fixture, "dpc024-after-reader");
  const auto after = SelectEquals(fixture, after_reader, "name", "alpha");
  RequireOk(after, "DPC-024 post-merge select failed");
  Require(after.visible_count == 2, "DPC-024 post-merge base lookup lost rows");
  Require(!HasEvidence(after.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-024 post-merge lookup still required overlay");
  Require(EvidenceValue(after.evidence, "mga_secondary_index_lookup_path") ==
              "non_unique_synchronous_no_delta",
          "DPC-024 post-merge lookup did not use base-only path");
  Rollback(after_reader);

  auto active_reader = Begin(fixture, "dpc024-active-sync-reader");
  const auto active_after = SelectEquals(fixture, active_reader, "name", "bravo");
  RequireOk(active_after, "DPC-024 active synchronous post-merge select failed");
  Require(active_after.visible_count == 1,
          "DPC-024 merge rewrite dropped an active synchronous index entry");
  Require(EvidenceValue(active_after.evidence, "mga_secondary_index_lookup_path") ==
              "non_unique_synchronous_no_delta",
          "DPC-024 active synchronous preserved entry did not use base index path");
  Rollback(active_reader);
}

void ValidateHorizonRetainsFutureAndResourceRefuses() {
  auto fixture = MakeFixture("horizon_resource", 2000);
  (void)SeedCommittedRow(fixture, "id-one", "alpha", true);
  (void)SeedCommittedRow(fixture, "id-two", "bravo", true);

  const auto ledger = LoadLedger(fixture);
  Require(ledger.records.size() == 2, "DPC-024 resource fixture ledger size changed");
  auto throttled = MergeRequest(fixture, MaxLedgerLocalTransactionId(ledger));
  throttled.max_records_to_scan = 1;
  const auto refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-resource-refusal"),
      throttled);
  Require(!refused.ok, "DPC-024 resource governor accepted over-budget scan");
  Require(refused.diagnostic.code == "resource_governor_throttled",
          "DPC-024 resource governor diagnostic changed");

  const platform::u64 low_horizon =
      std::min(ledger.records[0].delta.local_transaction_id,
               ledger.records[1].delta.local_transaction_id);
  const auto partial = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-horizon-retain"),
      MergeRequest(fixture, low_horizon));
  Require(partial.ok, "DPC-024 bounded horizon merge failed");
  Require(partial.retained_count == 1, "DPC-024 above-horizon delta was not retained");
  Require(partial.cleaned_count == 1, "DPC-024 eligible delta was not cleaned");

  auto budget_fixture = MakeFixture("merge_budget", 2500);
  (void)SeedCommittedRow(budget_fixture, "id-a", "alpha", true);
  (void)SeedCommittedRow(budget_fixture, "id-b", "alpha", true);
  const auto budget_ledger = LoadLedger(budget_fixture);
  auto budgeted = MergeRequest(budget_fixture,
                               MaxLedgerLocalTransactionId(budget_ledger));
  budgeted.max_records_to_merge = 1;
  const auto partial_budget = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(budget_fixture, "dpc024-merge-budget"),
      budgeted);
  Require(partial_budget.ok, "DPC-024 merge-budget throttle refused all work");
  Require(partial_budget.throttled,
          "DPC-024 merge-budget throttle flag was not set");
  Require(partial_budget.throttle_or_refusal_reason == "resource_governor_throttled",
          "DPC-024 merge-budget throttle reason changed");
  Require(partial_budget.retained_count == 1,
          "DPC-024 merge-budget retained count changed");
  Require(partial_budget.cleaned_count == 1,
          "DPC-024 merge-budget cleaned count changed");
}

void ValidateRefusalDiagnostics() {
  auto fixture = MakeFixture("refusals", 3000);
  (void)SeedCommittedRow(fixture, "id-delta", "alpha", true);
  const auto horizon = MaxLedgerLocalTransactionId(LoadLedger(fixture));

  const auto no_authority = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-no-authority"),
      MergeRequest(fixture, horizon, false));
  Require(!no_authority.ok, "DPC-024 accepted non-authoritative horizon");
  Require(no_authority.diagnostic.code == "not_authoritative_horizon",
          "DPC-024 not-authoritative diagnostic changed");

  auto disabled = MergeRequest(fixture, horizon);
  disabled.merge_disabled = true;
  const auto disabled_refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-disabled"),
      disabled);
  Require(!disabled_refused.ok, "DPC-024 accepted disabled merge agent");
  Require(disabled_refused.diagnostic.code == "merge_agent_disabled",
          "DPC-024 disabled-agent diagnostic changed");

  auto zero_budget = MergeRequest(fixture, horizon);
  zero_budget.max_records_to_merge = 0;
  const auto zero_refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-zero-budget"),
      zero_budget);
  Require(!zero_refused.ok, "DPC-024 accepted zero merge budget");
  Require(zero_refused.throttled,
          "DPC-024 zero-budget throttle flag was not set");
  Require(zero_refused.diagnostic.code == "resource_governor_throttled",
          "DPC-024 zero-budget diagnostic changed");

  auto unique = MergeRequest(fixture, horizon);
  unique.index_uuid = fixture.unique_index_uuid;
  const auto unique_refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-unique"),
      unique);
  Require(!unique_refused.ok, "DPC-024 accepted unique-index delta merge");
  Require(unique_refused.diagnostic.code == "unique_index_delta_refused",
          "DPC-024 unique refusal diagnostic changed");

  const auto path = fixture.database_path.string() +
                    ".sb.mga_secondary_index_delta_ledger";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "DPC024_CORRUPT_LEDGER";
  out.flush();
  Require(static_cast<bool>(out), "DPC-024 corrupt ledger write failed");
  const auto corrupt = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-corrupt"),
      MergeRequest(fixture, horizon));
  Require(!corrupt.ok, "DPC-024 accepted corrupt ledger");
  Require(corrupt.diagnostic.code == "corrupt_ledger_refused",
          "DPC-024 corrupt ledger diagnostic changed");
}

}  // namespace

int main() {
  auto policy=scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name="secondary_index_merge_admission_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy,"secondary_index_merge_admission_fixture").ok(),
          "secondary index fixture memory policy failed");
  Require(kMergeSearchKey == "DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE",
          "DPC-024 gate search key drifted");
  ValidateAuthoritativeMergeDrainsIntoBase();
  ValidateHorizonRetainsFutureAndResourceRefuses();
  ValidateRefusalDiagnostics();
  return EXIT_SUCCESS;
}
