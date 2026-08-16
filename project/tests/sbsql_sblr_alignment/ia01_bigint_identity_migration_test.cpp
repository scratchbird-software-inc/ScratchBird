#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr const char* kLegacy = "67000000-696e-7436-b400-000000000000";
constexpr const char* kCanonical = "019d0000-0000-7000-8000-00000000d712";

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Require(bool value, const char* message) { if (!value) Fail(message); }

std::string Id(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1786830000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string table_uuid{Id(UuidKind::object, 20)};
  std::string column_uuid{Id(UuidKind::object, 21)};
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    for (const char* suffix : {".sb.transaction_inventory", ".sb.mga_relation_metadata",
                               ".sb.mga_event_sequences", ".sb.mga_savepoints",
                               ".sb.owner.lock", ".dirty.manifest", ".recovery.evidence"}) {
      std::filesystem::remove(path.string() + suffix, ignored);
    }
  }
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.path = std::filesystem::temp_directory_path() /
      ("sb_bigint_identity_migration_" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".sbdb");
  db::DatabaseCreateConfig config;
  config.path = fixture.path.string();
  config.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1786830000001ull).value;
  config.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1786830000002ull).value;
  config.page_size = 16384;
  config.creation_unix_epoch_millis = 1786830000003ull;
  config.allow_minimal_resource_bootstrap = true;
  config.require_resource_seed_pack = false;
  config.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(config);
  Require(created.ok(), "database creation failed");
  const auto inventory = db::PersistLocalTransactionInventoryToDatabase(
      fixture.path.string(),
      scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory());
  Require(inventory.ok(), "transaction inventory initialization failed");
  fixture.database_uuid = uuid::UuidToString(config.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.request_id = "ia01-bigint-identity-migration";
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.session_uuid.canonical = Id(UuidKind::object, 3);
  context.principal_uuid.canonical = Id(UuidKind::principal, 4);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  return context;
}

api::EngineRequestContext Begin(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok && !begun.diagnostics.empty()) {
    std::cerr << begun.diagnostics.front().code << ':'
              << begun.diagnostics.front().message_key << ':'
              << begun.diagnostics.front().detail << '\n';
  }
  Require(begun.ok, "transaction begin failed");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  Require(api::EngineCommitTransaction(request).ok, "transaction commit failed");
}
void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  Require(api::EngineRollbackTransaction(request).ok, "transaction rollback failed");
}

api::CrudTableRecord LegacyTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "migration_target";
  table.columns.push_back({"id", "column_uuid=" + fixture.column_uuid +
      ";canonical=bigint;type_uuid=" + kLegacy + ";nullability=non_null"});
  return table;
}

api::MgaBigintIdentityMigrationRequest Migration(const Fixture& fixture,
                                                  std::uint64_t old_generation) {
  api::MgaBigintIdentityMigrationRequest request;
  request.prior_catalog_snapshot_uuid = Id(UuidKind::object, 30);
  request.new_catalog_snapshot_uuid = Id(UuidKind::object, 31);
  request.prior_catalog_generation = 7;
  request.new_catalog_generation = 8;
  request.rows.push_back({fixture.table_uuid, fixture.column_uuid, old_generation});
  return request;
}

std::string VisibleDescriptor(const api::EngineRequestContext& context,
                              const Fixture& fixture) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "relation metadata recovery load failed");
  const auto newest = api::FindVisibleCrudTable(
      loaded.state.crud_metadata, fixture.table_uuid,
      context.local_transaction_id);
  Require(newest.has_value() && newest->columns.size() == 1,
          "visible table projection missing");
  return newest->columns.front().second;
}
}  // namespace

int main() {
  auto fixture = MakeFixture();
  auto seed = Begin(BaseContext(fixture));
  Require(!api::AppendMgaTableMetadata(seed, LegacyTable(fixture)).error,
          "legacy metadata seed failed");
  Commit(seed);

  auto rollback_tx = Begin(BaseContext(fixture));
  auto rollback_request = Migration(fixture, 1);
  rollback_tx.statement_metadata_snapshot_uuid.canonical =
      rollback_request.prior_catalog_snapshot_uuid;
  const auto appended = api::AppendMgaBigintIdentityMigrationBatch(
      rollback_tx, rollback_request);
  Require(appended.ok && appended.migrated_row_count == 1 &&
              appended.decision_sha256.starts_with("sha256:"),
          "sealed migration append failed");
  Require(VisibleDescriptor(rollback_tx, fixture).find(kCanonical) != std::string::npos,
          "creator transaction did not see migration");
  Rollback(rollback_tx);

  auto after_rollback = Begin(BaseContext(fixture));
  Require(VisibleDescriptor(after_rollback, fixture).find(kLegacy) != std::string::npos,
          "rolled-back migration became visible");
  Rollback(after_rollback);

  // Recovery ignores a torn physical append because it has neither a complete
  // shape nor a seal/hash with publication authority.
  {
    std::ofstream out(fixture.path.string() + ".sb.mga_relation_metadata",
                      std::ios::app | std::ios::binary);
    out << "SBMGA1\tBIGINT_IDENTITY_MIGRATION_BATCH\t999\n";
  }
  auto torn_recovery = Begin(BaseContext(fixture));
  Require(VisibleDescriptor(torn_recovery, fixture).find(kLegacy) != std::string::npos,
          "torn migration append affected recovery visibility");
  Rollback(torn_recovery);

  auto commit_tx = Begin(BaseContext(fixture));
  auto commit_request = Migration(fixture, 1);
  commit_tx.statement_metadata_snapshot_uuid.canonical =
      commit_request.prior_catalog_snapshot_uuid;
  Require(api::AppendMgaBigintIdentityMigrationBatch(commit_tx, commit_request).ok,
          "committed migration append failed");
  Commit(commit_tx);

  auto recovered = Begin(BaseContext(fixture));
  const std::string descriptor = VisibleDescriptor(recovered, fixture);
  Require(descriptor.find(kCanonical) != std::string::npos &&
              descriptor.find(kLegacy) == std::string::npos,
          "committed migration did not recover canonically");
  const auto replay = api::AppendMgaBigintIdentityMigrationBatch(
      recovered, commit_request);
  Require(!replay.ok && replay.diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID",
          "stale migration generation did not fail closed");
  Rollback(recovered);
  return EXIT_SUCCESS;
}
