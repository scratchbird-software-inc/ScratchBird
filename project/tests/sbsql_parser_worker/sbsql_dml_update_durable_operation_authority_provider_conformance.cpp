// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/update_durable_operation_authority_provider.hpp"
#include "local_transaction_store.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/sb_update_durable_provider.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* made = ::mkdtemp(writable.data());
    Require(made != nullptr, "durable provider temporary directory failed");
    path_ = made;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string NewUuid(UuidKind kind, std::uint64_t identity_time) {
  const auto generated =
      uuid::GenerateDurableEngineIdentityV7(kind, identity_time);
  Require(generated.ok(), "durable provider UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::filesystem::path database_path;
  std::string database_uuid;
  api::EngineRequestContext context;
};

Fixture CreateFixture(const std::filesystem::path& path) {
  const auto database_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::database, 1788210000000ull);
  const auto filespace_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::filespace, 1788210000001ull);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "durable provider database identities failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1788210000002ull;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "durable provider database create failed");
  Require(db::OpenDatabaseFile({path.string(), false, false, false}).ok(),
          "durable provider first open failed");
  Require(db::MarkDatabaseCleanShutdown(path.string()).ok(),
          "durable provider clean marker failed");

  const auto inventory =
      db::LoadLocalTransactionInventoryFromDatabase(path.string());
  Require(inventory.ok(), "durable provider inventory load failed");
  const auto transaction_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::transaction, 1788210000003ull);
  Require(transaction_uuid.ok(),
          "durable provider transaction identity failed");
  const auto begun = mga::BeginLocalTransaction(
      inventory.inventory, transaction_uuid.value, 1788210000004ull);
  Require(begun.ok(), "durable provider transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                         begun.inventory)
              .ok(),
          "durable provider active transaction persistence failed");

  Fixture fixture;
  fixture.database_path = path;
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.context.trust_mode = api::EngineTrustMode::server_isolated;
  fixture.context.database_path = path.string();
  fixture.context.database_uuid.canonical = fixture.database_uuid;
  fixture.context.database_page_size_bytes = create.page_size;
  fixture.context.local_transaction_id = begun.entry.identity.local_id.value;
  fixture.context.transaction_uuid.canonical =
      uuid::UuidToString(begun.entry.identity.transaction_uuid.value);
  fixture.context.statement_receipt_uuid.canonical =
      NewUuid(UuidKind::object, 1788210000005ull);
  fixture.context.statement_snapshot_uuid.canonical =
      NewUuid(UuidKind::object, 1788210000006ull);
  fixture.context.statement_metadata_snapshot_engine_owned = true;
  fixture.context.statement_metadata_snapshot_uuid.canonical =
      NewUuid(UuidKind::object, 1788210000007ull);
  fixture.context.security_context_present = true;
  fixture.context.trace_tags.emplace_back("private_dml_update_rows_binder");
  return fixture;
}

void Rollback(const Fixture& fixture) {
  db::PhysicalMgaCowFinalizeRequest finalize;
  finalize.database_path = fixture.database_path.string();
  finalize.local_transaction_id =
      mga::MakeLocalTransactionId(fixture.context.local_transaction_id);
  finalize.decision = db::PhysicalMgaCowFinalizeDecision::rollback;
  finalize.final_unix_epoch_millis = 1788210001000ull;
  Require(db::FinalizePhysicalMgaCowTransaction(finalize).ok(),
          "durable provider transaction rollback failed");
}

api::EngineDmlUpdateDurableAuthorityReservationRequestV1 Reservation(
    const api::EngineRequestContext& context) {
  api::EngineDmlUpdateDurableAuthorityReservationRequestV1 request;
  request.context = context;
  request.reservation.operation_uuid =
      NewUuid(UuidKind::object, 1788210000100ull);
  request.reservation.operation_generation = 1;
  request.reservation.descriptor_uuid =
      NewUuid(UuidKind::object, 1788210000101ull);
  request.reservation.descriptor_generation = 1;
  request.reservation.recovery_token_uuid =
      NewUuid(UuidKind::object, 1788210000102ull);
  request.reservation.recovery_generation = 1;
  return request;
}

void TestReservationAuthority(Fixture* fixture) {
  Require(fixture != nullptr, "durable provider fixture missing");
  const auto request = Reservation(fixture->context);
  const auto reserved =
      api::ReserveDmlUpdateDurableOperationAuthorityV1(request);
  Require(reserved.ok() &&
              reserved.outcome ==
                  api::MgaDmlUpdateDurableOperationOutcomeV1::committed &&
              !reserved.identity.validated_durable_handle_uuid.empty() &&
              reserved.identity.validated_durable_handle_generation == 1 &&
              !reserved.identity.reserved_statement_barrier_uuid.empty() &&
              reserved.identity.reserved_statement_barrier_generation == 1 &&
              reserved.identity.validated_durable_handle_uuid !=
                  reserved.identity.reserved_statement_barrier_uuid,
          "durable provider did not issue distinct nonzero handle/barrier authority");

  const auto exact_retry =
      api::ReserveDmlUpdateDurableOperationAuthorityV1(request);
  Require(exact_retry.ok() &&
              exact_retry.outcome ==
                  api::MgaDmlUpdateDurableOperationOutcomeV1::already_exact &&
              exact_retry.identity == reserved.identity,
          "durable provider exact reservation retry was not idempotent");

  auto cross_receipt = request;
  cross_receipt.context.statement_receipt_uuid.canonical =
      NewUuid(UuidKind::object, 1788210000200ull);
  const auto denied =
      api::ReserveDmlUpdateDurableOperationAuthorityV1(cross_receipt);
  Require(!denied.ok() &&
              denied.outcome ==
                  api::MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
          "durable provider reservation crossed statement receipts");

  api::EngineDmlUpdateDurableAuthorityAbandonRequestV1 forged;
  forged.context = fixture->context;
  forged.identity = reserved.identity;
  ++forged.identity.validated_durable_handle_generation;
  const auto forged_refusal =
      api::AbandonDmlUpdateDurableOperationAuthorityReservationV1(forged);
  Require(!forged_refusal.ok() &&
              forged_refusal.outcome ==
                  api::MgaDmlUpdateDurableOperationOutcomeV1::stale,
          "durable provider admitted forged handle generation");

  api::EngineDmlUpdateDurableAuthorityAbandonRequestV1 abandon;
  abandon.context = fixture->context;
  abandon.identity = reserved.identity;
  const pid_t abandon_child = ::fork();
  Require(abandon_child >= 0,
          "durable provider abandonment child could not start");
  if (abandon_child == 0) {
    const auto abandoned =
        api::AbandonDmlUpdateDurableOperationAuthorityReservationV1(abandon);
    ::_exit(abandoned.ok() ? EXIT_SUCCESS : 91);
  }
  int abandon_status = 0;
  Require(::waitpid(abandon_child, &abandon_status, 0) == abandon_child &&
              WIFEXITED(abandon_status) &&
              WEXITSTATUS(abandon_status) == EXIT_SUCCESS,
          "durable provider child could not durably abandon reservation");
  const auto abandoned_retry =
      api::AbandonDmlUpdateDurableOperationAuthorityReservationV1(abandon);
  Require(abandoned_retry.ok() &&
              abandoned_retry.outcome ==
                  api::MgaDmlUpdateDurableOperationOutcomeV1::already_exact,
          "durable provider reservation abandonment was not idempotent");

  api::EngineDmlUpdateDurableRecoverChainRequestV1 recover;
  recover.context = fixture->context;
  recover.context.trace_tags.clear();
  recover.context.trace_tags.emplace_back(
      "private_dml_update_rows_recovery");
  recover.lookup.descriptor_uuid = request.reservation.descriptor_uuid;
  recover.lookup.descriptor_generation =
      request.reservation.descriptor_generation;
  recover.lookup.structural_occurrence_id = 1;
  const auto undiscoverable =
      api::RecoverDmlUpdateDurableOperationChainV1(recover);
  Require(!undiscoverable.ok() &&
              undiscoverable.outcome ==
                  api::MgaDmlUpdateDurableOperationOutcomeV1::access_denied &&
              undiscoverable.diagnostic.code == "SECURITY.ACCESS_DENIED" &&
              undiscoverable.authority_snapshot ==
                  api::MgaDmlUpdateDurableAuthoritySnapshotV1{} &&
              undiscoverable.journal.empty() &&
              !undiscoverable.staged_successor_present &&
              undiscoverable.recovery_observation_dumo.empty() &&
              !undiscoverable.validated_handle.valid() &&
              !undiscoverable.quarantined,
          "abandoned unbound reservation remained recoverable");

  const auto reserved_again =
      api::ReserveDmlUpdateDurableOperationAuthorityV1(request);
  Require(reserved_again.ok() &&
              reserved_again.identity.validated_durable_handle_uuid !=
                  reserved.identity.validated_durable_handle_uuid &&
              reserved_again.identity.reserved_statement_barrier_uuid !=
                  reserved.identity.reserved_statement_barrier_uuid,
          "durable provider reused abandoned reservation authority");
  abandon.identity = reserved_again.identity;
  Require(api::AbandonDmlUpdateDurableOperationAuthorityReservationV1(abandon)
              .ok(),
          "second durable reservation abandonment failed");
}

}  // namespace

int main() {
  TemporaryDirectory temporary;
  auto fixture =
      CreateFixture(temporary.path() / "durable-operation-authority.sbdb");
  TestReservationAuthority(&fixture);
  Rollback(fixture);
  std::cout
      << "sbsql_dml_update_durable_operation_authority_provider_conformance: PASS\n";
  return EXIT_SUCCESS;
}
