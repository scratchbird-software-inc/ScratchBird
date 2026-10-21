#include "sblr_executor_availability_registry.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "uuid.hpp"
#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"

#include <cassert>
#include <cstdio>
#include <chrono>
#include <filesystem>

namespace api = scratchbird::engine::internal_api;
namespace {
api::EngineUuid Id(scratchbird::core::platform::UuidKind kind) {
  static std::uint64_t stamp = 1787003000000ull;
  if (!scratchbird::core::uuid::UuidKindAllowsDurableIdentity(kind)) {
    auto raw = scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(++stamp);
    assert(raw.ok());
    auto typed = scratchbird::core::uuid::MakeTypedUuid(kind, raw.value);
    assert(typed.ok());
    return typed.value.value;
  }
  auto value = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, ++stamp);
  assert(value.ok());
  return value.value.value;
}
}

int main() {
  const auto base = std::filesystem::temp_directory_path() /
      ("sb_savepoint_" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  // Savepoint ACTIVATION binds its rollback boundary to the MGA savepoint
  // marker store -- the same store row, metadata and index visibility use --
  // so it needs a real database behind context.database_path. RESERVE does
  // not, which is why this fixture reserved successfully and then aborted on
  // the first activation with only a temp path that was never created.
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "ia05_transaction_savepoint_coordinator");
  std::filesystem::create_directories(base);
  api::EngineRequestContext context;
  context.database_path = (base / "savepoint.sbdb").string();
  context.database_uuid = Id(scratchbird::core::platform::UuidKind::database);
  context.session_uuid = Id(scratchbird::core::platform::UuidKind::session);
  context.transaction_uuid = Id(scratchbird::core::platform::UuidKind::object);
  context.statement_uuid = Id(scratchbird::core::platform::UuidKind::object);
  context.local_transaction_id = 41;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_savepoint_coordination"};
  // Fields EngineBeginTransaction requires, taken from the working engine
  // statement fixture rather than guessed.
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "ia05-transaction-savepoint-coordinator";
  context.database_page_size_bytes = 16384;
  context.identifier_profile_uuid = "sbsql_v3";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;

  {
    scratchbird::storage::database::DatabaseCreateConfig create;
    create.path = context.database_path;
    // The marker store validates that the context's database identity IS the
    // created database's, so both must be the same value, not two draws from
    // the fixture generator.
    create.database_uuid = {scratchbird::core::platform::UuidKind::database,
                            context.database_uuid};
    const auto filespace = Id(scratchbird::core::platform::UuidKind::filespace);
    create.filespace_uuid = {scratchbird::core::platform::UuidKind::filespace,
                             filespace};
    context.default_root_uuid = filespace;
    create.page_size = 16384;
    create.creation_unix_epoch_millis = 1789310000000ULL;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    const auto created =
        scratchbird::storage::database::CreateDatabaseFile(create);
    if (!created.ok()) {
      std::fprintf(stderr, "savepoint fixture database refused: %s:%s\n",
                   created.diagnostic.diagnostic_code.c_str(),
                   created.diagnostic.message_key.c_str());
    }
    assert(created.ok());
    const auto bootstrap =
        scratchbird::storage::database::ReadDatabaseBootstrapSecurityCatalog(
            create.path);
    assert(bootstrap.ok() && bootstrap.state.present);
    context.principal_uuid = bootstrap.state.principal_uuid.value;
  }

  // The marker store also validates the transaction identity against the
  // durable inventory, so the fixture needs a real MGA transaction rather than
  // an invented uuid and local id.
  {
    api::EngineBeginTransactionRequest begin;
    begin.context = context;
    // Admission refuses a context that already names a bound transaction, so
    // the synthetic identity this fixture used to carry is cleared here and
    // replaced by the real one below.
    begin.context.transaction_uuid = {};
    begin.context.local_transaction_id = 0;
    begin.isolation_level = "repeatable_read";
    const auto begun = api::EngineBeginTransaction(begin);
    if (!begun.ok) {
      std::fprintf(stderr, "savepoint fixture transaction refused: ");
      for (const auto& diagnostic : begun.diagnostics) {
        std::fprintf(stderr, "%s:%s:%s ", diagnostic.code.c_str(),
                     diagnostic.message_key.c_str(), diagnostic.detail.c_str());
      }
      std::fprintf(stderr, "\n");
    }
    assert(begun.ok);
    context.local_transaction_id = begun.local_transaction_id;
    context.transaction_uuid = begun.transaction_uuid;
    context.snapshot_visible_through_local_transaction_id =
        begun.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = begun.isolation_level;
  }

  const std::string transaction_evidence = "sha256:" + std::string(64, 'a');
  const std::string symbol_sha = "sha256:" + std::string(64, 'b');
  auto reserved = api::ReserveSblrSavepoint(
      context, context.statement_uuid, transaction_evidence, 7, symbol_sha);
  assert(reserved.ok && reserved.snapshot.transaction_ordinal == 1 &&
         reserved.snapshot.state == api::SblrSavepointState::reserved);
  auto replay = api::ActivateSblrSavepoint(
      context, context.statement_uuid, reserved.snapshot.descriptor_uuid,
      reserved.snapshot.descriptor_generation + 1,
      reserved.snapshot.descriptor_evidence_sha256, 1);
  assert(!replay.ok && replay.diagnostic.code == "MGA.SAVEPOINT.STALE");
  auto active = api::ActivateSblrSavepoint(
      context, context.statement_uuid, reserved.snapshot.descriptor_uuid,
      reserved.snapshot.descriptor_generation,
      reserved.snapshot.descriptor_evidence_sha256, 1);
  if (!active.ok) {
    std::fprintf(stderr, "activate refused: %s:%s:%s\n",
                 active.diagnostic.code.c_str(),
                 active.diagnostic.message_key.c_str(),
                 active.diagnostic.detail.c_str());
  }
  assert(active.ok && active.snapshot.state == api::SblrSavepointState::active &&
         active.snapshot.stack_generation == 1);
  replay = api::ActivateSblrSavepoint(
      context, context.statement_uuid, reserved.snapshot.descriptor_uuid,
      reserved.snapshot.descriptor_generation,
      reserved.snapshot.descriptor_evidence_sha256, 1);
  assert(!replay.ok && replay.diagnostic.code == "MGA.SAVEPOINT.STALE");

  api::SblrExecutorAvailabilityRowIdentity row;
  row.executor_id = api::kSblrTxnSavepointExecutorId;
  row.opcode_code = api::kSblrTxnSavepointOpcodeCode;
  row.opcode_version = api::kSblrTxnSavepointOpcodeVersion;
  row.operand_descriptor_id = api::kSblrTxnSavepointOperandDescriptorId;
  row.result_descriptor_id = api::kSblrTxnSavepointResultDescriptorId;
  row.result_descriptor_version = api::kSblrTxnSavepointResultDescriptorVersion;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  const auto installed = api::LoadSblrExecutorAvailabilitySnapshot(context, row);
  assert(installed.ok && installed.snapshot.installed);
  api::SblrExecutorAvailabilitySetRequest request;
  request.database_uuid = context.database_uuid;
  request.expected_snapshot_uuid = installed.snapshot.snapshot_uuid;
  request.expected_generation = installed.snapshot.generation;
  request.exact_row_identity = row;
  request.requested_state = api::SblrExecutorAvailabilityState::revoked;
  request.reason_code = "CSC-TEST-002359";
  assert(api::SetSblrExecutorAvailability(context, request).ok);
  api::SblrExecutorAvailabilitySnapshot current;
  assert(api::RevalidateSblrExecutorAvailability(
             context, row, installed.snapshot, &current).code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");

  std::error_code ec;
  std::filesystem::remove(base.string()+".sb.sblr_savepoint_coordinator.v1",ec);
  std::filesystem::remove(base.string()+".sb.sblr_savepoint_coordinator.v2",ec);
  std::filesystem::remove(base.string()+".sb.mga_savepoints",ec);
  std::filesystem::remove(base.string()+".sb.sblr_executor_availability_registry.v1.txn_savepoint",ec);
  return 0;
}
