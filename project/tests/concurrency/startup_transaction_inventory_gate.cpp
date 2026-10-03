// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "transaction/startup_transaction_inventory.hpp"
#include "transaction/transaction_api.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "time.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#ifdef SB_RUNTIME_CREDENTIAL_FAULT_PROBE
#include <openssl/evp.h>
#include <cerrno>
#include <new>
#include <pthread.h>
#endif

extern char** environ;

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
using Outcome = api::StartupTransactionInventoryOutcome;

#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
std::atomic_size_t inventory_guard_calls{0};
extern "C" std::unique_lock<std::recursive_mutex>
__real__ZN11scratchbird6engine12internal_api32AcquireTransactionInventoryGuardERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(const std::string&);
extern "C" std::unique_lock<std::recursive_mutex>
__wrap__ZN11scratchbird6engine12internal_api32AcquireTransactionInventoryGuardERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(const std::string& path) {
  inventory_guard_calls.fetch_add(1, std::memory_order_relaxed);
  return __real__ZN11scratchbird6engine12internal_api32AcquireTransactionInventoryGuardERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(path);
}
#endif

#ifdef SB_RUNTIME_CREDENTIAL_FAULT_PROBE
thread_local bool fail_after_password_verification = false;
thread_local bool fail_credential_allocation = false;
thread_local pthread_mutex_t* fail_credential_mutex = nullptr;
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t size) {
  if (fail_credential_allocation) {
    fail_credential_allocation = false;
    throw std::bad_alloc();
  }
  return __real__Znwm(size);
}
extern "C" int __real_PKCS5_PBKDF2_HMAC(const char*, int, const unsigned char*,
    int, int, const EVP_MD*, int, unsigned char*);
extern "C" int __wrap_PKCS5_PBKDF2_HMAC(const char* password, int length,
    const unsigned char* salt, int salt_length, int iterations,
    const EVP_MD* digest, int key_length, unsigned char* output) {
  const int result = __real_PKCS5_PBKDF2_HMAC(password, length, salt, salt_length,
      iterations, digest, key_length, output);
  if (fail_after_password_verification) {
    fail_after_password_verification = false;
    fail_credential_allocation = true;
  }
  return result;
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (mutex == fail_credential_mutex) {
    fail_credential_mutex = nullptr;
    return EINVAL;
  }
  return __real_pthread_mutex_lock(mutex);
}
#endif

unsigned checks = 0;
const char* executable = nullptr;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

platform::TypedUuid NewId(platform::UuidKind kind) {
  const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
  Check(clock.ok(), "clock");
  const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
  Check(millis.ok(), "clock conversion");
  const auto id = uuid::GenerateEngineIdentityV7(kind, millis.unix_epoch_millis);
  Check(id.ok(), "binary identity generation");
  return id.value;
}

#include "runtime_principal_observation_checks.hpp"

void ColdRead(const api::StartupTransactionInventoryRequest& request,
              const api::StartupTransactionInventoryObservation& expected) {
  // Fresh exec, no inherited engine cache. Fixed binary identities cross the
  // fixture pipe; the file path is the only request field in argv.
  int pipe_fds[2];
  Check(pipe(pipe_fds) == 0, "cold probe pipe");
  posix_spawn_file_actions_t actions;
  Check(posix_spawn_file_actions_init(&actions) == 0, "spawn actions");
  Check(posix_spawn_file_actions_adddup2(&actions, pipe_fds[0], STDIN_FILENO) == 0,
        "spawn input");
  Check(posix_spawn_file_actions_addclose(&actions, pipe_fds[1]) == 0, "spawn close writer");
  char* args[]{const_cast<char*>(executable), const_cast<char*>("--cold"),
               const_cast<char*>(request.database_path.c_str()), nullptr};
  pid_t pid = 0;
  const int spawned = posix_spawn(&pid, executable, &actions, nullptr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipe_fds[0]);
  if (spawned != 0) {
    close(pipe_fds[1]);
    Check(false, "fresh process spawn");
  }
  const auto send = [&](const auto& value) {
    Check(write(pipe_fds[1], &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)),
          "binary probe input");
  };
  send(request.database_uuid.bytes);
  send(request.transaction_uuid.bytes);
  send(request.local_transaction_id);
  send(expected.outcome);
  send(expected.publication_base->generation);
  send(expected.publication_base->inventory_sha256);
  close(pipe_fds[1]);
  int status = 0;
  Check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fresh process exact inventory observation");
}

// Every transform below is durably published by the actual native store.
// This fixture tests inventory observation, NOT engine session admission,
// startup operation ownership or successful startup recovery.
void Run(const std::filesystem::path& root, std::uint32_t page_size) {
  const auto path = root / (std::to_string(page_size) + ".sbdb");
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = NewId(platform::UuidKind::database);
  create.filespace_uuid = NewId(platform::UuidKind::filespace);
  create.page_size = page_size;
  create.creation_unix_epoch_millis = 1;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = db::CreateDatabaseFile(create);
  Check(created.ok(), created.diagnostic.diagnostic_code.c_str());

  auto loaded = db::LoadLocalTransactionInventoryFromDatabase(path.string());
  Check(loaded.ok(), "load native inventory");
  auto transaction = NewId(platform::UuidKind::transaction);
  auto begun = mga::BeginLocalTransaction(loaded.inventory, transaction, 2);
  Check(begun.ok(), "candidate begin");
  auto persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), begun.inventory);
  Check(persisted.ok(), "actual durable begin");
  api::StartupTransactionInventoryRequest request{
      path.string(), create.database_uuid.value, transaction.value,
      begun.entry.identity.local_id.value};

#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
  const auto valid_guards_before = inventory_guard_calls.load();
#endif
  const auto active = api::InspectStartupTransactionInventory(request);
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
  Check(inventory_guard_calls.load() > valid_guards_before,
        "valid observation reaches actual inventory guard; probe is live");
#endif
  Check(active.outcome == Outcome::unresolved, "active cannot prove completion");
  Check(active.observed_state == mga::TransactionState::active, "actual active state");
  Check(active.publication_base == persisted.inventory.publication_base, "exact native base");
  ColdRead(request, active);

  auto wrong = request;
  wrong.database_uuid = NewId(platform::UuidKind::database).value;
  Check(api::InspectStartupTransactionInventory(wrong).outcome == Outcome::database_mismatch,
        "another database cannot own work");
  wrong = request;
  wrong.transaction_uuid = NewId(platform::UuidKind::transaction).value;
  Check(api::InspectStartupTransactionInventory(wrong).outcome == Outcome::identity_mismatch,
        "same local number different UUID");
  wrong = request;
  wrong.local_transaction_id += 1000;
  Check(api::InspectStartupTransactionInventory(wrong).outcome == Outcome::identity_mismatch,
        "same UUID different local number");
  wrong.transaction_uuid = NewId(platform::UuidKind::transaction).value;
  Check(api::InspectStartupTransactionInventory(wrong).outcome == Outcome::identity_missing,
        "missing is not no effect");

  const auto bytes = [&] {
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "read-only oracle opens actual database");
    return std::vector<char>(std::istreambuf_iterator<char>(input), {});
  };
  const auto before_invalid_single = bytes();
  for (int invalid = 0; invalid != 9; ++invalid) {
    wrong = request;
    switch (invalid) {
      case 0: wrong.database_path.clear(); break;
      case 1: wrong.database_uuid = {}; break;
      case 2: wrong.transaction_uuid = {}; break;
      case 3: wrong.local_transaction_id = 0; break;
      case 4: wrong.transaction_uuid.bytes[6] = 0x40; break;
      case 5: wrong.database_uuid.bytes[8] = 0; break;
      case 6: wrong.database_path.push_back('\0'); break;
      case 7: wrong.database_path += std::string("\0.other", 7); break;
      case 8: wrong.database_path.insert(0, 1, '\0'); break;
    }
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
    const auto guards_before = inventory_guard_calls.load();
#endif
    const auto result = api::InspectStartupTransactionInventory(wrong);
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
    Check(inventory_guard_calls.load() == guards_before,
          "invalid single request never enters inventory guard");
#endif
    Check(result.outcome == Outcome::invalid_request && !result.publication_base &&
              result.observed_state == mga::TransactionState::none,
          "malformed request never obtains inventory provenance");
  }
  Check(bytes() == before_invalid_single, "invalid single routing changes no database bytes");
  const auto corrected = api::InspectStartupTransactionInventory(request);
  Check(corrected.outcome == active.outcome && corrected.observed_state == active.observed_state &&
            corrected.publication_base == active.publication_base,
        "corrected single route retains exact original inventory");

  auto committed = mga::CommitLocalTransaction(persisted.inventory, begun.entry.identity.local_id, 3);
  Check(committed.ok(), "candidate commit");
  persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), committed.inventory);
  Check(persisted.ok(), "actual durable commit");
  const auto final = api::InspectStartupTransactionInventory(request);
  Check(final.outcome == Outcome::committed, "committed by current inventory");
  Check(final.publication_base == persisted.inventory.publication_base, "committed native base");
  Check(final.publication_base->generation > active.publication_base->generation,
        "fresh publication generation");
  Check(final.publication_base->inventory_sha256 != active.publication_base->inventory_sha256,
        "fresh native inventory digest");
  Check(active.outcome == Outcome::unresolved, "retained observation never upgrades itself");

  auto archived = mga::ArchiveLocalTransaction(persisted.inventory, begun.entry.identity.local_id);
  Check(archived.ok(), "candidate archive");
  persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), archived.inventory);
  Check(persisted.ok(), "actual durable archive");
  const auto archive_observation = api::InspectStartupTransactionInventory(request);
  Check(archive_observation.outcome == Outcome::committed &&
            archive_observation.observed_state == mga::TransactionState::archived,
        "archived commit retains original outcome");
  ColdRead(request, archive_observation);

  transaction = NewId(platform::UuidKind::transaction);
  begun = mga::BeginLocalTransaction(persisted.inventory, transaction, 4);
  Check(begun.ok(), "second begin");
  persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), begun.inventory);
  Check(persisted.ok(), "second durable begin");
  auto rollback_request = request;
  rollback_request.transaction_uuid = transaction.value;
  rollback_request.local_transaction_id = begun.entry.identity.local_id.value;
  Check(api::InspectStartupTransactionInventory(rollback_request).outcome == Outcome::unresolved,
        "unrelated commit cannot resolve original active work");
  using BatchOutcome = api::StartupTransactionInventoryBatchOutcome;
  std::array<api::StartupTransactionInventoryIdentity, 2> identities{{
      {request.transaction_uuid, request.local_transaction_id},
      {rollback_request.transaction_uuid, rollback_request.local_transaction_id}}};
  std::array<api::StartupTransactionInventoryEntryObservation, 3> observations{};
  observations[2].outcome = Outcome::invalid_request;
  auto batch = api::InspectStartupTransactionInventories(
      path.string(), request.database_uuid, identities, observations);
  Check(batch.outcome == BatchOutcome::observed && batch.records_written == 2,
        "complete startup set observed in one snapshot");
  Check(batch.publication_base == persisted.inventory.publication_base,
        "batch uses current exact native publication");
  Check(observations[0].outcome == Outcome::committed &&
            observations[0].observed_state == mga::TransactionState::archived &&
            observations[1].outcome == Outcome::unresolved &&
            observations[1].observed_state == mga::TransactionState::active,
        "partial committed set retains unresolved original transaction");
  Check(observations[2].outcome == Outcome::invalid_request,
        "batch leaves output tail untouched");
  const auto before_invalid_batch = bytes();
  for (int invalid = 0; invalid != 10; ++invalid) {
    auto malformed = identities;
    auto batch_path = path.string();
    auto batch_database = request.database_uuid;
    auto count = malformed.size();
    auto capacity = observations.size();
    switch (invalid) {
      case 0: batch_path.clear(); break;
      case 1: batch_database = {}; break;
      case 2: malformed[1].transaction_uuid = {}; break;
      case 3: malformed[1].local_transaction_id = 0; break;
      case 4: count = 0; break;
      case 5: capacity = 1; break;
      case 6: malformed[1].transaction_uuid.bytes[6] = 0x40; break;
      case 7: batch_path.push_back('\0'); break;
      case 8: batch_path += std::string("\0.other", 7); break;
      case 9: batch_path.insert(0, 1, '\0'); break;
    }
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
    const auto guards_before = inventory_guard_calls.load();
#endif
    const auto rejected = api::InspectStartupTransactionInventories(
        batch_path, batch_database, std::span(malformed).first(count),
        std::span(observations).first(capacity));
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
    Check(inventory_guard_calls.load() == guards_before,
          "invalid batch request never enters inventory guard");
#endif
    Check(rejected.outcome == BatchOutcome::invalid_request &&
              rejected.records_written == 0 && !rejected.publication_base,
          "invalid set has no partial output or provenance");
    Check(observations[0].outcome == Outcome::committed &&
              observations[0].observed_state == mga::TransactionState::archived &&
              observations[1].outcome == Outcome::unresolved &&
              observations[1].observed_state == mga::TransactionState::active &&
              observations[2].outcome == Outcome::invalid_request &&
              observations[2].observed_state == mga::TransactionState::none,
          "invalid later tuple leaves earlier output untouched");
  }
  Check(bytes() == before_invalid_batch, "invalid batch routing changes no database bytes");
  batch = api::InspectStartupTransactionInventories(path.string(), request.database_uuid,
                                                   identities, observations);
  Check(batch.outcome == BatchOutcome::observed && batch.records_written == 2 &&
            batch.publication_base == persisted.inventory.publication_base &&
            observations[0].outcome == Outcome::committed &&
            observations[1].outcome == Outcome::unresolved,
        "corrected batch route retains both original transactions");
  auto rolled_back = mga::RollbackLocalTransaction(persisted.inventory, begun.entry.identity.local_id, 5);
  Check(rolled_back.ok(), "candidate rollback");
  persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), rolled_back.inventory);
  Check(persisted.ok(), "actual durable rollback");
  Check(api::InspectStartupTransactionInventory(rollback_request).outcome == Outcome::rolled_back,
        "rolled back by actual inventory");
  archived = mga::ArchiveLocalTransaction(persisted.inventory, begun.entry.identity.local_id);
  Check(archived.ok(), "rollback archive");
  persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), archived.inventory);
  Check(persisted.ok(), "durable rollback archive");
  Check(api::InspectStartupTransactionInventory(rollback_request).outcome == Outcome::rolled_back,
        "archive does not turn rollback into commit");
  ColdRead(rollback_request, api::InspectStartupTransactionInventory(rollback_request));
  batch = api::InspectStartupTransactionInventories(
      path.string(), request.database_uuid, identities, observations);
  Check(batch.outcome == BatchOutcome::observed &&
            batch.publication_base == persisted.inventory.publication_base &&
            observations[0].outcome == Outcome::committed &&
            observations[1].outcome == Outcome::rolled_back,
        "fresh set read preserves both actual terminal outcomes");
  auto missing = identities;
  missing[1].transaction_uuid = NewId(platform::UuidKind::transaction).value;
  missing[1].local_transaction_id += 1000;
  batch = api::InspectStartupTransactionInventories(
      path.string(), request.database_uuid, missing, observations);
  Check(batch.outcome == BatchOutcome::observed &&
            observations[0].outcome == Outcome::committed &&
            observations[1].outcome == Outcome::identity_missing,
        "successful batch read is not all-transactions-resolved");
  batch = api::InspectStartupTransactionInventories(
      path.string(), NewId(platform::UuidKind::database).value, identities, observations);
  Check(batch.outcome == BatchOutcome::database_mismatch && batch.records_written == 0,
        "wrong database grants no batch observations");

  const auto before_inspection = bytes();
  Check(api::InspectStartupTransactionInventory(request).outcome == Outcome::committed,
        "repeat exact original observation");
  batch = api::InspectStartupTransactionInventories(
      path.string(), request.database_uuid, identities, observations);
  Check(batch.outcome == BatchOutcome::observed, "repeat batch observation");
  Check(bytes() == before_inspection, "inspection changes no database bytes");

  const auto held = path.string() + ".held";
  std::filesystem::rename(path, held);
  const auto absent = api::InspectStartupTransactionInventory(request);
  Check(absent.outcome == Outcome::authority_unavailable && !absent.publication_base,
        "warm cache cannot authorize absent database");
  Check(!absent.diagnostic.diagnostic_code.empty(), "retain storage read failure");
  batch = api::InspectStartupTransactionInventories(
      path.string(), request.database_uuid, identities, observations);
  Check(batch.outcome == BatchOutcome::authority_unavailable &&
            batch.records_written == 0 && !batch.publication_base &&
            !batch.diagnostic.diagnostic_code.empty(),
        "batch cannot use warm cache when native file is unavailable");
  std::filesystem::rename(held, path);
  Check(api::InspectStartupTransactionInventory(request).outcome == Outcome::committed,
        "restored file read observes original committed identity");
  // Read-only failure is not a startup fence, and this successful read is not a
  // recovery disposition. The future owning service must enforce that boundary.
}

int main(int argc, char** argv) {
  executable = argv[0];
  if (argc == 3 && std::string(argv[1]) == "--cold-principal")
    return runtime_principal_checks::ColdMain(argv[2]);
  if (argc == 3 && std::string(argv[1]) == "--cold") {
    api::StartupTransactionInventoryRequest request;
    request.database_path = argv[2];
    Outcome expected;
    std::uint64_t generation = 0;
    std::array<platform::byte, 32> hash{};
    const auto receive = [](auto& value) {
      std::cin.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    receive(request.database_uuid.bytes);
    receive(request.transaction_uuid.bytes);
    receive(request.local_transaction_id);
    receive(expected);
    receive(generation);
    receive(hash);
    if (!std::cin) return 2;
    const auto result = api::InspectStartupTransactionInventory(request);
    return result.outcome == expected && result.publication_base &&
                   result.publication_base->generation == generation &&
                   result.publication_base->inventory_sha256 == hash ? 0 : 1;
  }
  if (argc != 1) return 2;
  std::string pattern = (std::filesystem::temp_directory_path() / "sb-startup-inventory-XXXXXX").string();
  if (!mkdtemp(pattern.data())) return 2;
  const std::filesystem::path root(pattern);
  try {
    for (const std::uint32_t profile : {8192U, 16384U, 32768U, 65536U, 131072U}) {
      Run(root, profile);
      runtime_principal_checks::Run(root, profile);
    }
    std::filesystem::remove_all(root);
    std::cout << "PASS " << checks << " checks; native inventory/principal and authentication identity; no startup admission\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what()
              << "; fixture retained at " << root << '\n';
    return 1;
  }
}
