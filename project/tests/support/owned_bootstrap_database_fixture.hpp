// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine_statement_fixture.hpp"
#include "owned_temp_directory.hpp"
#include "native_database_create_fixture.hpp"
#include "../../src/engine/internal_api/transaction/transaction_api.hpp"

namespace scratchbird::tests {
inline void ConfigureOwnedBootstrapFixtureMemory(std::string_view label) {
  auto policy = core::memory::DefaultLocalEngineMemoryPolicy();
  const auto configured = core::memory::ConfigureDefaultMemoryManagerForFixture(
      policy, std::string(label));
  if (!configured.ok())
    throw std::runtime_error("explicit administrative fixture memory configuration failed");
}
// Administrative component fixture, not a runtime service-principal fallback.
// Creation publishes the credentialed bootstrap owner; rights are read from
// that durable catalog. No trace tags or caller-invented grants confer rights.
//
// SEARCH_KEY: SB_NATIVE_CREATE_TEST_FIXTURE (owner Q256 = 1, 2026-11-16 legacy
// cut). The database is the NATIVE single-file database, made through the
// production native create (native_database_create_fixture.hpp). The engine
// mints the database identity, so the caller's `database` argument is no longer
// the file's identity (native create has no such input; the argument is kept so
// the 8 includers compile and is recorded in `requested_database_uuid`). The
// owner context is built from the native receipt: the bootstrap principal UUID
// comes from the receipt, not from the legacy security catalog read.
// What this fixture can no longer provide until the native serving path exists
// (S3b+): the materialized durable authorization and the engine transaction,
// both of which read the legacy catalog (MaterializeBootstrapFixtureAuthorization,
// EngineBeginTransaction) and fail at runtime on a native file. That failure is
// expected (owner 2026-11-16) and is reported, not hidden: Context() throws
// with the engine's own refusal.
// (Was: CreateDatabaseFile legacy + BootstrapFixtureOwnerContext.)
class OwnedBootstrapDatabaseFixture final {
 public:
  explicit OwnedBootstrapDatabaseFixture(core::platform::Uuid database) {
    requested_database_uuid_ = database;
    NativeFixtureCreateRequest request;
    request.path = directory_.path() / "node.sbdb";
    request.database_name = "owned_bootstrap_fixture";
    const auto made = CreateNativeFixtureDatabase(request);
    if (!made.ok) {
      std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
      directory_.Cleanup();
      throw std::runtime_error("owned credentialed database creation failed");
    }
    base_.database_path = request.path.string();
    base_.database_uuid = made.database_uuid;
    base_.default_root_uuid = made.filespace_uuid;
    base_.principal_uuid = made.bootstrap_principal_uuid;
    base_.session_uuid = engine::internal_api::GenerateCrudEngineUuid("object");
    base_.trust_mode = engine::internal_api::EngineTrustMode::server_isolated;
    base_.security_context_present = true;
    base_.catalog_generation_id = 1;
    base_.security_epoch = 1;
    base_.resource_epoch = 1;
    base_.name_resolution_epoch = 1;
    base_.datatype_catalog_snapshot_uuid = engine::internal_api::kBootstrapDatatypeCatalogUuid;
    base_.datatype_catalog_generation = engine::internal_api::kBootstrapDatatypeCatalogGeneration;
    base_.datatype_registry_generation = engine::internal_api::kBootstrapDatatypeRegistryGeneration;
  }
  OwnedBootstrapDatabaseFixture(const OwnedBootstrapDatabaseFixture&) = delete;
  OwnedBootstrapDatabaseFixture& operator=(const OwnedBootstrapDatabaseFixture&) = delete;
  ~OwnedBootstrapDatabaseFixture() noexcept {
    if (!transaction_) return;
    engine::internal_api::EngineRollbackTransactionRequest rollback;
    rollback.context = active_;
    if (!engine::internal_api::EngineRollbackTransaction(rollback).ok)
      std::terminate(); // Never delete storage still owned by an unresolved txn.
  }
  const std::filesystem::path& database_path() const noexcept { return database_path_; }
  const core::platform::Uuid& database_uuid() const noexcept { return base_.database_uuid; }
  const core::platform::Uuid& requested_database_uuid() const noexcept { return requested_database_uuid_; }
  engine::internal_api::EngineRequestContext Context(bool mutation = false) {
    if (finished_) throw std::runtime_error("owned database fixture already closed");
    if (mutation && !transaction_) {
      engine::internal_api::EngineBeginTransactionRequest begin;
      begin.context = base_;
      begin.isolation_level = "read_committed";
      const auto admitted = engine::internal_api::EngineBeginTransaction(begin);
      Check(admitted, "fixture transaction begin");
      active_ = base_;
      active_.transaction_uuid = admitted.transaction_uuid;
      active_.local_transaction_id = admitted.local_transaction_id;
      active_.snapshot_visible_through_local_transaction_id = admitted.snapshot_visible_through_local_transaction_id;
      active_.transaction_isolation_level = admitted.isolation_level;
      transaction_ = true;
    }
    auto context = transaction_ ? active_ : base_;
    MaterializeBootstrapFixtureAuthorization(context);
    return context;
  }
  void Finish() {
    if (transaction_) {
      engine::internal_api::EngineCommitTransactionRequest commit;
      commit.context = active_;
      const auto result = engine::internal_api::EngineCommitTransaction(commit);
      Check(result, "fixture transaction commit");
      transaction_ = false;
    }
    directory_.Cleanup();
    finished_ = true;
  }
 private:
  static void Check(const engine::internal_api::EngineApiResult& result, const char* phase) {
    if (result.ok) return;
    for (const auto& diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    throw std::runtime_error(phase);
  }
  OwnedTempDirectory directory_;
  std::filesystem::path database_path_ = directory_.path() / "node.sbdb";
  core::platform::Uuid requested_database_uuid_{};
  engine::internal_api::EngineRequestContext base_, active_;
  bool transaction_ = false, finished_ = false;
};
}
