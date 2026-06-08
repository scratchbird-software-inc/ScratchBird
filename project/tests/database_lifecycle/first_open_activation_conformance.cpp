// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
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
namespace disk = scratchbird::storage::disk;
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
         ("sb_dblc_005_first_open_activation_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

bool HasCommittedEvidenceTransaction(const mga::LocalTransactionInventory& inventory,
                                     std::uint64_t local_transaction_id) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id &&
        entry.state == mga::TransactionState::committed &&
        entry.evidence_record_written) {
      return true;
    }
  }
  return false;
}

std::uint32_t CountTransaction(const mga::LocalTransactionInventory& inventory,
                               std::uint64_t local_transaction_id) {
  std::uint32_t count = 0;
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id) {
      ++count;
    }
  }
  return count;
}

bool HasEvidenceFlag(const db::StartupStateRecord& state, std::uint64_t flag) {
  return (state.durable_evidence_flags & flag) != 0;
}

db::StartupStateRecord ReadDurableStartupState(const std::filesystem::path& database_path,
                                               std::uint32_t page_size) {
  disk::FileDevice device;
  const auto opened = device.Open(database_path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "could not open database read-only for startup state verification");

  const auto startup = db::ReadStartupStatePageBody(&device, page_size);
  if (!startup.ok()) {
    std::cerr << startup.diagnostic.diagnostic_code << '\n';
  }
  Require(startup.ok(), "durable startup state page did not parse");
  return startup.state;
}

mga::LocalTransactionInventory ReadDurableTransactionInventory(
    const std::filesystem::path& database_path,
    std::uint32_t page_size) {
  disk::FileDevice device;
  const auto opened = device.Open(database_path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "could not open database read-only for transaction inventory verification");

  const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&device, page_size);
  if (!inventory.ok()) {
    std::cerr << inventory.diagnostic.diagnostic_code << '\n';
  }
  Require(inventory.ok(), "durable transaction inventory did not parse");
  return inventory.inventory;
}

void RequireCreateEvidence(const db::DatabaseLifecycleState& state) {
  Require(state.startup_state_present, "create did not return startup state");
  Require(state.local_transaction_inventory_present, "create did not return transaction inventory");
  Require(state.resource_seed_catalog_present, "create did not return resource seed catalog");
  Require(state.resource_seed_catalog.active, "create did not load active seed pack");
  Require(state.resource_seed_catalog.seed_pack_name == "initial-resource-pack",
          "create did not use the initial seed pack");
  Require(state.startup_state.bootstrap_local_transaction_id == 1,
          "create did not record bootstrap tx1");
  Require(state.startup_state.first_open_activation_local_transaction_id == 0,
          "create recorded first-open activation before first open");
  Require(state.startup_state.last_lifecycle_local_transaction_id == 1,
          "create last lifecycle transaction is not tx1");
  Require(state.startup_state.durable_lifecycle_phase ==
              db::StartupLifecycleDurablePhase::create_tx1_committed,
          "create did not persist create_tx1_committed phase");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed),
          "create did not persist bootstrap_tx1_committed evidence flag");
  Require(HasCommittedEvidenceTransaction(state.local_transaction_inventory, 1),
          "create tx1 is not committed with evidence");
  Require(CountTransaction(state.local_transaction_inventory, 2) == 0,
          "create unexpectedly allocated activation tx2");
}

void RequireReadOnlyBeforeActivation(const db::DatabaseLifecycleState& state) {
  Require(state.read_only_open, "pre-activation open was not reported read-only");
  Require(state.startup_state_present, "read-only pre-activation open did not return startup state");
  Require(state.local_transaction_inventory_present,
          "read-only pre-activation open did not return transaction inventory");
  Require(state.startup_state.first_open_activation_local_transaction_id == 0,
          "read-only pre-activation open recorded activation tx2");
  Require(CountTransaction(state.local_transaction_inventory, 2) == 0,
          "read-only pre-activation open committed or allocated tx2");
  Require(!HasEvidenceFlag(state.startup_state,
                           db::StartupLifecycleEvidenceFlag::first_open_tx2_committed),
          "read-only pre-activation open persisted first_open_tx2 evidence");
}

void RequireFirstWritableOpenEvidence(const db::DatabaseLifecycleState& state) {
  Require(!state.read_only_open, "first writable open was reported read-only");
  Require(state.startup_state_present, "first writable open did not return startup state");
  Require(state.local_transaction_inventory_present,
          "first writable open did not return transaction inventory");
  Require(state.startup_state.first_open_activation_local_transaction_id == 2,
          "first writable open did not persist activation tx2");
  Require(HasCommittedEvidenceTransaction(state.local_transaction_inventory, 2),
          "first writable open tx2 is not committed with evidence");
  Require(CountTransaction(state.local_transaction_inventory, 2) == 1,
          "first writable open did not produce exactly one tx2 entry");
  Require(state.startup_state.durable_lifecycle_phase ==
              db::StartupLifecycleDurablePhase::open_ready,
          "first writable open did not persist open_ready phase");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::first_open_tx2_committed),
          "first writable open did not persist first_open_tx2 evidence flag");
  Require(HasEvidenceFlag(state.startup_state,
                          db::StartupLifecycleEvidenceFlag::authorities_ready),
          "first writable open did not persist authorities_ready evidence flag");
  Require(!state.write_admission_fenced, "first writable open left write admission fenced");
  Require(!state.startup_state.write_admission_fenced,
          "first writable open startup state left write admission fenced");
  Require(state.startup_state.config_authority_loaded,
          "first writable open did not load config authority");
  Require(state.startup_state.security_authority_loaded,
          "first writable open did not load security authority");
  Require(state.startup_state.i18n_authority_loaded,
          "first writable open did not load i18n authority");
  Require(state.startup_state.runtime_activation_complete,
          "first writable open did not mark runtime activation complete");
  Require(state.startup_state.runtime_activation_generation != 0,
          "first writable open did not advance runtime activation generation");
}

void RequireDurableStartupMatchesFirstOpen(const db::DatabaseLifecycleState& opened,
                                           const db::StartupStateRecord& durable) {
  Require(durable.first_open_activation_local_transaction_id ==
              opened.startup_state.first_open_activation_local_transaction_id,
          "durable startup state activation tx does not match returned state");
  Require(durable.last_lifecycle_local_transaction_id ==
              opened.startup_state.last_lifecycle_local_transaction_id,
          "durable startup state last lifecycle tx does not match returned state");
  Require(durable.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::open_ready,
          "durable startup state is not open_ready");
  Require(durable.durable_evidence_flags == opened.startup_state.durable_evidence_flags,
          "durable startup evidence flags do not match returned state");
  Require(!durable.write_admission_fenced, "durable startup state left write admission fenced");
  Require(durable.config_authority_loaded, "durable startup state lacks config authority flag");
  Require(durable.security_authority_loaded, "durable startup state lacks security authority flag");
  Require(durable.i18n_authority_loaded, "durable startup state lacks i18n authority flag");
  Require(durable.runtime_activation_complete,
          "durable startup state lacks runtime activation complete flag");
  Require(durable.runtime_activation_generation != 0,
          "durable startup state lacks runtime activation generation");
}

void RequireCleanShutdownEvidence(const db::StartupStateRecord& startup,
                                  const mga::LocalTransactionInventory& inventory,
                                  std::uint64_t first_open_tx) {
  const auto clean_shutdown_tx = startup.clean_shutdown_local_transaction_id;
  Require(clean_shutdown_tx > first_open_tx,
          "clean shutdown tx did not advance beyond activation tx2");
  Require(startup.last_lifecycle_local_transaction_id == clean_shutdown_tx,
          "clean shutdown did not persist last lifecycle tx");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::clean_shutdown,
          "clean shutdown did not persist clean_shutdown phase");
  Require(HasEvidenceFlag(startup, db::StartupLifecycleEvidenceFlag::clean_shutdown_tx_committed),
          "clean shutdown did not persist clean shutdown evidence flag");
  Require(HasCommittedEvidenceTransaction(inventory, clean_shutdown_tx),
          "clean shutdown tx is not committed with evidence");
}

void RequireSecondWritableOpenEvidence(const db::DatabaseLifecycleState& state,
                                       std::uint64_t clean_shutdown_tx) {
  Require(state.startup_state.first_open_activation_local_transaction_id == 2,
          "second writable open changed first-open activation transaction");
  Require(CountTransaction(state.local_transaction_inventory, 2) == 1,
          "second writable open created another tx2 entry");
  Require(HasCommittedEvidenceTransaction(state.local_transaction_inventory, 2),
          "second writable open lost committed tx2 evidence");
  Require(state.startup_state.last_lifecycle_local_transaction_id >= clean_shutdown_tx,
          "second writable open regressed last lifecycle transaction below clean shutdown tx");
}

}  // namespace

int main() {
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
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "CreateDatabaseFile failed");
  RequireCreateEvidence(created.state);

  const auto durable_create_startup =
      ReadDurableStartupState(database_path, created.state.header.page_size);
  Require(durable_create_startup.first_open_activation_local_transaction_id == 0,
          "durable create startup state recorded activation before first open");

  db::DatabaseOpenConfig open_ro_before_activation;
  open_ro_before_activation.path = database_path.string();
  open_ro_before_activation.read_only = true;
  const auto opened_ro_before_activation = db::OpenDatabaseFile(open_ro_before_activation);
  if (!opened_ro_before_activation.ok()) {
    std::cerr << opened_ro_before_activation.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_ro_before_activation.ok(), "read-only open before activation failed");
  RequireReadOnlyBeforeActivation(opened_ro_before_activation.state);

  const auto durable_ro_startup =
      ReadDurableStartupState(database_path, created.state.header.page_size);
  Require(durable_ro_startup.first_open_activation_local_transaction_id == 0,
          "read-only open durably recorded activation tx2");
  const auto durable_ro_inventory =
      ReadDurableTransactionInventory(database_path, created.state.header.page_size);
  Require(CountTransaction(durable_ro_inventory, 2) == 0,
          "read-only open durably created tx2");

  db::DatabaseOpenConfig open_rw;
  open_rw.path = database_path.string();
  open_rw.read_only = false;
  const auto opened_rw = db::OpenDatabaseFile(open_rw);
  if (!opened_rw.ok()) {
    std::cerr << opened_rw.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_rw.ok(), "first writable open failed");
  RequireFirstWritableOpenEvidence(opened_rw.state);

  const auto durable_first_open_startup =
      ReadDurableStartupState(database_path, opened_rw.state.header.page_size);
  RequireDurableStartupMatchesFirstOpen(opened_rw.state, durable_first_open_startup);
  const auto durable_first_open_inventory =
      ReadDurableTransactionInventory(database_path, opened_rw.state.header.page_size);
  Require(HasCommittedEvidenceTransaction(durable_first_open_inventory, 2),
          "durable transaction inventory does not show committed tx2 evidence");

  const auto clean = db::MarkDatabaseCleanShutdown(database_path.string());
  if (!clean.ok()) {
    std::cerr << clean.diagnostic.diagnostic_code << '\n';
  }
  Require(clean.ok(), "clean shutdown failed");

  const auto durable_clean_startup =
      ReadDurableStartupState(database_path, opened_rw.state.header.page_size);
  const auto durable_clean_inventory =
      ReadDurableTransactionInventory(database_path, opened_rw.state.header.page_size);
  RequireCleanShutdownEvidence(durable_clean_startup,
                               durable_clean_inventory,
                               opened_rw.state.startup_state.first_open_activation_local_transaction_id);
  const auto clean_shutdown_tx = durable_clean_startup.clean_shutdown_local_transaction_id;

  const auto opened_second_rw = db::OpenDatabaseFile(open_rw);
  if (!opened_second_rw.ok()) {
    std::cerr << opened_second_rw.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_second_rw.ok(), "second writable open failed");
  RequireSecondWritableOpenEvidence(opened_second_rw.state, clean_shutdown_tx);

  return EXIT_SUCCESS;
}
