// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "isolation.hpp"
#include "transaction_recovery.hpp"
#include "transaction_inventory_validation.hpp"
#include "physical_mga_cow_store.hpp"
#include "transaction_inventory_page.hpp"
#include "page_header.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"
#include <cerrno>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool reject_write = false, reject_sync = false;
unsigned write_faults = 0, sync_faults = 0;
unsigned write_calls = 0, sync_calls = 0;
enum class OwnedFault { none, row_write, row_sync, after_row_write_exception,
                        rollback_write, exception_rollback_write,
                        batch_release_write, batch_process_exit };
OwnedFault owned_fault = OwnedFault::none;
off_t owned_row_offset = 0;
unsigned owned_faults = 0;
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t count, off_t offset) {
  ++write_calls;
  if (owned_fault != OwnedFault::none && offset == owned_row_offset) {
    const auto fault = owned_fault; owned_fault = OwnedFault::none; ++owned_faults;
    if (fault == OwnedFault::row_write || fault == OwnedFault::rollback_write) {
      if (fault == OwnedFault::rollback_write) reject_write = true;
      errno = EIO; return -1;
    }
    const auto written = __real_pwrite(fd, bytes, count, offset);
    if (fault == OwnedFault::batch_process_exit) ::_exit(written == static_cast<ssize_t>(count) ? 74 : 75);
    if (fault == OwnedFault::batch_release_write) { reject_write = true; return written; }
    if (fault == OwnedFault::after_row_write_exception || fault == OwnedFault::exception_rollback_write) {
      if (fault == OwnedFault::exception_rollback_write) reject_write = true;
      throw std::bad_alloc();
    }
    reject_sync = true;
    return written;
  }
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
  mga::TransactionIdentity Begin(bool read_only = false) {
    auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&device, page_size);
    Check(loaded.ok(), "load owned inventory");
    auto begun = read_only
        ? mga::BeginLocalReadOnlyTransaction(loaded.inventory, Id(UuidKind::transaction), 1790000000200ull)
        : mga::BeginLocalTransaction(loaded.inventory, Id(UuidKind::transaction), 1790000000200ull);
    Check(begun.ok(), "begin actual MGA transaction");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&device, page_size, begun.inventory).ok(), "persist active inventory");
    return begun.entry.identity;
  }
  void Finish(mga::TransactionIdentity tx, bool commit) {
    db::PhysicalMgaCowFinalization request;
    request.transaction = tx;
    request.decision = commit ? db::PhysicalMgaCowFinalizeDecision::commit
                              : db::PhysicalMgaCowFinalizeDecision::rollback;
    request.final_unix_epoch_millis = 1790000000300ull;
    const auto final = db::FinalizePhysicalMgaCowTransactionToOpenDevice(device, request);
    Check(final.ok(), "actual retained-device MGA finality");
    Check(final.transaction_entry.identity.local_id.value == tx.local_id.value &&
          final.transaction_entry.identity.transaction_uuid.value == tx.transaction_uuid.value,
          "finality receipt changed transaction identity");
    Check(final.transaction_entry.state == (commit ? mga::TransactionState::committed
                                                  : mga::TransactionState::rolled_back),
          "finality receipt lost exact terminal state");
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
  db::PhysicalMgaCowReadResult Read(u64 page, mga::TransactionIdentity reader = {}, u64 boundary = 0,
                                  std::source_location at = std::source_location::current()) {
    mga::VisibilitySnapshot snapshot;
    snapshot.reader_transaction = reader.local_id;
    snapshot.visible_through_local_transaction_id = boundary;
    snapshot.visible_through_local_transaction_id_is_boundary = true;
    auto result = db::ReadPhysicalMgaCowRowsFromOpenDevice(device, relation, page, snapshot,
        !reader.local_id.valid() && boundary == 0, reader);
    if (!result.ok()) std::cerr << "read_line=" << at.line() << " page=" << page << ' ' << result.diagnostic.diagnostic_code
                                << ':' << result.diagnostic.message_key << '\n';
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

// Independent-process oracle contains only explicitly expected terminal
// identities/states, never an image copied from the implementation's result.
void WriteFinalityOracle(const std::string& path,
    const std::vector<std::pair<mga::TransactionIdentity, mga::TransactionState>>& expected) {
  std::ofstream out(path + ".finality-oracle", std::ios::binary | std::ios::trunc);
  for (const auto& [tx, state] : expected) {
    std::array<platform::byte, 25> row{};
    platform::StoreLittle64(row.data(), tx.local_id.value);
    std::copy(tx.transaction_uuid.value.bytes.begin(), tx.transaction_uuid.value.bytes.end(), row.begin() + 8);
    row[24] = static_cast<platform::byte>(state);
    out.write(reinterpret_cast<const char*>(row.data()), row.size());
  }
  out.close(); Check(out.good(), "write independent finality oracle");
}
int VerifyFinalityOracle(const std::string& path) {
  const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(path);
  if (!inventory.ok()) return 10;
  std::ifstream in(path + ".finality-oracle", std::ios::binary);
  if (!in) return 11;
  unsigned count = 0;
  std::array<platform::byte, 25> row{};
  while (in.read(reinterpret_cast<char*>(row.data()), row.size())) {
    const auto found = mga::LookupLocalTransaction(inventory.inventory,
        mga::MakeLocalTransactionId(platform::LoadLittle64(row.data())));
    if (!found.ok() || found.entry.identity.scope != mga::TransactionScope::local_node ||
        found.entry.identity.transaction_uuid.kind != UuidKind::transaction ||
        !std::equal(row.begin() + 8, row.begin() + 24, found.entry.identity.transaction_uuid.value.bytes.begin()) ||
        static_cast<platform::byte>(found.entry.state) != row[24]) return 12;
    ++count;
  }
  return in.eof() && in.gcount() == 0 && count == 3 ? 0 : 13;
}
void ReaderIdentityBeforeMaterialization() {
  Fixture f;
  const auto writer = f.Begin();
  const auto other = f.Begin();
  const auto mutation = f.Mutation(writer, Id(UuidKind::row), f.first_page, "writer-private");
  Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, mutation).ok(), "create private native row");
  Payload(f.Read(f.first_page, writer), "writer-private");
  Check(f.Read(f.first_page, other).visible_rows.empty(), "other transaction read uncommitted writer row");
  Check(f.Read(f.first_page).visible_rows.empty(), "anonymous reader saw uncommitted writer row");
  const auto before = f.Bytes();
  mga::VisibilitySnapshot snapshot;
  snapshot.reader_transaction = writer.local_id;
  snapshot.visible_through_local_transaction_id = other.local_id.value;
  snapshot.visible_through_local_transaction_id_is_boundary = true;
  const auto refused = [&](const mga::TransactionIdentity& identity,
                           const mga::VisibilitySnapshot& requested, bool latest,
                           std::string_view reason) {
    const auto read = db::ReadPhysicalMgaCowRowsFromOpenDevice(
        f.device, f.relation, f.first_page, requested, latest, identity);
    Check(!read.ok() && read.diagnostic.diagnostic_code == "CATALOG.INVALID_INPUT" &&
        read.diagnostic.message_key == reason, "wrong native reader identity refusal");
    Check(read.rows.empty() && read.visible_rows.empty() && read.row_page.rows.empty() &&
        read.version_metadata.empty() && read.visible_delete_marker_count == 0 &&
        read.wait_for_transaction_count == 0 && read.recovery_required_count == 0 &&
        read.rolled_back_version_count == 0, "refused reader received partial materialization");
    Check(f.device.is_open() && f.Bytes() == before, "reader refusal changed node or owner");
  };
  constexpr auto invalid = "storage.physical_mga_cow.reader_identity_invalid";
  constexpr auto mismatch = "storage.physical_mga_cow.reader_identity_mismatch";
  for (bool latest : {false, true}) {
    refused({}, snapshot, latest, invalid);
    refused(other, snapshot, latest, invalid);
    auto forged = writer; forged.transaction_uuid = other.transaction_uuid;
    refused(forged, snapshot, latest, mismatch);
    for (auto scope : {mga::TransactionScope::unknown, mga::TransactionScope::cluster_global,
                       static_cast<mga::TransactionScope>(65535)}) {
      forged = writer; forged.scope = scope;
      refused(forged, snapshot, latest, invalid);
    }
    for (unsigned version = 0; version < 16; ++version) {
      if (version == 7) continue;
      forged = writer;
      forged.transaction_uuid.value.bytes[6] =
          (forged.transaction_uuid.value.bytes[6] & 0x0f) | (version << 4);
      refused(forged, snapshot, latest, invalid);
    }
    forged = writer; forged.transaction_uuid.value.bytes[8] &= 0x3f;
    refused(forged, snapshot, latest, invalid);
    forged = writer; forged.transaction_uuid.kind = UuidKind::row;
    refused(forged, snapshot, latest, invalid);
    forged = writer; forged.transaction_uuid.value = {};
    refused(forged, snapshot, latest, invalid);
    forged = writer; forged.local_id = {};
    refused(forged, snapshot, latest, invalid);
    auto no_reader = snapshot; no_reader.reader_transaction = {};
    refused(writer, no_reader, latest, invalid);
    forged = {}; forged.scope = mga::TransactionScope::local_node;
    refused(forged, no_reader, latest, invalid);
    forged = writer; forged.local_id = mga::MakeLocalTransactionId(std::numeric_limits<u64>::max());
    auto missing = snapshot; missing.reader_transaction = forged.local_id;
    refused(forged, missing, latest, mismatch);
  }
  f.Locked();
  // The path-owning API must not reconstruct the missing UUID from a number.
  Check(f.device.Close().ok(), "release for reader path wrapper");
  db::PhysicalMgaCowReadRequest request;
  request.database_path = f.path; request.relation_uuid = f.relation; request.page_number = f.first_page;
  request.use_latest_committed_snapshot = false; request.visibility_snapshot = snapshot;
  Check(!db::ReadPhysicalMgaCowRows(request).ok(), "path reader accepted number-only authority");
  request.reader_identity = writer;
  Payload(db::ReadPhysicalMgaCowRows(request), "writer-private");
  request.reader_identity.transaction_uuid = other.transaction_uuid;
  Check(!db::ReadPhysicalMgaCowRows(request).ok(), "path reader accepted mismatched UUID");
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reclaim reader owner");
  Check(f.Bytes() == before, "path read changed node bytes");
  f.Finish(writer, true);
  Payload(f.Read(f.first_page, other, other.local_id.value), "writer-private");
  Payload(f.Read(f.first_page), "writer-private");
  f.Finish(other, false);
}

void PublishedSnapshotNativeVisibility() {
  for (bool deleted : {false, true}) for (bool prepared : {false, true}) {
    Fixture f;
    const auto row_uuid = Id(UuidKind::row);
    const auto base = f.Begin();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
        f.Mutation(base, row_uuid, f.first_page, "before-snapshot")).ok(), "write snapshot base");
    f.Finish(base, true);
    const auto writer = f.Begin();
    auto mutation = f.Mutation(writer, row_uuid, f.first_page, "after-snapshot");
    mutation.kind = deleted ? db::PhysicalMgaCowMutationKind::delete_row : db::PhysicalMgaCowMutationKind::update;
    if (deleted) mutation.cells.clear();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, mutation).ok(), "write snapshot successor");
    if (prepared) {
      const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
      Check(inventory.ok(), "load before prepare");
      const auto prepare = mga::PrepareLocalTransaction(inventory.inventory, writer.local_id);
      Check(prepare.ok() && db::PersistLocalTransactionInventoryToOpenDevice(
          &f.device, page_size, prepare.inventory).ok(), "persist prepared snapshot exclusion");
    }
    const auto marker = f.Begin(); f.Finish(marker, true);
    const auto reader = f.Begin();
    const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(inventory.ok(), "load snapshot publication inventory");
    const auto captured = mga::CreateLocalTransactionSnapshot(inventory.inventory, reader.local_id);
    Check(captured.ok(), "capture complete local native visibility");
    const auto published = mga::PublishStatementStableSnapshotVector(
        inventory.inventory, reader.local_id, 1790000000400ull);
    Check(published.ok() && published.descriptor.visible_committed_high_watermark == marker.local_id.value,
        "publish actual high-water-above-writer snapshot");
    const auto& exclusions = prepared ? published.descriptor.in_doubt_excluded_local_transaction_ids
                                     : published.descriptor.active_excluded_local_transaction_ids;
    Check(std::find(exclusions.begin(), exclusions.end(), writer.local_id.value) != exclusions.end(),
        "snapshot failed to capture writer exclusion");
    auto pin = mga::RetainPublishedSnapshotVector(published.descriptor.snapshot_uuid);
    Check(pin.valid(), "retain actual native snapshot owner");
    const auto read = [&] {
      auto result = db::ReadPhysicalMgaCowRowsFromOpenDevice(
          f.device, f.relation, f.first_page, {}, false, reader, &pin);
      Check(result.ok(), "read pinned native snapshot");
      return result;
    };
    Payload(read(), "before-snapshot");
    f.Finish(writer, true);
    // The writer was below the captured committed high water but in flight.
    // Re-reading today's inventory must not expose its later update/delete.
    Payload(read(), "before-snapshot");
    Payload(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation, f.first_page,
        captured.visibility_snapshot, false, reader), "before-snapshot");
    if (deleted) Check(f.Read(f.first_page).visible_rows.empty(), "latest snapshot missed committed DELETE");
    else Payload(f.Read(f.first_page), "after-snapshot");
    const auto own_page = f.first_page + 1;
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
        f.Mutation(reader, Id(UuidKind::row), own_page, "reader-own")).ok(), "write pin owner row");
    Payload(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation, own_page,
        {}, false, reader, &pin), "reader-own");
    const auto foreign = f.Begin();
    const auto before = f.Bytes();
    const auto no_materialization = [&](const db::PhysicalMgaCowReadResult& refused) {
      Check(!refused.ok() && refused.rows.empty() && refused.visible_rows.empty() &&
          refused.row_page.rows.empty() && refused.version_metadata.empty() &&
          refused.visible_delete_marker_count == 0 && refused.wait_for_transaction_count == 0 &&
          refused.recovery_required_count == 0 && refused.rolled_back_version_count == 0,
          "invalid snapshot published row state");
      Check(f.device.is_open() && f.Bytes() == before, "invalid snapshot changed node/ownership");
    };
    no_materialization(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
        f.first_page, {}, false, foreign, &pin));
    no_materialization(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
        f.first_page, {}, false, {}, &pin));
    no_materialization(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
        f.first_page, {}, true, reader, &pin));
    for (unsigned field = 0; field != 9; ++field) {
      mga::VisibilitySnapshot raw;
      if (field == 0) raw.reader_transaction = reader.local_id;
      if (field == 1) raw.visible_through_local_transaction_id = marker.local_id.value;
      if (field == 2) raw.visible_through_local_transaction_id_is_boundary = true;
      if (field == 3) raw.allow_reader_own_uncommitted = false;
      if (field == 4) raw.recovery_context = true;
      if (field == 5) raw.active_excluded_local_transaction_ids = {writer.local_id.value};
      if (field == 6) raw.in_doubt_excluded_local_transaction_ids = {writer.local_id.value};
      if (field == 7) raw.visible_through_commit_sequence = 1;
      if (field == 8) raw.visible_through_commit_sequence_is_boundary = true;
      no_materialization(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
          f.first_page, raw, false, reader, &pin));
    }
    mga::PublishedSnapshotPin absent;
    no_materialization(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
        f.first_page, {}, false, reader, &absent));
    f.Locked();
    mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);
    Check(!mga::RetainPublishedSnapshotVector(published.descriptor.snapshot_uuid).valid(),
        "released publication issued new pin");
    Payload(read(), "before-snapshot");
    Check(f.device.Close().ok(), "release native owner for path pinned read");
    db::PhysicalMgaCowReadRequest request;
    request.database_path = f.path; request.relation_uuid = f.relation;
    request.page_number = f.first_page; request.reader_identity = reader;
    request.use_latest_committed_snapshot = false; request.snapshot_pin = &pin;
    Payload(db::ReadPhysicalMgaCowRows(request), "before-snapshot");
    Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen pinned native node");
    Check(f.Bytes() == before, "pinned path read mutated node");
    if (prepared) {
      mga::RevokePublishedSnapshotVector(published.descriptor.snapshot_uuid);
      no_materialization(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
          f.first_page, {}, false, reader, &pin));
      f.Finish(reader, false);
    } else {
      f.Finish(reader, false);
      const auto result = db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation,
          f.first_page, {}, false, reader, &pin);
      Check(!result.ok() && result.visible_rows.empty() && result.row_page.rows.empty() &&
          result.diagnostic.message_key == "storage.physical_mga_cow.snapshot_pin_owner_mismatch",
          "terminal owner reused snapshot pin");
    }
    pin.Release(); f.Finish(foreign, false);
  }
}

int VerifyStartSnapshot(const std::string& path) {
  std::ifstream oracle(path + ".start-oracle", std::ios::binary);
  std::array<platform::byte, 72> bytes{};
  if (!oracle.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || oracle.peek() != EOF) return 21;
  const auto reader_number = platform::LoadLittle64(bytes.data());
  const auto writer_number = platform::LoadLittle64(bytes.data() + 8);
  const auto expected_begin = platform::LoadLittle64(bytes.data() + 16);
  const auto expected_writer_commit = platform::LoadLittle64(bytes.data() + 24);
  const auto page = platform::LoadLittle64(bytes.data() + 32);
  TypedUuid relation; relation.kind = UuidKind::object;
  std::copy(bytes.begin() + 40, bytes.begin() + 56, relation.value.bytes.begin());
  TypedUuid reader_uuid; reader_uuid.kind = UuidKind::transaction;
  std::copy(bytes.begin() + 56, bytes.end(), reader_uuid.value.bytes.begin());
  disk::FileDevice device;
  if (!device.Open(path, disk::FileOpenMode::open_existing).ok()) return 22;
  const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&device, page_size);
  if (!inventory.ok()) return 23;
  const auto reader = mga::LookupLocalTransaction(inventory.inventory, mga::MakeLocalTransactionId(reader_number));
  const auto writer = mga::LookupLocalTransaction(inventory.inventory, mga::MakeLocalTransactionId(writer_number));
  if (!reader.ok() || !writer.ok() || reader.entry.identity.transaction_uuid.value != reader_uuid.value ||
      reader.entry.begin_visible_through_commit_sequence != expected_begin ||
      writer.entry.commit_sequence != expected_writer_commit ||
      inventory.inventory.next_commit_sequence != expected_writer_commit + 1) return 24;
  const auto captured = mga::CreateLocalTransactionSnapshot(inventory.inventory, reader.entry.identity.local_id);
  if (!captured.ok()) return 25;
  const auto boundary = mga::SnapshotPolicyForIsolation(mga::IsolationLevel::repeatable_read, captured.snapshot);
  const auto rows = db::ReadPhysicalMgaCowRowsFromOpenDevice(device, relation, page, boundary,
      false, reader.entry.identity);
  if (!rows.ok() || rows.visible_rows.size() != 1 || rows.visible_rows[0].cells.size() != 1) return 26;
  const auto& payload = rows.visible_rows[0].cells[0].value.payload;
  return std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()) == "begin-visible" ? 0 : 27;
}

void TransactionStartCommitOrder() {
  for (bool deleted : {false, true}) for (bool read_only : {false, true}) {
    Fixture f;
    const auto initial = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(initial.ok(), "load initial commit order");
    const auto first_commit = initial.inventory.next_commit_sequence;
    const auto row = Id(UuidKind::row);
    const auto base = f.Begin();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
        f.Mutation(base, row, f.first_page, "begin-visible")).ok(), "write transaction-start base");
    f.Finish(base, true);
    const auto writer = f.Begin();
    auto replacement = f.Mutation(writer, row, f.first_page, "committed-later");
    replacement.kind = deleted ? db::PhysicalMgaCowMutationKind::delete_row : db::PhysicalMgaCowMutationKind::update;
    if (deleted) replacement.cells.clear();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, replacement).ok(), "stage transaction-start successor");
    const auto marker = f.Begin(); f.Finish(marker, true);
    const auto reader = f.Begin(read_only);
    f.Finish(writer, true);
    Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen transaction-start inventory");
    const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(inventory.ok() && inventory.inventory.next_commit_sequence == first_commit + 3, "durable next commit sequence");
    const auto own = mga::LookupLocalTransaction(inventory.inventory, reader.local_id);
    const auto committed = mga::LookupLocalTransaction(inventory.inventory, writer.local_id);
    Check(own.ok() && own.entry.begin_visible_through_commit_sequence == first_commit + 1 &&
        committed.ok() && committed.entry.commit_sequence == first_commit + 2, "durable begin/commit sequence order");
    const auto captured = mga::CreateLocalTransactionSnapshot(inventory.inventory, reader.local_id);
    Check(captured.ok(), "reconstruct from durable begin boundary");
    for (auto level : {mga::IsolationLevel::repeatable_read, mga::IsolationLevel::serializable}) {
      auto boundary = mga::SnapshotPolicyForIsolation(level, captured.snapshot);
      Check(boundary.visible_through_commit_sequence_is_boundary &&
          boundary.visible_through_commit_sequence == first_commit + 1, "isolation dropped durable begin order");
      Payload(db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation, f.first_page,
          boundary, false, reader), "begin-visible");
    }
    const auto current = mga::SnapshotPolicyForIsolation(mga::IsolationLevel::read_committed, captured.snapshot);
    const auto rows = db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device, f.relation, f.first_page, current, false, reader);
    Check(rows.ok(), "read-committed current boundary");
    if (deleted) Check(rows.visible_rows.empty(), "read committed missed later DELETE");
    else Payload(rows, "committed-later");
    std::array<platform::byte, 72> expected{};
    platform::StoreLittle64(expected.data(), reader.local_id.value);
    platform::StoreLittle64(expected.data() + 8, writer.local_id.value);
    platform::StoreLittle64(expected.data() + 16, first_commit + 1);
    platform::StoreLittle64(expected.data() + 24, first_commit + 2);
    platform::StoreLittle64(expected.data() + 32, f.first_page);
    std::copy(f.relation.value.bytes.begin(), f.relation.value.bytes.end(), expected.begin() + 40);
    std::copy(reader.transaction_uuid.value.bytes.begin(), reader.transaction_uuid.value.bytes.end(), expected.begin() + 56);
    std::ofstream oracle(f.path + ".start-oracle", std::ios::binary);
    oracle.write(reinterpret_cast<const char*>(expected.data()), expected.size()); oracle.close();
    Check(oracle.good() && f.device.Close().ok(), "publish independent begin-order oracle and release owner");
    const auto child = ::fork(); Check(child >= 0, "fork fresh begin-snapshot reader");
    if (child == 0) { ::execl("/proc/self/exe", "start-snapshot-probe", "--start-snapshot-probe", f.path.c_str(), nullptr); ::_exit(99); }
    int status = 0; pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fresh process lost transaction-start visibility");
    Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reclaim begin-snapshot owner");
    f.Finish(reader, false);
  }
}

int VerifyCatalogVersion(const char* path) {
  std::ifstream oracle(std::string(path) + ".catalog-oracle", std::ios::binary);
  std::array<platform::byte, 24> header{};
  oracle.read(reinterpret_cast<char*>(header.data()), header.size());
  if (!oracle) return 31;
  const auto page = platform::LoadLittle64(header.data());
  TypedUuid relation; relation.kind = UuidKind::object;
  std::copy(header.begin() + 8, header.end(), relation.value.bytes.begin());
  const std::vector<platform::byte> expected(std::istreambuf_iterator<char>(oracle), {});
  disk::FileDevice device;
  if (!device.Open(path, disk::FileOpenMode::open_existing).ok()) return 32;
  const auto rows = db::ReadNativeCatalogVersionsFromOpenDevice(device, relation, page, {}, true);
  if (!rows.ok() || rows.rows.size() != 1 || rows.rows[0].provisional) return 33;
  const auto actual = scratchbird::core::catalog::EncodeCatalogMetadataVersion(rows.rows[0].metadata);
  return actual.ok() && actual.bytes == expected ? 0 : 34;
}

void NativeCatalogMetadataVersions() {
  namespace catalog = scratchbird::core::catalog;
  Fixture f;
  const auto created = f.Begin();
  catalog::CatalogMetadataVersion metadata;
  metadata.record.header.kind = catalog::CatalogRecordKind::sql_object;
  metadata.record.header.row_uuid = Id(UuidKind::row);
  metadata.record.header.object_uuid = Id(UuidKind::object);
  metadata.record.header.parent_uuid = Id(UuidKind::schema);
  metadata.owner_uuid = Id(UuidKind::principal); metadata.audit_uuid = Id(UuidKind::object);
  metadata.owning_schema_uuid = metadata.record.header.parent_uuid;
  metadata.creator_transaction_uuid = created.transaction_uuid;
  metadata.creator_local_transaction_id = created.local_id.value;
  metadata.definition_version = metadata.schema_epoch = metadata.security_epoch = 1;
  metadata.catalog_generation = metadata.dependency_generation = metadata.invalidation_generation = 1;
  metadata.lifecycle = catalog::CatalogObjectLifecycle::active; metadata.status = catalog::CatalogObjectStatus::active;
  metadata.trace_search_key = "NATIVE-CATALOG-VERSION-TEST";
  metadata.object_subtype = "application"; metadata.retention_class = "catalog_history";
  constexpr char payload[] = "opaque-family-storage-oracle\0\r\n";
  metadata.record.payload.assign(payload, sizeof(payload) - 1);
  // Deliberately an opaque storage oracle, not a valid SQL object definition.
  // Family/name/dependency/audit admission is the owning catalog operation;
  // these checks prove native bytes/version/finality, not successful SQL DDL.
  const auto encoded = catalog::EncodeCatalogMetadataVersion(metadata);
  Check(encoded.ok(), "encode native metadata version");
  Check(encoded.bytes.size() == 384 + metadata.trace_search_key.size() + metadata.object_subtype.size() +
      metadata.retention_class.size() + 96 + metadata.record.payload.size(), "independent metadata extent");
  Check(std::equal(encoded.bytes.begin(), encoded.bytes.begin() + 8, "SBCMV001") &&
      platform::LoadLittle16(encoded.bytes.data() + 10) == 384 &&
      platform::LoadLittle64(encoded.bytes.data() + 88) == created.local_id.value &&
      std::equal(created.transaction_uuid.value.bytes.begin(), created.transaction_uuid.value.bytes.end(), encoded.bytes.begin() + 112),
      "independent metadata UUID/counter offsets");
  const auto decoded = catalog::DecodeCatalogMetadataVersion(encoded.bytes);
  Check(decoded.ok() && decoded.bytes == encoded.bytes && decoded.record.record.payload == metadata.record.payload &&
      decoded.record.owner_uuid.value == metadata.owner_uuid.value, "metadata roundtrip changed binary fields");
  for (std::size_t i = 0; i < encoded.bytes.size(); ++i) {
    auto changed = encoded.bytes; changed[i] ^= 1;
    const auto bad = catalog::DecodeCatalogMetadataVersion(changed);
    Check(!bad.ok() && bad.bytes.empty() && bad.record.record.header.row_uuid.value.is_nil(), "modified envelope admitted partial metadata");
  }
  // A valid outer checksum is not a substitute for semantic/canonical checks.
  for (std::size_t offset : {24u, 283u, 288u, 368u, 272u, 32u, 16u}) {
    auto changed = encoded.bytes; changed[offset] ^= 1;
    std::fill(changed.begin() + 320, changed.begin() + 352, 0);
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(changed);
    Check(digest.ok(), "reseal adversarial catalog envelope");
    std::copy(digest.digest.begin(), digest.digest.end(), changed.begin() + 320);
    const auto bad = catalog::DecodeCatalogMetadataVersion(changed);
    Check(!bad.ok() && bad.bytes.empty(), "resealed invalid metadata accepted");
  }
  auto missing = metadata; missing.creator_transaction_uuid = {};
  Check(!catalog::EncodeCatalogMetadataVersion(missing).ok(), "missing catalog creator accepted");
  missing = metadata; missing.definition_version = 0;
  Check(!catalog::EncodeCatalogMetadataVersion(missing).ok(), "missing definition version accepted");
  missing = metadata; missing.owning_schema_uuid.kind = UuidKind::object;
  Check(!catalog::EncodeCatalogMetadataVersion(missing).ok(), "mistyped owning schema accepted");
  const auto write = [&](const auto& tx, const auto& value, platform::Uuid expected = {}) {
    return db::WriteNativeCatalogVersionToOpenDevice(f.device, {f.relation, f.first_page, tx, value, expected});
  };
  const auto staged = write(created, metadata);
  Check(staged.ok() && staged.row_version.row_uuid.value == metadata.record.header.row_uuid.value &&
      !staged.row_version.version_uuid.is_nil(), "native catalog create omitted real row/version receipt");
  mga::VisibilitySnapshot own; own.reader_transaction = created.local_id;
  auto rows = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, f.first_page, own, false, created);
  Check(rows.ok() && rows.rows.size() == 1 && rows.rows[0].provisional &&
      rows.rows[0].effective_lifecycle == catalog::CatalogObjectLifecycle::creating, "own catalog proposal visibility");
  rows = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, f.first_page, {}, true);
  Check(rows.ok() && rows.rows.empty(), "uncommitted catalog metadata leaked");
  const auto contender = f.Begin();
  auto duplicate = metadata;
  duplicate.record.header.row_uuid = Id(UuidKind::row);
  duplicate.creator_transaction_uuid = contender.transaction_uuid;
  duplicate.creator_local_transaction_id = contender.local_id.value;
  const auto collision_before = f.Bytes(); const auto collision_writes = write_calls;
  Check(!write(contender, duplicate).ok() && f.Bytes() == collision_before && write_calls == collision_writes,
      "hidden active catalog object allowed a second authoritative row");
  f.Finish(contender, false);
  f.Finish(created, true);
  const auto reader = f.Begin();
  auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  const auto captured = mga::CreateLocalTransactionSnapshot(inventory.inventory, reader.local_id);
  Check(inventory.ok() && captured.ok(), "capture native catalog snapshot");
  const auto old_snapshot = mga::SnapshotPolicyForIsolation(mga::IsolationLevel::repeatable_read, captured.snapshot);
  const auto replacement_tx = f.Begin();
  auto replacement = metadata;
  replacement.creator_transaction_uuid = replacement_tx.transaction_uuid;
  replacement.creator_local_transaction_id = replacement_tx.local_id.value;
  replacement.definition_version = replacement.schema_epoch = replacement.catalog_generation = 2;
  replacement.dependency_generation = replacement.invalidation_generation = 2;
  replacement.trace_search_key = "NATIVE-CATALOG-VERSION-REPLACED";
  const auto replaced = write(replacement_tx, replacement, staged.row_version.version_uuid);
  Check(replaced.ok() && replaced.row_version.previous_version_uuid == staged.row_version.version_uuid,
      "catalog replacement lost native chain precondition");
  f.Finish(replacement_tx, true);
  rows = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, f.first_page, old_snapshot, false, reader);
  Check(rows.ok() && rows.rows.size() == 1 && rows.rows[0].metadata.definition_version == 1 &&
      rows.rows[0].metadata.trace_search_key == metadata.trace_search_key, "old snapshot exposed replaced metadata");
  const auto third_tx = f.Begin();
  auto third = replacement; third.creator_transaction_uuid = third_tx.transaction_uuid;
  third.creator_local_transaction_id = third_tx.local_id.value; third.definition_version = 3;
  const auto before = f.Bytes(); const auto writes_before = write_calls;
  Check(!write(third_tx, third, staged.row_version.version_uuid).ok() && f.Bytes() == before && write_calls == writes_before,
      "stale metadata base overwrote newer definition");
  const auto provisional = write(third_tx, third, replaced.row_version.version_uuid);
  Check(provisional.ok(), "stage rollback catalog version"); f.Finish(third_tx, false);
  rows = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, f.first_page, {}, true);
  Check(rows.ok() && rows.rows.size() == 1 && rows.rows[0].metadata.definition_version == 2 && !rows.rows[0].provisional,
      "rolled back catalog replacement became visible");
  const auto drop_tx = f.Begin();
  auto dropped = third; dropped.creator_transaction_uuid = dropped.retired_transaction_uuid = drop_tx.transaction_uuid;
  dropped.creator_local_transaction_id = drop_tx.local_id.value;
  dropped.record.header.deleted = true; dropped.lifecycle = catalog::CatalogObjectLifecycle::dropped;
  dropped.status = catalog::CatalogObjectStatus::retired;
  const auto retirement = write(drop_tx, dropped, replaced.row_version.version_uuid);
  Check(retirement.ok() && !retirement.row_version.deleted && !retirement.row_version.cells.empty(),
      "catalog retirement discarded metadata payload");
  f.Finish(drop_tx, true);
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen native catalog versions");
  rows = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, f.first_page, {}, true);
  Check(rows.ok() && rows.rows.size() == 1 && rows.rows[0].metadata.record.header.deleted &&
      rows.rows[0].metadata.retired_transaction_uuid.value == drop_tx.transaction_uuid.value &&
      rows.rows[0].metadata.audit_uuid.value == metadata.audit_uuid.value, "retirement identity/evidence lost after reopen");
  rows = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, f.first_page, old_snapshot, false, reader);
  Check(rows.ok() && rows.rows.size() == 1 && rows.rows[0].metadata.definition_version == 1,
      "retirement erased old metadata snapshot");
  f.Finish(reader, false);
  const auto expected_retirement = catalog::EncodeCatalogMetadataVersion(dropped);
  Check(expected_retirement.ok(), "encode independent retirement oracle");
  std::array<platform::byte, 24> oracle_header{};
  platform::StoreLittle64(oracle_header.data(), f.first_page);
  std::copy(f.relation.value.bytes.begin(), f.relation.value.bytes.end(), oracle_header.begin() + 8);
  std::ofstream oracle(f.path + ".catalog-oracle", std::ios::binary);
  oracle.write(reinterpret_cast<const char*>(oracle_header.data()), oracle_header.size());
  oracle.write(reinterpret_cast<const char*>(expected_retirement.bytes.data()), expected_retirement.bytes.size());
  oracle.close();
  Check(oracle.good() && f.device.Close().ok(), "release catalog node for fresh reader");
  const auto child = ::fork(); Check(child >= 0, "fork catalog reader");
  if (child == 0) { ::execl("/proc/self/exe", "catalog-probe", "--catalog-probe", f.path.c_str(), nullptr); ::_exit(99); }
  int status = 0; pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fresh process changed native catalog retirement");
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reclaim catalog node");

  for (unsigned fault = 0; fault != 6; ++fault) {
    const auto bad_tx = f.Begin();
    auto bad_metadata = metadata;
    bad_metadata.record.header.row_uuid = Id(UuidKind::row);
    bad_metadata.creator_transaction_uuid = bad_tx.transaction_uuid;
    bad_metadata.creator_local_transaction_id = bad_tx.local_id.value;
    if (fault == 0) bad_metadata.creator_transaction_uuid = created.transaction_uuid;
    if (fault == 1) bad_metadata.creator_local_transaction_id = created.local_id.value;
    const auto wire = catalog::EncodeCatalogMetadataVersion(bad_metadata);
    Check(wire.ok(), "encode malformed native binding fixture");
    auto native = f.Mutation(bad_tx, bad_metadata.record.header.row_uuid, f.first_page + 1 + fault, "");
    native.cells[0].value.type_id = types::CanonicalTypeId::binary;
    native.cells[0].value.payload = wire.bytes;
    if (fault == 2) native.row_uuid = Id(UuidKind::row);
    if (fault == 3) native.cells[0].column_ordinal = 2;
    if (fault == 4) {
      native.cells[0].value.type_id = types::CanonicalTypeId::character;
      native.cells[0].value.payload.assign({'n', 'o', 't', '-', 'c', 'a', 't', 'a', 'l', 'o', 'g'});
    }
    if (fault == 5) native.cells[0].value.payload.back() ^= 1;
    const auto staged_bad = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, native);
    Check(staged_bad.ok(), "stage adversarial native row " + std::to_string(fault) + ":" +
        staged_bad.diagnostic.diagnostic_code + ":" + staged_bad.diagnostic.message_key);
    // Even hidden active or rolled-back versions must not turn corruption into empty success.
    for (bool rolled_back : {false, true}) {
      if (rolled_back) f.Finish(bad_tx, false);
      const auto before_bad_read = f.Bytes(); const auto bad_writes = write_calls;
      const auto invalid = db::ReadNativeCatalogVersionsFromOpenDevice(f.device, f.relation, native.page_number, {}, true);
      Check(!invalid.ok() && invalid.rows.empty() && f.Bytes() == before_bad_read && write_calls == bad_writes,
          "hidden invalid catalog row published absence or mutated storage");
    }
  }
  Fixture conflicts;
  const auto pending = conflicts.Begin();
  auto reserved = metadata;
  reserved.creator_transaction_uuid = pending.transaction_uuid;
  reserved.creator_local_transaction_id = pending.local_id.value;
  const auto stage = [&](const auto& tx, const auto& value) {
    return db::WriteNativeCatalogVersionToOpenDevice(conflicts.device,
        {conflicts.relation, conflicts.first_page, tx, value, {}});
  };
  Check(stage(pending, reserved).ok(), "reserve second catalog object");
  const auto prepare_base = db::LoadLocalTransactionInventoryFromOpenDevice(&conflicts.device, page_size);
  Check(prepare_base.ok(), "load pending catalog inventory");
  const auto prepared = mga::PrepareLocalTransaction(prepare_base.inventory, pending.local_id);
  Check(prepared.ok() && db::PersistLocalTransactionInventoryToOpenDevice(
      &conflicts.device, page_size, prepared.inventory).ok(), "prepare catalog reservation");
  const auto second = conflicts.Begin();
  auto retry = reserved; retry.record.header.row_uuid = Id(UuidKind::row);
  retry.creator_transaction_uuid = second.transaction_uuid;
  retry.creator_local_transaction_id = second.local_id.value;
  const auto reserved_bytes = conflicts.Bytes(); const auto reserved_writes = write_calls;
  Check(!stage(second, retry).ok() && conflicts.Bytes() == reserved_bytes && write_calls == reserved_writes,
      "prepared hidden catalog reservation ignored");
  conflicts.Finish(pending, false);
  Check(stage(second, retry).ok(), "rolled-back creator permanently reserved object UUID");
  conflicts.Finish(second, true);
  const auto committed_rows = db::ReadNativeCatalogVersionsFromOpenDevice(conflicts.device,
      conflicts.relation, conflicts.first_page, {}, true);
  Check(committed_rows.ok() && committed_rows.rows.size() == 1 &&
      committed_rows.rows[0].metadata.record.header.row_uuid.value == retry.record.header.row_uuid.value,
      "rollback release did not publish the actual replacement row");

  Fixture combined;
  const auto batch_tx = combined.Begin();
  std::vector<db::NativeCatalogVersionMutation> mutations;
  for (unsigned i = 0; i != 3; ++i) {
    auto value = metadata;
    value.record.header.row_uuid = Id(UuidKind::row); value.record.header.object_uuid = Id(UuidKind::object);
    value.creator_transaction_uuid = batch_tx.transaction_uuid; value.creator_local_transaction_id = batch_tx.local_id.value;
    value.trace_search_key = "NATIVE-CATALOG-BATCH-" + std::to_string(i);
    mutations.push_back({combined.relation, combined.first_page + i / 2, batch_tx, value, {}});
  }
  for (unsigned fault = 0; fault != 4; ++fault) {
    auto invalid = mutations;
    if (fault == 0) invalid.back().metadata.definition_version = 0;
    if (fault == 1) invalid.back().metadata.record.header.row_uuid = invalid.front().metadata.record.header.row_uuid;
    if (fault == 2) invalid.back().metadata.record.header.object_uuid = invalid.front().metadata.record.header.object_uuid;
    if (fault == 3) invalid.back().transaction.local_id.value += 1;
    const auto before = combined.Bytes(); const auto before_writes = write_calls;
    const auto refused = db::WriteNativeCatalogVersionsToOpenDevice(combined.device, invalid);
    Check(!refused.ok() && refused.row_receipts.empty() && combined.Bytes() == before && write_calls == before_writes,
        "invalid catalog batch wrote earlier metadata or issued receipts");
  }
  const auto batch = db::WriteNativeCatalogVersionsToOpenDevice(combined.device, mutations);
  Check(batch.ok() && batch.row_receipts.size() == 3 && batch.written_rows == 3 && batch.pages_written == 2,
      "catalog batch omitted actual native receipts");
  for (std::size_t i = 0; i != mutations.size(); ++i) {
    const auto& receipt = batch.row_receipts[i]; const auto& request = mutations[i];
    Check(receipt.row_uuid.value == request.metadata.record.header.row_uuid.value &&
        receipt.relation_uuid.value == request.relation_uuid.value && receipt.page_number == request.page_number &&
        receipt.creator.transaction_uuid.value == batch_tx.transaction_uuid.value &&
        receipt.creator.local_id.value == batch_tx.local_id.value &&
        receipt.creator.scope == batch_tx.scope && !receipt.version_uuid.is_nil() &&
        receipt.previous_version_uuid.is_nil() && receipt.row_version == 1 && !receipt.deleted,
        "catalog receipt changed identity/order or invented a predecessor");
    disk::SerializedPageHeader header{};
    Check(combined.device.ReadAt(receipt.page_number * page_size, header.data(), header.size()).ok(), "read receipt page header");
    const auto physical = disk::ParsePageHeader(header);
    Check(physical.ok() && physical.header.database_uuid == receipt.database_uuid.value &&
        physical.header.filespace_uuid == receipt.filespace_uuid.value &&
        physical.header.page_uuid == receipt.page_uuid.value && physical.header.page_generation == receipt.page_generation,
        "receipt did not identify the actual written page image");
    const auto native = combined.Read(receipt.page_number, batch_tx);
    const auto actual = std::find_if(native.visible_rows.begin(), native.visible_rows.end(),
        [&](const auto& row) { return row.version_uuid == receipt.version_uuid; });
    Check(actual != native.visible_rows.end() && actual->stable_slot_id == receipt.stable_slot_id &&
        actual->row_version == receipt.row_version && actual->previous_version_uuid == receipt.previous_version_uuid,
        "receipt did not identify an actual staged row version");
    Check(combined.Read(receipt.page_number).visible_rows.empty(), "catalog batch leaked before commit");
  }
  combined.Finish(batch_tx, true);
  const auto replace_batch_tx = combined.Begin();
  for (std::size_t i = 0; i != mutations.size(); ++i) {
    auto& request = mutations[i]; request.transaction = replace_batch_tx;
    request.metadata.creator_transaction_uuid = replace_batch_tx.transaction_uuid;
    request.metadata.creator_local_transaction_id = replace_batch_tx.local_id.value;
    request.metadata.definition_version = 2;
    request.expected_version_uuid = batch.row_receipts[i].version_uuid;
  }
  {
    auto stale = mutations; stale.back().expected_version_uuid = batch.row_receipts.front().version_uuid;
    const auto before = combined.Bytes(); const auto before_writes = write_calls;
    const auto refused = db::WriteNativeCatalogVersionsToOpenDevice(combined.device, stale);
    Check(!refused.ok() && refused.row_receipts.empty() && before == combined.Bytes() && before_writes == write_calls,
        "stale later catalog precondition staged earlier successors");
  }
  const auto successors = db::WriteNativeCatalogVersionsToOpenDevice(combined.device, mutations);
  Check(successors.ok() && successors.row_receipts.size() == 3, "stage complete catalog successor set");
  for (std::size_t i = 0; i != mutations.size(); ++i)
    Check(successors.row_receipts[i].previous_version_uuid == batch.row_receipts[i].version_uuid &&
        successors.row_receipts[i].row_version == 2, "batch successor receipt lost native chain");
  combined.Finish(replace_batch_tx, false);
  const auto failed_tx = combined.Begin();
  for (auto& request : mutations) {
    request.transaction = failed_tx; request.metadata.creator_transaction_uuid = failed_tx.transaction_uuid;
    request.metadata.creator_local_transaction_id = failed_tx.local_id.value;
  }
  owned_row_offset = static_cast<off_t>((combined.first_page + 1) * page_size);
  owned_fault = OwnedFault::row_write;
  const auto failed_batch = db::WriteNativeCatalogVersionsToOpenDevice(combined.device, mutations);
  Check(!failed_batch.ok() && owned_fault == OwnedFault::none && failed_batch.row_receipts.empty() &&
      failed_batch.unresolved_mutation_transaction.transaction_uuid.value == failed_tx.transaction_uuid.value,
      "catalog batch failure lost native exclusion or issued partial receipts");
  db::PhysicalMgaCowFinalization commit_failed;
  commit_failed.transaction = failed_tx; commit_failed.decision = db::PhysicalMgaCowFinalizeDecision::commit;
  commit_failed.final_unix_epoch_millis = 1790000000300ull;
  Check(!db::FinalizePhysicalMgaCowTransactionToOpenDevice(combined.device, commit_failed).ok(), "partial catalog batch remained committable");
  combined.Finish(failed_tx, false);
  Check(combined.device.Close().ok() && combined.device.Open(combined.path, disk::FileOpenMode::open_existing).ok(),
      "reopen catalog batch rollback");
  unsigned retained_count = 0;
  for (unsigned page = 0; page != 2; ++page) {
    const auto visible = db::ReadNativeCatalogVersionsFromOpenDevice(combined.device, combined.relation,
        combined.first_page + page, {}, true);
    Check(visible.ok(), "read catalog batch committed baseline");
    for (const auto& row : visible.rows) {
      ++retained_count;
      Check(row.metadata.definition_version == 1 && !row.provisional, "failed catalog batch changed committed metadata");
    }
  }
  Check(retained_count == 3, "catalog batch rollback lost committed members");
}

int CrashNativeBatch(const char* path) {
  std::ifstream oracle(std::string(path) + ".batch-oracle", std::ios::binary);
  std::array<platform::byte, 48> bytes{};
  oracle.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!oracle) return 71;
  disk::FileDevice device;
  if (!device.Open(path, disk::FileOpenMode::open_existing).ok()) return 72;
  TypedUuid relation{UuidKind::object, {}};
  TypedUuid transaction{UuidKind::transaction, {}};
  std::copy(bytes.begin(), bytes.begin() + 16, relation.value.bytes.begin());
  std::copy(bytes.begin() + 16, bytes.begin() + 32, transaction.value.bytes.begin());
  const auto local_id = platform::LoadLittle64(bytes.data() + 32);
  const auto page = platform::LoadLittle64(bytes.data() + 40);
  db::PhysicalMgaCowMutationBatch batch;
  batch.sync_after_batch = false;
  for (unsigned i = 0; i != 2; ++i) {
    db::PhysicalMgaCowMutation row;
    row.relation_uuid = relation; row.row_uuid = Id(UuidKind::row);
    row.transaction_uuid = transaction; row.existing_local_transaction_id = mga::MakeLocalTransactionId(local_id);
    row.use_existing_transaction = true; row.page_number = page + i;
    types::DatatypeBinaryValue value; value.type_id = types::CanonicalTypeId::binary; value.payload = {0x42};
    row.cells.push_back({1, std::move(value)}); batch.mutations.push_back(std::move(row));
  }
  owned_row_offset = static_cast<off_t>(page * page_size);
  owned_fault = OwnedFault::batch_process_exit;
  (void)db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(device, std::move(batch));
  return 73;
}

void FailedNativeBatchCannotCommitPrefix() {
  {
    Fixture f;
    const auto tx = f.Begin();
    const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(inventory.ok(), "load pre-existing rollback-only owner");
    const auto marked = mga::MarkLocalTransactionRollbackOnly(inventory.inventory, tx.local_id);
    Check(marked.ok() && db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, marked.inventory).ok(),
        "persist pre-existing rollback-only marker");
    db::PhysicalMgaCowMutationBatch batch;
    batch.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page, "must-not-clear"));
    const auto before = f.Bytes(); const auto writes_before = write_calls;
    const auto refused = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch);
    Check(!refused.ok() && !refused.unresolved_mutation_transaction.valid() &&
        before == f.Bytes() && writes_before == write_calls, "batch cleared someone else's rollback-only marker");
    f.Finish(tx, false);
  }
  for (bool explicit_sync : {false, true}) for (auto fault : {OwnedFault::row_write, OwnedFault::row_sync,
       OwnedFault::after_row_write_exception, OwnedFault::batch_release_write}) {
  Fixture f;
  const auto tx = f.Begin();
  db::PhysicalMgaCowMutationBatch batch;
  batch.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page, "first-prefix"));
  batch.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page + 1, "second-missing"));
  batch.sync_after_batch = explicit_sync;
  owned_row_offset = static_cast<off_t>((f.first_page + 1) * page_size);
  owned_fault = fault;
  bool threw = false;
  db::PhysicalMgaCowMutationBatchResult failed;
  try { failed = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch); }
  catch (const std::bad_alloc&) { threw = true; }
  Check(owned_fault == OwnedFault::none && !reject_write && !reject_sync, "second-page batch failure not reached");
  if (fault == OwnedFault::after_row_write_exception) Check(threw, "batch exception swallowed");
  else Check(!threw && !failed.ok() && failed.written_rows == 0 && failed.pages_written == 0 && failed.row_receipts.empty() &&
      failed.unresolved_mutation_transaction.transaction_uuid.value == tx.transaction_uuid.value &&
      failed.unresolved_mutation_transaction.local_id.value == tx.local_id.value,
      "failed batch issued partial receipt or lost recovery identity");
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen after partial batch");
  const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(inventory.ok(), "read failed batch durable inventory");
  const auto blocked = mga::LookupLocalTransaction(inventory.inventory, tx.local_id);
  Check(blocked.ok() && blocked.entry.rollback_only, "failed batch lost durable commit fence");
  db::PhysicalMgaCowFinalization commit;
  commit.transaction = tx; commit.decision = db::PhysicalMgaCowFinalizeDecision::commit;
  commit.final_unix_epoch_millis = 1790000000300ull;
  const auto outcome = db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, commit);
  Check(!outcome.ok(), "failed native batch committed its successful prefix");
  Check(f.Read(f.first_page).visible_rows.empty(), "failed batch prefix became visible");
  f.Finish(tx, false);
  }
  for (bool explicit_sync : {false, true}) {
    Fixture f;
    const auto tx = f.Begin();
    db::PhysicalMgaCowMutationBatch batch;
    batch.sync_after_batch = explicit_sync;
    batch.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page, "complete-first"));
    batch.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page + 1, "complete-second"));
    if (explicit_sync) {
      batch.mutations[0].stable_slot_id = 1; batch.mutations[1].stable_slot_id = 2;
    }
    const auto written = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch);
    Check(written.ok() && written.written_rows == 2 && written.pages_written == 2 &&
        !written.unresolved_mutation_transaction.valid(), "complete batch lacked success receipt");
    const auto fast_path_evidence = std::string("physical_mga_cow.empty_page_insert_fast_path=") + (explicit_sync ? "true" : "false");
    Check(std::find(written.evidence.begin(), written.evidence.end(), fast_path_evidence) != written.evidence.end(),
        "batch reported a fast path that was not used");
    Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen successful batch");
    const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(inventory.ok(), "load completed batch inventory");
    const auto active = mga::LookupLocalTransaction(inventory.inventory, tx.local_id);
    Check(active.ok() && !active.entry.rollback_only && active.entry.state == mga::TransactionState::active,
        "successful batch changed finality or retained commit fence");
    f.Finish(tx, true);
    Payload(f.Read(f.first_page), "complete-first"); Payload(f.Read(f.first_page + 1), "complete-second");
    const auto update_tx = f.Begin();
    for (auto& mutation : batch.mutations) {
      mutation.transaction_uuid = update_tx.transaction_uuid;
      mutation.existing_local_transaction_id = update_tx.local_id;
      mutation.kind = db::PhysicalMgaCowMutationKind::update;
    }
    batch.mutations.front().kind = db::PhysicalMgaCowMutationKind::delete_row;
    batch.mutations.front().cells.clear();
    const auto changed = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, batch);
    Check(changed.ok() && changed.row_receipts.size() == 2, "mixed update/delete receipt set missing");
    for (std::size_t i = 0; i != changed.row_receipts.size(); ++i) {
      const auto& receipt = changed.row_receipts[i];
      Check(receipt.deleted == (i == 0) && receipt.previous_version_uuid == written.row_receipts[i].version_uuid &&
          receipt.row_version == 2 && receipt.stable_slot_id == written.row_receipts[i].stable_slot_id,
          "mixed mutation receipt lost deletion/slot/version identity");
      const auto actual_page = f.Read(receipt.page_number, update_tx);
      const auto row = std::find_if(actual_page.row_page.rows.begin(), actual_page.row_page.rows.end(),
          [&](const auto& item) { return item.version_uuid == receipt.version_uuid; });
      Check(row != actual_page.row_page.rows.end() && row->deleted == receipt.deleted &&
          row->previous_version_uuid == receipt.previous_version_uuid,
          "mixed mutation receipt does not reference actual native version");
    }
    f.Finish(update_tx, false);
  }
  {
    Fixture f;
    const auto tx = f.Begin();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
        f.Mutation(tx, Id(UuidKind::row), f.first_page, "prior-statement")).ok(), "stage prior statement");
    db::PhysicalMgaCowMutationBatch invalid;
    invalid.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page + 1, "must-not-write"));
    invalid.mutations.push_back(f.Mutation(tx, Id(UuidKind::row), f.first_page + 2, "bad-text"));
    invalid.mutations.back().cells[0].value.payload = {0xff};
    const auto before = f.Bytes(); const auto writes_before = write_calls;
    const auto refused = db::WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(f.device, invalid);
    Check(!refused.ok() && !refused.unresolved_mutation_transaction.valid() &&
        write_calls == writes_before && before == f.Bytes(), "invalid later page wrote an earlier page or transaction fence");
    f.Finish(tx, true);
    Payload(f.Read(f.first_page), "prior-statement");
    Check(f.Read(f.first_page + 1).visible_rows.empty(), "preflight failure published partial batch");
  }
  {
    Fixture f;
    const auto tx = f.Begin();
    std::array<platform::byte, 48> bytes{};
    std::copy(f.relation.value.bytes.begin(), f.relation.value.bytes.end(), bytes.begin());
    std::copy(tx.transaction_uuid.value.bytes.begin(), tx.transaction_uuid.value.bytes.end(), bytes.begin() + 16);
    platform::StoreLittle64(bytes.data() + 32, tx.local_id.value);
    platform::StoreLittle64(bytes.data() + 40, f.first_page);
    std::ofstream oracle(f.path + ".batch-oracle", std::ios::binary);
    oracle.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); oracle.close();
    Check(oracle.good() && f.device.Close().ok(), "release batch node for crash process");
    const auto child = ::fork(); Check(child >= 0, "fork batch crash process");
    if (child == 0) { ::execl("/proc/self/exe", "batch-crash", "--batch-crash", f.path.c_str(), nullptr); ::_exit(99); }
    int status = 0; pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 74, "batch crash point not reached");
    Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "recover node after batch process loss");
    db::PhysicalMgaCowFinalization commit;
    commit.transaction = tx; commit.decision = db::PhysicalMgaCowFinalizeDecision::commit;
    commit.final_unix_epoch_millis = 1790000000300ull;
    const auto result = db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, commit);
    Check(!result.ok() && result.diagnostic.diagnostic_code == "SB-MGA-ROLLBACK-ONLY-COMMIT-REFUSED",
        "fresh process committed interrupted native batch prefix");
    Check(f.Read(f.first_page).visible_rows.empty(), "crashed batch became independently visible");
    f.Finish(tx, false);
  }
}

void NativeInventoryPublicationConcurrency() {
  Fixture f;
  auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(loaded.ok() && loaded.inventory.publication_base.has_value(), "native load omitted publication base");
  const auto equal_state = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, loaded.inventory);
  Check(equal_state.ok() && equal_state.inventory.publication_base != loaded.inventory.publication_base,
      "equal-state publication reused previous publication authority");
  Check(equal_state.inventory.publication_base->generation == loaded.inventory.publication_base->generation + 1,
      "equal-state publication did not advance exactly one generation");
  const auto stale_equal = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, loaded.inventory);
  Check(!stale_equal.ok() && stale_equal.diagnostic.diagnostic_code == "SB-TXN-INVENTORY-SNAPSHOT-STALE",
      "equal-state publication left old writer authorized");
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  const auto interrupted_base = loaded.inventory.publication_base;
  const auto failures_before = write_faults;
  reject_write = true;
  const auto interrupted = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, loaded.inventory);
  Check(!interrupted.ok() && !reject_write && write_faults == failures_before + 1,
      "native inventory page-write fault did not fire after publishing carrier");
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(),
      "reopen actual interrupted inventory publication");
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(loaded.ok() && loaded.inventory.publication_base == interrupted_base,
      "actual failed attempt did not recover exact prior publication base");
  const auto retry = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, loaded.inventory);
  Check(retry.ok() && retry.inventory.publication_base->generation == interrupted_base->generation + 2,
      "native retry reused failed attempt generation");
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  const auto begin_a = mga::BeginLocalTransaction(loaded.inventory, Id(UuidKind::transaction), 1790000000200ull);
  const auto begin_b = mga::BeginLocalTransaction(loaded.inventory, Id(UuidKind::transaction), 1790000000200ull);
  Check(begin_a.ok() && begin_b.ok(), "derive competing begins");
  const auto a = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, begin_a.inventory);
  Check(a.ok() && a.inventory.publication_base != loaded.inventory.publication_base,
      "successful publication did not renew base");
  const auto journal_bytes = [&] {
    std::ifstream in(f.path + ".sb.txn_publish", std::ios::binary);
    Check(static_cast<bool>(in), "open publication image");
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  const auto reject = [&](mga::LocalTransactionInventory replacement) {
    const auto before = f.Bytes(); const auto journal = journal_bytes();
    const auto writes_before = write_calls, syncs_before = sync_calls;
    const auto result = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, std::move(replacement));
    Check(!result.ok() && result.inventory.entries.empty() && !result.inventory.publication_base,
        "stale publication fabricated inventory receipt");
    Check(result.diagnostic.diagnostic_code == "SB-TXN-INVENTORY-SNAPSHOT-STALE",
        "stale publication diagnostic");
    Check(f.Bytes() == before && journal_bytes() == journal && write_calls == writes_before && sync_calls == syncs_before,
        "rejected publication changed node or journal");
  };
  reject(begin_b.inventory);
  auto missing = a.inventory; missing.publication_base.reset(); reject(missing);
  auto different_node = a.inventory; different_node.publication_base->database_uuid = Id(UuidKind::database).value;
  reject(different_node);
  auto altered_digest = a.inventory; altered_digest.publication_base->inventory_sha256[0] ^= 1; reject(altered_digest);
  // A pristine-looking replacement cannot reset an already initialized node.
  reject(mga::MakeEmptyLocalTransactionInventory());
  auto regressed = a.inventory; regressed.entries.clear(); regressed.next_commit_sequence = 1;
  const auto before_regression = f.Bytes(); const auto journal_before_regression = journal_bytes();
  const auto regression = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, regressed);
  Check(!regression.ok() && regression.diagnostic.diagnostic_code == "CATALOG.INVALID_INPUT" &&
      f.Bytes() == before_regression && journal_bytes() == journal_before_regression,
      "matching base admitted counter regression");
  const auto next = mga::BeginLocalTransaction(a.inventory, Id(UuidKind::transaction), 1790000000210ull);
  const auto next_publish = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, next.inventory);
  Check(next.ok() && next_publish.ok(), "returned publication base cannot continue mutation");

  // Exact two-writer history: both replacements derive from the same actual
  // native load; only one can publish, even though both are structurally valid.
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  std::array<mga::LocalTransactionInventory, 2> replacements;
  for (auto& replacement : replacements) {
    const auto begun = mga::BeginLocalTransaction(loaded.inventory, Id(UuidKind::transaction), 1790000000220ull);
    Check(begun.ok(), "begin simultaneous publication"); replacement = begun.inventory;
  }
  std::array<db::LocalTransactionStoreResult, 2> outcomes;
  std::barrier start(3);
  std::array<std::thread, 2> writers;
  for (std::size_t i = 0; i < writers.size(); ++i)
    writers[i] = std::thread([&, i] {
      start.arrive_and_wait();
      outcomes[i] = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, replacements[i]);
    });
  start.arrive_and_wait(); for (auto& writer : writers) writer.join();
  Check(outcomes[0].ok() != outcomes[1].ok(), "competing stale copies both published or neither published");
  const auto winner = outcomes[0].ok() ? 0u : 1u;
  Check(outcomes[1 - winner].diagnostic.diagnostic_code == "SB-TXN-INVENTORY-SNAPSHOT-STALE",
      "competing writer did not get stale-base refusal");
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(loaded.ok() && loaded.inventory.entries.back().identity.transaction_uuid.value ==
      replacements[winner].entries.back().identity.transaction_uuid.value, "publication winner lost after native reload");

  // Full native finalizers hold the compound guard across load/transition/
  // publish, so independent valid transactions all commit, without stale retry
  // races or duplicate sequence allocation.
  constexpr std::size_t count = 6;
  std::array<mga::TransactionIdentity, count> transactions;
  for (auto& tx : transactions) {
    tx = f.Begin();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
        f.Mutation(tx, Id(UuidKind::row), f.first_page, "concurrent-finality")).ok(), "stage concurrent finality row");
  }
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(loaded.ok(), "load expected initial commit order");
  const auto first_sequence = loaded.inventory.next_commit_sequence;
  const auto first_generation = loaded.inventory.publication_base->generation;
  const auto next_transaction = loaded.inventory.next_local_transaction_id;
  std::array<db::PhysicalMgaCowFinalizeResult, count> finalized;
  std::vector<std::thread> finalizers;
  std::barrier commit_start(static_cast<std::ptrdiff_t>(count + 1));
  for (std::size_t i = 0; i < count; ++i)
    finalizers.emplace_back([&, i] {
      commit_start.arrive_and_wait();
      finalized[i] = db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device,
          {transactions[i], db::PhysicalMgaCowFinalizeDecision::commit, 1790000000300ull});
    });
  commit_start.arrive_and_wait(); for (auto& finalizer : finalizers) finalizer.join();
  std::array<bool, count> sequences{};
  for (const auto& final : finalized) {
    Check(final.ok() && final.transaction_entry.commit_sequence >= first_sequence &&
        final.transaction_entry.commit_sequence < first_sequence + count, "concurrent native finalizer failed or sequence escaped");
    const auto index = final.transaction_entry.commit_sequence - first_sequence;
    Check(!sequences[index], "concurrent finalizers reused commit order"); sequences[index] = true;
    Check(final.inventory.publication_base.has_value(), "finalizer dropped refreshed publication base");
  }
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen concurrent finality");
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(loaded.ok() && loaded.inventory.next_commit_sequence == first_sequence + count &&
      f.Read(f.first_page).visible_rows.size() == count, "concurrent finality not durable after reopen");
  Check(loaded.inventory.next_local_transaction_id == next_transaction &&
      loaded.inventory.publication_base->generation == first_generation + count,
      "commits without transaction allocation did not each publish a fresh generation");

  // A different node has independent compound-operation ownership.
  Fixture other;
  auto held = f.device.AcquireOperationGuard();
  std::atomic<bool> other_loaded{false};
  std::thread independent([&] {
    other_loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&other.device, page_size).ok();
  });
  independent.join();
  Check(other_loaded.load(), "separate node shared compound-operation guard");
}

void ArchiveAndRecoveryCommitOrder() {
  for (bool commit : {false, true}) {
    Fixture f;
    const auto tx = f.Begin();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
        f.Mutation(tx, Id(UuidKind::row), f.first_page, "archive-outcome")).ok(), "write archive outcome row");
    f.Finish(tx, commit);
    const auto prior = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(prior.ok(), "load prearchive inventory");
    const auto archived = mga::ArchiveLocalTransaction(prior.inventory, tx.local_id);
    Check(archived.ok() && archived.entry.archived_from_state ==
        (commit ? mga::TransactionState::committed : mga::TransactionState::rolled_back), "archive lost exact terminal origin");
    Check(archived.inventory.next_commit_sequence == prior.inventory.next_commit_sequence &&
        (archived.entry.commit_sequence != 0) == commit, "archive changed finality or commit order");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, archived.inventory).ok(), "persist archive origin");
    Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen archive outcome");
    const auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(loaded.ok() && loaded.inventory.entries.back().archived_from_state == archived.entry.archived_from_state,
        "native archive origin lost after reopen");
    const auto rows = f.Read(f.first_page);
    if (commit) Payload(rows, "archive-outcome");
    else Check(rows.visible_rows.empty() && rows.rolled_back_version_count == 1, "archived rollback resurrected a row");
    mga::TransactionInventoryCompactionRequest request;
    request.inventory = loaded.inventory; request.inventory_authoritative = true;
    request.oldest_required_local_transaction_id = mga::MakeLocalTransactionId(loaded.inventory.next_local_transaction_id);
    const auto compacted = mga::CompactLocalTransactionInventory(request);
    Check(compacted.ok() && compacted.compacted_entry_count == 1 &&
        compacted.inventory.entries.size() + 1 == loaded.inventory.entries.size() &&
        !mga::LookupLocalTransaction(compacted.inventory, tx.local_id).ok() &&
        compacted.inventory.next_commit_sequence == loaded.inventory.next_commit_sequence, "compaction reset commit order");
    Check(compacted.inventory.publication_base == loaded.inventory.publication_base,
        "compaction dropped exact native publication base");
    const auto next = mga::BeginLocalTransaction(compacted.inventory, Id(UuidKind::transaction), 1790000000500ull);
    Check(next.ok() && next.entry.begin_visible_through_commit_sequence == loaded.inventory.next_commit_sequence - 1,
        "compacted begin guessed commit order from remaining rows");
    const auto finalized = mga::CommitLocalTransaction(next.inventory, next.entry.identity.local_id, 1790000000600ull);
    Check(finalized.ok() && finalized.entry.commit_sequence == loaded.inventory.next_commit_sequence, "compacted commit reused old order");
  }
  // Pure recovery-kernel cases model admitted durable states. They do not
  // establish operator/evidence authentication or cluster-provider authority.
  auto begun = mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(),
      Id(UuidKind::transaction), 1790000000100ull);
  Check(begun.ok(), "begin recovery counter test");
  auto input = begun.inventory;
  input.entries[0].state = mga::TransactionState::committing;
  input.entries[0].evidence_record_written = true;
  const auto recovered = mga::ApplyLocalTransactionInventoryRecovery(input, 1790000000200ull);
  Check(recovered.ok() && recovered.recovered_inventory.entries[0].commit_sequence == 1 &&
      recovered.recovered_inventory.next_commit_sequence == 2, "recovered commit omitted sequence");
  const auto replay = mga::ApplyLocalTransactionInventoryRecovery(recovered.recovered_inventory, 1790000000300ull);
  Check(replay.ok() && !replay.inventory_changed && replay.recovered_inventory.next_commit_sequence == 2,
      "recovery replay consumed another sequence");
  input.next_commit_sequence = std::numeric_limits<u64>::max();
  const auto exhausted = mga::ApplyLocalTransactionInventoryRecovery(input, 1790000000200ull);
  Check(!exhausted.ok() && !exhausted.inventory_changed && exhausted.recovered_inventory.entries[0].commit_sequence == 0,
      "recovery overflow partially committed inventory");
  auto ordinary = begun.inventory; ordinary.next_commit_sequence = std::numeric_limits<u64>::max();
  Check(!mga::CommitLocalTransaction(ordinary, begun.entry.identity.local_id, 1790000000200ull).ok(), "ordinary commit sequence wrapped");
  for (auto decision : {mga::LimboOperatorDecision::commit, mga::LimboOperatorDecision::rollback,
                        mga::LimboOperatorDecision::fail_terminal}) {
    auto limbo = begun.inventory; limbo.entries[0].state = mga::TransactionState::limbo;
    mga::LimboOperatorResolutionPolicy policy;
    policy.operator_decision_authoritative = true; policy.operator_evidence_reference = "component-admitted-operator-decision";
    const auto resolved = mga::ResolveLimboLocalTransactionWithOperatorDecision(limbo, begun.entry.identity.local_id,
        decision, 1790000000200ull, policy);
    const bool commit = decision == mga::LimboOperatorDecision::commit;
    Check(resolved.ok() && resolved.entry.commit_sequence == (commit ? 1u : 0u) &&
        resolved.inventory.next_commit_sequence == (commit ? 2u : 1u), "operator resolution commit order differs from outcome");
    const auto archived = mga::ArchiveLocalTransaction(resolved.inventory, begun.entry.identity.local_id);
    Check(archived.ok() && archived.entry.archived_from_state == resolved.entry.state &&
        mga::HasCommittedInventoryOutcome(archived.entry) == commit &&
        *mga::ValidateLocalTransactionInventoryStructure(archived.inventory) == '\0', "archived operator outcome changed finality");
  }
}

void FinalizationIdentityAndOwnership() {
  Fixture f;
  const auto committed = f.Begin(), rolled_back = f.Begin();
  const auto row = f.Mutation(committed, Id(UuidKind::row), f.first_page, "finalization-identity");
  Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, row).ok(), "write finality row");
  db::PhysicalMgaCowFinalization request{committed, db::PhysicalMgaCowFinalizeDecision::commit, 1790000000300ull};
  const auto before = f.Bytes();
  const auto writes_before = write_calls, syncs_before = sync_calls;
  const auto no_receipt = [&](const db::PhysicalMgaCowFinalizeResult& result) {
    Check(!result.ok() && result.evidence.empty() && result.inventory.entries.empty() &&
          !result.transaction_entry.identity.valid(), "failed finalization fabricated finality receipt");
  };
  const auto reject = [&](const db::PhysicalMgaCowFinalization& bad) {
    const auto result = db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, bad);
    no_receipt(result);
    Check(result.diagnostic.diagnostic_code == "CATALOG.INVALID_INPUT", "invalid finalization diagnostic");
    Check(f.Bytes() == before && write_calls == writes_before && sync_calls == syncs_before,
          "invalid finalization changed native storage");
    Check(f.device.is_open(), "invalid finalization released node ownership");
  };
  reject({});
  auto bad = request; bad.transaction.local_id = {}; reject(bad);
  bad = request; bad.transaction.local_id.value = std::numeric_limits<u64>::max(); reject(bad);
  bad = request; bad.transaction.transaction_uuid = {}; reject(bad);
  bad = request; bad.transaction.transaction_uuid = Id(UuidKind::transaction); reject(bad);
  bad = request; bad.transaction.local_id = rolled_back.local_id; reject(bad);
  bad = request; bad.transaction.transaction_uuid = rolled_back.transaction_uuid; reject(bad);
  bad = request; bad.transaction.transaction_uuid.kind = UuidKind::object; reject(bad);
  for (unsigned version = 0; version < 16; ++version) if (version != 7) {
    bad = request; bad.transaction.transaction_uuid.value.bytes[6] = static_cast<platform::byte>(version << 4); reject(bad);
  }
  for (const unsigned variant : {0u, 0x40u, 0xc0u}) {
    bad = request; bad.transaction.transaction_uuid.value.bytes[8] = static_cast<platform::byte>(variant); reject(bad);
  }
  for (const auto scope : {mga::TransactionScope::unknown, mga::TransactionScope::cluster_global,
                          static_cast<mga::TransactionScope>(65535)}) {
    bad = request; bad.transaction.scope = scope; reject(bad);
  }
  for (const unsigned decision : {2u, 3u, 65535u}) {
    bad = request; bad.decision = static_cast<db::PhysicalMgaCowFinalizeDecision>(decision); reject(bad);
  }
  bad = request; bad.final_unix_epoch_millis = 0; reject(bad);
  db::PhysicalMgaCowFinalizeRequest path_request;
  static_cast<db::PhysicalMgaCowFinalization&>(path_request) = request;
  path_request.database_path = f.path;
  const auto blocked_path = db::FinalizePhysicalMgaCowTransaction(path_request);
  no_receipt(blocked_path);
  Check(OwnershipError(blocked_path.diagnostic), "path finalizer bypassed retained node owner");
  f.Locked();
  f.Finish(committed, true); f.Finish(rolled_back, false);
  Payload(f.Read(f.first_page), "finalization-identity");
  const auto terminal_bytes = f.Bytes();
  no_receipt(db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, request));
  request.decision = db::PhysicalMgaCowFinalizeDecision::rollback;
  no_receipt(db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, request));
  Check(f.Bytes() == terminal_bytes, "terminal replay rewrote finality");

  const auto read_only = f.Begin();
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing_read_only).ok(),
        "open retained read-only node");
  request = {read_only, db::PhysicalMgaCowFinalizeDecision::commit, 1790000000300ull};
  const auto read_only_bytes = f.Bytes();
  const auto read_only_result = db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, request);
  no_receipt(read_only_result);
  Check(read_only_result.diagnostic.diagnostic_code == "STORAGE.READ_ONLY_DEVICE" && f.Bytes() == read_only_bytes,
        "read-only finalization changed storage or lost diagnostic");
  f.Locked();
  Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "restore writable node");
  f.Finish(read_only, false);

  for (const bool fail_sync : {false, true}) {
    const auto tx = f.Begin();
    request = {tx, db::PhysicalMgaCowFinalizeDecision::commit, 1790000000300ull};
    const auto failures_before = fail_sync ? sync_faults : write_faults;
    if (fail_sync) reject_sync = true; else reject_write = true;
    const auto failed = db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, request);
    no_receipt(failed);
    Check(!failed.diagnostic.diagnostic_code.empty() &&
          (fail_sync ? sync_faults : write_faults) == failures_before + 1 && !reject_write && !reject_sync,
          "finalization did not exercise actual inventory write/sync failure");
    f.Locked();
    // A failed persistence call does not prove absence of durable finality.
    // Reload through real recovery before deciding how to resolve the owner.
    const auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(loaded.ok(), "reload inventory after finalization IO failure");
    const auto current = mga::LookupLocalTransaction(loaded.inventory, tx.local_id);
    Check(current.ok() && current.entry.identity.transaction_uuid.value == tx.transaction_uuid.value,
          "IO failure lost exact transaction owner");
    Check(current.entry.state == mga::TransactionState::active || current.entry.state == mga::TransactionState::committed,
          "IO failure left an unexplained transaction state");
    if (current.entry.state == mga::TransactionState::active) f.Finish(tx, false);
  }
  WriteFinalityOracle(f.path, {{committed, mga::TransactionState::committed},
      {rolled_back, mga::TransactionState::rolled_back}, {read_only, mga::TransactionState::rolled_back}});
  Check(f.device.Close().ok(), "close finalized owner before independent reopen");
  const auto child = ::fork(); Check(child >= 0, "fork independent finality reader");
  if (child == 0) { ::execl("/proc/self/exe", "finality-probe", "--finality-probe", f.path.c_str(), nullptr); ::_exit(99); }
  int status = 0; pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "independent process failed exact native finality oracle");
  // The path adapter delegates the same checked owner after exclusion ends.
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen to begin path finality");
  const auto path_tx = f.Begin(); Check(f.device.Close().ok(), "release path fixture owner");
  static_cast<db::PhysicalMgaCowFinalization&>(path_request) =
      {path_tx, db::PhysicalMgaCowFinalizeDecision::rollback, 1790000000300ull};
  Check(db::FinalizePhysicalMgaCowTransaction(path_request).ok(), "path adapter did not perform real finalization");
  no_receipt(db::FinalizePhysicalMgaCowTransactionToOpenDevice(f.device, request));
}
void OwnedMutationFailureFinality() {
  for (auto fault : {OwnedFault::row_write, OwnedFault::row_sync,
                    OwnedFault::after_row_write_exception, OwnedFault::rollback_write,
                    OwnedFault::exception_rollback_write}) {
    const bool rollback_fails = fault == OwnedFault::rollback_write || fault == OwnedFault::exception_rollback_write;
    Fixture f;
    const auto row = Id(UuidKind::row);
    const auto initial = f.Begin();
    Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(
        f.device, f.Mutation(initial, row, f.first_page, "before")).ok(), "initial failure fixture row");
    f.Finish(initial, true);
    const auto inventory = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(inventory.ok(), "inventory before owned mutation failure");
    auto mutation = f.Mutation({}, row, f.first_page, "after");
    mutation.transaction_uuid = Id(UuidKind::transaction);
    mutation.kind = db::PhysicalMgaCowMutationKind::update;
    mutation.use_existing_transaction = false;
    mutation.begin_unix_epoch_millis = 1790000000200ull;
    owned_row_offset = static_cast<off_t>(f.first_page * page_size);
    owned_fault = fault;
    const auto previous_faults = owned_faults;
    bool allocation_thrown = false;
    db::PhysicalMgaCowMutationResult failed;
    try {
      failed = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device, mutation);
    } catch (const std::bad_alloc&) { allocation_thrown = true; }
    Check(owned_fault == OwnedFault::none && owned_faults == previous_faults + 1,
          "owned mutation did not reach selected actual page IO boundary");
    Check(!reject_write && !reject_sync, "owned mutation left a fault unexercised");
    Check(allocation_thrown == (fault == OwnedFault::after_row_write_exception),
          "native mutation lost the injected allocation exception");
    if (!allocation_thrown) {
      Check(!failed.ok() && failed.row_page.rows.empty() && failed.page_uuid.value.is_nil(),
            "IO failure fabricated a page publication result");
      const bool rolled = std::find(failed.evidence.begin(), failed.evidence.end(),
          "physical_mga_cow.failed_owned_transaction_rolled_back=true") != failed.evidence.end();
      Check(rolled == !rollback_fails, "rollback receipt disagrees with actual IO");
      if (rollback_fails) {
        Check(failed.unresolved_owned_transaction.transaction_uuid.value == mutation.transaction_uuid.value &&
              failed.unresolved_owned_transaction.local_id.value == inventory.inventory.next_local_transaction_id,
              "failed rollback stranded a transaction without its exact recovery identity");
        Check(std::any_of(failed.diagnostic.arguments.begin(), failed.diagnostic.arguments.end(),
            [&](const auto& argument) { return fault == OwnedFault::exception_rollback_write
                ? argument.key == "mutation_exception" && argument.value == "propagation_interrupted_by_rollback_failure"
                : argument.key == "mutation_failure_code" && !argument.value.empty(); }),
            "rollback failure lost original mutation diagnostic");
      } else {
        Check(!failed.unresolved_owned_transaction.local_id.valid(),
              "confirmed rollback reported an unresolved transaction");
      }
    }
    auto actual = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
    Check(actual.ok(), "reload durable inventory after owned mutation failure");
    const auto entry = mga::LookupLocalTransaction(actual.inventory,
        mga::MakeLocalTransactionId(inventory.inventory.next_local_transaction_id));
    Check(entry.ok() && entry.entry.identity.transaction_uuid.value == mutation.transaction_uuid.value,
          "owned failure reassigned transaction number or UUID");
    Check(entry.entry.state == (rollback_fails ? mga::TransactionState::active : mga::TransactionState::rolled_back),
          "owned failure has incorrect durable finality");
    Payload(f.Read(f.first_page), "before");
    f.Locked();
    if (rollback_fails) f.Finish(entry.entry.identity, false);
    Check(f.device.Close().ok() && f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(),
          "reopen after owned mutation IO failure");
    Payload(f.Read(f.first_page), "before");
  }
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
void NativeInventoryPageBindings() {
  namespace page=scratchbird::storage::page;
  Fixture f; const auto owner=f.Begin(); f.Begin();
  const auto state=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
  Check(state.ok()&&state.inventory.entries.size()>=2,"real inventory for page binding fixture");
  const auto data_page=f.first_page+5;
  Check(db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(f.device,
      f.Mutation(owner,Id(UuidKind::row),data_page,"owning-filespace-row")).ok(),"actual row for caller filespace binding");
  disk::SerializedPageHeader data_header_bytes{};
  Check(f.device.ReadAt(data_page*page_size,data_header_bytes.data(),data_header_bytes.size()).ok(),"actual row page identity");
  const auto data_header=disk::ParsePageHeader(data_header_bytes);Check(data_header.ok(),"decode actual row page identity");
  const auto journal=std::filesystem::path(f.path+".sb.txn_publish");
  const auto retained=std::filesystem::path(f.path+".saved_publication");
  std::filesystem::rename(journal,retained);
  disk::SerializedPageHeader root_bytes{};
  Check(f.device.ReadAt(db::kTransactionInventoryPageNumber*page_size,root_bytes.data(),root_bytes.size()).ok(),"actual root header");
  const auto root=disk::ParsePageHeader(root_bytes);Check(root.ok(),"decode actual root identity");
  const u64 continuation=f.first_page;
  auto overflow=root.header;overflow.page_number=continuation;overflow.page_uuid=Id(UuidKind::page).value;
  const auto overflow_bytes=disk::SerializePageHeader(overflow);Check(overflow_bytes.ok(),"independent continuation header");
  std::array<page::TransactionInventoryPageBody,2> bodies;
  for(unsigned i=0;i<2;++i) {
    page::TransactionInventoryPageBody body;
    body.page_number=i?continuation:db::kTransactionInventoryPageNumber;
    body.previous_page_number=i?db::kTransactionInventoryPageNumber:0;
    body.next_page_number=i?0:continuation;body.inventory_generation=7;
    body.inventory.next_local_transaction_id=state.inventory.next_local_transaction_id;
    body.inventory.next_commit_sequence=state.inventory.next_commit_sequence;
    if(i) body.inventory.entries.assign(state.inventory.entries.begin()+1,state.inventory.entries.end());
    else body.inventory.entries.push_back(state.inventory.entries.front());
    if(!i) body.horizons=state.horizons;
    bodies[i]=body;
    const auto encoded=page::BuildTransactionInventoryPageBody(body,page_size);Check(encoded.ok(),"real two-page native inventory body");
    const auto& header=i?overflow_bytes.serialized:root_bytes;
    Check(f.device.WriteAt(body.page_number*page_size,header.data(),header.size()).ok()
      &&f.device.WriteAt(body.page_number*page_size+128,encoded.serialized.data(),encoded.serialized.size()).ok(),"persist native inventory chain");
  }
  Check(f.device.Sync().ok(),"sync inventory chain");
  auto loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
  Check(loaded.ok()&&loaded.inventory.entries.size()==state.inventory.entries.size(),"valid real chain accepted without journal");
  Check(loaded.inventory.publication_base && loaded.inventory.publication_base->generation == 7,
      "page-only authority lost actual body publication generation");
  mga::VisibilitySnapshot own_snapshot;own_snapshot.reader_transaction=owner.local_id;
  const auto read_own=[&] {return db::ReadPhysicalMgaCowRowsFromOpenDevice(f.device,f.relation,data_page,own_snapshot,false,owner);};
  for(unsigned mode=0;mode<7;++mode) {
    auto bad=mode>=5?root.header:overflow;
    if(mode==0||mode==5) bad.database_uuid=Id(UuidKind::database).value;
    if(mode==1||mode==6) bad.filespace_uuid=Id(UuidKind::filespace).value;
    if(mode==2) ++bad.page_number;
    if(mode==3) bad.page_uuid=root.header.page_uuid;
    if(mode==4) bad.page_generation=0;
    const auto encoded=disk::SerializePageHeader(bad);Check(encoded.ok(),"reseal adversarial inventory page header");
    const auto target=mode>=5?db::kTransactionInventoryPageNumber:continuation;
    Check(f.device.WriteAt(target*page_size,encoded.serialized.data(),encoded.serialized.size()).ok(),"write own adversarial inventory header");
    const auto writes=write_calls;
    loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
    Check(!loaded.ok()&&loaded.inventory.entries.empty()&&!loaded.inventory.publication_base,
      "foreign/duplicate/zero-generation inventory page accepted or returned prefix");
    Check(write_calls==writes,"inventory refusal wrote node state");
    // A valid owning-node recovery carrier may still supply the exact selected
    // inventory; damaged page bytes must not override it or invent a new state.
    std::filesystem::rename(retained,journal);
    loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
    Check(loaded.ok()&&loaded.inventory.entries.size()==state.inventory.entries.size()
      &&loaded.inventory.publication_base==state.inventory.publication_base,"bound recovery authority lost after page rejection");
    const auto wrong_size=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size/2);
    Check(!wrong_size.ok()&&wrong_size.inventory.entries.empty()&&!wrong_size.inventory.publication_base,
      "journal recovery turned a wrong caller page size into success");
    if(mode==6) {
      auto substituted=data_header.header;substituted.filespace_uuid=bad.filespace_uuid;
      const auto substituted_bytes=disk::SerializePageHeader(substituted);Check(substituted_bytes.ok(),"reseal transplanted data filespace");
      Check(f.device.WriteAt(data_page*page_size,substituted_bytes.serialized.data(),substituted_bytes.serialized.size()).ok(),"write matching foreign data/root filespace fixture");
      const auto read=read_own();
      Check(!read.ok()&&read.visible_rows.empty(),"recovered inventory authorized foreign physical row context");
      Check(f.device.WriteAt(data_page*page_size,data_header_bytes.data(),data_header_bytes.size()).ok(),"restore own data page header");
    }
    std::filesystem::rename(journal,retained);
    const auto& original=mode>=5?root_bytes:overflow_bytes.serialized;
    Check(f.device.WriteAt(target*page_size,original.data(),original.size()).ok(),"restore own inventory header");
  }
  for(unsigned mode=0;mode<2;++mode) {
    auto body=bodies[1];body.next_page_number=mode?f.device.Size().size_bytes/page_size:db::kTransactionInventoryPageNumber;
    const auto bad=page::BuildTransactionInventoryPageBody(body,page_size);Check(bad.ok(),"reseal invalid inventory chain route");
    Check(f.device.WriteAt(continuation*page_size+128,bad.serialized.data(),bad.serialized.size()).ok(),"write own cycle/out-of-range fixture");
    const auto writes=write_calls;loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
    Check(!loaded.ok()&&loaded.inventory.entries.empty()&&!loaded.inventory.publication_base&&write_calls==writes,
      "cycle/out-of-range chain returned prefix or wrote state");
  }
  const auto restore=page::BuildTransactionInventoryPageBody(bodies[1],page_size);Check(restore.ok(),"restore inventory body encoding");
  Check(f.device.WriteAt(continuation*page_size+128,restore.serialized.data(),restore.serialized.size()).ok(),"restore own inventory chain");
  disk::SerializedPageHeader startup_bytes{};
  Check(f.device.ReadAt(db::kSystemStatePageNumber*page_size,startup_bytes.data(),startup_bytes.size()).ok(),"actual startup header");
  auto startup=disk::ParsePageHeader(startup_bytes);Check(startup.ok(),"decode actual startup header");
  startup.header.database_uuid=Id(UuidKind::database).value;
  const auto wrong_startup=disk::SerializePageHeader(startup.header);Check(wrong_startup.ok(),"reseal foreign startup header");
  Check(f.device.WriteAt(db::kSystemStatePageNumber*page_size,wrong_startup.serialized.data(),wrong_startup.serialized.size()).ok(),"write own startup header fixture");
  loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
  Check(!loaded.ok()&&loaded.inventory.entries.empty(),"foreign startup header admitted inventory chain");
  std::filesystem::rename(retained,journal);
  Check(db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size).ok(),"same-node recovery survives damaged startup page header");
  Check(!read_own().ok(),"recovered state bypassed physical startup/header binding");
  std::filesystem::rename(journal,retained);
  Check(f.device.WriteAt(db::kSystemStatePageNumber*page_size,startup_bytes.data(),startup_bytes.size()).ok(),"restore own startup header");
  // With no carrier, generation exhaustion is owned by the validated chain.
  for (auto& body : bodies) {
    body.inventory_generation = ~u64{0};
    const auto encoded = page::BuildTransactionInventoryPageBody(body, page_size);
    Check(encoded.ok() && f.device.WriteAt(body.page_number*page_size+128,
        encoded.serialized.data(), encoded.serialized.size()).ok(), "install exhausted native chain");
  }
  loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&f.device, page_size);
  Check(loaded.ok() && loaded.inventory.publication_base->generation == ~u64{0}, "load exhausted page-only authority");
  const auto exhaustion_writes = write_calls; const auto exhaustion_bytes = f.Bytes();
  const auto exhausted = db::PersistLocalTransactionInventoryToOpenDevice(&f.device, page_size, loaded.inventory);
  Check(!exhausted.ok() && exhausted.diagnostic.diagnostic_code == "SB-TXN-INVENTORY-PAGE-GENERATION-INVALID" &&
      write_calls == exhaustion_writes && f.Bytes() == exhaustion_bytes && !std::filesystem::exists(journal),
      "page-only generation exhaustion changed durable authority");
  f.Locked();Check(f.device.Close().ok()&&f.device.Open(f.path,disk::FileOpenMode::open_existing_read_only).ok(),"readonly inventory chain reopen");
  loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
  Check(loaded.ok()&&loaded.inventory.entries.size()==state.inventory.entries.size(),"reopened real page-bound inventory");
  Payload(f.Read(data_page,owner),"owning-filespace-row");
}
void NativeInventoryLongChain() {
  namespace page=scratchbird::storage::page;
  Fixture f;
  constexpr u64 count=4097;
  disk::SerializedPageHeader root_bytes{};
  Check(f.device.ReadAt(db::kTransactionInventoryPageNumber*page_size,root_bytes.data(),root_bytes.size()).ok(),"long chain root header");
  const auto root=disk::ParsePageHeader(root_bytes);Check(root.ok(),"long chain root identity");
  const auto page_at=[&](u64 i) {return i?f.first_page+i-1:db::kTransactionInventoryPageNumber;};
  std::vector<platform::Uuid> transaction_ids;transaction_ids.reserve(count);
  for(u64 i=0;i<count;++i) {
    page::TransactionInventoryPageBody body;body.page_number=page_at(i);
    body.previous_page_number=i?page_at(i-1):0;body.next_page_number=i+1<count?page_at(i+1):0;
    body.inventory_generation=1;body.inventory.next_local_transaction_id=count+1;
    mga::TransactionInventoryEntry entry;entry.identity.local_id=mga::MakeLocalTransactionId(i+1);
    entry.identity.transaction_uuid=Id(UuidKind::transaction);transaction_ids.push_back(entry.identity.transaction_uuid.value);
    entry.identity.scope=mga::TransactionScope::local_node;entry.state=mga::TransactionState::active;
    entry.begin_unix_epoch_millis=1790000000000ull+i;body.inventory.entries.push_back(entry);
    const auto encoded=page::BuildTransactionInventoryPageBody(body,page_size);Check(encoded.ok(),"independent one-entry-per-page long-chain fixture");
    auto header=root.header;header.page_number=body.page_number;
    if(i) header.page_uuid=Id(UuidKind::page).value;
    const auto serialized=disk::SerializePageHeader(header);Check(serialized.ok(),"long-chain exact page header");
    Check(f.device.WriteAt(body.page_number*page_size,serialized.serialized.data(),serialized.serialized.size()).ok()
      &&f.device.WriteAt(body.page_number*page_size+128,encoded.serialized.data(),encoded.serialized.size()).ok(),"persist actual long inventory chain");
  }
  Check(f.device.Sync().ok(),"sync long inventory chain");
  const auto writes=write_calls;
  const auto loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&f.device,page_size);
  Check(loaded.ok()&&loaded.inventory.entries.size()==count&&loaded.inventory.next_local_transaction_id==count+1,
    "valid allocated inventory chain rejected by invented 4096-page limit");
  for(u64 i=0;i<count;++i) Check(loaded.inventory.entries[i].identity.transaction_uuid.value==transaction_ids[i],"long-chain native transaction identity changed");
  Check(write_calls==writes,"long inventory read performed writes");f.Locked();
}
}  // namespace
int main(int argc, char** argv) {
  if(argc==2&&std::string_view(argv[1])=="--inventory-long") {
    try {NativeInventoryLongChain();std::cout<<"long_inventory checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
  }
  if (argc == 3 && std::string_view(argv[1]) == "--batch-crash") return CrashNativeBatch(argv[2]);
  if (argc == 3 && std::string_view(argv[1]) == "--catalog-probe") return VerifyCatalogVersion(argv[2]);
  if (argc == 3 && std::string_view(argv[1]) == "--start-snapshot-probe") return VerifyStartSnapshot(argv[2]);
  if (argc == 3 && std::string_view(argv[1]) == "--finality-probe") return VerifyFinalityOracle(argv[2]);
  if (argc == 3 && std::string_view(argv[1]) == "--probe") {
    disk::FileDevice device; const auto opened = device.Open(argv[2], disk::FileOpenMode::open_existing);
    return !opened.ok() && OwnershipError(opened.diagnostic) ? 0 : 1;
  }
  try { NativeInventoryPageBindings(); NativeInventoryLongChain(); Run(); OwnedMutationFailureFinality(); FinalizationIdentityAndOwnership(); ReaderIdentityBeforeMaterialization(); PublishedSnapshotNativeVisibility(); TransactionStartCommitOrder(); ArchiveAndRecoveryCommitOrder(); NativeInventoryPublicationConcurrency(); NativeCatalogMetadataVersions(); FailedNativeBatchCannotCommitPrefix(); std::cout << "owned_device checks=" << checks << " failures=0\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
