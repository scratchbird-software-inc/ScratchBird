// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// The hosted custody operation reads through page zero's fixed roots (C3a of
// the drift removal). A database written by the production create is adopted
// through the real route-ownership transfer. Under one operation, the fixed-
// root read must describe the roots, transaction inventory, allocation and
// directory the file holds (until R3b3, the same as a selected-checkpoint
// read; that reader was deleted with the checkpoint layer, W4d). The fixed-
// root read holds the operation's single read reservation; its observation
// retires the device guards so the same operation can do
// bounded I/O. Every read the fixed-root read makes is then failed in turn and
// corrupted in turn: each must be refused, and the reservation released.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_lease_read_source.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "../../src/server/native_hosted_database_custody.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
thread_local bool track = false;
thread_local unsigned reads = 0, fail_at = 0, corrupt_at = 0;
}  // namespace
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* b, size_t n, off_t offset) {
  if (!track) return __real_pread(fd, b, n, offset);
  ++reads;
  if (fail_at && reads == fail_at) {
    errno = EIO;
    return -1;
  }
  const auto r = __real_pread(fd, b, n, offset);
  if (corrupt_at && reads == corrupt_at && r > 0) static_cast<unsigned char*>(b)[r - 1] ^= 1;
  return r;
}

namespace {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
namespace server = scratchbird::server;
namespace mga = scratchbird::transaction::mga;
using namespace scratchbird::core::platform;
using Host = server::NativeHostedDatabaseCustody;
using WE = db::NativeWriterError;
unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

struct Grant {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding;
  db::NativeStorageMemory memory;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = 1ull << 30;
    p.per_context_limit_bytes = 1ull << 30;
    return p;
  }
  Grant(const Uuid& database, u64 bytes) : manager(Policy()), binding{database, Fixed(61), Fixed(62), Fixed(63)} {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = m::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "storage.fixed_root.custody";
    r.purpose = "fixed-root custody conformance";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "fixed-root custody conformance";
    for (const auto& scope : r.scope_chain) {
      m::HierarchicalMemoryBudget b;
      b.scope = scope;
      b.hard_limit_bytes = bytes;
      b.provenance = r.provenance;
      Check(ledger.SetBudget(b).ok(), "parent budget");
    }
    auto grant = m::AcquireReservationBackedMemoryResource(r);
    Check(grant.ok(), "node-issued grant");
    auto adopted = db::AdoptNativeStorageMemory(binding, grant.resource);
    Check(adopted.ok(), "grant adoption");
    memory = std::move(adopted.memory);
  }
};

// A production-created database, closed so the route owner can open it.
struct Database {
  scratchbird::tests::OwnedTempDirectory artifacts;
  std::string path;
  db::NativeDatabaseCreateReceipt receipt;
  u64 size = 0;
  Uuid profile;
  explicit Database(unsigned profile_index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[profile_index];
    size = p.page_size_bytes;
    profile = p.uuid;
    db::NativeDatabaseCreateRequest r;
    r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    r.database_name = "orders";
    r.database_default_language = "en";
    r.page_size_profile_uuid = p.uuid;
    r.page_size_bytes = p.page_size_bytes;
    r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
    r.creation_utc_millis = 1789357072000ULL;
    r.engine_uuid = Fixed(4);
    r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
    r.bootstrap_principal_name = "BootstrapAdmin";
    r.bootstrap_credential_fingerprint = kFingerprint;
    r.service_principal_name = "NodeService";
    r.service_credential_fingerprint = kFingerprint;
    r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
    path = (artifacts.path() / "node").string();
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::create_new).ok(), "node device");
    u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
    auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
    Check(made.ok(), "the production create writes a complete native database");
    receipt = *made.receipt;
    Check(device.Close().ok(), "release the creating handle before route ownership");
  }
};

// The database adopted by hosted custody, with one operation reserved.
struct Hosted {
  Database& n;
  Grant persistent, transient, reading;
  server::DatabaseOwnershipResult route;
  std::shared_ptr<Host> custody;
  Host::Operation operation;
  explicit Hosted(Database& database)
      : n(database),
        persistent(database.receipt.database_uuid, 1u << 20),
        transient(database.receipt.database_uuid, 128u << 20),
        reading(database.receipt.database_uuid, 512u << 20) {
    server::DatabaseOwnershipRequest ownership;
    ownership.database_path = n.path;
    ownership.storage_access = d::RouteStorageAccess::read_write;
    route = server::AcquireDatabaseOwnership(ownership);
    Check(route.acquired && route.lock, "the original route owner");
    auto begun = Host::Begin(route.lock);
    Check(begun.ok(), "custody retains the original route");
    custody = std::move(begun.custody);
    std::vector<std::unique_ptr<d::FileDevice>> secondary;
    const auto adopted = Host::AdoptWriter(custody, n.receipt.database_uuid, n.receipt.filespace_uuid, secondary,
                                           {64 * 1024, {256 * n.size, 64u << 20}}, persistent.memory,
                                           persistent.binding, transient.memory, transient.binding);
    Check(adopted.ok(), "custody adopts the writer over the production database");
    auto begun_operation = Host::TryBeginOperation(custody, Fixed(234));
    Check(begun_operation.ok(), "one operation reserved");
    operation = std::move(begun_operation.operation);
  }
  ~Hosted() {
    operation.Reset();
    custody.reset();
    route.lock.reset();
  }
  db::NativeFixedRootMemoryLimits Limits() const { return {256 * n.size, 64u << 20}; }
  Host::FixedRootReadResult ReadFixed() { return operation.ReadFixedRootWithMemory(Limits(), reading.memory, reading.binding); }
};

void Differential(unsigned profile) {
  Database n(profile);
  Hosted h(n);
  // The fixed-root read holds the operation's one read reservation.
  auto fixed = h.ReadFixed();
  Check(fixed.ok() && fixed.lease.operation_uuid() == Fixed(234), "fixed-root read under the reserved operation");
  // (Until W4d this also refused a second, selected-checkpoint reader; that
  // reader kind no longer exists.)
  Check(h.ReadFixed().writer_error == WE::read_handle_live, "a live fixed-root reader excludes a second reader");
  std::vector<byte> page(n.size);
  Check(h.operation.ReadAtBounded(n.receipt.filespace_uuid, n.profile, 0, page).error == WE::read_handle_live,
        "a live fixed-root reader excludes bounded I/O on its operation");
  const auto& lease = fixed.lease.source();
  const auto id = lease.identity();
  const auto token = db::NativeReadSourceIdentityFromFixedRoot(id);
  Check(token.kind == db::NativeReadSourceKind::fixed_root && token.database_uuid == n.receipt.database_uuid &&
            token.filespace_uuid == n.receipt.filespace_uuid && token.page_size_bytes == n.size &&
            token.root_set_uuid == id.page_zero_uuid && token.source_generation == 1 && token.root_set_generation == 1,
        "the fixed-root identity is page zero's own, as created");
  const auto fs = db::detail::NativeLeaseSourceAccess::From(lease);
  const auto inventory = *fs.inventory;
  const auto entries = std::vector(inventory.entries.begin(), inventory.entries.end());
  const auto allocation_pages = fs.allocation->pages.size();
  const auto directory_records = fs.directory->pages.front().directory.total_records;
  std::array<std::optional<db::detail::NativeLeaseRoot>, 11> roots;
  for (const u16 kind : {u16{1}, u16{2}, u16{3}, u16{4}, u16{5}, u16{6}, u16{7}, u16{8}, u16{10}})
    if (const auto* r = fs.Root(kind)) roots[kind] = *r;
  fixed.lease.Reset();
  // Drift removal R3b3: the production create writes no checkpoint, so the
  // fixed-root read is checked against the file read independently, through
  // this operation's own bounded I/O, and against the create receipt. (Until
  // R3b3 it was compared with a selected-checkpoint read under the same
  // operation; every root, the inventory, the allocation chain and the
  // directory agreed.)
  {
    auto next = h.ReadFixed();
    Check(next.ok(), "the reservation returns when the fixed-root reader retires");
  }
  std::vector<byte> zero_bytes(n.size);
  Check(h.operation.ReadAtBounded(n.receipt.filespace_uuid, n.profile, 0, zero_bytes).ok(),
        "page zero read straight from the file");
  const auto zero = d::DecodeFilespacePageZero(zero_bytes.data(), zero_bytes.size());
  Check(zero.ok() && zero.record->page_uuid == id.page_zero_uuid &&
            zero.record->bootstrap.database_uuid == n.receipt.database_uuid &&
            zero.record->bootstrap.filespace_uuid == n.receipt.filespace_uuid,
        "the decoded page zero is the one the fixed-root read names");
  Check(std::none_of(zero.record->roots.begin(), zero.record->roots.end(),
                     [](const auto& r) { return r.kind == 9 || (r.kind >= 18 && r.kind <= 21); }),
        "page zero names no checkpoint, selector or watermark root");
  for (const u16 kind : {u16{1}, u16{2}, u16{3}, u16{4}, u16{5}, u16{6}, u16{7}, u16{8}, u16{10}}) {
    Check(roots[kind].has_value(), "every fixed root is present");
    const auto stored = std::find_if(zero.record->roots.begin(), zero.record->roots.end(),
                                     [&](const auto& r) { return r.kind == kind; });
    Check(stored != zero.record->roots.end() && stored->page_type == roots[kind]->page_type &&
              roots[kind]->page == d::NativePageReference{stored->filespace_uuid, stored->page_number,
                                                          stored->page_generation, stored->page_size_profile_uuid} &&
              stored->object_uuid == roots[kind]->object_uuid,
          "the fixed-root read names exactly the page page zero stores for every root");
  }
  Check(inventory.next_local_transaction_id == 2 && entries.size() == 1 &&
            entries.front().identity.local_id.value == 1 &&
            entries.front().identity.transaction_uuid.value ==
                n.receipt.construction_transaction.transaction_uuid.value &&
            mga::HasCommittedInventoryOutcome(entries.front()),
        "the inventory holds exactly the committed construction transaction");
  Check(allocation_pages == n.receipt.workspace.map_pages && directory_records == 1,
        "the allocation chain is the created maps; the directory holds the one filespace");
  // The observation keeps the images but retires the guards and the
  // reservation: the same operation can now do bounded I/O.
  auto observed = h.operation.ObserveFixedRootWithMemory(h.Limits(), h.reading.memory, h.reading.binding);
  Check(observed.ok() && observed.observation.operation_uuid() == Fixed(234) && observed.observation.identity() == id &&
            observed.observation.Root(4) && observed.observation.Root(4)->page_number == roots[4]->page.page_number &&
            observed.observation.inventory().entries.size() == entries.size(),
        "the observation keeps the fixed-root images");
  const auto io = h.operation.ReadAtBounded(n.receipt.filespace_uuid, n.profile, 0, page);
  Check(io.ok() && io.io->bytes_transferred == n.size, "bounded I/O while the observation is retained");
  auto again = h.ReadFixed();
  Check(again.ok(), "an observation does not hold the read reservation");
  again.lease.Reset();
  observed.observation.Reset();
}

// Every read a fixed-root read makes, failed in turn and corrupted in turn.
void Sweeps(unsigned profile) {
  Database n(profile);
  Hosted h(n);
  reads = 0;
  track = true;
  auto clean = h.ReadFixed();
  track = false;
  const auto total = reads;
  Check(clean.ok() && total >= 5, "a clean fixed-root read through custody");
  std::cout << "profile " << profile << ": a fixed-root read makes " << total << " reads\n";
  clean.lease.Reset();
  for (const bool corrupt : {false, true}) {
    for (unsigned k = 1; k <= total; ++k) {
      reads = 0;
      (corrupt ? corrupt_at : fail_at) = k;
      track = true;
      auto r = h.ReadFixed();
      track = false;
      fail_at = corrupt_at = 0;
      if (r.ok())
        std::cerr << (corrupt ? "corrupted" : "failed") << " read " << k << " of " << total << " was accepted\n";
      Check(!r.ok() && r.writer_error == WE::source_failure && r.error == db::NativeOwnedSourceError::source_failure &&
                r.fixed_root_error != db::NativeFixedRootMemoryError::none,
            corrupt ? "every corrupted read is refused through custody" : "every failed read is refused through custody");
      auto next = h.ReadFixed();
      Check(next.ok(), "a refused read releases the operation's reservation");
      next.lease.Reset();
    }
  }
  // Too little backing for the lease: refused, reservation released.
  auto small = h.operation.ReadFixedRootWithMemory({256 * n.size, 4096}, h.reading.memory, h.reading.binding);
  Check(!small.ok() && small.writer_error == WE::source_failure && !small.lease, "a lease without backing is refused");
  auto next = h.ReadFixed();
  Check(next.ok(), "the reservation returns after a refused lease");
}
}  // namespace

int main() {
  try {
    for (unsigned profile = 0; profile < 5; ++profile) Differential(profile);
    Sweeps(0);
    Sweeps(4);
    std::cout << "native fixed root custody: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& e) {
    track = false;
    std::cerr << "FAIL " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
