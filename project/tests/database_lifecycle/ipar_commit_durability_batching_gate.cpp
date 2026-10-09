// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/engine_statement_fixture.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "session/scoped_value_store.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cerrno>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
namespace mem = scratchbird::core::memory;
namespace scoped = scratchbird::engine::internal_api::session;
using scratchbird::core::platform::UuidKind;
using Policy = db::InventoryPageSyncPolicy;
constexpr unsigned kPageSize = 8192;
constexpr unsigned kHeaderSize = 128;
unsigned checks = 0;
struct Observation {
  bool enabled = false;
  dev_t device{};
  ino_t inode{};
  unsigned long long writes = 0, bodies = 0, bytes = 0, syncs = 0;
  std::array<unsigned long long, 256> bodies_at_sync{};
  bool fail_body_sync = false;
} observed;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) throw std::runtime_error(why);
}
template<class Result>
void CheckApi(const Result& result, const char* why) {
  if (!result.ok)
    for (const auto& d : result.diagnostics) std::cerr << d.code << ':' << d.detail << '\n';
  Check(result.ok, why);
}
bool Watched(int fd) {
  struct stat state{};
  return observed.enabled && ::fstat(fd, &state) == 0 &&
      state.st_dev == observed.device && state.st_ino == observed.inode;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (Watched(fd)) {
    if (observed.syncs < observed.bodies_at_sync.size())
      observed.bodies_at_sync[observed.syncs] = observed.bodies;
    ++observed.syncs;
    if (observed.fail_body_sync && observed.bodies != 0) {
      observed.fail_body_sync = false;
      errno = EIO;
      return -1;
    }
  }
  return __real_fsync(fd);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t size, off_t offset) {
  const auto written = __real_pwrite(fd, bytes, size, offset);
  if (written > 0 && Watched(fd)) {
    ++observed.writes;
    if (offset % kPageSize == kHeaderSize && size == kPageSize - kHeaderSize) {
      ++observed.bodies;
      observed.bytes += static_cast<unsigned long long>(written);
    }
  }
  return written;
}
void Watch(const std::string& path, bool fail = false) {
  struct stat state{};
  Check(::stat(path.c_str(), &state) == 0, "stat real database");
  observed = {};
  observed.device = state.st_dev;
  observed.inode = state.st_ino;
  observed.fail_body_sync = fail;
  observed.enabled = true;
}
auto Identity(UuidKind kind) {
  const auto issued = uuid::IssueRuntimeIdentityV7();
  Check(issued.has_value(), "issue binary identity");
  const auto typed = uuid::MakeTypedUuid(kind, *issued);
  Check(typed.ok(), "type binary identity");
  return typed.value;
}
struct Directory {
  std::filesystem::path path;
  ~Directory() {
    observed.enabled = false;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
struct PhaseTraceScope {
  static constexpr const char* key = "SCRATCHBIRD_TRANSACTION_API_PHASE_TRACE_FILE";
  std::optional<std::string> original;
  explicit PhaseTraceScope(const std::string& path) {
    if (const char* old = std::getenv(key)) original = old;
    Check(path.empty() ? ::unsetenv(key) == 0 : ::setenv(key, path.c_str(), 1) == 0,
          "set test-local phase trace");
  }
  ~PhaseTraceScope() {
    if (original) (void)::setenv(key, original->c_str(), 1);
    else (void)::unsetenv(key);
  }
};
api::EngineRequestContext Begin(const api::EngineRequestContext& owner,
                               scoped::ScopedValueStore* state = nullptr) {
  api::EngineBeginTransactionRequest request;
  request.context = owner;
  request.session_state = state;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  CheckApi(begun, "actual engine BEGIN");
  auto context = owner;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}
auto Inventory(const std::string& path) {
  observed.enabled = false;
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(path);
  Check(loaded.ok(), "reopen native inventory");
  Check(!loaded.publication_io.complete, "read cannot assert publication");
  return loaded.inventory;
}
void CheckState(const api::EngineRequestContext& context, txn::TransactionState state) {
  const auto inventory = Inventory(context.database_path);
  const auto found = std::find_if(inventory.entries.begin(), inventory.entries.end(),
      [&](const auto& e) { return e.identity.local_id.value == context.local_transaction_id; });
  Check(found != inventory.entries.end() &&
        found->identity.transaction_uuid.value == context.transaction_uuid && found->state == state,
        "exact composite transaction identity/state persisted");
}
bool HasDetail(const api::EngineApiResult& result, const std::string& detail) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto& d) { return d.detail.find(detail) != std::string::npos; });
}
void Exercise(const db::DatabaseCreateConfig& create, const api::EngineRequestContext& owner,
              const scratchbird::tests::FixtureEngineSession& session, Policy policy) {
  const std::string trace_path = create.path + ".commit-phases";
  PhaseTraceScope trace(policy == Policy::batched ? trace_path : "");
  auto retained = scoped::ScopedValueStore::Create(*mem::DefaultMemoryManager().allocator(),
      owner.database_uuid.bytes, owner.session_uuid.bytes);
  Check(retained.status == scoped::StoreStatus::ok, "admit binary-bound session state");
  auto* state = retained.owner.get();
  const auto key = scoped::ValueKey{scoped::ValueFamily::setting, Identity(UuidKind::object).value.bytes};
  const auto datatype = Identity(UuidKind::object).value.bytes;
  const auto label = Identity(UuidKind::object).value.bytes;
  const auto write = [&](std::uint8_t value, const api::EngineUuid& transaction) {
    const std::array<std::uint8_t, 1> payload{value};
    const auto scope = transaction.is_nil() ? scoped::ValueScope::session : scoped::ValueScope::transaction;
    Check(state->WriteAdmitted(owner.session_uuid.bytes, key, scope, transaction.bytes,
        state->Snapshot(owner.session_uuid.bytes).generation,
        {datatype, 1, label, false, payload}).ok(), "retain already-admitted scalar bytes");
  };
  const auto expect = [&](std::uint8_t value, const api::EngineUuid& transaction) {
    std::array<std::uint8_t, 1> bytes{};
    const auto result = state->ReadAdmitted(owner.session_uuid.bytes, key, transaction.bytes,
        state->Snapshot(owner.session_uuid.bytes).generation, bytes);
    Check(result.ok() && bytes[0] == value && result.datatype_uuid == datatype &&
          result.security_label_uuid == label, "exact scope value survives actual MGA decision");
  };
  auto base = Begin(owner, state);
  write(7, {}); write(9, base.transaction_uuid); write(11, {});
  // Binding a store from another database is not permission to retain this
  // owner's transaction, even when its session UUID happens to be identical.
  auto foreign = scoped::ScopedValueStore::Create(*mem::DefaultMemoryManager().allocator(),
      Identity(UuidKind::database).value.bytes, owner.session_uuid.bytes);
  Check(foreign.status == scoped::StoreStatus::ok, "foreign owner fixture");
  api::EngineBeginTransactionRequest wrong_owner;
  wrong_owner.context = owner; wrong_owner.isolation_level = "read_committed";
  wrong_owner.session_state = foreign.owner.get();
  Watch(create.path);
  const auto refused_owner = api::EngineBeginTransaction(wrong_owner);
  Check(!refused_owner.ok && !refused_owner.diagnostics.empty() &&
        refused_owner.diagnostics.front().code == "SECURITY.ACCESS_DENIED" &&
        observed.writes == 0 && observed.syncs == 0, "wrong state owner refuses before publication");
  scoped::StoreLimits limits; limits.active_transactions = 1;
  auto full = scoped::ScopedValueStore::Create(*mem::DefaultMemoryManager().allocator(),
      owner.database_uuid.bytes, owner.session_uuid.bytes, limits);
  Check(full.status == scoped::StoreStatus::ok, "bounded participant fixture");
  const auto occupied = Begin(owner, full.owner.get());
  wrong_owner.session_state = full.owner.get();
  Watch(create.path);
  const auto quota = api::EngineBeginTransaction(wrong_owner);
  Check(!quota.ok && !quota.diagnostics.empty() && quota.diagnostics.front().code == "RESOURCE.BUDGET_EXCEEDED" &&
        observed.writes == 0 && observed.syncs == 0, "state capacity refuses BEGIN before physical effects");
  api::EngineRollbackTransactionRequest close_occupied;
  close_occupied.context = occupied; close_occupied.session_state = full.owner.get();
  CheckApi(api::EngineRollbackTransaction(close_occupied), "settle bounded participant fixture");
  scratchbird::tests::FixtureEngineStatement statement(session, base);
  const auto& context = statement.context;
  // Benchmark counts and textual authority assertions cannot replace typed
  // publication policy. Each refusal leaves this real transaction committable.
  const auto before = Inventory(create.path);
  for (const auto& options : std::vector<std::vector<std::string>>{
           {"commit.durability_write_batching:enabled",
            "commit.durability_write_batching.dirty_pages:6"},
           {"commit.durability_write_batching:enabled",
            "commit.durability_write_batching.resource_pressure:true"},
           {"commit.durability_write_batching:required",
            "commit.durability_write_batching.resource_pressure:true"},
           {"commit.durability_write_batching:required",
            "commit.durability_write_batching.parser_authority:true"}}) {
    api::EngineCommitTransactionRequest request;
    request.context = context;
    request.session_state = state;
    request.option_envelopes = options;
    Watch(create.path);
    const auto refused = api::EngineCommitTransaction(request);
    Check(!refused.ok && HasDetail(refused, "benchmark_options_are_not_inventory_publication_authority"),
          "benchmark/pressure/parser options must not claim durability authority");
    Check(!refused.inventory_publication_io.complete && refused.engine_finality_known &&
          !refused.post_inventory_secondary_failure, "pre-effect refusal classification");
    Check(observed.writes == 0 && observed.syncs == 0, "refusal precedes physical database effects");
    CheckState(context, txn::TransactionState::active);
    expect(9, context.transaction_uuid); expect(11, {});
    Check(Inventory(create.path).publication_base->generation == before.publication_base->generation,
          "refusal preserves publication generation");
  }
  api::EngineCommitTransactionRequest request;
  request.context = context;
  request.session_state = state;
  request.inventory_page_sync_policy = static_cast<Policy>(0);
  Watch(create.path);
  const auto invalid = api::EngineCommitTransaction(request);
  Check(!invalid.ok && HasDetail(invalid, "inventory_page_sync_policy_invalid") &&
        observed.writes == 0 && observed.syncs == 0, "invalid typed policy is effect-free");
  request.inventory_page_sync_policy = policy;
  Watch(create.path, true);
  const auto failed = api::EngineCommitTransaction(request);
  Check(!failed.ok && !failed.inventory_publication_io.complete && !failed.engine_finality_known,
        "actual sync failure cannot assert durable completion or known finality");
  Check(observed.bodies != 0 && !observed.fail_body_sync, "failure reached real inventory body sync");
  CheckState(context, txn::TransactionState::active);
  expect(9, context.transaction_uuid); expect(11, {});
  // Recover before retrying; never mint a replacement transaction identity.
  Watch(create.path);
  const auto committed = api::EngineCommitTransaction(request);
  CheckApi(committed, "retry engine COMMIT through actual native inventory");
  const auto& io = committed.inventory_publication_io;
  Check(io.complete && io.sync_policy == policy && io.database_uuid == create.database_uuid.value &&
        io.filespace_uuid == create.filespace_uuid.value, "exact publication owner/policy");
  Check(io.publications == 1 && io.page_body_writes >= 2, "real multi-page inventory publication");
  Check(io.page_body_writes == observed.bodies && io.body_bytes_written == observed.bytes &&
        io.body_bytes_written == io.page_body_writes * (kPageSize - kHeaderSize),
        "receipt matches independently observed database body writes/bytes");
  Check(io.page_sync_calls == (policy == Policy::batched ? 1 : io.page_body_writes) &&
        observed.syncs >= io.page_sync_calls, "batch/per-page synchronization actually performed");
  Check(observed.syncs <= observed.bodies_at_sync.size(), "bounded syscall observation");
  unsigned body_syncs = 0;
  for (unsigned i = 0; i < observed.syncs; ++i) {
    const auto bodies = observed.bodies_at_sync[i];
    if (bodies == 0) continue;  // Earlier commit page barrier, not inventory publication.
    ++body_syncs;
    Check(bodies == (policy == Policy::batched ? io.page_body_writes : body_syncs),
          "actual syscall order must batch all bodies or sync each body exactly once");
  }
  Check(body_syncs == io.page_sync_calls, "receipt matches exact observed inventory sync sequence");
  Check(committed.engine_finality_known && !committed.post_inventory_secondary_failure &&
        committed.commit_finality_state == "committed_by_engine_inventory" &&
        committed.transaction_uuid == context.transaction_uuid &&
        committed.local_transaction_id == context.local_transaction_id, "exact MGA commit finality");
  const auto inventory = Inventory(create.path);
  Check(io.inventory_generation == inventory.publication_base->generation,
        "receipt matches reopened authoritative generation");
  CheckState(context, txn::TransactionState::committed);
  Check(state->Snapshot(owner.session_uuid.bytes).active_transactions == 0,
        "confirmed commit expires override");
  expect(11, {});
  // A separate real rollback expires its override without restoring an older
  // session value. This directly exercises the internal lifecycle participant;
  // these writes do not claim parser/datatype/authorization admission coverage.
  const auto rolling = Begin(owner, state);
  write(13, rolling.transaction_uuid); write(17, {});
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = rolling; rollback.session_state = state;
  Watch(create.path, true);
  const auto uncertain = api::EngineRollbackTransaction(rollback);
  Check(!uncertain.ok && !uncertain.engine_finality_known && !observed.fail_body_sync,
        "actual rollback sync failure remains uncertain");
  expect(13, rolling.transaction_uuid); expect(17, {});
  CheckState(rolling, txn::TransactionState::active);
  const auto rolled = api::EngineRollbackTransaction(rollback);
  CheckApi(rolled, "actual MGA rollback after recovery");
  Check(rolled.engine_finality_known && state->Snapshot(owner.session_uuid.bytes).active_transactions == 0,
        "confirmed rollback expires override");
  CheckState(rolling, txn::TransactionState::rolled_back);
  expect(17, {});
  if (policy == Policy::batched) {
    std::ifstream input(trace_path);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    Check(text.find("operation_id=transaction.commit\tok=true") != std::string::npos &&
          text.find("operation_id=transaction.commit\tok=false") != std::string::npos &&
          text.find("persist_transaction_inventory_us=") != std::string::npos &&
          text.find("shape_commit_result_us=") != std::string::npos &&
          text.find("total_us=") != std::string::npos,
          "enabled phase tracing preserves successful/failed timing evidence");
  }
  std::cout << "publication policy=" << static_cast<unsigned>(policy)
            << " pages=" << io.page_body_writes << " bytes=" << io.body_bytes_written
            << " syncs=" << io.page_sync_calls << '\n';
}
int main() {
  try {
    Check(mem::ConfigureDefaultMemoryManagerForFixture(mem::DefaultLocalEngineMemoryPolicy(),
          "ipar_commit_durability_batching_gate").ok(), "fixture memory policy");
    auto pattern = (std::filesystem::temp_directory_path() / "sb-commit-batch-XXXXXX").string();
    const auto* created_directory = ::mkdtemp(pattern.data());
    Check(created_directory != nullptr, "unique fixture directory");
    Directory directory{created_directory};
    db::DatabaseCreateConfig create;
    create.path = (directory.path / "commit.sbdb").string();
    create.database_uuid = Identity(UuidKind::database);
    create.filespace_uuid = Identity(UuidKind::filespace);
    create.page_size = kPageSize;
    create.creation_unix_epoch_millis = 1790000000000ull;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    const auto created = db::CreateDatabaseFile(create);
    if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ':'
                                << created.diagnostic.message_key << '\n';
    Check(created.ok(), "real credentialed database creation");
    auto inventory = Inventory(create.path);
    const auto capacity = scratchbird::storage::page::MaxTransactionInventoryEntriesPerPage(kPageSize);
    for (unsigned i = 0; i < capacity + 2; ++i) {
      const auto begun = txn::BeginLocalTransaction(inventory, Identity(UuidKind::transaction),
                                                   create.creation_unix_epoch_millis + i + 1);
      Check(begun.ok(), "real MGA allocation for multi-page inventory");
      const auto rolled_back = txn::RollbackLocalTransaction(begun.inventory,
          begun.entry.identity.local_id, create.creation_unix_epoch_millis + capacity + i + 10);
      Check(rolled_back.ok(), "settle padding transaction");
      inventory = rolled_back.inventory;
    }
    Check(db::PersistLocalTransactionInventoryToDatabase(create.path, inventory).ok(),
          "publish actual multi-page inventory");
    const auto owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
    scratchbird::tests::FixtureEngineSession session(owner);
    Exercise(create, owner, session, Policy::batched);
    Exercise(create, owner, session, Policy::per_page);
    std::cout << "PASS commit durability batching checks=" << checks << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    observed.enabled = false;
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
