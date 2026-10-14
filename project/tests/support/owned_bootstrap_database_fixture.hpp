// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine_statement_fixture.hpp"
#include "owned_temp_directory.hpp"
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
class OwnedBootstrapDatabaseFixture final {
 public:
  explicit OwnedBootstrapDatabaseFixture(core::platform::Uuid database) {
    storage::database::DatabaseCreateConfig create;
    create.path = (directory_.path() / "node.sbdb").string();
    create.database_uuid = {core::platform::UuidKind::database, database};
    create.filespace_uuid = {core::platform::UuidKind::filespace,
        engine::internal_api::GenerateCrudEngineUuid("object")};
    ConfigureCredentialedFixtureBootstrap(create);
    const auto made = storage::database::CreateDatabaseFile(create);
    if (!made.ok() || made.create_finality != storage::database::DatabaseCreateFinalityClass::committed) {
      std::cerr << made.diagnostic.message_key << '\n';
      for (const auto& argument : made.diagnostic.arguments)
        if (const auto* value = argument.text()) std::cerr << argument.key << '=' << *value << '\n';
      directory_.Cleanup();
      throw std::runtime_error("owned credentialed database creation failed");
    }
    base_ = BootstrapFixtureOwnerContext(create);
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
  engine::internal_api::EngineRequestContext base_, active_;
  bool transaction_ = false, finished_ = false;
};
}
