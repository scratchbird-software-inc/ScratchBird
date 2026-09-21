// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "page_header.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace db = scratchbird::storage::database;
namespace txn = scratchbird::transaction::mga;
namespace mem = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using Policy = db::InventoryPageSyncPolicy;
constexpr unsigned kPageSize = 8192;
constexpr unsigned kHeaderSize = 128; // Independent admitted prototype-page layout.
struct Observation {
  bool enabled = false;
  dev_t device{};
  ino_t inode{};
  unsigned long long syncs = 0, writes = 0, bodies = 0, bytes = 0, fail_sync = 0;
  bool crash = false;
} observed;
unsigned checks = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}
bool Watched(int fd) {
  struct stat state{};
  return observed.enabled && ::fstat(fd, &state) == 0 &&
         state.st_dev == observed.device && state.st_ino == observed.inode;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (Watched(fd)) {
    ++observed.syncs;
    if (observed.syncs == observed.fail_sync) {
      if (observed.crash) ::_exit(200);
      errno = EIO;
      return -1;
    }
  }
  return __real_fsync(fd);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t size, off_t offset) {
  const auto result = __real_pwrite(fd, bytes, size, offset);
  if (result > 0 && Watched(fd)) {
    ++observed.writes;
    if (offset % kPageSize == kHeaderSize && size == kPageSize - kHeaderSize) {
      ++observed.bodies;
      observed.bytes += static_cast<unsigned long long>(result);
    }
  }
  return result;
}
void Watch(const std::string& path, unsigned long long fail_sync = 0, bool crash = false) {
  struct stat state{};
  Check(::stat(path.c_str(), &state) == 0, "stat database");
  observed = {};
  observed.device = state.st_dev;
  observed.inode = state.st_ino;
  observed.fail_sync = fail_sync;
  observed.crash = crash;
  observed.enabled = true;
}
auto Identity(UuidKind kind) {
  const auto issued = uuid::IssueRuntimeIdentityV7();
  Check(issued.has_value(), "runtime identity issuance");
  const auto typed = uuid::MakeTypedUuid(kind, *issued);
  Check(typed.ok(), "typed identity");
  return typed.value;
}
void Configure() {
  mem::AllocationPolicy policy;
  policy.policy_name = "inventory_publication_io_fixture";
  policy.hard_limit_bytes = 64ull * 1024 * 1024;
  policy.soft_limit_bytes = 48ull * 1024 * 1024;
  policy.per_context_limit_bytes = 32ull * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  Check(mem::ConfigureDefaultMemoryManagerForFixture(policy, policy.policy_name).ok(),
        "memory fixture");
}
void FreshRead(const std::string& path, size_t count, txn::TransactionState state) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(path);
  Check(loaded.ok(), "fresh-process inventory load");
  Check(!loaded.publication_io.complete, "read is not a publication");
  Check(loaded.inventory.entries.size() == count, "fresh-process entry count");
  Check(loaded.inventory.entries.back().state == state, "fresh-process last state");
}
void Child(const std::string& path, size_t count, txn::TransactionState state,
           Policy policy = Policy::batched, bool crash = false) {
  const auto count_text = std::to_string(count);
  const auto state_text = std::to_string(static_cast<unsigned>(state));
  const auto policy_text = std::to_string(static_cast<unsigned>(policy));
  const auto pid = ::fork();
  Check(pid >= 0, "fork reader");
  if (pid == 0) {
    ::execl("/proc/self/exe", "inventory_publication_io_test",
            crash ? "--crash" : "--read", path.c_str(), count_text.c_str(),
            state_text.c_str(), policy_text.c_str(), nullptr);
    ::_exit(201);
  }
  int status = 0;
  Check(::waitpid(pid, &status, 0) == pid, "wait reader");
  Check(WIFEXITED(status) && WEXITSTATUS(status) == (crash ? 200 : 0),
        "fresh-process outcome");
}
void CheckIo(const db::LocalTransactionStoreResult& result, Policy policy,
             unsigned publications, unsigned pages, const db::DatabaseCreateConfig& create) {
  if (!result.ok()) std::cerr << result.diagnostic.diagnostic_code << ' '
                             << result.diagnostic.message_key << '\n';
  Check(result.ok(), "native publication success");
  const auto& io = result.publication_io;
  Check(io.complete && io.sync_policy == policy, "complete typed observation");
  Check(io.database_uuid == create.database_uuid.value &&
        io.filespace_uuid == create.filespace_uuid.value, "raw owning identities");
  Check(io.inventory_generation != 0, "published generation");
  Check(result.inventory.publication_base.has_value() &&
        io.inventory_generation == result.inventory.publication_base->generation,
        "observation retains final authoritative generation");
  Check(io.publications == publications, "phase aggregation");
  Check(io.page_body_writes == publications * pages, "multi-page write count");
  Check(io.page_body_writes == observed.bodies, "independent body syscall count");
  Check(io.body_bytes_written == observed.bytes &&
        io.body_bytes_written == io.page_body_writes * (kPageSize - kHeaderSize),
        "independent body bytes");
  Check(io.page_sync_calls == observed.syncs, "independent database sync count");
  Check(io.page_sync_calls == publications * (policy == Policy::batched ? 1 : pages),
        "sync policy executed");
}
void Exercise(const std::filesystem::path& directory, Policy policy) {
  db::DatabaseCreateConfig create;
  create.path = (directory / (policy == Policy::batched ? "batched.sbdb" : "per-page.sbdb")).string();
  create.database_uuid = Identity(UuidKind::database);
  create.filespace_uuid = Identity(UuidKind::filespace);
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = 1789900000000ull;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  observed.enabled = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ' '
                              << created.diagnostic.message_key << '\n';
  Check(created.ok(), "real database creation");
  auto loaded = db::LoadLocalTransactionInventoryFromDatabase(create.path);
  Check(loaded.ok() && !loaded.publication_io.complete, "initial load");
  const auto old_inventory = loaded.inventory;
  auto inventory = old_inventory;
  const auto capacity = scratchbird::storage::page::MaxTransactionInventoryEntriesPerPage(kPageSize);
  Check(capacity > 1, "nontrivial capacity");
  for (unsigned i = 0; i < capacity + 2; ++i) {
    auto begun = txn::BeginLocalTransaction(inventory, Identity(UuidKind::transaction),
                                           create.creation_unix_epoch_millis + i + 1);
    Check(begun.ok(), "real MGA begin");
    inventory = std::move(begun.inventory);
  }
  const auto count = inventory.entries.size();
  Watch(create.path);
  auto published = db::PersistLocalTransactionInventoryToDatabase(create.path, inventory, policy);
  CheckIo(published, policy, 2, 2, create);
  Child(create.path, count, txn::TransactionState::active);

  Watch(create.path);
  auto stale = db::PersistLocalTransactionInventoryToDatabase(create.path, old_inventory, policy);
  Check(!stale.ok() && !stale.publication_io.complete, "stale publication refused");
  Check(observed.writes == 0 && observed.syncs == 0, "stale performs no database writes");
  auto invalid = db::PersistLocalTransactionInventoryToDatabase(create.path, published.inventory,
                                                               static_cast<Policy>(0));
  Check(!invalid.ok() && !invalid.publication_io.complete, "invalid policy refused");
  Check(observed.writes == 0 && observed.syncs == 0, "invalid performs no database writes");
  Child(create.path, count, txn::TransactionState::active);

  // Crash before a required database sync; the publishing journal selects old
  // inventory even if one or more new page bodies reached the file.
  observed.enabled = false;
  Child(create.path, count, txn::TransactionState::active, policy, true);
  Child(create.path, count, txn::TransactionState::active);
  loaded = db::LoadLocalTransactionInventoryFromDatabase(create.path);
  Check(loaded.ok(), "reload after crash");
  auto committed = txn::CommitLocalTransaction(loaded.inventory,
      loaded.inventory.entries.back().identity.local_id, create.creation_unix_epoch_millis + 100000);
  Check(committed.ok(), "real MGA commit");
  Watch(create.path, policy == Policy::batched ? 1 : 2);
  auto failed = db::PersistLocalTransactionInventoryToDatabase(create.path, committed.inventory, policy);
  Check(!failed.ok() && !failed.publication_io.complete, "sync error has no completion");
  Check(observed.syncs == observed.fail_sync, "fault reached actual database sync");
  Child(create.path, count, txn::TransactionState::active);
  loaded = db::LoadLocalTransactionInventoryFromDatabase(create.path);
  Check(loaded.ok(), "reload after sync failure");
  committed = txn::CommitLocalTransaction(loaded.inventory,
      loaded.inventory.entries.back().identity.local_id, create.creation_unix_epoch_millis + 100001);
  Check(committed.ok(), "retry real MGA commit");
  Watch(create.path);
  published = db::PersistLocalTransactionInventoryToDatabase(create.path, committed.inventory, policy);
  CheckIo(published, policy, 1, 2, create);
  Child(create.path, count, txn::TransactionState::committed);
  // Fail activation only after the created/starting allocation is durable.
  // An error must not conceal the consumed transaction ID or claim rollback.
  auto begun = txn::BeginLocalTransaction(published.inventory, Identity(UuidKind::transaction),
                                         create.creation_unix_epoch_millis + 200000);
  Check(begun.ok(), "activation-failure begin");
  const auto consumed_id = begun.entry.identity.local_id.value;
  Watch(create.path, policy == Policy::batched ? 2 : 3);
  failed = db::PersistLocalTransactionInventoryToDatabase(create.path, begun.inventory, policy);
  Check(!failed.ok() && !failed.publication_io.complete, "failed activation observation incomplete");
  Check(observed.syncs == observed.fail_sync, "activation sync failure reached");
  Child(create.path, count + 1, txn::TransactionState::created);
  loaded = db::LoadLocalTransactionInventoryFromDatabase(create.path);
  Check(loaded.ok() && loaded.inventory.next_local_transaction_id == consumed_id + 1,
        "failed activation consumes allocation");
  Watch(create.path);
  stale = db::PersistLocalTransactionInventoryToDatabase(create.path, begun.inventory, policy);
  Check(!stale.ok() && observed.writes == 0 && observed.syncs == 0,
        "pre-allocation base cannot replay after activation failure");
  begun = txn::BeginLocalTransaction(loaded.inventory, Identity(UuidKind::transaction),
                                    create.creation_unix_epoch_millis + 200001);
  Check(begun.ok() && begun.entry.identity.local_id.value == consumed_id + 1,
        "fresh begin cannot reuse consumed identity");
  Watch(create.path);
  published = db::PersistLocalTransactionInventoryToDatabase(create.path, begun.inventory, policy);
  CheckIo(published, policy, 2, 2, create);
  Child(create.path, count + 2, txn::TransactionState::active);
  observed.enabled = false;
}
int main(int argc, char** argv) {
  try {
    Configure();
    if (argc == 6) {
      if (std::string(argv[1]) == "--read") {
        FreshRead(argv[2], std::stoull(argv[3]), static_cast<txn::TransactionState>(std::stoul(argv[4])));
        return 0;
      }
      Check(std::string(argv[1]) == "--crash", "known child mode");
      auto loaded = db::LoadLocalTransactionInventoryFromDatabase(argv[2]);
      Check(loaded.ok(), "crash child load");
      auto committed = txn::CommitLocalTransaction(loaded.inventory,
          loaded.inventory.entries.back().identity.local_id, 1789900100000ull);
      Check(committed.ok(), "crash child commit");
      const auto policy = static_cast<Policy>(std::stoul(argv[5]));
      Watch(argv[2], policy == Policy::batched ? 1 : 2, true);
      db::PersistLocalTransactionInventoryToDatabase(argv[2], committed.inventory, policy);
      throw std::runtime_error("crash injection missed");
    }
    Check(argc == 1, "usage");
    char pattern[] = "/tmp/sb-inventory-publication-XXXXXX";
    const auto* created = ::mkdtemp(pattern);
    Check(created != nullptr, "unique fixture directory");
    struct Directory {
      std::filesystem::path path;
      ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    } directory{created};
    Exercise(directory.path, Policy::batched);
    Exercise(directory.path, Policy::per_page);
    std::cout << "PASS inventory publication checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    observed.enabled = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
