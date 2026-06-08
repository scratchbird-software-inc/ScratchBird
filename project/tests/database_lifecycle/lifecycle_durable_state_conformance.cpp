// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "startup_state.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_003_durable_state_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

bool HasCommittedEvidenceTransaction(const db::DatabaseLifecycleState& state,
                                     std::uint64_t local_transaction_id) {
  for (const auto& entry : state.local_transaction_inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id &&
        entry.state == mga::TransactionState::committed &&
        entry.evidence_record_written) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceFlag(const db::StartupStateRecord& state, std::uint64_t flag) {
  return (state.durable_evidence_flags & flag) != 0;
}

void RequireLifecyclePhaseNamesStable() {
  Require(std::string(db::StartupLifecycleDurablePhaseName(
              db::StartupLifecycleDurablePhase::create_tx1_committed)) == "create_tx1_committed",
          "create_tx1_committed durable lifecycle phase name is not stable");
  Require(std::string(db::StartupLifecycleDurablePhaseName(
              db::StartupLifecycleDurablePhase::open_ready)) == "open_ready",
          "open_ready durable lifecycle phase name is not stable");
  Require(std::string(db::StartupLifecycleDurablePhaseName(
              db::StartupLifecycleDurablePhase::clean_shutdown)) == "clean_shutdown",
          "clean_shutdown durable lifecycle phase name is not stable");
}

void RequireBootstrapEvidence(const db::DatabaseLifecycleState& state) {
  Require(state.local_transaction_inventory_present,
          "create did not return a local transaction inventory");
  Require(state.startup_state_present, "create did not return startup state");
  Require(HasCommittedEvidenceTransaction(state, 1),
          "create transaction 1 is not committed with durable evidence");
  Require(state.startup_state.bootstrap_local_transaction_id == 1,
          "startup state did not persist bootstrap transaction 1");
  Require(state.startup_state.last_lifecycle_local_transaction_id == 1,
          "startup state last lifecycle transaction is not bootstrap transaction 1");
  Require(state.startup_state.lifecycle_generation >= 1,
          "startup state lifecycle generation was not initialized");
  Require(state.startup_state.last_lifecycle_event_unix_epoch_millis > 0,
          "startup state lifecycle event timestamp was not initialized");
  Require(state.startup_state.durable_lifecycle_phase ==
              db::StartupLifecycleDurablePhase::create_tx1_committed,
          "create did not persist create_tx1_committed durable lifecycle phase");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed),
          "create did not persist bootstrap_tx1_committed evidence flag");
}

void RequireFirstOpenEvidence(const db::DatabaseLifecycleState& state) {
  Require(state.local_transaction_inventory_present,
          "writable open did not return a local transaction inventory");
  Require(state.startup_state_present, "writable open did not return startup state");
  const auto tx2 = state.startup_state.first_open_activation_local_transaction_id;
  Require(tx2 == 2, "first writable open did not persist activation transaction 2");
  Require(HasCommittedEvidenceTransaction(state, tx2),
          "first writable open activation transaction is not committed with evidence");
  Require(state.startup_state.last_lifecycle_local_transaction_id == tx2,
          "startup state last lifecycle transaction is not first-open activation");
  Require(state.startup_state.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::open_ready,
          "writable open did not persist open_ready durable lifecycle phase");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed),
          "writable open lost bootstrap_tx1_committed evidence flag");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::first_open_tx2_committed),
          "writable open did not persist first_open_tx2_committed evidence flag");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::startup_owner_token_persisted),
          "writable open did not persist startup_owner_token_persisted evidence flag");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::authorities_ready),
          "writable open did not persist authorities_ready evidence flag");
}

void RequireCleanShutdownEvidence(const db::DatabaseLifecycleState& state,
                                  std::uint64_t first_open_transaction_id) {
  Require(state.local_transaction_inventory_present,
          "read-only reopen did not return a local transaction inventory");
  Require(state.startup_state_present, "read-only reopen did not return startup state");
  const auto clean_tx = state.startup_state.clean_shutdown_local_transaction_id;
  Require(clean_tx > first_open_transaction_id,
          "clean shutdown transaction id did not advance beyond first-open transaction");
  Require(HasCommittedEvidenceTransaction(state, clean_tx),
          "clean shutdown transaction is not committed with durable evidence");
  Require(state.startup_state.last_lifecycle_local_transaction_id == clean_tx,
          "startup state last lifecycle transaction is not clean shutdown transaction");
  Require(state.startup_state.durable_lifecycle_phase ==
              db::StartupLifecycleDurablePhase::clean_shutdown,
          "clean shutdown did not persist clean_shutdown durable lifecycle phase");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::clean_shutdown_tx_committed),
          "clean shutdown did not persist clean_shutdown_tx_committed evidence flag");
}

}  // namespace

int main() {
  RequireLifecyclePhaseNamesStable();

  const auto database_path = TestDatabasePath();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      if (!path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    }
  } cleanup{database_path};

  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "CreateDatabaseFile failed");
  RequireBootstrapEvidence(created.state);

  db::DatabaseOpenConfig open_rw;
  open_rw.path = database_path.string();
  open_rw.read_only = false;
  const auto opened_rw = db::OpenDatabaseFile(open_rw);
  if (!opened_rw.ok()) {
    std::cerr << opened_rw.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_rw.ok(), "OpenDatabaseFile read-write failed");
  RequireFirstOpenEvidence(opened_rw.state);

  const auto first_open_transaction_id =
      opened_rw.state.startup_state.first_open_activation_local_transaction_id;
  const auto clean = db::MarkDatabaseCleanShutdown(database_path.string());
  if (!clean.ok()) {
    std::cerr << clean.diagnostic.diagnostic_code << '\n';
  }
  Require(clean.ok(), "MarkDatabaseCleanShutdown failed");

  db::DatabaseOpenConfig open_ro;
  open_ro.path = database_path.string();
  open_ro.read_only = true;
  const auto opened_ro = db::OpenDatabaseFile(open_ro);
  if (!opened_ro.ok()) {
    std::cerr << opened_ro.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_ro.ok(), "OpenDatabaseFile read-only after clean shutdown failed");
  RequireCleanShutdownEvidence(opened_ro.state, first_open_transaction_id);

  return EXIT_SUCCESS;
}
