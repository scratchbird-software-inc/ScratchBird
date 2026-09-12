// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool reject_write = false, reject_sync = false;
unsigned write_faults = 0, sync_faults = 0;
unsigned write_calls = 0, sync_calls = 0;
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t count, off_t offset) {
  ++write_calls;
  if (reject_write) { reject_write = false; ++write_faults; errno = EIO; return -1; }
  return __real_pwrite(fd, bytes, count, offset);
}
extern "C" int __wrap_fsync(int fd) {
  ++sync_calls;
  if (reject_sync) { reject_sync = false; ++sync_faults; errno = EIO; return -1; }
  return __real_fsync(fd);
}

namespace {
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace types = scratchbird::core::datatypes;
namespace uuid = scratchbird::core::uuid;
using platform::TypedUuid;
using platform::UuidKind;
using platform::u64;
constexpr platform::u32 page_size = 16384;
unsigned checks = 0;
void Check(bool ok, std::string_view why,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " line=" + std::to_string(at.line()));
}
TypedUuid Id(UuidKind kind) {
  static u64 sequence = 0;
  auto id = uuid::GenerateDurableEngineIdentityV7(kind, 1790000000000ull + ++sequence);
  Check(id.ok(), "identity generation");
  return id.value;
}
bool OwnershipError(const platform::DiagnosticRecord& diagnostic) {
  return diagnostic.diagnostic_code == "SB-STORAGE-DISK-OWNER-LOCK-HELD" ||
         diagnostic.diagnostic_code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD" ||
         diagnostic.diagnostic_code == "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD";
}
struct Fixture {
  std::filesystem::path root;
  std::string path;
  disk::FileDevice device;
  TypedUuid relation = Id(UuidKind::object);
  u64 first_page = 0;
  Fixture() {
    std::string pattern = (std::filesystem::temp_directory_path() / "sb_owned_mga.XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back(0);
    const auto made = ::mkdtemp(writable.data()); Check(made, "private temp root"); root = made;
    try {
      path = (root / "node.sbdb").string();
      db::DatabaseCreateConfig config;
      config.path = path; config.database_uuid = Id(UuidKind::database);
      config.filespace_uuid = Id(UuidKind::filespace); config.page_size = page_size;
      config.creation_unix_epoch_millis = 1790000000100ull;
      config.allow_minimal_resource_bootstrap = true; config.require_resource_seed_pack = false;
      config.require_bootstrap_principal = false; config.allow_uncredentialed_bootstrap = true;
      Check(db::CreateDatabaseFile(config).ok(), "create real node");
      Check(device.Open(path, disk::FileOpenMode::open_existing).ok(), "own node device");
      const auto size = device.Size(); Check(size.ok() && size.size_bytes % page_size == 0, "node page alignment");
      first_page = size.size_bytes / page_size;
    } catch (...) {
      device.Close(); std::error_code error; std::filesystem::remove_all(root, error); throw;
    }
  }
  ~Fixture() { device.Close(); std::error_code error; std::filesystem::remove_all(root, error); }
  mga::TransactionIdentity Begin() {
    auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&device, page_size);
    Check(loaded.ok(), "load owned inventory");
    auto begun = mga::BeginLocalTransaction(loaded.inventory, Id(UuidKind::transaction), 1790000000200ull);
    Check(begun.ok(), "begin actual MGA transaction");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&device, page_size, begun.inventory).ok(), "persist active inventory");
    return begun.entry.identity;
  }
  void Finish(mga::TransactionIdentity tx, bool commit) {
    auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&device, page_size);
    Check(loaded.ok(), "reload before finality");
    auto final = commit ? mga::CommitLocalTransaction(loaded.inventory, tx.local_id, 1790000000300ull)
                        : mga::RollbackLocalTransaction(loaded.inventory, tx.local_id, 1790000000300ull);
    Check(final.ok(), "actual MGA finality");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&device, page_size, final.inventory).ok(), "persist final inventory");
  }
  db::PhysicalMgaCowMutation Mutation(mga::TransactionIdentity tx, TypedUuid row, u64 page, std::string_view value) {
    db::PhysicalMgaCowMutation mutation;
    mutation.relation_uuid = relation; mutation.row_uuid = row;
    mutation.transaction_uuid = tx.transaction_uuid;
    mutation.existing_local_transaction_id = tx.local_id; mutation.use_existing_transaction = true;
    mutation.page_number = page;
    types::DatatypeBinaryValue cell; cell.type_id = types::CanonicalTypeId::character;
    cell.payload.assign(value.begin(), value.end()); mutation.cells.push_back({1, std::move(cell)});
    return mutation;
  }
  db::PhysicalMgaCowReadResult Read(u64 page, mga::TransactionIdentity reader = {}, u64 boundary = 0) {
    mga::VisibilitySnapshot snapshot;
    snapshot.reader_transaction = reader.local_id;
    snapshot.visible_through_local_transaction_id = boundary;
    snapshot.visible_through_local_transaction_id_is_boundary = true;
    auto result = db::ReadPhysicalMgaCowRowsFromOpenDevice(device, relation, page, snapshot, !reader.local_id.valid() && boundary == 0);
    Check(result.ok(), "read actual owned row page");
    return result;
  }
  std::vector<platform::byte> Bytes() {
    const auto size = device.Size(); Check(size.ok(), "read node size");
    std::vector<platform::byte> bytes(size.size_bytes);
    Check(device.ReadAt(0, bytes.data(), bytes.size()).ok(), "read node image");
    return bytes;
  }
  void Locked() {
    Check(device.is_open(), "writer closed owned device");
    disk::FileDevice competing;
    const auto open = competing.Open(path, disk::FileOpenMode::open_existing);
    Check(!open.ok() && OwnershipError(open.diagnostic), "second local open admitted");
    const auto child = ::fork(); Check(child >= 0, "fork ownership probe");
    if (child == 0) { ::execl("/proc/self/exe", "owned-device-probe", "--probe", path.c_str(), nullptr); ::_exit(99); }
    int status = 0; pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fresh process opened owned node");
  }
};
void Payload(const db::PhysicalMgaCowReadResult& read, std::string_view expected) {
  Check(read.visible_rows.size() == 1 && read.visible_rows[0].cells.size() == 1, "exact visible row count");
  const auto& bytes = read.visible_rows[0].cells[0].value.payload;
  Check(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == expected, "persisted payload mismatch");
}
void TransactionStateRefusals() {
  Fixture f;
  std::cout << "expected_persisted_state_refusal_cases=20\n" << std::flush;
  unsigned executed = 0;
  for (auto state : {mga::TransactionState::read_only_active, mga::TransactionState::prepared,
                     mga::TransactionState::committed, mga::TransactionState::rolled_back,
                     mga::TransactionState::active}) {
    const auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(loaded.ok(), "load state-refusal fixture inventory");
    const auto identity = Id(UuidKind::transaction);
    auto transaction = state == mga::TransactionState::read_only_active
        ? mga::BeginLocalReadOnlyTransaction(loaded.inventory, identity, 1790000000200ull)
        : mga::BeginLocalTransaction(loaded.inventory, identity, 1790000000200ull);
    Check(transaction.ok(), "begin state-refusal fixture transaction");
    if (state == mga::TransactionState::prepared)
      transaction = mga::PrepareLocalTransaction(transaction.inventory, transaction.entry.identity.local_id);
    else if (state == mga::TransactionState::committed)
      transaction = mga::CommitLocalTransaction(transaction.inventory, transaction.entry.identity.local_id, 1790000000300ull);
    else if (state == mga::TransactionState::rolled_back)
      transaction = mga::RollbackLocalTransaction(transaction.inventory, transaction.entry.identity.local_id, 1790000000300ull);
    else if (state == mga::TransactionState::active)
      transaction = mga::MarkLocalTransactionRollbackOnly(transaction.inventory, transaction.entry.identity.local_id);
    Check(transaction.ok(), "establish real lifecycle state");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, transaction.inventory).ok(),
          "persist real lifecycle state");
    const auto request = f.Mutation(transaction.entry.identity, Id(UuidKind::row), f.first_page, "must-not-write");
    db::PhysicalMgaCowMutationBatch batch; batch.mutations.push_back(request);
    const bool read_only = state == mga::TransactionState::read_only_active;
    const std::string_view expected_code = read_only ? "MGA.COW.READ_ONLY_TRANSACTION" : "MGA.COW.TRANSACTION_NOT_WRITABLE";
    const auto expected_status = read_only ? platform::StatusCode::mga_cow_read_only_transaction
                                          : platform::StatusCode::mga_cow_transaction_not_writable;
    for (bool reopen : {false,true}) {
      if (reopen) Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(),
                        "reopen state-refusal inventory");
      const auto before = f.Bytes();
      const auto prior_writes = write_calls, prior_syncs = sync_calls;
      const auto single = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, request);
      Check(!single.ok() && single.status.code == expected_status && single.diagnostic.diagnostic_code == expected_code,
            "single writer canonical lifecycle refusal");
      Check(f.Bytes() == before, "single refused writer changed durable node bytes"); ++executed;
      const auto many = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch);
      Check(!many.ok() && many.status.code == expected_status && many.diagnostic.diagnostic_code == expected_code,
            "batch writer canonical lifecycle refusal");
      Check(f.Bytes() == before, "batch refused writer changed durable node bytes"); ++executed;
      Check(write_calls == prior_writes && sync_calls == prior_syncs,
            "refused lifecycle state reached physical write or durability barrier");
    }
    f.Locked();
  }
  Check(executed == 20, "all persisted lifecycle refusal cases executed");
}
void Run() {
  TransactionStateRefusals();
  Fixture f; f.Locked();
  const auto row = Id(UuidKind::row); auto first = f.Begin();
  auto insert = f.Mutation(first, row, f.first_page, "original");
  auto written = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, insert);
  Check(written.ok() && written.row_version.row_uuid.kind == row.kind &&
        written.row_version.row_uuid.value == row.value, "owned-device insert");
  Check(f.Read(f.first_page).visible_rows.empty(), "uncommitted row leaked");
  Payload(f.Read(f.first_page, first), "original"); f.Locked(); f.Finish(first, true);
  Payload(f.Read(f.first_page), "original");
  auto second = f.Begin(); auto update = f.Mutation(second, row, f.first_page, "replacement");
  update.kind = db::PhysicalMgaCowMutationKind::update;
  Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, update).ok(), "owned-device update");
  Payload(f.Read(f.first_page), "original"); Payload(f.Read(f.first_page, second, first.local_id.value), "replacement");
  f.Finish(second, true); Payload(f.Read(f.first_page), "replacement");
  Payload(f.Read(f.first_page, {}, first.local_id.value), "original");
  auto rolled = f.Begin(); update.transaction_uuid = rolled.transaction_uuid; update.existing_local_transaction_id = rolled.local_id;
  update.kind = db::PhysicalMgaCowMutationKind::delete_row; update.cells.clear();
  Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, update).ok(), "owned-device delete");
  const auto own_delete = f.Read(f.first_page, rolled, second.local_id.value);
  Check(own_delete.visible_rows.empty() && own_delete.visible_delete_marker_count == 1, "own delete not visible");
  Payload(f.Read(f.first_page), "replacement");
  Check(f.Read(f.first_page).wait_for_transaction_count == 1, "foreign delete lost pending transaction");
  Check(!own_delete.rows.empty() && !own_delete.rows[0].metadata.payload_present, "delete fabricated payload");
  auto marker = own_delete.rows[0].metadata;
  mga::VisibilitySnapshot marker_snapshot; marker_snapshot.reader_transaction = rolled.local_id;
  marker_snapshot.visible_through_local_transaction_id = second.local_id.value;
  marker_snapshot.visible_through_local_transaction_id_is_boundary = true;
  Check(mga::EvaluateVisibility(marker, marker_snapshot).decision == mga::VisibilityDecision::invisible, "delete effect became ordinary user row");
  for (auto state : {mga::TransactionState::limbo, mga::TransactionState::recovering}) {
    marker.creator_transaction_state = state;
    Check(mga::EvaluateVersionEffectVisibility(marker, marker_snapshot).decision == mga::VisibilityDecision::requires_recovery, "delete effect ignored recovery state");
  }
  for (auto state : {mga::TransactionState::rolled_back, mga::TransactionState::failed_terminal}) {
    marker.creator_transaction_state = state;
    Check(mga::EvaluateVersionEffectVisibility(marker, marker_snapshot).decision == mga::VisibilityDecision::invisible, "terminal delete remained visible");
  }
  marker.creator_transaction_state = mga::TransactionState::active;
  marker_snapshot.allow_reader_own_uncommitted = false;
  Check(mga::EvaluateVersionEffectVisibility(marker, marker_snapshot).decision == mga::VisibilityDecision::wait_for_transaction, "disabled own-write marker visibility ignored");
  f.Finish(rolled, false); Payload(f.Read(f.first_page), "replacement");

  auto batch_tx = f.Begin(); db::PhysicalMgaCowMutationBatch batch;
  batch.mutations.push_back(f.Mutation(batch_tx, Id(UuidKind::row), f.first_page + 1, "page-one"));
  batch.mutations.push_back(f.Mutation(batch_tx, Id(UuidKind::row), f.first_page + 2, "page-two"));
  const auto before = f.Bytes();
  { auto invalid = batch; invalid.mutations.back().transaction_uuid = first.transaction_uuid;
    Check(!db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, invalid).ok(), "mixed creator batch accepted");
    Check(f.Bytes() == before, "invalid batch changed node bytes"); }
  { auto invalid = batch; invalid.mutations.back().cells.clear();
    Check(!db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, invalid).ok(), "missing payload batch accepted");
    Check(f.Bytes() == before, "later invalid request partially published batch"); }
  Check(!db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, {}).ok(), "empty batch accepted");
  auto result = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch);
  Check(result.ok() && result.written_rows == 2 && result.pages_written == 2, "actual two-page batch write");
  Check(f.Read(f.first_page + 1).visible_rows.empty() && f.Read(f.first_page + 2).visible_rows.empty(), "batch publication invented finality");
  Payload(f.Read(f.first_page + 1, batch_tx), "page-one"); Payload(f.Read(f.first_page + 2, batch_tx), "page-two");
  f.Finish(batch_tx, true); f.Locked();
  Payload(f.Read(f.first_page + 1), "page-one"); Payload(f.Read(f.first_page + 2), "page-two");
  const auto pending = f.Begin();
  auto read_only_mutation = f.Mutation(pending, Id(UuidKind::row), f.first_page + 3, "must-not-write");
  db::PhysicalMgaCowMutationBatch read_only_batch; read_only_batch.mutations.push_back(read_only_mutation);
  auto invalid = insert; invalid.relation_uuid.value.bytes[6] = 0x40;
  const auto stable = f.Bytes();
  Check(!db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, invalid).ok(), "v4 system relation accepted");
  Check(f.Bytes() == stable, "invalid single write changed node"); f.Locked();

  Check(f.device.Close().ok(), "release owned node");
  Check(!db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, insert).ok(), "closed device write accepted");
  Check(!db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch).ok(), "closed device batch accepted");
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing_read_only).ok(), "read-only node ownership");
  Check(!db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, read_only_mutation).ok(), "read-only write accepted");
  Check(!db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, read_only_batch).ok(), "read-only batch accepted");
  Check(f.Bytes() == stable, "read-only failures changed persisted bytes");
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen persisted node");
  Payload(f.Read(f.first_page), "replacement"); Payload(f.Read(f.first_page + 1), "page-one"); Payload(f.Read(f.first_page + 2), "page-two");
  f.Locked();
  // Existing path-owning callers still reach the same real mutation path.
  Check(f.device.Close().ok(), "release for path-owning wrapper");
  db::PhysicalMgaCowMutationRequest legacy;
  static_cast<db::PhysicalMgaCowMutation&>(legacy) = read_only_mutation; legacy.database_path = f.path;
  Check(db::WritePhysicalMgaCowUnpublishedMutation(legacy).ok(), "path-owning insert regression");
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reacquire after wrapper");
  Payload(f.Read(f.first_page + 3, pending), "must-not-write"); f.Finish(pending, false);
  Check(f.Read(f.first_page + 3).visible_rows.empty(), "wrapper rollback not durable");
  auto wrapper_tx = f.Begin(); db::PhysicalMgaCowMutationBatchRequest wrapper_batch;
  for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
    db::PhysicalMgaCowMutationRequest item;
    static_cast<db::PhysicalMgaCowMutation&>(item) = f.Mutation(wrapper_tx, Id(UuidKind::row), f.first_page + 4 + ordinal, "wrapper-batch");
    item.database_path = f.path; wrapper_batch.mutations.push_back(std::move(item));
  }
  const auto before_wrapper = f.Bytes();
  Check(f.device.Close().ok(), "release for batch wrapper");
  { auto mixed = wrapper_batch; mixed.mutations.back().database_path = f.path + ".other";
    Check(!db::WritePhysicalMgaCowUnpublishedMutationBatch(mixed).ok(), "mixed-node wrapper batch accepted"); }
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen after wrapper rejection");
  Check(f.Bytes() == before_wrapper, "mixed-node wrapper mutated node");
  Check(f.device.Close().ok(), "release valid wrapper batch");
  const auto wrapper_result = db::WritePhysicalMgaCowUnpublishedMutationBatch(wrapper_batch);
  Check(wrapper_result.ok() && wrapper_result.written_rows == 2, "path-owning batch regression");
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen after wrapper batch");
  f.Finish(wrapper_tx, true);
  Payload(f.Read(f.first_page + 4), "wrapper-batch"); Payload(f.Read(f.first_page + 5), "wrapper-batch");
  // Repeated own writes must bind the writer's current head, not wait on self.
  for (bool together : {false, true}) {
    auto tx = f.Begin(); auto repeated_row = Id(UuidKind::row);
    const auto page = f.first_page + (together ? 7 : 6);
    auto item = f.Mutation(tx, repeated_row, page, "initial");
    db::PhysicalMgaCowMutationBatch sequence; sequence.mutations.push_back(item);
    item.kind = db::PhysicalMgaCowMutationKind::update;
    item.cells[0].value.payload = {'u', 'p', 'd', 'a', 't', 'e'}; sequence.mutations.push_back(item);
    item.kind = db::PhysicalMgaCowMutationKind::delete_row; item.cells.clear(); sequence.mutations.push_back(item);
    item = f.Mutation(tx, repeated_row, page, "reinserted"); sequence.mutations.push_back(item);
    if (together) {
      auto applied = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, sequence);
      Check(applied.ok() && applied.written_rows == 4 && applied.pages_written == 1, "own-write batch chain failed");
    } else {
      unsigned version = 0;
      for (const auto& mutation : sequence.mutations) {
        const auto applied = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, mutation);
        Check(applied.ok() && applied.row_version.row_version == ++version, "repeated own write waited on self or lost chain");
        if (mutation.kind == db::PhysicalMgaCowMutationKind::delete_row)
          Check(f.Read(page, tx).visible_rows.empty(), "repeated own DELETE leaked old row");
      }
    }
    Payload(f.Read(page, tx), "reinserted"); Check(f.Read(page).visible_rows.empty(), "own repeated chain leaked to independent reader");
    f.Finish(tx, true); Payload(f.Read(page), "reinserted");
  }
  const auto failure_tx = f.Begin();
  auto failure_row = f.Mutation(failure_tx, Id(UuidKind::row), f.first_page + 8, "unpublished-failure");
  const auto before_io = f.Bytes();
  reject_write = true;
  const auto failed_write = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, failure_row);
  Check(!failed_write.ok() && write_faults == 1 && !reject_write, "real pwrite failure not propagated");
  Check(f.Bytes() == before_io, "failed first write mutated node"); f.Locked();
  reject_sync = true;
  const auto failed_sync = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, failure_row);
  Check(!failed_sync.ok() && sync_faults == 1 && !reject_sync, "real fsync failure not propagated");
  Check(f.Read(f.first_page + 8).visible_rows.empty(), "failed sync published transaction visibility");
  f.Locked(); f.Finish(failure_tx, false);
  Check(f.Read(f.first_page + 8).visible_rows.empty(), "failed publication survived rollback as visible");
  const auto failed_batch_tx = f.Begin(); db::PhysicalMgaCowMutationBatch failing_batch;
  failing_batch.mutations.push_back(f.Mutation(failed_batch_tx, Id(UuidKind::row), f.first_page + 9, "failed-batch-one"));
  failing_batch.mutations.push_back(f.Mutation(failed_batch_tx, Id(UuidKind::row), f.first_page + 10, "failed-batch-two"));
  reject_sync = true;
  const auto failed_batch = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, failing_batch);
  Check(!failed_batch.ok() && sync_faults == 2 && !reject_sync, "batch fsync failure not propagated");
  Check(f.Read(f.first_page + 9).visible_rows.empty() && f.Read(f.first_page + 10).visible_rows.empty(), "failed batch leaked independent rows");
  f.Locked(); f.Finish(failed_batch_tx, false);
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen after failed publication");
  Check(f.Read(f.first_page + 9).visible_rows.empty() && f.Read(f.first_page + 10).visible_rows.empty(), "failed batch visibility after restart");
}
}  // namespace
int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--probe") {
    disk::FileDevice device; const auto opened = device.Open(argv[2], disk::FileOpenMode::open_existing);
    return !opened.ok() && OwnershipError(opened.diagnostic) ? 0 : 1;
  }
  try { Run(); std::cout << "owned_device checks=" << checks << " failures=0\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
