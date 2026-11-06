// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// The fixed-root read source (R1 of the drift removal): a real native create,
// then the lease read straight from page zero's roots.
#include "native_fixed_root_memory_lease.hpp"
#include "native_creation_workspace.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
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
namespace mga = scratchbird::transaction::mga;
using namespace scratchbird::core::platform;
using E = db::NativeFixedRootMemoryError;
unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid Id(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}

struct MemoryFixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding{Id(1), Id(61), Id(62), Id(63)};
  db::NativeStorageMemory memory;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = 64u << 20;
    p.per_context_limit_bytes = 64u << 20;
    return p;
  }
  explicit MemoryFixture(u64 bytes) : manager(Policy()) {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = m::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "storage.fixed_root.conformance";
    r.purpose = "fixed-root read";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Id(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "fixed-root conformance";
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

struct Node {
  std::filesystem::path directory, path;
  d::FileDevice device;
  db::NativeFilespaceInitializationRequest request;
  u64 size = 0;
  Node(unsigned profile, db::NativeCreationInventoryFormat format = db::NativeCreationInventoryFormat::in_place) {
    char name[] = "/tmp/sb-fixed-root-XXXXXX";
    if (!mkdtemp(name)) throw std::runtime_error("mkdtemp");
    directory = name;
    path = directory / "node";
    const auto& p = d::kCanonicalFilespacePageProfiles[profile];
    size = p.page_size_bytes;
    request.bootstrap = {Id(1), Id(2), p.uuid, d::kNativeBootstrapIntegrityProfile, {}, p.page_size_bytes, 1, 0, 1, 7};
    request.operation_uuid = Id(3);
    request.writer_uuid = Id(4);
    request.policy_snapshot_uuid = Id(10);
    request.creator.transaction_uuid = {UuidKind::transaction, Id(5)};
    request.creator.local_id = mga::MakeLocalTransactionId(1);
    request.creator.scope = mga::TransactionScope::local_node;
    request.creation_utc_millis = 1789357072000ULL;
    request.total_pages = 64;
    request.inventory_format = format;
    Check(device.Open(path.string(), d::FileOpenMode::create_new).ok(), "node device");
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1), Id(10)}, {{}, 0, 1000});
    Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device, request, 256 * size, issuer).ok(), "real native create");
  }
  ~Node() {
    (void)device.Close();
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }
  std::vector<d::NativeFilespaceDevice> Devices() { return {{Id(2), request.bootstrap.page_size_profile_uuid, &device}}; }
};

db::NativeFixedRootMemoryResult Acquire(Node& n, MemoryFixture& f, db::NativeFixedRootMemoryLimits limits,
                                       Uuid database = Id(1), Uuid primary = Id(2)) {
  const auto devices = n.Devices();
  return db::AcquireNativeFixedRootMemoryLease(database, devices, primary, limits, f.memory, f.binding);
}

void TestProfile(unsigned profile) {
  Node n(profile);
  MemoryFixture f(32u << 20);
  const db::NativeFixedRootMemoryLimits limits{256 * n.size, 8u << 20};
  auto r = Acquire(n, f, limits);
  Check(r.ok(), "fixed-root lease on a freshly created database");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(n.device);
  Check(zero.ok(), "page zero");
  const auto& z = *zero.record;
  const auto& lease = *r.lease;
  const auto& id = lease.identity();
  Check(id.database_uuid == Id(1) && id.filespace_uuid == Id(2) && id.page_zero_uuid == z.page_uuid &&
            id.page_generation == z.page_generation && id.root_set_generation == z.root_set_generation &&
            id.total_pages == z.total_pages && id.creation_utc_millis == z.creation_utc_millis &&
            id.creation_operation_uuid == z.creation_operation_uuid && !id.cluster_authority_required(),
        "the identity is page zero's own");
  const auto same = [](const d::FilespaceRootReference& a, const d::FilespaceRootReference& b) {
    return a.kind == b.kind && a.page_type == b.page_type && a.filespace_uuid == b.filespace_uuid &&
           a.page_number == b.page_number && a.page_generation == b.page_generation &&
           a.page_size_profile_uuid == b.page_size_profile_uuid && a.object_uuid == b.object_uuid;
  };
  Check(lease.roots().size() == z.roots.size() &&
            std::equal(lease.roots().begin(), lease.roots().end(), z.roots.begin(), same),
        "the roots are page zero's, as stored");
  for (const u16 kind : {u16{1}, u16{2}, u16{3}, u16{4}, u16{5}, u16{6}, u16{7}, u16{8}, u16{10}})
    Check(lease.Root(kind) && lease.Root(kind)->kind == kind, "every fixed root is present");
  const auto& inv = lease.inventory();
  Check(inv.next_local_transaction_id == 2 && inv.entries.size() == 1 && inv.entries[0].identity.local_id.value == 1 &&
            inv.entries[0].identity.transaction_uuid.value == Id(5) && mga::HasCommittedInventoryOutcome(inv.entries[0]),
        "the in-place inventory: transaction 1 committed, Next 2");
  Check(lease.inventory_pages().size() == 2 && lease.inventory_pages()[0].header.page_number == lease.Root(4)->page_number &&
            lease.inventory_pages()[0].object_uuid == lease.Root(4)->object_uuid &&
            lease.inventory_pages()[1].header.page_type == 0x301,
        "state page then header page, both of the inventory object");
  const u64 maps = profile ? 1 : 2;  // 64 pages: 60 per 8 KiB map
  Check(lease.allocation().pages.size() == maps && lease.directory().pages.size() == 1, "allocation and directory chains");
  Check(lease.retained_image_bytes() == (1 + 2 + maps + 1) * n.size && lease.backing_bytes_used() > 0,
        "page zero, two inventory pages, the maps and the directory are retained");
  // Two leases on one database see the same identity.
  MemoryFixture g(32u << 20);
  auto again = Acquire(n, g, limits);
  Check(again.ok() && again.lease->identity() == id, "the identity is stable across reads");
  // The exact backing boundary.
  const auto used = lease.backing_bytes_used();
  r.lease.Reset();
  again.lease.Reset();
  MemoryFixture exact(32u << 20);
  Check(Acquire(n, exact, {256 * n.size, used}).ok(), "exactly the backing used");
  MemoryFixture short_one(32u << 20);
  const auto refused = Acquire(n, short_one, {256 * n.size, used - 1});
  Check(!refused.ok() && refused.error == E::resource_exhausted && !refused.lease, "one byte less is refused");
}

void TestRefusals() {
  {
    Node legacy(0, db::NativeCreationInventoryFormat::transitional_legacy);
    MemoryFixture f(32u << 20);
    const auto r = Acquire(legacy, f, {256 * legacy.size, 8u << 20});
    Check(!r.ok() && r.error == E::inventory_failure && r.inventory_error == scratchbird::storage::page::NativeInventoryError::invalid_family,
          "an SBTINV01 inventory has no fixed-root source");
  }
  Node n(0);
  {
    MemoryFixture f(32u << 20);
    Check(Acquire(n, f, {256 * n.size, 8u << 20}, Id(1), Id(9)).error == E::invalid_device, "the primary must be supplied");
  }
  {
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20}, Id(7));
    Check(!r.ok() && r.error == E::memory_binding_failure, "memory bound to another database");
  }
  {
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {n.size, 8u << 20});
    Check(!r.ok() && r.error == E::resource_exhausted, "a one-page image budget is refused");
  }
  {
    MemoryFixture f(32u << 20);
    Check(Acquire(n, f, {0, 8u << 20}).error == E::invalid_request && Acquire(n, f, {256 * n.size, 0}).error == E::invalid_request,
          "zero limits are refused");
  }
  // Every read fails in turn; every corrupted read in turn.
  MemoryFixture base(32u << 20);
  track = true;
  reads = 0;
  auto ok = Acquire(n, base, {256 * n.size, 8u << 20});
  track = false;
  const unsigned total = reads;
  Check(ok.ok() && total > 4, "fault sweep baseline");
  ok.lease.Reset();
  for (unsigned at = 1; at <= total; ++at) {
    MemoryFixture f(32u << 20);
    track = true;
    reads = 0;
    fail_at = at;
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    track = false;
    fail_at = 0;
    Check(!r.ok() && !r.lease, "every failed read refuses without a lease");
  }
  for (unsigned at = 1; at <= total; ++at) {
    MemoryFixture f(32u << 20);
    track = true;
    reads = 0;
    corrupt_at = at;
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    track = false;
    corrupt_at = 0;
    if (r.ok()) std::cerr << "corrupted read " << at << " of " << total << " was accepted\n";
    Check(!r.ok() && !r.lease, "every corrupted read refuses without a lease");
  }
  // A root whose object is not the page's allocation owner.
  {
    auto zero = d::ReadFilespacePageZeroFromOpenDevice(n.device);
    Check(zero.ok(), "page zero for the misbound root");
    auto bad = *zero.record;
    const auto at = std::find_if(bad.roots.begin(), bad.roots.end(), [](const auto& r) { return r.kind == 6; });
    Check(at != bad.roots.end(), "configuration root");
    at->object_uuid = Id(900);
    const auto encoded = d::EncodeFilespacePageZero(bad);
    Check(encoded.ok(), "misbound page zero encodes");
    std::vector<byte> original(n.size);
    Check(n.device.ReadAt(0, original.data(), original.size()).ok(), "keep page zero");
    Check(n.device.WriteAt(0, encoded.bytes->data(), encoded.bytes->size()).ok() && n.device.Sync().ok(), "write misbound root");
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    Check(!r.ok() && r.error == E::root_mismatch && r.root_kind == u16{6}, "a misbound root is refused, naming its kind");
    Check(n.device.WriteAt(0, original.data(), original.size()).ok() && n.device.Sync().ok(), "restore page zero");
    MemoryFixture g(32u << 20);
    Check(Acquire(n, g, {256 * n.size, 8u << 20}).ok(), "restored page zero reads again");
  }
}
}  // namespace

int main() {
  try {
    for (unsigned profile = 0; profile < 5; ++profile) TestProfile(profile);
    TestRefusals();
  } catch (const std::exception& e) {
    std::cerr << "native fixed root FAIL " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "native fixed root: " << checks << " checks passed\n";
  return 0;
}
