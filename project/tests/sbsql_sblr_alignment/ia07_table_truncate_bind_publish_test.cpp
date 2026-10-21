// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// BindStatementTableTruncateTargetV1 resolve-and-publish half, over a real
// published table in a real database.
//
// SBSQL-TRUNCATE-TABLE-V1 section 4 steps 1-3: the engine resolves the explicit
// target itself and captures the engine-owned relation descriptor. This gate
// proves the engine resolved the name to the actual published table and
// published occurrence -> persisted descriptor onto the live receipt.
//
// Publication is proven indirectly and deliberately: the attach authority is
// once-only and refuses when the receipt already carries mappings, so a second
// bind on the same receipt refusing is proof the first one published. That is
// stronger than reading back a copy, because the server's own view copy is
// taken at acquisition and would not show a later publish at all.

#include "../support/native_catalog_column_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "database_lifecycle.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;

int checks = 0;
void Check(bool pass, std::string_view detail) {
  ++checks;
  if (!pass) {
    std::cerr << "FAIL table truncate bind publish: " << detail << '\n';
    std::exit(EXIT_FAILURE);
  }
}

constexpr std::uint64_t kEpochMillis = 1789310000000ULL;

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  namespace uuid = scratchbird::core::uuid;
  static std::atomic<std::uint64_t> ordinal{1};
  const auto timestamp = kEpochMillis + ordinal.fetch_add(1);
  // Session identities are not engine identities, so they take the
  // compatibility generator and are typed afterwards.
  const auto generated = [&]() {
    if (kind != platform::UuidKind::session) {
      return uuid::GenerateEngineIdentityV7(kind, timestamp);
    }
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(timestamp);
    if (!raw.ok()) {
      uuid::TypedUuidResult failed;
      failed.status = raw.status;
      failed.diagnostic = raw.diagnostic;
      return failed;
    }
    return uuid::MakeTypedUuid(kind, raw.value);
  }();
  Check(generated.ok(), "fixture identity generation failed");
  return generated.value;
}
platform::Uuid Identity(const platform::TypedUuid& value) { return value.value; }
sb_engine_uuid_t PublicUuid(const platform::TypedUuid& value) {
  sb_engine_uuid_t out{};
  std::copy(value.value.bytes.begin(), value.value.bytes.end(), out.bytes);
  return out;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  platform::TypedUuid database = NewUuid(platform::UuidKind::database);
  platform::TypedUuid filespace = NewUuid(platform::UuidKind::filespace);
  platform::TypedUuid principal = NewUuid(platform::UuidKind::principal);
  platform::TypedUuid session = NewUuid(platform::UuidKind::session);
  platform::TypedUuid schema = NewUuid(platform::UuidKind::schema);
  platform::TypedUuid relation = NewUuid(platform::UuidKind::object);
  api::EngineRequestContext transaction;
  api::MgaRelationStorageDescriptor descriptor;
};

constexpr const char* kTargetName = "truncate_bind_target";

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "table-truncate-bind-publish";
  context.database_path = fixture.database_path.string();
  context.database_uuid = Identity(fixture.database);
  context.database_page_size_bytes = 16384;
  context.default_root_uuid = Identity(fixture.filespace);
  context.current_schema_uuid = Identity(fixture.schema);
  context.principal_uuid = Identity(fixture.principal);
  context.session_uuid = Identity(fixture.session);
  context.identifier_profile_uuid = "sbsql_v3";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

std::unique_ptr<Fixture> MakeFixture() {
  auto fixture = std::make_unique<Fixture>();
  fixture->directory = std::filesystem::temp_directory_path() /
      ("scratchbird_truncate_bind_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(fixture->directory);
  fixture->database_path = fixture->directory / "truncate_bind.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture->database_path.string();
  create.database_uuid = fixture->database;
  create.filespace_uuid = fixture->filespace;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = kEpochMillis;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << "create refused: " << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Check(created.ok(), "database creation failed");
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(create.path);
  Check(bootstrap.ok() && bootstrap.state.present &&
            bootstrap.state.committed_by_inventory,
        "durable bootstrap principal missing");
  fixture->principal = bootstrap.state.principal_uuid;

  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(*fixture);
  begin.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(begin);
  Check(begun.ok, "transaction begin failed");
  fixture->transaction = begin.context;
  fixture->transaction.local_transaction_id = begun.local_transaction_id;
  fixture->transaction.transaction_uuid = begun.transaction_uuid;
  fixture->transaction.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  fixture->transaction.transaction_isolation_level = begun.isolation_level;

  // A table carrying one column. Truncate admits indexed tables too, but the
  // bind only needs a resolvable relation with a published descriptor.
  api::CrudTableRecord table;
  table.table_uuid = Identity(fixture->relation);
  table.default_name = kTargetName;
  table.columns = {{"id", scratchbird::tests::NativeCatalogColumnFixture({
      {{"type", "int32"}, {"nullable", "false"}},
      {{"datatype_descriptor_uuid",
        scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716")},
       {"type_uuid",
        scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d717")}}})}};
  fixture->transaction.current_schema_uuid = {};
  Check(!scratchbird::tests::PublishMgaTableFixture(
             fixture->transaction, table, {"int32"}).error,
        "published bound table cohort failed");
  fixture->schema = {platform::UuidKind::schema,
                     fixture->transaction.current_schema_uuid};
  const auto loaded =
      api::LoadMgaRelationStorageDescriptor(fixture->transaction, table.table_uuid);
  Check(loaded.ok, "published relation descriptor unavailable");
  fixture->descriptor = loaded.descriptor;
  const auto epochs =
      api::LoadCatalogObjectLifecycleEpochState(fixture->transaction);
  Check(epochs.ok && epochs.state.metadata_epoch &&
            epochs.state.name_resolution_epoch,
        "published catalog epochs unavailable");
  fixture->transaction.catalog_generation_id = epochs.state.metadata_epoch;
  fixture->transaction.name_resolution_epoch = epochs.state.name_resolution_epoch;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(fixture->transaction);
  return fixture;
}

class Session {
 public:
  explicit Session(const Fixture& fixture) : path_(fixture.database_path.string()) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = path_.data();
    open.database_path_size = path_.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Check(sb_engine_open(&open, &engine_, nullptr) == SB_ENGINE_STATUS_OK,
          "engine open failed");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    begin.effective_user_uuid = PublicUuid(fixture.principal);
    begin.session_uuid = PublicUuid(fixture.session);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Check(sb_engine_session_begin(engine_, &begin, &session_, nullptr) ==
              SB_ENGINE_STATUS_OK,
          "engine session begin failed");
  }
  ~Session() {
    if (session_ != nullptr) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end.rollback_active_transactions = 1;
      end.cancel_open_results = 1;
      (void)sb_engine_session_end(session_, &end, nullptr);
    }
    if (engine_ != nullptr) (void)sb_engine_close(engine_, nullptr);
  }
  sb_engine_session_t get() const { return session_; }

 private:
  std::string path_;
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

bridge::StatementContextReceiptHandle Acquire(
    const Session& session, const api::EngineRequestContext& context,
    bridge::StatementContextReceiptView* view) {
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid;
  bridge::StatementContextReceiptHandle receipt;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::AcquireStatementContextReceipt(
      session.get(), &request, &receipt, view, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  Check(status == SB_ENGINE_STATUS_OK && receipt,
        "statement receipt acquisition failed");
  return receipt;
}

bridge::StatementTableTruncateBindRequestV1 Demand(
    const bridge::StatementContextReceiptView& view, std::string_view name,
    std::uint64_t structural = 7, std::uint32_t occurrence = 2) {
  bridge::StatementTableTruncateBindRequestV1 request;
  request.authenticated_receipt_uuid = view.receipt_uuid;
  request.structural_occurrence = structural;
  request.truncate_occurrence = occurrence;
  request.target_name_atoms.push_back({std::string(name), false});
  request.identity_action = 1;
  request.dependency_action = 1;
  return request;
}

struct Outcome {
  sb_engine_status_t status = SB_ENGINE_STATUS_OK;
  bridge::StatementTableTruncateBindAckV1 ack;
};

Outcome Bind(bridge::StatementContextReceiptHandle receipt,
             const bridge::StatementTableTruncateBindRequestV1& request) {
  Outcome out;
  sb_engine_result_t result = nullptr;
  out.status = bridge::BindStatementTableTruncateTargetV1(
      receipt, &request, &out.ack, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  return out;
}
}  // namespace

int main() {
  // Database creation allocates through the engine's memory manager, which a
  // standalone fixture must configure first.
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "table_truncate_bind_publish");
  const auto fixture = MakeFixture();
  Session session(*fixture);

  // 1. The engine resolves the name itself and reports the published table.
  {
    bridge::StatementContextReceiptView view;
    const auto receipt = Acquire(session, fixture->transaction, &view);
    const auto bound = Bind(receipt, Demand(view, kTargetName));
    if (bound.status != SB_ENGINE_STATUS_OK) {
      std::cerr << "bind refused: " << bound.ack.failure_code << ':'
                << bound.ack.failure_message_key << ':'
                << bound.ack.failure_detail << '\n';
    }
    Check(bound.status == SB_ENGINE_STATUS_OK, "a valid truncate bind was refused");
    Check(bound.ack.failure_code.empty(),
          "a successful bind reported a failure code");
    Check(bound.ack.resolved_table_uuid == Identity(fixture->relation),
          "the bind resolved a different table than the published fixture");
    Check(bound.ack.persisted_descriptor_uuid ==
              fixture->descriptor.descriptor_uuid,
          "the bind published a descriptor identity the relation store does not own");
    Check(bound.ack.persisted_descriptor_generation ==
              fixture->descriptor.descriptor_generation,
          "the bind published a descriptor generation the relation store does not own");
    Check(bound.ack.persisted_descriptor_generation != 0,
          "the bind published a zero descriptor generation");
    Check(bound.ack.structural_occurrence == 7 &&
              bound.ack.truncate_occurrence == 2,
          "the ack did not echo the admitted occurrences");
    Check(bound.ack.authenticated_receipt_uuid == view.receipt_uuid,
          "the ack did not echo its authenticated receipt");

    // 2. Publication proof: the attach authority is once-only, so a second
    //    bind on this receipt must now refuse. If the first bind had not
    //    published, this would succeed.
    const auto again = Bind(receipt, Demand(view, kTargetName, 9, 4));
    Check(again.status != SB_ENGINE_STATUS_OK,
          "a second bind succeeded, so the first published no mapping");
    // The cause must be the attach authority refusing an already-populated
    // receipt. Without checking the cause this proves only "something
    // refused", which would also pass if the request had failed validation
    // before ever reaching the attach.
    Check(again.ack.failure_message_key ==
              "sblr.table_truncate.bind_mapping_rejected",
          "the second bind refused before reaching the attach authority, so "
          "it proves nothing about publication");
    Check(again.ack.persisted_descriptor_uuid ==
              api::EngineUuid{},
          "a refused second bind published a descriptor identity");
    (void)bridge::ReleaseStatementContextReceipt(receipt);
  }

  // 3. An unresolvable name refuses and publishes nothing. A fresh receipt,
  //    because the once-only attach would otherwise mask the cause.
  {
    bridge::StatementContextReceiptView view;
    const auto receipt = Acquire(session, fixture->transaction, &view);
    const auto missing = Bind(receipt, Demand(view, "no_such_truncate_target"));
    Check(missing.status != SB_ENGINE_STATUS_OK,
          "an unresolvable target name was bound");
    Check(missing.ack.resolved_table_uuid == api::EngineUuid{},
          "an unresolved bind published a table identity");
    Check(missing.ack.persisted_descriptor_uuid == api::EngineUuid{},
          "an unresolved bind published a descriptor identity");
    Check(!missing.ack.failure_code.empty(),
          "an unresolved bind carried no failure code");
    // The mapping must still be unpublished, so a valid bind now succeeds on
    // this same receipt.
    const auto good = Bind(receipt, Demand(view, kTargetName));
    Check(good.status == SB_ENGINE_STATUS_OK,
          "a refused bind consumed the receipt's once-only attach");
    (void)bridge::ReleaseStatementContextReceipt(receipt);
  }

  // 4. A receipt UUID that does not match the live receipt is hidden, even
  //    though the handle itself is valid.
  {
    bridge::StatementContextReceiptView view;
    const auto receipt = Acquire(session, fixture->transaction, &view);
    auto request = Demand(view, kTargetName);
    request.authenticated_receipt_uuid = Identity(fixture->session);
    const auto refused = Bind(receipt, request);
    Check(refused.status == SB_ENGINE_STATUS_SECURITY_DENIED,
          "a mismatched receipt UUID was not refused as hidden");
    Check(refused.ack.failure_message_key == "sblr.table_truncate.bind_hidden",
          "a mismatched receipt UUID reported an unexpected cause");
    Check(refused.ack.resolved_table_uuid == api::EngineUuid{},
          "a hidden bind published a table identity");
    (void)bridge::ReleaseStatementContextReceipt(receipt);
  }

  // 5. A released receipt is hidden.
  {
    bridge::StatementContextReceiptView view;
    const auto receipt = Acquire(session, fixture->transaction, &view);
    Check(bridge::ReleaseStatementContextReceipt(receipt) == SB_ENGINE_STATUS_OK,
          "receipt release failed");
    const auto refused = Bind(receipt, Demand(view, kTargetName));
    Check(refused.status == SB_ENGINE_STATUS_SECURITY_DENIED,
          "a released receipt was still bound");
    Check(refused.ack.resolved_table_uuid == api::EngineUuid{},
          "a released-receipt bind published a table identity");
  }

  std::filesystem::remove_all(fixture->directory);
  std::printf("PASS table truncate bind publish checks=%d\n", checks);
  return 0;
}
