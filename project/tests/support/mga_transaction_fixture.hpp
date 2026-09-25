// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/storage/database/database_lifecycle.hpp"
#include "../../src/engine/internal_api/transaction/transaction_api.hpp"
#include "../../src/core/uuid/uuid.hpp"
#include <chrono>
#include <filesystem>
#include <stdexcept>
namespace scratchbird::tests {
// A real durable inventory transaction for otherwise isolated scalar tests.
// Its identifier and visibility watermark are always issued by the engine.
class MgaTransactionFixture {
 public:
  engine::internal_api::EngineRequestContext context;
  explicit MgaTransactionFixture(engine::internal_api::EngineRequestContext base)
      : context(std::move(base)) {
    namespace db = storage::database;
    namespace api = engine::internal_api;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 256; ++i) {
      directory_ = std::filesystem::temp_directory_path() /
          ("sb_scalar_mga_" + std::to_string(suffix) + "_" + std::to_string(i));
      if (std::filesystem::create_directory(directory_)) break;
      if (i == 255) throw std::runtime_error("scalar MGA fixture directory unavailable");
    }
    db::DatabaseCreateConfig create;
    create.path = (directory_ / "fixture.sbdb").string();
    const auto database = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::database, millis);
    const auto filespace = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::filespace, millis);
    if (!database.ok() || !filespace.ok()) throw std::runtime_error("scalar MGA fixture identity generation failed");
    create.database_uuid = database.value;
    create.filespace_uuid = filespace.value;
    create.creation_unix_epoch_millis = millis;
    create.require_resource_seed_pack = false;
    create.allow_minimal_resource_bootstrap = true;
    const auto created = db::CreateDatabaseFile(create);
    if (!created.ok()) throw std::runtime_error("scalar MGA fixture database create failed");
    context.database_path = create.path;
    context.database_uuid = database.value.value;
    context.default_root_uuid = filespace.value.value;
    context.trust_mode = api::EngineTrustMode::server_isolated;
    // A newly created local database starts at the same bootstrap session
    // epochs used by server session admission; no object generation is claimed.
    context.catalog_generation_id = 1;
    context.security_epoch = 1;
    context.resource_epoch = 1;
    context.name_resolution_epoch = 1;
    api::EngineBeginTransactionRequest request;
    request.context = context;
    request.isolation_level = "read_committed";
    const auto begun = api::EngineBeginTransaction(request);
    if (!begun.ok) throw std::runtime_error("scalar MGA fixture transaction begin failed:" +
        (begun.diagnostics.empty() ? std::string{} : begun.diagnostics.front().detail));
    context.transaction_uuid = begun.transaction_uuid;
    context.local_transaction_id = begun.local_transaction_id;
    context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = begun.isolation_level;
  }
  MgaTransactionFixture(const MgaTransactionFixture&) = delete;
  MgaTransactionFixture& operator=(const MgaTransactionFixture&) = delete;
  ~MgaTransactionFixture() {
    engine::internal_api::EngineRollbackTransactionRequest request;
    request.context = context;
    const auto rolled_back = engine::internal_api::EngineRollbackTransaction(request);
    // Preserve the database if cleanup cannot establish durable rollback.
    if (rolled_back.ok) {
      std::error_code ignored;
      std::filesystem::remove_all(directory_, ignored);
    }
  }
 private:
  std::filesystem::path directory_;
};
}
